#include "../Shaders.h"

namespace ferric::shaders
{
/**
    The compander, on the GPU.

    One tap set, eleven fetches, and everything else is scalar arithmetic on top
    of it -- the two low-pass widths the sliding band moves between, and the
    side-chain level, all read off the same eleven samples. See Shaders.h.

    ⚠️ `ferricGain` and `ferricSlide` are MIRRORS of `compander::encodeGain` and
    `compander::slide`. Two lines each, marked `//= mirrored` in both places,
    and checked by `frtest --nr`. Compander.h has the argument for why this one
    curve is duplicated where the rest of the fleet would upload a table.
*/
const char* const kCompandFunctions = R"(
//---------------------------------------------------------------------------
// The compander, as uniforms. The type, the stage table and the strength are
// all resolved away on the CPU: a stage this type does not have arrives with a
// zero boost, which makes both branches an identity.
//---------------------------------------------------------------------------
uniform float NrThresh[ 2 ];
uniform float NrBoost[ 2 ];
uniform float NrMistrack;   // linear level multiplier the decoder is wrong by
uniform float NrTexelX;     // 1.0 / render width

// Eleven taps at one-pixel spacing. Wide is a sigma-3 weighting of them and
// narrow a sigma-1, so the band the compander works on can slide between two
// corner frequencies for the cost of one fetch set. Unnormalised; the loop
// divides by the weight it accumulated, which is cheaper than getting six
// decimal places right twice.
const float kWide[ 6 ]   = float[ 6 ]( 1.0, 0.945959, 0.800737, 0.606531, 0.411112, 0.249352 );
const float kNarrow[ 6 ] = float[ 6 ]( 1.0, 0.606531, 0.135335, 0.011109, 0.000335, 0.0000037 );

struct FerricScan
{
	vec3 lpWide;
	vec3 lpNarrow;
	/// Mean absolute deviation from the wide low-pass, on luma. The side chain's
	/// rectifier: what the compander decides how hard to work from.
	float level;
};

/// `base` is unscaled 0..1 uv; `maxUV` is applied at the fetch, so this works
/// against a padded host texture and an unpadded buffer of ours alike.
FerricScan ferricScanAt( sampler2D tex, vec2 base, vec2 maxUV )
{
	FerricScan s;
	s.lpWide = vec3( 0.0 );
	s.lpNarrow = vec3( 0.0 );

	float wSum = 0.0;
	float nSum = 0.0;
	vec3 taps[ 11 ];

	// Half a texel inside the picture at every fetch. GL_LINEAR at the very
	// edge of a padded host texture takes half its weight from undrawn padding,
	// and the compander would read that as a colossal high-frequency edge and
	// slam the whole side chain.
	float halfTexel = 0.5 * NrTexelX;

	for( int k = -5; k <= 5; ++k )
	{
		float x = clamp( base.x + float( k ) * NrTexelX, halfTexel, 1.0 - halfTexel );
		vec3 c = texture( tex, vec2( x, base.y ) * maxUV ).rgb;

		int a = abs( k );
		taps[ k + 5 ] = c;
		s.lpWide += c * kWide[ a ];
		s.lpNarrow += c * kNarrow[ a ];
		wSum += kWide[ a ];
		nSum += kNarrow[ a ];
	}

	s.lpWide /= wSum;
	s.lpNarrow /= nSum;

	float dev = 0.0;
	for( int k = 0; k < 11; ++k )
		dev += abs( dot( taps[ k ] - s.lpWide, vec3( 0.299, 0.587, 0.114 ) ) );

	s.level = dev * ( 1.0 / 11.0 );
	return s;
}

//= mirrored -- compander::encodeGain
float ferricGain( int stage, float level )
{
	float t = max( 1e-6, NrThresh[ stage ] );
	float l = max( 0.0, level );
	return NrBoost[ stage ] / ( 1.0 + l / t );
}
//= end mirrored

//= mirrored -- compander::slide
float ferricSlide( int stage, float level )
{
	float t = max( 1e-6, NrThresh[ stage ] );
	float l = max( 0.0, level );
	return l / ( l + t );
}
//= end mirrored

vec3 ferricEncode( vec3 c, FerricScan b )
{
	float lvl = b.level;

	float g1 = ferricGain( 0, lvl );
	vec3 d1 = c - mix( b.lpWide, b.lpNarrow, ferricSlide( 0, lvl ) );
	vec3 y = c + g1 * d1;

	// Stage two sees stage one's OUTPUT, whose detail band is (1+g1) times as
	// big, so its level is predicted rather than re-measured. That prediction is
	// the one approximation in this file and it is a good one: the low-pass of a
	// high-passed signal is near zero, so y's low-pass is c's. Re-measuring
	// would be eleven more fetches to move the second stage's gain by less than
	// the level control's own resolution.
	float lvl2 = lvl * ( 1.0 + g1 );
	float g2 = ferricGain( 1, lvl2 );
	vec3 d2 = y - mix( b.lpWide, b.lpNarrow, ferricSlide( 1, lvl2 ) );
	return y + g2 * d2;
}

vec3 ferricDecode( vec3 y, FerricScan b )
{
	// The decoder never saw the input, so it derives its control signal from its
	// own OUTPUT. That is the feedback topology, and it is why real decoders
	// track at all rather than merely approximately undoing things.
	//
	// It is a fixed point, and this is one iteration of it -- done entirely in
	// scalars, with no second fetch set. Decoding shrinks the detail band by a
	// factor that follows from the gains alone, so the encoder's level can be
	// predicted from the encoded level arithmetically.
	float lvlY = b.level * NrMistrack;

	float g1 = ferricGain( 0, lvlY );
	float g2 = ferricGain( 1, lvlY * ( 1.0 + g1 ) );
	float shrink = ( 1.0 / ( 1.0 + g1 ) ) * ( 1.0 / ( 1.0 + g2 ) );

	float lvlX = lvlY * shrink;
	g1 = ferricGain( 0, lvlX );
	float lvl2 = lvlX * ( 1.0 + g1 );
	g2 = ferricGain( 1, lvl2 );

	// ⚠️ Inverted in the OPPOSITE order to the encode. Encoding is S2(S1(x)), so
	// decoding has to be S1inv(S2inv(y)) -- stage two comes off first. This is
	// invisible with one stage, because there is nothing to get out of order,
	// and wrong by a few percent with two: exactly the size of error that reads
	// as "the emulation is a bit off" rather than as a bug.
	vec3 z = y - ( g2 / ( 1.0 + g2 ) ) * ( y - mix( b.lpWide, b.lpNarrow, ferricSlide( 1, lvl2 ) ) );
	return z - ( g1 / ( 1.0 + g1 ) ) * ( z - mix( b.lpWide, b.lpNarrow, ferricSlide( 0, lvlX ) ) );
}
)";
} // namespace ferric::shaders
