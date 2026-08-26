#include "../Shaders.h"

namespace ferric::shaders
{
namespace
{
constexpr const char* kHeader = "#version 410 core\nin vec2 uv;\nout vec4 fragColor;\n";

//---------------------------------------------------------------------------
// Pass one. Encode, and resolve MaxUV.
//
// This pass runs whatever the noise reduction is set to, because it has a
// second job that has nothing to do with companding: the host's input texture
// can be LARGER than the picture, with MaxUV the fraction of it that was really
// drawn. A filter that samples where it was told never notices. A warp samples
// wherever it likes, so it does -- and the tape pass is a warp. Copying into a
// buffer of our own resolves that once, here, and everything downstream reads
// an unpadded texture it can sample anywhere in.
//---------------------------------------------------------------------------
constexpr const char* kEncodeMain = R"(
uniform sampler2D InputTexture;
uniform vec2 InputMaxUV;
uniform int DoEncode;

void main()
{
	vec4 src = texture( InputTexture, uv * InputMaxUV );
	vec3 c = src.rgb;

	if( DoEncode == 1 )
		c = ferricEncode( c, ferricScanAt( InputTexture, uv, InputMaxUV ) );

	fragColor = vec4( c, src.a );
}
)";

//---------------------------------------------------------------------------
// Pass two. The medium.
//---------------------------------------------------------------------------
constexpr const char* kTapeMain = R"(
uniform sampler2D TapeTexture;   // pass one's buffer: no padding, MaxUV is 1

uniform float TbeAmount;         // peak displacement, fraction of picture width
uniform float TbeVertical;       // how much of the slow error moves the frame
uniform float TbeAspectWH;       // width / height, so vertical moves as far as horizontal

uniform float HissAmp;
uniform float HissWidth;         // correlation length, pixels along the scan
uniform float HissPhase;         // wrapped on the CPU -- see below
uniform float DropoutP;
uniform float DropBlocks;        // dropout blocks across one line
uniform float DropPhase;
uniform float HeadWear;
uniform float TexelX;
uniform float PixelsPerLine;

vec4 ferricFetch( vec2 pic )
{
	return texture( TapeTexture, clamp( vec2( pic.x, 1.0 - pic.y ), vec2( 0.0 ), vec2( 1.0 ) ) );
}

/// Head wear: bandwidth lost along the scan. Five taps, horizontal only, for
/// the same reason everything else here is horizontal -- a worn head loses high
/// frequencies, and a picture's high frequencies along the tape are its
/// vertical edges.
vec3 ferricWear( vec2 pic, float radius )
{
	if( radius <= 0.0 )
		return ferricFetch( pic ).rgb;

	float t = TexelX * radius;
	return ferricFetch( pic ).rgb * 0.40
	     + ( ferricFetch( pic + vec2( t, 0.0 ) ).rgb + ferricFetch( pic - vec2( t, 0.0 ) ).rgb ) * 0.24
	     + ( ferricFetch( pic + vec2( 2.0 * t, 0.0 ) ).rgb + ferricFetch( pic - vec2( 2.0 * t, 0.0 ) ).rgb ) * 0.06;
}

float ferricHash2( int a, int b )
{
	uint h = ferricHashU( uint( a + 65536 ) * 1664525u + uint( b + 65536 ) );
	return float( h & 0xFFFFFFu ) * ( 1.0 / 16777216.0 );
}

void main()
{
	// Out of GL space and into picture space, once, here. Everything below --
	// and everything in Tbe.cpp -- works with y = 0 at the top.
	vec2 pic = vec2( uv.x, 1.0 - uv.y );

	float dt = ferricTapeOffset( pic );
	vec2 e = ferricError( dt );

	float dx = e.x * TbeAmount;

	// Vertical hold follows the slow part only, and is converted into
	// picture-HEIGHT units so a given Amount moves the frame the same visible
	// distance up as it does sideways. Without the aspect term the vertical
	// component is nearly half as strong on a 16:9 render as on a square one,
	// which reads as the control being weak rather than as a unit mistake.
	float dy = e.y * TbeAmount * TbeVertical * TbeAspectWH;

	vec2 src = vec2( pic.x - dx, pic.y - dy );

	// Content pushed off the end of a line came from the line NEXT DOOR, because
	// that is what is adjacent to it on the tape. Wrapping to the other edge one
	// scanline over is not a trick to avoid clamping -- it is what a picture
	// looks like when the line it is made of has been shifted in time, and it is
	// where the diagonal tear at the edge of a badly tracking tape comes from.
	//
	// One wrap only. A displacement past a whole line width is further than any
	// of this is claiming to model.
	float lines = max( 1.0, TbeLines );
	if( src.x < 0.0 )
	{
		src.x += 1.0;
		src.y -= 1.0 / lines;
	}
	else if( src.x > 1.0 )
	{
		src.x -= 1.0;
		src.y += 1.0 / lines;
	}

	float alpha = ferricFetch( src ).a;
	vec3 c = ferricWear( src, HeadWear * 4.0 );

	// Hiss. Indexed along the SCAN -- one dimension, the tape's -- so it comes
	// out smeared horizontally and independent line to line. That is why video
	// noise looks like horizontal grain and film grain does not.
	//
	// ☠️ The index is built from the picture plus a phase wrapped on the CPU,
	// never from absolute tape time. At 1920 pixels a line and a hundred lines a
	// second, an absolute index passes a float's ability to separate adjacent
	// pixels within about a minute, and the hiss freezes into blocks. See
	// Transport.h -- this is the same trap, in the one place that does not go
	// through phaseAt().
	float scanPx = floor( pic.y * lines ) * PixelsPerLine + pic.x * PixelsPerLine;
	float n = ferricValueNoise( scanPx / max( 0.5, HissWidth ) + HissPhase );
	c += vec3( n * HissAmp );

	// Dropouts: a run of a line losing contact with the head. Hashed per line
	// per block so a dropout is a streak rather than a speckle, and tapered at
	// both ends because oxide does not leave in a straight edge.
	if( DropoutP > 0.0 )
	{
		float pos = pic.x * DropBlocks + DropPhase;
		float h = ferricHash2( int( floor( pic.y * lines ) ), int( floor( pos ) ) );
		float f = fract( pos );
		float taper = smoothstep( 0.0, 0.15, f ) * smoothstep( 1.0, 0.85, f );
		c = mix( c, vec3( 0.88 ), step( h, DropoutP ) * taper );
	}

	fragColor = vec4( c, alpha );
}
)";

//---------------------------------------------------------------------------
// Pass three. Decode, mix, overlay.
//---------------------------------------------------------------------------
constexpr const char* kDecodeMain = R"(
uniform sampler2D TapeTexture;   // pass two's buffer: no padding
uniform sampler2D InputTexture;  // the host's original, for the dry side of Mix
uniform vec2 InputMaxUV;
uniform int DoDecode;
uniform float MixAmount;
uniform int ShowTrace;

void main()
{
	vec4 wet = texture( TapeTexture, uv );
	vec3 c = wet.rgb;

	if( DoDecode == 1 )
		c = ferricDecode( c, ferricScanAt( TapeTexture, uv, vec2( 1.0 ) ) );

	vec4 dry = texture( InputTexture, uv * InputMaxUV );

	vec3 result = mix( dry.rgb, c, MixAmount );
	float alpha = mix( dry.a, wet.a, MixAmount );

	// The overlay is drawn AFTER the mix and is not subject to it. A diagnostic
	// that fades out with the control it is there to help you set would be worse
	// than no diagnostic.
	if( ShowTrace == 1 )
	{
		vec4 trace = ferricTrace( uv );
		result = mix( result, trace.rgb, trace.a );
		alpha = max( alpha, trace.a );
	}

	fragColor = vec4( result, alpha );
}
)";
} // namespace

/**
    The trace overlay.

    Not a preview and not a decoration: it draws the error signal **as a
    function of position down the picture**, which is the exact quantity the
    tape pass is applying. So the shape on the graticule is the shape in the
    picture, and setting Tape Speed against it is a matter of looking at how
    many cycles fit rather than nudging until it feels right.

    Underneath it are the five numbers the audio drive produces and a
    seven-segment readout of the weighted figure. There is no font here -- the
    fleet's text-capable plugins carry a glyph atlas and this one has no other
    reason to -- so the readout is seven rectangles per digit and reads
    `D.DD`, clamped at 9.99.
*/
const char* const kTraceFunctions = R"(
uniform float TraceBass;
uniform float TraceMid;
uniform float TraceHigh;
uniform float TraceLevel;
uniform float TraceBeat;
uniform float TraceWfPercent;
uniform float TraceAspect;      // width / height

float ferricRect( vec2 p, vec4 r )
{
	return step( r.x, p.x ) * step( p.x, r.z ) * step( r.y, p.y ) * step( p.y, r.w );
}

/// One seven-segment digit in a 0..1 cell. Bits: a b c d e f g = 1 2 4 8 16 32 64.
float ferricDigit( vec2 p, int d )
{
	int masks[ 10 ] = int[ 10 ]( 63, 6, 91, 79, 102, 109, 125, 7, 127, 111 );
	if( p.x < 0.0 || p.x > 1.0 || p.y < 0.0 || p.y > 1.0 )
		return 0.0;

	int m = masks[ clamp( d, 0, 9 ) ];
	float on = 0.0;

	if( ( m & 1 ) != 0 )  on = max( on, ferricRect( p, vec4( 0.20, 0.84, 0.80, 0.94 ) ) );
	if( ( m & 2 ) != 0 )  on = max( on, ferricRect( p, vec4( 0.80, 0.52, 0.90, 0.88 ) ) );
	if( ( m & 4 ) != 0 )  on = max( on, ferricRect( p, vec4( 0.80, 0.12, 0.90, 0.48 ) ) );
	if( ( m & 8 ) != 0 )  on = max( on, ferricRect( p, vec4( 0.20, 0.06, 0.80, 0.16 ) ) );
	if( ( m & 16 ) != 0 ) on = max( on, ferricRect( p, vec4( 0.10, 0.12, 0.20, 0.48 ) ) );
	if( ( m & 32 ) != 0 ) on = max( on, ferricRect( p, vec4( 0.10, 0.52, 0.20, 0.88 ) ) );
	if( ( m & 64 ) != 0 ) on = max( on, ferricRect( p, vec4( 0.20, 0.45, 0.80, 0.55 ) ) );

	return on;
}

float ferricMeter( vec2 p, vec4 box, float value )
{
	float inside = ferricRect( p, box );
	float t = ( p.y - box.y ) / max( 1e-5, box.w - box.y );
	return inside * step( t, clamp( value, 0.0, 1.0 ) );
}

/// Returns colour in rgb and coverage in a, in GL uv space.
vec4 ferricTrace( vec2 guv )
{
	// Bottom left, in GL uv where y = 0 is the bottom.
	vec4 panel = vec4( 0.03, 0.04, 0.45, 0.50 );
	if( ferricRect( guv, panel ) < 0.5 )
		return vec4( 0.0 );

	vec2 p = ( guv - panel.xy ) / ( panel.zw - panel.xy );

	vec3 col = vec3( 0.04, 0.05, 0.06 );
	float a = 0.92;

	//-------------------------------------------------------------- the plot
	// The plot's x axis is position DOWN the picture, top to bottom. Its y axis
	// is the error, centred, spanning +/- 1.5 so a fully wound transport still
	// fits on the graticule instead of clipping against the top of it.
	vec4 plot = vec4( 0.05, 0.42, 0.95, 0.95 );
	if( ferricRect( p, plot ) > 0.5 )
	{
		float px = ( p.x - plot.x ) / ( plot.z - plot.x );
		float py = ( p.y - plot.y ) / ( plot.w - plot.y );

		col = vec3( 0.07, 0.08, 0.10 );

		// Centre line, and the two-thirds graticule.
		if( abs( py - 0.5 ) < 0.006 )
			col = vec3( 0.34, 0.36, 0.39 );
		// +/- 0.5 and +/- 1.0 on a +/- 1.5 scale.
		if( abs( abs( py - 0.5 ) - ( 1.0 / 6.0 ) ) < 0.004 )
			col = vec3( 0.20, 0.21, 0.24 );
		if( abs( abs( py - 0.5 ) - ( 1.0 / 3.0 ) ) < 0.004 )
			col = vec3( 0.20, 0.21, 0.24 );

		// The error at that scanline, sampled at the middle of the line so the
		// within-line stretch does not tilt the trace.
		// Full scale is +/- 1.5 error units, with the graticule at +/- 0.5 and
		// +/- 1.0, so a reading can be taken off it rather than merely looked at.
		//
		// The first version ran to +/- 3 for headroom and an ordinary transport
		// drew a flat line across the middle -- a diagnostic scaled for a
		// setting nobody uses reads as a broken diagnostic. The components sum,
		// so a transport with everything wound up can still run off the top;
		// that is a picture already visibly coming apart, and clipping the trace
		// costs nothing it was going to tell anybody.
		float v = ferricError( ferricTapeOffset( vec2( 0.5, px ) ) ).x;
		float ty = 0.5 - clamp( v / 3.0, -0.5, 0.5 );

		// A line rather than a point: the trace has to survive a slope of many
		// units per pixel, which is exactly what scrape flutter is.
		float vNext = ferricError( ferricTapeOffset( vec2( 0.5, min( 1.0, px + 0.004 ) ) ) ).x;
		float tyNext = 0.5 - clamp( vNext / 3.0, -0.5, 0.5 );
		float lo = min( ty, tyNext ) - 0.006;
		float hi = max( ty, tyNext ) + 0.006;

		if( py >= lo && py <= hi )
			col = vec3( 0.45, 0.95, 0.62 );
	}

	//------------------------------------------------------------- the meters
	// Bass, mid, high, level, beat -- so "is it hearing anything?" is answerable
	// without leaving the host.
	float meters = 0.0;
	meters = max( meters, ferricMeter( p, vec4( 0.05, 0.05, 0.10, 0.34 ), TraceBass ) );
	meters = max( meters, ferricMeter( p, vec4( 0.12, 0.05, 0.17, 0.34 ), TraceMid ) );
	meters = max( meters, ferricMeter( p, vec4( 0.19, 0.05, 0.24, 0.34 ), TraceHigh ) );
	meters = max( meters, ferricMeter( p, vec4( 0.26, 0.05, 0.31, 0.34 ), TraceLevel ) );
	meters = max( meters, ferricMeter( p, vec4( 0.33, 0.05, 0.38, 0.34 ), TraceBeat ) );
	if( meters > 0.5 )
		col = vec3( 0.35, 0.62, 0.95 );

	//----------------------------------------------------------- the readout
	// Weighted wow and flutter, as D.DD. Clamped at 9.99 because a transport
	// past that is not being measured any more, it is being enjoyed.
	float wf = clamp( TraceWfPercent, 0.0, 9.99 );
	int d0 = int( wf );
	int d1 = int( wf * 10.0 ) - d0 * 10;
	int d2 = int( wf * 100.0 ) - d0 * 100 - d1 * 10;

	// ⚠️ Digit cells are shaped in PANEL space, which is not square and is not
	// the frame's shape either, so the correction is two conversions and not one.
	//
	// A cell `cw` wide in panel space is `cw * panelW * frameW` pixels across;
	// `ch` tall is `ch * panelH * frameH` pixels down. Wanting a digit 1.6 times
	// as tall as it is wide therefore means multiplying by the panel's aspect AND
	// by the frame's -- and the first version of this divided by the frame's,
	// which produced glyphs twenty-one pixels wide and six tall, smeared across
	// the bottom of the panel and unreadable. It looked like a blending bug.
	float cw = 0.060;
	float ch = 1.6 * cw * ( ( panel.z - panel.x ) / ( panel.w - panel.y ) ) * TraceAspect;
	float base = 0.05;

	float glyph = 0.0;
	glyph = max( glyph, ferricDigit( ( p - vec2( 0.50, base ) ) / vec2( cw, ch ), d0 ) );
	glyph = max( glyph, ferricRect( p, vec4( 0.565, base + 0.04 * ch, 0.580, base + 0.14 * ch ) ) );
	glyph = max( glyph, ferricDigit( ( p - vec2( 0.59, base ) ) / vec2( cw, ch ), d1 ) );
	glyph = max( glyph, ferricDigit( ( p - vec2( 0.66, base ) ) / vec2( cw, ch ), d2 ) );

	if( glyph > 0.5 )
		col = vec3( 0.95, 0.86, 0.42 );

	return vec4( col, a );
}
)";

std::string EncodeFragment()
{
	return std::string( kHeader ) + kCompandFunctions + kEncodeMain;
}

std::string TapeFragment()
{
	return std::string( kHeader ) + kTbeFunctions + kTapeMain;
}

std::string DecodeFragment()
{
	// Order matters: the trace calls ferricError and ferricTapeOffset, so the
	// mirrored block has to be above it.
	return std::string( kHeader ) + kCompandFunctions + kTbeFunctions + kTraceFunctions + kDecodeMain;
}

std::string TbeProbeFragment()
{
	// Assembled from the SAME kTbeFunctions the tape pass uses, so --tbe checks
	// the real code rather than a copy of it that has been kept up to date by
	// hand until the day it was not.
	return std::string( kHeader ) + kTbeFunctions + R"(
void main()
{
	vec2 pic = vec2( uv.x, 1.0 - uv.y );
	float dt = ferricTapeOffset( pic );
	vec2 e = ferricError( dt );

	// Biased into 0..1 so an ordinary 8-bit readback can carry it. The harness
	// undoes exactly this; the scale is 0.25 rather than 0.5 because the
	// components sum and a fully wound transport reaches past 1.
	fragColor = vec4( e.x * 0.25 + 0.5, e.y * 0.25 + 0.5, dt, 1.0 );
}
)";
}

} // namespace ferric::shaders
