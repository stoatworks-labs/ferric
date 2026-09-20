#include "Ferric.h"

//The SDK's umbrella FFGLSDK.h pulls in every other scoped binding but leaves
//this one out (SDK b1afaf9), so it has to be reached for by hand.
#include <ffglex/FFGLScopedFBOBinding.h>

#include "Diag.h"
#include "Shaders.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <string>

using namespace ffglex;
using namespace ferric;

static CFFGLPluginInfo PluginInfo(
	PluginFactory< Ferric >,                             // Create method
	"FR01",                                              // Plugin unique ID of maximum length 4
	"SW Ferric",                                         // Plugin name
	2,                                                   // API major version number
	1,                                                   // API minor version number
	0,                                                   // Plugin major version number
	1,                                                   // Plugin minor version number
	FF_EFFECT,                                           // Plugin type
	"Put the picture on tape.\n\nWow and flutter is not a wobble applied to a picture. It is a timing error applied to a signal, and a picture read off tape is a signal with a clock - so every pixel has a tape time, and its displacement is one error signal evaluated there.\n\nEverything falls out of that. Wow is slower than one picture, so the whole frame leans and breathes together. Flutter fits a cycle or two into a picture, so it draws a travelling wave down the image.\n\nThe oxide adds its own hiss and drops out here and there, and the sliding-band noise reduction that hid one under the other is here too.\n\nStart from a Preset, at the bottom.",// Plugin description
	"Ferric FFGL effect"                                 // About
);

namespace
{
/// Frames that must agree before the host's clock unit is settled.
constexpr int kClockVotes = 4;

/// The longest frame delta the hiss phase will accept. A host that stalls --
/// loading a clip, or an operator dragging the transport -- hands over an
/// enormous delta on the next frame, and advancing the hiss by it puts a visible
/// jump in the grain. Clamping costs nothing anybody can see and buys not
/// flinching after every hiccup.
constexpr double kMaxFrameDelta = 0.1;

/// Dropout blocks across one scanline. Twenty-four makes the shortest dropout
/// about four percent of the picture wide, which is roughly what a real one
/// looks like; more and they read as speckle rather than as loss of contact.
constexpr float kDropBlocks = 24.0f;

/// Where the hiss and dropout phases wrap. Same reasoning as the transport's
/// noise wrap: far enough apart that the seam is uncorrelated with what came
/// before it, near enough that a float still separates adjacent pixels.
constexpr double kPhaseWrap = 65536.0;

/// Wall clock, for hosts that never call SetTime. Steady rather than system, so
/// it cannot go backwards when the machine's clock is corrected mid-show.
double wallSeconds()
{
	using namespace std::chrono;
	static const auto origin = steady_clock::now();
	return duration_cast< duration< double > >( steady_clock::now() - origin ).count();
}

/// glGetString returns nullptr when there is no current context, and feeding
/// that to std::string is undefined behaviour. A logging call must never be the
/// thing that brings the host down.
std::string glStringOrUnknown( GLenum name )
{
	const GLubyte* value = glGetString( name );
	return value ? reinterpret_cast< const char* >( value ) : "unknown";
}

double wrapPhase( double v )
{
	return v - kPhaseWrap * std::floor( v / kPhaseWrap );
}
} // namespace

Ferric::Ferric()
{
	SetMinInputs( 1 );
	SetMaxInputs( 1 );

	//Without this the host is entitled never to call SetTime, and a transport
	//with no clock is a still picture with a fixed distortion on it.
	SetTimeSupported( true );

	//---------------------------------------------------------------------
	// Defaults. SetParamInfof reads each one back out of GetFloatParameter, so
	// these assignments are what the host is told the defaults are.
	//
	// They are a cassette that is visibly and audibly a cassette the moment it
	// lands on a layer -- wow, flutter and hiss all present -- with the noise
	// reduction OFF and every reactive depth at zero. All three halves of that
	// matter. An effect that does nothing until six sliders move is an effect
	// nobody finds out is any good; an effect gated by audio nobody has routed
	// looks broken in the same way; and noise reduction defaulting ON would mean
	// the first thing an operator sees is a stage quietly cancelling another
	// one, which is the least legible possible introduction to it.
	//---------------------------------------------------------------------
	const controls::HostValues defaults;

	params[ PT_MACHINE ]      = defaults.machine;
	params[ PT_WOW ]          = defaults.wow;
	params[ PT_WOW_RATE ]     = defaults.wowRate;
	params[ PT_FLUTTER ]      = defaults.flutter;
	params[ PT_FLUTTER_RATE ] = defaults.flutterRate;
	params[ PT_SCRAPE ]       = defaults.scrape;
	params[ PT_DRIFT ]        = defaults.drift;
	params[ PT_TAPE_SPEED ]   = defaults.tapeSpeed;
	params[ PT_AMOUNT ]       = defaults.amount;
	params[ PT_VERTICAL ]     = defaults.vertical;

	params[ PT_HISS ]      = defaults.hiss;
	params[ PT_DROPOUTS ]  = defaults.dropouts;
	params[ PT_HEAD_WEAR ] = defaults.headWear;

	params[ PT_NR_TYPE ]     = defaults.nrType;
	params[ PT_NR_MODE ]     = defaults.nrMode;
	params[ PT_MISTRACKING ] = defaults.mistracking;
	params[ PT_NR_STRENGTH ] = defaults.nrStrength;

	params[ PT_AUDIO ]       = 0.0f;
	params[ PT_SYNC ]        = defaults.sync;
	params[ PT_BEAT_DEPTH ]  = defaults.beatDepth;
	params[ PT_BEAT_DECAY ]  = defaults.beatDecay;
	params[ PT_DIVISION ]    = defaults.division;
	params[ PT_LEVEL_DEPTH ] = defaults.levelDepth;
	params[ PT_BAND_DEPTH ]  = defaults.bandDepth;
	params[ PT_ROUTE ]       = defaults.route;

	params[ PT_SHOW_TRACE ] = defaults.showTrace;
	params[ PT_MIX ]        = defaults.mix;

	params[ PT_PRESET ] = 0.0f;//Custom: the sliders are the truth

	//---------------------------------------------------------------------
	// Declaration.
	//
	// ⚠️ Every name here is 16 characters or fewer. FFGL's legacy
	// FF_GET_PARAMETER_NAME hands the host a 16-character buffer and the SDK
	// does not enforce it -- the plugin stores the whole string, hands over a
	// pointer to all of it, and Resolume copies sixteen. Every offline harness
	// passes, --list prints the full name, and only the host is ever wrong. Six
	// plugins in this fleet shipped a control called `Background Opaci` before
	// anybody noticed.
	//
	// Group names and element names are NOT subject to it, which is why
	// "Noise Reduction" and "Encode + Decode" are allowed to be long.
	//---------------------------------------------------------------------
	SetOptionParamInfo( PT_MACHINE, "Machine", transport::machineCount(), params[ PT_MACHINE ] );
	for( int i = 0; i < transport::machineCount(); ++i )
		SetParamElementInfo( PT_MACHINE, i, transport::machineLabel( i ), static_cast< float >( i ) );

	SetParamInfof( PT_WOW, "Wow", FF_TYPE_STANDARD );
	SetParamInfof( PT_WOW_RATE, "Wow Rate", FF_TYPE_STANDARD );
	SetParamInfof( PT_FLUTTER, "Flutter", FF_TYPE_STANDARD );
	SetParamInfof( PT_FLUTTER_RATE, "Flutter Rate", FF_TYPE_STANDARD );
	SetParamInfof( PT_SCRAPE, "Scrape", FF_TYPE_STANDARD );
	SetParamInfof( PT_DRIFT, "Drift", FF_TYPE_STANDARD );
	SetParamInfof( PT_TAPE_SPEED, "Tape Speed", FF_TYPE_STANDARD );
	SetParamInfof( PT_AMOUNT, "Amount", FF_TYPE_STANDARD );
	SetParamInfof( PT_VERTICAL, "Vertical", FF_TYPE_STANDARD );

	SetParamInfof( PT_HISS, "Hiss", FF_TYPE_STANDARD );
	SetParamInfof( PT_DROPOUTS, "Dropouts", FF_TYPE_STANDARD );
	SetParamInfof( PT_HEAD_WEAR, "Head Wear", FF_TYPE_STANDARD );

	SetOptionParamInfo( PT_NR_TYPE, "NR Type", compander::typeCount(), params[ PT_NR_TYPE ] );
	for( int i = 0; i < compander::typeCount(); ++i )
		SetParamElementInfo( PT_NR_TYPE, i, compander::typeLabel( i ), static_cast< float >( i ) );

	SetOptionParamInfo( PT_NR_MODE, "NR Mode", compander::modeCount(), params[ PT_NR_MODE ] );
	for( int i = 0; i < compander::modeCount(); ++i )
		SetParamElementInfo( PT_NR_MODE, i, compander::modeLabel( i ), static_cast< float >( i ) );

	SetParamInfof( PT_MISTRACKING, "Mistracking", FF_TYPE_STANDARD );
	SetParamInfof( PT_NR_STRENGTH, "NR Strength", FF_TYPE_STANDARD );

	// An FFT buffer. Resolume renders it as an audio-source picker and writes one
	// spectrum bin per element; the elements are declared with a default of 0 so
	// a host that shows the picker but has nothing routed reads as silence rather
	// than as noise.
	SetBufferParamInfo( PT_AUDIO, "Audio", drive::kAudioBins, FF_USAGE_FFT );
	for( int i = 0; i < drive::kAudioBins; ++i )
		SetParamElementInfo( PT_AUDIO, i, "", 0.0f );

	SetOptionParamInfo( PT_SYNC, "Sync", drive::syncCount(), params[ PT_SYNC ] );
	for( int i = 0; i < drive::syncCount(); ++i )
		SetParamElementInfo( PT_SYNC, i, drive::syncLabel( i ), static_cast< float >( i ) );

	SetParamInfof( PT_BEAT_DEPTH, "Beat Depth", FF_TYPE_STANDARD );
	SetParamInfof( PT_BEAT_DECAY, "Beat Decay", FF_TYPE_STANDARD );

	SetOptionParamInfo( PT_DIVISION, "Division", controls::divisionCount(), params[ PT_DIVISION ] );
	for( int i = 0; i < controls::divisionCount(); ++i )
		SetParamElementInfo( PT_DIVISION, i, controls::divisionLabel( i ), static_cast< float >( i ) );

	SetParamInfof( PT_LEVEL_DEPTH, "Level Depth", FF_TYPE_STANDARD );
	SetParamInfof( PT_BAND_DEPTH, "Band Depth", FF_TYPE_STANDARD );

	SetOptionParamInfo( PT_ROUTE, "Route", drive::routeCount(), params[ PT_ROUTE ] );
	for( int i = 0; i < drive::routeCount(); ++i )
		SetParamElementInfo( PT_ROUTE, i, drive::routeLabel( i ), static_cast< float >( i ) );

	SetParamInfof( PT_SHOW_TRACE, "Show Trace", FF_TYPE_BOOLEAN );
	SetParamInfof( PT_MIX, "Mix", FF_TYPE_STANDARD );

	SetOptionParamInfo( PT_PRESET, "Preset", presets::elementCount(), params[ PT_PRESET ] );
	for( int i = 0; i < presets::elementCount(); ++i )
		SetParamElementInfo( PT_PRESET, i, presets::label( i ), static_cast< float >( i ) );

	// The About block. Inline rather than through a helper: SetParamInfo is
	// protected on CFFGLPlugin, so nothing outside the class can call it.
	SetParamInfo( PT_ABOUT_FIRST, "About", FF_TYPE_TEXT, stoatworks::about::defaultText() );
	{
		FFUInt32 aboutId = PT_ABOUT_FIRST + 1;
		for( const auto& b : stoatworks::about::buttons() )
			SetParamInfo( aboutId++, b.label, FF_TYPE_EVENT, false );
	}

	// Groups. This is twenty-eight parameters, and an ungrouped list of
	// twenty-eight in somebody else's inspector is unusable.
	for( FFUInt32 i = PT_MACHINE; i <= PT_VERTICAL; ++i )
		SetParamGroup( i, "Transport" );
	for( FFUInt32 i = PT_HISS; i <= PT_HEAD_WEAR; ++i )
		SetParamGroup( i, "Tape" );
	for( FFUInt32 i = PT_NR_TYPE; i <= PT_NR_STRENGTH; ++i )
		SetParamGroup( i, "Noise Reduction" );
	for( FFUInt32 i = PT_AUDIO; i <= PT_ROUTE; ++i )
		SetParamGroup( i, "Reaction" );
	for( FFUInt32 i = PT_SHOW_TRACE; i <= PT_MIX; ++i )
		SetParamGroup( i, "Output" );

	SetParamGroup( PT_PRESET, "Preset" );

	FFGLLog::LogToHost( "Created Ferric effect" );

	diag::init();
}

controls::HostValues Ferric::hostValues() const
{
	controls::HostValues out;

	out.machine     = params[ PT_MACHINE ];
	out.wow         = params[ PT_WOW ];
	out.wowRate     = params[ PT_WOW_RATE ];
	out.flutter     = params[ PT_FLUTTER ];
	out.flutterRate = params[ PT_FLUTTER_RATE ];
	out.scrape      = params[ PT_SCRAPE ];
	out.drift       = params[ PT_DRIFT ];
	out.tapeSpeed   = params[ PT_TAPE_SPEED ];
	out.amount      = params[ PT_AMOUNT ];
	out.vertical    = params[ PT_VERTICAL ];

	out.hiss     = params[ PT_HISS ];
	out.dropouts = params[ PT_DROPOUTS ];
	out.headWear = params[ PT_HEAD_WEAR ];

	out.nrType      = params[ PT_NR_TYPE ];
	out.nrMode      = params[ PT_NR_MODE ];
	out.mistracking = params[ PT_MISTRACKING ];
	out.nrStrength  = params[ PT_NR_STRENGTH ];

	out.sync       = params[ PT_SYNC ];
	out.beatDepth  = params[ PT_BEAT_DEPTH ];
	out.beatDecay  = params[ PT_BEAT_DECAY ];
	out.division   = params[ PT_DIVISION ];
	out.levelDepth = params[ PT_LEVEL_DEPTH ];
	out.bandDepth  = params[ PT_BAND_DEPTH ];
	out.route      = params[ PT_ROUTE ];

	out.showTrace = params[ PT_SHOW_TRACE ];
	out.mix       = params[ PT_MIX ];

	return out;
}

bool Ferric::compileShaders()
{
	struct Stage
	{
		FFGLShader* shader;
		std::string fragment;
		const char* name;
	};

	const Stage stages[] = {
		{ &encodeShader, shaders::EncodeFragment(), "encode" },
		{ &tapeShader, shaders::TapeFragment(), "tape" },
		{ &decodeShader, shaders::DecodeFragment(), "decode" },
	};

	for( const Stage& stage : stages )
	{
		if( !stage.shader->Compile( shaders::kVertex, stage.fragment.c_str() ) )
		{
			//Returning FF_FAIL from InitGL is invisible to the operator: the
			//effect simply does nothing in Resolume, with no message anywhere.
			//This line is the only record of which stage it was -- and each of
			//these is assembled from several strings at runtime, so any line
			//number the driver reports refers to a file that does not exist.
			diag::error( std::string( "the " ) + stage.name
			             + " shader failed to compile - the effect will do nothing" );
			FFGLLog::LogToHost( "Ferric: shader failed to compile" );
			return false;
		}
	}

	return true;
}

FFResult Ferric::InitGL( const FFGLViewportStruct* vp )
{
	//The GL strings first, and unconditionally. When a shader will not compile it
	//is almost always the driver or the GL version, and knowing which machine
	//reported what is the whole diagnosis.
	diag::info( std::string( "GL vendor=" ) + glStringOrUnknown( GL_VENDOR )
	            + " renderer=" + glStringOrUnknown( GL_RENDERER )
	            + " version=" + glStringOrUnknown( GL_VERSION ) );

	if( !compileShaders() )
	{
		DeInitGL();
		return FF_FAIL;
	}

	if( !quad.Initialise() )
	{
		diag::error( "quad geometry failed to initialise" );
		FFGLLog::LogToHost( "Ferric: quad geometry failed to initialise" );
		DeInitGL();
		return FF_FAIL;
	}

	diag::info( "initialised" );

	//Use the base class init as the success result so it retains the viewport.
	return CFFGLPlugin::InitGL( vp );
}

void Ferric::refreshWeighted( const transport::Settings& t )
{
	// The lurch is excluded from the figure, so a frame that differs only by
	// what the music did is not a new measurement.
	const auto same = []( const transport::Settings& a, const transport::Settings& b ) {
		return a.machine == b.machine && a.wow == b.wow && a.flutter == b.flutter
		       && a.scrape == b.scrape && a.drift == b.drift && a.wowRate == b.wowRate
		       && a.flutterRate == b.flutterRate && a.amount == b.amount;
	};

	if( cachedWfValid && same( cachedWfFor, t ) )
		return;

	cachedWf      = transport::weightedPercent( t );
	cachedWfFor   = t;
	cachedWfValid = true;
}

FFResult Ferric::ProcessOpenGL( ProcessOpenGLStruct* pGL )
{
	if( pGL->numInputTextures < 1 || pGL->inputTextures[ 0 ] == nullptr )
		return FF_FAIL;

	const FFGLTextureStruct& input = *pGL->inputTextures[ 0 ];

	//The host's viewport, not the one InitGL was handed: Resolume changes
	//composition resolution without reinitialising the plugin.
	//
	//It also has to be captured before any pass runs, because ScopedFBOBinding
	//restores the framebuffer binding and NOT the viewport -- so an off-screen
	//pass's ResizeViewPort() leaks into the decode pass, which draws to the
	//host's own framebuffer and has no buffer of its own to size itself from.
	//The symptom does not look like a viewport bug: the effect renders correctly
	//into a corner of the frame and leaves the rest untouched.
	GLint hostViewport[ 4 ] = { 0, 0, 0, 0 };
	glGetIntegerv( GL_VIEWPORT, hostViewport );
	const int frameW = std::max( 1, hostViewport[ 2 ] );
	const int frameH = std::max( 1, hostViewport[ 3 ] );

	const float frameWf = static_cast< float >( frameW );
	const float frameHf = static_cast< float >( frameH );

	const controls::HostValues host = hostValues();

	//---------------------------------------------------------------------
	// Time. Normalise the host's clock to seconds first -- Resolume sends
	// milliseconds, this repo's harness sends seconds, and the FFGL header says
	// nothing at all.
	//
	// steady_clock says how much real time passed, the host says how much host
	// time passed, and the ratio names the unit outright -- 1 for seconds, 1000
	// for milliseconds, and nothing plausible in between. Several frames rather
	// than one, so a single odd frame cannot decide it alone.
	//---------------------------------------------------------------------
	const double wallNow = wallSeconds();
	if( wallStart < 0.0 )
		wallStart = wallNow;

	const double raw = hostTime;

	if( clockScale == 0.0 && raw >= 0.0 && lastRawTime >= 0.0 && lastWallTime >= 0.0 )
	{
		const double hostDelta = raw - lastRawTime;
		const double wallDelta = wallNow - lastWallTime;

		// A paused host, a looping clip or a stalled frame tells us nothing.
		if( hostDelta > 0.0 && wallDelta >= 0.0005 )
		{
			const double ratio = hostDelta / wallDelta;
			if( ratio > 0.1 && ratio < 10.0 )
				++secondsVotes;
			else if( ratio > 100.0 && ratio < 10000.0 )
				++millisVotes;

			if( secondsVotes >= kClockVotes || millisVotes >= kClockVotes )
				clockScale = millisVotes > secondsVotes ? 0.001 : 1.0;
		}
	}

	if( raw >= 0.0 )
		lastRawTime = raw;
	lastWallTime = wallNow;

	// Until the unit is settled -- and for a host that never calls SetTime --
	// run on the real clock: wrong in origin but right in rate, where assuming
	// seconds would be a thousand times fast on Resolume.
	const double now = ( raw >= 0.0 && clockScale != 0.0 ) ? raw * clockScale
	                                                       : wallNow - wallStart;

	updateAudio();

	//---------------------------------------------------------------------
	// What the music is doing. All of it decided in Drive.cpp, on the CPU: the
	// spectrum never reaches a shader.
	//---------------------------------------------------------------------
	{
		drive::Input in;
		in.bins     = audioLevel.data();
		in.binCount = static_cast< int >( audioLevel.size() );
		in.bpm      = bpm;
		in.barPhase = barPhase;
		in.seconds  = now;

		driveOut = drive::compute( controls::driveSettings( host ), in );
	}

	//One scanline per output row, so the picture quantises to the render it is
	//actually being drawn at rather than to a number somebody picked.
	renderOut = controls::render( host, frameH, driveOut );

	const transport::Settings& tset = renderOut.transport;
	const transport::Phase phase    = transport::phaseAt( tset, now );
	const transport::MachineProfile& machine = transport::machineProfile( tset.machine );

	//The hiss advances along the tape with the clock, wrapped, for the reason
	//given in Ferric.h. Integrated rather than derived from `now` so that a
	//host stall does not jump it.
	if( lastHostTime >= 0.0 )
	{
		const double delta = std::clamp( now - lastHostTime, 0.0, kMaxFrameDelta );

		//Scan samples per second: lines per second times pixels per line.
		const double linesPerSecond = static_cast< double >( frameH )
		                              / std::max( 1e-4, static_cast< double >( tset.secondsPerPicture ) );
		hissPhase = wrapPhase( hissPhase + delta * linesPerSecond * frameWf
		                       / std::max( 0.5, static_cast< double >( renderOut.tape.hissWidth ) ) );
		dropPhase = wrapPhase( dropPhase + delta * linesPerSecond * 0.01 );
	}

	if( ++clockFrames == 60 )
		diag::info( "host clock at frame 60: raw=" + std::to_string( raw )
		            + " scale=" + std::to_string( clockScale )
		            + " seconds=" + std::to_string( now )
		            + " bpm=" + std::to_string( bpm )
		            + " barPhase=" + std::to_string( barPhase ) );

	lastHostTime = now;

	if( renderOut.showTrace )
		refreshWeighted( tset );

	//---------------------------------------------------------------------
	// Every allocation FIRST, before anything is bound.
	//
	// FFGLFBO::Initialise sizes its colour texture inside a scoped texture
	// binding, and those CLEAR to 0 on scope exit rather than restoring -- so an
	// Ensure() called after a texture was bound silently unbinds it, and the
	// frame that allocated renders black. PassBuffer::Ensure saves and restores
	// around it as well, but the ordering here is the real defence: do not move
	// this below the passes.
	//
	// RGBA16F and not 8-bit. The encode side can push a bright edge well past 1
	// and the decode side brings it back; clipping it in between would make the
	// round trip lossy in exactly the place the whole design says it is not.
	//---------------------------------------------------------------------
	if( !encodeBuffer.Ensure( frameW, frameH, GL_RGBA16F ) )
	{
		diag::error( "could not allocate the encode buffer" );
		return FF_FAIL;
	}

	if( !tapeBuffer.Ensure( frameW, frameH, GL_RGBA16F ) )
	{
		diag::error( "could not allocate the tape buffer" );
		return FF_FAIL;
	}

	const FFGLTexCoords maxCoords = GetMaxGLTexCoords( input );
	const compander::Uniforms nr  = compander::uniforms( renderOut.nr );

	//Every pass does its geometry in picture space and applies MaxUV at the
	//fetch, so the vertex shader's scaling is always off.
	const float kNoScale = 1.0f;

	//------------------------------------------------------------------
	// 1. Encode, into a buffer of ours.
	//
	//    Runs whatever the noise reduction is set to. Resolving MaxUV is its
	//    other job and the tape pass is a warp, which samples wherever it likes.
	//------------------------------------------------------------------
	{
		ScopedFBOBinding fbo( encodeBuffer.GetGLID(), ScopedFBOBinding::RB_REVERT );
		encodeBuffer.ResizeViewPort();
		ScopedShaderBinding shader( encodeShader.GetGLID() );
		ScopedSamplerActivation sampler( 0 );
		Scoped2DTextureBinding texture( input.Handle );

		encodeShader.Set( "MaxUV", kNoScale, kNoScale );
		encodeShader.Set( "InputTexture", 0 );
		encodeShader.Set( "InputMaxUV", maxCoords.s, maxCoords.t );
		encodeShader.Set( "NrTexelX", 1.0f / frameWf );

		//`FFGLShader::Set` has no array overload and no bool overload. An array
		//pushed through the float one is a GL_INVALID_OPERATION that leaves the
		//uniform at zero with nothing anywhere the plugin can see, so both go
		//through the raw calls.
		glUniform1fv( encodeShader.FindUniform( "NrThresh" ), compander::kMaxStages, nr.threshold );
		glUniform1fv( encodeShader.FindUniform( "NrBoost" ), compander::kMaxStages, nr.boost );
		glUniform1i( encodeShader.FindUniform( "DoEncode" ), renderOut.encode ? 1 : 0 );

		quad.Draw();
	}

	//------------------------------------------------------------------
	// 2. The medium.
	//------------------------------------------------------------------
	{
		ScopedFBOBinding fbo( tapeBuffer.GetGLID(), ScopedFBOBinding::RB_REVERT );
		tapeBuffer.ResizeViewPort();
		ScopedShaderBinding shader( tapeShader.GetGLID() );
		ScopedSamplerActivation sampler( 0 );
		Scoped2DTextureBinding texture( encodeBuffer.GetTextureInfo().Handle );

		tapeShader.Set( "MaxUV", kNoScale, kNoScale );
		tapeShader.Set( "TapeTexture", 0 );

		//The machine profile, multiplied out here so nothing in GLSL knows what
		//a machine is. A table has no reason to exist twice.
		tapeShader.Set( "TbeWowRate", tset.wowRate * machine.wowRate );
		tapeShader.Set( "TbeFlutRate", tset.flutterRate * machine.flutterRate );
		tapeShader.Set( "TbeWowGain", machine.wowGain * tset.wow );
		tapeShader.Set( "TbeFlutGain", machine.flutterGain * tset.flutter );
		tapeShader.Set( "TbeScrapeGain", machine.scrapeGain * tset.scrape );
		tapeShader.Set( "TbeDriftGain", machine.driftGain * tset.drift );
		tapeShader.Set( "TbePeriodic", machine.periodic );
		tapeShader.Set( "TbeLurch", tset.lurch );

		tapeShader.Set( "TbePhaseWow1", phase.wow1 );
		tapeShader.Set( "TbePhaseWow2", phase.wow2 );
		tapeShader.Set( "TbePhaseFlut1", phase.flutter1 );
		tapeShader.Set( "TbePhaseFlut2", phase.flutter2 );
		tapeShader.Set( "TbePhaseScrape", phase.scrape );
		tapeShader.Set( "TbePhaseDrift", phase.drift );

		tapeShader.Set( "TbeSeconds", tset.secondsPerPicture );
		tapeShader.Set( "TbeLines", static_cast< float >( tset.lines ) );
		tapeShader.Set( "TbeAmount", tset.amount );
		tapeShader.Set( "TbeVertical", tset.vertical );
		tapeShader.Set( "TbeAspectWH", frameWf / frameHf );

		tapeShader.Set( "HissAmp", renderOut.tape.hiss );
		tapeShader.Set( "HissWidth", renderOut.tape.hissWidth );
		tapeShader.Set( "HissPhase", static_cast< float >( hissPhase ) );
		tapeShader.Set( "DropoutP", renderOut.tape.dropouts );
		tapeShader.Set( "DropBlocks", kDropBlocks );
		tapeShader.Set( "DropPhase", static_cast< float >( dropPhase ) );
		tapeShader.Set( "HeadWear", renderOut.tape.headWear );
		tapeShader.Set( "TexelX", 1.0f / frameWf );
		tapeShader.Set( "PixelsPerLine", frameWf );

		quad.Draw();
	}

	//------------------------------------------------------------------
	// 3. Decode, mix and overlay, straight into whatever the host handed us.
	//------------------------------------------------------------------
	{
		glBindFramebuffer( GL_FRAMEBUFFER, pGL->HostFBO );
		glViewport( hostViewport[ 0 ], hostViewport[ 1 ], hostViewport[ 2 ], hostViewport[ 3 ] );

		ScopedShaderBinding shader( decodeShader.GetGLID() );

		glActiveTexture( GL_TEXTURE0 );
		glBindTexture( GL_TEXTURE_2D, tapeBuffer.GetTextureInfo().Handle );
		glActiveTexture( GL_TEXTURE1 );
		glBindTexture( GL_TEXTURE_2D, input.Handle );
		glActiveTexture( GL_TEXTURE0 );

		decodeShader.Set( "MaxUV", kNoScale, kNoScale );
		decodeShader.Set( "TapeTexture", 0 );
		decodeShader.Set( "InputTexture", 1 );
		decodeShader.Set( "InputMaxUV", maxCoords.s, maxCoords.t );
		decodeShader.Set( "NrTexelX", 1.0f / frameWf );
		decodeShader.Set( "NrMistrack", nr.mistrack );
		decodeShader.Set( "MixAmount", renderOut.mix );

		glUniform1fv( decodeShader.FindUniform( "NrThresh" ), compander::kMaxStages, nr.threshold );
		glUniform1fv( decodeShader.FindUniform( "NrBoost" ), compander::kMaxStages, nr.boost );
		glUniform1i( decodeShader.FindUniform( "DoDecode" ), renderOut.decode ? 1 : 0 );
		glUniform1i( decodeShader.FindUniform( "ShowTrace" ), renderOut.showTrace ? 1 : 0 );

		//The trace plots the error signal, so it needs the same transport
		//uniforms the tape pass had. Cheap, and the alternative -- drawing the
		//overlay in the tape pass -- would put the noise reduction and the
		//wet/dry mix on top of a diagnostic.
		decodeShader.Set( "TbeWowRate", tset.wowRate * machine.wowRate );
		decodeShader.Set( "TbeFlutRate", tset.flutterRate * machine.flutterRate );
		decodeShader.Set( "TbeWowGain", machine.wowGain * tset.wow );
		decodeShader.Set( "TbeFlutGain", machine.flutterGain * tset.flutter );
		decodeShader.Set( "TbeScrapeGain", machine.scrapeGain * tset.scrape );
		decodeShader.Set( "TbeDriftGain", machine.driftGain * tset.drift );
		decodeShader.Set( "TbePeriodic", machine.periodic );
		decodeShader.Set( "TbeLurch", tset.lurch );
		decodeShader.Set( "TbePhaseWow1", phase.wow1 );
		decodeShader.Set( "TbePhaseWow2", phase.wow2 );
		decodeShader.Set( "TbePhaseFlut1", phase.flutter1 );
		decodeShader.Set( "TbePhaseFlut2", phase.flutter2 );
		decodeShader.Set( "TbePhaseScrape", phase.scrape );
		decodeShader.Set( "TbePhaseDrift", phase.drift );
		decodeShader.Set( "TbeSeconds", tset.secondsPerPicture );
		decodeShader.Set( "TbeLines", static_cast< float >( tset.lines ) );

		decodeShader.Set( "TraceBass", driveOut.bass );
		decodeShader.Set( "TraceMid", driveOut.mid );
		decodeShader.Set( "TraceHigh", driveOut.high );
		decodeShader.Set( "TraceLevel", driveOut.level );
		decodeShader.Set( "TraceBeat", driveOut.beat );
		decodeShader.Set( "TraceWfPercent", cachedWf );
		decodeShader.Set( "TraceAspect", frameWf / frameHf );

		quad.Draw();

		glActiveTexture( GL_TEXTURE1 );
		glBindTexture( GL_TEXTURE_2D, 0 );
		glActiveTexture( GL_TEXTURE0 );
		glBindTexture( GL_TEXTURE_2D, 0 );
	}

	return FF_SUCCESS;
}

FFResult Ferric::DeInitGL()
{
	encodeShader.FreeGLResources();
	tapeShader.FreeGLResources();
	decodeShader.FreeGLResources();
	quad.Release();
	encodeBuffer.Destroy();
	tapeBuffer.Destroy();

	return FF_SUCCESS;
}

FFResult Ferric::SetTime( double time )
{
	hostTime = time;
	return FF_SUCCESS;
}

void Ferric::updateAudio()
{
	const ParamInfo* info = FindParamInfo( PT_AUDIO );
	if( info == nullptr )
		return;

	// Frame delta for the release filter, off the same clock everything else
	// runs on -- lastHostTime is already normalised to seconds, so the
	// milliseconds question is settled before it gets here. First frame, or a
	// clock that has not moved, snaps instead.
	const double now = lastHostTime;
	const double dt  = ( audioClock >= 0.0 && now > audioClock ) ? now - audioClock : 0.0;
	audioClock       = now;

	// Fast up, slow down. A beat that arrives a frame late reads as broken,
	// while one that takes ~150 ms to die away reads as intended -- and for a
	// transport lurch in particular the asymmetry is what makes it a slip rather
	// than a flicker.
	const float release = dt > 0.0 ? 1.0f - std::exp( static_cast< float >( -dt / 0.15 ) ) : 1.0f;

	const size_t bins = std::min( info->elements.size(), audioLevel.size() );
	for( size_t i = 0; i < bins; ++i )
	{
		// sqrt because bin magnitudes bunch up near zero: a spectrum used raw
		// reacts to the kick drum and to nothing else.
		const float raw = std::sqrt( std::max( 0.0f, info->elements[ i ].value ) );

		if( raw >= audioLevel[ i ] )
			audioLevel[ i ] = raw;
		else
			audioLevel[ i ] += ( raw - audioLevel[ i ] ) * release;
	}
}

bool Ferric::isPresetParam( unsigned int index )
{
	for( unsigned int id : kPresetParamIDs )
	{
		if( id == index )
			return true;
	}

	return false;
}

void Ferric::seedHostSent()
{
	if( hostSeeded )
		return;

	for( unsigned int i = 0; i < PT_COUNT; ++i )
		hostSent[ i ] = params[ i ];

	hostSeeded = true;
}

FFResult Ferric::SetFloatParameter( unsigned int index, float value )
{
	if( index >= PT_COUNT )
		return FF_FAIL;

	// ☠️ Before the preset branch, always. Seeding lazily from inside it would
	// record the preset's own values as the host's opening position. See
	// Ferric.h.
	seedHostSent();

	// An About button is a press, not a value to keep: it opens a browser and
	// nothing about the effect changes.
	if( index >= PT_ABOUT_FIRST )
		return stoatworks::about::handleParam( index - PT_ABOUT_FIRST, value ) ? FF_SUCCESS : FF_FAIL;

	if( index == PT_PRESET )
	{
		hostSent[ index ] = value;

		const int chosen = static_cast< int >( std::lround( value ) );
		if( chosen != static_cast< int >( std::lround( params[ PT_PRESET ] ) ) )
			applyPreset( chosen );

		return FF_SUCCESS;
	}

	const float lastFromHost = hostSent[ index ];
	hostSent[ index ]        = value;

	const int active = static_cast< int >( std::lround( params[ PT_PRESET ] ) );
	if( active > 0 && isPresetParam( index ) )
	{
		// Three cases, and only the third is an operator.
		//
		//   The host restating what it already said  -> ignore
		//   The host echoing the preset back at us   -> ignore
		//   Anything else                            -> a real edit
		//
		// Judged on what the value IS. Judging on "did it change" is the bug:
		// Resolume does not consume value events, so after a preset lands it
		// carries on pushing the values it still believes in, and those have
		// changed relative to the preset every single frame.
		const bool restatement  = std::fabs( value - lastFromHost ) <= presets::kEchoTolerance;
		const bool echoOfPreset = std::fabs( value - params[ index ] ) <= presets::kEchoTolerance;

		if( restatement || echoOfPreset )
			return FF_SUCCESS;

		params[ index ]     = value;
		params[ PT_PRESET ] = 0.0f;
		diag::info( "preset dropped to Custom: parameter " + std::to_string( index )
		            + " moved to " + std::to_string( value ) );
		RaiseParamEvent( PT_PRESET, FF_EVENT_FLAG_VALUE );
		return FF_SUCCESS;
	}

	params[ index ] = value;
	return FF_SUCCESS;
}

void Ferric::applyPreset( int presetIndex )
{
	params[ PT_PRESET ] = static_cast< float >( presetIndex );

	const float* values = presets::values( presetIndex );
	if( values == nullptr )
		return;//Custom: the sliders keep whatever they said

	for( int j = 0; j < presets::kParamCount; ++j )
	{
		const unsigned int id = kPresetParamIDs[ j ];
		if( std::fabs( params[ id ] - values[ j ] ) <= 1e-6f )
			continue;

		// The write is what changes the picture. The event only asks the host to
		// re-read the slider, and a host that ignores it -- Resolume does --
		// renders the preset correctly and merely shows stale knobs.
		params[ id ] = values[ j ];
		RaiseParamEvent( id, FF_EVENT_FLAG_VALUE );
	}
}

float Ferric::GetFloatParameter( unsigned int index )
{
	if( index >= PT_COUNT )
		return 0.0f;

	return params[ index ];
}

char* Ferric::GetTextParameter( unsigned int index )
{
	// The host is handed a bare pointer, so the string is kept as a member
	// rather than built on the stack here.
	if( index == PT_ABOUT_FIRST )
	{
		aboutText = stoatworks::about::textParam( 0 );
		return const_cast< char* >( aboutText.c_str() );
	}

	return CFFGLPlugin::GetTextParameter( index );
}

FFResult Ferric::SetTextParameter( unsigned int index, const char* value )
{
	// See the declaration: the base class returns FF_FAIL, and instantiateGL
	// deletes the whole instance when setting any default fails. The About text
	// is display-only, so there is genuinely nothing to store -- but it has to
	// say so successfully.
	(void)value;

	if( index == PT_ABOUT_FIRST )
		return FF_SUCCESS;

	return CFFGLPlugin::SetTextParameter( index, value );
}

void Ferric::SetClockScaleForTest( double scale )
{
	clockScale = scale;
}
