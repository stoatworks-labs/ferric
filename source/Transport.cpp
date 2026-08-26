#include "Transport.h"

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace ferric
{
namespace transport
{
namespace
{
constexpr float kTau  = 6.28318530717958647692f;
constexpr double kTauD = 6.28318530717958647692;

/// Scrape flutter's centre rate, Hz. High enough that a picture at the default
/// Tape Speed carries twenty-odd cycles of it, which is what makes it read as
/// fine horizontal banding rather than as more flutter.
constexpr float kScrapeHz = 220.0f;

/// The random walk's rate, Hz. Below the weighting curve's passband on purpose:
/// Drift is meant to be the thing that moves the picture around over seconds
/// without registering as wow, which is exactly what a wow-and-flutter meter
/// says about a slow speed error.
constexpr float kDriftHz = 0.13f;

/// Ratio of the second wow component to the first, and of the second flutter
/// component to its first. Neither is a harmonic: a wow built from one sine, or
/// from two in a rational ratio, repeats -- and a repeating wow reads as a
/// pendulum, which is the one thing a worn transport does not do.
constexpr float kWowRatio     = 0.37f;
constexpr float kFlutterRatio = 2.13f;

/// Where the noise coordinates wrap. Large enough that the wrap is invisible --
/// the noise either side of it is uncorrelated, so a seam would look like one
/// more grain of scrape -- and small enough that a float still resolves
/// individual pixels within it. See Transport.h.
constexpr double kNoiseWrap = 65536.0;

/// Mean of ((1 + sin) / 2)^6 over a cycle, and the reciprocal of its peak
/// excursion above that mean.
///
/// Written out rather than computed because they are the closed form:
/// E[(1+s)^6]/64 with E[s^even] = 1, 1/2, 3/8, 5/16 comes to 14.4375/64. The
/// pulse is shifted by the mean so the once-per-revolution component adds no
/// DC -- a lurch that also pushed the picture permanently sideways would read
/// as a centring bug -- and scaled by the reciprocal so it peaks at 1 like
/// every other component.
///
/// Both appear as the same literals in shaders/Tbe.cpp. The division is written
/// out there too rather than folded, so that both sides round it identically.
constexpr float kRevMean = 0.2255859375f;
constexpr float kRevNorm = 1.0f / 0.7744140625f;

//= mirrored in shaders/Tbe.cpp -- integer hash, value noise and fbm
//
// An INTEGER hash, not the `fract( sin(x) * 43758.5453 )` idiom the shader
// literature is built on. That idiom cannot be mirrored: sin() is a library
// function on the CPU and a hardware approximation on the GPU, they disagree in
// the last few bits, and fract() of a number scaled by forty thousand turns
// those last few bits into the whole answer. The two sides would produce
// completely different noise from identical source, and `--tbe` would fail with
// no pattern to it.
//
// This one is exact on both: uint arithmetic wraps identically in C++ and in
// GLSL, and the result is masked to 24 bits before the float conversion so the
// conversion is exact too rather than rounding.
uint32_t hashU( uint32_t v )
{
	v          = v * 747796405u + 2891336453u;
	uint32_t w = ( ( v >> ( ( v >> 28 ) + 4u ) ) ^ v ) * 277803737u;
	return ( w >> 22 ) ^ w;
}

float hashF( int i )
{
	// Biased into positive territory before the cast because a scrub backwards
	// gives a negative tape time, and that is a thing operators do constantly.
	const uint32_t h = hashU( static_cast< uint32_t >( i + 65536 ) );
	return static_cast< float >( h & 0xFFFFFFu ) * ( 1.0f / 16777216.0f );
}

/// Smooth 1-D value noise, -1..1.
float valueNoise( float x )
{
	const float fx = std::floor( x );
	const int i    = static_cast< int >( fx );
	const float f  = x - fx;
	const float u  = f * f * ( 3.0f - 2.0f * f );

	const float a = hashF( i );
	const float b = hashF( i + 1 );
	return ( a + ( b - a ) * u ) * 2.0f - 1.0f;
}

/// Three octaves of it, normalised to -1..1.
float fbm( float x )
{
	return ( 0.5f * valueNoise( x )
	         + 0.25f * valueNoise( x * 2.03f + 11.1f )
	         + 0.125f * valueNoise( x * 4.07f + 23.7f ) )
	       * ( 1.0f / 0.875f );
}
//= end mirrored

/// The machines. Multipliers over the operator's controls, never replacements
/// for them -- see Transport.h.
constexpr MachineProfile kMachines[ kMachineCount ] = {
	//  label            wowRate flutRate  wowG   flutG  scrapeG driftG periodic
	{ "Cassette",         1.00f,  1.00f,   1.00f, 0.70f, 0.80f,  1.00f, 0.15f },
	{ "Reel to Reel",     0.55f,  1.80f,   0.35f, 0.45f, 0.30f,  0.50f, 0.05f },
	{ "Video Head",       2.20f,  3.20f,   0.60f, 0.50f, 0.40f,  0.35f, 0.75f },
	{ "Failing",          0.75f,  0.80f,   1.90f, 1.20f, 1.30f,  1.80f, 0.85f },
};

//---------------------------------------------------------------------------
// The weighting curve.
//
// One second-order band-pass section, placed so that its -3 dB points land on
// 0.5 Hz and 25 Hz exactly. For a band-pass those two frequencies fix
// everything: the centre is their geometric mean and Q is the centre over the
// bandwidth.
//
//     f0 = sqrt( 0.5 * 25 )      = 3.5355 Hz
//     Q  = f0 / ( 25 - 0.5 )     = 0.14431
//
// Which puts the maximum at 3.54 Hz where DIN 45507 names 4 Hz. That is not a
// discrepancy worth fixing: at Q = 0.144 the curve is so broad that 4 Hz sits
// within 0.01 dB of the peak, and moving the centre to 4 would push the -3 dB
// points off the two frequencies the standard is actually specific about.
//---------------------------------------------------------------------------
constexpr float kWeightF0 = 3.53553390593f;
constexpr float kWeightQ  = 0.144307308f;

/// Wrap a phase in double precision, then narrow. The order matters: narrowing
/// first is the bug this whole structure exists to avoid.
float wrapRadians( double turns )
{
	const double frac = turns - std::floor( turns );
	return static_cast< float >( frac * kTauD );
}

float wrapNoise( double coordinate )
{
	const double wrapped = coordinate - kNoiseWrap * std::floor( coordinate / kNoiseWrap );
	return static_cast< float >( wrapped );
}
} // namespace

const MachineProfile& machineProfile( int machine )
{
	return kMachines[ std::clamp( machine, 0, static_cast< int >( kMachineCount ) - 1 ) ];
}

int machineCount()
{
	return kMachineCount;
}

const char* machineLabel( int machine )
{
	return machineProfile( machine ).label;
}

Phase phaseAt( const Settings& s, double tapeTime )
{
	const MachineProfile& m = machineProfile( s.machine );

	const double fw = std::max( 0.01, static_cast< double >( s.wowRate ) * m.wowRate );
	const double ff = std::max( 0.01, static_cast< double >( s.flutterRate ) * m.flutterRate );

	Phase p;
	p.wow1     = wrapRadians( fw * tapeTime );
	p.wow2     = wrapRadians( fw * kWowRatio * tapeTime );
	p.flutter1 = wrapRadians( ff * tapeTime );
	p.flutter2 = wrapRadians( ff * kFlutterRatio * tapeTime );
	p.scrape   = wrapNoise( tapeTime * kScrapeHz );
	p.drift    = wrapNoise( tapeTime * kDriftHz );
	return p;
}

//= mirrored in shaders/Tbe.cpp
Error error( const Settings& s, const Phase& p, float dt )
{
	const MachineProfile& m = machineProfile( s.machine );

	const float fw = std::max( 0.01f, s.wowRate * m.wowRate );
	const float ff = std::max( 0.01f, s.flutterRate * m.flutterRate );

	// Every argument is a wrapped phase plus a within-picture advance. Neither
	// term is ever large. See Transport.h.
	const float wowSine = 0.62f * std::sin( p.wow1 + kTau * fw * dt )
	                      + 0.38f * std::sin( p.wow2 + kTau * fw * kWowRatio * dt + 1.7f );

	// The once-per-revolution lurch: a narrow pulse at the wow rate, for the
	// flat spot on the idler. Sixth power by repeated multiplication rather
	// than pow() -- pow is a library call on one side of the mirror and a
	// hardware approximation on the other, and three multiplies are exact on
	// both.
	const float rev      = 0.5f + 0.5f * std::sin( p.wow1 + kTau * fw * dt + 0.3f );
	const float rev2     = rev * rev;
	const float revPulse = ( rev2 * rev2 * rev2 - kRevMean ) * kRevNorm;

	// Machines blend between the two characters rather than choosing one, which
	// is why Video Head still wanders slightly and Cassette still has a trace of
	// once-per-revolution in it.
	const float wowSig  = wowSine + m.periodic * ( revPulse - wowSine );
	const float wowPart = wowSig * m.wowGain * s.wow;

	const float flutSig = 0.70f * std::sin( p.flutter1 + kTau * ff * dt + 0.9f )
	                      + 0.30f * std::sin( p.flutter2 + kTau * ff * kFlutterRatio * dt + 2.4f );
	const float flutPart = flutSig * m.flutterGain * s.flutter;

	const float scrapePart = valueNoise( p.scrape + kScrapeHz * dt ) * m.scrapeGain * s.scrape;
	const float driftPart  = fbm( p.drift + kDriftHz * dt ) * m.driftGain * s.drift;

	Error e;
	// Vertical hold follows the slow part only. Drift belongs in it and scrape
	// does not: a sync separator has a time constant, and anything above field
	// rate averages out of it.
	e.slow  = wowPart + driftPart;
	e.total = e.slow + flutPart + scrapePart + s.lurch;
	return e;
}

float pictureTapeOffset( const Settings& s, float u, float v )
{
	const float lines = static_cast< float >( std::max( 1, s.lines ) );

	// Quantised to a whole scanline before the vertical term and left
	// continuous in the horizontal one. Both halves matter: without the floor
	// the error varies smoothly down the picture and the result is a warp
	// rather than a scan, and without the continuous `u` every line is a rigid
	// offset and lines never stretch.
	const float line = std::floor( v * lines );

	return ( line / lines ) * s.secondsPerPicture
	       + u * ( s.secondsPerPicture / lines );
}
//= end mirrored

float weighting( float hz )
{
	// Continuous-time magnitude of a second-order band-pass, which is what the
	// curve is defined as. The sampled version used by weightedPercent() is the
	// bilinear transform of this same section; they are the same filter.
	const float x = hz / kWeightF0;
	const float a = x / kWeightQ;
	return a / std::sqrt( ( 1.0f - x * x ) * ( 1.0f - x * x ) + a * a );
}

float weightedPercent( const Settings& s )
{
	// A real meter demodulates the recovered tone and puts the deviation
	// through the weighting network. This does the same thing to the same
	// signal, which is the only way to get a figure that covers scrape and the
	// once-per-revolution pulse's harmonics as well as the sines.
	constexpr int kRate    = 2000;  ///< Hz. Comfortably above scrape at 220.
	constexpr int kSamples = 16384; ///< 8.19 s, so 0.5 Hz gets four cycles.
	constexpr int kSettle  = 4096;  ///< Discarded while the filter fills.

	// The transport's own figure, so the audio drive's lurch comes out.
	Settings clean = s;
	clean.lurch    = 0.0f;

	// RBJ constant-peak-gain band-pass, bilinear transformed at kRate.
	const float w0    = kTau * kWeightF0 / static_cast< float >( kRate );
	const float alpha = std::sin( w0 ) / ( 2.0f * kWeightQ );
	const float a0    = 1.0f + alpha;
	const float b0    = alpha / a0;
	const float b2    = -alpha / a0;
	const float a1    = ( -2.0f * std::cos( w0 ) ) / a0;
	const float a2    = ( 1.0f - alpha ) / a0;

	float x1 = 0.0f, x2 = 0.0f, y1 = 0.0f, y2 = 0.0f;
	double sumSq = 0.0;

	for( int n = 0; n < kSamples; ++n )
	{
		// Through phaseAt each time rather than advancing dt, so the
		// measurement runs the same path the render does -- a figure measured
		// through a shortcut the plugin does not take would be measuring
		// something else.
		const double t = static_cast< double >( n ) / static_cast< double >( kRate );
		const float x  = error( clean, phaseAt( clean, t ), 0.0f ).total;

		const float y = b0 * x + b2 * x2 - a1 * y1 - a2 * y2;
		x2            = x1;
		x1            = x;
		y2            = y1;
		y1            = y;

		if( n >= kSettle )
			sumSq += static_cast< double >( y ) * static_cast< double >( y );
	}

	const double rms = std::sqrt( sumSq / static_cast< double >( kSamples - kSettle ) );

	// Against one line period. See Transport.h on why there is no tape speed
	// here for it to be a percentage of.
	return static_cast< float >( rms ) * s.amount * 100.0f;
}

} // namespace transport
} // namespace ferric
