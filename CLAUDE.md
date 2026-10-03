# ferric

The video signal treated as an analogue tape signal: wow, flutter and scrape from
an unsteady transport, hiss and dropouts from the oxide, and the consumer
sliding-band noise reduction that hid one under the other. An FFGL effect for
Resolume Arena/Avenue, and the same effect as an OpenFX plugin for Resolve,
Vegas, Nuke and Natron. C++/GLSL, CMake MODULE → universal `.bundle` (macOS) +
Windows `.dll`, plus `Ferric.ofx.bundle` (macOS universal, Win64, Linux x86-64).
Public MIT repo.

Read `AGENTS.md` before changing the error signal, the compander, the pass order
or the clock.

## Commands (CMake)
- Configure: `cmake -B build -DCMAKE_BUILD_TYPE=Release`
- Fast dev build: add `-DCMAKE_OSX_ARCHITECTURES=arm64`
- Build: `cmake --build build` (both plugins; `-DBUILD_OFX=OFF` skips OpenFX)
- OpenFX alone, no FFGL SDK or GLEW (what the Linux job does):
  `cmake -B build-ofx -DFERRIC_BUILD_FFGL=OFF -DFERRIC_BUILD_TOOLS=OFF`
- Install to Resolume: `cmake --install build`
- OpenFX: copy `build/Ferric.ofx.bundle` to `/Library/OFX/Plugins/`
- Render ofxprobe's input through the FFGL plugin, to compare with an OpenFX
  render: `./build/frtest --card ramp --frames 1 --silent --out /tmp/gpu.png`
- Render a frame offline: `./build/frtest --out /tmp/frame.png`
- The test card on its own: `./build/frtest --scene /tmp/card.png`
- List parameters, with types, defaults and any at the 16-character limit:
  `./build/frtest --list`
- Set anything by its host-facing name:
  `--set "Flutter=0.8" --set "NR Type=2" --set "Tape Speed=0.2"`
- Render size: `--size 1920x1080`
- Advance the synthetic transport: `--frames 60` (60 fps, 120 bpm)
- Put real footage through the real shaders (for the project video):
  `ffmpeg … -f rawvideo -pix_fmt rgba - | ./build/frtest --pipe --size WxH --fps 30 --script cues.txt | ffmpeg …`
- A cue sheet is `frame  Parameter Name  value`; option and boolean parameters
  must STEP (two keys one frame apart), never ramp.

## Verify
- **Everything (34 checks, clean universal build, both plugins): `tools/verify.sh`**
  — the Fusion check needs `FERRIC_OFXPROBE` pointing at a probe with
  `--quirks fusion`, and skips without one
- The GLSL error signal against `Transport.cpp`: `./build/frtest --tbe`
- The DIN weighting curve is where this repo claims: `./build/frtest --weighting`
- The weighted figure responds to the transport: `./build/frtest --wf`
- A neutral plugin is transparent: `./build/frtest --identity`
- Encode into decode cancels: `./build/frtest --roundtrip`
- Noise reduction reduces noise: `./build/frtest --denoise`
- The GLSL gain law against `Compander.cpp`: `./build/frtest --nr`
- The compander works along the scan and only along it: `./build/frtest --scan`
- The reaction arithmetic (no GL): `./build/frtest --drive`
- ms vs seconds hosts: `./build/frtest --clock`
- Presets distinct and non-degenerate: `./build/frtest --presets`
- Presets survive the host's own echo (no GL): `./build/frtest --echo`
- No dead controls: `python3 tools/sweep.py`
- The OpenFX build's CPU passes against the GPU: `./build/frtest --cpu`
- The built OpenFX bundle in ofxprobe against the FFGL plugin:
  `python3 tools/ofxcheck.py --build build` — with a probe that takes `--in`
  and `--time` (`--ofxprobe PATH`, or `FERRIC_OFXPROBE` for verify.sh) it also
  compares the test card seconds into the clock; `--depth float` for float
- Render cost, GPU and the OpenFX build's CPU passes: `./build/frtest --bench`

`--drive`, `--weighting`, `--wf` and `--echo` need **no GL context** and are what
CI runs; everything else renders and runs locally before a tag.

## Notes
- **The transport and the noise reduction are separate and each has one idea.**
  Wow and flutter is a *timing* error evaluated at each pixel's tape time
  (`Transport.h`); noise reduction is a *round trip through a medium* whose
  artifacts are the two ends disagreeing (`Compander.h`). They meet only in the
  pass order.
- **The pass order IS the signal chain**: encode → tape → decode. That is what
  makes the hiss bent once where the picture is bent twice, and it is why
  encode-only cannot change the noise floor.
- **`Tape Speed` is the fiction, stated.** A real picture scans at 15625 lines a
  second, where audio-band flutter is a *constant* across the whole frame and
  invisible. The scan rate is therefore a control. See `Transport.h`.
- **The scan rate control runs backwards**: the quantity is seconds of tape per
  picture, and a faster tape means fewer.
- **Nothing is ever handed an absolute tape time as a float.** Phases are
  computed once per frame in double precision and wrapped; everything mirrored
  works in the offset *within one picture*. A version that skipped this looks
  perfect for the first few minutes of a set.
- **The compander is one-dimensional and horizontal.** Tape has one frequency
  axis. `--scan` is the check that keeps it that way, and it is the difference
  between this and a detail compressor.
- **Three things are mirrored and all are marked `//= mirrored` in both
  files**: the error signal (`Transport.cpp` ↔ `shaders/Tbe.cpp`, `--tbe`), the
  gain law (`Compander.cpp` ↔ `shaders/Compand.cpp`, `--nr`), and the per-pixel
  passes the OpenFX build renders with (`CpuPasses.cpp` ↔ `shaders/Passes.cpp`
  and `shaders/Compand.cpp`, `--cpu`). Edit a marked GLSL block and edit its
  twin. The machine table and the stage table are NOT mirrored — they arrive as
  uniforms.
- **The OpenFX build has no Reaction group.** No audio, no tempo. `cpu::frameAt`
  runs `Drive.cpp` on a zeroed input (the manual transport), and the OpenFX
  preset menu leaves out every preset that sets a reactive depth (Beat Slip,
  Breathing). Its hiss and dropout phases are `time × rate` where FFGL
  integrates them, because OpenFX renders frames out of order.
- **A factory preset is an override, not a write.** Resolume does not consume
  value events. See `Presets.h`; `--echo` is the check.
- **Resolume sends `SetTime` in MILLISECONDS.** The unit is settled by comparing
  the host's delta against a steady wall clock. `--clock` is the only check that
  would catch getting this wrong.
- **Every parameter name must be 16 characters or fewer.** FFGL hands the host a
  16-character buffer and the SDK does not enforce it. `--list` flags them and
  `verify.sh` fails on them.
- macOS build must be universal (arm64 + x86_64). Verify with `lipo`, never the
  build log — and note `cmake -B build` reuses a warm cache, which is why
  `verify.sh` builds into `build-verify/` with the architectures stated.
- `layout`, `flat`, `active`, `filter`, `input`, `output`, `sample`, `common` and
  **`half`** are GLSL reserved words. `half` cost a shader here.
- Public repo. "Commit" = commit **and** push.

## Released

**v0.1.0, 2026-08-26** — the first release. Signed and notarised on macOS,
Windows x64 built and tested. All five homes agree: repo, website project page,
YouTube, both embed links, and the download block. See `docs/NOTES.md`.

**The OpenFX build was added after v0.1.3, on 2026-10-03.** Verified against the
FFGL plugin in ofxprobe and by `--cpu`. In a real Resolve 21.1 it failed every
frame in the Fusion page (no frame rate reported, read unguarded); fixed the same
day with a 24 fps fallback and not yet re-checked in Resolve. Vegas, Nuke and
Natron have never loaded it. `docs/NOTES.md` says exactly what was and was not
checked.

## Not built yet
- **The browser demo.** Most video plugins in this fleet ship a hand-written
  WebGL demo at `<slug>-demo.stoatworks-labs.com`, served from `demo/` by the
  repo's own Worker. Ferric has none. Nothing deploys it automatically and
  `gen-downloads.py` does not know it exists, so adding one is a deliberate
  piece of work — see the checklist's section 1c.

## Diagnostics

`source/Diag.{h,cpp}` — log file only, no crash handler (this runs inside
Resolume), no bundle command. It covers the failures that actually happen: a
shader that will not compile, which otherwise looks like "the effect does
nothing" with no message anywhere; the host clock's units at frame 60; and a
factory preset dropping to Custom.

    ~/Library/Logs/ferric/ferric.YYYY-MM-DD.log
