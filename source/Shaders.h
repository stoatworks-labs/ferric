#pragma once

#include <string>

/**
    The GLSL.

    Three passes, and they are the signal chain written out in order:

      Encode   The picture, into a buffer of ours, with the compander's encode
               side applied. Runs even when the noise reduction is off, because
               it has a second job -- resolving MaxUV, so that everything
               downstream reads an unpadded texture it can sample anywhere in.
      Tape     Time-base error, hiss, dropouts and head wear. The medium.
      Decode   The compander's decode side, the wet/dry mix, and the trace
               overlay. Draws straight to the host's framebuffer.

    That order is not an implementation detail, it is the whole design of the
    noise-reduction half: encoding happens before the tape and decoding after
    it, so the hiss is bent once and the picture is bent twice. See Compander.h.

    ------------------------------------------------- one tap set does everything

    Both compander passes need a high-pass at two different widths (the band
    slides) *and* a side-chain level. The obvious build fetches a neighbourhood
    per width per stage, which is four sets of taps a pass and about sixty
    fetches a pixel.

    Instead each pass fetches **one** set of eleven horizontal taps and derives
    all of it from those: the wide low-pass is a sigma-3 weighting of them, the
    narrow low-pass a sigma-1 weighting, and the level is their mean absolute
    deviation from the wide one. Eleven fetches, twice.

    The taps are horizontal and there is no vertical equivalent anywhere in this
    file. That is the point of the effect rather than a saving -- see
    Compander.h on why tape has one frequency axis.

    ------------------------------------------------------------- the mirror

    `Tbe.cpp` carries the same arithmetic as `Transport.cpp` -- the hash, the
    value noise, the fbm, `error()` and `pictureTapeOffset()`. It has to: the
    error is a function of every pixel, so the GPU has to evaluate it, while the
    harness and the weighted-percentage readout need the C++. Every mirrored
    block is marked `//= mirrored` in both files.

    What makes the mirror survivable is that it is one string and every consumer
    is assembled around it. `TapeFragment()`, `DecodeFragment()` and
    `TbeProbeFragment()` all concatenate the *same* `kTbeFunctions`, so
    `frtest --tbe` is not checking a copy of the shader that resembles the real
    one, it is checking the real one.

    The machine table is NOT mirrored, and neither is the compander's gain
    curve. Both arrive as uniforms -- a table has no reason to exist twice, and
    a curve that existed twice is a curve a preset could disagree with itself
    about.

    ------------------------------------------------------- the assembled shaders

    Two of these programs are concatenated from several strings at runtime,
    which has one consequence worth knowing before debugging one: **any line
    number the driver reports refers to a file that does not exist.** Diag logs
    which program failed, which is the part that actually narrows it down.

    And `layout`, `flat`, `active`, `filter`, `input`, `output`, `sample` and
    `common` are all GLSL reserved words. A shader that will not compile
    surfaces at runtime as "the effect does nothing", with no message anywhere
    the operator can see.
*/
namespace ferric::shaders
{
/// Shared by every pass: draws the screen quad.
///
/// MaxUV is ALWAYS set to 1 here and the scaling is done at each fetch instead.
/// The decode pass reads a host texture (which may be padded) and one of our
/// own buffers (which is not) in the same program, and a single vertex-stage
/// scale cannot serve both.
extern const char* const kVertex;

/// The mirrored block: the hash, the noise, `ferricError` and
/// `ferricTapeOffset`, plus the uniforms they read. Not a complete shader --
/// no `#version` and no `main` -- because it is concatenated into three of them.
extern const char* const kTbeFunctions;

/// The tap set and the compander. Uniforms, `ferricScan`, `ferricEncode` and
/// `ferricDecode`. Also not a complete shader.
extern const char* const kCompandFunctions;

/// The trace overlay: graticule, error waveform, band meters and a seven-segment
/// readout of the weighted figure. Concatenated into the decode pass only, and
/// it depends on `kTbeFunctions` being above it.
extern const char* const kTraceFunctions;

/// Pass one. Encode, and resolve MaxUV.
std::string EncodeFragment();

/// Pass two. The medium.
std::string TapeFragment();

/// Pass three. Decode, mix, overlay.
std::string DecodeFragment();

/// The test probe. Writes the raw error into the red channel, the slow part
/// into green and the within-picture tape offset into blue, so `--tbe` can
/// compare what the GPU computed against `Transport.cpp` directly rather than
/// inferring it from a finished picture.
///
/// Inferring it was the alternative and it is a worse test: the warp, the hiss
/// and the compander would all sit between the thing under test and the
/// measurement, so a wrong error signal and a wrong fetch would be
/// indistinguishable.
std::string TbeProbeFragment();

} // namespace ferric::shaders
