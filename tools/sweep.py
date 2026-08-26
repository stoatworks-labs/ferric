"""Every parameter must actually change the picture.

A GLSL uniform name that does not match the C++ is silently ignored:
glGetUniformLocation returns -1, glUniform on -1 is a documented no-op, and
nothing in the build says a word. A control can therefore be completely dead
while everything compiles, links, loads and renders. Nothing else in this repo
catches that.

So: render each parameter at both ends of its range against a baseline where
every stage is switched on, and report any that made no difference.

    python3 tools/sweep.py

Run it after adding a parameter, renaming a uniform, or moving anything between
the C++ and the GLSL. Exit code 1 means something is dead.

------------------------------------------------------------------- the traps

Several of this plugin's controls are SUPPOSED to do nothing in the default
configuration, and a sweep that ignores that reports them dead and is right to.

  * **The FFT buffer is not a control and must be skipped.** `Audio` is an
    FF_TYPE_BUFFER whose 64 elements the host writes; its own float value is
    meaningless, so setting it to 0 and then 1 renders the same frame twice and
    reports a working audio input as dead. It is excluded by type, along with
    the About block's text line and buttons.

  * ☠️ **The noise reduction cancels itself, which is the entire design.** With
    NR Mode on Encode + Decode and the levels matched, Type C differs from Off by
    about 1.3 rms -- because the two ends are supposed to undo each other. So
    `NR Type` and `NR Strength` are swept in **Decode Only**, where the stage is
    the only thing acting on the picture. Sweeping them in round trip measures
    how well the compander cancels, which is `--roundtrip`'s job, and calls a
    working control nearly dead.

  * **Everything under Noise Reduction needs a type selected.** Mode, Mistracking
    and Strength are all inert with Type on Off, correctly.

  * **Every reactive control needs the reaction switched on.** Beat Depth needs
    Sync on a grid; Beat Decay and Division need a Beat Depth to shape *and* a
    moment that is not exactly on the beat -- at the instant of a beat the
    envelope is 1.0 whatever its decay. The harness drives a transport whose
    barPhase tracks its clock rather than sitting at zero, and two frames in is
    already off the grid.

  * **Band Depth and Route need a spectrum.** There is no audio in a headless
    process, so `frtest` injects a synthetic one on every frame. Without that
    both read as dead and the plugin is right.

  * **`Vertical` needs a slow error to follow.** It is driven by wow and drift
    only -- see Transport.h -- so a baseline with those at zero would report it
    dead.

If a control ever reads dead, work out what is masking it before assuming the
test is wrong.
"""
import argparse
import os
import struct
import subprocess
import sys
import tempfile
import zlib

SIZE = "640x360"
SCRATCH = tempfile.mkdtemp(prefix="ferricsweep")

# A baseline with every stage active, so nothing reads dead merely because the
# thing it modifies is switched off.
BASE = {
    "Machine": 0,
    "Wow": 0.50,
    "Wow Rate": 0.50,
    "Flutter": 0.50,
    "Flutter Rate": 0.45,
    "Scrape": 0.30,
    "Drift": 0.30,
    "Tape Speed": 0.40,
    "Amount": 0.40,
    "Vertical": 0.30,
    "Hiss": 0.35,
    "Dropouts": 0.25,
    "Head Wear": 0.30,
    "NR Type": 0,
    "NR Mode": 0,
    "Mistracking": 0.50,
    "NR Strength": 1.0,
    "Sync": 0,
    "Beat Depth": 0.0,
    "Beat Decay": 0.45,
    "Division": 2,
    "Level Depth": 0.0,
    "Band Depth": 0.0,
    "Route": 0,
    "Show Trace": 0,
    "Mix": 1.0,
    # Last, so that sweeping it overrides everything above rather than being
    # overridden. Custom, so it changes nothing while other controls are swept.
    "Preset": 0,
}

# Parameters that only do anything in one configuration, and the baseline change
# that switches it on.
CONTEXT = {
    # Decode Only, not round trip. See the module docstring -- this is the one
    # that would call a correct compander dead.
    "NR Type": {"NR Mode": 1},
    "NR Strength": {"NR Type": 2, "NR Mode": 1},
    "NR Mode": {"NR Type": 2},
    "Mistracking": {"NR Type": 2},
    # The reaction. Sync chooses whether the envelope follows the grid; with no
    # depth handed to the beat it has nothing to choose about.
    "Sync": {"Beat Depth": 0.90},
    "Beat Depth": {"Sync": 1},
    "Beat Decay": {"Sync": 1, "Beat Depth": 0.90},
    "Division": {"Sync": 1, "Beat Depth": 0.90},
    # BOTH halves of the routing, or the sweep only exercises one of them and
    # the endpoints below stop meaning anything.
    "Route": {"Band Depth": 0.90, "NR Type": 2, "NR Mode": 1, "Sync": 1, "Beat Depth": 0.90},
    # Band Depth pushes the decoder off level, so there has to be a decoder.
    "Band Depth": {"NR Type": 2, "NR Mode": 1},
}

# Endpoints to sweep between, where 0 and 1 are the wrong pair.
ENDS = {
    "Machine": (0, 3),
    "NR Type": (0, 2),
    # Decode Only against Encode Only: the two extremes of the round trip, and
    # the pair that differs most.
    "NR Mode": (1, 2),
    "Sync": (0, 1),
    "Division": (0, 5),
    # ⚠️ Natural against INVERTED, not against Treble Only.
    #
    # Natural sends bass to the lurch and treble to the decoder; Treble Only
    # sends treble to both. They therefore share a mistracking band, and with no
    # beat depth in the context they produce byte-identical frames -- which is
    # how this first reported Route as dead. Inverted differs from Natural on
    # every path, including the sign of the mistracking, so it is the pair that
    # actually tests the control.
    #
    # Worth knowing that a sweep reporting a dropdown dead is sometimes right:
    # this fleet has shipped an option whose two entries were byte-identical
    # code paths. Check which it is before changing the endpoints.
    "Route": (0, 1),
    "Show Trace": (0, 1),
    "Preset": (0, 9),
    # Amount at 0 is a genuine bypass, so sweep from a small real error --
    # otherwise this measures "the effect does something", which every other
    # control's sweep already establishes.
    "Amount": (0.15, 0.85),
    # At the very bottom the tape is so slow that a single flutter cycle spans a
    # few scanlines and the picture is noise either way; the pair below still
    # spans rigid-frame to visible-tearing.
    "Tape Speed": (0.20, 0.80),
}

# Not controls: the About block is a text line and four buttons that open a
# browser, and Audio is a host-written FFT buffer whose own value means nothing.
SKIP_TYPES = {"text", "event", "buffer"}
KNOWN_TYPES = {"standard", "boolean", "option", "text", "event", "buffer"}


def render(binary, path, overrides, frames):
    args = [binary, "--out", path, "--size", SIZE, "--frames", str(frames)]
    merged = dict(BASE)
    merged.update(overrides)
    for key, value in merged.items():
        args += ["--set", f"{key}={value}"]
    result = subprocess.run(args, capture_output=True, text=True)
    if result.returncode != 0:
        print("render failed:", result.stdout, result.stderr)
        sys.exit(1)
    with open(path, "rb") as handle:
        return handle.read()


def pixels(png):
    i = 8
    idat = b""
    width = height = 0
    while i < len(png):
        length = struct.unpack(">I", png[i:i + 4])[0]
        kind = png[i + 4:i + 8]
        data = png[i + 8:i + 8 + length]
        if kind == b"IHDR":
            width, height = struct.unpack(">II", data[:8])
        if kind == b"IDAT":
            idat += data
        i += 12 + length
    raw = zlib.decompress(idat)
    stride = width * 4
    return b"".join(raw[y * (stride + 1) + 1:(y + 1) * (stride + 1)] for y in range(height))


def difference(a, b):
    pa, pb = pixels(a), pixels(b)
    changed = 0
    total = 0
    count = len(pa) // 4
    for i in range(0, len(pa), 4):
        d = max(abs(pa[i] - pb[i]), abs(pa[i + 1] - pb[i + 1]), abs(pa[i + 2] - pb[i + 2]))
        if d > 2:
            changed += 1
        total += d
    return changed / count * 100.0, total / count


def parameters(binary):
    """Names and types, read out of the plugin itself.

    `--list` prints `id  name  type  default  notes`, and the NAME contains
    spaces. So the type is found by looking for the one field that is a known
    type word, and everything between the id and it is the name -- rather than
    counting fields, which breaks the first time a name gains or loses a word.
    """
    listing = subprocess.run([binary, "--list"], capture_output=True, text=True)
    if listing.returncode != 0:
        print("could not list parameters:", listing.stderr)
        sys.exit(1)

    out = []
    for line in listing.stdout.strip().splitlines():
        fields = line.split()
        if len(fields) < 3 or not fields[0].isdigit():
            continue

        kind_at = next((i for i, f in enumerate(fields) if f in KNOWN_TYPES), None)
        if kind_at is None:
            continue

        kind = fields[kind_at]
        name = " ".join(fields[1:kind_at])
        if kind in SKIP_TYPES or not name:
            continue
        out.append(name)
    return out


def main():
    parser = argparse.ArgumentParser()
    # verify.sh builds into its own directory with the shipping architectures and
    # runs this against THAT binary -- hardcoding a path would silently sweep a
    # different build from the one being verified.
    parser.add_argument("--binary", default=os.environ.get("FRTEST", "./build/frtest"))
    args = parser.parse_args()

    if not os.path.exists(args.binary):
        print(f"{args.binary} not found -- build first")
        return 1

    names = parameters(args.binary)
    print(f"{'parameter':<14} {'pixels changed':>15} {'mean delta':>11}   verdict")

    dead = []
    for name in names:
        low, high = ENDS.get(name, (0.0, 1.0))
        context = CONTEXT.get(name, {})

        a = render(args.binary, os.path.join(SCRATCH, "a.png"), {**context, name: low}, 2)
        b = render(args.binary, os.path.join(SCRATCH, "b.png"), {**context, name: high}, 2)

        percent, mean = difference(a, b)
        # A tenth of a per cent of the frame is a real change; anything below is
        # dithering and rounding between two renders of the same picture.
        alive = percent > 0.1
        if not alive:
            dead.append(name)

        note = "  (" + ", ".join(f"{k}={v}" for k, v in context.items()) + ")" if context else ""
        print(f"{name:<14} {percent:>14.2f}% {mean:>11.3f}   {'ok' if alive else 'DEAD'}{note}")

    print()
    if dead:
        print("DEAD CONTROLS:", ", ".join(dead))
        print("Check the uniform name matches the GLSL, and that nothing in BASE masks it.")
        return 1

    print(f"all {len(names)} controls reach the picture")
    return 0


if __name__ == "__main__":
    sys.exit(main())
