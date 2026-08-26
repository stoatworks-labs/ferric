/**
    frtest -- the offline harness.

    It drives **the real plugin class** through the real FFGL sequence in a
    headless core-profile context, including `SetTime`, `SetBeatInfo` and a
    synthetic spectrum injected through the host-facing element API. Not a
    reimplementation and not a preview: the thing under test is `Ferric`,
    compiled from the same objects that go into the bundle, and every number
    below comes out of a frame it actually rendered.

        --out PATH        render a frame
        --scene PATH      write the synthetic test card
        --list            parameters, with their types and defaults
        --set "Name=v"    set any parameter by its host-facing name (repeatable)
        --tbe             the GLSL error signal against Transport.cpp
        --weighting       the DIN weighting curve is where this repo claims it is
        --wf              the weighted figure responds to the transport
        --identity        neutral settings return the picture untouched
        --roundtrip       encode into decode cancels, and by how much
        --nr              the GLSL gain law against Compander.cpp
        --scan            the compander works along the scan and ONLY along it
        --denoise         noise reduction actually reduces noise
        --drive           the reaction arithmetic, including the bar recovery
        --clock           milliseconds and seconds hosts produce the same picture
        --presets         every factory preset is distinct and non-degenerate
        --bench           frame cost at 1080p and 4K
        --size WxH        render size (default 640x360)
        --frames N        advance this many frames at 60 fps before writing (default 2)

    ## What each check can and cannot catch

    `--tbe` is the only thing standing between `Transport.cpp` and its GLSL
    mirror in `shaders/Tbe.cpp`. It renders the error through a probe shader
    assembled from the *same string the tape pass uses* -- so it is not checking
    a lookalike -- into an RGBA32F buffer, and compares what the GPU wrote
    against the C++ at a few thousand points. **It carries its own control**: the
    same comparison against a deliberately detuned transport, which must FAIL. A
    row of agreements is exactly when to start wondering whether a test can fail
    at all.

    It cannot catch an error signal that is mirrored correctly and wrong in both
    copies. Nothing here can; that is what looking at the picture is for.

    `--weighting` checks the three things Transport.h actually claims about the
    curve -- the peak's position, and -3 dB at 0.5 Hz and 25 Hz -- rather than a
    transcribed table nobody could verify. It is deliberately narrow.

    `--identity` is the check that a plugin doing nothing really does nothing.
    Amount at zero, no hiss, no dropouts, no wear, noise reduction off: the
    output must be the input. It sounds trivial and it is the fastest way to
    catch a sign convention, an off-by-half-a-texel fetch, or a pass that
    quietly runs when it should not.

    `--roundtrip` is the one that pins down the noise reduction. Encode into
    decode with the levels matched must give the picture back, because that is
    the entire claim the design rests on -- the artifacts are supposed to come
    from the two ends *disagreeing*. It prints the residual rather than merely
    passing, because the residual is a real number about a real approximation
    and hiding it would be hiding the interesting part.

    `--scan` is the give-away. The compander is one-dimensional and horizontal,
    so a vertical edge must move under it and a horizontal edge must not. Get
    that wrong and the plugin is a detail compressor that happens to be on a
    video effect -- which looks fine, works fine, and is not what this is.

    `--drive` is pure arithmetic with no GL at all. It is where the bar
    recovery, the depth carve-out and the logarithmic band split are actually
    pinned down, because none of those are judgeable by eye -- "it moves with the
    music" is true of almost every wrong answer.

    `--clock` feeds one plugin a seconds-shaped clock and another a
    milliseconds-shaped one and demands the same picture out of both. Resolume
    sends milliseconds; this harness sends seconds; the FFGL header says
    nothing. Nothing else here would notice a plugin that runs a thousand times
    fast in the only host that matters.

    `--presets` catches the degenerate ones -- a preset that renders black, or
    that is identical to another, or that does nothing at all.

    None of them catches a dead control. See `tools/sweep.py`.
*/

#include <OpenGL/OpenGL.h>
#include <OpenGL/gl3.h>
#include <zlib.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include "Compander.h"
#include "Controls.h"
#include "Drive.h"
#include "Ferric.h"
#include "Presets.h"
#include "Shaders.h"
#include "Transport.h"

using namespace ferric;

namespace
{
//---------------------------------------------------------------------------
// PNG. zlib ships with the OS, so this is fifty lines rather than a dependency.
//---------------------------------------------------------------------------
void putBigEndian( std::vector< unsigned char >& out, unsigned int value )
{
	out.push_back( static_cast< unsigned char >( ( value >> 24 ) & 0xff ) );
	out.push_back( static_cast< unsigned char >( ( value >> 16 ) & 0xff ) );
	out.push_back( static_cast< unsigned char >( ( value >> 8 ) & 0xff ) );
	out.push_back( static_cast< unsigned char >( value & 0xff ) );
}

void putChunk( std::vector< unsigned char >& out, const char* type,
               const unsigned char* data, size_t length )
{
	putBigEndian( out, static_cast< unsigned int >( length ) );
	const size_t crcStart = out.size();
	out.insert( out.end(), type, type + 4 );
	out.insert( out.end(), data, data + length );
	const unsigned long crc = crc32( 0, out.data() + crcStart,
	                                 static_cast< unsigned int >( out.size() - crcStart ) );
	putBigEndian( out, static_cast< unsigned int >( crc ) );
}

bool writePng( const std::string& path, int width, int height,
               const std::vector< unsigned char >& rgba )
{
	//Each scanline gets a filter byte. Filter 0 (none) throughout: this is a test
	//artefact, not a delivery format.
	std::vector< unsigned char > raw;
	raw.reserve( static_cast< size_t >( height ) * ( 1 + static_cast< size_t >( width ) * 4 ) );
	for( int y = 0; y < height; ++y )
	{
		raw.push_back( 0 );
		const unsigned char* row = rgba.data() + static_cast< size_t >( y ) * width * 4;
		raw.insert( raw.end(), row, row + static_cast< size_t >( width ) * 4 );
	}

	uLongf compressedSize = compressBound( static_cast< uLong >( raw.size() ) );
	std::vector< unsigned char > compressed( compressedSize );
	if( compress2( compressed.data(), &compressedSize, raw.data(),
	               static_cast< uLong >( raw.size() ), 6 ) != Z_OK )
		return false;
	compressed.resize( compressedSize );

	std::vector< unsigned char > png = { 0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n' };

	std::vector< unsigned char > ihdr;
	putBigEndian( ihdr, static_cast< unsigned int >( width ) );
	putBigEndian( ihdr, static_cast< unsigned int >( height ) );
	ihdr.push_back( 8 );//bit depth
	ihdr.push_back( 6 );//truecolour with alpha
	ihdr.push_back( 0 );//deflate
	ihdr.push_back( 0 );//adaptive filtering
	ihdr.push_back( 0 );//no interlace
	putChunk( png, "IHDR", ihdr.data(), ihdr.size() );
	putChunk( png, "IDAT", compressed.data(), compressed.size() );
	putChunk( png, "IEND", nullptr, 0 );

	FILE* file = std::fopen( path.c_str(), "wb" );
	if( file == nullptr )
		return false;
	const size_t written = std::fwrite( png.data(), 1, png.size(), file );
	std::fclose( file );
	return written == png.size();
}

//---------------------------------------------------------------------------
// The synthetic scenes.
//
// Four of them, and the split is the point: each check gets a scene that makes
// the thing it measures measurable, instead of one scene that makes everything
// vaguely visible.
//---------------------------------------------------------------------------
struct Rgba
{
	float r, g, b, a;
};

/// The general test card. Frame coordinates, 0..1, **y DOWN**.
///
/// Built to exercise a tape emulation specifically, so it carries the three
/// things this plugin is about:
///
///   - **Vertical bars of decreasing width**, which are high spatial frequency
///     ALONG the scan and therefore the only thing the compander can see. The
///     narrowest are two pixels at 640 wide, which is where head wear shows.
///   - **Horizontal bars of the same widths**, which are the control. They are
///     not a frequency to a tape machine at all, and anything that happens to
///     them is this plugin doing something it should not.
///   - **A low-contrast grey wedge**, so the sliding band has somewhere quiet to
///     work. A test card of nothing but hard edges puts the whole side chain at
///     the top of its range and the compander never leaves its knee.
Rgba scenePixel( float x, float y )
{
	//A hard vertical edge down the middle of the top band: the reference for
	//anything measuring displacement.
	if( y < 0.16f )
	{
		const float v = x < 0.5f ? 0.05f : 0.92f;
		return { v, v, v, 1.0f };
	}

	//Vertical bars, widths halving across the frame.
	if( y < 0.40f )
	{
		const float period = 0.08f / ( 1.0f + 3.0f * x );
		const float phase  = std::fmod( x, period ) / period;
		const float v      = phase < 0.5f ? 0.15f : 0.85f;
		return { v, v * 0.96f, v * 0.90f, 1.0f };
	}

	//Horizontal bars, the same widths. The control for --scan.
	if( y < 0.64f )
	{
		const float period = 0.08f / ( 1.0f + 3.0f * x );
		const float phase  = std::fmod( y - 0.40f, period ) / period;
		const float v      = phase < 0.5f ? 0.15f : 0.85f;
		return { v * 0.90f, v * 0.96f, v, 1.0f };
	}

	//A quiet wedge with faint detail on it, for the compander's low end.
	if( y < 0.84f )
	{
		const float base   = 0.10f + 0.35f * x;
		const float ripple = 0.02f * std::sin( x * 220.0f );
		const float v      = base + ripple;
		return { v, v, v, 1.0f };
	}

	//Colour, so a channel getting lost is obvious at a glance.
	const int band = std::min( 5, static_cast< int >( x * 6.0f ) );
	static const Rgba bands[ 6 ] = {
		{ 0.85f, 0.20f, 0.20f, 1.0f }, { 0.85f, 0.60f, 0.15f, 1.0f },
		{ 0.80f, 0.80f, 0.20f, 1.0f }, { 0.20f, 0.75f, 0.35f, 1.0f },
		{ 0.20f, 0.45f, 0.85f, 1.0f }, { 0.60f, 0.30f, 0.80f, 1.0f }
	};
	return bands[ band ];
}

/// A single mid grey, everywhere, fully opaque.
///
/// For `--identity` and `--roundtrip`: a flat field has no high-frequency
/// content at all, so anything that comes back other than the same flat field is
/// arithmetic rather than a judgement about tolerances.
Rgba flatPixel( float, float )
{
	return { 0.45f, 0.45f, 0.45f, 1.0f };
}

/// Vertical bars only, at a single fixed width. High frequency ALONG the scan.
///
/// For `--scan`, together with its twin below. The two scenes are the same
/// picture rotated a quarter turn, which is what makes the comparison between
/// them a statement about the axis and not about the content.
Rgba vBarsPixel( float x, float )
{
	const float v = std::fmod( x, 0.02f ) < 0.01f ? 0.30f : 0.70f;
	return { v, v, v, 1.0f };
}

/// The same bars, horizontal. NOT a frequency to a tape machine.
Rgba hBarsPixel( float, float y )
{
	const float v = std::fmod( y, 0.02f ) < 0.01f ? 0.30f : 0.70f;
	return { v, v, v, 1.0f };
}

std::vector< unsigned char > buildScene( int width, int height, Rgba ( *pixel )( float, float ) )
{
	std::vector< unsigned char > rgba( static_cast< size_t >( width ) * height * 4 );

	for( int y = 0; y < height; ++y )
	{
		//Pixel centres, not corners. Half a pixel matters to --identity, which
		//is checking that a fetch lands exactly where it started.
		const float fy = ( static_cast< float >( y ) + 0.5f ) / static_cast< float >( height );
		for( int x = 0; x < width; ++x )
		{
			const float fx = ( static_cast< float >( x ) + 0.5f ) / static_cast< float >( width );
			const Rgba c   = pixel( fx, fy );

			unsigned char* p = rgba.data() + ( static_cast< size_t >( y ) * width + x ) * 4;
			//Premultiplied, which is what an FFGL host hands over.
			p[ 0 ] = static_cast< unsigned char >( std::clamp( c.r * c.a, 0.0f, 1.0f ) * 255.0f + 0.5f );
			p[ 1 ] = static_cast< unsigned char >( std::clamp( c.g * c.a, 0.0f, 1.0f ) * 255.0f + 0.5f );
			p[ 2 ] = static_cast< unsigned char >( std::clamp( c.b * c.a, 0.0f, 1.0f ) * 255.0f + 0.5f );
			p[ 3 ] = static_cast< unsigned char >( std::clamp( c.a, 0.0f, 1.0f ) * 255.0f + 0.5f );
		}
	}

	return rgba;
}

//---------------------------------------------------------------------------
// GL plumbing.
//---------------------------------------------------------------------------
CGLContextObj createContext()
{
	const CGLPixelFormatAttribute accelerated[] = {
		kCGLPFAOpenGLProfile, static_cast< CGLPixelFormatAttribute >( kCGLOGLPVersion_GL4_Core ),
		kCGLPFAAccelerated,
		kCGLPFAColorSize, static_cast< CGLPixelFormatAttribute >( 24 ),
		kCGLPFAAlphaSize, static_cast< CGLPixelFormatAttribute >( 8 ),
		static_cast< CGLPixelFormatAttribute >( 0 )
	};
	const CGLPixelFormatAttribute software[] = {
		kCGLPFAOpenGLProfile, static_cast< CGLPixelFormatAttribute >( kCGLOGLPVersion_GL4_Core ),
		kCGLPFAColorSize, static_cast< CGLPixelFormatAttribute >( 24 ),
		kCGLPFAAlphaSize, static_cast< CGLPixelFormatAttribute >( 8 ),
		static_cast< CGLPixelFormatAttribute >( 0 )
	};

	CGLPixelFormatObj format = nullptr;
	GLint formatCount        = 0;
	if( CGLChoosePixelFormat( accelerated, &format, &formatCount ) != kCGLNoError || format == nullptr )
	{
		if( CGLChoosePixelFormat( software, &format, &formatCount ) != kCGLNoError || format == nullptr )
			return nullptr;
	}

	CGLContextObj context = nullptr;
	const CGLError error  = CGLCreateContext( format, nullptr, &context );
	CGLDestroyPixelFormat( format );
	if( error != kCGLNoError )
		return nullptr;

	CGLSetCurrentContext( context );
	return context;
}

struct Target
{
	GLuint texture = 0;
	GLuint fbo     = 0;
	int width      = 0;
	int height     = 0;
};

Target makeTarget( int width, int height, GLenum internalFormat = GL_RGBA8 )
{
	Target target;
	target.width  = width;
	target.height = height;

	const bool isFloat = internalFormat == GL_RGBA32F;

	glGenTextures( 1, &target.texture );
	glBindTexture( GL_TEXTURE_2D, target.texture );
	glTexImage2D( GL_TEXTURE_2D, 0, static_cast< GLint >( internalFormat ), width, height, 0,
	              GL_RGBA, isFloat ? GL_FLOAT : GL_UNSIGNED_BYTE, nullptr );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST );
	glBindTexture( GL_TEXTURE_2D, 0 );

	glGenFramebuffers( 1, &target.fbo );
	glBindFramebuffer( GL_FRAMEBUFFER, target.fbo );
	glFramebufferTexture2D( GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, target.texture, 0 );
	return target;
}

void releaseTarget( Target& target )
{
	if( target.fbo != 0 )
		glDeleteFramebuffers( 1, &target.fbo );
	if( target.texture != 0 )
		glDeleteTextures( 1, &target.texture );
	target = Target();
}

GLuint uploadTexture( const std::vector< unsigned char >& rgba, int width, int height )
{
	GLuint texture = 0;
	glGenTextures( 1, &texture );
	glBindTexture( GL_TEXTURE_2D, texture );
	glTexImage2D( GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba.data() );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE );
	glBindTexture( GL_TEXTURE_2D, 0 );
	return texture;
}

/// Straight out of GL, **bottom row first**. Every sampler below takes frame
/// coordinates with y down and flips here, in one place.
std::vector< unsigned char > readBytes( const Target& target )
{
	std::vector< unsigned char > pixels( static_cast< size_t >( target.width ) * target.height * 4 );
	glBindFramebuffer( GL_FRAMEBUFFER, target.fbo );
	glPixelStorei( GL_PACK_ALIGNMENT, 1 );
	glReadPixels( 0, 0, target.width, target.height, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data() );
	return pixels;
}

std::vector< float > readFloats( const Target& target )
{
	std::vector< float > pixels( static_cast< size_t >( target.width ) * target.height * 4 );
	glBindFramebuffer( GL_FRAMEBUFFER, target.fbo );
	glPixelStorei( GL_PACK_ALIGNMENT, 1 );
	glReadPixels( 0, 0, target.width, target.height, GL_RGBA, GL_FLOAT, pixels.data() );
	return pixels;
}

std::vector< unsigned char > flipRows( const std::vector< unsigned char >& image, int width, int height )
{
	std::vector< unsigned char > flipped( image.size() );
	const size_t stride = static_cast< size_t >( width ) * 4;
	for( int y = 0; y < height; ++y )
		std::memcpy( flipped.data() + static_cast< size_t >( y ) * stride,
		             image.data() + static_cast< size_t >( height - 1 - y ) * stride, stride );
	return flipped;
}

/**
    Upload a scene built by `buildScene`.

    `buildScene` works top-down, because that is how a frame is described and how
    `scenePixel` is written. **`glTexImage2D` treats its first row as v = 0, which
    is the BOTTOM.** So the rows have to be reversed on the way in, or the texture
    holds the picture upside down.

    Getting this wrong does not look like an orientation bug. The picture comes
    out inverted, somebody reaches for the readback and flips that instead, and
    then the two flips cancel for anything symmetric -- which is most of what a
    check looks at -- while `samplePixel` and the Show Field meters quietly
    address the wrong half of the frame. That is exactly what happened here: the
    first version flipped at the readback, `--field` and `--offset` both passed
    because they measure flip-insensitive quantities, and only looking at a
    bypassed frame showed it.

    One flip, here, where the convention actually changes.
*/
GLuint uploadScene( const std::vector< unsigned char >& topDown, int width, int height )
{
	return uploadTexture( flipRows( topDown, width, height ), width, height );
}


/// One pixel of a bottom-up RGBA8 read, in frame coordinates (0..1, y down),
/// un-premultiplied.
Rgba samplePixel( const std::vector< unsigned char >& bottomUp, int width, int height,
                  float fx, float fy )
{
	const int x     = std::clamp( static_cast< int >( fx * static_cast< float >( width ) ), 0, width - 1 );
	const int yDown = std::clamp( static_cast< int >( fy * static_cast< float >( height ) ), 0, height - 1 );
	const int y     = height - 1 - yDown;

	const unsigned char* p = bottomUp.data() + ( static_cast< size_t >( y ) * width + x ) * 4;
	const float a          = static_cast< float >( p[ 3 ] ) / 255.0f;
	if( a <= 0.001f )
		return { 0.0f, 0.0f, 0.0f, 0.0f };

	return { static_cast< float >( p[ 0 ] ) / 255.0f / a,
	         static_cast< float >( p[ 1 ] ) / 255.0f / a,
	         static_cast< float >( p[ 2 ] ) / 255.0f / a,
	         a };
}

//---------------------------------------------------------------------------
// Driving the plugin.
//---------------------------------------------------------------------------
/**
    The plugin plus the state needed to render into it repeatedly.

    InitGL is called only when the size changes, which is not tidiness: InitGL
    compiles the shaders, and `FFGLShader::Compile` does not free the program it
    is replacing -- so calling it per frame leaks three programs per frame, which
    over a --bench run is enough to matter and to distort the very timings it is
    there to measure.
*/
struct Driver
{
	Ferric plugin;
	int width  = 0;
	int height = 0;

	bool render( const Target& target, GLuint input, int inputWidth, int inputHeight )
	{
		FFGLTextureStruct inputStruct {};
		inputStruct.Width = inputStruct.HardwareWidth = static_cast< FFUInt32 >( inputWidth );
		inputStruct.Height = inputStruct.HardwareHeight = static_cast< FFUInt32 >( inputHeight );
		inputStruct.Handle                              = input;
		FFGLTextureStruct* inputs[ 1 ]                  = { &inputStruct };

		ProcessOpenGLStruct process {};
		process.numInputTextures = 1;
		process.inputTextures    = inputs;
		process.HostFBO          = target.fbo;

		glBindFramebuffer( GL_FRAMEBUFFER, target.fbo );
		glViewport( 0, 0, target.width, target.height );
		glClearColor( 0.0f, 0.0f, 0.0f, 0.0f );
		glClear( GL_COLOR_BUFFER_BIT );

		if( target.width != width || target.height != height )
		{
			FFGLViewportStruct viewport {};
			viewport.width  = static_cast< FFUInt32 >( target.width );
			viewport.height = static_cast< FFUInt32 >( target.height );
			if( plugin.InitGL( &viewport ) != FF_SUCCESS )
				return false;
			width  = target.width;
			height = target.height;
		}

		return plugin.ProcessOpenGL( &process ) == FF_SUCCESS;
	}
};

/// Every parameter's host-facing name, read out of the plugin itself.
///
/// Built at runtime rather than kept as a table beside Controls.h, and that is
/// not tidiness: a hand-written table is a second place for a name to live, and
/// the failure it produces is a `--set` that silently addresses nothing while
/// everything else about the run looks correct.
std::map< std::string, unsigned int > parameterIndex( Ferric& plugin )
{
	std::map< std::string, unsigned int > byName;
	for( unsigned int id = 0; id < Ferric::PT_COUNT; ++id )
	{
		const char* name = plugin.GetParamName( id );
		if( name != nullptr && name[ 0 ] != '\0' )
			byName[ name ] = id;
	}
	return byName;
}

/// A synthetic spectrum, delivered the way a host delivers one: through the
/// public element API, one bin at a time.
///
/// Without this every reactive control reads as dead, because there is no audio
/// in a headless process and the plugin is right to do nothing.
void injectSpectrum( Ferric& plugin, const std::vector< float >& bins )
{
	for( size_t i = 0; i < bins.size() && i < static_cast< size_t >( drive::kAudioBins ); ++i )
		plugin.SetParamElementValue( Ferric::PT_AUDIO, static_cast< unsigned int >( i ), bins[ i ] );
}

struct Options
{
	int width  = 640;
	int height = 360;
	int frames = 2;
	std::vector< std::pair< std::string, float > > sets;
};

bool applySets( Ferric& plugin, const Options& options )
{
	const std::map< std::string, unsigned int > byName = parameterIndex( plugin );

	for( const auto& set : options.sets )
	{
		const auto found = byName.find( set.first );
		if( found == byName.end() )
		{
			std::printf( "no parameter called '%s'\n", set.first.c_str() );
			return false;
		}

		plugin.SetFloatParameter( found->second, set.second );
	}

	return true;
}

/// Advance the plugin `frames` frames at 60 fps and leave the last one in the
/// target.
///
/// More than one frame by default because the clock calibration needs a couple
/// of frames before it commits, and because a plugin that only ever renders
/// frame zero is a plugin whose time handling is never exercised.
bool driveFrames( Driver& driver, const Target& target, GLuint input,
                  int inputWidth, int inputHeight, int frames, double startSeconds = 0.0 )
{
	//The harness DECLARES its unit rather than letting the calibration infer
	//one. An absolute time handed over in a single frame is genuinely
	//ambiguous, and an implicit unit is what lets a millisecond bug through.
	driver.plugin.SetClockScaleForTest( 1.0 );

	//A synthetic spectrum, every frame, with energy at both ends of it.
	//
	//Without this every band-driven control reads as dead and the plugin is
	//right to do nothing: there is no audio in a headless process. It is
	//harmless to the checks that want a still picture, because every reactive
	//depth defaults to zero and a depth of zero ignores the spectrum entirely --
	//which `--drive` asserts directly.
	std::vector< float > bins( drive::kAudioBins );
	for( int i = 0; i < drive::kAudioBins; ++i )
	{
		//Loud at the bottom, present at the top, quiet in the middle: a mix
		//shape rather than a flat spectrum, so Route has something to route.
		const float t = static_cast< float >( i ) / static_cast< float >( drive::kAudioBins - 1 );
		bins[ i ]     = 0.15f + 0.70f * std::exp( -t * 6.0f ) + 0.35f * t * t;
	}

	for( int f = 0; f < std::max( 1, frames ); ++f )
	{
		const double seconds = startSeconds + static_cast< double >( f ) / 60.0;
		injectSpectrum( driver.plugin, bins );
		driver.plugin.SetTime( seconds );

		//A transport that never gets a tempo cannot lock to one, and Beat Depth
		//would read as dead for reasons that have nothing to do with the plugin.
		const double barSeconds = 240.0 / 120.0;
		const double bars       = seconds / barSeconds;
		driver.plugin.SetBeatInfo( 120.0f, static_cast< float >( bars - std::floor( bars ) ) );

		if( !driver.render( target, input, inputWidth, inputHeight ) )
			return false;
	}

	return true;
}

//---------------------------------------------------------------------------
// Probe rendering, for the two checks that look at the shader's arithmetic
// directly rather than at a finished picture.
//---------------------------------------------------------------------------
struct Probe
{
	ffglex::FFGLShader shader;
	ffglex::FFGLScreenQuad quad;
	bool ready = false;

	bool build( const std::string& fragment )
	{
		if( !shader.Compile( shaders::kVertex, fragment.c_str() ) )
			return false;
		if( !quad.Initialise() )
			return false;
		ready = true;
		return true;
	}

	void release()
	{
		shader.FreeGLResources();
		quad.Release();
		ready = false;
	}
};

/// Push a transport onto a probe shader exactly the way ProcessOpenGL does.
///
/// The machine profile is multiplied out here, as it is there. If this drifted
/// from the plugin's own upload, `--tbe` would be comparing the C++ against a
/// shader nobody actually runs -- so the two orders of multiplication are the
/// same on purpose and any change belongs in both.
void setTbeUniforms( ffglex::FFGLShader& shader, const transport::Settings& t,
                     const transport::Phase& p )
{
	const transport::MachineProfile& m = transport::machineProfile( t.machine );

	shader.Set( "MaxUV", 1.0f, 1.0f );
	shader.Set( "TbeWowRate", t.wowRate * m.wowRate );
	shader.Set( "TbeFlutRate", t.flutterRate * m.flutterRate );
	shader.Set( "TbeWowGain", m.wowGain * t.wow );
	shader.Set( "TbeFlutGain", m.flutterGain * t.flutter );
	shader.Set( "TbeScrapeGain", m.scrapeGain * t.scrape );
	shader.Set( "TbeDriftGain", m.driftGain * t.drift );
	shader.Set( "TbePeriodic", m.periodic );
	shader.Set( "TbeLurch", t.lurch );
	shader.Set( "TbePhaseWow1", p.wow1 );
	shader.Set( "TbePhaseWow2", p.wow2 );
	shader.Set( "TbePhaseFlut1", p.flutter1 );
	shader.Set( "TbePhaseFlut2", p.flutter2 );
	shader.Set( "TbePhaseScrape", p.scrape );
	shader.Set( "TbePhaseDrift", p.drift );
	shader.Set( "TbeSeconds", t.secondsPerPicture );
	shader.Set( "TbeLines", static_cast< float >( t.lines ) );
}

//---------------------------------------------------------------------------
// Measurements shared between checks.
//---------------------------------------------------------------------------
/// Mean and worst absolute difference between two RGBA8 buffers, on the colour
/// channels only, in 0..255.
void compareBytes( const std::vector< unsigned char >& a, const std::vector< unsigned char >& b,
                   double* meanOut, int* worstOut )
{
	double sum  = 0.0;
	int worst   = 0;
	size_t used = 0;

	for( size_t i = 0; i + 3 < a.size() && i + 3 < b.size(); i += 4 )
	{
		for( int c = 0; c < 3; ++c )
		{
			const int d = std::abs( static_cast< int >( a[ i + c ] ) - static_cast< int >( b[ i + c ] ) );
			sum += d;
			worst = std::max( worst, d );
			++used;
		}
	}

	*meanOut  = used > 0 ? sum / static_cast< double >( used ) : 0.0;
	*worstOut = worst;
}

/// RMS difference between two RGBA8 buffers, colour channels only, in 0..255.
double rmsDifference( const std::vector< unsigned char >& a, const std::vector< unsigned char >& b )
{
	double sum  = 0.0;
	size_t used = 0;

	for( size_t i = 0; i + 3 < a.size() && i + 3 < b.size(); i += 4 )
	{
		for( int c = 0; c < 3; ++c )
		{
			const double d = static_cast< double >( a[ i + c ] ) - static_cast< double >( b[ i + c ] );
			sum += d * d;
			++used;
		}
	}

	return used > 0 ? std::sqrt( sum / static_cast< double >( used ) ) : 0.0;
}

/// Standard deviation of a buffer's luma. Zero means a flat frame, which is
/// what "the preset renders nothing" looks like.
double lumaSpread( const std::vector< unsigned char >& rgba )
{
	double sum = 0.0, sumSq = 0.0;
	size_t n = 0;

	for( size_t i = 0; i + 3 < rgba.size(); i += 4 )
	{
		const double y = 0.299 * rgba[ i ] + 0.587 * rgba[ i + 1 ] + 0.114 * rgba[ i + 2 ];
		sum += y;
		sumSq += y * y;
		++n;
	}

	if( n == 0 )
		return 0.0;

	const double mean = sum / static_cast< double >( n );
	return std::sqrt( std::max( 0.0, sumSq / static_cast< double >( n ) - mean * mean ) );
}

//===========================================================================
// The checks.
//===========================================================================

/**
    The GLSL error signal against Transport.cpp.

    Two comparisons, and splitting them is what makes the result mean anything.

    **`error()` is compared using the GPU's OWN `dt`**, read back out of the
    probe's blue channel, rather than against a `dt` recomputed here. The two
    would not agree bit for bit -- `uv` arrives from the rasteriser's
    interpolation of the quad's texture coordinates, and a last-bit difference
    either side of a scanline boundary sends `floor( v * lines )` to a different
    line. That is a real disagreement about *one texel's* line number, not about
    the error signal, and letting it into this comparison would either mask a
    genuine mirror failure behind a huge tolerance or fail the test for a reason
    that is nobody's bug.

    **`pictureTapeOffset()` is then compared separately**, and only well inside a
    scanline, where the boundary question does not arise.

    What cannot match is `sin()`, which is a library function on one side and
    hardware on the other and disagrees in the last few bits by construction.

    **Measured, on Apple Silicon, the worst disagreement over 6400 points is
    1.9e-5** -- so the tolerance is 2e-4, an order of headroom over what the
    transcendentals actually cost, and three orders tighter than the 4e-2 the
    control produces. It is a measurement with margin, not an epsilon somebody
    raised until the test passed. If this ever needs loosening, the thing to
    check first is whether something stopped being exact rather than whether the
    number is big enough.
*/
constexpr float kMirrorTolerance = 2e-4f;

bool checkTbe()
{
	constexpr int kW = 320;
	constexpr int kH = 180;

	Probe probe;
	if( !probe.build( shaders::TbeProbeFragment() ) )
	{
		std::printf( "FAIL  the probe shader would not compile\n" );
		return false;
	}

	transport::Settings t;
	t.machine           = transport::kMachineCassette;
	t.wow               = 0.60f;
	t.flutter           = 0.50f;
	t.scrape            = 0.30f;
	t.drift             = 0.40f;
	t.wowRate           = 1.2f;
	t.flutterRate       = 9.0f;
	t.secondsPerPicture = 0.10f;
	t.lines             = kH;
	t.amount            = 0.05f;
	t.lurch             = 0.02f;

	const transport::Phase phase = transport::phaseAt( t, 3.7 );

	Target target = makeTarget( kW, kH, GL_RGBA32F );
	glBindFramebuffer( GL_FRAMEBUFFER, target.fbo );
	glViewport( 0, 0, kW, kH );
	glClearColor( 0.0f, 0.0f, 0.0f, 1.0f );
	glClear( GL_COLOR_BUFFER_BIT );

	glUseProgram( probe.shader.GetGLID() );
	setTbeUniforms( probe.shader, t, phase );
	probe.quad.Draw();
	glUseProgram( 0 );

	const std::vector< float > pixels = readFloats( target );
	releaseTarget( target );
	probe.release();

	//------------------------------------------------------------------
	// 1. error(), against the GPU's own dt.
	//------------------------------------------------------------------
	float worstTotal = 0.0f;
	float worstSlow  = 0.0f;
	float worstOffset = 0.0f;
	int offsetPoints  = 0;

	//The same comparison against a transport the GPU was never given. This must
	//FAIL, and it is here because a row of agreements is exactly when to start
	//wondering whether a test can fail at all.
	transport::Settings detuned = t;
	detuned.wowRate             = t.wowRate * 1.25f;
	float worstControl          = 0.0f;

	for( int y = 0; y < kH; y += 3 )
	{
		for( int x = 0; x < kW; x += 3 )
		{
			const float* p = pixels.data() + ( static_cast< size_t >( y ) * kW + x ) * 4;

			const float gpuTotal = ( p[ 0 ] - 0.5f ) * 4.0f;
			const float gpuSlow  = ( p[ 1 ] - 0.5f ) * 4.0f;
			const float gpuDt    = p[ 2 ];

			const transport::Error cpu = transport::error( t, phase, gpuDt );
			worstTotal = std::max( worstTotal, std::fabs( cpu.total - gpuTotal ) );
			worstSlow  = std::max( worstSlow, std::fabs( cpu.slow - gpuSlow ) );

			const transport::Error control = transport::error( detuned, phase, gpuDt );
			worstControl = std::max( worstControl, std::fabs( control.total - gpuTotal ) );

			//------------------------------------------------------------
			// 2. pictureTapeOffset(), away from the scanline boundaries.
			//------------------------------------------------------------
			const float u  = ( static_cast< float >( x ) + 0.5f ) / static_cast< float >( kW );
			const float vGL = ( static_cast< float >( y ) + 0.5f ) / static_cast< float >( kH );
			const float pv = 1.0f - vGL;

			const float withinLine = pv * static_cast< float >( kH ) - std::floor( pv * static_cast< float >( kH ) );
			if( withinLine > 0.3f && withinLine < 0.7f )
			{
				const float cpuDt = transport::pictureTapeOffset( t, u, pv );
				worstOffset       = std::max( worstOffset, std::fabs( cpuDt - gpuDt ) );
				++offsetPoints;
			}
		}
	}

	std::printf( "  error total   worst disagreement %.6f  (tolerance %.6f)\n", worstTotal, kMirrorTolerance );
	std::printf( "  error slow    worst disagreement %.6f\n", worstSlow );
	std::printf( "  tape offset   worst disagreement %.6f over %d points\n", worstOffset, offsetPoints );
	std::printf( "  control       detuned transport disagrees by %.6f (must exceed the tolerance)\n", worstControl );

	bool ok = true;

	if( worstTotal > kMirrorTolerance || worstSlow > kMirrorTolerance )
	{
		std::printf( "FAIL  the GLSL error signal does not match Transport.cpp\n" );
		ok = false;
	}

	//The offset is exact arithmetic on both sides -- no transcendentals -- so it
	//gets a tolerance an order tighter than the error's.
	if( worstOffset > 1e-4f || offsetPoints < 100 )
	{
		std::printf( "FAIL  the GLSL tape offset does not match Transport.cpp (%d points sampled)\n", offsetPoints );
		ok = false;
	}

	if( worstControl <= kMirrorTolerance )
	{
		std::printf( "FAIL  the control passed, so this check cannot fail and proves nothing\n" );
		ok = false;
	}

	if( ok )
		std::printf( "PASS  tbe\n" );

	return ok;
}

/**
    The weighting curve is where Transport.h says it is.

    Deliberately narrow. It checks the three things the header actually claims --
    -3 dB at 0.5 Hz, -3 dB at 25 Hz, and a maximum near their geometric mean --
    and nothing else. Checking a transcribed table would be checking a
    transcription.
*/
bool checkWeighting()
{
	const float atLow  = transport::weighting( 0.5f );
	const float atHigh = transport::weighting( 25.0f );

	//Sweep for the peak rather than asserting where it is, so the number printed
	//is a measurement.
	float peak   = 0.0f;
	float peakHz = 0.0f;
	for( int i = 0; i <= 4000; ++i )
	{
		const float hz = 0.1f * std::pow( 1000.0f, static_cast< float >( i ) / 4000.0f );
		const float w  = transport::weighting( hz );
		if( w > peak )
		{
			peak   = w;
			peakHz = hz;
		}
	}

	const float target3dB = 1.0f / std::sqrt( 2.0f );

	std::printf( "  peak %.4f at %.3f Hz (geometric mean of 0.5 and 25 is 3.536)\n", peak, peakHz );
	std::printf( "  0.5 Hz  %.4f   25 Hz  %.4f   (-3 dB is %.4f)\n", atLow, atHigh, target3dB );
	std::printf( "  4 Hz    %.4f   (the frequency the standard names; %.3f dB off the peak)\n",
	             transport::weighting( 4.0f ),
	             20.0f * std::log10( transport::weighting( 4.0f ) / peak ) );

	bool ok = true;

	if( std::fabs( atLow - target3dB ) > 1e-3f || std::fabs( atHigh - target3dB ) > 1e-3f )
	{
		std::printf( "FAIL  the -3 dB points are not at 0.5 and 25 Hz\n" );
		ok = false;
	}

	if( std::fabs( peakHz - 3.5355f ) > 0.2f || std::fabs( peak - 1.0f ) > 1e-3f )
	{
		std::printf( "FAIL  the peak is not where a unity-gain band-pass at 3.536 Hz puts it\n" );
		ok = false;
	}

	//The claim in Transport.h that 4 Hz is within 0.01 dB of the peak.
	if( std::fabs( 20.0f * std::log10( transport::weighting( 4.0f ) / peak ) ) > 0.01f )
	{
		std::printf( "FAIL  4 Hz is further off the peak than Transport.h claims\n" );
		ok = false;
	}

	if( ok )
		std::printf( "PASS  weighting\n" );

	return ok;
}

/// The weighted figure responds to the transport, and to nothing else.
bool checkWf()
{
	transport::Settings t;
	t.machine     = transport::kMachineCassette;
	t.wow         = 0.0f;
	t.flutter     = 0.0f;
	t.scrape      = 0.0f;
	t.drift       = 0.0f;
	t.amount      = 1.0f;
	t.lines       = 1080;

	bool ok = true;

	const float silent = transport::weightedPercent( t );
	std::printf( "  a transport with every depth at zero reads %.4f %%\n", silent );
	if( silent > 1e-4f )
	{
		std::printf( "FAIL  a still transport is not reading zero\n" );
		ok = false;
	}

	//Monotonic in wow, and in flutter.
	float previous = -1.0f;
	std::printf( "  wow     " );
	for( float depth : { 0.0f, 0.25f, 0.5f, 0.75f, 1.0f } )
	{
		transport::Settings s = t;
		s.wow                 = depth;
		const float wf        = transport::weightedPercent( s );
		std::printf( "%.3f=%.4f  ", depth, wf );
		if( wf < previous )
		{
			std::printf( "\nFAIL  more wow read as less wow\n" );
			ok = false;
		}
		previous = wf;
	}
	std::printf( "%%\n" );

	previous = -1.0f;
	std::printf( "  flutter " );
	for( float depth : { 0.0f, 0.25f, 0.5f, 0.75f, 1.0f } )
	{
		transport::Settings s = t;
		s.flutter             = depth;
		const float wf        = transport::weightedPercent( s );
		std::printf( "%.3f=%.4f  ", depth, wf );
		if( wf < previous )
		{
			std::printf( "\nFAIL  more flutter read as less flutter\n" );
			ok = false;
		}
		previous = wf;
	}
	std::printf( "%%\n" );

	//A reel machine must measure better than a cassette at identical settings,
	//which is the entire justification for the machine table's numbers.
	transport::Settings cassette = t;
	cassette.wow                 = 0.5f;
	cassette.flutter             = 0.5f;
	transport::Settings reel     = cassette;
	reel.machine                 = transport::kMachineReel;

	const float wfCassette = transport::weightedPercent( cassette );
	const float wfReel     = transport::weightedPercent( reel );
	std::printf( "  cassette %.4f %%   reel %.4f %%\n", wfCassette, wfReel );
	if( wfReel >= wfCassette )
	{
		std::printf( "FAIL  the reel machine does not measure better than the cassette\n" );
		ok = false;
	}

	//The lurch is the music, not the transport, and must not reach the figure.
	transport::Settings lurched = cassette;
	lurched.lurch               = 0.8f;
	if( std::fabs( transport::weightedPercent( lurched ) - wfCassette ) > 1e-5f )
	{
		std::printf( "FAIL  the audio drive's lurch is reaching the weighted figure\n" );
		ok = false;
	}

	if( ok )
		std::printf( "PASS  wf\n" );

	return ok;
}

/// Set a parameter, by id, on a plugin the harness owns.
void set( Ferric& plugin, Ferric::ParamID id, float value )
{
	plugin.SetFloatParameter( id, value );
}

/// Everything that could disturb the picture, turned off.
void neutralise( Ferric& plugin )
{
	set( plugin, Ferric::PT_AMOUNT, 0.0f );
	set( plugin, Ferric::PT_HISS, 0.0f );
	set( plugin, Ferric::PT_DROPOUTS, 0.0f );
	set( plugin, Ferric::PT_HEAD_WEAR, 0.0f );
	set( plugin, Ferric::PT_NR_TYPE, 0.0f );
	set( plugin, Ferric::PT_SHOW_TRACE, 0.0f );
	set( plugin, Ferric::PT_MIX, 1.0f );
}

/**
    A plugin doing nothing really does nothing.

    Sounds trivial. It is the fastest way to catch a sign convention, a fetch
    landing half a texel off, a pass that runs when it should not, or a buffer
    format quietly clipping -- all of which look like "a bit soft" on a real
    picture and are unmistakable here.
*/
bool checkIdentity()
{
	constexpr int kW = 640;
	constexpr int kH = 360;

	const std::vector< unsigned char > scene = buildScene( kW, kH, scenePixel );
	const GLuint input                       = uploadScene( scene, kW, kH );

	Driver driver;
	neutralise( driver.plugin );

	Target target = makeTarget( kW, kH );
	if( !driveFrames( driver, target, input, kW, kH, 3 ) )
	{
		std::printf( "FAIL  the plugin would not render\n" );
		return false;
	}

	const std::vector< unsigned char > out = flipRows( readBytes( target ), kW, kH );
	releaseTarget( target );
	glDeleteTextures( 1, &input );

	double mean = 0.0;
	int worst   = 0;
	compareBytes( scene, out, &mean, &worst );

	std::printf( "  mean difference %.4f/255, worst %d/255\n", mean, worst );

	//One code value of slack, for the trip through RGBA16F and back. Not two:
	//two is enough to hide a half-texel fetch error on a soft edge.
	if( worst > 1 )
	{
		std::printf( "FAIL  a neutral Ferric is not transparent\n" );
		return false;
	}

	std::printf( "PASS  identity\n" );
	return true;
}

/**
    Encode into decode cancels.

    The whole noise-reduction design rests on this: the artifacts are supposed to
    come from the two ends *disagreeing*, so with the levels matched and nothing
    between them the picture has to come back. It prints the residual rather than
    merely passing, because the residual is a real number about a real
    approximation -- the high-pass is only approximately a projector, and the
    decoder's fixed point is solved once rather than converged.

    The control is the same round trip with the decoder badly off level, which
    must be far worse. Without it this check would pass just as happily against a
    compander that had been switched off.
*/
bool checkRoundTrip()
{
	constexpr int kW = 640;
	constexpr int kH = 360;

	const std::vector< unsigned char > scene = buildScene( kW, kH, scenePixel );
	const GLuint input                       = uploadScene( scene, kW, kH );

	const auto run = [ & ]( int type, float mistracking, std::vector< unsigned char >* out ) {
		Driver driver;
		neutralise( driver.plugin );
		set( driver.plugin, Ferric::PT_NR_TYPE, static_cast< float >( type ) );
		set( driver.plugin, Ferric::PT_NR_MODE, 0.0f );//round trip
		set( driver.plugin, Ferric::PT_MISTRACKING, mistracking );
		set( driver.plugin, Ferric::PT_NR_STRENGTH, 1.0f );

		Target target = makeTarget( kW, kH );
		const bool ok = driveFrames( driver, target, input, kW, kH, 2 );
		if( ok )
			*out = flipRows( readBytes( target ), kW, kH );
		releaseTarget( target );
		return ok;
	};

	bool ok = true;

	for( int type = compander::kTypeB; type <= compander::kTypeC; ++type )
	{
		std::vector< unsigned char > matched;
		if( !run( type, 0.5f, &matched ) )
		{
			std::printf( "FAIL  the plugin would not render\n" );
			ok = false;
			break;
		}

		double mean = 0.0;
		int worst   = 0;
		compareBytes( scene, matched, &mean, &worst );
		const double rms = rmsDifference( scene, matched );

		std::vector< unsigned char > mistracked;
		run( type, 0.95f, &mistracked );
		const double mistrackedRms = rmsDifference( scene, mistracked );

		std::printf( "  %-7s matched: mean %.3f worst %d rms %.3f   |   mistracked rms %.3f\n",
		             compander::typeLabel( type ), mean, worst, rms, mistrackedRms );

		//Four code values RMS. Generous against a compander that adds and
		//subtracts a band worth several times the picture's own detail, and
		//nowhere near loose enough to pass a stage that is not cancelling.
		if( rms > 4.0 )
		{
			std::printf( "FAIL  %s does not cancel itself\n", compander::typeLabel( type ) );
			ok = false;
		}

		if( mistrackedRms <= rms * 2.0 )
		{
			std::printf( "FAIL  mistracking %s barely differs from tracking it -- "
			             "this check would pass with the compander switched off\n",
			             compander::typeLabel( type ) );
			ok = false;
		}
	}

	glDeleteTextures( 1, &input );

	if( ok )
		std::printf( "PASS  roundtrip\n" );

	return ok;
}

/// The GLSL gain law against Compander.cpp.
bool checkNr()
{
	constexpr int kW = 256;
	constexpr int kH = 4;

	//The highest side-chain level the probe sweeps. Past this every stage's
	//boost is into its tail and the comparison stops being informative.
	constexpr float kMaxLevel = 0.2f;

	const std::string fragment =
		std::string( "#version 410 core\nin vec2 uv;\nout vec4 fragColor;\n" )
		+ shaders::kCompandFunctions
		+ "uniform float ProbeMaxLevel;\n"
		  "void main()\n"
		  "{\n"
		  "	float lvl = uv.x * ProbeMaxLevel;\n"
		  "	fragColor = vec4( ferricGain( 0, lvl ), ferricGain( 1, lvl ),\n"
		  "	                  ferricSlide( 0, lvl ), ferricSlide( 1, lvl ) );\n"
		  "}\n";

	Probe probe;
	if( !probe.build( fragment ) )
	{
		std::printf( "FAIL  the compander probe shader would not compile\n" );
		return false;
	}

	compander::Settings s;
	s.type                       = compander::kTypeC;
	s.strength                   = 1.0f;
	const compander::Uniforms u  = compander::uniforms( s );
	const compander::Stage* st   = compander::stages( s.type );

	Target target = makeTarget( kW, kH, GL_RGBA32F );
	glBindFramebuffer( GL_FRAMEBUFFER, target.fbo );
	glViewport( 0, 0, kW, kH );
	glClear( GL_COLOR_BUFFER_BIT );

	glUseProgram( probe.shader.GetGLID() );
	probe.shader.Set( "MaxUV", 1.0f, 1.0f );
	probe.shader.Set( "ProbeMaxLevel", kMaxLevel );
	glUniform1fv( probe.shader.FindUniform( "NrThresh" ), compander::kMaxStages, u.threshold );
	glUniform1fv( probe.shader.FindUniform( "NrBoost" ), compander::kMaxStages, u.boost );
	probe.quad.Draw();
	glUseProgram( 0 );

	const std::vector< float > pixels = readFloats( target );
	releaseTarget( target );
	probe.release();

	float worstGain  = 0.0f;
	float worstSlide = 0.0f;

	for( int x = 0; x < kW; ++x )
	{
		const float* p  = pixels.data() + static_cast< size_t >( x ) * 4;
		const float lvl = ( ( static_cast< float >( x ) + 0.5f ) / static_cast< float >( kW ) ) * kMaxLevel;

		worstGain = std::max( worstGain, std::fabs( p[ 0 ] - compander::encodeGain( st[ 0 ], lvl, s.strength ) ) );
		worstGain = std::max( worstGain, std::fabs( p[ 1 ] - compander::encodeGain( st[ 1 ], lvl, s.strength ) ) );
		worstSlide = std::max( worstSlide, std::fabs( p[ 2 ] - compander::slide( st[ 0 ], lvl ) ) );
		worstSlide = std::max( worstSlide, std::fabs( p[ 3 ] - compander::slide( st[ 1 ], lvl ) ) );
	}

	std::printf( "  gain  worst disagreement %.7f\n", worstGain );
	std::printf( "  slide worst disagreement %.7f\n", worstSlide );

	//Reciprocals and one divide, on both sides. There is no transcendental here
	//to excuse a loose tolerance.
	bool ok = true;
	if( worstGain > 1e-5f || worstSlide > 1e-5f )
	{
		std::printf( "FAIL  the GLSL gain law does not match Compander.cpp\n" );
		ok = false;
	}

	//The mistracking factor's direction, which is the one thing about this
	//stage that is easy to get backwards and impossible to see.
	if( !( compander::mistrackFactor( -6.0f ) < 1.0f && compander::mistrackFactor( 6.0f ) > 1.0f ) )
	{
		std::printf( "FAIL  mistracking runs the wrong way\n" );
		ok = false;
	}

	if( ok )
		std::printf( "PASS  nr\n" );

	return ok;
}

/**
    The compander works along the scan, and ONLY along the scan.

    The same bars, twice, a quarter turn apart. Vertical bars are a high spatial
    frequency to a tape machine and must move under the compander; horizontal
    bars are not a frequency at all -- they are the next line -- and must not
    move at all.

    This is the check that keeps the effect what it is. An isotropic
    implementation looks fine, works fine, passes `--roundtrip`, and is a detail
    compressor that happens to be on a video plugin.
*/
bool checkScan()
{
	constexpr int kW = 640;
	constexpr int kH = 360;

	const auto measure = [ & ]( Rgba ( *pixel )( float, float ), double* rmsOut ) {
		const std::vector< unsigned char > scene = buildScene( kW, kH, pixel );
		const GLuint input                       = uploadScene( scene, kW, kH );

		Driver driver;
		neutralise( driver.plugin );
		set( driver.plugin, Ferric::PT_NR_TYPE, static_cast< float >( compander::kTypeC ) );
		set( driver.plugin, Ferric::PT_NR_MODE, static_cast< float >( compander::kModeDecodeOnly ) );
		set( driver.plugin, Ferric::PT_MISTRACKING, 0.5f );

		Target target = makeTarget( kW, kH );
		const bool ok = driveFrames( driver, target, input, kW, kH, 2 );
		if( ok )
			*rmsOut = rmsDifference( scene, flipRows( readBytes( target ), kW, kH ) );

		releaseTarget( target );
		glDeleteTextures( 1, &input );
		return ok;
	};

	double vertical   = 0.0;
	double horizontal = 0.0;

	if( !measure( vBarsPixel, &vertical ) || !measure( hBarsPixel, &horizontal ) )
	{
		std::printf( "FAIL  the plugin would not render\n" );
		return false;
	}

	std::printf( "  vertical bars   changed by %.3f rms\n", vertical );
	std::printf( "  horizontal bars changed by %.3f rms\n", horizontal );

	bool ok = true;

	if( vertical < 4.0 )
	{
		std::printf( "FAIL  the compander is not acting on detail along the scan at all\n" );
		ok = false;
	}

	if( horizontal > 1.0 )
	{
		std::printf( "FAIL  the compander is acting ACROSS the scan -- it has gone isotropic\n" );
		ok = false;
	}

	if( ok )
		std::printf( "PASS  scan\n" );

	return ok;
}

/**
    Noise reduction reduces noise.

    The check the whole second half of this plugin lives or dies by, and it was
    missing from the first version of this harness -- `--roundtrip` proves the
    two ends cancel each other, `--scan` proves the axis, and neither of them
    would notice a compander that did all of that while leaving the hiss exactly
    where it was.

    A flat grey field, so the only high-frequency content in the picture IS the
    hiss and its RMS is the noise floor with nothing to separate it from. Then:

      Off            the noise floor as recorded
      Type B decode  a decoder cutting quiet detail, which is what the hiss is
      Type C decode  the same, twice, an order quieter

    Type C must beat Type B, because that is the entire difference between them
    and the reason anybody paid for it.

    ------------------------------------------- what encode-only proves, and where

    ⚠️ **Encoding alone must leave the noise floor exactly where it was**, and
    the first version of this check asserted the opposite and failed.

    That failure was the model being right. The encoder runs BEFORE the tape, so
    it never sees the tape's noise -- it cannot lift a hiss that does not exist
    yet. On a flat field it has no detail to work on either, so encode-only is a
    bit-for-bit no-op, and that is the pass order being exactly what Compander.h
    says it is. Getting a lifted noise floor here would mean the encoder had
    somehow ended up downstream of the medium, which is the one structural
    mistake that would quietly undo the whole design.

    So encode-only is checked twice: identical to Off on a flat field, and
    plainly different from it on a picture with detail in it.
*/
bool checkDenoise()
{
	constexpr int kW = 640;
	constexpr int kH = 360;

	const std::vector< unsigned char > scene = buildScene( kW, kH, flatPixel );
	const GLuint input                       = uploadScene( scene, kW, kH );

	const std::vector< unsigned char > card = buildScene( kW, kH, scenePixel );
	const GLuint cardInput                  = uploadScene( card, kW, kH );

	const auto floorForScene = [ & ]( const std::vector< unsigned char >& reference, GLuint tex,
	                                 int type, int mode, float hiss, double* rmsOut ) {
		Driver driver;
		neutralise( driver.plugin );
		//The medium, and nothing else: no transport at all, so the only thing
		//between the picture and the measurement is hiss and the compander.
		set( driver.plugin, Ferric::PT_HISS, hiss );
		set( driver.plugin, Ferric::PT_NR_TYPE, static_cast< float >( type ) );
		set( driver.plugin, Ferric::PT_NR_MODE, static_cast< float >( mode ) );
		set( driver.plugin, Ferric::PT_MISTRACKING, 0.5f );

		Target target = makeTarget( kW, kH );
		const bool ok = driveFrames( driver, target, tex, kW, kH, 3 );
		if( ok )
			*rmsOut = rmsDifference( reference, flipRows( readBytes( target ), kW, kH ) );

		releaseTarget( target );
		return ok;
	};

	const auto floorFor = [ & ]( int type, int mode, double* rmsOut ) {
		return floorForScene( scene, input, type, mode, 0.6f, rmsOut );
	};

	double off = 0.0, typeB = 0.0, typeC = 0.0, encodedFlat = 0.0;
	double cardOff = 0.0, cardEncoded = 0.0;
	const bool rendered =
		floorFor( compander::kTypeOff, compander::kModeRoundTrip, &off )
		&& floorFor( compander::kTypeB, compander::kModeDecodeOnly, &typeB )
		&& floorFor( compander::kTypeC, compander::kModeDecodeOnly, &typeC )
		&& floorFor( compander::kTypeC, compander::kModeEncodeOnly, &encodedFlat )
		//The card comparison runs with NO hiss. With the hiss on, its 6.9 rms
		//swamps what the encoder does to the detail, and the check turns into a
		//statement about the noise floor -- which is the other half's job.
		&& floorForScene( card, cardInput, compander::kTypeOff, compander::kModeRoundTrip, 0.0f, &cardOff )
		&& floorForScene( card, cardInput, compander::kTypeC, compander::kModeEncodeOnly, 0.0f, &cardEncoded );

	//The characteristic signature of a two-stage compander: its advantage grows
	//as the material gets quieter, because that is when the second stage -- whose
	//threshold is an order below the first's -- comes into its own.
	bool ok = true;

	std::printf( "  Type C's advantage over Type B, against the recorded level:\n" );
	double advantageQuiet = 0.0;
	double advantageLoud  = 0.0;
	for( float hiss : { 0.15f, 0.30f, 0.60f } )
	{
		double b = 0.0, c = 0.0;
		floorForScene( scene, input, compander::kTypeB, compander::kModeDecodeOnly, hiss, &b );
		floorForScene( scene, input, compander::kTypeC, compander::kModeDecodeOnly, hiss, &c );

		const double advantage = 20.0 * std::log10( std::max( 1e-6, c ) / std::max( 1e-6, b ) );
		std::printf( "    hiss %.2f   B %.3f   C %.3f   C is %+.2f dB on B\n", hiss, b, c, advantage );

		if( hiss < 0.2f )
			advantageQuiet = advantage;
		if( hiss > 0.5f )
			advantageLoud = advantage;
	}

	//A whole decibel of separation between the two ends. Not a large number and
	//not meant to be: the assertion is about the SHAPE -- that the second stage
	//engages on quiet material and stands down on loud -- and a threshold big
	//enough to be impressive would be a threshold tuned to today's numbers.
	if( !( advantageQuiet < advantageLoud - 1.0 ) )
	{
		std::printf( "FAIL  Type C's advantage does not grow as the material gets quieter -- "
		             "its second stage is not doing what a second stage is for\n" );
		ok = false;
	}

	glDeleteTextures( 1, &input );
	glDeleteTextures( 1, &cardInput );

	if( !rendered )
	{
		std::printf( "FAIL  the plugin would not render\n" );
		return false;
	}

	const auto dB = []( double after, double before ) {
		return 20.0 * std::log10( std::max( 1e-6, after ) / std::max( 1e-6, before ) );
	};

	std::printf( "  noise floor, rms of a flat field:\n" );
	std::printf( "    Off             %.3f\n", off );
	std::printf( "    Type B decode   %.3f  (%+.1f dB)\n", typeB, dB( typeB, off ) );
	std::printf( "    Type C decode   %.3f  (%+.1f dB)\n", typeC, dB( typeC, off ) );
	std::printf( "    Type C encode   %.3f  (%+.1f dB)  -- must be unchanged: the\n"
	             "                                       encoder runs before the tape\n",
	             encodedFlat, dB( encodedFlat, off ) );
	std::printf( "  on a picture with detail and no hiss: off %.3f, Type C encode %.3f\n",
	             cardOff, cardEncoded );

	if( !( typeB < off ) )
	{
		std::printf( "FAIL  Type B decoding did not reduce the noise floor at all\n" );
		ok = false;
	}

	if( !( typeC < typeB ) )
	{
		std::printf( "FAIL  Type C is no quieter than Type B, which is the only difference between them\n" );
		ok = false;
	}

	//The encoder is upstream of the medium, so on a field with no detail it has
	//nothing to do and the noise floor must come out untouched.
	if( std::fabs( encodedFlat - off ) > 1e-6 )
	{
		std::printf( "FAIL  encoding changed the noise floor of a flat field -- "
		             "the encoder is not upstream of the tape\n" );
		ok = false;
	}

	//And on a picture that does have detail it must plainly be doing something,
	//or the check above passes for the wrong reason.
	if( cardOff > 0.01 || cardEncoded < 2.0 )
	{
		std::printf( "FAIL  encoding a detailed picture barely changed it\n" );
		ok = false;
	}

	if( ok )
		std::printf( "PASS  denoise\n" );

	return ok;
}

/// The reaction arithmetic. No GL at all.
bool checkDrive()
{
	bool ok = true;

	//------------------------------------------------------------------
	// The band split is logarithmic, not equal thirds.
	//------------------------------------------------------------------
	{
		//Energy only in the top half of the spectrum. Under equal thirds the
		//"bass" band would still collect a third of it; under the log split it
		//gets nothing.
		std::vector< float > bins( drive::kAudioBins, 0.0f );
		for( int i = drive::kAudioBins / 2; i < drive::kAudioBins; ++i )
			bins[ i ] = 1.0f;

		float bass = 0.0f, mid = 0.0f, high = 0.0f;
		drive::bands( bins.data(), drive::kAudioBins, &bass, &mid, &high );

		std::printf( "  treble-only spectrum -> bass %.3f mid %.3f high %.3f\n", bass, mid, high );
		if( bass > 1e-6f || high < 0.5f )
		{
			std::printf( "FAIL  the band split is not logarithmic\n" );
			ok = false;
		}
	}

	//------------------------------------------------------------------
	// Silence is neutral, at every depth.
	//------------------------------------------------------------------
	{
		drive::Settings s;
		s.sync       = drive::kSyncFree;
		s.beatDepth  = 1.0f;
		s.levelDepth = 1.0f;
		s.bandDepth  = 1.0f;

		drive::Input in;
		const drive::Output out = drive::compute( s, in );

		if( std::fabs( out.lurch ) > 1e-6f || std::fabs( out.mistrackDb ) > 1e-6f )
		{
			std::printf( "FAIL  a plugin with no audio routed is still being pushed around\n" );
			ok = false;
		}

		//Level depth 1 with silence hands the whole transport to the music and
		//the music is not there, so the transport must be still.
		if( std::fabs( out.scale ) > 1e-6f )
		{
			std::printf( "FAIL  full level depth with silence did not still the transport\n" );
			ok = false;
		}
	}

	//------------------------------------------------------------------
	// Every depth at zero is exactly neutral, whatever the audio says.
	//------------------------------------------------------------------
	{
		std::vector< float > bins( drive::kAudioBins, 0.8f );

		drive::Settings s;
		s.sync = drive::kSyncLocked;

		drive::Input in;
		in.bins     = bins.data();
		in.binCount = drive::kAudioBins;
		in.seconds  = 1.234;

		const drive::Output out = drive::compute( s, in );
		if( std::fabs( out.scale - 1.0f ) > 1e-6f || out.lurch != 0.0f || out.mistrackDb != 0.0f )
		{
			std::printf( "FAIL  a plugin with every depth at zero is not a manual transport\n" );
			ok = false;
		}
	}

	//------------------------------------------------------------------
	// The beat envelope lands on the grid.
	//
	// This is the one the fleet has been caught by: a barPhase pinned at zero
	// recovers bar 0 exactly, the envelope sits at its peak forever, and Beat
	// Decay and Division both read as dead because at the instant of a beat
	// neither does anything.
	//------------------------------------------------------------------
	{
		drive::Settings s;
		s.sync         = drive::kSyncLocked;
		s.beatDepth    = 1.0f;
		s.beatDecay    = 4.0f;
		s.beatDivision = 1.0f;

		const double barSeconds = 240.0 / 120.0;//120 bpm

		float peak    = 0.0f;
		double peakAt = 0.0;
		float trough  = 1.0f;

		for( int i = 0; i <= 400; ++i )
		{
			const double seconds = static_cast< double >( i ) * barSeconds / 400.0;
			const double bars    = seconds / barSeconds;

			drive::Input in;
			in.seconds  = seconds;
			in.bpm      = 120.0f;
			in.barPhase = static_cast< float >( bars - std::floor( bars ) );

			const drive::Output out = drive::compute( s, in );
			if( out.beat > peak )
			{
				peak   = out.beat;
				peakAt = seconds;
			}
			trough = std::min( trough, out.beat );
		}

		std::printf( "  beat envelope peaks at %.4f (t=%.3fs) and falls to %.4f\n", peak, peakAt, trough );
		if( peak < 0.99f || trough > 0.05f )
		{
			std::printf( "FAIL  the beat envelope is not a beat envelope\n" );
			ok = false;
		}
	}

	//------------------------------------------------------------------
	// Routing. Natural sends bass to the lurch and treble to the decoder;
	// Inverted swaps them AND reverses the mistracking, or it is merely a
	// different routing rather than an inversion.
	//------------------------------------------------------------------
	{
		std::vector< float > bassOnly( drive::kAudioBins, 0.0f );
		for( int i = 0; i < 4; ++i )
			bassOnly[ i ] = 1.0f;

		//A spectrum with BOTH ends in it, for the mistracking half. Bass-only
		//cannot answer that question: Natural routes treble to the decoder, so
		//with no treble it correctly pushes by zero, and zero has no sign to
		//compare against. The first version of this check used the bass-only
		//spectrum for both halves and failed for exactly that reason -- which is
		//a test asking the wrong question, not a plugin doing the wrong thing.
		std::vector< float > fullRange( drive::kAudioBins, 0.9f );

		drive::Settings s;
		s.sync         = drive::kSyncLocked;
		s.beatDepth    = 1.0f;
		s.bandDepth    = 1.0f;
		s.beatDivision = 1.0f;

		drive::Input in;
		in.bins     = bassOnly.data();
		in.binCount = drive::kAudioBins;
		in.bpm      = 120.0f;
		in.barPhase = 0.0f;
		in.seconds  = 0.0;

		s.route                     = drive::kRouteNatural;
		const drive::Output natural = drive::compute( s, in );
		s.route                      = drive::kRouteInverted;
		const drive::Output inverted = drive::compute( s, in );

		std::printf( "  bass only: natural lurch %.3f | inverted lurch %.3f\n",
		             natural.lurch, inverted.lurch );

		if( !( natural.lurch > inverted.lurch ) )
		{
			std::printf( "FAIL  Natural does not send bass to the lurch\n" );
			ok = false;
		}

		in.bins = fullRange.data();

		s.route                          = drive::kRouteNatural;
		const drive::Output naturalFull  = drive::compute( s, in );
		s.route                          = drive::kRouteInverted;
		const drive::Output invertedFull = drive::compute( s, in );

		std::printf( "  full range: natural mistrack %+.3f dB | inverted mistrack %+.3f dB\n",
		             naturalFull.mistrackDb, invertedFull.mistrackDb );

		if( !( naturalFull.mistrackDb * invertedFull.mistrackDb < 0.0f ) )
		{
			std::printf( "FAIL  Inverted does not reverse the mistracking\n" );
			ok = false;
		}
	}

	if( ok )
		std::printf( "PASS  drive\n" );

	return ok;
}

/**
    A milliseconds host and a seconds host render the same picture.

    Resolume sends milliseconds. This harness sends seconds. The FFGL header
    says nothing at all, and the SDK's own example divides by a thousand without
    a word about why. Nothing else here would notice a plugin running a thousand
    times fast in the only host that matters.
*/
bool checkClock()
{
	constexpr int kW = 320;
	constexpr int kH = 180;

	const std::vector< unsigned char > scene = buildScene( kW, kH, scenePixel );
	const GLuint input                       = uploadScene( scene, kW, kH );

	const auto run = [ & ]( double scale, std::vector< unsigned char >* out ) {
		Driver driver;
		//A transport doing something visible, so the comparison has something to
		//be about.
		set( driver.plugin, Ferric::PT_HISS, 0.0f );
		set( driver.plugin, Ferric::PT_AMOUNT, 0.6f );
		driver.plugin.SetClockScaleForTest( scale );

		Target target = makeTarget( kW, kH );
		bool ok       = true;

		for( int f = 0; f < 30 && ok; ++f )
		{
			const double seconds = static_cast< double >( f ) / 60.0;
			//The host's own units: a milliseconds host says 16.67, not 0.0167.
			driver.plugin.SetTime( seconds / scale );
			driver.plugin.SetBeatInfo( 120.0f, 0.0f );
			ok = driver.render( target, input, kW, kH );
		}

		if( ok )
			*out = flipRows( readBytes( target ), kW, kH );

		releaseTarget( target );
		return ok;
	};

	std::vector< unsigned char > seconds, millis;
	const bool rendered = run( 1.0, &seconds ) && run( 0.001, &millis );
	glDeleteTextures( 1, &input );

	if( !rendered )
	{
		std::printf( "FAIL  the plugin would not render\n" );
		return false;
	}

	double mean = 0.0;
	int worst   = 0;
	compareBytes( seconds, millis, &mean, &worst );
	std::printf( "  seconds host vs milliseconds host: mean %.4f/255, worst %d/255\n", mean, worst );

	if( worst > 1 )
	{
		std::printf( "FAIL  the two hosts disagree -- the clock is not being normalised\n" );
		return false;
	}

	std::printf( "PASS  clock\n" );
	return true;
}

/// Every factory preset renders, differs from the defaults, and differs from
/// every other one.
bool checkPresets()
{
	constexpr int kW = 320;
	constexpr int kH = 180;

	const std::vector< unsigned char > scene = buildScene( kW, kH, scenePixel );
	const GLuint input                       = uploadScene( scene, kW, kH );

	std::vector< std::vector< unsigned char > > frames;
	std::vector< std::string > names;
	bool ok = true;

	for( int element = 0; element < presets::elementCount(); ++element )
	{
		Driver driver;
		//Deliberately through the host-facing route, so the preset machinery --
		//not just the table -- is what is being exercised.
		set( driver.plugin, Ferric::PT_PRESET, static_cast< float >( element ) );

		Target target = makeTarget( kW, kH );
		if( !driveFrames( driver, target, input, kW, kH, 3 ) )
		{
			std::printf( "FAIL  preset '%s' would not render\n", presets::label( element ) );
			releaseTarget( target );
			ok = false;
			break;
		}

		std::vector< unsigned char > frame = flipRows( readBytes( target ), kW, kH );
		releaseTarget( target );

		const double spread = lumaSpread( frame );
		if( spread < 5.0 )
		{
			std::printf( "FAIL  preset '%s' renders a nearly flat frame (luma spread %.2f)\n",
			             presets::label( element ), spread );
			ok = false;
		}

		//The preset really took, rather than the dropdown having snapped back to
		//Custom on the host's own echo. That is the failure this plugin's
		//override pattern exists to prevent, and it is invisible in the picture.
		if( element > 0 )
		{
			const int held = static_cast< int >( std::lround( driver.plugin.GetFloatParameter( Ferric::PT_PRESET ) ) );
			if( held != element )
			{
				std::printf( "FAIL  preset '%s' did not stick -- the dropdown reads %d\n",
				             presets::label( element ), held );
				ok = false;
			}
		}

		frames.push_back( std::move( frame ) );
		names.push_back( presets::label( element ) );
	}

	glDeleteTextures( 1, &input );

	for( size_t i = 0; i < frames.size() && ok; ++i )
	{
		for( size_t j = i + 1; j < frames.size(); ++j )
		{
			const double rms = rmsDifference( frames[ i ], frames[ j ] );
			if( rms < 1.0 )
			{
				std::printf( "FAIL  '%s' and '%s' render the same picture (rms %.3f)\n",
				             names[ i ].c_str(), names[ j ].c_str(), rms );
				ok = false;
			}
		}
	}

	if( ok )
		std::printf( "PASS  presets  (%d, all distinct)\n", static_cast< int >( frames.size() ) );

	return ok;
}

/**
    The host's echo does not un-set a preset.

    No GL. Drives `SetFloatParameter` the way Resolume actually does: pick a
    preset, then have the host go on restating the values it still believes in --
    which are the ones from BEFORE the preset, because Resolume does not consume
    value events. A naive implementation drops to Custom on the first of those
    and the operator sees the dropdown snap back with nothing changed.

    Three hosts are simulated, because the fleet has met all three: one that
    ignores the events entirely, one that honours them, and one that honours them
    but quantises the value on the way through. The last is the one that caught a
    tolerance of 1e-4 being a float epsilon rather than a quantisation allowance.
*/
bool checkPresetEcho()
{
	enum HostKind
	{
		kIgnores,
		kHonours,
		kQuantises
	};

	const char* kindNames[] = { "ignores events", "honours events", "honours + quantises" };
	bool ok                 = true;

	for( int kind = kIgnores; kind <= kQuantises; ++kind )
	{
		for( int element = 1; element < presets::elementCount(); ++element )
		{
			Ferric plugin;

			//The host's opening position: it pushes every default at the plugin
			//before anything else happens, which is what Resolume does on load.
			std::vector< float > hostBelieves( Ferric::PT_COUNT );
			for( unsigned int id = 0; id < Ferric::PT_COUNT; ++id )
			{
				hostBelieves[ id ] = plugin.GetFloatParameter( id );
				if( id < Ferric::PT_ABOUT_FIRST )
					plugin.SetFloatParameter( id, hostBelieves[ id ] );
			}

			plugin.SetFloatParameter( Ferric::PT_PRESET, static_cast< float >( element ) );

			//Now the host carries on, for several frames.
			for( int frame = 0; frame < 5; ++frame )
			{
				for( unsigned int id = 0; id < Ferric::PT_ABOUT_FIRST; ++id )
				{
					if( id == Ferric::PT_PRESET )
						continue;

					float value = hostBelieves[ id ];
					if( kind != kIgnores )
					{
						//An event-honouring host re-reads what the plugin now
						//holds and pushes THAT back.
						value              = plugin.GetFloatParameter( id );
						hostBelieves[ id ] = value;
					}
					if( kind == kQuantises )
						value = std::round( value * 1000.0f ) / 1000.0f;

					plugin.SetFloatParameter( id, value );
				}
			}

			const int held = static_cast< int >( std::lround( plugin.GetFloatParameter( Ferric::PT_PRESET ) ) );
			if( held != element )
			{
				std::printf( "FAIL  [%s] preset '%s' dropped to %d under the host's own echo\n",
				             kindNames[ kind ], presets::label( element ), held );
				ok = false;
			}
		}
	}

	//And the other direction: a real edit MUST drop to Custom, or the preset
	//would be a trap the operator cannot get out of.
	{
		Ferric plugin;
		for( unsigned int id = 0; id < Ferric::PT_ABOUT_FIRST; ++id )
			plugin.SetFloatParameter( id, plugin.GetFloatParameter( id ) );

		plugin.SetFloatParameter( Ferric::PT_PRESET, 1.0f );
		plugin.SetFloatParameter( Ferric::PT_WOW, 0.777f );

		if( static_cast< int >( std::lround( plugin.GetFloatParameter( Ferric::PT_PRESET ) ) ) != 0 )
		{
			std::printf( "FAIL  moving a covered control did not drop the preset to Custom\n" );
			ok = false;
		}

		if( std::fabs( plugin.GetFloatParameter( Ferric::PT_WOW ) - 0.777f ) > 1e-6f )
		{
			std::printf( "FAIL  the operator's edit was swallowed\n" );
			ok = false;
		}
	}

	if( ok )
		std::printf( "PASS  echo\n" );

	return ok;
}

bool bench()
{
	struct Size
	{
		const char* name;
		int width, height;
	};

	const Size sizes[] = { { "1080p", 1920, 1080 }, { "4K", 3840, 2160 } };

	for( const Size& size : sizes )
	{
		const std::vector< unsigned char > scene = buildScene( size.width, size.height, scenePixel );
		const GLuint input                       = uploadScene( scene, size.width, size.height );

		Driver driver;
		set( driver.plugin, Ferric::PT_NR_TYPE, static_cast< float >( compander::kTypeC ) );
		set( driver.plugin, Ferric::PT_DROPOUTS, 0.4f );

		Target target = makeTarget( size.width, size.height );

		//Warm up first: the first frame compiles, allocates and uploads, and
		//timing it would be timing the setup.
		if( !driveFrames( driver, target, input, size.width, size.height, 5 ) )
		{
			std::printf( "FAIL  the plugin would not render at %s\n", size.name );
			releaseTarget( target );
			glDeleteTextures( 1, &input );
			return false;
		}

		constexpr int kFrames = 60;
		const auto start      = std::chrono::steady_clock::now();
		for( int f = 0; f < kFrames; ++f )
		{
			driver.plugin.SetTime( 1.0 + static_cast< double >( f ) / 60.0 );
			driver.render( target, input, size.width, size.height );
		}
		glFinish();
		const auto end = std::chrono::steady_clock::now();

		const double ms = std::chrono::duration< double, std::milli >( end - start ).count()
		                  / static_cast< double >( kFrames );
		std::printf( "  %-6s Type C + dropouts  %.3f ms/frame  (%.0f fps)\n", size.name, ms, 1000.0 / ms );

		releaseTarget( target );
		glDeleteTextures( 1, &input );
	}

	return true;
}

void list( Ferric& plugin )
{
	for( unsigned int id = 0; id < Ferric::PT_COUNT; ++id )
	{
		const char* name = plugin.GetParamName( id );
		if( name == nullptr || name[ 0 ] == '\0' )
			continue;

		const unsigned int type = plugin.GetParamType( id );
		const char* typeName    = "standard";
		if( type == FF_TYPE_BOOLEAN )
			typeName = "boolean";
		else if( type == FF_TYPE_OPTION )
			typeName = "option";
		else if( type == FF_TYPE_TEXT )
			typeName = "text";
		else if( type == FF_TYPE_EVENT )
			typeName = "event";
		else if( type == FF_TYPE_BUFFER )
			typeName = "buffer";

		//⚠️ The length, printed, because FFGL truncates a name to sixteen
		//characters in Resolume and the SDK does not enforce it -- the plugin
		//hands over a pointer to the whole string and the host copies sixteen.
		//Every offline harness passes; only the host is ever wrong. Six plugins
		//in this fleet shipped a control called `Background Opaci`.
		const size_t length = std::strlen( name );
		std::printf( "  %2u  %-16s %-9s %6.3f  %s\n", id, name, typeName,
		             plugin.GetFloatParameter( id ),
		             length > 16 ? "<-- TRUNCATED BY THE HOST" : ( length == 16 ? "(exactly 16)" : "" ) );
	}
}

bool renderOne( const Options& options, const std::string& path, Rgba ( *pixel )( float, float ) )
{
	const std::vector< unsigned char > scene = buildScene( options.width, options.height, pixel );
	const GLuint input                       = uploadScene( scene, options.width, options.height );

	Driver driver;
	if( !applySets( driver.plugin, options ) )
	{
		glDeleteTextures( 1, &input );
		return false;
	}

	Target target = makeTarget( options.width, options.height );
	const bool ok = driveFrames( driver, target, input, options.width, options.height, options.frames );

	bool written = false;
	if( ok )
		written = writePng( path, options.width, options.height,
		                    flipRows( readBytes( target ), options.width, options.height ) );

	releaseTarget( target );
	glDeleteTextures( 1, &input );

	if( !ok )
		std::printf( "the plugin would not render\n" );
	else if( !written )
		std::printf( "could not write %s\n", path.c_str() );
	else
		std::printf( "wrote %s (%dx%d)\n", path.c_str(), options.width, options.height );

	return ok && written;
}

} // namespace

int main( int argc, char** argv )
{
	Options options;
	std::string outPath;
	std::string scenePath;
	bool wantList = false;
	std::vector< std::string > checks;

	for( int i = 1; i < argc; ++i )
	{
		const std::string arg = argv[ i ];
		const auto next       = [ & ]() -> std::string { return i + 1 < argc ? argv[ ++i ] : std::string(); };

		if( arg == "--out" )
			outPath = next();
		else if( arg == "--scene" )
			scenePath = next();
		else if( arg == "--list" )
			wantList = true;
		else if( arg == "--size" )
		{
			const std::string value = next();
			const size_t x          = value.find( 'x' );
			if( x != std::string::npos )
			{
				options.width  = std::max( 1, std::atoi( value.substr( 0, x ).c_str() ) );
				options.height = std::max( 1, std::atoi( value.substr( x + 1 ).c_str() ) );
			}
		}
		else if( arg == "--frames" )
			options.frames = std::max( 1, std::atoi( next().c_str() ) );
		else if( arg == "--set" )
		{
			const std::string value = next();
			const size_t eq         = value.find( '=' );
			if( eq == std::string::npos )
			{
				std::printf( "--set wants Name=value\n" );
				return 1;
			}
			options.sets.emplace_back( value.substr( 0, eq ),
			                           static_cast< float >( std::atof( value.substr( eq + 1 ).c_str() ) ) );
		}
		else if( arg.rfind( "--", 0 ) == 0 )
			checks.push_back( arg.substr( 2 ) );
		else
		{
			std::printf( "unrecognised argument '%s'\n", arg.c_str() );
			return 1;
		}
	}

	//---------------------------------------------------------------------
	// A context, but only if something is going to need one.
	//
	// Four of the checks are pure arithmetic -- the reaction, the weighting
	// curve, the weighted figure and the preset echo -- and they are exactly the
	// set a CI runner can be trusted with, because a hosted runner has no GPU
	// and a software GL stack is not a thing to hang a merge gate on. Demanding
	// a 4.1 core context before running them would make the one job CI *can* do
	// fail for a reason that has nothing to do with the code.
	//
	// Everything else renders, and those run locally before a tag.
	//---------------------------------------------------------------------
	static const std::vector< std::string > kNoGL = { "drive", "weighting", "wf", "echo" };

	bool needsGL = wantList || !outPath.empty() || !scenePath.empty() || checks.empty();
	for( const std::string& check : checks )
	{
		if( std::find( kNoGL.begin(), kNoGL.end(), check ) == kNoGL.end() )
			needsGL = true;
	}

	CGLContextObj context = nullptr;
	if( needsGL )
	{
		context = createContext();
		if( context == nullptr )
		{
			std::printf( "could not create an OpenGL 4.1 core context\n" );
			return 1;
		}
	}

	int failures = 0;

	if( wantList )
	{
		Ferric plugin;
		applySets( plugin, options );
		list( plugin );
	}

	if( !scenePath.empty() )
	{
		const std::vector< unsigned char > scene = buildScene( options.width, options.height, scenePixel );
		if( !writePng( scenePath, options.width, options.height, scene ) )
		{
			std::printf( "could not write %s\n", scenePath.c_str() );
			++failures;
		}
		else
			std::printf( "wrote %s (%dx%d)\n", scenePath.c_str(), options.width, options.height );
	}

	if( !outPath.empty() && !renderOne( options, outPath, scenePixel ) )
		++failures;

	for( const std::string& check : checks )
	{
		std::printf( "\n== %s ==\n", check.c_str() );

		bool ok = false;
		if( check == "tbe" )
			ok = checkTbe();
		else if( check == "weighting" )
			ok = checkWeighting();
		else if( check == "wf" )
			ok = checkWf();
		else if( check == "identity" )
			ok = checkIdentity();
		else if( check == "roundtrip" )
			ok = checkRoundTrip();
		else if( check == "nr" )
			ok = checkNr();
		else if( check == "scan" )
			ok = checkScan();
		else if( check == "denoise" )
			ok = checkDenoise();
		else if( check == "drive" )
			ok = checkDrive();
		else if( check == "clock" )
			ok = checkClock();
		else if( check == "presets" )
			ok = checkPresets();
		else if( check == "echo" )
			ok = checkPresetEcho();
		else if( check == "bench" )
			ok = bench();
		else
		{
			std::printf( "no check called '%s'\n", check.c_str() );
			ok = false;
		}

		if( !ok )
			++failures;
	}

	if( context != nullptr )
	{
		CGLSetCurrentContext( nullptr );
		CGLDestroyContext( context );
	}

	if( failures > 0 )
		std::printf( "\n%d check%s failed\n", failures, failures == 1 ? "" : "s" );

	return failures == 0 ? 0 : 1;
}
