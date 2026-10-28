// The JavaScript half of the browser demo's port check: demo/ferric.js's port,
// driven through the same scenario as printer.cpp and printing the same lines,
// so the two can be compared exactly.
//
//     node demo/tools/crosscheck/frames.mjs < scenario.txt > js.txt
//
// The page's renderer is not involved: FerricFrames.frame() RETURNS every
// uniform, which is what the GL code binds.

import { readFileSync } from 'node:fs';
import { dirname, join } from 'node:path';
import { fileURLToPath, pathToFileURL } from 'node:url';

const here = dirname(fileURLToPath(import.meta.url));
const F = await import(pathToFileURL(join(here, '..', '..', 'ferric.js')).href);

const view = new DataView(new ArrayBuffer(8));
const f = (v) => { view.setFloat32(0, v); return view.getUint32(0).toString(16).padStart(8, '0'); };
const d = (v) => {
  view.setFloat64(0, v);
  return view.getUint32(0).toString(16).padStart(8, '0') + view.getUint32(4).toString(16).padStart(8, '0');
};
const fromHex32 = (hex) => { view.setUint32(0, parseInt(hex, 16)); return view.getFloat32(0); };
const fromHex64 = (hex) => {
  view.setUint32(0, parseInt(hex.slice(0, 8), 16));
  view.setUint32(4, parseInt(hex.slice(8), 16));
  return view.getFloat64(0);
};
const b = (v) => (v ? 1 : 0);

const out = [];
let frames = new F.FerricFrames();
let host = { ...F.HOST_DEFAULTS };
let width = 960;
let height = 540;
let frameNo = 0;

const TBE = ['TbeWowRate', 'TbeFlutRate', 'TbeWowGain', 'TbeFlutGain', 'TbeScrapeGain', 'TbeDriftGain', 'TbePeriodic',
  'TbeLurch', 'TbePhaseWow1', 'TbePhaseWow2', 'TbePhaseFlut1', 'TbePhaseFlut2', 'TbePhaseScrape', 'TbePhaseDrift',
  'TbeSeconds', 'TbeLines'];

for (const line of readFileSync(0, 'utf8').split('\n')) {
  const words = line.trim().split(/\s+/);
  const verb = words[0];
  if (!verb) continue;
  if (verb === 'new') {
    frames = new F.FerricFrames();
    host = { ...F.HOST_DEFAULTS };
    frameNo = 0;
    out.push('N');
  } else if (verb === 'size') {
    width = Number(words[1]);
    height = Number(words[2]);
    out.push(`S ${width}x${height}`);
  } else if (verb === 'set') {
    if (!(words[1] in F.HOST_DEFAULTS)) throw new Error(`no host field ${words[1]}`);
    host[words[1]] = fromHex32(words[2]);
  } else if (verb === 'preset') {
    const element = Number(words[1]);
    const preset = F.PRESETS[element - 1];
    if (preset) F.PRESET_COLUMNS.forEach((id, j) => { host[id] = preset.values[j]; });
    out.push(`P ${element} ${F.PRESET_ELEMENTS[element] ?? 'Custom'}`);
  } else if (verb === 'frame') {
    const now = fromHex64(words[1]);
    const r = frames.frame(host, width, height, now);
    const dr = r.driveOut;
    const t = r.render.transport;
    out.push(`D ${frameNo} scale=${f(dr.scale)} lurch=${f(dr.lurch)} mistrackDb=${f(dr.mistrackDb)} bass=${f(dr.bass)} mid=${f(dr.mid)} high=${f(dr.high)} level=${f(dr.level)} beat=${f(dr.beat)}`);
    out.push(`C ${frameNo} hissPhase=${d(frames.hissPhase)} dropPhase=${d(frames.dropPhase)} encode=${b(r.render.encode)} decode=${b(r.render.decode)} showTrace=${b(r.render.showTrace)} machine=${t.machine} lines=${t.lines} nrType=${r.render.nr.type} nrMode=${r.render.nr.mode} wowRate=${f(t.wowRate)} flutterRate=${f(t.flutterRate)} seconds=${f(t.secondsPerPicture)} hissWidth=${f(r.render.tape.hissWidth)}`);
    const e = r.encode;
    out.push(`U ${frameNo} encode NrTexelX=${f(e.floats.NrTexelX)} NrThresh=${e.arrays.NrThresh.map(f).join(',')} NrBoost=${e.arrays.NrBoost.map(f).join(',')} DoEncode=${e.ints.DoEncode}`);
    const tp = r.tape.floats;
    const tbe = TBE.map((k) => ` ${k}=${f(tp[k])}`).join('');
    out.push(`U ${frameNo} tape${tbe} TbeAmount=${f(tp.TbeAmount)} TbeVertical=${f(tp.TbeVertical)} TbeAspectWH=${f(tp.TbeAspectWH)} HissAmp=${f(tp.HissAmp)} HissWidth=${f(tp.HissWidth)} HissPhase=${f(tp.HissPhase)} DropoutP=${f(tp.DropoutP)} DropBlocks=${f(tp.DropBlocks)} DropPhase=${f(tp.DropPhase)} HeadWear=${f(tp.HeadWear)} TexelX=${f(tp.TexelX)} PixelsPerLine=${f(tp.PixelsPerLine)}`);
    const dc = r.decode;
    const dtbe = TBE.map((k) => ` ${k}=${f(dc.floats[k])}`).join('');
    out.push(`U ${frameNo} decode NrTexelX=${f(dc.floats.NrTexelX)} NrMistrack=${f(dc.floats.NrMistrack)} MixAmount=${f(dc.floats.MixAmount)} NrThresh=${dc.arrays.NrThresh.map(f).join(',')} NrBoost=${dc.arrays.NrBoost.map(f).join(',')} DoDecode=${dc.ints.DoDecode} ShowTrace=${dc.ints.ShowTrace}${dtbe} TraceBass=${f(dc.floats.TraceBass)} TraceMid=${f(dc.floats.TraceMid)} TraceHigh=${f(dc.floats.TraceHigh)} TraceLevel=${f(dc.floats.TraceLevel)} TraceBeat=${f(dc.floats.TraceBeat)} TraceWfPercent=${f(dc.floats.TraceWfPercent)} TraceAspect=${f(dc.floats.TraceAspect)}`);
    frameNo += 1;
  } else if (verb === 'machines') {
    F.MACHINES.forEach((p, m) => {
      out.push(`M ${m} ${p.label} ${[p.wowRate, p.flutterRate, p.wowGain, p.flutterGain, p.scrapeGain, p.driftGain, p.periodic].map(f).join(' ')}`);
    });
  } else if (verb === 'labels') {
    const list = (name, items) => out.push(`L ${name}${items.map((x) => ` |${x}`).join('')}`);
    list('machine', F.MACHINES.map((m) => m.label));
    list('nrType', F.NR_TYPES);
    list('nrMode', F.NR_MODES);
    list('sync', F.SYNC_LABELS);
    list('division', F.DIVISIONS.map(([label]) => label));
    list('route', F.ROUTE_LABELS);
    list('preset', F.PRESET_ELEMENTS);
    const h = F.HOST_DEFAULTS;
    const order = ['machine', 'wow', 'wowRate', 'flutter', 'flutterRate', 'scrape', 'drift', 'tapeSpeed', 'amount',
      'vertical', 'hiss', 'dropouts', 'headWear', 'nrType', 'nrMode', 'mistracking', 'nrStrength', 'sync', 'beatDepth',
      'beatDecay', 'division', 'levelDepth', 'bandDepth', 'route', 'showTrace', 'mix'];
    out.push(`H ${order.map((k) => `${k}=${f(h[k])}`).join(' ')}`);
    F.PRESETS.forEach((p, i) => out.push(`T ${i + 1} ${p.values.map(f).join(' ')}`));
  } else {
    throw new Error(`unknown verb ${verb}`);
  }
}

process.stdout.write(`${out.join('\n')}\n`);
