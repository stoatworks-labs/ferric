#include "Controls.h"

#include <algorithm>
#include <cmath>

namespace ferric
{
namespace controls
{
namespace
{
//--------------------------------------------------------------------------
// Ranges. Every one of these is a decision about what the control is for, so
// they are named and gathered rather than spelled out at the point of use.
//--------------------------------------------------------------------------

/// Wow, in Hz. The bottom is slower than a picture is long, so it reads as a
/// lean rather than a wobble; the top is fast enough to be flutter, which is
/// wanted -- the two controls are allowed to overlap because a real transport's
/// components do.
constexpr float kWowRateLo = 0.2f;
constexpr float kWowRateHi = 7.0f;

/// Flutter, in Hz. The top is well past the weighting curve's passband and into
/// where it starts to read as texture rather than movement.
constexpr float kFlutterRateLo = 2.0f;
constexpr float kFlutterRateHi = 60.0f;

/// Seconds of tape per picture, at slider 0 and slider 1 respectively. Note the
/// order: the SLOW end is first, because the slider is a speed and the quantity
/// is its reciprocal. See Controls.h.
constexpr float kTapeSecondsSlow = 2.0f;
constexpr float kTapeSecondsFast = 0.002f;

/// Peak displacement as a fraction of picture width. A fifth of the frame is
/// far past anything a transport does and about right for where an effect
/// should stop being useful.
constexpr float kAmountMax = 0.20f;

/// Mistracking, in dB either side of correct. Type C is visibly breathing by 4
/// and unusable well before 12, so the control spends most of its travel in the
/// part worth having.
constexpr float kMistrackDb = 12.0f;

/// Hiss amplitude at full, on a 0..1 picture, and the exponent that gets the
/// bottom of the control usable. Linear hiss is either invisible or grainy
/// within a few percent of travel; the square puts the useful range in the
/// middle where a control's range should be.
constexpr float kHissMax = 0.15f;

/// Hiss correlation length in pixels, from a fresh head to a worn one.
constexpr float kHissWidthNew = 1.5f;
constexpr float kHissWidthOld = 6.0f;

/// Dropout probability at full. Higher than this and the picture is more
/// dropout than picture, which is a look but not one worth half a control's
/// travel.
constexpr float kDropoutMax = 0.05f;

/// Beat envelope decay exponent.
constexpr float kDecayLo = 1.0f;
constexpr float kDecayHi = 16.0f;

struct Division
{
	const char* label;
	float beats;
};

/// Divisions, in the dropdown's order. A quarter beat to two bars, which is the
/// range where a transport fault reads as musical rather than as an error.
constexpr Division kDivisions[] = {
	{ "1/4 Beat", 0.25f },
	{ "1/2 Beat", 0.5f },
	{ "Beat", 1.0f },
	{ "2 Beats", 2.0f },
	{ "Bar", 4.0f },
	{ "2 Bars", 8.0f },
};
constexpr int kDivisionCount = static_cast< int >( sizeof( kDivisions ) / sizeof( kDivisions[ 0 ] ) );
} // namespace

float expRange( float t, float lo, float hi )
{
	const float u = std::clamp( t, 0.0f, 1.0f );
	return lo * std::pow( hi / lo, u );
}

int option( float value, int elementCount )
{
	if( elementCount <= 0 )
		return 0;

	return std::clamp( static_cast< int >( std::lround( value ) ), 0, elementCount - 1 );
}

int divisionCount()
{
	return kDivisionCount;
}

const char* divisionLabel( int index )
{
	return kDivisions[ std::clamp( index, 0, kDivisionCount - 1 ) ].label;
}

float divisionValue( int index )
{
	return kDivisions[ std::clamp( index, 0, kDivisionCount - 1 ) ].beats;
}

drive::Settings driveSettings( const HostValues& host )
{
	drive::Settings s;
	s.sync         = option( host.sync, drive::kSyncCount );
	s.route        = option( host.route, drive::kRouteCount );
	s.beatDepth    = std::clamp( host.beatDepth, 0.0f, 1.0f );
	s.levelDepth   = std::clamp( host.levelDepth, 0.0f, 1.0f );
	s.bandDepth    = std::clamp( host.bandDepth, 0.0f, 1.0f );
	s.beatDecay    = expRange( host.beatDecay, kDecayLo, kDecayHi );
	s.beatDivision = divisionValue( option( host.division, kDivisionCount ) );
	return s;
}

Render render( const HostValues& host, int lines, const drive::Output& driveOut )
{
	Render r;

	//---------------------------------------------------------------------
	// Transport.
	//---------------------------------------------------------------------
	transport::Settings& t = r.transport;
	t.machine              = option( host.machine, transport::kMachineCount );

	// The drive's scale multiplies the DEPTHS and not the amount. Those are
	// different pictures: scaling the amount shrinks the whole error including
	// the components the operator wanted always on, while scaling the depths
	// hands over exactly the wow and flutter and leaves everything else where it
	// was put.
	const float scale = std::max( 0.0f, driveOut.scale );
	t.wow             = std::clamp( host.wow, 0.0f, 1.0f ) * scale;
	t.flutter         = std::clamp( host.flutter, 0.0f, 1.0f ) * scale;
	t.scrape          = std::clamp( host.scrape, 0.0f, 1.0f ) * scale;
	t.drift           = std::clamp( host.drift, 0.0f, 1.0f ) * scale;

	t.wowRate     = expRange( host.wowRate, kWowRateLo, kWowRateHi );
	t.flutterRate = expRange( host.flutterRate, kFlutterRateLo, kFlutterRateHi );

	// Backwards, deliberately. See Controls.h.
	t.secondsPerPicture = expRange( host.tapeSpeed, kTapeSecondsSlow, kTapeSecondsFast );

	t.lines    = std::max( 1, lines );
	t.amount   = std::clamp( host.amount, 0.0f, 1.0f ) * kAmountMax;
	t.vertical = std::clamp( host.vertical, 0.0f, 1.0f );
	t.lurch    = driveOut.lurch;

	//---------------------------------------------------------------------
	// Tape.
	//---------------------------------------------------------------------
	const float hiss = std::clamp( host.hiss, 0.0f, 1.0f );
	r.tape.hiss      = kHissMax * hiss * hiss;
	r.tape.headWear  = std::clamp( host.headWear, 0.0f, 1.0f );
	r.tape.hissWidth = kHissWidthNew + ( kHissWidthOld - kHissWidthNew ) * r.tape.headWear;

	const float drop = std::clamp( host.dropouts, 0.0f, 1.0f );
	r.tape.dropouts  = kDropoutMax * drop * drop;

	//---------------------------------------------------------------------
	// Noise reduction.
	//---------------------------------------------------------------------
	compander::Settings& nr = r.nr;
	nr.type                 = option( host.nrType, compander::kTypeCount );
	nr.mode                 = option( host.nrMode, compander::kModeCount );
	nr.strength             = std::clamp( host.nrStrength, 0.0f, 1.0f );

	// Centred: 0.5 is a correctly aligned deck. The band drive adds on top, so a
	// deck that was already off level goes further off rather than being
	// overridden -- an operator who dialled in a mistrack and then routed audio
	// to it should get both.
	nr.mistrackDb = ( std::clamp( host.mistracking, 0.0f, 1.0f ) - 0.5f ) * 2.0f * kMistrackDb
	                + driveOut.mistrackDb;

	const bool nrOn = nr.type != compander::kTypeOff;
	r.encode        = nrOn && nr.mode != compander::kModeDecodeOnly;
	r.decode        = nrOn && nr.mode != compander::kModeEncodeOnly;

	r.mix       = std::clamp( host.mix, 0.0f, 1.0f );
	r.showTrace = host.showTrace >= 0.5f;

	return r;
}

} // namespace controls
} // namespace ferric
