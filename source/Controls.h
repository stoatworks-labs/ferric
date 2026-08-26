#pragma once

#include "Compander.h"
#include "Drive.h"
#include "Transport.h"

namespace ferric
{
/**
    The one place a slider position becomes a physical quantity.

    -------------------------------------------------------------------- why

    **A ranged FF_TYPE_STANDARD parameter cannot have a ranged default.** The
    SDK's `SetParamInfo` clamps a default into 0..1 *before* returning, and
    `SetParamRange` can only be called afterwards because it finds the parameter
    by id. There is no `SetParamDefault`. So a control declared in hertz cannot
    declare a default in hertz -- 9 silently becomes 1. Every standard parameter
    here therefore lives in 0..1 and is converted on the way through, which is
    this file.

    The second reason arrives with the OpenFX build: both hosts will expose the
    same 0..1 controls and the same factory presets, so a conversion living in
    each host's glue would be two copies of every curve and a preset would mean
    something slightly different in Resolume and in Resolve. Both fill a
    `HostValues` and ask here.

    ------------------------------------------------------------- the curves

    Anything that is a **rate** converts exponentially, so half a slider is the
    geometric middle of the range and equal distances either side are reciprocal
    factors -- which is the only way a control spanning 2 Hz to 60 Hz is usable
    at both ends. Anything that is an **amount** converts linearly. Anything
    centred on "no error" puts that at 0.5, which is why Mistracking defaults
    there and not to zero.

    ⚠️ **`Tape Speed` runs backwards on purpose.** The physical quantity is
    seconds of tape per picture, and a faster tape means *fewer*. So the slider
    reads high-is-fast the way a transport control should, and the conversion
    inverts. Getting this the other way round is not a subtle bug -- the control
    does the opposite of its label -- but it is invisible in any test that only
    checks the control is not dead.
*/
namespace controls
{
/// The controls exactly as the host holds them: every one 0..1, option
/// parameters holding their element index.
struct HostValues
{
	//Transport.
	//
	//Tuned by rendering, not by taste. The first set had Wow and Drift high
	//enough that most of the error was a CONSTANT across any one picture -- the
	//whole frame shoved sideways twenty pixels and held there -- which reads as
	//a badly centred effect rather than as a transport. Wow at 1.2 Hz moves
	//through a tenth of a cycle in one picture; Drift moves through a hundredth.
	//Only Flutter fits whole cycles into a frame, so Flutter is what makes the
	//default look like the thing this plugin is named after, and it leads.
	float machine     = 0.0f;
	float wow         = 0.30f;
	float wowRate     = 0.50f;
	float flutter     = 0.45f;
	float flutterRate = 0.45f;
	float scrape      = 0.12f;
	float drift       = 0.12f;
	float tapeSpeed   = 0.36f;
	float amount      = 0.18f;
	float vertical    = 0.20f;

	//Tape. Hiss is NOT zero by default, and that is a design decision rather
	//than a taste one: the noise reduction stage has nothing to reduce without
	//it, so a plugin shipped with hiss off would have half its controls appear
	//to do nothing. See Compander.h.
	float hiss     = 0.25f;
	float dropouts = 0.0f;
	float headWear = 0.20f;

	//Noise reduction
	float nrType      = 0.0f;
	float nrMode      = 0.0f;
	float mistracking = 0.5f;///< centre is zero error
	float nrStrength  = 1.0f;

	//Reaction. Every depth starts at zero: dropped on a layer with nothing
	//routed, this is a manual tape emulation and looks like one.
	float sync       = 0.0f;
	float beatDepth  = 0.0f;
	float beatDecay  = 0.45f;
	/// Element 2, "Beat". NOT element 0, which is a quarter beat -- a plugin
	/// whose reaction fires four times a beat out of the box reads as twitchy
	/// rather than as wrongly defaulted, and the rest of the fleet has been
	/// caught by exactly that.
	float division   = 2.0f;
	float levelDepth = 0.0f;
	float bandDepth  = 0.0f;
	float route      = 0.0f;

	//Output
	float showTrace = 0.0f;
	float mix       = 1.0f;
};

/// The medium itself. No interesting arithmetic lives on this side of it -- the
/// hiss and the dropouts are per-pixel hashes and belong in the shader -- so
/// this is a conversion result and not a module.
struct Tape
{
	/// Peak hiss amplitude on a 0..1 picture.
	float hiss = 0.0f;

	/// Horizontal correlation length of the hiss, in pixels.
	///
	/// Tape noise is one-dimensional: it lies along the tape, which lands along
	/// the scan. Played back through a limited bandwidth it comes out smeared
	/// horizontally and independent vertically, which is why video noise reads
	/// as horizontal grain and film grain does not. A worn head has less
	/// bandwidth, so `Head Wear` widens this.
	float hissWidth = 1.5f;

	/// Probability that a given block of a scanline drops out.
	float dropouts = 0.0f;

	/// High-frequency loss along the scan, 0..1.
	float headWear = 0.0f;
};

/// Everything the render needs, in physical units.
struct Render
{
	transport::Settings transport;
	compander::Settings nr;
	Tape tape;

	/// Which ends of the round trip actually run. Derived from the NR mode
	/// rather than read from it, because the encode pass has a second job --
	/// resolving MaxUV into a texture of ours -- and therefore runs even when
	/// it is companding nothing. See Shaders.h.
	bool encode = false;
	bool decode = false;

	float mix      = 1.0f;
	bool showTrace = false;
};

/// Element labels for the host's dropdowns.
int divisionCount();
const char* divisionLabel( int index );
/// Division in beats for a Division dropdown index.
float divisionValue( int index );

/// Read an option parameter. Option parameters hold the element value the
/// operator chose -- 0, 1, 2 -- not a 0..1 fraction, so they are rounded and
/// clamped rather than scaled. A stale composition naming an element that no
/// longer exists is why it clamps.
int option( float value, int elementCount );

/// The reactive settings these controls describe.
drive::Settings driveSettings( const HostValues& host );

/// Everything the render needs.
///
/// `lines` is the render height, so the picture quantises to one scanline per
/// output row. `driveOut` is this frame's reaction; pass a default-constructed
/// one for the manual transport.
Render render( const HostValues& host, int lines, const drive::Output& driveOut );

/// Exponential conversion, exposed because the harness checks the ends and the
/// midpoint of every rate control against it.
float expRange( float t, float lo, float hi );

} // namespace controls
} // namespace ferric
