#include "Compander.h"

#include <algorithm>
#include <cmath>

namespace ferric
{
namespace compander
{
namespace
{
/// Type B: one stage, about +10 dB on quiet detail.
///
/// maxBoost is a multiplier on the high-passed band, so 2.2 adds a band 2.2
/// times its own size back on top of the signal -- roughly 10 dB of that band
/// where the band is all there is, which is what "10 dB of noise reduction"
/// means when the noise is all in the band.
///
/// The threshold is in the same units the side chain measures in: mean absolute
/// high-passed value over the neighbourhood, on a 0..1 picture. 0.06 puts the
/// half-boost point at the level of ordinary film grain, so grain gets the
/// treatment and a hard graphic edge does not.
constexpr Stage kStagesB[ kMaxStages ] = {
	{ 0.060f, 2.20f },
	{ 0.0f, 0.0f },
};

/// Type C: two stages, and the second is the one that gives it its reputation.
///
/// The first is close to Type B. The second works an order quieter -- threshold
/// 0.008, which is a few code values -- and adds another 2.6 on top of whatever
/// the first already did. That compounding is why the system reaches 20 dB and
/// why it mistracks so visibly: a level error the first stage barely notices
/// puts the second stage on completely the wrong part of its curve.
constexpr Stage kStagesC[ kMaxStages ] = {
	{ 0.060f, 2.20f },
	{ 0.008f, 2.60f },
};

constexpr Stage kOff[ kMaxStages ] = {
	{ 1.0f, 0.0f },
	{ 1.0f, 0.0f },
};

const char* const kTypeLabels[ kTypeCount ] = { "Off", "Type B", "Type C" };
const char* const kModeLabels[ kModeCount ] = { "Encode + Decode", "Decode Only", "Encode Only" };
} // namespace

int typeCount()
{
	return kTypeCount;
}

const char* typeLabel( int type )
{
	return kTypeLabels[ std::clamp( type, 0, static_cast< int >( kTypeCount ) - 1 ) ];
}

int modeCount()
{
	return kModeCount;
}

const char* modeLabel( int mode )
{
	return kModeLabels[ std::clamp( mode, 0, static_cast< int >( kModeCount ) - 1 ) ];
}

int stageCount( int type )
{
	switch( type )
	{
		case kTypeB: return 1;
		case kTypeC: return 2;
		default: return 0;
	}
}

const Stage* stages( int type )
{
	switch( type )
	{
		case kTypeB: return kStagesB;
		case kTypeC: return kStagesC;
		default: return kOff;
	}
}

float encodeGain( const Stage& stage, float level, float strength )
{
	// A reciprocal, not an exponential. This is a compressor's law: at zero
	// level the full boost, falling to nothing as the material gets loud enough
	// to hide the noise on its own. The half-boost point is the threshold, by
	// construction -- level == threshold gives maxBoost/2 -- which is what makes
	// the threshold a number somebody can reason about instead of a coefficient.
	const float t = std::max( 1e-6f, stage.threshold );
	const float l = std::max( 0.0f, level );
	return stage.maxBoost * std::clamp( strength, 0.0f, 1.0f ) / ( 1.0f + l / t );
}

float decodeGain( float encode )
{
	// (I + gH)^-1 = I - g/(1+g) H, exactly, for a projector H. See Compander.h.
	return encode / ( 1.0f + encode );
}

float mistrackFactor( float db )
{
	return std::pow( 10.0f, db / 20.0f );
}

float slide( const Stage& stage, float level )
{
	const float t = std::max( 1e-6f, stage.threshold );
	const float l = std::max( 0.0f, level );
	return l / ( l + t );
}

Uniforms uniforms( const Settings& s )
{
	const Stage* st  = stages( s.type );
	const int active = stageCount( s.type );

	Uniforms u;
	u.mistrack = mistrackFactor( s.mistrackDb );

	for( int stage = 0; stage < kMaxStages; ++stage )
	{
		if( stage >= active )
			continue;

		u.threshold[ stage ] = std::max( 1e-6f, st[ stage ].threshold );
		u.boost[ stage ]     = st[ stage ].maxBoost * std::clamp( s.strength, 0.0f, 1.0f );
	}

	return u;
}

} // namespace compander
} // namespace ferric
