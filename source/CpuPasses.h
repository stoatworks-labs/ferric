#pragma once

#include "Compander.h"
#include "Controls.h"
#include "Drive.h"
#include "Transport.h"

namespace ferric
{
/**
    The three passes, on the CPU. What the OpenFX build renders with.

    ------------------------------------------------------------ what this is

    `shaders/Passes.cpp` and `shaders/Compand.cpp` written out again in C++,
    pixel for pixel, in the same order: encode, tape, decode. Not a port of the
    *model* -- the model already has one home, and every call below into
    `transport::`, `compander::` and `controls::` is the same code the FFGL build
    runs on its CPU side. What is duplicated is only what the GPU did per
    fragment: the eleven-tap scan, the encode and decode branches, the warp and
    its line wrap, head wear, the hiss and dropout indexing, the mix, and the
    trace overlay.

    ⚠️ **Every block here is marked `//= mirrored` and has a twin in the GLSL
    marked the same way.** Change one and change the other; `frtest --cpu`
    renders both and fails when they part.

    ------------------------------------------------------------ the layout

    Images are RGBA float, **premultiplied**, row 0 at the **BOTTOM** -- GL's
    order, and OFX's, so nothing on either side of this file flips. Pixel `(x, y)`
    sits at uv `((x + 0.5) / w, (y + 0.5) / h)`, which is where the rasteriser
    puts a fragment, and every fetch below is GL's `texture()` with
    `GL_LINEAR` and `GL_CLAMP_TO_EDGE`, which is how every texture the plugin
    reads is set up.

    The FFGL build's two intermediate buffers are `RGBA16F`; these are float.
    That is the one deliberate difference in kind, and it is in this build's
    favour.

    -------------------------------------------------------------- the clock

    `frameAt()` is the OpenFX clock, and it is the one place this file departs
    from the FFGL build on purpose rather than by precision. An OpenFX host
    renders frames out of order, alone and on several threads at once, so
    nothing may carry from one frame to the next. The transport never did --
    `transport::phaseAt` is already a function of absolute time. The hiss and the
    dropouts did: the FFGL build integrates their phases over clamped frame
    deltas so a stalled host does not jump the grain. Here they are `time *
    rate`, with the rate from `controls::tapeRates`, which is what the
    integration converges to for a host that never stalls.
*/
namespace cpu
{
/// Everything one frame needs, resolved before a pixel is touched: what
/// `Ferric::ProcessOpenGL` works out on the CPU and uploads as uniforms.
struct Frame
{
	int width  = 1;
	int height = 1;

	controls::Render render;
	transport::Phase phase;
	compander::Uniforms nr;

	/// Wrapped on the CPU in double, for the reason in Transport.h.
	float hissPhase = 0.0f;
	float dropPhase = 0.0f;

	/// The weighted wow-and-flutter figure the trace prints. Measured only
	/// when the trace is on; it is eight thousand evaluations of the error.
	float wfPercent = 0.0f;

	/// The reaction. Always neutral here -- see `frameAt` -- and kept only
	/// because the trace's meters read it.
	drive::Output drive;
};

/**
    The frame at `seconds` of clock, `width` x `height` pixels.

    **The Reaction group's fields in `host` are ignored** and put back to their
    defaults, which are neutral: OpenFX has no audio and no tempo, so this is
    the manual transport. `drive::compute` still runs, on a zeroed
    `drive::Input`, and returns scale 1, no lurch and no push -- the same
    arithmetic the FFGL build runs with nothing routed, rather than a second
    reduced copy of it.

    Every phase is a function of `seconds`. Nothing is remembered.
*/
Frame frameAt( const controls::HostValues& host, int width, int height, double seconds );

/// Pass one, rows `[y0, y1)` of `encoded`: the compander's encode side, or a
/// copy when it is off. Reads only the same rows of `input`.
void encodeRows( const Frame& frame, const float* input, float* encoded, int y0, int y1 );

/// Pass two, rows `[y0, y1)` of `taped`: the medium. Reads ANY row of
/// `encoded` -- it is a warp -- so pass one must have finished the whole frame.
void tapeRows( const Frame& frame, const float* encoded, float* taped, int y0, int y1 );

/// Pass three, rows `[y0, y1)` of `out`: decode, mix against `input`, and the
/// trace. Reads only the same rows of `taped` and `input`. `out` may be the
/// buffer pass one wrote, which nothing reads by then.
void decodeRows( const Frame& frame, const float* taped, const float* input, float* out, int y0,
                 int y1 );

/// All three, on one thread. For harnesses; the OpenFX plugin runs the passes
/// itself, one band of rows per thread with a barrier between them.
void render( const Frame& frame, const float* input, float* out );

} // namespace cpu
} // namespace ferric
