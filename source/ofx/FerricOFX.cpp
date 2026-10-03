/// The OpenFX build of Ferric, for DaVinci Resolve, Nuke, Natron, Vegas and
/// other OFX hosts.
///
/// ------------------------------------------------------- what is shared
///
/// **Everything that is not per-pixel.** `Transport.cpp`, `Compander.cpp`,
/// `Controls.cpp` and `Drive.cpp` link straight in -- the error signal, the
/// phases, the gain law, the stage table, every slider curve and the preset
/// table are the same code the FFGL build runs. The per-pixel stage is
/// `CpuPasses.cpp`, the CPU mirror of the three GLSL passes, which `frtest
/// --cpu` holds against the GPU frame for frame. This file is marshalling and
/// threading: OFX's pixel formats in, premultiplied float through the three
/// passes, and back out.
///
/// ------------------------------------------------------ what is missing
///
/// **The Reaction group, entirely** -- the audio input, Sync, Beat Depth, Beat
/// Decay, Division, Level Depth, Band Depth and Route. OFX has no spectrum to
/// offer and no tempo or bar phase, and a timeline renders frames in any order,
/// so an envelope follower or a beat grid would be a nonsense here. The group
/// is not declared rather than declared and dead, and `cpu::frameAt` runs the
/// drive on a zeroed input, which is the manual transport.
///
/// The consequence for presets: **Beat Slip and Breathing are not in this
/// menu**, because their character IS the reaction, and offered here they
/// would be a different, quieter picture under the same name. The other seven
/// set no reactive depth and mean exactly what they mean in Resolume. The menu
/// is filtered from the table rather than listed by hand, so a reactive preset
/// added later stays out by itself.
///
/// ------------------------------------------------------------- the clock
///
/// OFX time is in FRAMES; seconds are `time / frame rate`. The transport's
/// phases were already a function of absolute time in both builds. The hiss
/// and dropout phases are the one departure: the FFGL build integrates them
/// over clamped frame deltas so a host stall does not jump the grain, and here
/// they are `seconds * rate` -- a host that renders frame 500 before frame 4
/// has no previous frame to integrate from, and a frame that renders the same
/// alone as in sequence matters more. See CpuPasses.h.
///
/// ------------------------------------------------------- tiles and threads
///
/// No tiles: the tape pass is a warp and reads anywhere in the encoded frame,
/// and both compander passes read eleven pixels along the row. So each pass
/// runs over the whole frame, split into bands of rows across the host's
/// threads, with a barrier between passes -- the CPU's version of the FFGL
/// build's three draws into three buffers.
///
/// ------------------------------------------------------- render scale
///
/// Not compensated for. The compander's eleven taps, head wear, the hiss
/// grain and the scanline count are in render pixels, exactly as they are in
/// Resolume at whatever size the composition runs -- so a host's proxy preview
/// at half scale looks coarser than the full render. Compensating properly
/// would mean fractional-pixel taps and a scanline count taken from the
/// full-size height: a different per-pixel stage from the GLSL this file is
/// held to, for a difference that only ever shows in a preview.

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "ofxsImageEffect.h"
#include "ofxsMultiThread.h"

// After the OFX Support headers, which is where the OFX types come from.
#include "StoatworksAboutOFX.h"

#include "../Compander.h"
#include "../Controls.h"
#include "../CpuPasses.h"
#include "../Drive.h"
#include "../Presets.h"
#include "../Transport.h"

namespace
{
constexpr const char* kPluginIdentifier = "com.stoatworks.ferric";
constexpr const char* kPluginName       = "Ferric";
constexpr const char* kPluginGrouping   = "Stoatworks";
constexpr const char* kPluginDescription =
	"Put the picture on tape.\n\n"
	"Wow and flutter is not a wobble applied to a picture. It is a timing error "
	"applied to a signal, and a picture read off tape is a signal with a clock - "
	"so every pixel has a tape time, and its displacement is one error signal "
	"evaluated there.\n\n"
	"Everything falls out of that. Wow is slower than one picture, so the whole "
	"frame leans and breathes together. Flutter fits a cycle or two into a "
	"picture, so it draws a travelling wave down the image.\n\n"
	"The oxide adds its own hiss and drops out here and there, and the "
	"sliding-band noise reduction that hid one under the other is here too.\n\n"
	"The Resolume build of this effect reacts to audio and to the host's beat. "
	"OpenFX has neither to offer a plugin, so that Reaction group is absent here "
	"rather than present and doing nothing, and the two presets built on it -- "
	"Beat Slip and Breathing -- are not in this menu.\n\n"
	"Every frame is a function of its own time, so any frame renders on its own "
	"and a scrub lands where it should.\n\n"
	"https://stoatworks-labs.com";

// Parameter names are identity: a saved project refers to them. Do not rename.
constexpr const char* kParamPreset      = "preset";
constexpr const char* kParamMachine     = "machine";
constexpr const char* kParamWow         = "wow";
constexpr const char* kParamWowRate     = "wowRate";
constexpr const char* kParamFlutter     = "flutter";
constexpr const char* kParamFlutterRate = "flutterRate";
constexpr const char* kParamScrape      = "scrape";
constexpr const char* kParamDrift       = "drift";
constexpr const char* kParamTapeSpeed   = "tapeSpeed";
constexpr const char* kParamAmount      = "amount";
constexpr const char* kParamVertical    = "vertical";
constexpr const char* kParamHiss        = "hiss";
constexpr const char* kParamDropouts    = "dropouts";
constexpr const char* kParamHeadWear    = "headWear";
constexpr const char* kParamNrType      = "nrType";
constexpr const char* kParamNrMode      = "nrMode";
constexpr const char* kParamMistracking = "mistracking";
constexpr const char* kParamNrStrength  = "nrStrength";
constexpr const char* kParamShowTrace   = "showTrace";
constexpr const char* kParamMix         = "mix";

using namespace ferric;

/// The preset table is host-agnostic; this is the OFX binding of it, in
/// presets::Param order -- the same job as the FFGL build's kPresetParamIDs.
/// nullptr is a column with no parameter here: the Reaction group's.
const char* const kPresetParamNames[ presets::kParamCount ] = {
	kParamMachine, kParamWow, kParamWowRate, kParamFlutter, kParamFlutterRate, kParamScrape,
	kParamDrift, kParamTapeSpeed, kParamAmount, kParamVertical, kParamHiss, kParamDropouts,
	kParamHeadWear, kParamNrType, kParamNrMode, kParamMistracking, kParamNrStrength,
	nullptr,// Sync
	nullptr,// Beat Depth
	nullptr,// Level Depth
	nullptr,// Band Depth
};

static_assert( sizeof( kPresetParamNames ) / sizeof( kPresetParamNames[ 0 ] ) == presets::kParamCount,
               "the OFX preset binding and the preset table disagree" );

bool isChoice( const char* name )
{
	return name == kParamMachine || name == kParamNrType || name == kParamNrMode;
}

/// Whether a preset means here what it means in Resolume: it sets no reactive
/// depth. With every depth at zero the drive is neutral and Sync does nothing,
/// so the picture is the same in both builds.
bool presetIsManual( const presets::Preset& p )
{
	return p.values[ presets::P_BEAT_DEPTH ] == 0.0f && p.values[ presets::P_LEVEL_DEPTH ] == 0.0f
	       && p.values[ presets::P_BAND_DEPTH ] == 0.0f;
}

/// The table index behind a menu element, or -1 for Custom (element 0) and
/// anything out of range. The menu is Custom followed by the manual presets,
/// in table order.
int tableIndexForMenu( int element )
{
	if( element <= 0 )
		return -1;

	int seen = 0;
	for( int i = 0; i < presets::kCount; ++i )
	{
		if( !presetIsManual( presets::kPresets[ i ] ) )
			continue;
		if( ++seen == element )
			return i;
	}
	return -1;
}

//---------------------------------------------------------------------------
// Threads.
//
// One band of rows per thread through the host's multi-thread suite, and
// `run()` returns only when every band has finished -- which is the barrier
// the passes need, because the tape pass reads rows the encode pass wrote on
// another thread.
//---------------------------------------------------------------------------
class RowBands : public OFX::MultiThread::Processor
{
public:
	RowBands( OFX::ImageEffect& owner, int rowCount, std::function< void( int, int ) > work ) :
		effect( owner ),
		rows( rowCount ),
		body( std::move( work ) )
	{
	}

	void multiThreadFunction( unsigned int threadID, unsigned int nThreads ) override
	{
		const int threads = static_cast< int >( std::max( 1u, nThreads ) );
		const int per     = ( rows + threads - 1 ) / threads;
		const int y0      = static_cast< int >( threadID ) * per;
		const int y1      = std::min( rows, y0 + per );

		if( y0 < y1 && !effect.abort() )
			body( y0, y1 );
	}

	void run()
	{
		const unsigned int cpus = std::max( 1u, OFX::MultiThread::getNumCPUs() );
		multiThread( std::min( cpus, static_cast< unsigned int >( std::max( 1, rows ) ) ) );
	}

private:
	OFX::ImageEffect& effect;
	int rows;
	std::function< void( int, int ) > body;
};

//---------------------------------------------------------------------------
// Marshalling.
//
// Premultiplied float RGBA with row 0 at the bottom -- the order OFX uses and
// the order CpuPasses works in, so nothing here flips. The passes work
// premultiplied because that is what an FFGL host hands the GPU, and the same
// arithmetic on the same values is what makes the two builds agree.
//---------------------------------------------------------------------------
template< typename Pixel, int Components, int Maximum >
void gatherRows( const OFX::Image* src, const OfxRectI& bounds, bool premultiplied, float* out, int y0, int y1 )
{
	const int width   = bounds.x2 - bounds.x1;
	const float scale = 1.0f / static_cast< float >( Maximum );

	for( int y = y0; y < y1; ++y )
	{
		float* row = out + static_cast< size_t >( y ) * static_cast< size_t >( width ) * 4;

		for( int x = 0; x < width; ++x )
		{
			const Pixel* px = static_cast< const Pixel* >( src->getPixelAddress( bounds.x1 + x, bounds.y1 + y ) );
			float* dst      = row + static_cast< size_t >( x ) * 4;

			if( px == nullptr )
			{
				dst[ 0 ] = dst[ 1 ] = dst[ 2 ] = dst[ 3 ] = 0.0f;
				continue;
			}

			const float a = Components == 4 ? static_cast< float >( px[ 3 ] ) * scale : 1.0f;

			for( int c = 0; c < 3; ++c )
			{
				const float v = static_cast< float >( px[ c ] ) * scale;
				dst[ c ]      = premultiplied ? v : v * a;
			}
			dst[ 3 ] = a;
		}
	}
}

template< typename Pixel, int Components, int Maximum >
void scatterRows( const float* in, OFX::Image* dst, const OfxRectI& bounds, const OfxRectI& window,
                  bool premultiplied, int y0, int y1 )
{
	const int width   = bounds.x2 - bounds.x1;
	const float scale = static_cast< float >( Maximum );

	// Rows y0..y1 of the frame, clipped to the render window.
	const int rowFrom = std::max( window.y1, bounds.y1 + y0 );
	const int rowTo   = std::min( window.y2, bounds.y1 + y1 );
	const int colFrom = std::max( window.x1, bounds.x1 );
	const int colTo   = std::min( window.x2, bounds.x2 );

	for( int y = rowFrom; y < rowTo; ++y )
	{
		for( int x = colFrom; x < colTo; ++x )
		{
			Pixel* px = static_cast< Pixel* >( dst->getPixelAddress( x, y ) );
			if( px == nullptr )
				continue;

			const float* source = in
			                      + ( static_cast< size_t >( y - bounds.y1 ) * static_cast< size_t >( width )
			                          + static_cast< size_t >( x - bounds.x1 ) )
			                            * 4;
			const float a = source[ 3 ];

			for( int c = 0; c < 3; ++c )
			{
				float v = source[ c ];
				if( !premultiplied )
					v = a > 0.0f ? v / a : 0.0f;

				// Integer formats clamp, as the FFGL build's 8-bit host buffer
				// does. Float is left alone: a float pipeline may legitimately
				// carry values outside 0..1, and the encode side pushes bright
				// edges past 1 on purpose.
				if constexpr( Maximum == 1 )
					px[ c ] = static_cast< Pixel >( v );
				else
					px[ c ] = static_cast< Pixel >( std::lround( std::clamp( v, 0.0f, 1.0f ) * scale ) );
			}

			if constexpr( Components == 4 )
			{
				if constexpr( Maximum == 1 )
					px[ 3 ] = static_cast< Pixel >( a );
				else
					px[ 3 ] = static_cast< Pixel >( std::lround( std::clamp( a, 0.0f, 1.0f ) * scale ) );
			}
		}
	}
}

class FerricPlugin : public OFX::ImageEffect
{
public:
	explicit FerricPlugin( OfxImageEffectHandle handle ) :
		OFX::ImageEffect( handle )
	{
		dstClip = fetchClip( kOfxImageEffectOutputClipName );
		srcClip = fetchClip( kOfxImageEffectSimpleSourceClipName );

		preset      = fetchChoiceParam( kParamPreset );
		machine     = fetchChoiceParam( kParamMachine );
		wow         = fetchDoubleParam( kParamWow );
		wowRate     = fetchDoubleParam( kParamWowRate );
		flutter     = fetchDoubleParam( kParamFlutter );
		flutterRate = fetchDoubleParam( kParamFlutterRate );
		scrape      = fetchDoubleParam( kParamScrape );
		drift       = fetchDoubleParam( kParamDrift );
		tapeSpeed   = fetchDoubleParam( kParamTapeSpeed );
		amount      = fetchDoubleParam( kParamAmount );
		vertical    = fetchDoubleParam( kParamVertical );
		hiss        = fetchDoubleParam( kParamHiss );
		dropouts    = fetchDoubleParam( kParamDropouts );
		headWear    = fetchDoubleParam( kParamHeadWear );
		nrType      = fetchChoiceParam( kParamNrType );
		nrMode      = fetchChoiceParam( kParamNrMode );
		mistracking = fetchDoubleParam( kParamMistracking );
		nrStrength  = fetchDoubleParam( kParamNrStrength );
		showTrace   = fetchBooleanParam( kParamShowTrace );
		mix         = fetchDoubleParam( kParamMix );
	}

	/// The whole source, always. The warp reads anywhere in the frame and the
	/// compander eleven pixels along the row, so a tile of the input is not
	/// enough to render a tile of the output.
	void getRegionsOfInterest( const OFX::RegionsOfInterestArguments& args,
	                           OFX::RegionOfInterestSetter& rois ) override
	{
		rois.setRegionOfInterest( *srcClip, srcClip->getRegionOfDefinition( args.time ) );
	}

	/// Mix at zero is the input exactly, unless the trace is on: the overlay is
	/// drawn after the mix and is not subject to it.
	bool isIdentity( const OFX::IsIdentityArguments& args, OFX::Clip*& identityClip,
	                 double& identityTime ) override
	{
		if( mix->getValueAtTime( args.time ) <= 0.0 && !showTrace->getValueAtTime( args.time ) )
		{
			identityClip = srcClip;
			identityTime = args.time;
			return true;
		}
		return false;
	}

	void render( const OFX::RenderArguments& args ) override
	{
		std::unique_ptr< OFX::Image > dst( dstClip->fetchImage( args.time ) );
		std::unique_ptr< OFX::Image > src( srcClip->fetchImage( args.time ) );

		if( dst == nullptr || src == nullptr )
			OFX::throwSuiteStatusException( kOfxStatFailed );

		const OFX::BitDepthEnum depth       = dst->getPixelDepth();
		const OFX::PixelComponentEnum comps = dst->getPixelComponents();

		if( comps != OFX::ePixelComponentRGBA && comps != OFX::ePixelComponentRGB )
			OFX::throwSuiteStatusException( kOfxStatErrUnsupported );
		if( src->getPixelDepth() != depth || src->getPixelComponents() != comps )
			OFX::throwSuiteStatusException( kOfxStatErrImageFormat );

		const OfxRectI bounds = src->getBounds();
		const int width       = bounds.x2 - bounds.x1;
		const int height      = bounds.y2 - bounds.y1;
		if( width <= 0 || height <= 0 )
			return;

		// An RGB clip has no alpha to be premultiplied by, and a host that says
		// "unpremultiplied" about one is describing something that does not
		// exist. Treating it as premultiplied keeps the round trip an identity.
		const bool premultiplied = comps != OFX::ePixelComponentRGBA
		                           || srcClip->getPreMultiplication() != OFX::eImageUnPreMultiplied;

		//OFX time is FRAMES. Seconds come from the clip's frame rate, and a
		//host that reports zero would otherwise divide by it.
		double fps = dstClip->getFrameRate();
		if( !( fps > 0.0 ) )
			fps = srcClip->getFrameRate();
		if( !( fps > 0.0 ) )
			fps = 25.0;

		const cpu::Frame frame = cpu::frameAt( hostValuesAt( args.time ), width, height, args.time / fps );

		const size_t samples = static_cast< size_t >( width ) * static_cast< size_t >( height ) * 4;
		std::vector< float > input( samples );
		std::vector< float > work( samples );
		std::vector< float > taped( samples );

		const auto pass = [ & ]( std::function< void( int, int ) > rows ) {
			RowBands bands( *this, height, std::move( rows ) );
			bands.run();
		};

		pass( [ & ]( int y0, int y1 ) { gather( depth, comps, src.get(), bounds, premultiplied, input.data(), y0, y1 ); } );

		// Encode, the medium, decode -- in that order and each over the whole
		// frame, because that order is the signal chain. See Shaders.h.
		pass( [ & ]( int y0, int y1 ) { cpu::encodeRows( frame, input.data(), work.data(), y0, y1 ); } );
		if( abort() )
			return;
		pass( [ & ]( int y0, int y1 ) { cpu::tapeRows( frame, work.data(), taped.data(), y0, y1 ); } );
		if( abort() )
			return;
		pass( [ & ]( int y0, int y1 ) { cpu::decodeRows( frame, taped.data(), input.data(), work.data(), y0, y1 ); } );
		if( abort() )
			return;

		pass( [ & ]( int y0, int y1 ) {
			scatter( depth, comps, work.data(), dst.get(), bounds, args.renderWindow, premultiplied, y0, y1 );
		} );
	}

	void changedParam( const OFX::InstanceChangedArgs& args, const std::string& paramName ) override
	{
		// The About links open a browser and change nothing about the render.
		if( stoatworks::about::ofx::changedParam( args, paramName ) )
			return;

		if( paramName == kParamPreset )
		{
			if( applyingPreset )
				return;

			int chosen = 0;
			preset->getValue( chosen );
			const int table = tableIndexForMenu( chosen );
			if( table < 0 )
				return;//Custom: the sliders keep whatever they said

			// The copy IS the preset -- same table as the FFGL build, same 0..1
			// space. One edit block so undo takes the whole preset back at once.
			const presets::Preset& p = presets::kPresets[ table ];
			applyingPreset           = true;
			beginEditBlock( "Preset" );
			for( int i = 0; i < presets::kParamCount; ++i )
			{
				if( kPresetParamNames[ i ] != nullptr && differs( kPresetParamNames[ i ], p.values[ i ] ) )
					set( kPresetParamNames[ i ], p.values[ i ] );
			}
			endEditBlock();
			applyingPreset = false;
			return;
		}

		// Editing a covered control while a preset is active hands control back
		// to the sliders. Judged by VALUE, never by the change reason: hosts are
		// not consistent about reasons, but "still equal to the preset" is
		// unambiguous and also absorbs a host echoing our own writes. The
		// tolerance is the same quantisation allowance the FFGL build uses.
		if( applyingPreset || args.reason == OFX::eChangeTime )
			return;

		int active = 0;
		preset->getValue( active );
		const int table = tableIndexForMenu( active );
		if( table < 0 )
			return;

		const presets::Preset& p = presets::kPresets[ table ];
		for( int i = 0; i < presets::kParamCount; ++i )
		{
			if( kPresetParamNames[ i ] == nullptr || paramName != kPresetParamNames[ i ] )
				continue;

			if( differs( kPresetParamNames[ i ], p.values[ i ] ) )
			{
				applyingPreset = true;
				preset->setValue( 0 );
				applyingPreset = false;
			}
			return;
		}
	}

private:
	/// The controls at `time`, as the shared control struct. The Reaction
	/// fields are left at their defaults; `cpu::frameAt` forces them there
	/// anyway.
	controls::HostValues hostValuesAt( double time ) const
	{
		controls::HostValues v;

		//ChoiceParam answers through an out parameter rather than a return
		//value, unlike every other param type in the Support library.
		const auto choice = [ time ]( OFX::ChoiceParam* param ) {
			int value = 0;
			param->getValueAtTime( time, value );
			return static_cast< float >( value );
		};
		const auto slider = [ time ]( OFX::DoubleParam* param ) {
			return static_cast< float >( param->getValueAtTime( time ) );
		};

		v.machine     = choice( machine );
		v.wow         = slider( wow );
		v.wowRate     = slider( wowRate );
		v.flutter     = slider( flutter );
		v.flutterRate = slider( flutterRate );
		v.scrape      = slider( scrape );
		v.drift       = slider( drift );
		v.tapeSpeed   = slider( tapeSpeed );
		v.amount      = slider( amount );
		v.vertical    = slider( vertical );

		v.hiss     = slider( hiss );
		v.dropouts = slider( dropouts );
		v.headWear = slider( headWear );

		v.nrType      = choice( nrType );
		v.nrMode      = choice( nrMode );
		v.mistracking = slider( mistracking );
		v.nrStrength  = slider( nrStrength );

		v.showTrace = showTrace->getValueAtTime( time ) ? 1.0f : 0.0f;
		v.mix       = slider( mix );

		return v;
	}

	/// Whether a covered control differs from a preset value. Option values
	/// in the table are element indices.
	bool differs( const char* name, float value )
	{
		if( isChoice( name ) )
		{
			int current = 0;
			fetchChoiceParam( name )->getValue( current );
			return current != static_cast< int >( std::lround( value ) );
		}

		double current = 0.0;
		fetchDoubleParam( name )->getValue( current );
		return std::fabs( current - static_cast< double >( value ) ) > presets::kEchoTolerance;
	}

	void set( const char* name, float value )
	{
		if( isChoice( name ) )
			fetchChoiceParam( name )->setValue( static_cast< int >( std::lround( value ) ) );
		else
			fetchDoubleParam( name )->setValue( static_cast< double >( value ) );
	}

	static void gather( OFX::BitDepthEnum depth, OFX::PixelComponentEnum comps, const OFX::Image* src,
	                    const OfxRectI& bounds, bool premultiplied, float* out, int y0, int y1 )
	{
		const bool rgba = comps == OFX::ePixelComponentRGBA;
		switch( depth )
		{
			case OFX::eBitDepthUByte:
				rgba ? gatherRows< unsigned char, 4, 255 >( src, bounds, premultiplied, out, y0, y1 )
				     : gatherRows< unsigned char, 3, 255 >( src, bounds, premultiplied, out, y0, y1 );
				break;
			case OFX::eBitDepthUShort:
				rgba ? gatherRows< unsigned short, 4, 65535 >( src, bounds, premultiplied, out, y0, y1 )
				     : gatherRows< unsigned short, 3, 65535 >( src, bounds, premultiplied, out, y0, y1 );
				break;
			case OFX::eBitDepthFloat:
				rgba ? gatherRows< float, 4, 1 >( src, bounds, premultiplied, out, y0, y1 )
				     : gatherRows< float, 3, 1 >( src, bounds, premultiplied, out, y0, y1 );
				break;
			default:
				break;
		}
	}

	static void scatter( OFX::BitDepthEnum depth, OFX::PixelComponentEnum comps, const float* in, OFX::Image* dst,
	                     const OfxRectI& bounds, const OfxRectI& window, bool premultiplied, int y0, int y1 )
	{
		const bool rgba = comps == OFX::ePixelComponentRGBA;
		switch( depth )
		{
			case OFX::eBitDepthUByte:
				rgba ? scatterRows< unsigned char, 4, 255 >( in, dst, bounds, window, premultiplied, y0, y1 )
				     : scatterRows< unsigned char, 3, 255 >( in, dst, bounds, window, premultiplied, y0, y1 );
				break;
			case OFX::eBitDepthUShort:
				rgba ? scatterRows< unsigned short, 4, 65535 >( in, dst, bounds, window, premultiplied, y0, y1 )
				     : scatterRows< unsigned short, 3, 65535 >( in, dst, bounds, window, premultiplied, y0, y1 );
				break;
			case OFX::eBitDepthFloat:
				rgba ? scatterRows< float, 4, 1 >( in, dst, bounds, window, premultiplied, y0, y1 )
				     : scatterRows< float, 3, 1 >( in, dst, bounds, window, premultiplied, y0, y1 );
				break;
			default:
				break;
		}
	}

	OFX::Clip* dstClip = nullptr;
	OFX::Clip* srcClip = nullptr;

	OFX::ChoiceParam* preset      = nullptr;
	OFX::ChoiceParam* machine     = nullptr;
	OFX::DoubleParam* wow         = nullptr;
	OFX::DoubleParam* wowRate     = nullptr;
	OFX::DoubleParam* flutter     = nullptr;
	OFX::DoubleParam* flutterRate = nullptr;
	OFX::DoubleParam* scrape      = nullptr;
	OFX::DoubleParam* drift       = nullptr;
	OFX::DoubleParam* tapeSpeed   = nullptr;
	OFX::DoubleParam* amount      = nullptr;
	OFX::DoubleParam* vertical    = nullptr;
	OFX::DoubleParam* hiss        = nullptr;
	OFX::DoubleParam* dropouts    = nullptr;
	OFX::DoubleParam* headWear    = nullptr;
	OFX::ChoiceParam* nrType      = nullptr;
	OFX::ChoiceParam* nrMode      = nullptr;
	OFX::DoubleParam* mistracking = nullptr;
	OFX::DoubleParam* nrStrength  = nullptr;
	OFX::BooleanParam* showTrace  = nullptr;
	OFX::DoubleParam* mix         = nullptr;

	/// True while our own setValues are in flight, so the resulting
	/// changedParam callbacks are not mistaken for the operator editing.
	bool applyingPreset = false;
};

OFX::DoubleParamDescriptor* defineSlider( OFX::ImageEffectDescriptor& desc, OFX::PageParamDescriptor* page,
                                          OFX::GroupParamDescriptor* group, const char* name, const char* label,
                                          const char* hint, double value )
{
	OFX::DoubleParamDescriptor* param = desc.defineDoubleParam( name );
	param->setLabels( label, label, label );
	param->setHint( hint );
	param->setRange( 0.0, 1.0 );
	param->setDisplayRange( 0.0, 1.0 );
	param->setDoubleType( OFX::eDoubleTypePlain );
	param->setDefault( value );
	param->setParent( *group );
	page->addChild( *param );
	return param;
}

void defineChoice( OFX::ImageEffectDescriptor& desc, OFX::PageParamDescriptor* page, OFX::GroupParamDescriptor* group,
                   const char* name, const char* label, const char* hint, int count,
                   const char* ( *labelFor )( int ), int value )
{
	OFX::ChoiceParamDescriptor* param = desc.defineChoiceParam( name );
	param->setLabels( label, label, label );
	param->setHint( hint );
	for( int i = 0; i < count; ++i )
		param->appendOption( labelFor( i ) );
	param->setDefault( value );
	param->setAnimates( false );
	param->setParent( *group );
	page->addChild( *param );
}

OFX::GroupParamDescriptor* defineGroup( OFX::ImageEffectDescriptor& desc, OFX::PageParamDescriptor* page,
                                        const char* name )
{
	OFX::GroupParamDescriptor* group = desc.defineGroupParam( name );
	group->setLabels( name, name, name );
	page->addChild( *group );
	return group;
}

mDeclarePluginFactory( FerricPluginFactory, {}, {} );
} // namespace

void FerricPluginFactory::describe( OFX::ImageEffectDescriptor& desc )
{
	desc.setLabels( kPluginName, kPluginName, kPluginName );
	desc.setPluginGrouping( kPluginGrouping );
	desc.setPluginDescription( kPluginDescription );

	desc.addSupportedContext( OFX::eContextFilter );
	desc.addSupportedContext( OFX::eContextGeneral );

	desc.addSupportedBitDepth( OFX::eBitDepthUByte );
	desc.addSupportedBitDepth( OFX::eBitDepthUShort );
	desc.addSupportedBitDepth( OFX::eBitDepthFloat );

	desc.setSingleInstance( false );
	desc.setHostFrameThreading( false );
	desc.setSupportsMultiResolution( true );

	// ⚠️ Tiles OFF, and not for speed. The tape pass is a warp that reads
	// anywhere in the encoded frame -- and off the end of a line it reads the
	// line next door -- so a tile of output needs the whole of its input.
	desc.setSupportsTiles( false );

	// No temporal access: every frame is a function of the source at the same
	// time, the parameters at that time, and the time itself. Nothing is
	// remembered from one render to the next.
	desc.setTemporalClipAccess( false );
	desc.setRenderTwiceAlways( false );
	desc.setSupportsMultipleClipPARs( false );
	desc.setRenderThreadSafety( OFX::eRenderFullySafe );
}

void FerricPluginFactory::describeInContext( OFX::ImageEffectDescriptor& desc, OFX::ContextEnum )
{
	OFX::ClipDescriptor* srcClip = desc.defineClip( kOfxImageEffectSimpleSourceClipName );
	srcClip->addSupportedComponent( OFX::ePixelComponentRGBA );
	srcClip->addSupportedComponent( OFX::ePixelComponentRGB );
	srcClip->setTemporalClipAccess( false );
	srcClip->setSupportsTiles( false );
	srcClip->setIsMask( false );

	OFX::ClipDescriptor* dstClip = desc.defineClip( kOfxImageEffectOutputClipName );
	dstClip->addSupportedComponent( OFX::ePixelComponentRGBA );
	dstClip->addSupportedComponent( OFX::ePixelComponentRGB );
	dstClip->setSupportsTiles( false );

	// Same parameters, same 0..1 ranges, same defaults and groups as the FFGL
	// build, so the two inspectors read identically and one guide covers both.
	OFX::PageParamDescriptor* page = desc.definePageParam( "Controls" );
	const controls::HostValues defaults;

	OFX::ChoiceParamDescriptor* presetParam = desc.defineChoiceParam( kParamPreset );
	presetParam->setLabels( "Preset", "Preset", "Preset" );
	presetParam->setHint( "Start here. Picking one sets the controls below; editing any of them "
	                      "afterwards falls back to Custom. Beat Slip and Breathing are Resolume-only: "
	                      "they are built on the audio reaction, which OpenFX has no way to feed." );
	presetParam->appendOption( "Custom" );
	for( int i = 0; i < presets::kCount; ++i )
	{
		if( presetIsManual( presets::kPresets[ i ] ) )
			presetParam->appendOption( presets::kPresets[ i ].name );
	}
	presetParam->setDefault( 0 );
	presetParam->setIsPersistant( true );
	presetParam->setEvaluateOnChange( false );//the copied values re-render; the label does not
	presetParam->setAnimates( false );
	page->addChild( *presetParam );

	//--------------------------------------------------------------- Transport
	OFX::GroupParamDescriptor* transportGroup = defineGroup( desc, page, "Transport" );

	defineChoice( desc, page, transportGroup, kParamMachine, "Machine",
	              "Sets the character of the error rather than replacing your controls. Cassette: "
	              "wow dominates, scrape from the felt pad. Reel to Reel: everything an order "
	              "better. Video Head: a once-per-revolution drum error, far more periodic. "
	              "Failing: a slipping belt, deep wow with a hard lurch on it.",
	              transport::machineCount(), transport::machineLabel, static_cast< int >( defaults.machine ) );

	defineSlider( desc, page, transportGroup, kParamWow, "Wow",
	              "The slow component. Slower than one picture, so the whole frame leans and "
	              "breathes together.",
	              defaults.wow );
	defineSlider( desc, page, transportGroup, kParamWowRate, "Wow Rate",
	              "Wow's rate, 0.2 to 7 Hz, exponential so half the slider is the geometric middle.",
	              defaults.wowRate );
	defineSlider( desc, page, transportGroup, kParamFlutter, "Flutter",
	              "Fits a cycle or two into a picture, so it draws a travelling wave down the image "
	              "that scrolls as the clock moves. The one that makes it look like tape.",
	              defaults.flutter );
	defineSlider( desc, page, transportGroup, kParamFlutterRate, "Flutter Rate",
	              "Flutter's rate, 2 to 60 Hz, exponential.", defaults.flutterRate );
	defineSlider( desc, page, transportGroup, kParamScrape, "Scrape",
	              "Scrape flutter at about 220 Hz: many cycles per picture, so fine horizontal banding.",
	              defaults.scrape );
	defineSlider( desc, page, transportGroup, kParamDrift, "Drift",
	              "A slow random walk over seconds, below where a wow-and-flutter meter looks.",
	              defaults.drift );
	defineSlider( desc, page, transportGroup, kParamTapeSpeed, "Tape Speed",
	              "The control that matters: how much tape one picture occupies, from about 2 s "
	              "(ribbons) at the bottom to 2 ms (a rigid frame, only wow survives) at the top. A "
	              "real picture scans so fast that flutter would be invisible, so this is the "
	              "fiction, exposed. Turn on Show Trace while setting it.",
	              defaults.tapeSpeed );
	defineSlider( desc, page, transportGroup, kParamAmount, "Amount",
	              "Peak displacement at full error, up to a fifth of the picture width.", defaults.amount );
	defineSlider( desc, page, transportGroup, kParamVertical, "Vertical",
	              "How much of the SLOW error also rolls the frame. A sync separator never sees "
	              "flutter, so only wow and drift move it vertically.",
	              defaults.vertical );

	//-------------------------------------------------------------------- Tape
	OFX::GroupParamDescriptor* tapeGroup = defineGroup( desc, page, "Tape" );

	defineSlider( desc, page, tapeGroup, kParamHiss, "Hiss",
	              "The oxide's noise, along the scan, so it comes out as horizontal grain. Not zero by "
	              "default: noise reduction with no noise to reduce has nothing to show.",
	              defaults.hiss );
	defineSlider( desc, page, tapeGroup, kParamDropouts, "Dropouts",
	              "Runs of a scanline losing contact with the head, tapered at both ends.",
	              defaults.dropouts );
	defineSlider( desc, page, tapeGroup, kParamHeadWear, "Head Wear",
	              "High frequencies lost along the scan, and the hiss widened with them.",
	              defaults.headWear );

	//--------------------------------------------------------- Noise Reduction
	OFX::GroupParamDescriptor* nrGroup = defineGroup( desc, page, "Noise Reduction" );

	defineChoice( desc, page, nrGroup, kParamNrType, "NR Type",
	              "The sliding-band curve. Type B: one band, forgiving. Type C: two bands, an order "
	              "quieter and far less forgiving of mistracking.",
	              compander::typeCount(), compander::typeLabel, static_cast< int >( defaults.nrType ) );
	defineChoice( desc, page, nrGroup, kParamNrMode, "NR Mode",
	              "Which ends of the round trip are present. Encode + Decode is nearly transparent "
	              "with the levels matched -- that is the point. Decode Only goes dull and breathes. "
	              "Encode Only goes hard and glassy.",
	              compander::modeCount(), compander::modeLabel, static_cast< int >( defaults.nrMode ) );
	defineSlider( desc, page, nrGroup, kParamMistracking, "Mistracking",
	              "The decoder's level error, +/- 12 dB, centred on correct. Off centre the round trip "
	              "cancels at some brightnesses and not others, so detail pumps: a tape recorded on "
	              "one deck and played on another.",
	              defaults.mistracking );
	defineSlider( desc, page, nrGroup, kParamNrStrength, "NR Strength",
	              "Scales every stage's boost. A real system has no such knob.", defaults.nrStrength );

	//------------------------------------------------------------------ Output
	OFX::GroupParamDescriptor* outputGroup = defineGroup( desc, page, "Output" );

	OFX::BooleanParamDescriptor* traceParam = desc.defineBooleanParam( kParamShowTrace );
	traceParam->setLabels( "Show Trace", "Show Trace", "Show Trace" );
	traceParam->setHint( "Draws the error signal as a function of position down the picture -- the "
	                     "shape the image is being torn by -- with a readout of weighted wow and flutter "
	                     "as a percentage of a line period. A diagnostic, drawn after Mix. The audio "
	                     "meters of the Resolume build read zero here." );
	traceParam->setDefault( defaults.showTrace >= 0.5f );
	traceParam->setParent( *outputGroup );
	page->addChild( *traceParam );

	defineSlider( desc, page, outputGroup, kParamMix, "Mix", "Wet/dry against the untouched input.", defaults.mix );

	// The Stoatworks About block: a read-only credit line and one push button
	// per link, in a group that starts folded. Last, so it sits under the
	// effect's own controls.
	stoatworks::about::ofx::describe( desc, page );
}

OFX::ImageEffect* FerricPluginFactory::createInstance( OfxImageEffectHandle handle, OFX::ContextEnum )
{
	return new FerricPlugin( handle );
}

void OFX::Plugin::getPluginIDs( OFX::PluginFactoryArray& ids )
{
	// Deliberately leaked: a by-value static would register an exit-time
	// destructor inside this module, and a host that dlclose()s the bundle
	// before process exit then jumps through a dangling pointer.
	static FerricPluginFactory* factory =
		new FerricPluginFactory( kPluginIdentifier, PLUGIN_VERSION_MAJOR, PLUGIN_VERSION_MINOR );
	ids.push_back( factory );
}
