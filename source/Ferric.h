#pragma once

#include <FFGLSDK.h>

#include <array>
#include <string>

#include "Compander.h"
#include "Controls.h"
#include "Drive.h"
#include "PassBuffer.h"
#include "Presets.h"
#include "StoatworksAboutParams.h"
#include "Transport.h"

/**
    Ferric -- wow, flutter and tape noise reduction for Resolume.

    The picture is treated as an analogue signal on a tape: pulled past a head
    by a transport that is not quite steady, laid on an oxide that has its own
    noise floor and its own faults, and put through the compander that consumer
    decks used to hide the one under the other.

    ------------------------------------------------------------ two halves

    They are genuinely separate and each has its own file and its own idea.

    **The transport** (`Transport.h`). Wow and flutter is a timing error, and a
    picture read off tape is a signal with a clock -- so every pixel gets a tape
    time and one scalar error signal is evaluated there. Wow leans the frame,
    flutter draws a travelling wave down it, scrape flutter bands it, and the
    wave scrolls between frames because the clock moved. None of that is coded;
    it is what one substitution does.

    **The noise reduction** (`Compander.h`). It is a round trip through a
    medium, not a filter, and every artifact anybody recognises is the two ends
    disagreeing. So the chain is the real chain -- encode, then the tape, then
    decode -- and the hiss is bent once where the picture is bent twice.

    The two halves meet only in `ProcessOpenGL`'s pass order and in the fact
    that the tape sits between them. Neither knows anything about the other.

    --------------------------------------------------------------- the clock

    ⚠️ **Resolume sends `SetTime` in MILLISECONDS.** The FFGL header never says,
    the SDK's own Particles sample divides by 1000, and this repo's harness sends
    seconds -- so a plugin that consumes `hostTime` raw runs a thousand times
    fast in a real host, and no offline test can catch it. The unit is settled by
    comparing the host's own delta against a steady wall clock over several
    frames, which names the ratio outright rather than guessing from the
    magnitude of one frame.

    That clock feeds tape time directly rather than being integrated. Unlike the
    fleet's drifting-noise plugins there is nothing here whose history a
    rescaled clock would rewrite: the phases are recomputed from absolute time
    every frame, so a scrub is a scrub and lands where it should.

    ------------------------------------------------------ the About block, and why

    An FFGL 2.x plugin has no window. It declares parameters and the host draws
    them, so the name, the version and the links are parameters -- a text one and
    four buttons. `SetTextParameter` **must** be overridden even though the text
    is display-only: the SDK's `instantiateGL` sets every parameter's default on
    a fresh instance and deletes the instance if any set returns FF_FAIL, and the
    base class's implementation is a stub that returns exactly that. Skip it and
    no real host can instantiate the plugin at all, while every harness that
    drives the class directly passes.
*/
class Ferric : public CFFGLPlugin
{
public:
	/// Clock test hook. The offline harness DECLARES its unit rather than
	/// leaving the calibration to infer one -- an absolute time handed over in a
	/// single frame is genuinely ambiguous, and an implicit unit is what lets a
	/// millisecond bug through in the first place.
	void SetClockScaleForTest( double scale );

	Ferric();

	//CFFGLPlugin
	FFResult InitGL( const FFGLViewportStruct* vp ) override;
	FFResult ProcessOpenGL( ProcessOpenGLStruct* pGL ) override;
	FFResult DeInitGL() override;

	FFResult SetFloatParameter( unsigned int index, float value ) override;
	float GetFloatParameter( unsigned int index ) override;

	FFResult SetTime( double time ) override;

	char* GetTextParameter( unsigned int index ) override;

	/// Display-only text still needs this. See the class comment.
	FFResult SetTextParameter( unsigned int index, const char* value ) override;

	/// Everything the operator can reach, in the order Resolume shows it: the
	/// mechanism, the medium, the noise reduction, what the music is allowed to
	/// do to all three, and how much of the result to keep.
	///
	/// Public because the harness drives the plugin by parameter id and needs
	/// PT_COUNT to enumerate them.
	///
	/// Nothing here is appended out of place. The fleet's rule is that a
	/// *released* plugin must never renumber its ParamIDs, because saved
	/// compositions refer to them. This plugin has never shipped, so its ids are
	/// still free to be in the order the inspector should show them. **From
	/// v0.1.0 onward that freedom is gone** and new controls go on the end.
	enum ParamID : FFUInt32
	{
		//Transport
		PT_MACHINE,
		PT_WOW,
		PT_WOW_RATE,
		PT_FLUTTER,
		PT_FLUTTER_RATE,
		PT_SCRAPE,
		PT_DRIFT,
		PT_TAPE_SPEED,
		PT_AMOUNT,
		PT_VERTICAL,

		//Tape
		PT_HISS,
		PT_DROPOUTS,
		PT_HEAD_WEAR,

		//Noise reduction
		PT_NR_TYPE,
		PT_NR_MODE,
		PT_MISTRACKING,
		PT_NR_STRENGTH,

		//Reaction. PT_AUDIO is an FFT buffer (FF_TYPE_BUFFER, FF_USAGE_FFT):
		//Resolume renders it as an audio-source picker -- Local, Composition or
		//External -- and writes one spectrum bin per element, low frequencies
		//first.
		PT_AUDIO,
		PT_SYNC,
		PT_BEAT_DEPTH,
		PT_BEAT_DECAY,
		PT_DIVISION,
		PT_LEVEL_DEPTH,
		PT_BAND_DEPTH,
		PT_ROUTE,

		//Output
		PT_SHOW_TRACE,
		PT_MIX,

		//Preset. Declared after the real controls so their IDs -- which a saved
		//composition refers to -- do not shift under existing users.
		PT_PRESET,

		//About. FFGL has no window, so the name, the version and the links are
		//parameters the host draws. See StoatworksAboutParams.h.
		PT_ABOUT_FIRST,
		PT_COUNT = PT_ABOUT_FIRST + stoatworks::about::kParamCount
	};

	/// params[] as the shared control struct, so this build and the OpenFX one
	/// that follows ask the same question of the same code. Public for the
	/// harness.
	ferric::controls::HostValues hostValues() const;

	/// What the music did on the frame just rendered. Public so the harness can
	/// assert on the reaction without re-deriving it -- and so a failing
	/// assertion is about the plugin's own arithmetic rather than about a second
	/// copy of it in the test.
	///
	/// Zero-valued before the first ProcessOpenGL.
	const ferric::drive::Output& lastDrive() const
	{
		return driveOut;
	}

	/// The resolved render settings from the frame just rendered, for the same
	/// reason.
	const ferric::controls::Render& lastRender() const
	{
		return renderOut;
	}

	/// The weighted wow-and-flutter figure the trace overlay is showing.
	float weightedPercent() const
	{
		return cachedWf;
	}

private:
	/// The ParamID each presets::Param drives, in presets::Param order. The
	/// preset table stays host-agnostic; this is the FFGL binding of it.
	static constexpr unsigned int kPresetParamIDs[ ferric::presets::kParamCount ] = {
		PT_MACHINE, PT_WOW, PT_WOW_RATE, PT_FLUTTER, PT_FLUTTER_RATE, PT_SCRAPE,
		PT_DRIFT, PT_TAPE_SPEED, PT_AMOUNT, PT_VERTICAL, PT_HISS, PT_DROPOUTS,
		PT_HEAD_WEAR, PT_NR_TYPE, PT_NR_MODE, PT_MISTRACKING, PT_NR_STRENGTH,
		PT_SYNC, PT_BEAT_DEPTH, PT_LEVEL_DEPTH, PT_BAND_DEPTH
	};

	static bool isPresetParam( unsigned int index );

	/// Write a factory preset into `params[]`. `presetIndex` is 1-based; 0 is
	/// Custom and leaves every slider alone.
	void applyPreset( int presetIndex );

	/// Record the host's opening position, once.
	///
	/// ☠️ This MUST run before any preset can be applied. Seeding lazily from
	/// `params[]` inside the preset guard records the *preset's own* values as
	/// the host's last word, so the host's very next restatement looks like an
	/// operator edit and the dropdown snaps back to Custom -- which is the exact
	/// bug the whole `hostSent[]` mechanism exists to prevent. It is called at
	/// the top of SetFloatParameter, ahead of the PT_PRESET branch.
	void seedHostSent();

	bool compileShaders();

	/// Fold the host's spectrum buffer into `audioLevel` through an attack and
	/// release filter.
	void updateAudio();

	/// Recompute the weighted figure only when the transport it describes has
	/// actually changed. It is eight thousand evaluations of the error signal;
	/// running it every frame would be the most expensive thing in the plugin
	/// and it would be for a readout that is off by default.
	void refreshWeighted( const ferric::transport::Settings& t );

	ffglex::FFGLShader encodeShader;
	ffglex::FFGLShader tapeShader;
	ffglex::FFGLShader decodeShader;
	ffglex::FFGLScreenQuad quad;

	/// The picture, as ours, with the encode side of the compander applied.
	/// Allocated and drawn every frame whatever the noise reduction is set to:
	/// resolving MaxUV is its other job, and the tape pass is a warp that
	/// samples wherever it likes. See Shaders.h.
	ferric::PassBuffer encodeBuffer;

	/// After the medium.
	ferric::PassBuffer tapeBuffer;

	//---------------------------------------------------------------------
	// Time. See the class comment: the host's unit is not knowable in advance.
	//---------------------------------------------------------------------
	double hostTime     = -1.0;
	double lastHostTime = -1.0;

	double clockScale   = 0.0;///< 0 until decided; then 1.0 or 0.001
	double lastWallTime = -1.0;
	double wallStart    = -1.0;
	int secondsVotes    = 0;
	int millisVotes     = 0;
	double lastRawTime  = -1.0;

	/// Counts frames so the sixtieth can log what the host's clock actually
	/// looks like. One line, once, in the diag log -- and the only way to find
	/// out after the fact which unit a given host was really sending.
	int clockFrames = 0;

	/// Advances the hiss along the tape. Kept as a wrapped double and handed to
	/// the shader already reduced, for the same reason the transport's phases
	/// are: an absolute scan-sample index passes a float's resolution within a
	/// minute and the hiss freezes into blocks.
	double hissPhase = 0.0;
	double dropPhase = 0.0;

	//---------------------------------------------------------------------
	// Audio.
	//---------------------------------------------------------------------
	std::array< float, ferric::drive::kAudioBins > audioLevel = {};
	double audioClock = -1.0;

	ferric::drive::Output driveOut;
	ferric::controls::Render renderOut;

	/// The weighted figure, and the transport it was measured from.
	float cachedWf = 0.0f;
	ferric::transport::Settings cachedWfFor;
	bool cachedWfValid = false;

	/// What the render uses.
	///
	/// Zero-initialised: the constructor writes a default for every real
	/// control, but the About block's ids are never stored to -- pressing a
	/// button opens a browser and returns -- so without this GetFloatParameter
	/// hands the host whatever was on the stack for them.
	float params[ PT_COUNT ] = {};

	/// What the host last SENT, which is not the same thing.
	///
	/// ☠️ A factory preset is an OVERRIDE, not a write, because **Resolume does
	/// not consume value events**: it goes on pushing the values it still
	/// believes in, and a naive "a covered control changed, so drop to Custom"
	/// rule fires on the host's own echo before the operator has touched
	/// anything. Keeping the host's last word separately is what makes the three
	/// cases distinguishable -- the host restating itself, the host echoing our
	/// preset, and a real edit -- and the judgement is on what the value IS,
	/// never on the fact that it changed. See Presets.h.
	float hostSent[ PT_COUNT ] = {};
	bool hostSeeded            = false;

	/// GetTextParameter hands the host a bare pointer, so the string has to
	/// outlive the call.
	std::string aboutText;
};
