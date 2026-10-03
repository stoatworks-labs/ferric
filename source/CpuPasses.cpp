#include "CpuPasses.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace ferric
{
namespace cpu
{
namespace
{
//---------------------------------------------------------------------------
// GLSL's vocabulary, so the passes below read line for line against theirs.
//---------------------------------------------------------------------------
struct Vec3
{
	float r = 0.0f, g = 0.0f, b = 0.0f;
};

inline Vec3 operator+( Vec3 a, Vec3 b ) { return { a.r + b.r, a.g + b.g, a.b + b.b }; }
inline Vec3 operator-( Vec3 a, Vec3 b ) { return { a.r - b.r, a.g - b.g, a.b - b.b }; }
inline Vec3 operator*( Vec3 a, float s ) { return { a.r * s, a.g * s, a.b * s }; }
inline Vec3 operator*( float s, Vec3 a ) { return { s * a.r, s * a.g, s * a.b }; }

/// GLSL `mix`: x * (1 - a) + y * a, which is how the specification defines it.
inline float mixf( float x, float y, float a ) { return x * ( 1.0f - a ) + y * a; }
inline Vec3 mix3( Vec3 x, Vec3 y, float a ) { return { mixf( x.r, y.r, a ), mixf( x.g, y.g, a ), mixf( x.b, y.b, a ) }; }

inline float clampf( float v, float lo, float hi ) { return std::min( std::max( v, lo ), hi ); }

/// GLSL `step`: 0 below the edge, 1 at or above it.
inline float stepf( float edge, float x ) { return x < edge ? 0.0f : 1.0f; }

/// GLSL `smoothstep`, as the formula -- including with the edges reversed,
/// which the specification leaves undefined and every implementation computes
/// this way. The dropout taper relies on it.
inline float smoothstepf( float e0, float e1, float x )
{
	const float t = clampf( ( x - e0 ) / ( e1 - e0 ), 0.0f, 1.0f );
	return t * t * ( 3.0f - 2.0f * t );
}

inline float fractf( float x ) { return x - std::floor( x ); }

/// One image, as a sampler: RGBA float, premultiplied, row 0 at the bottom.
struct Plane
{
	const float* px = nullptr;
	int w           = 1;
	int h           = 1;

	/// One texel, with GL_CLAMP_TO_EDGE's answer outside the picture.
	const float* at( int x, int y ) const
	{
		x = std::clamp( x, 0, w - 1 );
		y = std::clamp( y, 0, h - 1 );
		return px + ( static_cast< size_t >( y ) * static_cast< size_t >( w ) + static_cast< size_t >( x ) ) * 4;
	}

	/// `texture()` at normalised GL coordinates, GL_LINEAR + GL_CLAMP_TO_EDGE.
	void sample( float s, float t, float out[ 4 ] ) const
	{
		const float fx  = s * static_cast< float >( w ) - 0.5f;
		const float fy  = t * static_cast< float >( h ) - 0.5f;
		const float x0f = std::floor( fx );
		const float y0f = std::floor( fy );
		const float ax  = fx - x0f;
		const float ay  = fy - y0f;
		const int x0    = static_cast< int >( x0f );
		const int y0    = static_cast< int >( y0f );

		const float* p00 = at( x0, y0 );
		const float* p10 = at( x0 + 1, y0 );
		const float* p01 = at( x0, y0 + 1 );
		const float* p11 = at( x0 + 1, y0 + 1 );

		for( int c = 0; c < 4; ++c )
		{
			const float bottom = p00[ c ] + ( p10[ c ] - p00[ c ] ) * ax;
			const float top    = p01[ c ] + ( p11[ c ] - p01[ c ] ) * ax;
			out[ c ]           = bottom + ( top - bottom ) * ay;
		}
	}
};

inline Vec3 rgbOf( const float* p ) { return { p[ 0 ], p[ 1 ], p[ 2 ] }; }

inline float* pixelAt( float* image, int w, int x, int y )
{
	return image + ( static_cast< size_t >( y ) * static_cast< size_t >( w ) + static_cast< size_t >( x ) ) * 4;
}

//===========================================================================
// The compander. Mirrors kCompandFunctions in shaders/Compand.cpp.
//===========================================================================

//= mirrored in shaders/Compand.cpp -- kWide, kNarrow and ferricScanAt
constexpr float kWide[ 6 ]   = { 1.0f, 0.945959f, 0.800737f, 0.606531f, 0.411112f, 0.249352f };
constexpr float kNarrow[ 6 ] = { 1.0f, 0.606531f, 0.135335f, 0.011109f, 0.000335f, 0.0000037f };

struct Scan
{
	Vec3 lpWide;
	Vec3 lpNarrow;
	float level = 0.0f;
};

/// Eleven taps along row `y`, centred on `x`. On the GPU each tap is a
/// `texture()` at a texel centre clamped half a texel inside the picture,
/// which is exactly the clamped texel -- so here it is the texel.
Scan scanAt( const Plane& tex, int x, int y )
{
	Scan s;
	float wSum = 0.0f;
	float nSum = 0.0f;
	Vec3 taps[ 11 ];

	for( int k = -5; k <= 5; ++k )
	{
		const Vec3 c = rgbOf( tex.at( x + k, y ) );

		const int a   = std::abs( k );
		taps[ k + 5 ] = c;
		s.lpWide      = s.lpWide + c * kWide[ a ];
		s.lpNarrow    = s.lpNarrow + c * kNarrow[ a ];
		wSum += kWide[ a ];
		nSum += kNarrow[ a ];
	}

	s.lpWide   = { s.lpWide.r / wSum, s.lpWide.g / wSum, s.lpWide.b / wSum };
	s.lpNarrow = { s.lpNarrow.r / nSum, s.lpNarrow.g / nSum, s.lpNarrow.b / nSum };

	float dev = 0.0f;
	for( int k = 0; k < 11; ++k )
	{
		const Vec3 d = taps[ k ] - s.lpWide;
		dev += std::fabs( d.r * 0.299f + d.g * 0.587f + d.b * 0.114f );
	}

	s.level = dev * ( 1.0f / 11.0f );
	return s;
}
//= end mirrored

/// `ferricGain` and `ferricSlide` are themselves the GLSL mirrors of
/// `compander::encodeGain` and `compander::slide`, so here they ARE those
/// functions -- with the strength already folded into the boost, as the
/// uniforms carry it.
inline float gain( const compander::Uniforms& nr, int stage, float level )
{
	return compander::encodeGain( compander::Stage { nr.threshold[ stage ], nr.boost[ stage ] }, level, 1.0f );
}

inline float slide( const compander::Uniforms& nr, int stage, float level )
{
	return compander::slide( compander::Stage { nr.threshold[ stage ], nr.boost[ stage ] }, level );
}

//= mirrored in shaders/Compand.cpp -- ferricEncode
Vec3 encode( const compander::Uniforms& nr, Vec3 c, const Scan& b )
{
	const float lvl = b.level;

	const float g1 = gain( nr, 0, lvl );
	const Vec3 d1  = c - mix3( b.lpWide, b.lpNarrow, slide( nr, 0, lvl ) );
	const Vec3 y   = c + g1 * d1;

	const float lvl2 = lvl * ( 1.0f + g1 );
	const float g2   = gain( nr, 1, lvl2 );
	const Vec3 d2    = y - mix3( b.lpWide, b.lpNarrow, slide( nr, 1, lvl2 ) );
	return y + g2 * d2;
}
//= end mirrored

//= mirrored in shaders/Compand.cpp -- ferricDecode
Vec3 decode( const compander::Uniforms& nr, Vec3 y, const Scan& b )
{
	const float lvlY = b.level * nr.mistrack;

	float g1           = gain( nr, 0, lvlY );
	float g2           = gain( nr, 1, lvlY * ( 1.0f + g1 ) );
	const float shrink = ( 1.0f / ( 1.0f + g1 ) ) * ( 1.0f / ( 1.0f + g2 ) );

	const float lvlX = lvlY * shrink;
	g1               = gain( nr, 0, lvlX );
	const float lvl2 = lvlX * ( 1.0f + g1 );
	g2               = gain( nr, 1, lvl2 );

	// Stage two comes off first: encoding was S2(S1(x)). See Compand.cpp.
	const Vec3 z = y - ( g2 / ( 1.0f + g2 ) ) * ( y - mix3( b.lpWide, b.lpNarrow, slide( nr, 1, lvl2 ) ) );
	return z - ( g1 / ( 1.0f + g1 ) ) * ( z - mix3( b.lpWide, b.lpNarrow, slide( nr, 0, lvlX ) ) );
}
//= end mirrored

//===========================================================================
// The medium. Mirrors kTapeMain in shaders/Passes.cpp.
//===========================================================================

//= mirrored in shaders/Passes.cpp -- ferricFetch, ferricWear and ferricHash2
/// A fetch in picture space (y = 0 at the TOP), clamped into the picture.
inline void fetch( const Plane& tape, float picX, float picY, float out[ 4 ] )
{
	tape.sample( clampf( picX, 0.0f, 1.0f ), clampf( 1.0f - picY, 0.0f, 1.0f ), out );
}

inline Vec3 fetchRgb( const Plane& tape, float picX, float picY )
{
	float p[ 4 ];
	fetch( tape, picX, picY, p );
	return { p[ 0 ], p[ 1 ], p[ 2 ] };
}

/// Head wear: five taps along the scan.
Vec3 wear( const Plane& tape, float picX, float picY, float radius, float texelX )
{
	if( radius <= 0.0f )
		return fetchRgb( tape, picX, picY );

	const float t = texelX * radius;
	return fetchRgb( tape, picX, picY ) * 0.40f
	       + ( fetchRgb( tape, picX + t, picY ) + fetchRgb( tape, picX - t, picY ) ) * 0.24f
	       + ( fetchRgb( tape, picX + 2.0f * t, picY ) + fetchRgb( tape, picX - 2.0f * t, picY ) ) * 0.06f;
}

inline float hash2( int a, int b )
{
	const uint32_t h = transport::hashU( static_cast< uint32_t >( a + 65536 ) * 1664525u
	                                     + static_cast< uint32_t >( b + 65536 ) );
	return static_cast< float >( h & 0xFFFFFFu ) * ( 1.0f / 16777216.0f );
}
//= end mirrored

//===========================================================================
// The trace overlay. Mirrors kTraceFunctions in shaders/Passes.cpp.
//===========================================================================

//= mirrored in shaders/Passes.cpp -- ferricRect, ferricDigit, ferricMeter, ferricTrace
struct Box
{
	float x, y, z, w;
};

inline float rect( float px, float py, const Box& r )
{
	return stepf( r.x, px ) * stepf( px, r.z ) * stepf( r.y, py ) * stepf( py, r.w );
}

float digit( float px, float py, int d )
{
	static constexpr int kMasks[ 10 ] = { 63, 6, 91, 79, 102, 109, 125, 7, 127, 111 };
	if( px < 0.0f || px > 1.0f || py < 0.0f || py > 1.0f )
		return 0.0f;

	const int m = kMasks[ std::clamp( d, 0, 9 ) ];
	float on    = 0.0f;

	if( ( m & 1 ) != 0 )  on = std::max( on, rect( px, py, { 0.20f, 0.84f, 0.80f, 0.94f } ) );
	if( ( m & 2 ) != 0 )  on = std::max( on, rect( px, py, { 0.80f, 0.52f, 0.90f, 0.88f } ) );
	if( ( m & 4 ) != 0 )  on = std::max( on, rect( px, py, { 0.80f, 0.12f, 0.90f, 0.48f } ) );
	if( ( m & 8 ) != 0 )  on = std::max( on, rect( px, py, { 0.20f, 0.06f, 0.80f, 0.16f } ) );
	if( ( m & 16 ) != 0 ) on = std::max( on, rect( px, py, { 0.10f, 0.12f, 0.20f, 0.48f } ) );
	if( ( m & 32 ) != 0 ) on = std::max( on, rect( px, py, { 0.10f, 0.52f, 0.20f, 0.88f } ) );
	if( ( m & 64 ) != 0 ) on = std::max( on, rect( px, py, { 0.20f, 0.45f, 0.80f, 0.55f } ) );

	return on;
}

float meter( float px, float py, const Box& box, float value )
{
	const float inside = rect( px, py, box );
	const float t      = ( py - box.y ) / std::max( 1e-5f, box.w - box.y );
	return inside * stepf( t, clampf( value, 0.0f, 1.0f ) );
}

/// Colour in rgb, coverage in a, at GL uv (y = 0 at the BOTTOM).
void trace( const Frame& f, float gx, float gy, float out[ 4 ] )
{
	out[ 0 ] = out[ 1 ] = out[ 2 ] = out[ 3 ] = 0.0f;

	const Box panel = { 0.03f, 0.04f, 0.45f, 0.50f };
	if( rect( gx, gy, panel ) < 0.5f )
		return;

	const float px = ( gx - panel.x ) / ( panel.z - panel.x );
	const float py = ( gy - panel.y ) / ( panel.w - panel.y );

	Vec3 col      = { 0.04f, 0.05f, 0.06f };
	const float a = 0.92f;

	const transport::Settings& t = f.render.transport;

	//-------------------------------------------------------------- the plot
	const Box plot = { 0.05f, 0.42f, 0.95f, 0.95f };
	if( rect( px, py, plot ) > 0.5f )
	{
		const float qx = ( px - plot.x ) / ( plot.z - plot.x );
		const float qy = ( py - plot.y ) / ( plot.w - plot.y );

		col = { 0.07f, 0.08f, 0.10f };

		if( std::fabs( qy - 0.5f ) < 0.006f )
			col = { 0.34f, 0.36f, 0.39f };
		if( std::fabs( std::fabs( qy - 0.5f ) - ( 1.0f / 6.0f ) ) < 0.004f )
			col = { 0.20f, 0.21f, 0.24f };
		if( std::fabs( std::fabs( qy - 0.5f ) - ( 1.0f / 3.0f ) ) < 0.004f )
			col = { 0.20f, 0.21f, 0.24f };

		const float v  = transport::error( t, f.phase, transport::pictureTapeOffset( t, 0.5f, qx ) ).total;
		const float ty = 0.5f - clampf( v / 3.0f, -0.5f, 0.5f );

		const float vNext =
		    transport::error( t, f.phase, transport::pictureTapeOffset( t, 0.5f, std::min( 1.0f, qx + 0.004f ) ) )
		        .total;
		const float tyNext = 0.5f - clampf( vNext / 3.0f, -0.5f, 0.5f );
		const float lo     = std::min( ty, tyNext ) - 0.006f;
		const float hi     = std::max( ty, tyNext ) + 0.006f;

		if( qy >= lo && qy <= hi )
			col = { 0.45f, 0.95f, 0.62f };
	}

	//------------------------------------------------------------- the meters
	float meters = 0.0f;
	meters       = std::max( meters, meter( px, py, { 0.05f, 0.05f, 0.10f, 0.34f }, f.drive.bass ) );
	meters       = std::max( meters, meter( px, py, { 0.12f, 0.05f, 0.17f, 0.34f }, f.drive.mid ) );
	meters       = std::max( meters, meter( px, py, { 0.19f, 0.05f, 0.24f, 0.34f }, f.drive.high ) );
	meters       = std::max( meters, meter( px, py, { 0.26f, 0.05f, 0.31f, 0.34f }, f.drive.level ) );
	meters       = std::max( meters, meter( px, py, { 0.33f, 0.05f, 0.38f, 0.34f }, f.drive.beat ) );
	if( meters > 0.5f )
		col = { 0.35f, 0.62f, 0.95f };

	//----------------------------------------------------------- the readout
	const float wf = clampf( f.wfPercent, 0.0f, 9.99f );
	const int d0   = static_cast< int >( wf );
	const int d1   = static_cast< int >( wf * 10.0f ) - d0 * 10;
	const int d2   = static_cast< int >( wf * 100.0f ) - d0 * 100 - d1 * 10;

	const float aspect = static_cast< float >( f.width ) / static_cast< float >( f.height );
	const float cw     = 0.060f;
	const float ch     = 1.6f * cw * ( ( panel.z - panel.x ) / ( panel.w - panel.y ) ) * aspect;
	const float base   = 0.05f;

	float glyph = 0.0f;
	glyph       = std::max( glyph, digit( ( px - 0.50f ) / cw, ( py - base ) / ch, d0 ) );
	glyph       = std::max( glyph, rect( px, py, { 0.565f, base + 0.04f * ch, 0.580f, base + 0.14f * ch } ) );
	glyph       = std::max( glyph, digit( ( px - 0.59f ) / cw, ( py - base ) / ch, d1 ) );
	glyph       = std::max( glyph, digit( ( px - 0.66f ) / cw, ( py - base ) / ch, d2 ) );

	if( glyph > 0.5f )
		col = { 0.95f, 0.86f, 0.42f };

	out[ 0 ] = col.r;
	out[ 1 ] = col.g;
	out[ 2 ] = col.b;
	out[ 3 ] = a;
}
//= end mirrored

} // namespace

Frame frameAt( const controls::HostValues& host, int width, int height, double seconds )
{
	Frame f;
	f.width  = std::max( 1, width );
	f.height = std::max( 1, height );

	// The Reaction group is FFGL's alone. Put back to its defaults whatever the
	// caller filled in, so a stray value cannot make this build lurch on a beat
	// it has no way to hear.
	controls::HostValues manual = host;
	const controls::HostValues neutral;
	manual.sync       = neutral.sync;
	manual.beatDepth  = neutral.beatDepth;
	manual.beatDecay  = neutral.beatDecay;
	manual.division   = neutral.division;
	manual.levelDepth = neutral.levelDepth;
	manual.bandDepth  = neutral.bandDepth;
	manual.route      = neutral.route;

	// A zeroed input: no bins, the default tempo, and the frame's own time.
	drive::Input in;
	in.seconds = seconds;
	f.drive    = drive::compute( controls::driveSettings( manual ), in );

	// One scanline per output row, exactly as ProcessOpenGL does it.
	f.render = controls::render( manual, f.height, f.drive );
	f.phase  = transport::phaseAt( f.render.transport, seconds );
	f.nr     = compander::uniforms( f.render.nr );

	// `time * rate`, not an integral. See the header.
	const controls::TapeRates rates = controls::tapeRates( f.render, f.width, f.height );
	f.hissPhase = static_cast< float >( controls::wrapTapePhase( seconds * rates.hiss ) );
	f.dropPhase = static_cast< float >( controls::wrapTapePhase( seconds * rates.dropouts ) );

	if( f.render.showTrace )
		f.wfPercent = transport::weightedPercent( f.render.transport );

	return f;
}

//= mirrored in shaders/Passes.cpp -- kEncodeMain
void encodeRows( const Frame& f, const float* input, float* encoded, int y0, int y1 )
{
	const Plane in { input, f.width, f.height };

	for( int y = std::max( 0, y0 ); y < std::min( f.height, y1 ); ++y )
	{
		for( int x = 0; x < f.width; ++x )
		{
			const float* src = in.at( x, y );
			Vec3 c           = rgbOf( src );

			if( f.render.encode )
				c = encode( f.nr, c, scanAt( in, x, y ) );

			float* out = pixelAt( encoded, f.width, x, y );
			out[ 0 ]   = c.r;
			out[ 1 ]   = c.g;
			out[ 2 ]   = c.b;
			out[ 3 ]   = src[ 3 ];
		}
	}
}
//= end mirrored

//= mirrored in shaders/Passes.cpp -- kTapeMain
void tapeRows( const Frame& f, const float* encoded, float* taped, int y0, int y1 )
{
	const Plane tape { encoded, f.width, f.height };

	const transport::Settings& t   = f.render.transport;
	const controls::Tape& medium   = f.render.tape;
	const float frameW             = static_cast< float >( f.width );
	const float frameH             = static_cast< float >( f.height );
	const float aspectWH           = frameW / frameH;
	const float texelX             = 1.0f / frameW;
	const float pixelsPerLine      = frameW;
	const float lines              = std::max( 1.0f, static_cast< float >( t.lines ) );
	const float hissWidth          = std::max( 0.5f, medium.hissWidth );

	for( int y = std::max( 0, y0 ); y < std::min( f.height, y1 ); ++y )
	{
		const float uvY = ( static_cast< float >( y ) + 0.5f ) / frameH;

		// Out of GL space and into picture space, once, here.
		const float picY = 1.0f - uvY;

		for( int x = 0; x < f.width; ++x )
		{
			const float picX = ( static_cast< float >( x ) + 0.5f ) / frameW;

			const float dt           = transport::pictureTapeOffset( t, picX, picY );
			const transport::Error e = transport::error( t, f.phase, dt );

			const float dx = e.total * t.amount;
			const float dy = e.slow * t.amount * t.vertical * aspectWH;

			float srcX = picX - dx;
			float srcY = picY - dy;

			// Off the end of a line is the line next door, once.
			if( srcX < 0.0f )
			{
				srcX += 1.0f;
				srcY -= 1.0f / lines;
			}
			else if( srcX > 1.0f )
			{
				srcX -= 1.0f;
				srcY += 1.0f / lines;
			}

			float at[ 4 ];
			fetch( tape, srcX, srcY, at );
			const float alpha = at[ 3 ];
			Vec3 c            = wear( tape, srcX, srcY, medium.headWear * 4.0f, texelX );

			// Hiss, indexed along the scan from the picture plus a phase
			// wrapped on the CPU -- never from absolute tape time.
			const float scanPx = std::floor( picY * lines ) * pixelsPerLine + picX * pixelsPerLine;
			const float n      = transport::valueNoise( scanPx / hissWidth + f.hissPhase );
			c                  = c + Vec3 { n * medium.hiss, n * medium.hiss, n * medium.hiss };

			if( medium.dropouts > 0.0f )
			{
				const float pos   = picX * controls::kDropBlocks + f.dropPhase;
				const float h     = hash2( static_cast< int >( std::floor( picY * lines ) ),
				                           static_cast< int >( std::floor( pos ) ) );
				const float frac  = fractf( pos );
				const float taper = smoothstepf( 0.0f, 0.15f, frac ) * smoothstepf( 1.0f, 0.85f, frac );
				c                 = mix3( c, Vec3 { 0.88f, 0.88f, 0.88f }, stepf( h, medium.dropouts ) * taper );
			}

			float* out = pixelAt( taped, f.width, x, y );
			out[ 0 ]   = c.r;
			out[ 1 ]   = c.g;
			out[ 2 ]   = c.b;
			out[ 3 ]   = alpha;
		}
	}
}
//= end mirrored

//= mirrored in shaders/Passes.cpp -- kDecodeMain
void decodeRows( const Frame& f, const float* taped, const float* input, float* out, int y0, int y1 )
{
	const Plane tape { taped, f.width, f.height };
	const Plane dryIn { input, f.width, f.height };

	const float frameW = static_cast< float >( f.width );
	const float frameH = static_cast< float >( f.height );
	const float amount = f.render.mix;

	for( int y = std::max( 0, y0 ); y < std::min( f.height, y1 ); ++y )
	{
		for( int x = 0; x < f.width; ++x )
		{
			const float* wet = tape.at( x, y );
			Vec3 c           = rgbOf( wet );

			if( f.render.decode )
				c = decode( f.nr, c, scanAt( tape, x, y ) );

			const float* dry = dryIn.at( x, y );

			Vec3 result = mix3( rgbOf( dry ), c, amount );
			float alpha = mixf( dry[ 3 ], wet[ 3 ], amount );

			// After the mix, and not subject to it.
			if( f.render.showTrace )
			{
				float overlay[ 4 ];
				trace( f, ( static_cast< float >( x ) + 0.5f ) / frameW, ( static_cast< float >( y ) + 0.5f ) / frameH,
				       overlay );
				result = mix3( result, Vec3 { overlay[ 0 ], overlay[ 1 ], overlay[ 2 ] }, overlay[ 3 ] );
				alpha  = std::max( alpha, overlay[ 3 ] );
			}

			// `out` may be the buffer pass one wrote; nothing reads it again.
			float* o = pixelAt( out, f.width, x, y );
			o[ 0 ]   = result.r;
			o[ 1 ]   = result.g;
			o[ 2 ]   = result.b;
			o[ 3 ]   = alpha;
		}
	}
}
//= end mirrored

void render( const Frame& f, const float* input, float* out )
{
	const size_t n = static_cast< size_t >( f.width ) * static_cast< size_t >( f.height ) * 4;
	std::vector< float > encoded( n ), taped( n );

	encodeRows( f, input, encoded.data(), 0, f.height );
	tapeRows( f, encoded.data(), taped.data(), 0, f.height );
	decodeRows( f, taped.data(), input, out, 0, f.height );
}

} // namespace cpu
} // namespace ferric
