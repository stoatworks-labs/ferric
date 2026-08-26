# ferric — for whoever picks this up next

The video signal treated as an analogue tape signal, as an FFGL 2.1 effect for
Resolume (`Ferric`, ID **`FR01`**). C++17 + GLSL 4.1, CMake. `CLAUDE.md` is the
command reference and is not duplicated here; this file is the *why*.

## Two ideas, and why the code is shaped like this

They are genuinely separate. Each has its own header, its own idea, and no
knowledge of the other. They meet only in `ProcessOpenGL`'s pass order.

### 1. Wow and flutter is a timing error, and a picture read off tape has a clock

Every pixel has a **tape time** — how far into the recording the head was when
that pixel came off the oxide — and the horizontal displacement of that pixel is
one scalar error signal evaluated there. That is the whole model. Everything the
effect looks like falls out of the substitution:

- **Wow** (0.5–3 Hz) is slower than one picture, so the frame leans as a whole.
- **Flutter** (6–20 Hz) fits a cycle or two into a picture, so it draws a
  travelling wave down the image — and because the frame's tape time advances,
  the wave **scrolls**. That scroll is what reads as tape rather than as a wobble
  filter, and it is not coded anywhere. It is what a moving clock does to a fixed
  waveform.
- **Scrape** (100 Hz+) fits many cycles in, so it becomes fine horizontal banding.
- Because tape time also varies *along* the line, the error is a per-line
  **stretch** rather than a per-line offset. Lines get longer and shorter. Free;
  it is the `u` term.

### 2. Noise reduction is a round trip through a medium

Not a filter. A sliding-band compander boosts quiet high-frequency detail before
the tape so the tape's noise floor sits under it, and cuts it by the same amount
after — taking the hiss down with it. Nothing is removed. **Everything anybody
recognises about the format is the two ends disagreeing.**

So the chain is the real chain, in the real order:

    input -> ENCODE -> [ tape: time-base error, hiss, dropouts ] -> DECODE -> out

and that order is the design, not an implementation detail. `--denoise` asserts
it directly: encoding alone cannot change the noise floor of a flat field,
because the encoder runs before the tape and there is no hiss there yet.

## Load-bearing invariants

**The scan rate is a CONTROL, and the physically correct value produces nothing.**
A real 625-line picture scans at 15625 lines a second. One scanline is 64
microseconds; flutter at 10 Hz moves through 0.0004 of a cycle in that time, and
a whole frame spans 0.0016. Put the real numbers in and the error is a *constant*
across the entire frame — no vertical structure, no travelling wave, no tearing,
just a frame sliding sideways very slightly. Measured at Flutter 1.0 and the real
scan rate, variation across the picture is under a thousandth of the amplitude.

That is a true fact about videotape rather than a failure of the model: audio wow
and flutter and video time-base error are not the same phenomenon and do not live
in the same band. A VCR tears because of head-drum and servo error at field rate.
So the plugin does not pretend: `Tape Speed` exposes seconds-of-tape-per-picture
from 2 ms (rigid frame) to 2 s (ribbons), the fiction is stated in `Transport.h`,
and the operator chooses how far into it to go.

**`Tape Speed` runs backwards.** The quantity is seconds per picture; a faster
tape means fewer. Getting this inverted is not subtle — the control does the
opposite of its label — but it is invisible to any test that only checks a
control is not dead.

**The compander is one-dimensional and horizontal, and that is the effect.** Tape
knows one axis. A vertical edge is a high frequency to a tape machine; a
horizontal edge is not a frequency at all, it is the next line. Doing it
isotropically would make the stage a detail compressor that happens to be on a
video plugin, and the give-away artifact — vertical edges pumping while
horizontal ones sit perfectly still — would be gone. `--scan` measures both axes
and demands the horizontal one be untouched. It currently reads **9.996 rms
against 0.000**.

**Decode is `g/(1+g)`, and that is exact rather than approximate.**
`(I + gH)^-1 = I - g/(1+g) H` follows directly whenever `H` is a projector. A real
high-pass is only approximately one, so the round trip is only approximately
transparent — `--roundtrip` reports the residual instead of this repo claiming it
is zero. Measured: **0.72 rms for Type B, 1.31 for Type C**, against 3.9 and 4.8
for the same round trip mistracked.

**The decoder derives its control signal from its own output.** That is the
feedback topology, and it is why real decoders track at all. In a per-pixel
shader it is a fixed point, solved by one iteration entirely in scalars — the
detail band shrinks by a factor that follows from the gains alone, so the
encoder's level is predicted arithmetically with no second fetch set.

**The hiss is load-bearing.** Noise reduction with no noise to reduce is an
identity function with extra steps, and the decode stage would have nothing to
show for itself. `Hiss` defaults above zero deliberately.

**Every parameter name is 16 characters or fewer.** FFGL's legacy
`FF_GET_PARAMETER_NAME` hands the host a 16-character, non-null-terminated buffer
and the SDK returns a pointer to the full `std::string`. The plugin stores
eighteen, Resolume copies sixteen, every offline harness passes, and only the
host is ever wrong. Six plugins in this fleet shipped a control called
`Background Opaci`. `--list` flags anything over, and separately lists anything
at exactly 16 — because the host cannot tell "fits exactly" from "cut", so those
need a human.

## Traps that have already cost time here

**☠️ Nothing is ever handed an absolute tape time as a float, and a version that
did would look perfect for the first few minutes of a set.** Scrape indexes its
noise at 220 Hz; twenty minutes in that is an index of 264000, where a float's
spacing is about 0.03 — so adjacent pixels, a few millionths apart, land on the
same float and the noise freezes into blocks. The sines go the same way slightly
later. Nothing about it shows up in a test: every offline render starts at t = 0
and every screenshot is taken in the first minute.

So `phaseAt()` computes the frame's phases once, on the CPU, in double precision,
and wraps them; everything mirrored works in `dt`, the offset *within one
picture*, which is at most two seconds by construction. The hiss index has the
same treatment in the one place that does not go through `phaseAt` — see the ☠️
in `shaders/Passes.cpp`.

**Measured in a real Resolume, this was load-bearing from the first frame, not
after twenty minutes.** Arena's `SetTime` on the macOS test machine read
**499,217,238 ms** — 5.8 days of absolute clock, because the host's clock is not
zeroed when a plugin is instantiated. A `float` at 499217 s has a spacing of
0.03, which is a fifth of a whole picture's worth of tape at the default Tape
Speed; every pixel in a frame would have collapsed onto one or two distinct
values of the error signal. The reasoning that produced this design predicted a
failure twenty minutes into a set. The host was worse than the reasoning.

**`half` is a GLSL reserved word.** It is not on the list anybody quotes, and it
cost the compander pass: `float half = 0.5 * NrTexelX` produced
`'half' : Reserved word` at *runtime*, in a shader assembled from three strings,
so the reported line number referred to a file that does not exist. `--nr` caught
it because it compiles the same string; nothing else would have, and the symptom
in Resolume would have been "the noise reduction does nothing".

**The hash cannot be `fract( sin(x) * 43758.5453 )`.** That idiom is not
mirrorable: `sin` is a library function on the CPU and hardware on the GPU, they
disagree in the last few bits, and multiplying by forty thousand promotes those
bits to the whole answer. The two sides would produce entirely different noise
from identical source and `--tbe` would fail with no pattern to it. It is an
integer hash, masked to 24 bits before the float conversion so that conversion is
exact too. Measured mirror disagreement over 6400 points: **1.9e-5**, all of it
`sin`.

**Decode has to invert the two stages in the OPPOSITE order to encode.** Encoding
is `S2(S1(x))`, so decoding is `S1inv(S2inv(y))`. Invisible with one stage and
wrong by a few percent with two — exactly the size of error that reads as "the
emulation is a bit off" rather than as a bug.

**☠️ A factory preset is an OVERRIDE, not a write, because Resolume does not
consume value events.** It goes on pushing the values it still believes in — the
ones from before the preset — as ordinary `SetFloatParameter` calls, so a naive
"a covered control changed, so drop to Custom" rule fires on the host's own echo
immediately, every time. The dropdown snaps back and nothing happens. This cost
the fleet a shipped release and an external bug report.

`hostSent[]` keeps what the host last *said* separately from what the render
uses, which makes three cases distinguishable: the host restating itself, the
host echoing our preset, and a real edit. Judge on what the value **is**, never
on that it changed. The tolerance is **1e-3**, a host-quantisation allowance and
not a float epsilon — 1e-4 reads a rounded echo of our own value as an edit and
the bug comes straight back. And `seedHostSent()` must run *before* any preset
can be applied, or it records the preset's own values as the host's opening
position and the bug returns by a different route. `--echo` simulates three hosts
— ignores events, honours them, honours-and-quantises — and the third is the one
that catches the tolerance.

**☠️ `grep -q` under `set -o pipefail` fails when it succeeds.** `grep -q` exits
the moment it matches, closing the pipe, killing the upstream command with
SIGPIPE, failing the pipeline. `verify.sh` reported a missing build stamp on a
binary that plainly had one — a check that fails only on success. It bites in
proportion to how much output is left to write, so the `nm` version survived and
the `strings` version did not, which is why the working one was not evidence of
anything. Capture, then match.

**`FFGLShader::Set` has no array overload and no bool overload.** The overloads
are `float`, `vec2`..`vec4` and `int`. An array pushed through the float one is a
`GL_INVALID_OPERATION` that leaves the uniform at zero with nothing anywhere the
plugin can see. `NrThresh`, `NrBoost`, `DoEncode`, `DoDecode` and `ShowTrace` all
go through the raw `glUniform*` calls.

**`ScopedFBOBinding` restores the framebuffer binding and NOT the viewport**, so
an off-screen pass's `ResizeViewPort()` leaks into the pass that draws to the
host's framebuffer. The symptom does not look like a viewport bug: the effect
renders correctly into a *corner* of the frame and leaves the rest untouched.
The host viewport is captured at the top of `ProcessOpenGL` and restored before
the final pass.

**Every `ffglex::Scoped*` binding CLEARS to 0 on scope exit rather than
restoring**, and `FFGLFBO::Initialise` sizes its colour texture under one — so
allocating a buffer silently unbinds the input texture. Correct on every frame
*except* the one that allocates. Every `Ensure()` is called before anything is
bound, and `PassBuffer::Ensure` saves and restores `GL_TEXTURE_BINDING_2D` as
well, so that stops being something a future edit can undo by moving one line.

**A missing `SetTextParameter` override makes the whole plugin
uninstantiable.** `instantiateGL` sets every parameter's default on a fresh
instance and deletes the instance if any set returns FF_FAIL; the base class's
`SetTextParameter` is a stub returning exactly that. Declaring the About text
block without overriding it means no real host can load the plugin — while every
harness that drives the class directly passes, because they bypass `plugMain`.
`verify.sh`'s ffgltest step is the only thing here that would catch it.

**The trace overlay's digit cells need two aspect corrections, not one.** Panel
space is not square and is not the frame's shape either. The first version
divided by the frame aspect where it should have multiplied, and produced glyphs
twenty-one pixels wide and six tall — which looked like a blending bug.

## Tests that were wrong, and what that taught

Worth recording because in both cases the plugin was right and the check was not,
and both looked like failures at first.

**`--drive` asserted that Inverted reverses the mistracking, using a bass-only
spectrum.** Natural routes *treble* to the decoder, so with no treble it
correctly pushes by zero, and zero has no sign to compare against. The check now
uses a bass-only spectrum for the lurch half and a full-range one for the
mistracking half.

**`--denoise` asserted that encoding without decoding lifts the noise floor.** It
does not, and cannot: the encoder is upstream of the medium. On a flat field it
is a bit-for-bit no-op. That failure was the pass order being exactly right, so
the assertion was inverted into a check *for* it — encode-only must be identical
to Off on a flat field, and plainly different on a picture with detail.

**`tools/sweep.py` reported `Route` dead.** Natural and Treble Only share a
mistracking band, so with no beat depth in the context they render identical
frames. The endpoints are now Natural against Inverted, which differ on every
path. Worth knowing that a sweep reporting a dropdown dead is *sometimes right* —
this fleet has shipped an option whose two entries were byte-identical code
paths — so check which it is before changing the endpoints.

## What the real hosts established, 2026-08-26

Arena 7.27.1 rev 15990 on both, driven over the REST API.

**The FFGL 16-character limit does NOT apply to option element names.** Fleet
lore has this as "unverified either way". It is now verified in one direction:
`Encode + Decode` (15), `Compact Cassette` (16), `Reel to Reel` (12) and
`Treble Only` (11) all came back from Arena complete, on both platforms. So a
dropdown entry may be as long as it needs to be; only the parameter's own name
is cut at sixteen.

**Resolume's clock is a large absolute value.** See the ☠️ above — this is the
single most useful number the host test produced.

**The preset override pattern survives Resolume.** A preset applied through the
API held across nine seconds of the host restating its own values, and a manual
edit of a covered control released it back to Custom and kept the edit. That is
the bug that cost the fleet a shipped release, and it is the first time the fix
has been observed rather than simulated.

**Mesa llvmpipe compiles all three programs.** A genuinely different GLSL
compiler from Apple's, so `half` was not the only reserved-word risk and there
are no others. It proves correctness, never performance.

⚠️ **One unexplained Arena restart on the Windows box.** No application crash
events, the plugin had already rendered correctly, and the instance that came up
afterwards never loaded Ferric at all. A stale Windows Firewall prompt for an
unrelated application (`frame-ferret`, prefetch entry dated four days earlier)
was found modal on that desktop, which is what returned 412 to every subsequent
edit. Recorded as unattributed rather than as either a pass or a fault — if it
recurs, that is the thing to chase.

## Measured, on Apple Silicon

| what | reading |
|---|---|
| mirror disagreement, error signal, 6400 points | 1.9e-5 (tolerance 2e-4) |
| mirror disagreement, gain law | 2e-7 (tolerance 1e-5) |
| identity, neutral settings | 0/255 worst, bit-exact |
| round trip, Type B / Type C | 0.72 / 1.31 rms |
| the same, mistracked | 3.95 / 4.78 rms |
| compander along the scan / across it | 9.996 / 0.000 rms |
| noise floor, Type B / Type C decode | −4.4 dB / −5.1 dB |
| Type C's advantage over B, hiss 0.6 → 0.15 | −0.76 dB → −3.51 dB |
| 1080p, Type C + dropouts | 0.34 ms/frame |
| 4K, Type C + dropouts | 0.70 ms/frame |

The two-stage signature is the interesting row: Type C pulls further ahead of
Type B as the recorded level drops, which is what a second stage an order below
the first is *for*, and `--denoise` asserts the trend rather than a bare margin.
