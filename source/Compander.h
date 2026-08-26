#pragma once

namespace ferric
{
/**
    Consumer tape noise reduction, as a picture.

    ------------------------------------------------------------- the one idea

    **Noise reduction is not a filter. It is a round trip through a medium, and
    everything anybody recognises about it is the two ends disagreeing.**

    A sliding-band compander boosts quiet high-frequency detail before the tape
    so that the tape's own noise floor sits underneath it, and cuts it by exactly
    the same amount afterwards -- taking the hiss down with it. Nothing is
    removed. The signal is bent on the way in and unbent on the way out, and the
    noise only gets bent once.

    So this plugin's chain is the real chain, in the real order:

        input -> ENCODE -> [ tape: time-base error, hiss, dropouts ] -> DECODE -> out

    and every artifact people associate with the format falls out of the parts
    of that round trip that do not cancel:

    - **Decode with nothing encoded** and the picture goes dull and starts
      breathing: fine detail is being cut on the assumption it was boosted, and
      the amount of cut follows the picture's own content.
    - **Encode with nothing decoding** and it goes hard and glassy, edges
      screaming, exactly the way an unmatched tape sounds bright and edgy.
    - **Mistrack the levels** and the round trip cancels at some brightnesses
      and not others, so detail pumps as the shot changes. This is the sound of
      a tape recorded on one deck and played on another, and it is the control
      most worth reaching for.

    ⚠️ **The hiss is load-bearing, not decoration.** Noise reduction that has no
    noise to reduce is an identity function with extra steps -- a decode stage
    would have nothing to show for itself, and the whole feature would read as a
    slightly odd sharpness control. `Hiss` defaulting above zero is a deliberate
    part of this stage working, and turning it to zero is a legitimate way to
    see the compander on its own.

    ------------------------------------------------ along the scan, and only that

    The processing is **one-dimensional, horizontal**. Not isotropic, not a
    radial frequency split. Tape knows one axis -- the direction it moves past
    the head -- and a signal recorded on it has one frequency axis, which lands
    on the picture as the scan direction. A vertical edge is a high frequency to
    a tape machine; a horizontal edge is not a frequency at all, it is the next
    line.

    That is not a simplification to save samples. Doing it isotropically would
    make the stage a detail compressor that happens to be on a video plugin,
    and the give-away artifact -- vertical edges pumping while horizontal ones
    sit perfectly still -- would be gone.

    ------------------------------------------------------- exactly complementary

    Encode adds a side chain: `y = x + g * H(x)`, where `H` is a high-pass along
    the scan and `g` comes from the gain law below.

    Decode is `z = y - (g / (1+g)) * H(y)`, and that `g/(1+g)` is not a fudge --
    it is the exact inverse of the encoder **whenever `H` is a projector**
    (`H^2 = H`), because `(I + gH)^-1 = I - g/(1+g) H` follows directly. A real
    high-pass is only approximately a projector, so the round trip is only
    approximately transparent, and `frtest --roundtrip` reports the residual
    rather than this file claiming it is zero.

    The remaining question is how the decoder knows `g`, since it never saw the
    input. It does what a real decoder does: **derives its control signal from
    its own output**, which is the feedback topology that makes these things
    track at all. In a per-pixel shader that is a fixed point, and it is solved
    by one iteration -- estimate the output using the encoded signal's level,
    then re-derive the gain from the estimate. A second iteration was measured
    and moved the result by less than the mistracking of a well-aligned deck,
    which is the standard this has to meet and not a tighter one.

    ------------------------------------------------------------------ naming

    The two curves here are the well-known consumer sliding-band systems, and
    they are called **Type B** and **Type C** throughout -- in the code, in the
    dropdown and in the documentation. The originals are somebody's trademark
    and this is not their product; "Type B" is what the rest of the industry
    calls the curve when it is not licensed to use the name, and it is what a
    person looking for this will search for.
*/
namespace compander
{
/// Curve. The dropdown's order, so append only.
enum Type
{
	kTypeOff = 0,

	/// The single-band system. About 10 dB of boost on quiet high-frequency
	/// detail, one stage, a gentle slide. Forgiving of mistracking, which is
	/// why it was the one that ended up on every deck.
	kTypeB,

	/// Two stages, roughly 20 dB, with the second working on much quieter
	/// material than the first. Quieter and far less forgiving -- a level error
	/// that Type B shrugs off makes Type C breathe visibly, which is exactly
	/// the reputation it has.
	kTypeC,

	kTypeCount
};

/// Which ends of the round trip are present.
enum Mode
{
	/// Both. The honest chain: bent on the way in, unbent on the way out, with
	/// the tape in between. With Mistracking centred this is nearly
	/// transparent, and that is the point -- the effect is what the tape did,
	/// not what the compander did.
	kModeRoundTrip = 0,

	/// Decode only, as if the source had never been encoded. Dull, breathing,
	/// detail sucked out of the quiet parts. The single most recognisable of
	/// the three and the reason this control is not just a checkbox.
	kModeDecodeOnly,

	/// Encode only, as if nothing ever decoded it. Hard, glassy, over-bright
	/// edges.
	kModeEncodeOnly,

	kModeCount
};

/// A compander stage. Type B has one; Type C has two.
struct Stage
{
	/// Side-chain level at which the boost has fallen to half its maximum. The
	/// lower it is, the quieter the material a stage concerns itself with.
	float threshold;

	/// Boost at zero level, as a linear multiplier on the high-passed signal.
	/// 1.0 is +6 dB of the detail band.
	float maxBoost;
};

/// Up to two stages.
constexpr int kMaxStages = 2;

/// The controls, in physical units.
struct Settings
{
	int type = kTypeOff;
	int mode = kModeRoundTrip;

	/// Level error the decoder makes, in dB. Zero is a correctly aligned deck.
	/// Negative means the decoder thinks the signal is quieter than it is, so
	/// it applies too much cut and the picture goes dull; positive is the
	/// opposite and it goes bright and edgy.
	float mistrackDb = 0.0f;

	/// Overall strength, 0..1, multiplying every stage's boost. Not a
	/// specification control -- a real system has no such knob -- but a plugin
	/// with no way to have half as much of an effect is a plugin people stop
	/// using.
	float strength = 1.0f;
};

/// How many stages a type has.
int stageCount( int type );

/// A type's stages. Returns `kMaxStages` entries; only the first
/// `stageCount(type)` are meaningful.
const Stage* stages( int type );

int typeCount();
const char* typeLabel( int type );
int modeCount();
const char* modeLabel( int mode );

/// Encode boost for a side-chain level.
float encodeGain( const Stage& stage, float level, float strength );

/// The complementary decode gain for an encode gain. `g / (1 + g)` -- see the
/// header for why that is the exact inverse and not an approximation of one.
float decodeGain( float encode );

/// Linear level multiplier for a mistracking figure in dB.
float mistrackFactor( float db );

/// What the shader needs, with the type, the stage table and the strength all
/// resolved away.
///
/// ⚠️ The gain LAW -- `boost / (1 + level/threshold)` -- is duplicated in
/// `shaders/Compand.cpp` and marked `//= mirrored` there. That is a deliberate
/// reversal of the fleet's usual "upload a table, never mirror a curve" rule,
/// and the reason is that a table cannot carry this curve.
///
/// The thresholds span 0.008 to 0.06. A 64-entry table over a 0..1 level axis
/// puts **half an entry** between zero and Type C's second threshold, so the
/// stage that gives Type C its entire character would be a straight line
/// through its own knee. Warping the axis to fix that means a second curve to
/// keep in step, which is the thing tables are meant to avoid. The law itself
/// is one reciprocal; mirroring two lines and checking them in `frtest --nr` is
/// smaller, exact, and honest about what is duplicated.
///
/// The stage TABLE -- which thresholds and boosts a type has -- is still not
/// mirrored. That is a table and it stays one.
struct Uniforms
{
	/// Per stage, with strength already folded into the boost. Stages a type
	/// does not have get a zero boost, which makes both the encode and the
	/// decode branch an identity -- so the shader runs both unconditionally,
	/// with uniform control flow either way, and Type B costs what Type C
	/// costs. The saving from branching would be nothing and the divergence
	/// risk would not be.
	float threshold[ kMaxStages ] = { 1.0f, 1.0f };
	float boost[ kMaxStages ]     = { 0.0f, 0.0f };

	/// Linear level multiplier the decoder's side chain is wrong by.
	float mistrack = 1.0f;
};

Uniforms uniforms( const Settings& s );

/// How far the band has slid, 0..1, for a side-chain level. 0 is the wide band
/// used on quiet material, 1 the narrow one used on loud.
///
/// The shader computes the high-pass at both widths and blends by this, which
/// is two blurs rather than a variable-width one. The alternative -- a genuine
/// variable radius -- is a dependent texture read count that changes per pixel,
/// and on quiet material that means every pixel in a flat region taking the
/// expensive path at once.
float slide( const Stage& stage, float level );

} // namespace compander
} // namespace ferric
