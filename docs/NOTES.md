# Notes

Working notes for this repo: status, decisions, and the traps that have actually
bitten. Written in the first person and dated by when each thing was learned —
that date is usually the useful part.

Cross-cutting notes that are not specific to this repo live in
[fleet-notes](https://github.com/stoatworks-labs/fleet-notes).

*ferric — the video signal as an analogue tape signal: wow and flutter as a
time-base error, plus a sliding-band noise-reduction round trip. FFGL effect,
`FR01`. Built 2026-08-26 and tested the same day in Arena 7.27.1 on macOS and on
Windows.*

## Status, 2026-08-26

**ferric** (`~/Projects/resolume/ferric`, started 2026-08-26, MIT, intended
**PUBLIC** at stoatworks-labs/ferric, **not yet pushed and not yet registered in
the backend**). One FFGL 2.1 effect. Builds universal, 23 checks in
`tools/verify.sh` all pass, all 27 controls reach the picture, and the bundle
instantiates through `plugMain` and renders under `ffgltest`.

**Asked for as "casette", renamed to "walkman" in the same message, and shipped
as ferric.** Walkman is a live Sony trademark and this fleet publishes public
repos with releases and project videos, which is the one context where that is a
real risk rather than a theoretical one. Capstan and Ferric were both offered;
Ferric was chosen. Worth remembering that the naming question is cheap to answer
before the repo exists and expensive afterwards — the repo, the four-character
ID, both bundle identifiers, the log directory and every document carry it.

**The same question applies to the noise reduction and was answered the same
way.** The dropdown says `Type B` and `Type C`, never the trademark. That is what
the industry calls the topology when it is not licensed, and it is what somebody
searching for this will search for. `ATTRIBUTIONS.md` carries the disclaimer.

### The host test, 2026-08-26

Ran in **Arena 7.27.1 rev 15990** on this Mac (Apple M4 Max, GL 4.1 Metal) and on
[win lab](https://github.com/stoatworks-labs/fleet-notes/blob/main/notes/reference_arena_on_winlab.md)
(Mesa llvmpipe, GL 4.5 Core), driven over the REST API. The Windows DLL was
cross-compiled x64 from the ARM64 Parallels guest — `winbuild/build-ferric.ps1`,
which is build-coinop.ps1's shape plus a `dumpbin /EXPORTS` check for `plugMain`
so a missing export is caught on the build box rather than on the test box. It
built clean first time; MSVC found nothing that clang had not.

Both hosts: all three shader programs compile, 34 parameters read back with no
truncation, every dropdown complete, renders correctly, trace overlay draws, and
the weighted readout shows **3.43 on both** at identical settings.

Three things worth keeping:

- ☠️ **Resolume's `SetTime` read 499,217,238 ms on the Mac** — 5.8 days of
  absolute clock. The phase-wrapping design was reasoned into existence against a
  predicted failure twenty minutes into a set; the real host is far worse than
  that, and a naive `float` would have been broken on the *first frame*. Worth
  remembering next time an "obviously over-careful" precision decision comes up.
- **FFGL's 16-character limit does not apply to option element names.** Fleet
  lore had this as unverified. `Compact Cassette` (16) and `Encode + Decode` (15)
  both came back whole from Arena on both platforms. Only the parameter's own
  name is cut. This belongs in fleet-notes' 16-char note.
- **The preset override pattern was observed working**, not just simulated: it
  held across nine seconds of Resolume restating its own values, and a manual
  edit released it back to Custom. First time in this fleet that fix has been
  watched rather than inferred.

⚠️ **One unexplained Arena restart on winlab.** No crash events in the
Application log, the plugin had already rendered correctly, and the instance that
came up afterwards never loaded Ferric at all. A stale Windows Firewall prompt
for an unrelated app (`frame-ferret`, prefetch dated 22 Aug) was found modal on
that desktop and is what returned 412 to every later edit. Left unattributed. If
it happens again that is the thing to chase; I would not call it a Ferric fault
on this evidence, and I would not call it cleared either.

### What is NOT done

- **No operator has dragged a slider.** Every control was driven over REST, so
  inspector layout and feel are unjudged, and so is the audio picker with real
  audio routed.
- **No NVIDIA or AMD driver has run it.** llvmpipe proves the shader source is
  portable, never that a vendor driver agrees, and nothing about performance.
- **No OpenFX build.** Deliberately deferred; the core is already host-agnostic
  and there is a note at the foot of `CMakeLists.txt` saying exactly what adding
  it involves, including which repo's `InfoOFX.plist.in` to copy (flipbook's,
  parameterised on `@PROJECT_NAME@` — downpour's hardcodes `CFBundleExecutable`
  and fails codesign after the tag).
- **No backend registration.** `source/StoatworksAbout.h` is hand-written and
  says so at the top. Adding the `projects.json` entry publishes a page, so it is
  a deliberate step rather than a side effect of scaffolding — and a sync run
  before the entry exists would DELETE the values rather than fill them in.
- No release, no demo, no video.

## The two ideas, in one paragraph each

**The transport.** Wow and flutter is a timing error and a picture read off tape
is a signal with a clock, so every pixel gets a tape time and one scalar error
signal is evaluated there. Wow leans the frame, flutter draws a travelling wave
down it, scrape bands it — and the wave scrolls between frames because the clock
moved, which is what reads as tape rather than as a wobble filter and is not
coded anywhere.

**The noise reduction.** It is a round trip through a medium, not a filter, and
the artifacts are the two ends disagreeing. The pass order is the signal chain:
encode, tape, decode. The hiss is bent once where the picture is bent twice.

Both are written out properly in `AGENTS.md`; this file does not repeat them.

## The decision that shaped everything else

**The physically correct scan rate produces no visible flutter, and I nearly
shipped a model that hid that.** A real picture scans at 15625 lines a second, so
audio-band flutter is a *constant* across a whole frame — measured, variation is
under a thousandth of the amplitude. Audio wow and flutter and video time-base
error are simply not the same phenomenon.

The options were to fudge the numbers quietly, or to expose the scan rate as a
control and say so. Exposing it turned out to be the better plugin as well as the
more honest one: `Tape Speed` spans rigid-frame to ribbons and is the single most
expressive control in the set, because it decides how much of the error signal
fits inside one picture.

## Traps

The full list is in `AGENTS.md`, which is where a person picking this up should
read. Three are worth repeating here because they are the ones that cost real
time and because two of them were the *test* being wrong:

**☠️ Absolute tape time as a float dies quietly, twenty minutes into a set.**
Scrape indexes noise at 220 Hz; at t = 1200 s that index is 264000, where a
float's spacing is 0.03 and adjacent pixels land on the same value. The noise
freezes into blocks. Nothing catches it: every offline render starts at t = 0 and
every screenshot is taken in the first minute. Phases are now computed once per
frame in double and wrapped, and everything mirrored works in the within-picture
offset. I found this by reasoning about the model rather than by seeing it, which
is the only way it *could* have been found before a show.

**`half` is a GLSL reserved word.** Not on the list anybody quotes. It failed at
runtime, in a shader assembled from three strings, so the line number referred to
a file that does not exist — and the symptom in Resolume would have been "the
noise reduction does nothing". `--nr` caught it only because it compiles the same
string the plugin does.

**Two checks failed because they were wrong, not the code.** `--drive` asked
whether Inverted reverses the mistracking using a bass-only spectrum, where
Natural correctly pushes by zero and zero has no sign. `--denoise` asserted that
encoding without decoding lifts the noise floor — it cannot, because the encoder
is upstream of the medium, and on a flat field it is a bit-for-bit no-op. The
second one was the pass order being exactly right, so the assertion was inverted
into a check *for* it. Both are recorded in `AGENTS.md` under "Tests that were
wrong", because next time the instinct will again be to reach for the code.

**☠️ `grep -q` under `set -o pipefail` fails when it succeeds.** `verify.sh`
reported a missing build stamp on a binary that plainly had one. A check that
fails only on success. It bites in proportion to how much output is left to
write, so the `nm` version survived and the `strings` version did not — which is
why the one that worked was not evidence of anything. This one is general enough
that it probably belongs in fleet-notes.

## Measured, on Apple Silicon, 2026-08-26

| what | reading |
|---|---|
| mirror disagreement, error signal, 6400 points | 1.9e-5 (tolerance 2e-4) |
| mirror disagreement, gain law | 2e-7 (tolerance 1e-5) |
| identity, neutral settings | bit-exact, 0/255 |
| round trip, Type B / Type C | 0.72 / 1.31 rms |
| the same, mistracked | 3.95 / 4.78 rms |
| compander along the scan / across it | 9.996 / 0.000 rms |
| noise floor, Type B / Type C decode | −4.4 dB / −5.1 dB |
| Type C's advantage over B, hiss 0.6 → 0.15 | −0.76 dB → −3.51 dB |
| 1080p, Type C + dropouts | 0.34 ms/frame |
| 4K, Type C + dropouts | 0.70 ms/frame |

The Type C row is the one worth keeping: its advantage over Type B *grows* as the
recorded level drops, which is exactly what a second stage an order below the
first is for. `--denoise` asserts the trend rather than a bare margin, because a
margin big enough to be impressive would be a margin tuned to today's numbers.

## Related

- [ffgl sdk bugs](https://github.com/stoatworks-labs/fleet-notes/blob/main/notes/reference_ffgl_sdk_bugs.md) — every SDK trap this repo works around
- [ffgl 16 char param names](https://github.com/stoatworks-labs/fleet-notes/blob/main/notes/reference_ffgl_16_char_param_names.md) — why `--list` prints name lengths
- [plugin factory presets](https://github.com/stoatworks-labs/fleet-notes/blob/main/notes/reference_plugin_factory_presets.md) — why a preset is an override
- [new plugin repo copy traps](https://github.com/stoatworks-labs/fleet-notes/blob/main/notes/reference_new_plugin_repo_copy_traps.md) — the checklist this repo was started against
- [abomerration](https://github.com/stoatworks-labs/abomerration/blob/main/docs/NOTES.md) — the donor for the scaffolding, the audio drive and the harness shape
