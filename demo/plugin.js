/**
 * Ferric — browser demo.
 *
 * Puts the picture on tape. The two ideas, from `AGENTS.md`: **wow and flutter
 * is a timing error, and a picture read off tape is a signal with a clock** --
 * every pixel has a tape time and one error signal is evaluated there -- and
 * **noise reduction is a round trip through a medium**, so the chain is encode,
 * then the tape, then decode, and everything recognisable about the format is
 * the two ends disagreeing.
 *
 * `ferric.js` holds the port and the passes and says which is which. This file
 * is the page: the controls as the constructor declares them, the plugin's own
 * Preset parameter, and a status line.
 *
 * ---------------------------------------------------------------------------
 * Decisions this page made, and why
 * ---------------------------------------------------------------------------
 *
 * **The audio-fed half of the Reaction group is absent.** `Audio` is an
 * FF_USAGE_FFT buffer Resolume fills with a spectrum; a browser has no
 * equivalent, and asking for a microphone to demonstrate a video effect is not
 * a trade worth making. So Audio, Level Depth, Band Depth and Route are not on
 * the panel rather than present and dead. The removal is exact: with silence
 * every band and the level are zero, so Band Depth pushes the decoder by
 * nothing, Route has nothing to route, and Level Depth at its default of zero
 * leaves the transport alone. They are held at their defaults -- or, where a
 * factory preset writes them (Breathing writes Band Depth), at what it wrote,
 * which with silence changes nothing either.
 *
 * **The tempo-fed half is here.** Sync, Beat Depth, Beat Decay and Division
 * read the host's tempo and bar position, not audio. The page is the host, so
 * it supplies them: 120 BPM, the bar position from the page's clock, which is
 * what `frtest` drives the plugin at. With no audio the lurch on each beat is
 * the plugin's floor for silence -- `0.35 x` Beat Depth, the "a grid is a grid"
 * term in Drive.cpp -- where a kick drum in Resolume takes it up to the full
 * depth. Beat Slip is the preset that shows it.
 *
 * **The Preset parameter is the plugin's own**, at the bottom where the plugin
 * declares it, with its ten elements. Choosing one writes its values into the
 * controls it covers; moving any covered control drops it back to Custom and
 * keeps the edit. That is the plugin's rule (Presets.h). Here the sliders move
 * to show the preset; in Resolume they do not, because Resolume ignores value
 * events -- the picture is the same either way. The kit's own preset menu is
 * not used, so there is one list, the plugin's.
 *
 * **The About block is not a parameter here**, as on every page in the suite.
 */

import { mountDemo } from './vendor/demo.js';
import {
  createRenderer, expRange, MACHINES, NR_TYPES, NR_MODES, SYNC_LABELS, DIVISIONS,
  PRESETS, PRESET_COLUMNS, PRESET_ELEMENTS, HOST_DEFAULTS,
} from './ferric.js';

const fix = (v, n) => v.toFixed(n);
const hz = (v) => (v < 10 ? `${fix(v, 2)} Hz` : `${fix(v, 1)} Hz`);

// Controls.cpp's ranges, for the readouts beside the sliders. The conversions
// themselves are ferric.js's port of the plugin's functions.
const seconds = (t) => {
  const s = expRange(t, Math.fround(2.0), Math.fround(0.002));
  return s < 1 ? `${fix(s * 1000, s < 0.01 ? 1 : 0)} ms` : `${fix(s, 2)} s`;
};

/** In the constructor's order and groups, with its names and defaults. */
const SPEC = [
  // Transport
  ['machine', 'Machine', 'Transport', 'option', MACHINES.map((m) => m.label), null,
    'Multiplies your controls rather than replacing them, and sets the character: Cassette wanders, Reel to Reel is an order better and faster, Video Head is periodic, Failing has a flat spot on an idler.'],
  ['wow', 'Wow', 'Transport', 'standard', null, (v) => fix(v, 2),
    'Slower than a picture, so the whole frame leans and breathes together.'],
  ['wowRate', 'Wow Rate', 'Transport', 'standard', null, (v) => hz(expRange(v, Math.fround(0.2), Math.fround(7.0))),
    '0.2 to 7 Hz, before the Machine’s multiplier.'],
  ['flutter', 'Flutter', 'Transport', 'standard', null, (v) => fix(v, 2),
    'Fits a cycle or two into a picture, so it draws a wave down the image that scrolls between frames. This is the one that looks like tape.'],
  ['flutterRate', 'Flutter Rate', 'Transport', 'standard', null, (v) => hz(expRange(v, Math.fround(2.0), Math.fround(60.0))),
    '2 to 60 Hz, before the Machine’s multiplier.'],
  ['scrape', 'Scrape', 'Transport', 'standard', null, (v) => fix(v, 2),
    'Scrape flutter at 220 Hz: many cycles a picture, so fine horizontal banding.'],
  ['drift', 'Drift', 'Transport', 'standard', null, (v) => fix(v, 2),
    'A slow wander over seconds, below what a wow-and-flutter meter counts.'],
  ['tapeSpeed', 'Tape Speed', 'Transport', 'standard', null, seconds,
    'Seconds of tape one picture occupies (the readout), and it runs backwards: faster is fewer seconds. The control that matters. A real 625-line scan would make flutter a constant across the frame, so this is the fiction, stated.'],
  ['amount', 'Amount', 'Transport', 'standard', null, (v) => `${fix(v * 20, 1)}% of width`,
    'Displacement at an error of 1, as a fraction of picture width. Scales every component.'],
  ['vertical', 'Vertical', 'Transport', 'standard', null, (v) => fix(v, 2),
    'How much of the slow part (wow and drift) also rolls the frame. Flutter never does: a sync separator has a time constant.'],
  // Tape
  ['hiss', 'Hiss', 'Tape', 'standard', null, (v) => `±${fix(15 * v * v, 2)}%`,
    'Along the scan, so it comes out as horizontal grain. Not zero by default: noise reduction with no noise to reduce does nothing.'],
  ['dropouts', 'Dropouts', 'Tape', 'standard', null, (v) => `${fix(5 * v * v, 2)}% of blocks`,
    'A run of a scanline losing contact with the head, tapered at both ends.'],
  ['headWear', 'Head Wear', 'Tape', 'standard', null, (v) => fix(v, 2),
    'High frequencies lost along the scan, and the hiss widened with them.'],
  // Noise Reduction
  ['nrType', 'NR Type', 'Noise Reduction', 'option', NR_TYPES, null,
    'Type B is one band and forgiving; Type C is two, an order quieter and far less forgiving.'],
  ['nrMode', 'NR Mode', 'Noise Reduction', 'option', NR_MODES, null,
    'Encode + Decode is the honest chain and nearly transparent. Decode Only is dull and breathing; Encode Only is hard and glassy.'],
  ['mistracking', 'Mistracking', 'Noise Reduction', 'standard', null, (v) => {
    const db = (v - 0.5) * 2 * 12;
    return `${db >= 0 ? '+' : '−'}${fix(Math.abs(db), 1)} dB`;
  }, 'The decoder’s level error, centred at correct. Off centre, the round trip cancels at some brightnesses and not others, and vertical edges pump.'],
  ['nrStrength', 'NR Strength', 'Noise Reduction', 'standard', null, (v) => fix(v, 2),
    'Multiplies every stage’s boost. No real deck has this knob.'],
  // Reaction: the tempo-fed half (see the header)
  ['sync', 'Sync', 'Reaction', 'option', SYNC_LABELS, null,
    'Locked follows the host’s transport. Here that is the page’s own 120 BPM. In Free, Beat Depth does nothing.'],
  ['beatDepth', 'Beat Depth', 'Reaction', 'standard', null, (v) => fix(v, 2),
    'A decaying lurch into the error signal on the grid: the belt slipping. With no audio here it is the plugin’s floor for silence, 0.35 of this; a kick drum in Resolume takes it to the full depth.'],
  ['beatDecay', 'Beat Decay', 'Reaction', 'standard', null, (v) => `exponent ${fix(expRange(v, Math.fround(1), Math.fround(16)), 1)}`,
    '1 is a linear ramp down over the division, 16 a click.'],
  ['division', 'Division', 'Reaction', 'option', DIVISIONS.map(([label]) => label), null,
    'Which musical division the lurch fires on.'],
  // Output
  ['showTrace', 'Show Trace', 'Output', 'boolean', null, null,
    'The error signal down the picture (the shape the image is torn by), the five meters, and weighted wow and flutter as a percentage of a line period. The audio meters read zero here; the beat meter moves when Sync is Locked.'],
  ['mix', 'Mix', 'Output', 'standard', null, (v) => fix(v, 2),
    'The whole effect against the clip. The trace is drawn over the mix.'],
  // Preset
  ['preset', 'Preset', 'Preset', 'option', PRESET_ELEMENTS, null,
    'The plugin’s factory presets. Choosing one writes the controls it covers; moving any of them drops it back to Custom. Breathing is built on audio, so here it renders as it would in Resolume with nothing routed.'],
];

const PARAMS = SPEC.map(([id, name, group, type, elements, display, hint]) => {
  const p = { id, name, group, type, hint, default: id === 'preset' ? 0 : HOST_DEFAULTS[id] };
  if (type === 'option') p.elements = elements;
  if (display) p.display = display;
  return p;
});

// The covered controls the page has no row for: written by a preset, read by
// the port, never shown. See the header.
const SHOWN = new Set(PARAMS.map((p) => p.id));
const hidden = {};
const resetHidden = () => {
  for (const id of PRESET_COLUMNS) if (!SHOWN.has(id)) hidden[id] = HOST_DEFAULTS[id];
};
resetHidden();

let renderer = null;

const mounted = mountDemo({
  name: 'Ferric',
  pluginId: 'FR01',
  tagline:
    'Put the picture on tape. Wow and flutter is not a wobble applied to a picture: it is a timing error applied to a signal, and a picture read off tape is a signal with a clock. So every pixel has a tape time, and its displacement is one error signal evaluated there. Wow leans the frame, flutter draws a wave down it that scrolls, scrape bands it. The oxide adds hiss and drops out, and the sliding-band noise reduction that hid one under the other is here too, both ends of it, which is where the interesting damage lives.',
  repo: 'https://github.com/stoatworks-labs/ferric',
  page: 'https://stoatworks-labs.com/software/ferric/',
  video: 'https://www.youtube.com/watch?v=PccB5tL7rsQ',
  kind: 'effect',

  blurb:
    'It is Ferric’s own three GLSL passes (encode, the tape, decode), ported from the repository to WebGL2 unedited and running on generated clips in this page, fed by a JavaScript port of the plugin’s CPU half: the frame’s transport phases computed in double from the clock, every control’s conversion, the compander’s stage table, the beat envelope and the weighted wow-and-flutter figure. There is no audio here, so the Audio buffer and the three controls it feeds are absent; the beat controls run on the page’s own 120 BPM.',

  // RGBA16F between passes, as in the plugin: WebGL2 renders to it only with
  // EXT_color_buffer_float, and the page stops and says so without it.
  needFloat: true,

  // The geometry card first: its grid shows the lean, the wave scrolling down
  // the picture and the lines stretching. Bars and fine detail are where the
  // compander shows (vertical edges pump, horizontal ones sit still).
  sources: ['grid', 'scene', 'bars', 'detail', 'ramp', 'spot'],

  params: PARAMS,

  differences: [
    'The CPU half is a PORT, not the plugin’s code. The plugin computes each frame’s transport phases on the CPU in double and wraps them (Transport.cpp’s phaseAt), converts every control (Controls.cpp), resolves the compander’s stage table (Compander.cpp), runs the reaction (Drive.cpp), integrates the hiss and dropout clocks over clamped frame deltas, and measures the weighted wow-and-flutter figure the trace prints (sixteen thousand evaluations of the error signal through a DIN 45507 band-pass). Here all of that is JavaScript. The wobble is not a random walk or a seeded generator in either: it is a function of the clock, the same in every instance.',
    'A check in the repository runs the port and the plugin’s own C++ side by side over the same scenario (every slider across its travel, every dropdown, every preset for two seconds, Resolume’s half-million-second clock, the beat at every division, three hundred random states) and compares 550,194 values: 548,532 are identical to the bit. The rest are libm’s last bit (Apple’s powf against JavaScript’s, about one slider position in 170) and the phases that one bit moves when the clock reads half a million seconds.',
    'Frames drawn by this page’s renderer were compared with the plugin’s own harness (frtest --pipe, the real plugin class in a GL 4.1 context) on its test card at 640 × 360 after 60 frames, on the same Mac’s GPU. In eight of nine cases every pixel is within one level of 255. In the ninth, Head Clog, 92.5% are and none is more than three out: the plugin built for Apple Silicon fuses one multiply-add in the hiss width, the port (like the plugin’s Intel build) does not, so the hiss width is one bit apart, the hiss travels at a rate one part in ten million different, and after a second it has drifted a fraction of a grain. On SwiftShader a handful of single pixels per case (five at most of 230,400) land on the other side of a hard edge.',
    'The GPU half is not a port: the encode, tape and decode passes are the plugin’s own GLSL, on the same RGBA16F buffers (WebGL2 needs EXT_color_buffer_float to render into them, and the page stops and says so without it). demo/tools/check_shaders.py fails the repository’s verify script if a character of them, or the way they are joined, drifts.',
    'No audio. Audio, Level Depth, Band Depth and Route are absent rather than present and dead: the spectrum reaches the plugin as a Resolume FFT buffer and a browser has no equivalent. The removal is exact, because with silence the plugin’s bands and level are zero and those three controls change nothing at their defaults. Show Trace’s bass, mid, high and level meters read zero, as they do in Resolume with nothing routed and in the OpenFX build.',
    'The tempo is the page’s. Sync, Beat Depth, Beat Decay and Division follow the host’s tempo and bar position; this host is the page, at a fixed 120 BPM with the bar position taken from its clock, as the plugin’s offline harness drives it. With no audio, a beat lurches by 0.35 of Beat Depth, the plugin’s own floor for silence; in Resolume a kick drum takes it up to the whole depth.',
    'The Preset parameter is the plugin’s own table and rule: choosing one writes the controls it covers, and moving one of those drops back to Custom. Here the sliders move to show it; Resolume ignores value events, so there they stay put and the picture changes all the same. Breathing pushes the decoder with treble, so with no audio it renders as it would in Resolume with nothing routed: a Type C round trip that does not breathe.',
    'The clock is the page’s: seconds from frame deltas, capped at a tenth of a second a frame, handed over already in seconds, so the plugin’s vote on whether its host sends seconds or milliseconds is not ported. Paused, no time passes, so the wave stops scrolling and the hiss holds still; Step is one frame of a sixtieth of a second. Restart takes the clock back to zero, which the plugin treats as a scrub: the transport lands where it was at zero, and the hiss does not jump.',
    'Scanlines are the render’s rows, as in the plugin, so changing Composition changes how many lines the tear is quantised to and how far the hiss moves a second.',
    'The About block is not on the panel.',
  ],

  createRenderer: (gl, quad) => {
    renderer = createRenderer(gl, quad, { hidden: () => hidden });
    return renderer;
  },
});

//---------------------------------------------------------------------------
// The Preset parameter (Presets.h): an override of the covered controls, and
// a real edit of one drops back to Custom.
//---------------------------------------------------------------------------
if (mounted) {
  const { params } = mounted;
  let applying = false;

  const applyPreset = (element) => {
    const preset = PRESETS[element - 1];
    if (!preset) return;
    applying = true;
    try {
      PRESET_COLUMNS.forEach((id, j) => {
        if (SHOWN.has(id)) params.set(id, preset.values[j]);
        else hidden[id] = preset.values[j];
      });
    } finally {
      applying = false;
    }
  };

  let lastPreset = Math.round(params.get('preset'));

  params.addEventListener('change', (event) => {
    const id = event.detail?.id;
    if (applying) return;
    if (id === null) {
      // Defaults, or a link: a whole state. A link naming a preset gets its
      // values, which a consistent state already has.
      lastPreset = Math.round(params.get('preset'));
      if (lastPreset === 0) resetHidden();
      else applyPreset(lastPreset);
      return;
    }
    if (id === 'preset') {
      const chosen = Math.round(params.get('preset'));
      if (chosen !== lastPreset && chosen > 0) applyPreset(chosen);
      lastPreset = chosen;
      return;
    }
    if (lastPreset > 0 && PRESET_COLUMNS.includes(id)) {
      lastPreset = 0;
      params.set('preset', 0);
    }
  });

  // mountDemo has already read the URL by now.
  if (lastPreset > 0) applyPreset(lastPreset);

  //-------------------------------------------------------------------------
  // A status line: reports what the port computed, measures nothing.
  //-------------------------------------------------------------------------
  const stage = document.querySelector('.stage');
  if (stage) {
    const line = document.createElement('p');
    line.className = 'stage__status';
    line.id = 'ferric-stats';
    line.setAttribute('aria-live', 'off');
    stage.append(line);
    setInterval(() => {
      const f = renderer?.last;
      if (!f) return;
      const t = f.render.transport;
      const parts = [
        MACHINES[t.machine].label,
        `${t.secondsPerPicture < 1 ? `${(t.secondsPerPicture * 1000).toFixed(t.secondsPerPicture < 0.01 ? 1 : 0)} ms` : `${t.secondsPerPicture.toFixed(2)} s`} of tape a picture`,
        `${t.lines} lines`,
        `NR ${NR_TYPES[f.render.nr.type]}${f.render.nr.type ? `, ${NR_MODES[f.render.nr.mode]}` : ''}`,
      ];
      if (Math.round(params.get('sync')) === 1) parts.push(`120 BPM, beat ${f.driveOut.beat.toFixed(2)}`);
      if (f.render.showTrace) parts.push(`weighted W&F ${renderer.frames.cachedWf.toFixed(3)}% of a line period`);
      line.textContent = parts.join(' · ');
    }, 250);
  }

  // For the headless checks in AGENTS.md: the page's pieces, read-only by convention.
  window.ferricDemo = { mounted, get renderer() { return renderer; }, PARAMS, hidden };
}
