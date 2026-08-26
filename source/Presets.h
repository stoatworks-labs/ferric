#pragma once

namespace ferric
{
/**
    Factory presets, in the host-facing 0..1 space.

    One table, host-agnostic, so the FFGL build and the OpenFX build that
    follows it cannot disagree about what a preset means. Element 0 of the
    dropdown is always "Custom" and is NOT in this table.

    ---------------------------------------- a preset is an OVERRIDE, not a write

    ☠️ **The obvious implementation does not work in Resolume, and it fails in a
    way that looks like the dropdown is broken.** Copying a preset's values into
    `params[]` and raising value events assumes the host consumes those events.
    Resolume does not. It goes on pushing the values it still believes in --
    the ones from before the preset -- as ordinary `SetFloatParameter` calls, and
    any "an operator edited a covered control, so drop to Custom" rule fires on
    the host's own echo, immediately, every time. The dropdown snaps back to
    Custom and nothing changes. This cost the fleet a shipped release and an
    external bug report.

    So the plugin keeps two things apart: `params[]`, what the render uses, and
    `hostValues[]`, **what the host last sent**. A restatement from the host that
    matches what a preset already put there is not written; a genuine edit
    differs and is. The judgement is on what the value **is**, never on the fact
    that it changed.

    The tolerance for that comparison is a **quantisation allowance, 1e-3**, and
    not a float epsilon. Resolume rounds to about a thousandth on the way
    through, so 1e-4 reads a rounded echo of our own value as an edit and the
    bug comes straight back.

    ------------------------------------------------------- what is not covered

    Mix, Show Trace and the Audio buffer. Mix is a compositing decision that
    belongs to whoever is running the show; Show Trace is a diagnostic; the
    audio parameter is a routing choice the host owns.

    The **reaction depths are covered**, which is a departure from most of the
    fleet, and deliberate: this is a sound-reactive plugin, and a preset list
    where none of the presets ever demonstrates that is a preset list that hides
    half the plugin. Seven of the nine leave every depth at zero, so a preset
    picked with nothing routed still behaves; the two that do not have it in
    their names.
*/
namespace presets
{
/// The parameters a preset covers, in table-column order. The FFGL binding of
/// these to real ParamIDs lives in Ferric.h, so this table stays host-agnostic.
enum Param
{
	P_MACHINE,
	P_WOW,
	P_WOW_RATE,
	P_FLUTTER,
	P_FLUTTER_RATE,
	P_SCRAPE,
	P_DRIFT,
	P_TAPE_SPEED,
	P_AMOUNT,
	P_VERTICAL,
	P_HISS,
	P_DROPOUTS,
	P_HEAD_WEAR,
	P_NR_TYPE,
	P_NR_MODE,
	P_MISTRACKING,
	P_NR_STRENGTH,
	P_SYNC,
	P_BEAT_DEPTH,
	P_LEVEL_DEPTH,
	P_BAND_DEPTH,
	kParamCount
};

struct Preset
{
	const char* name;
	float values[ kParamCount ];
};

/// ⚠️ Option parameters hold their ELEMENT INDEX here, not a 0..1 fraction --
/// Machine 3 means Failing, not "three quarters of the way along". Booleans, if
/// any are ever covered, must be exactly 0 or 1: the fleet has already shipped a
/// preset that set one to 0.35, where the FFGL shader read 0.35 and the OpenFX
/// build read `true`, and the two builds rendered different pictures from the
/// same preset.
///
/// Mistracking is centred, so 0.5 is a correctly aligned deck; 0.25 is -6 dB and
/// 0.79 is about +7.
inline constexpr Preset kPresets[] = {
	//                    mach  wow  wowR  flut flutR scrp drft tapeS amt  vert hiss drop wear nrT  nrM  mis  str  sync beat lvl  band
	{ "Clean Deck",      { 1.f, .12f, .45f, .10f, .50f, .05f, .08f, .70f, .10f, .10f, .12f, .00f, .08f, 1.f, 0.f, .50f, 1.f, 0.f, 0.f, 0.f, 0.f } },
	{ "Compact Cassette",{ 0.f, .38f, .48f, .32f, .45f, .22f, .25f, .45f, .28f, .25f, .35f, .10f, .30f, 1.f, 0.f, .50f, 1.f, 0.f, 0.f, 0.f, 0.f } },
	{ "Chewed Tape",     { 3.f, .85f, .35f, .55f, .40f, .45f, .70f, .35f, .60f, .45f, .45f, .45f, .55f, 0.f, 0.f, .50f, 1.f, 0.f, 0.f, 0.f, 0.f } },
	{ "Undecoded",       { 0.f, .20f, .50f, .18f, .45f, .12f, .15f, .50f, .16f, .15f, .40f, .00f, .25f, 2.f, 1.f, .25f, 1.f, 0.f, 0.f, 0.f, 0.f } },
	{ "Wrong Deck",      { 0.f, .30f, .50f, .26f, .45f, .18f, .20f, .48f, .22f, .18f, .38f, .05f, .28f, 2.f, 0.f, .79f, 1.f, 0.f, 0.f, 0.f, 0.f } },
	{ "Head Clog",       { 0.f, .25f, .50f, .22f, .45f, .30f, .20f, .50f, .18f, .12f, .55f, .60f, .80f, 0.f, 0.f, .50f, 1.f, 0.f, 0.f, 0.f, 0.f } },
	{ "Ribbons",         { 0.f, .30f, .55f, .80f, .62f, .35f, .25f, .12f, .55f, .20f, .30f, .15f, .20f, 0.f, 0.f, .50f, 1.f, 0.f, 0.f, 0.f, 0.f } },
	{ "Beat Slip",       { 2.f, .18f, .50f, .20f, .45f, .10f, .10f, .55f, .30f, .30f, .20f, .12f, .15f, 1.f, 0.f, .50f, 1.f, 1.f, .85f, 0.f, 0.f } },
	{ "Breathing",       { 0.f, .22f, .50f, .18f, .45f, .12f, .15f, .48f, .18f, .15f, .45f, .00f, .25f, 2.f, 0.f, .50f, 1.f, 1.f, 0.f, 0.f, .75f } },
};

inline constexpr int kCount = static_cast< int >( sizeof( kPresets ) / sizeof( kPresets[ 0 ] ) );

/// Dropdown element count, including Custom.
inline constexpr int elementCount()
{
	return kCount + 1;
}

/// Dropdown label. 0 is Custom.
inline const char* label( int element )
{
	if( element <= 0 || element > kCount )
		return "Custom";

	return kPresets[ element - 1 ].name;
}

/// A preset's values. `element` is 1-based; 0 (Custom) has none.
inline const float* values( int element )
{
	if( element <= 0 || element > kCount )
		return nullptr;

	return kPresets[ element - 1 ].values;
}

/// The comparison tolerance. A host-quantisation allowance, not a float
/// epsilon. See the header.
inline constexpr float kEchoTolerance = 1e-3f;

} // namespace presets
} // namespace ferric
