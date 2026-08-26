#pragma once

namespace ferric
{
/**
    What the music does to the transport.

    Everything reactive is decided here, in plain C++ with no GL and no host in
    sight: bins and a tempo go in, a handful of scalars come out, and the render
    multiplies by them. Reaction is the part of an audio-driven effect that is
    hardest to judge by eye -- "it moves when the music moves" is true of almost
    every wrong answer -- so it is kept where a test can state what it should be.

    -------------------------------------------------------- what it drives here

    A transport is a mechanism, so the reaction is mechanical rather than
    decorative. There are three things the music is allowed to do:

    - **Lurch.** A beat puts a sharp, decaying excursion straight into the error
      signal, on top of whatever the transport was already doing. This is the
      belt slipping, and it is by far the most useful of the three because it
      lands *on* the grid rather than merely correlating with it.
    - **Depth.** Level hands the transport's own wow and flutter over to the
      music, so quiet passages run clean and loud ones fall apart.
    - **Mistracking.** Band energy pushes the decoder's level error around, so
      the noise reduction breathes with the mix. Routed to treble by default,
      because hats and cymbals are what a sliding band is looking at.

    ------------------------------------------------------------- opt in, always

    Every depth defaults to zero, and at zero every output is its neutral value:
    scale 1, no lurch, no push. Dropped on a layer with no audio routed and
    nothing touched, this is an ordinary manual tape emulation and behaves like
    one. That is not a fallback for missing audio, it is half of how the plugin
    gets used.

    ---------------------------------------------------------------- FFGL only

    Beat information and the FFT buffer are FFGL features. When the OpenFX build
    lands it will fill a zeroed `Input` and get the manual transport -- the same
    code, the same arithmetic, no second reduced implementation to drift.
*/
namespace drive
{
/// Spectrum bins asked of the host. Nothing on the GPU has a matching array:
/// the spectrum collapses to three band energies and a level here, on the CPU,
/// so no uniform array length has to be kept in step with this number.
constexpr int kAudioBins = 64;

/// How the host's transport is being followed.
enum Sync
{
	/// No grid. The beat envelope is flat zero, so Beat Depth genuinely does
	/// nothing in this mode -- which is why tools/sweep.py sweeps it from a
	/// context that has already left Free.
	kSyncFree = 0,

	/// Locked to the host's transport. Which division it fires on is the
	/// Division control's job, not this one's.
	kSyncLocked,

	kSyncCount
};

/// Which band pushes what.
enum Route
{
	/// Bass lurches the transport, treble pushes the decoder off level. The two
	/// halves of the plugin get a band each, which is the only route where both
	/// are audibly doing something at once.
	kRouteNatural = 0,

	/// The same, swapped: treble lurches, bass mistracks. Reads as wrong on
	/// purpose -- the mechanism responds to the wrong thing.
	kRouteInverted,

	/// Bass drives both. Nothing responds to the top of the mix at all, which
	/// is the route that stays legible on a busy one.
	kRouteBass,

	/// Treble drives both. Nearly silent on a bass-heavy track, and that is the
	/// point of having it.
	kRouteTreble,

	kRouteCount
};

/// The reactive controls, in physical units.
struct Settings
{
	int sync  = kSyncFree;
	int route = kRouteNatural;

	/// 0..1 each. Beat and level depth are a SUM handed over to the music; see
	/// `Output::scale`.
	float beatDepth  = 0.0f;
	float levelDepth = 0.0f;
	float bandDepth  = 0.0f;

	/// Exponent on the beat envelope's decay, 1..16. 1 is a linear ramp down
	/// over the division, 16 is a click.
	float beatDecay = 4.0f;

	/// Musical division the beat envelope fires on, in beats. 1 is every beat,
	/// 4 is every bar.
	float beatDivision = 1.0f;
};

/// Everything the drive reads from the host this frame.
struct Input
{
	/// Smoothed spectrum, low frequencies first. Null or zero-length is a host
	/// with no audio routed, which is not an error.
	const float* bins = nullptr;
	int binCount      = 0;

	float bpm      = 120.0f;
	float barPhase = 0.0f;

	/// The host clock, already normalised to seconds. See Ferric.h for why that
	/// normalisation is not something a plugin can skip.
	double seconds = 0.0;
};

/// What the render multiplies by.
struct Output
{
	/// Multiplies the transport's depth controls. 1.0 when nothing is reacting.
	float scale = 1.0f;

	/// Added straight into the error signal, in the same dimensionless units.
	/// Zero when nothing is reacting.
	float lurch = 0.0f;

	/// Added to the decoder's level error, in dB. Signed: the route decides
	/// which way, and both directions are a real artifact.
	float mistrackDb = 0.0f;

	/// The three band energies and the overall level, 0..1. Outputs rather than
	/// internals because the harness asserts on them directly and because the
	/// trace overlay draws them -- which makes "is it even hearing anything?"
	/// answerable without leaving Resolume.
	float bass  = 0.0f;
	float mid   = 0.0f;
	float high  = 0.0f;
	float level = 0.0f;

	/// The beat envelope, 0..1. Zero in Free mode.
	float beat = 0.0f;
};

/**
    Split the spectrum into three bands.

    ------------------------------------------------------------------ the trap

    Resolume does not document what frequency range its FFT buffer spans, and
    the obvious split -- a third of the bins each -- is wrong for any buffer
    linear in frequency. With 64 linear bins over a normal sample rate one bin
    is several hundred hertz, so a "bass third" reaches past 7 kHz: everything
    anybody would call music lands in it and the other two bands sit near zero
    all night. The effect looks like it is only hearing the kick, and the
    natural conclusion is that the FFT is broken rather than the arithmetic
    dividing it up.

    So the split is logarithmic in bin index -- roughly two octaves per band --
    which is right for a linear-in-frequency buffer and merely differently
    weighted for an already-log-spaced one. Stated as an assumption because that
    is what it is: the host's mapping is undocumented, Route is how an operator
    compensates if a given host disagrees, and none of this is a claim about
    Resolume's internals.

    Each band is a mean over its bins, not a peak, so one loud bin cannot carry
    a band on its own.
*/
void bands( const float* bins, int binCount, float* outBass, float* outMid, float* outHigh );

/// Fold the frame's audio and transport into the scalars the render wants.
Output compute( const Settings& settings, const Input& in );

int syncCount();
const char* syncLabel( int index );
int routeCount();
const char* routeLabel( int index );

} // namespace drive
} // namespace ferric
