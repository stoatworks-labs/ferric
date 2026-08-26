#include "Drive.h"

#include <algorithm>
#include <cmath>

namespace ferric
{
namespace drive
{
namespace
{
/// Band edges as fractions of the bin count. Logarithmic rather than equal
/// thirds -- see the header for why equal thirds puts the entire mix in "bass".
///
/// Bass is the bottom sixteenth, mid the next quarter, treble everything above.
/// With 64 bins that is bins 0-3, 4-15 and 16-63, about two octaves each if the
/// buffer is linear in frequency.
constexpr float kBassEnd = 1.0f / 16.0f;
constexpr float kMidEnd  = 1.0f / 4.0f;

/// The deepest excursion a beat may add to the error signal, in the error's own
/// dimensionless units. One whole unit is the same size as a fully-wound wow, so
/// a lurch at full depth is a transport briefly losing its grip rather than a
/// modulation of one that has not.
constexpr float kMaxLurch = 1.0f;

/// The furthest a band may push the decoder off level, in dB. Six is well past
/// where Type C starts breathing visibly and nowhere near where the picture
/// falls apart, which is the range worth having a control over.
constexpr float kMaxMistrackDb = 6.0f;

const char* const kSyncLabels[ kSyncCount ]   = { "Free", "Locked" };
const char* const kRouteLabels[ kRouteCount ] = { "Natural", "Inverted", "Bass Only", "Treble Only" };

float meanOver( const float* bins, int from, int to )
{
	if( to <= from )
		return 0.0f;

	float sum = 0.0f;
	for( int i = from; i < to; ++i )
		sum += std::max( 0.0f, bins[ i ] );

	return sum / static_cast< float >( to - from );
}
} // namespace

int syncCount()
{
	return kSyncCount;
}

const char* syncLabel( int index )
{
	return kSyncLabels[ std::clamp( index, 0, static_cast< int >( kSyncCount ) - 1 ) ];
}

int routeCount()
{
	return kRouteCount;
}

const char* routeLabel( int index )
{
	return kRouteLabels[ std::clamp( index, 0, static_cast< int >( kRouteCount ) - 1 ) ];
}

void bands( const float* bins, int binCount, float* outBass, float* outMid, float* outHigh )
{
	*outBass = 0.0f;
	*outMid  = 0.0f;
	*outHigh = 0.0f;

	if( bins == nullptr || binCount <= 0 )
		return;

	const float n = static_cast< float >( binCount );

	// At least one bin per band however few the host supplies, and the edges
	// kept in order, so a host handing over eight bins still gets three distinct
	// bands rather than two empty ones and a mean of everything.
	const int bassEnd = std::clamp( static_cast< int >( std::lround( n * kBassEnd ) ), 1, binCount );
	const int midEnd  = std::clamp( static_cast< int >( std::lround( n * kMidEnd ) ), bassEnd + 1, binCount );

	*outBass = meanOver( bins, 0, bassEnd );
	*outMid  = meanOver( bins, bassEnd, midEnd );
	*outHigh = meanOver( bins, midEnd, binCount );
}

Output compute( const Settings& settings, const Input& in )
{
	Output out;

	bands( in.bins, in.binCount, &out.bass, &out.mid, &out.high );

	// The level is the mean of every bin, not the mean of the three bands: the
	// bands cover wildly different numbers of bins, so averaging them would
	// weight four bass bins the same as forty-eight treble ones and the "level"
	// would follow the kick drum -- which is what Bass Only is for, and is not
	// what an overall level should mean.
	out.level = in.bins != nullptr && in.binCount > 0
	                ? meanOver( in.bins, 0, in.binCount )
	                : 0.0f;

	//---------------------------------------------------------------------
	// The beat envelope.
	//---------------------------------------------------------------------
	if( settings.sync != kSyncFree )
	{
		// The host gives a tempo and a position within the current bar, never
		// which bar it is. Recover a continuous count without keeping any state:
		// the clock estimates how many bars have passed, barPhase is the exact
		// position inside this one, and the whole number reconciling them is
		// round( estimate - barPhase ). Continuous across the bar line, because
		// as barPhase wraps from 1 to 0 the rounded integer steps up at the same
		// instant, and exact for as long as the clock estimate stays within half
		// a bar of the truth. The same recovery the rest of the fleet uses.
		const double tempo      = in.bpm > 1.0f ? static_cast< double >( in.bpm ) : 120.0;
		const double barSeconds = 240.0 / tempo;// four beats to the bar
		const double estimate   = in.seconds / barSeconds;
		const double within     = std::clamp( static_cast< double >( in.barPhase ), 0.0, 1.0 );

		const double bars  = within + std::round( estimate - within );
		const double beats = bars * 4.0;

		const double division = std::max( 0.125, static_cast< double >( settings.beatDivision ) );
		const double position = beats / division;

		// Fractional part: 0 at the instant the division lands, approaching 1
		// just before the next. std::floor and not a cast -- a cast truncates
		// toward zero, so every division before the host's zero point would ramp
		// the wrong way, and a host reporting a negative transport position is a
		// scrub backwards, which operators do constantly.
		const double frac = position - std::floor( position );

		const float decay = std::clamp( settings.beatDecay, 1.0f, 16.0f );
		out.beat          = std::pow( 1.0f - static_cast< float >( frac ), decay );
	}

	//---------------------------------------------------------------------
	// Depth: the transport's own wow and flutter, handed over to the level.
	//
	// Carved out of the depths rather than added on top, so Level Depth 1 means
	// silence runs clean and a loud passage runs the full setting -- which is
	// what anybody means by a control called Depth. Clamped as a SUM: two
	// sources at 0.8 would otherwise leave the always-on part at -0.6, and a
	// negative scale does not saturate, it inverts the error and the picture
	// leans the wrong way.
	//---------------------------------------------------------------------
	const float beatDepth  = std::clamp( settings.beatDepth, 0.0f, 1.0f );
	const float levelDepth = std::clamp( settings.levelDepth, 0.0f, 1.0f );

	out.scale = ( 1.0f - levelDepth ) + levelDepth * std::clamp( out.level, 0.0f, 1.0f );

	//---------------------------------------------------------------------
	// The lurch.
	//
	// Beat depth drives this and NOT `scale`, which is the one structural
	// difference from the rest of the fleet's drives and is deliberate. Scaling
	// the transport's depth on a beat modulates a signal that is already
	// running, so the picture merely gets more of what it already had; adding an
	// excursion puts a discontinuity in, which is what a slipping belt does.
	// They look nothing alike, and only the second one reads as mechanical.
	//---------------------------------------------------------------------
	const float bass = std::clamp( out.bass, 0.0f, 1.0f );
	const float high = std::clamp( out.high, 0.0f, 1.0f );

	float lurchBand    = bass;
	float mistrackBand = high;
	float mistrackSign = 1.0f;

	switch( settings.route )
	{
		case kRouteInverted:
			lurchBand    = high;
			mistrackBand = bass;
			// The decoder is pushed the other way too, so the picture dulls on
			// the bass instead of brightening on it. Inverted has to invert both
			// halves or it is just a different routing.
			mistrackSign = -1.0f;
			break;

		case kRouteBass:
			lurchBand    = bass;
			mistrackBand = bass;
			break;

		case kRouteTreble:
			lurchBand    = high;
			mistrackBand = high;
			break;

		case kRouteNatural:
		default:
			break;
	}

	// The beat envelope shapes the lurch and the band decides how hard. With no
	// band energy at all a beat still lurches -- a grid is a grid -- so the band
	// only ever adds, which is why it is `0.35 + 0.65 * band` and not `band`.
	if( beatDepth > 0.0f )
		out.lurch = kMaxLurch * beatDepth * out.beat * ( 0.35f + 0.65f * lurchBand );

	const float bandDepth = std::clamp( settings.bandDepth, 0.0f, 1.0f );
	if( bandDepth > 0.0f )
		out.mistrackDb = kMaxMistrackDb * bandDepth * mistrackSign * std::clamp( mistrackBand, 0.0f, 1.0f );

	return out;
}

} // namespace drive
} // namespace ferric
