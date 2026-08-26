#include "../Shaders.h"

namespace ferric::shaders
{
/**
    The time-base error, on the GPU.

    ⚠️ **This file is a MIRROR of `Transport.cpp`.** Every block below is marked
    `//= mirrored` and has a twin there marked the same way. Change one and
    `frtest --tbe` fails, which is the entire reason it exists.

    Three things are deliberately identical rather than idiomatic:

    - **The hash is integer.** The `fract( sin(x) * 43758.5453 )` idiom cannot
      be mirrored -- `sin` is a library function on one side and hardware on the
      other, and multiplying by forty thousand promotes the disagreement in the
      last bits to the whole answer.
    - **The sixth power is three multiplications.** `pow()` is a library call on
      the CPU and an approximation on the GPU; `r2*r2*r2` is exact on both.
    - **Constants are written the same way in both files**, including
      `1.0/0.7744140625` left as a division rather than folded to a decimal, so
      that both compilers round it to the same float.

    What CANNOT match is `sin()` itself, which disagrees in the last few bits by
    construction. `--tbe` compares within a tolerance for that reason, and the
    tolerance is a stated number rather than an epsilon somebody tuned until the
    test passed.

    The machine table is not here. It is resolved on the CPU and arrives as the
    `Tbe*` uniforms below already multiplied out -- a table has no reason to
    exist twice.
*/
const char* const kTbeFunctions = R"(
//---------------------------------------------------------------------------
// The transport, as uniforms. Rates and gains arrive with the machine profile
// already multiplied in, so nothing here knows what a machine is.
//---------------------------------------------------------------------------
uniform float TbeWowRate;      // Hz, after the machine multiplier
uniform float TbeFlutRate;     // Hz, after the machine multiplier
uniform float TbeWowGain;      // machine gain * Wow depth
uniform float TbeFlutGain;
uniform float TbeScrapeGain;
uniform float TbeDriftGain;
uniform float TbePeriodic;     // machine's once-per-revolution weight
uniform float TbeLurch;        // added straight in, from the audio drive

// The frame's phases. Computed on the CPU in double precision and wrapped --
// see Transport.h. Everything below works in `dt`, the offset within ONE
// picture, so no argument here is ever large.
uniform float TbePhaseWow1;
uniform float TbePhaseWow2;
uniform float TbePhaseFlut1;
uniform float TbePhaseFlut2;
uniform float TbePhaseScrape;
uniform float TbePhaseDrift;

uniform float TbeSeconds;      // seconds of tape per picture
uniform float TbeLines;        // scanlines the picture quantises to

#define FERRIC_TAU 6.28318530717958647692
#define FERRIC_SCRAPE_HZ 220.0
#define FERRIC_DRIFT_HZ 0.13
#define FERRIC_WOW_RATIO 0.37
#define FERRIC_FLUT_RATIO 2.13

//= mirrored -- integer hash, value noise and fbm
uint ferricHashU( uint v )
{
	v = v * 747796405u + 2891336453u;
	uint w = ( ( v >> ( ( v >> 28 ) + 4u ) ) ^ v ) * 277803737u;
	return ( w >> 22 ) ^ w;
}

float ferricHashF( int i )
{
	uint h = ferricHashU( uint( i + 65536 ) );
	return float( h & 0xFFFFFFu ) * ( 1.0 / 16777216.0 );
}

float ferricValueNoise( float x )
{
	float fx = floor( x );
	int i = int( fx );
	float f = x - fx;
	float u = f * f * ( 3.0 - 2.0 * f );

	float a = ferricHashF( i );
	float b = ferricHashF( i + 1 );
	return ( a + ( b - a ) * u ) * 2.0 - 1.0;
}

float ferricFbm( float x )
{
	return ( 0.5 * ferricValueNoise( x )
	       + 0.25 * ferricValueNoise( x * 2.03 + 11.1 )
	       + 0.125 * ferricValueNoise( x * 4.07 + 23.7 ) )
	     * ( 1.0 / 0.875 );
}
//= end mirrored

//= mirrored -- transport::error
// Returns the whole error in .x and the slow part -- wow and drift, what a sync
// separator would follow -- in .y.
vec2 ferricError( float dt )
{
	float fw = max( 0.01, TbeWowRate );
	float ff = max( 0.01, TbeFlutRate );

	float wowSine = 0.62 * sin( TbePhaseWow1 + FERRIC_TAU * fw * dt )
	              + 0.38 * sin( TbePhaseWow2 + FERRIC_TAU * fw * FERRIC_WOW_RATIO * dt + 1.7 );

	float rev = 0.5 + 0.5 * sin( TbePhaseWow1 + FERRIC_TAU * fw * dt + 0.3 );
	float rev2 = rev * rev;
	float revPulse = ( rev2 * rev2 * rev2 - 0.2255859375 ) * ( 1.0 / 0.7744140625 );

	float wowSig = wowSine + TbePeriodic * ( revPulse - wowSine );
	float wowPart = wowSig * TbeWowGain;

	float flutSig = 0.70 * sin( TbePhaseFlut1 + FERRIC_TAU * ff * dt + 0.9 )
	              + 0.30 * sin( TbePhaseFlut2 + FERRIC_TAU * ff * FERRIC_FLUT_RATIO * dt + 2.4 );
	float flutPart = flutSig * TbeFlutGain;

	float scrapePart = ferricValueNoise( TbePhaseScrape + FERRIC_SCRAPE_HZ * dt ) * TbeScrapeGain;
	float driftPart = ferricFbm( TbePhaseDrift + FERRIC_DRIFT_HZ * dt ) * TbeDriftGain;

	float slow = wowPart + driftPart;
	return vec2( slow + flutPart + scrapePart + TbeLurch, slow );
}
//= end mirrored

//= mirrored -- transport::pictureTapeOffset
// `pic` is picture space: 0..1 with y = 0 at the TOP. The flip out of GL space
// happens once, in each main(), and everything downstream of it works here.
float ferricTapeOffset( vec2 pic )
{
	float lines = max( 1.0, TbeLines );

	// Quantised to a whole scanline vertically and left continuous
	// horizontally. Both halves matter: without the floor the error varies
	// smoothly down the picture and the result is a warp rather than a scan,
	// and without the continuous x every line is a rigid offset and lines never
	// stretch.
	float line = floor( pic.y * lines );

	return ( line / lines ) * TbeSeconds + pic.x * ( TbeSeconds / lines );
}
//= end mirrored
)";
} // namespace ferric::shaders
