"""The OpenFX plugin, loaded by a host, against the FFGL plugin on the GPU.

`frtest --cpu` holds the CPU mirror against the GPU without any host in the
way. This is the other half: the BUILT OpenFX bundle, loaded and driven by
ofxprobe (resolume-ofx-bridge) exactly as a host would load it, against the
FFGL plugin rendered by `frtest` -- same picture, same controls, same clock --
so the marshalling, the parameter wiring, the preset menu and the threading are
all inside the comparison rather than assumed.

    python3 tools/ofxcheck.py --build build-verify

Two kinds of probe, told apart by what `--help` offers:

  * **The stock ofxprobe** hands every plugin the same input -- a ramp, red
    `x * 4` and green `y * 8`, both wrapping, blue 128 -- and renders frame 0.
    `frtest --card ramp` builds that input byte for byte, and `--frames 1`
    renders the FFGL plugin's first frame, which is the same clock: at t = 0 the
    integrated hiss and dropout phases of the FFGL build and the `time * rate`
    ones of the OpenFX build are both zero.

  * **A probe with `--in`, `--time` and `--frame-rate`** (the extended test
    host) is given frtest's own tape test card and rendered at the time the
    FFGL plugin reached: N frames at 60 fps from zero against `--frame-rate 60
    --time N-1`. Cases then run seconds into the clock -- one ten seconds in --
    which is where integrated phases and `time * rate` would part if anything
    were wrong, and a second control renders the OpenFX side one frame late.
    `--depth float` runs the float pipeline instead.

`--silent` keeps a spectrum out of the FFGL side, so Show Trace's meters read
zero there as they always do here. Either way the probe's input half is checked
against frtest's input before anything is compared.

Every case is compared per pixel on the colour channels (ofxprobe writes a
24-bit BMP; the input is opaque), and the control -- the OpenFX plugin at one
Flutter against the FFGL plugin at another -- must fail.

Standard library only, like tools/sweep.py. Exit code 1 on any failure.
"""

import argparse
import json
import os
import re
import struct
import subprocess
import sys
import tempfile
import zlib

# OpenFX parameter name -> the FFGL plugin's host-facing name. The two builds
# share every control the OpenFX build has; the Reaction group has no row here
# because it has no OpenFX parameter.
NAMES = {
    "machine": "Machine",
    "wow": "Wow",
    "wowRate": "Wow Rate",
    "flutter": "Flutter",
    "flutterRate": "Flutter Rate",
    "scrape": "Scrape",
    "drift": "Drift",
    "tapeSpeed": "Tape Speed",
    "amount": "Amount",
    "vertical": "Vertical",
    "hiss": "Hiss",
    "dropouts": "Dropouts",
    "headWear": "Head Wear",
    "nrType": "NR Type",
    "nrMode": "NR Mode",
    "mistracking": "Mistracking",
    "nrStrength": "NR Strength",
    "showTrace": "Show Trace",
    "mix": "Mix",
}

# (name, OpenFX settings, frames at 60 fps). The stock probe can only render
# frame 0, so it runs every case at one frame and skips CLOCK_ONLY, whose whole
# point is the time.
CASES = [
    ("defaults", {}, 3),
    ("defaults, ten seconds in", {}, 601),
    ("Type C, mistracked, dropouts, worn",
     {"nrType": 2, "mistracking": 0.8, "dropouts": 0.6, "headWear": 0.7, "hiss": 0.5}, 31),
    ("Failing, ribbons, rolling",
     {"machine": 3, "tapeSpeed": 0.1, "amount": 0.6, "vertical": 1.0, "flutter": 0.8, "scrape": 0.6}, 121),
    ("Type B, decode only", {"nrType": 1, "nrMode": 1}, 3),
    ("Type C, encode only, half mix", {"nrType": 2, "nrMode": 2, "mix": 0.5}, 3),
    ("Video Head, show trace", {"machine": 2, "amount": 0.4, "showTrace": 1}, 61),
]
PRESET_FRAMES = 241
CLOCK_ONLY = {"defaults, ten seconds in"}

# Measured against the GPU at worst 1/255 with no pixel past one code value --
# see `frtest --cpu` for where that one code value comes from. The same margin.
WORST_TOLERANCE = 2
OVER_ONE_TOLERANCE = 0.001


def read_png(path):
    """frtest's own PNGs: 8-bit RGBA, filter 0 on every row, top row first."""
    data = open(path, "rb").read()
    pos, width, height, idat = 8, 0, 0, b""
    while pos < len(data):
        length, kind = struct.unpack(">I4s", data[pos:pos + 8])
        body = data[pos + 8:pos + 8 + length]
        if kind == b"IHDR":
            width, height = struct.unpack(">II", body[:8])
        elif kind == b"IDAT":
            idat += body
        pos += 12 + length
    raw = zlib.decompress(idat)
    stride = width * 4
    rows = []
    for y in range(height):
        start = y * (stride + 1)
        if raw[start] != 0:
            raise ValueError("unexpected PNG filter in " + path)
        rows.append(raw[start + 1:start + 1 + stride])
    return width, height, rows


# ofxprobe's comparison image: input, an 8-pixel gap, output.
PROBE_GAP = 8


def read_probe_bmp(path):
    """ofxprobe's 24-bit BMP is input | gap | output, bottom row first.
    Returns (input, output), each as (width, height, RGB rows top row first)."""
    data = open(path, "rb").read()
    offset = struct.unpack("<I", data[10:14])[0]
    width, height = struct.unpack("<ii", data[18:26])
    bpp = struct.unpack("<H", data[28:30])[0]
    if bpp != 24:
        raise ValueError("expected a 24-bit BMP from ofxprobe, got %d" % bpp)
    stride = (width * 3 + 3) & ~3
    half = (width - PROBE_GAP) // 2

    def half_at(first):
        rows = []
        for y in range(abs(height)):
            start = offset + y * stride + first * 3
            row = bytearray(half * 3)
            row[0::3] = data[start + 2:start + half * 3:3]
            row[1::3] = data[start + 1:start + half * 3:3]
            row[2::3] = data[start:start + half * 3:3]
            rows.append(bytes(row))
        if height > 0:
            rows.reverse()  # bottom-up on disk
        return half, abs(height), rows

    return half_at(0), half_at(half + PROBE_GAP)


def write_ppm(path, png):
    """An RGBA PNG as a binary PPM, which every probe that takes --in reads."""
    w, h, rows = png
    with open(path, "wb") as f:
        f.write(b"P6\n%d %d\n255\n" % (w, h))
        for row in rows:
            rgb = bytearray(w * 3)
            rgb[0::3] = row[0::4]
            rgb[1::3] = row[1::4]
            rgb[2::3] = row[2::4]
            f.write(rgb)


def read_ppm(path):
    """A binary PPM (P6, maxval 255) as RGB rows, top row first."""
    data = open(path, "rb").read()
    fields, pos = [], 0
    while len(fields) < 4:
        while data[pos:pos + 1].isspace():
            pos += 1
        start = pos
        while not data[pos:pos + 1].isspace():
            pos += 1
        fields.append(data[start:pos])
    pos += 1
    if fields[0] != b"P6" or int(fields[3]) != 255:
        raise ValueError("expected an 8-bit P6 PPM from the probe")
    w, h = int(fields[1]), int(fields[2])
    return w, h, [data[pos + y * w * 3:pos + (y + 1) * w * 3] for y in range(h)]


def is_extended(probe):
    """Whether this probe can be handed an input and a time."""
    usage = subprocess.run([probe, "--help"], capture_output=True, text=True)
    text = usage.stdout + usage.stderr
    return all(flag in text for flag in ("--in FILE", "--time", "--frame-rate", "--out-only", "--no-system-dirs"))


def compare(png, bmp):
    """Colour channels of an RGBA PNG against RGB rows: worst channel, pixels
    differing at all, pixels more than one code value out, pixel count."""
    w, h, gpu = png
    w2, h2, ofx = bmp
    if (w, h) != (w2, h2):
        raise ValueError("size mismatch: FFGL %dx%d, OpenFX %dx%d" % (w, h, w2, h2))
    worst, differing, over_one, total = 0, 0, 0, 0
    for y in range(h):
        a, b = gpu[y], ofx[y]
        for x in range(w):
            d = max(abs(a[x * 4 + c] - b[x * 3 + c]) for c in range(3))
            if d > worst:
                worst = d
            if d > 0:
                differing += 1
                if d > 1:
                    over_one += 1
        total += w
    return worst, differing, over_one, total


def run(cmd):
    result = subprocess.run(cmd, capture_output=True, text=True)
    if result.returncode != 0:
        sys.stdout.write(result.stdout + result.stderr)
        raise RuntimeError("failed: " + " ".join(cmd))
    return result.stdout


def preset_names(root):
    """The preset table's names in table order, read out of Presets.h, so the
    FFGL element for an OpenFX menu entry is found by NAME -- the OpenFX menu
    leaves out the reactive presets, so the two numberings need not agree."""
    text = open(os.path.join(root, "source", "Presets.h")).read()
    return re.findall(r'^\s*\{\s*"([^"]+)",\s*\{', text, re.M)


def find_probe(root):
    """ofxprobe beside this repo -- or, from a git worktree, beside the main
    checkout, which is where `../resolume-ofx-bridge` actually is."""
    candidates = [os.path.join(root, "..", "resolume-ofx-bridge", "build", "ofxprobe")]
    try:
        common = subprocess.run(["git", "-C", root, "rev-parse", "--path-format=absolute", "--git-common-dir"],
                                capture_output=True, text=True).stdout.strip()
        if common:
            checkout = os.path.dirname(common)
            candidates.append(os.path.join(checkout, "..", "resolume-ofx-bridge", "build", "ofxprobe"))
    except OSError:
        pass
    for candidate in candidates:
        if os.access(candidate, os.X_OK):
            return os.path.normpath(candidate)
    return os.path.normpath(candidates[0])


def main():
    here = os.path.dirname(os.path.abspath(__file__))
    root = os.path.dirname(here)

    parser = argparse.ArgumentParser()
    parser.add_argument("--build", default=os.path.join(root, "build"))
    parser.add_argument("--ofxprobe", default=None)
    parser.add_argument("--size", default="640x360")
    parser.add_argument("--depth", default="byte", choices=("byte", "float"),
                        help="pixel depth for an extended probe; the stock one is 8-bit only")
    args = parser.parse_args()

    frtest = os.path.join(args.build, "frtest")
    probe = args.ofxprobe or find_probe(root)
    for tool in (frtest, probe):
        if not os.access(tool, os.X_OK):
            print("missing: " + tool)
            return 1

    extended = is_extended(probe)
    # An installed Ferric would otherwise be found as well; the extended probe
    # can be told to look nowhere but here.
    scan = ["--no-system-dirs", "--dir", args.build] if extended else ["--dir", args.build]

    # The OpenFX menu, as the plugin describes it to a host.
    described = run([probe] + scan + ["--json"])
    menu = []
    start = described.find("[") if described.lstrip().startswith("[") else described.find("{")
    manifest = json.loads(described[start:])
    plugins = manifest if isinstance(manifest, list) else manifest.get("plugins", [])
    for plugin in plugins:
        if plugin.get("identifier") == "com.stoatworks.ferric":
            for param in plugin.get("params", []):
                if param.get("name") == "preset":
                    menu = param.get("choices", [])
    if not menu:
        print("FAIL  the OpenFX plugin did not describe a preset menu")
        return 1

    table = preset_names(root)
    cases = [case for case in CASES if extended or case[0] not in CLOCK_ONLY]
    for element, name in enumerate(menu):
        if element == 0:
            continue
        cases.append((name, {"preset": element, "_ffgl_preset": table.index(name) + 1}, PRESET_FRAMES))

    failures = 0
    scratch = tempfile.mkdtemp(prefix="ferric-ofxcheck-")

    # The input both plugins are given: ofxprobe's own ramp for the stock probe,
    # frtest's tape test card for one that takes --in.
    card = ["--card", "ramp"] if not extended else []
    source_path = os.path.join(scratch, "source.png")
    run([frtest] + card + ["--size", args.size, "--scene", source_path])
    source = read_png(source_path)
    source_ppm = os.path.join(scratch, "source.ppm")
    if extended:
        write_ppm(source_ppm, source)

    def render_pair(tag, ofx_settings, ffgl_settings, frames, ofx_frame=None):
        gpu_path = os.path.join(scratch, tag + "-ffgl.png")
        sbs_path = os.path.join(scratch, tag + "-ofx.bmp")
        out_path = os.path.join(scratch, tag + "-ofx.ppm")
        frames = frames if extended else 1

        cmd = [frtest] + card + ["--frames", str(frames), "--silent", "--size", args.size, "--out", gpu_path]
        for name, value in ffgl_settings.items():
            cmd += ["--set", "%s=%s" % (name, value)]
        run(cmd)

        cmd = [probe] + scan + ["--render", "com.stoatworks.ferric", "--size", args.size, "--out", sbs_path]
        if extended:
            # The FFGL side ran `frames` frames at 60 fps from zero, so its last
            # frame is at (frames - 1) / 60 s: the same instant as OpenFX frame
            # frames - 1 at 60 fps.
            when = frames - 1 if ofx_frame is None else ofx_frame
            cmd += ["--in", source_ppm, "--frame-rate", "60", "--time", str(when), "--depth", args.depth,
                    "--out-only", out_path]
        for name, value in ofx_settings.items():
            # A preset has to arrive as an EDIT, through kOfxActionInstanceChanged,
            # or the plugin's changedParam never copies it into the controls.
            flag = "--edit" if name == "preset" else "--set"
            cmd += [flag, "%s=%s" % (name, value)]
        run(cmd)

        given, rendered = read_probe_bmp(sbs_path)
        if extended:
            rendered = read_ppm(out_path)

        # The claim is "the same input", so check it rather than assume it: the
        # probe's input half must be frtest's input exactly, or every number
        # below is comparing two different pictures -- an orientation slip
        # included.
        same = compare(source, given)
        if same[0] != 0:
            raise RuntimeError("the probe's input is not frtest's (worst %d/255)" % same[0])

        return compare(read_png(gpu_path), rendered)

    if extended:
        print("extended probe: tape test card, 60 fps, depth %s" % args.depth)
    else:
        print("stock probe: ofxprobe's ramp, frame 0, 8-bit")
    print("  %-36s %6s %7s %10s %10s" % ("case", "frames", "worst", "differing", ">1/255"))
    for index, (name, settings, frames) in enumerate(cases):
        ofx = {k: v for k, v in settings.items() if not k.startswith("_")}
        if "preset" in settings:
            ffgl = {"Preset": settings["_ffgl_preset"]}
        else:
            ffgl = {NAMES[k]: v for k, v in settings.items()}
        worst, differing, over_one, total = render_pair("case%d" % index, ofx, ffgl, frames)
        print("  %-36s %6d %4d/255 %10d %10d" % (name, frames if extended else 1, worst, differing, over_one))
        if worst > WORST_TOLERANCE or over_one > OVER_ONE_TOLERANCE * total:
            print("FAIL  '%s': the OpenFX plugin and the FFGL plugin disagree" % name)
            failures += 1

    controls = [("control: Flutter 0.50 vs 0.45", {"flutter": 0.5}, None)]
    if extended:
        controls.append(("control: OpenFX one frame late", {}, 3))
    for name, settings, late in controls:
        worst, differing, over_one, total = render_pair("control%d" % len(name), settings, {}, 3, late)
        print("  %-36s %6d %4d/255 %10d %10d  (must FAIL)" % (name, 3 if extended else 1, worst, differing, over_one))
        if worst <= WORST_TOLERANCE and over_one <= OVER_ONE_TOLERANCE * total:
            print("FAIL  the control passed -- this comparison cannot tell them apart")
            failures += 1

    if failures:
        print("%d failure(s); renders kept in %s" % (failures, scratch))
        return 1

    print("PASS  ofxcheck  (%d cases within %d/255 worst, %s probe; control%s fail%s)"
          % (len(cases), WORST_TOLERANCE, "extended" if extended else "stock",
             "s" if len(controls) > 1 else "", "" if len(controls) > 1 else "s"))
    return 0


if __name__ == "__main__":
    sys.exit(main())
