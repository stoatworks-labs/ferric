/**
    The C++ half of the browser demo's port check.

    Reads a scenario (scenario.py writes one) on stdin and prints, for every
    frame, what the plugin's own CPU code makes of it: the drive's outputs, the
    render settings, the frame's phases, the hiss and dropout clocks, the
    weighted figure, and every uniform each of the three passes is given --
    as hex bit patterns, so a comparison is exact or it is nothing.

    Compiled against the plugin's own Transport.cpp, Controls.cpp,
    Compander.cpp and Drive.cpp (crosscheck.sh builds it), which is the point:
    this file calls them and computes nothing of its own. The ONE thing written
    out here is the composition -- `step()` below is the CPU part of
    `Ferric::ProcessOpenGL` (Ferric.cpp), the clock already in seconds, in the
    same order and with the same arithmetic, because the real method is
    tangled with GL and cannot run without a context. That copy is checked by a
    reader, and the frames drawn by the page were compared once with frames
    from `frtest --pipe`, which runs the real class (AGENTS.md has the numbers).

    demo/tools/crosscheck/frames.mjs prints the same lines from demo/ferric.js.
*/

#include "Compander.h"
#include "Controls.h"
#include "Drive.h"
#include "Presets.h"
#include "Transport.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

using namespace ferric;

namespace
{
std::string f( float v )
{
	uint32_t bits = 0;
	std::memcpy( &bits, &v, 4 );
	char out[ 16 ];
	std::snprintf( out, sizeof out, "%08x", bits );
	return out;
}

std::string d( double v )
{
	uint64_t bits = 0;
	std::memcpy( &bits, &v, 8 );
	char out[ 24 ];
	std::snprintf( out, sizeof out, "%016llx", static_cast< unsigned long long >( bits ) );
	return out;
}

double fromHex64( const std::string& hex )
{
	const uint64_t bits = std::stoull( hex, nullptr, 16 );
	double v            = 0.0;
	std::memcpy( &v, &bits, 8 );
	return v;
}

float fromHex32( const std::string& hex )
{
	const uint32_t bits = static_cast< uint32_t >( std::stoul( hex, nullptr, 16 ) );
	float v             = 0.0f;
	std::memcpy( &v, &bits, 4 );
	return v;
}

/// The state Ferric keeps between frames that the CPU half touches.
struct Instance
{
	controls::HostValues host;
	double hissPhase    = 0.0;
	double dropPhase    = 0.0;
	double lastHostTime = -1.0;
	std::array< float, drive::kAudioBins > audioLevel = {};// silence: the page has no audio
	float cachedWf = 0.0f;
	transport::Settings cachedWfFor;
	bool cachedWfValid = false;
};

float* field( controls::HostValues& h, const std::string& name )
{
	static const std::map< std::string, float controls::HostValues::* > fields = {
		{ "machine", &controls::HostValues::machine }, { "wow", &controls::HostValues::wow },
		{ "wowRate", &controls::HostValues::wowRate }, { "flutter", &controls::HostValues::flutter },
		{ "flutterRate", &controls::HostValues::flutterRate }, { "scrape", &controls::HostValues::scrape },
		{ "drift", &controls::HostValues::drift }, { "tapeSpeed", &controls::HostValues::tapeSpeed },
		{ "amount", &controls::HostValues::amount }, { "vertical", &controls::HostValues::vertical },
		{ "hiss", &controls::HostValues::hiss }, { "dropouts", &controls::HostValues::dropouts },
		{ "headWear", &controls::HostValues::headWear }, { "nrType", &controls::HostValues::nrType },
		{ "nrMode", &controls::HostValues::nrMode }, { "mistracking", &controls::HostValues::mistracking },
		{ "nrStrength", &controls::HostValues::nrStrength }, { "sync", &controls::HostValues::sync },
		{ "beatDepth", &controls::HostValues::beatDepth }, { "beatDecay", &controls::HostValues::beatDecay },
		{ "division", &controls::HostValues::division }, { "levelDepth", &controls::HostValues::levelDepth },
		{ "bandDepth", &controls::HostValues::bandDepth }, { "route", &controls::HostValues::route },
		{ "showTrace", &controls::HostValues::showTrace }, { "mix", &controls::HostValues::mix },
	};
	const auto found = fields.find( name );
	return found == fields.end() ? nullptr : &( h.*( found->second ) );
}

/// The CPU part of Ferric::ProcessOpenGL, from `updateAudio()` to the last
/// uniform, for a host at 120 bpm whose bar position follows the clock.
void step( Instance& in, int frameW, int frameH, double now, float bpm, float barPhase, int frameNo )
{
	const controls::HostValues& host = in.host;

	drive::Input dIn;
	dIn.bins     = in.audioLevel.data();
	dIn.binCount = static_cast< int >( in.audioLevel.size() );
	dIn.bpm      = bpm;
	dIn.barPhase = barPhase;
	dIn.seconds  = now;
	const drive::Output driveOut = drive::compute( controls::driveSettings( host ), dIn );

	const controls::Render renderOut = controls::render( host, frameH, driveOut );
	const transport::Settings& tset  = renderOut.transport;
	const transport::Phase phase     = transport::phaseAt( tset, now );
	const transport::MachineProfile& machine = transport::machineProfile( tset.machine );

	if( in.lastHostTime >= 0.0 )
	{
		const double delta = std::clamp( now - in.lastHostTime, 0.0, 0.1 );
		const controls::TapeRates rates = controls::tapeRates( renderOut, frameW, frameH );
		in.hissPhase = controls::wrapTapePhase( in.hissPhase + delta * rates.hiss );
		in.dropPhase = controls::wrapTapePhase( in.dropPhase + delta * rates.dropouts );
	}
	in.lastHostTime = now;

	if( renderOut.showTrace )
	{
		const auto same = []( const transport::Settings& a, const transport::Settings& b ) {
			return a.machine == b.machine && a.wow == b.wow && a.flutter == b.flutter
			       && a.scrape == b.scrape && a.drift == b.drift && a.wowRate == b.wowRate
			       && a.flutterRate == b.flutterRate && a.amount == b.amount;
		};
		if( !( in.cachedWfValid && same( in.cachedWfFor, tset ) ) )
		{
			in.cachedWf      = transport::weightedPercent( tset );
			in.cachedWfFor   = tset;
			in.cachedWfValid = true;
		}
	}

	const compander::Uniforms nr = compander::uniforms( renderOut.nr );
	const float frameWf          = static_cast< float >( frameW );
	const float frameHf          = static_cast< float >( frameH );

	std::ostringstream o;
	o << "D " << frameNo << " scale=" << f( driveOut.scale ) << " lurch=" << f( driveOut.lurch )
	  << " mistrackDb=" << f( driveOut.mistrackDb ) << " bass=" << f( driveOut.bass )
	  << " mid=" << f( driveOut.mid ) << " high=" << f( driveOut.high ) << " level=" << f( driveOut.level )
	  << " beat=" << f( driveOut.beat ) << "\n";
	o << "C " << frameNo << " hissPhase=" << d( in.hissPhase ) << " dropPhase=" << d( in.dropPhase )
	  << " encode=" << renderOut.encode << " decode=" << renderOut.decode
	  << " showTrace=" << renderOut.showTrace << " machine=" << tset.machine << " lines=" << tset.lines
	  << " nrType=" << renderOut.nr.type << " nrMode=" << renderOut.nr.mode
	  << " wowRate=" << f( tset.wowRate ) << " flutterRate=" << f( tset.flutterRate )
	  << " seconds=" << f( tset.secondsPerPicture ) << " hissWidth=" << f( renderOut.tape.hissWidth ) << "\n";

	// Every uniform, by pass, in the names the shaders use.
	o << "U " << frameNo << " encode NrTexelX=" << f( 1.0f / frameWf ) << " NrThresh=" << f( nr.threshold[ 0 ] )
	  << "," << f( nr.threshold[ 1 ] ) << " NrBoost=" << f( nr.boost[ 0 ] ) << "," << f( nr.boost[ 1 ] )
	  << " DoEncode=" << ( renderOut.encode ? 1 : 0 ) << "\n";

	std::ostringstream tbe;
	tbe << " TbeWowRate=" << f( tset.wowRate * machine.wowRate ) << " TbeFlutRate=" << f( tset.flutterRate * machine.flutterRate )
	    << " TbeWowGain=" << f( machine.wowGain * tset.wow ) << " TbeFlutGain=" << f( machine.flutterGain * tset.flutter )
	    << " TbeScrapeGain=" << f( machine.scrapeGain * tset.scrape ) << " TbeDriftGain=" << f( machine.driftGain * tset.drift )
	    << " TbePeriodic=" << f( machine.periodic ) << " TbeLurch=" << f( tset.lurch )
	    << " TbePhaseWow1=" << f( phase.wow1 ) << " TbePhaseWow2=" << f( phase.wow2 )
	    << " TbePhaseFlut1=" << f( phase.flutter1 ) << " TbePhaseFlut2=" << f( phase.flutter2 )
	    << " TbePhaseScrape=" << f( phase.scrape ) << " TbePhaseDrift=" << f( phase.drift )
	    << " TbeSeconds=" << f( tset.secondsPerPicture ) << " TbeLines=" << f( static_cast< float >( tset.lines ) );

	o << "U " << frameNo << " tape" << tbe.str() << " TbeAmount=" << f( tset.amount )
	  << " TbeVertical=" << f( tset.vertical ) << " TbeAspectWH=" << f( frameWf / frameHf )
	  << " HissAmp=" << f( renderOut.tape.hiss ) << " HissWidth=" << f( renderOut.tape.hissWidth )
	  << " HissPhase=" << f( static_cast< float >( in.hissPhase ) ) << " DropoutP=" << f( renderOut.tape.dropouts )
	  << " DropBlocks=" << f( controls::kDropBlocks ) << " DropPhase=" << f( static_cast< float >( in.dropPhase ) )
	  << " HeadWear=" << f( renderOut.tape.headWear ) << " TexelX=" << f( 1.0f / frameWf )
	  << " PixelsPerLine=" << f( frameWf ) << "\n";

	o << "U " << frameNo << " decode NrTexelX=" << f( 1.0f / frameWf ) << " NrMistrack=" << f( nr.mistrack )
	  << " MixAmount=" << f( renderOut.mix ) << " NrThresh=" << f( nr.threshold[ 0 ] ) << "," << f( nr.threshold[ 1 ] )
	  << " NrBoost=" << f( nr.boost[ 0 ] ) << "," << f( nr.boost[ 1 ] )
	  << " DoDecode=" << ( renderOut.decode ? 1 : 0 ) << " ShowTrace=" << ( renderOut.showTrace ? 1 : 0 ) << tbe.str()
	  << " TraceBass=" << f( driveOut.bass ) << " TraceMid=" << f( driveOut.mid ) << " TraceHigh=" << f( driveOut.high )
	  << " TraceLevel=" << f( driveOut.level ) << " TraceBeat=" << f( driveOut.beat )
	  << " TraceWfPercent=" << f( in.cachedWf ) << " TraceAspect=" << f( frameWf / frameHf ) << "\n";

	std::cout << o.str();
}
} // namespace

int main()
{
	Instance in;
	int width  = 960;
	int height = 540;
	int frames = 0;

	std::string line;
	while( std::getline( std::cin, line ) )
	{
		std::istringstream words( line );
		std::string verb;
		if( !( words >> verb ) )
			continue;

		if( verb == "new" )
		{
			in     = Instance{};
			frames = 0;
			std::cout << "N\n";
		}
		else if( verb == "size" )
		{
			words >> width >> height;
			std::cout << "S " << width << "x" << height << "\n";
		}
		else if( verb == "set" )
		{
			// set <name> <hex32>
			std::string name, hex;
			words >> name >> hex;
			float* slot = field( in.host, name );
			if( slot == nullptr )
			{
				std::cerr << "printer: no host field " << name << "\n";
				return 2;
			}
			*slot = fromHex32( hex );
		}
		else if( verb == "preset" )
		{
			// The table's values written into the covered fields, as applyPreset does.
			int element = 0;
			words >> element;
			static const char* const kColumns[ presets::kParamCount ] = {
				"machine", "wow", "wowRate", "flutter", "flutterRate", "scrape", "drift", "tapeSpeed", "amount",
				"vertical", "hiss", "dropouts", "headWear", "nrType", "nrMode", "mistracking", "nrStrength",
				"sync", "beatDepth", "levelDepth", "bandDepth",
			};
			const float* values = presets::values( element );
			if( values != nullptr )
				for( int j = 0; j < presets::kParamCount; ++j )
					*field( in.host, kColumns[ j ] ) = values[ j ];
			std::cout << "P " << element << " " << presets::label( element ) << "\n";
		}
		else if( verb == "frame" )
		{
			// frame <hex64 seconds>: the page's host -- 120 bpm, bar position
			// from the clock, as frtest's driveFrames.
			std::string hex;
			words >> hex;
			const double now  = fromHex64( hex );
			const double bars = now / ( 240.0 / 120.0 );
			step( in, width, height, now, 120.0f, static_cast< float >( bars - std::floor( bars ) ), frames++ );
		}
		else if( verb == "machines" )
		{
			for( int m = 0; m < transport::machineCount(); ++m )
			{
				const auto& p = transport::machineProfile( m );
				std::cout << "M " << m << " " << p.label << " " << f( p.wowRate ) << " " << f( p.flutterRate ) << " "
				          << f( p.wowGain ) << " " << f( p.flutterGain ) << " " << f( p.scrapeGain ) << " "
				          << f( p.driftGain ) << " " << f( p.periodic ) << "\n";
			}
		}
		else if( verb == "labels" )
		{
			// Every dropdown's elements, in order -- the page's lists must be these.
			auto list = []( const char* name, int count, auto label ) {
				std::cout << "L " << name;
				for( int i = 0; i < count; ++i )
					std::cout << " |" << label( i );
				std::cout << "\n";
			};
			list( "machine", transport::machineCount(), transport::machineLabel );
			list( "nrType", compander::typeCount(), compander::typeLabel );
			list( "nrMode", compander::modeCount(), compander::modeLabel );
			list( "sync", drive::syncCount(), drive::syncLabel );
			list( "division", controls::divisionCount(), controls::divisionLabel );
			list( "route", drive::routeCount(), drive::routeLabel );
			list( "preset", presets::elementCount(), presets::label );
			const controls::HostValues defaults;
			std::cout << "H machine=" << f( defaults.machine ) << " wow=" << f( defaults.wow ) << " wowRate=" << f( defaults.wowRate )
			          << " flutter=" << f( defaults.flutter ) << " flutterRate=" << f( defaults.flutterRate )
			          << " scrape=" << f( defaults.scrape ) << " drift=" << f( defaults.drift ) << " tapeSpeed=" << f( defaults.tapeSpeed )
			          << " amount=" << f( defaults.amount ) << " vertical=" << f( defaults.vertical ) << " hiss=" << f( defaults.hiss )
			          << " dropouts=" << f( defaults.dropouts ) << " headWear=" << f( defaults.headWear ) << " nrType=" << f( defaults.nrType )
			          << " nrMode=" << f( defaults.nrMode ) << " mistracking=" << f( defaults.mistracking )
			          << " nrStrength=" << f( defaults.nrStrength ) << " sync=" << f( defaults.sync ) << " beatDepth=" << f( defaults.beatDepth )
			          << " beatDecay=" << f( defaults.beatDecay ) << " division=" << f( defaults.division )
			          << " levelDepth=" << f( defaults.levelDepth ) << " bandDepth=" << f( defaults.bandDepth )
			          << " route=" << f( defaults.route ) << " showTrace=" << f( defaults.showTrace ) << " mix=" << f( defaults.mix ) << "\n";
			for( int e = 1; e < presets::elementCount(); ++e )
			{
				std::cout << "T " << e;
				for( int j = 0; j < presets::kParamCount; ++j )
					std::cout << " " << f( presets::values( e )[ j ] );
				std::cout << "\n";
			}
		}
		else
		{
			std::cerr << "printer: unknown verb " << verb << "\n";
			return 2;
		}
	}
	return 0;
}
