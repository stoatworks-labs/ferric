/**
 * Ferric for the browser: the plugin's CPU half, ported, and its three passes.
 *
 * Two halves, and they are not equally faithful here:
 *
 *   **The GPU half is the plugin's own GLSL.** `shaders.js` is the raw-string
 *   pieces of `source/shaders/*.cpp` plus `kHeader`, spliced by
 *   `demo/tools/check_shaders.py --write` and never typed; the same script
 *   compares them character for character, checks they are joined as
 *   `EncodeFragment()`, `TapeFragment()` and `DecodeFragment()` join them, and
 *   `tools/verify.sh` runs it. They run as the plugin's three passes -- encode
 *   into an RGBA16F buffer, the tape into another, decode/mix/trace into the
 *   canvas -- with the same buffers (linear, clamped).
 *
 *   **The CPU half is a port.** Between the PORT markers below: the parts of
 *   Transport.cpp the CPU runs (`phaseAt`, the hash and noise under it,
 *   `error`, `weightedPercent` and the machine table), Controls.cpp (every
 *   conversion, `tapeRates`, `wrapTapePhase`), Compander.cpp (`uniforms`),
 *   Drive.cpp (`compute`) and Presets.h. Then `FerricFrames`: the CPU half of
 *   Ferric.cpp's ProcessOpenGL -- the drive, the render settings, the frame's
 *   phases, the hiss and dropout clocks integrated over clamped frame deltas,
 *   the cached weighted figure -- as a function that RETURNS each pass's
 *   uniforms, so the GL code after the markers only binds and draws.
 *   demo/tools/crosscheck/ holds the PORT block to the plugin's own C++.
 *
 * The transport's wobble is not a random walk kept in state and not a seeded
 * generator. It is a function of the clock: `phaseAt()` turns the absolute time
 * into wrapped phases in double precision once a frame, and everything per
 * pixel -- two wow sines, two flutter sines, the once-per-revolution pulse,
 * scrape's value noise and drift's three-octave fbm, all on an integer hash --
 * is the shader's, evaluated within one picture. There is no seed: the noise is
 * the same noise in every instance, as in the plugin. JavaScript's numbers are
 * doubles, so `phaseAt` here is the plugin's arithmetic exactly; the floats it
 * narrows to are `Math.fround`, as the C++'s `static_cast< float >` is.
 *
 * Nothing in this file touches the DOM, so Node can import it (the crosscheck
 * does).
 */

import { Program, PassBuffer } from './vendor/gl.js';
import * as S from './shaders.js';

// ==== BEGIN PORT
const f32 = Math.fround;
const clamp = (v, lo, hi) => (v < lo ? lo : v > hi ? hi : v);
/** std::lround: half away from zero. */
const lround = (v) => (v < 0 ? -Math.round(-v) : Math.round(v));

//---------------------------------------------------------------------------
// Transport.cpp
//---------------------------------------------------------------------------
const kTau = f32(6.28318530717958647692);
const kTauD = 6.28318530717958647692;
const kScrapeHz = f32(220.0);
const kDriftHz = f32(0.13);
const kWowRatio = f32(0.37);
const kFlutterRatio = f32(2.13);
const kNoiseWrap = 65536.0;
const kRevMean = f32(0.2255859375);
const kRevNorm = f32(1.0 / 0.7744140625);

export const Machine = { Cassette: 0, Reel: 1, VideoHead: 2, Failing: 3 };

/** The machines: multipliers over the operator's controls (Transport.cpp kMachines). */
export const MACHINES = [
  //  label            wowRate      flutRate     wowG         flutG        scrapeG      driftG       periodic
  ['Cassette',        f32(1.00), f32(1.00), f32(1.00), f32(0.70), f32(0.80), f32(1.00), f32(0.15)],
  ['Reel to Reel',    f32(0.55), f32(1.80), f32(0.35), f32(0.45), f32(0.30), f32(0.50), f32(0.05)],
  ['Video Head',      f32(2.20), f32(3.20), f32(0.60), f32(0.50), f32(0.40), f32(0.35), f32(0.75)],
  ['Failing',         f32(0.75), f32(0.80), f32(1.90), f32(1.20), f32(1.30), f32(1.80), f32(0.85)],
].map(([label, wowRate, flutterRate, wowGain, flutterGain, scrapeGain, driftGain, periodic]) => ({
  label, wowRate, flutterRate, wowGain, flutterGain, scrapeGain, driftGain, periodic,
}));

export function machineProfile(machine) {
  return MACHINES[clamp(machine | 0, 0, MACHINES.length - 1)];
}

/** The integer hash. uint32 arithmetic, wrapped exactly as C++ and GLSL wrap it. */
export function hashU(v) {
  v = (Math.imul(v >>> 0, 747796405) + 2891336453) >>> 0;
  const w = Math.imul(((v >>> ((v >>> 28) + 4)) ^ v) >>> 0, 277803737) >>> 0;
  return ((w >>> 22) ^ w) >>> 0;
}

function hashF(i) {
  // static_cast< uint32_t >( i + 65536 ): a negative tape time wraps, it does not clamp.
  const h = hashU((i + 65536) >>> 0);
  return f32((h & 0xffffff) * (1.0 / 16777216.0));
}

/** Smooth 1-D value noise, -1..1, in float. */
export function valueNoise(x) {
  const fx = Math.floor(x);
  const i = fx | 0;
  const f = f32(x - fx);
  const u = f32(f32(f * f) * f32(3.0 - f32(2.0 * f)));
  const a = hashF(i);
  const b = hashF(i + 1);
  return f32(f32(f32(a + f32(f32(b - a) * u)) * 2.0) - 1.0);
}

function fbm(x) {
  const a = f32(0.5 * valueNoise(x));
  const b = f32(0.25 * valueNoise(f32(f32(x * f32(2.03)) + f32(11.1))));
  const c = f32(0.125 * valueNoise(f32(f32(x * f32(4.07)) + f32(23.7))));
  return f32(f32(f32(a + b) + c) * f32(1.0 / 0.875));
}

function wrapRadians(turns) {
  const frac = turns - Math.floor(turns);
  return f32(frac * kTauD);
}

function wrapNoise(coordinate) {
  return f32(coordinate - kNoiseWrap * Math.floor(coordinate / kNoiseWrap));
}

/** The frame's phases at an absolute tape time, in double, wrapped, then narrowed. */
export function phaseAt(s, tapeTime) {
  const m = machineProfile(s.machine);
  const fw = Math.max(0.01, s.wowRate * m.wowRate);
  const ff = Math.max(0.01, s.flutterRate * m.flutterRate);
  return {
    wow1: wrapRadians(fw * tapeTime),
    wow2: wrapRadians(fw * kWowRatio * tapeTime),
    flutter1: wrapRadians(ff * tapeTime),
    flutter2: wrapRadians(ff * kFlutterRatio * tapeTime),
    scrape: wrapNoise(tapeTime * kScrapeHz),
    drift: wrapNoise(tapeTime * kDriftHz),
  };
}

const sinf = (x) => f32(Math.sin(x));

/** transport::error -- the CPU side of the mirror. Only weightedPercent calls it here. */
export function error(s, p, dt) {
  const m = machineProfile(s.machine);
  const fw = Math.max(f32(0.01), f32(s.wowRate * m.wowRate));
  const ff = Math.max(f32(0.01), f32(s.flutterRate * m.flutterRate));
  const tfw = f32(f32(kTau * fw) * dt);

  const wowSine = f32(
    f32(f32(0.62) * sinf(f32(p.wow1 + tfw)))
    + f32(f32(0.38) * sinf(f32(f32(p.wow2 + f32(f32(f32(kTau * fw) * kWowRatio) * dt)) + f32(1.7)))),
  );

  const rev = f32(0.5 + f32(0.5 * sinf(f32(f32(p.wow1 + tfw) + f32(0.3)))));
  const rev2 = f32(rev * rev);
  const revPulse = f32(f32(f32(f32(rev2 * rev2) * rev2) - kRevMean) * kRevNorm);

  const wowSig = f32(wowSine + f32(m.periodic * f32(revPulse - wowSine)));
  const wowPart = f32(f32(wowSig * m.wowGain) * s.wow);

  const tff = f32(f32(kTau * ff) * dt);
  const flutSig = f32(
    f32(f32(0.70) * sinf(f32(f32(p.flutter1 + tff) + f32(0.9))))
    + f32(f32(0.30) * sinf(f32(f32(p.flutter2 + f32(f32(f32(kTau * ff) * kFlutterRatio) * dt)) + f32(2.4)))),
  );
  const flutPart = f32(f32(flutSig * m.flutterGain) * s.flutter);

  const scrapePart = f32(f32(valueNoise(f32(p.scrape + f32(kScrapeHz * dt))) * m.scrapeGain) * s.scrape);
  const driftPart = f32(f32(fbm(f32(p.drift + f32(kDriftHz * dt))) * m.driftGain) * s.drift);

  const slow = f32(wowPart + driftPart);
  return { slow, total: f32(f32(f32(slow + flutPart) + scrapePart) + s.lurch) };
}

const kWeightF0 = f32(3.53553390593);
const kWeightQ = f32(0.144307308);

/**
 * Weighted RMS wow and flutter, as a percentage of one line period: error()
 * sampled at 2 kHz for 8.19 s through the DIN 45507 band-pass, the settled part's
 * RMS. Lurch excluded. Sixteen thousand evaluations, so the caller caches it.
 */
export function weightedPercent(s) {
  const kRate = 2000;
  const kSamples = 16384;
  const kSettle = 4096;

  const clean = { ...s, lurch: 0 };

  const w0 = f32(f32(kTau * kWeightF0) / kRate);
  const alpha = f32(f32(Math.sin(w0)) / f32(2.0 * kWeightQ));
  const a0 = f32(1.0 + alpha);
  const b0 = f32(alpha / a0);
  const b2 = f32(-alpha / a0);
  const a1 = f32(f32(-2.0 * f32(Math.cos(w0))) / a0);
  const a2 = f32(f32(1.0 - alpha) / a0);

  let x1 = 0, x2 = 0, y1 = 0, y2 = 0;
  let sumSq = 0.0;

  for (let n = 0; n < kSamples; n += 1) {
    const t = n / kRate;
    const x = error(clean, phaseAt(clean, t), 0).total;
    const y = f32(f32(f32(f32(b0 * x) + f32(b2 * x2)) - f32(a1 * y1)) - f32(a2 * y2));
    x2 = x1; x1 = x; y2 = y1; y1 = y;
    if (n >= kSettle) sumSq += y * y;
  }

  const rms = Math.sqrt(sumSq / (kSamples - kSettle));
  return f32(f32(f32(f32(rms) * s.amount) * 100.0));
}

//---------------------------------------------------------------------------
// Compander.cpp
//---------------------------------------------------------------------------
export const NR_TYPES = ['Off', 'Type B', 'Type C'];
export const NR_MODES = ['Encode + Decode', 'Decode Only', 'Encode Only'];
const kTypeOff = 0;
const kModeDecodeOnly = 1;
const kModeEncodeOnly = 2;

const STAGES = [
  [{ threshold: f32(1.0), maxBoost: f32(0.0) }, { threshold: f32(1.0), maxBoost: f32(0.0) }], // Off
  [{ threshold: f32(0.060), maxBoost: f32(2.20) }, { threshold: f32(0.0), maxBoost: f32(0.0) }], // Type B
  [{ threshold: f32(0.060), maxBoost: f32(2.20) }, { threshold: f32(0.008), maxBoost: f32(2.60) }], // Type C
];
const STAGE_COUNT = [0, 1, 2];

/** compander::uniforms: the type, the stage table and the strength resolved away. */
export function companderUniforms(nr) {
  const st = STAGES[clamp(nr.type, 0, 2)];
  const active = STAGE_COUNT[clamp(nr.type, 0, 2)];
  const threshold = [f32(1.0), f32(1.0)];
  const boost = [f32(0.0), f32(0.0)];
  for (let stage = 0; stage < 2; stage += 1) {
    if (stage >= active) continue;
    threshold[stage] = Math.max(f32(1e-6), st[stage].threshold);
    boost[stage] = f32(st[stage].maxBoost * clamp(nr.strength, 0, 1));
  }
  return { threshold, boost, mistrack: f32(Math.pow(10.0, f32(nr.mistrackDb / 20.0))) };
}

//---------------------------------------------------------------------------
// Drive.cpp
//---------------------------------------------------------------------------
export const SYNC_LABELS = ['Free', 'Locked'];
export const ROUTE_LABELS = ['Natural', 'Inverted', 'Bass Only', 'Treble Only'];
export const kAudioBins = 64;
const kSyncFree = 0;
const kRouteInverted = 1;
const kRouteBass = 2;
const kRouteTreble = 3;
const kBassEnd = f32(1.0 / 16.0);
const kMidEnd = f32(1.0 / 4.0);
const kMaxLurch = f32(1.0);
const kMaxMistrackDb = f32(6.0);

function meanOver(bins, from, to) {
  if (to <= from) return 0;
  let sum = 0;
  for (let i = from; i < to; i += 1) sum = f32(sum + Math.max(0, bins[i]));
  return f32(sum / (to - from));
}

function bands(bins, binCount) {
  if (!bins || binCount <= 0) return { bass: 0, mid: 0, high: 0 };
  const n = binCount;
  const bassEnd = clamp(lround(f32(n * kBassEnd)), 1, binCount);
  const midEnd = clamp(lround(f32(n * kMidEnd)), bassEnd + 1, binCount);
  return { bass: meanOver(bins, 0, bassEnd), mid: meanOver(bins, bassEnd, midEnd), high: meanOver(bins, midEnd, binCount) };
}

/** drive::compute: the frame's audio and transport folded into what the render multiplies by. */
export function driveCompute(settings, input) {
  const out = { scale: 1, lurch: 0, mistrackDb: 0, ...bands(input.bins, input.binCount), level: 0, beat: 0 };
  out.level = input.bins && input.binCount > 0 ? meanOver(input.bins, 0, input.binCount) : 0;

  if (settings.sync !== kSyncFree) {
    const tempo = input.bpm > 1.0 ? input.bpm : 120.0;
    const barSeconds = 240.0 / tempo;
    const estimate = input.seconds / barSeconds;
    const within = clamp(input.barPhase, 0.0, 1.0);
    // std::round: half away from zero.
    const bars = within + lround(estimate - within);
    const beats = bars * 4.0;
    const division = Math.max(0.125, settings.beatDivision);
    const position = beats / division;
    const frac = position - Math.floor(position);
    const decay = clamp(settings.beatDecay, 1, 16);
    out.beat = f32(Math.pow(f32(1.0 - f32(frac)), decay));
  }

  const beatDepth = clamp(settings.beatDepth, 0, 1);
  const levelDepth = clamp(settings.levelDepth, 0, 1);
  out.scale = f32(f32(1.0 - levelDepth) + f32(levelDepth * clamp(out.level, 0, 1)));

  const bass = clamp(out.bass, 0, 1);
  const high = clamp(out.high, 0, 1);
  let lurchBand = bass;
  let mistrackBand = high;
  let mistrackSign = 1.0;
  if (settings.route === kRouteInverted) { lurchBand = high; mistrackBand = bass; mistrackSign = -1.0; }
  else if (settings.route === kRouteBass) { lurchBand = bass; mistrackBand = bass; }
  else if (settings.route === kRouteTreble) { lurchBand = high; mistrackBand = high; }

  if (beatDepth > 0) {
    out.lurch = f32(f32(f32(kMaxLurch * beatDepth) * out.beat) * f32(f32(0.35) + f32(f32(0.65) * lurchBand)));
  }
  const bandDepth = clamp(settings.bandDepth, 0, 1);
  if (bandDepth > 0) {
    out.mistrackDb = f32(f32(f32(kMaxMistrackDb * bandDepth) * mistrackSign) * clamp(mistrackBand, 0, 1));
  }
  return out;
}

//---------------------------------------------------------------------------
// Controls.cpp
//---------------------------------------------------------------------------
const kWowRateLo = f32(0.2);
const kWowRateHi = f32(7.0);
const kFlutterRateLo = f32(2.0);
const kFlutterRateHi = f32(60.0);
const kTapeSecondsSlow = f32(2.0);
const kTapeSecondsFast = f32(0.002);
const kAmountMax = f32(0.20);
const kMistrackDb = f32(12.0);
const kHissMax = f32(0.15);
const kHissWidthNew = f32(1.5);
const kHissWidthOld = f32(6.0);
const kDropoutMax = f32(0.05);
const kDecayLo = f32(1.0);
const kDecayHi = f32(16.0);
export const kDropBlocks = f32(24.0);

export const DIVISIONS = [
  ['1/4 Beat', 0.25], ['1/2 Beat', 0.5], ['Beat', 1.0], ['2 Beats', 2.0], ['Bar', 4.0], ['2 Bars', 8.0],
];

export function expRange(t, lo, hi) {
  const u = clamp(f32(t), 0, 1);
  return f32(lo * f32(Math.pow(f32(hi / lo), u)));
}

export function option(value, elementCount) {
  if (elementCount <= 0) return 0;
  return clamp(lround(value), 0, elementCount - 1);
}

/** controls::HostValues defaults, as the constructor writes them. */
export const HOST_DEFAULTS = Object.freeze({
  machine: 0, wow: f32(0.30), wowRate: f32(0.50), flutter: f32(0.45), flutterRate: f32(0.45),
  scrape: f32(0.12), drift: f32(0.12), tapeSpeed: f32(0.36), amount: f32(0.18), vertical: f32(0.20),
  hiss: f32(0.25), dropouts: 0, headWear: f32(0.20),
  nrType: 0, nrMode: 0, mistracking: f32(0.5), nrStrength: 1,
  sync: 0, beatDepth: 0, beatDecay: f32(0.45), division: 2, levelDepth: 0, bandDepth: 0, route: 0,
  showTrace: 0, mix: 1,
});

export function driveSettings(host) {
  return {
    sync: option(host.sync, 2),
    route: option(host.route, 4),
    beatDepth: clamp(host.beatDepth, 0, 1),
    levelDepth: clamp(host.levelDepth, 0, 1),
    bandDepth: clamp(host.bandDepth, 0, 1),
    beatDecay: expRange(host.beatDecay, kDecayLo, kDecayHi),
    beatDivision: f32(DIVISIONS[option(host.division, DIVISIONS.length)][1]),
  };
}

/** controls::render: everything the render needs, in physical units. */
export function controlsRender(host, lines, driveOut) {
  const scale = Math.max(0, driveOut.scale);
  const transport = {
    machine: option(host.machine, MACHINES.length),
    wow: f32(clamp(host.wow, 0, 1) * scale),
    flutter: f32(clamp(host.flutter, 0, 1) * scale),
    scrape: f32(clamp(host.scrape, 0, 1) * scale),
    drift: f32(clamp(host.drift, 0, 1) * scale),
    wowRate: expRange(host.wowRate, kWowRateLo, kWowRateHi),
    flutterRate: expRange(host.flutterRate, kFlutterRateLo, kFlutterRateHi),
    // Backwards, deliberately: the slider is a speed, the quantity its reciprocal.
    secondsPerPicture: expRange(host.tapeSpeed, kTapeSecondsSlow, kTapeSecondsFast),
    lines: Math.max(1, lines),
    amount: f32(clamp(host.amount, 0, 1) * kAmountMax),
    vertical: clamp(host.vertical, 0, 1),
    lurch: driveOut.lurch,
  };

  const hiss = clamp(host.hiss, 0, 1);
  const headWear = clamp(host.headWear, 0, 1);
  const drop = clamp(host.dropouts, 0, 1);
  const tape = {
    hiss: f32(f32(kHissMax * hiss) * hiss),
    headWear,
    hissWidth: f32(kHissWidthNew + f32(f32(kHissWidthOld - kHissWidthNew) * headWear)),
    dropouts: f32(f32(kDropoutMax * drop) * drop),
  };

  const nr = {
    type: option(host.nrType, NR_TYPES.length),
    mode: option(host.nrMode, NR_MODES.length),
    strength: clamp(host.nrStrength, 0, 1),
    mistrackDb: f32(f32(f32(f32(clamp(host.mistracking, 0, 1) - 0.5) * 2.0) * kMistrackDb) + driveOut.mistrackDb),
  };
  const nrOn = nr.type !== kTypeOff;

  return {
    transport,
    tape,
    nr,
    encode: nrOn && nr.mode !== kModeDecodeOnly,
    decode: nrOn && nr.mode !== kModeEncodeOnly,
    mix: clamp(host.mix, 0, 1),
    showTrace: host.showTrace >= 0.5,
  };
}

/** controls::tapeRates, in double. */
export function tapeRates(render, width, height) {
  const linesPerSecond = Math.max(1, height) / Math.max(1e-4, render.transport.secondsPerPicture);
  return {
    hiss: (linesPerSecond * Math.max(1, width)) / Math.max(0.5, render.tape.hissWidth),
    dropouts: linesPerSecond * 0.01,
  };
}

export function wrapTapePhase(phase) {
  const kWrap = 65536.0;
  return phase - kWrap * Math.floor(phase / kWrap);
}

//---------------------------------------------------------------------------
// Presets.h. Table-column order; option columns hold the element index.
//---------------------------------------------------------------------------
export const PRESET_COLUMNS = [
  'machine', 'wow', 'wowRate', 'flutter', 'flutterRate', 'scrape', 'drift', 'tapeSpeed', 'amount',
  'vertical', 'hiss', 'dropouts', 'headWear', 'nrType', 'nrMode', 'mistracking', 'nrStrength',
  'sync', 'beatDepth', 'levelDepth', 'bandDepth',
];

export const PRESETS = [
  //                    mach wow  wowR flut flutR scrp drft tapeS amt  vert hiss drop wear nrT nrM mis  str  sync beat lvl  band
  ['Clean Deck',       [1, .12, .45, .10, .50, .05, .08, .70, .10, .10, .12, .00, .08, 1, 0, .50, 1, 0, 0, 0, 0]],
  ['Compact Cassette', [0, .38, .48, .32, .45, .22, .25, .45, .28, .25, .35, .10, .30, 1, 0, .50, 1, 0, 0, 0, 0]],
  ['Chewed Tape',      [3, .85, .35, .55, .40, .45, .70, .35, .60, .45, .45, .45, .55, 0, 0, .50, 1, 0, 0, 0, 0]],
  ['Undecoded',        [0, .20, .50, .18, .45, .12, .15, .50, .16, .15, .40, .00, .25, 2, 1, .25, 1, 0, 0, 0, 0]],
  ['Wrong Deck',       [0, .30, .50, .26, .45, .18, .20, .48, .22, .18, .38, .05, .28, 2, 0, .79, 1, 0, 0, 0, 0]],
  ['Head Clog',        [0, .25, .50, .22, .45, .30, .20, .50, .18, .12, .55, .60, .80, 0, 0, .50, 1, 0, 0, 0, 0]],
  ['Ribbons',          [0, .30, .55, .80, .62, .35, .25, .12, .55, .20, .30, .15, .20, 0, 0, .50, 1, 0, 0, 0, 0]],
  ['Beat Slip',        [2, .18, .50, .20, .45, .10, .10, .55, .30, .30, .20, .12, .15, 1, 0, .50, 1, 1, .85, 0, 0]],
  ['Breathing',        [0, .22, .50, .18, .45, .12, .15, .48, .18, .15, .45, .00, .25, 2, 0, .50, 1, 1, 0, 0, .75]],
].map(([name, values]) => ({ name, values: values.map(f32) }));

/** The dropdown: element 0 is Custom and is not in the table. */
export const PRESET_ELEMENTS = ['Custom', ...PRESETS.map((p) => p.name)];

//---------------------------------------------------------------------------
// Ferric.cpp's ProcessOpenGL, the CPU half: everything up to the draws.
//---------------------------------------------------------------------------
const kMaxFrameDelta = 0.1;

/**
 * One instance's frame state. `frame()` takes the host values, the render size
 * and the clock in seconds, and returns each pass's uniforms. The page's host
 * hands over no audio (silence: every bin zero) and its own 120 BPM transport,
 * the tempo frtest drives the plugin at.
 */
export class FerricFrames {
  constructor() {
    this.hissPhase = 0.0;
    this.dropPhase = 0.0;
    this.lastHostTime = -1.0;
    this.audioLevel = new Float32Array(kAudioBins); // never filled: the page has no audio
    this.cachedWf = 0;
    this.cachedWfFor = null;
  }

  refreshWeighted(t) {
    const a = this.cachedWfFor;
    if (a && a.machine === t.machine && a.wow === t.wow && a.flutter === t.flutter && a.scrape === t.scrape
      && a.drift === t.drift && a.wowRate === t.wowRate && a.flutterRate === t.flutterRate && a.amount === t.amount) {
      return;
    }
    this.cachedWf = weightedPercent(t);
    this.cachedWfFor = { ...t };
  }

  frame(host, frameW, frameH, now, { bpm = 120, barPhase = null } = {}) {
    // The host's bar position. Resolume hands it over; the page computes it
    // from its own clock at its fixed tempo, as frtest does.
    const bars = now / (240.0 / bpm);
    const phaseInBar = barPhase ?? f32(bars - Math.floor(bars));

    const driveOut = driveCompute(driveSettings(host), {
      bins: this.audioLevel,
      binCount: this.audioLevel.length,
      bpm: f32(bpm),
      barPhase: phaseInBar,
      seconds: now,
    });

    const render = controlsRender(host, frameH, driveOut);
    const t = render.transport;
    const phase = phaseAt(t, now);
    const machine = machineProfile(t.machine);

    if (this.lastHostTime >= 0.0) {
      const delta = clamp(now - this.lastHostTime, 0.0, kMaxFrameDelta);
      const rates = tapeRates(render, frameW, frameH);
      this.hissPhase = wrapTapePhase(this.hissPhase + delta * rates.hiss);
      this.dropPhase = wrapTapePhase(this.dropPhase + delta * rates.dropouts);
    }
    this.lastHostTime = now;

    if (render.showTrace) this.refreshWeighted(t);

    const nr = companderUniforms(render.nr);
    const tbe = {
      TbeWowRate: f32(t.wowRate * machine.wowRate),
      TbeFlutRate: f32(t.flutterRate * machine.flutterRate),
      TbeWowGain: f32(machine.wowGain * t.wow),
      TbeFlutGain: f32(machine.flutterGain * t.flutter),
      TbeScrapeGain: f32(machine.scrapeGain * t.scrape),
      TbeDriftGain: f32(machine.driftGain * t.drift),
      TbePeriodic: machine.periodic,
      TbeLurch: t.lurch,
      TbePhaseWow1: phase.wow1,
      TbePhaseWow2: phase.wow2,
      TbePhaseFlut1: phase.flutter1,
      TbePhaseFlut2: phase.flutter2,
      TbePhaseScrape: phase.scrape,
      TbePhaseDrift: phase.drift,
      TbeSeconds: t.secondsPerPicture,
      TbeLines: f32(t.lines),
    };

    return {
      driveOut,
      render,
      phase,
      encode: {
        floats: { NrTexelX: f32(1.0 / frameW) },
        arrays: { NrThresh: nr.threshold, NrBoost: nr.boost },
        ints: { DoEncode: render.encode ? 1 : 0 },
      },
      tape: {
        floats: {
          ...tbe,
          TbeAmount: t.amount,
          TbeVertical: t.vertical,
          TbeAspectWH: f32(frameW / frameH),
          HissAmp: render.tape.hiss,
          HissWidth: render.tape.hissWidth,
          HissPhase: f32(this.hissPhase),
          DropoutP: render.tape.dropouts,
          DropBlocks: kDropBlocks,
          DropPhase: f32(this.dropPhase),
          HeadWear: render.tape.headWear,
          TexelX: f32(1.0 / frameW),
          PixelsPerLine: f32(frameW),
        },
        arrays: {},
        ints: {},
      },
      decode: {
        floats: {
          NrTexelX: f32(1.0 / frameW),
          NrMistrack: nr.mistrack,
          MixAmount: render.mix,
          ...tbe,
          TraceBass: driveOut.bass,
          TraceMid: driveOut.mid,
          TraceHigh: driveOut.high,
          TraceLevel: driveOut.level,
          TraceBeat: driveOut.beat,
          TraceWfPercent: this.cachedWf,
          TraceAspect: f32(frameW / frameH),
        },
        arrays: { NrThresh: nr.threshold, NrBoost: nr.boost },
        ints: {
          DoDecode: render.decode ? 1 : 0,
          ShowTrace: render.showTrace ? 1 : 0,
        },
      },
    };
  }
}
// ==== END PORT

/** The page's controls as the host holds them; anything the page lacks keeps its default. */
export function hostValues(params, hidden = {}) {
  const host = { ...HOST_DEFAULTS, ...hidden };
  for (const id of Object.keys(HOST_DEFAULTS)) {
    if (params.byId.has(id)) host[id] = f32(params.get(id));
  }
  return host;
}

//---------------------------------------------------------------------------
// The GL half: Ferric.cpp's three passes, binding and drawing only.
//---------------------------------------------------------------------------
const encodeFragment = S.HEADER + S.COMPAND_FUNCTIONS + S.ENCODE_MAIN;
const tapeFragment = S.HEADER + S.TBE_FUNCTIONS + S.TAPE_MAIN;
const decodeFragment = S.HEADER + S.COMPAND_FUNCTIONS + S.TBE_FUNCTIONS + S.TRACE_FUNCTIONS + S.DECODE_MAIN;

function apply(program, uniforms) {
  for (const [name, value] of Object.entries(uniforms.floats)) program.set(name, value);
  for (const [name, values] of Object.entries(uniforms.arrays)) program.setArray(name, new Float32Array(values), 1);
  for (const [name, value] of Object.entries(uniforms.ints)) program.setInt(name, value);
}

/**
 * @param {WebGL2RenderingContext} gl
 * @param {import('./vendor/gl.js').Quad} quad
 * @param {object} [options]
 * @param {() => object} [options.hidden] host values for parameters the page does not show
 */
export function createRenderer(gl, quad, { hidden = () => ({}) } = {}) {
  const programs = {
    encode: new Program(gl, S.VERTEX, encodeFragment, 'encode'),
    tape: new Program(gl, S.VERTEX, tapeFragment, 'tape'),
    decode: new Program(gl, S.VERTEX, decodeFragment, 'decode'),
  };

  // Ferric.h: the picture with the encode side applied, and after the medium.
  // RGBA16F, linear, clamped, as FFGLFBO allocates them.
  const encodeBuffer = new PassBuffer(gl, { filter: 'linear' });
  const tapeBuffer = new PassBuffer(gl, { filter: 'linear' });

  const frames = new FerricFrames();

  return {
    programs,
    frames,
    last: null,

    /**
     * `output` is a framebuffer to draw the last pass into instead of the
     * canvas; the crosscheck reads pixels back from one.
     */
    render({ input, params, width, height, time, output = null, host = null }) {
      const values = host ?? hostValues(params, hidden());
      const f = frames.frame(values, width, height, time);
      this.last = f;

      // Every allocation FIRST, before anything is bound (Ferric.cpp).
      encodeBuffer.ensure(width, height, gl.RGBA16F);
      tapeBuffer.ensure(width, height, gl.RGBA16F);

      gl.disable(gl.BLEND);

      // 1. Encode, into a buffer of ours. Runs whatever the NR is set to.
      encodeBuffer.bind();
      programs.encode.use();
      gl.activeTexture(gl.TEXTURE0);
      gl.bindTexture(gl.TEXTURE_2D, input.texture);
      programs.encode.set('MaxUV', 1.0, 1.0);
      programs.encode.setSampler('InputTexture', 0);
      programs.encode.set('InputMaxUV', 1.0, 1.0);
      apply(programs.encode, f.encode);
      quad.draw();

      // 2. The medium.
      tapeBuffer.bind();
      programs.tape.use();
      gl.activeTexture(gl.TEXTURE0);
      gl.bindTexture(gl.TEXTURE_2D, encodeBuffer.texture);
      programs.tape.set('MaxUV', 1.0, 1.0);
      programs.tape.setSampler('TapeTexture', 0);
      apply(programs.tape, f.tape);
      quad.draw();

      // 3. Decode, mix and overlay, straight into whatever the host handed us.
      gl.bindFramebuffer(gl.FRAMEBUFFER, output);
      gl.viewport(0, 0, width, height);
      programs.decode.use();
      gl.activeTexture(gl.TEXTURE0);
      gl.bindTexture(gl.TEXTURE_2D, tapeBuffer.texture);
      gl.activeTexture(gl.TEXTURE1);
      gl.bindTexture(gl.TEXTURE_2D, input.texture);
      gl.activeTexture(gl.TEXTURE0);
      programs.decode.set('MaxUV', 1.0, 1.0);
      programs.decode.setSampler('TapeTexture', 0);
      programs.decode.setSampler('InputTexture', 1);
      programs.decode.set('InputMaxUV', 1.0, 1.0);
      apply(programs.decode, f.decode);
      quad.draw();

      gl.activeTexture(gl.TEXTURE1);
      gl.bindTexture(gl.TEXTURE_2D, null);
      gl.activeTexture(gl.TEXTURE0);
      gl.bindTexture(gl.TEXTURE_2D, null);
    },
  };
}
