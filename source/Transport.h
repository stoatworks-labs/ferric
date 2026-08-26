#pragma once

namespace ferric
{
/**
    The transport, and the time-base error it puts on the signal.

    ------------------------------------------------------------- the one idea

    **Wow and flutter is not a wobble applied to a picture. It is a timing error
    applied to a signal, and a picture read off tape is a signal with a clock.**

    Every pixel therefore has a *tape time* -- how far into the recording the
    playback head was when that pixel came off the oxide:

        dt( x, y ) = (line / lines) * secondsPerPicture      // down the frame
                   + u * (secondsPerPicture / lines)         // along the line

    and the horizontal displacement of that pixel is one scalar error signal
    evaluated there, against phases the frame arrived with. Nothing else. Everything the effect looks like
    falls out of that single substitution:

    - **Wow** (0.5-3 Hz) is slower than one picture, so the whole frame leans and
      breathes together.
    - **Flutter** (6-20 Hz) fits a cycle or two into a picture, so it draws a
      travelling wave *down* the image -- and because `tapeTime` advances between
      frames, the wave scrolls rather than standing still. That scroll is the
      thing that reads as tape rather than as a wobble filter, and it is not
      coded anywhere; it is what a moving clock does to a fixed waveform.
    - **Scrape flutter** (100 Hz+) fits many cycles into a picture, so it becomes
      fine horizontal banding.
    - Because `t` also varies *along* the line, the error is not a per-line
      offset but a per-line **stretch**. Lines get longer and shorter. That is
      what real time-base error does and it is free here -- it is the `u` term.

    ------------------------------------------ the scan rate is a CONTROL, and why

    ⚠️ **The physically correct scan rate produces no visible flutter at all**,
    and this is worth stating plainly because it looks like a bug in the model.

    A real 625-line picture scans at 15625 lines per second. One scanline is 64
    microseconds. Flutter at 10 Hz moves through 0.0004 of a cycle in that time,
    and a whole frame spans 0.0016 of a cycle. Put the real numbers in and the
    error is a *constant* across the entire frame: no vertical structure, no
    travelling wave, no tearing. Just a frame that slides sideways very slightly.
    Measured, at Flutter 1.0 and the real scan rate, peak-to-peak variation
    across the picture is under a thousandth of the error's amplitude.

    That is not a failure of the model, it is a true fact about videotape: audio
    wow and flutter and video time-base error are not the same phenomenon and do
    not live in the same band. A VCR's picture tears because of head-drum and
    servo error at field rate, not because of anything an audio engineer would
    call flutter.

    So this plugin does not pretend otherwise. It does what it was asked to do --
    **run a picture through a cassette transport** -- and a cassette transport
    does not scan at 15625 lines per second. `secondsPerPicture` is therefore an
    exposed control (`Tape Speed`), spanning about 2 ms per picture, where the
    frame is rigid and only wow survives, to about 2 seconds per picture, where a
    single flutter cycle spans a few scanlines and the image comes apart into
    ribbons. The fiction is stated rather than hidden, and the control is the
    place an operator chooses how far into it to go.

    -------------------------------------------------------------- the mirror

    `error()` is evaluated per pixel, so the GPU has to have it -- but the
    harness, the weighted-percentage readout and (later) the OFX build need the
    C++. The arithmetic is therefore duplicated in `shaders/Tbe.cpp`, and every
    duplicated block is marked `//= mirrored` in both files. `frtest --tbe`
    renders the GPU's answer into a probe target and compares it against this
    file pixel for pixel; the probe shader is assembled from the *same string*
    the render pass uses, so the test checks the real code and not a lookalike.

    Nothing else in this repo is mirrored. The weighting curve, the machine
    table and every control curve exist once, in C++.

    ---------------------------------------------- why there is no absolute time

    ☠️ **Nothing here is ever handed an absolute tape time as a float, and a
    version that did would look perfect for the first few minutes of a set and
    then quietly die.**

    Scrape flutter indexes its noise at 220 Hz. Twenty minutes into a show that
    is an index of 264000, where a 32-bit float's spacing is about 0.03 -- so
    adjacent pixels, which are a few millionths apart, land on the *same* float.
    The noise stops varying across the picture and freezes into blocks. The sine
    components go the same way slightly later: `sin( 2*pi*220*t )` at t = 3600
    has an argument near five million, and the last bits of that argument are
    the whole phase.

    Nothing about this shows up in a test. Every offline render starts at t = 0,
    every screenshot is taken in the first minute, and the plugin looks correct
    right up until it is somebody's third hour.

    So the frame's **phases** are computed once per frame on the CPU in double
    precision and wrapped -- `phaseAt()` -- and everything mirrored works in
    `dt`, the offset *within one picture*, which is at most two seconds by
    construction. Both sides then only ever evaluate small arguments. This is
    also why `phaseAt()` is deliberately not part of the mirror: it is the one
    piece that needs the precision the GPU does not have.
*/
namespace transport
{
/// Machines. The dropdown order, so append only.
///
/// A machine is not a preset over the user's controls -- it multiplies them.
/// It sets the *character* of the error: which components dominate, and how
/// fast each one runs. The depth controls stay the operator's.
enum Machine
{
	/// Compact cassette, 1 7/8 ips. Wow dominates, flutter is soft and slow,
	/// scrape is present because the tape drags across a felt pressure pad.
	/// The spec sheet number for a decent deck is around 0.08% weighted.
	kMachineCassette = 0,

	/// 15 ips reel to reel. Everything an order better, and the flutter that is
	/// left sits higher because the capstan is turning faster.
	kMachineReel,

	/// Helical-scan video head. The error is dominated by a once-per-revolution
	/// component -- the drum -- so it is far more periodic than either tape
	/// machine and lands near field rate.
	kMachineVideoHead,

	/// A transport with something wrong with it: a slipping belt, a flat spot on
	/// an idler, a capstan with dirt on it. Deep once-per-revolution wow with a
	/// hard periodic lurch on it. This is the one that is not trying to be a
	/// specification.
	kMachineFailing,

	kMachineCount
};

/// The transport controls, in physical units.
struct Settings
{
	int machine = kMachineCassette;

	/// Depths, 0..1. Each scales its component's contribution to the error.
	float wow     = 0.35f;
	float flutter = 0.30f;
	float scrape  = 0.15f;
	float drift   = 0.20f;

	/// Rates in Hz, before the machine's multiplier.
	float wowRate     = 1.2f;
	float flutterRate = 9.0f;

	/// How much tape time one picture occupies, in seconds. See the header
	/// comment: this is the fiction, stated. Larger means more vertical
	/// structure and more tearing.
	float secondsPerPicture = 0.10f;

	/// Scanlines the picture is quantised to. Normally the render height, so
	/// one scanline per output row.
	int lines = 1080;

	/// Overall displacement, as a fraction of picture width, at error = 1.
	float amount = 0.05f;

	/// How much of the *low frequency* error also moves the frame vertically,
	/// 0..1. Vertical hold follows wow and drift only -- flutter and scrape are
	/// far above field rate, so a sync separator does not see them, and feeding
	/// them in produces a jitter that reads as a broken plugin rather than as a
	/// rolling picture.
	float vertical = 0.25f;

	/// Extra error added on top by the audio drive, already scaled. Kept
	/// separate from the depths so a lurch can exceed what the sliders allow
	/// and so `weightedPercent()` can report the transport's own figure.
	float lurch = 0.0f;
};

/// One evaluation of the error signal, split into the parts the render needs
/// separately.
struct Error
{
	/// The whole error, dimensionless. Nominally -1..1 but not clamped: the
	/// components sum, so all four at full depth reach further and that is
	/// allowed. The render multiplies by `Settings::amount`.
	float total = 0.0f;

	/// Wow and drift only -- everything below about 4 Hz. What vertical hold
	/// follows.
	float slow = 0.0f;
};

/// Where every component of the error signal has got to at the start of a
/// frame.
///
/// Computed once per frame, in double precision, and wrapped. See the header
/// for what happens to a plugin that skips this. **Not mirrored** -- this is
/// precisely the part the GPU cannot do.
struct Phase
{
	/// Radians, wrapped into 0..2pi. Two wow components at incommensurate
	/// rates, two flutter components likewise. The once-per-revolution pulse
	/// rides on `wow1` and needs no phase of its own.
	float wow1    = 0.0f;
	float wow2    = 0.0f;
	float flutter1 = 0.0f;
	float flutter2 = 0.0f;

	/// Noise coordinates, wrapped into a range a float still resolves finely.
	float scrape = 0.0f;
	float drift  = 0.0f;
};

/// The phases at an absolute tape time, in seconds.
Phase phaseAt( const Settings& s, double tapeTime );

/// Evaluate the error signal `dt` seconds into the frame.
///
/// `dt` is an offset *within one picture*, so it never exceeds
/// `Settings::secondsPerPicture`. Absolute time never appears.
///
//= mirrored in shaders/Tbe.cpp
Error error( const Settings& s, const Phase& p, float dt );

/// How far into the frame's tape a point in the picture is, in seconds.
///
/// `u` and `v` are 0..1 with v = 0 at the TOP, which is picture space and not
/// GL space -- the flip happens once, in each shader's main().
///
//= mirrored in shaders/Tbe.cpp
float pictureTapeOffset( const Settings& s, float u, float v );

/// The DIN 45507 / IEC 386 weighting curve, as a linear gain at a frequency in
/// Hz.
///
/// **This is a fit to the curve's two defining points, not a transcription of
/// the standard's network.** DIN 45507 is specific about -3 dB at 0.5 Hz and
/// 25 Hz, and for a second-order band-pass those two numbers fix the filter
/// completely: the centre is their geometric mean and Q is the centre over the
/// bandwidth. So there is one section, placed exactly there.
///
/// That puts the maximum at **3.54 Hz where the standard names 4 Hz**, and the
/// discrepancy is not worth removing: the curve is broad enough at Q = 0.144
/// that 4 Hz sits within 0.01 dB of the peak, and re-centring on 4 would move
/// the -3 dB points off the two frequencies the standard is actually specific
/// about. `frtest --weighting` checks exactly the three things claimed here and
/// nothing else -- a transcribed table nobody could check would be worse than
/// a fit whose error is stated.
///
/// Nothing in the render calls this. It exists so `weightedPercent()` can report
/// a number that means what the specification means, instead of a number that
/// merely correlates with the sliders.
float weighting( float hz );

/// Weighted RMS wow and flutter, as a percentage -- the figure a deck's spec
/// sheet quotes.
///
/// Measured, not derived: `error()` is sampled at 2 kHz for four seconds, run
/// through the weighting filter, and the RMS of the settled part is taken. That
/// is what a real wow-and-flutter meter does, and it is the only approach that
/// covers the noise components as well as the sines -- a closed form would have
/// to expand the once-per-revolution pulse's harmonic series and would still
/// have nothing to say about scrape.
///
/// **The unit is a percentage of one line period**, not of nominal tape speed.
/// A deck quotes the latter; there is no tape speed here to be a percentage of,
/// because the scan rate is a control (see the header). One line period is the
/// closest thing this plugin has to a nominal clock, so that is what the figure
/// is against, and the trace overlay says so.
///
/// Expensive enough not to want per frame -- roughly eight thousand evaluations
/// -- so the caller caches it against the settings that produced it.
///
/// `Settings::lurch` is deliberately EXCLUDED. The readout describes the
/// transport, and a transport does not lurch because a kick drum landed.
float weightedPercent( const Settings& s );

/// The machine's multipliers, exposed because the harness asserts on them and
/// because the trace overlay prints them.
struct MachineProfile
{
	const char* label;
	float wowRate;    ///< multiplies Settings::wowRate
	float flutterRate;///< multiplies Settings::flutterRate
	float wowGain;
	float flutterGain;
	float scrapeGain;
	float driftGain;
	/// Weight of the once-per-revolution component, which is what makes the
	/// video head and the failing transport periodic rather than wandering.
	float periodic;
};

const MachineProfile& machineProfile( int machine );
int machineCount();
const char* machineLabel( int machine );

} // namespace transport
} // namespace ferric
