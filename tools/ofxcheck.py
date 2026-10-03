"""The OpenFX plugin, loaded by a host, against the FFGL plugin on the GPU.

`frtest --cpu` holds the CPU mirror against the GPU without any host in the
way. This is the other half: the BUILT OpenFX bundle, loaded and driven by
ofxprobe (resolume-ofx-bridge) exactly as a host would load it, against the
FFGL plugin rendered by `frtest` -- same picture, same controls, same clock --
so the marshalling, the parameter wiring, the preset menu and the threading are
all inside the comparison rather than assumed.

    python3 tools/ofxcheck.py --build build-verify

ofxprobe hands every plugin the same input -- a ramp, red `x * 4` and green
`y * 8`, both wrapping, blue 128 -- and renders frame 0. `frtest --card ramp`
builds that input byte for byte, and `--frames 1` renders the FFGL plugin's
first frame, which is the same clock: at t = 0 the integrated hiss and dropout
phases of the FFGL build and the `time * rate` ones of the OpenFX build are both
zero. `--silent` keeps a spectrum out of the FFGL side, so Show Trace's meters
read zero there as they always do here.

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

CASES = [
    ("defaults", {}),
    ("Type C, mistracked, dropouts, worn",
     {"nrType": 2, "mistracking": 0.8, "dropouts": 0.6, "headWear": 0.7, "hiss": 0.5}),
    ("Failing, ribbons, rolling",
     {"machine": 3, "tapeSpeed": 0.1, "amount": 0.6, "vertical": 1.0, "flutter": 0.8, "scrape": 0.6}),
    ("Type B, decode only", {"nrType": 1, "nrMode": 1}),
    ("Type C, encode only, half mix", {"nrType": 2, "nrMode": 2, "mix": 0.5}),
    ("Video Head, show trace", {"machine": 2, "amount": 0.4, "showTrace": 1}),
]

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
    args = parser.parse_args()

    frtest = os.path.join(args.build, "frtest")
    probe = args.ofxprobe or find_probe(root)
    for tool in (frtest, probe):
        if not os.access(tool, os.X_OK):
            print("missing: " + tool)
            return 1

    # The OpenFX menu, as the plugin describes it to a host.
    described = run([probe, "--dir", args.build, "--json"])
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
    cases = list(CASES)
    for element, name in enumerate(menu):
        if element == 0:
            continue
        cases.append((name, {"preset": element, "_ffgl_preset": table.index(name) + 1}))

    failures = 0
    scratch = tempfile.mkdtemp(prefix="ferric-ofxcheck-")

    ramp_path = os.path.join(scratch, "ramp.png")
    run([frtest, "--card", "ramp", "--size", args.size, "--scene", ramp_path])
    ramp = read_png(ramp_path)

    def render_pair(tag, ofx_settings, ffgl_settings):
        gpu_path = os.path.join(scratch, tag + "-ffgl.png")
        ofx_path = os.path.join(scratch, tag + "-ofx.bmp")

        cmd = [frtest, "--card", "ramp", "--frames", "1", "--silent", "--size", args.size, "--out", gpu_path]
        for name, value in ffgl_settings.items():
            cmd += ["--set", "%s=%s" % (name, value)]
        run(cmd)

        cmd = [probe, "--dir", args.build, "--render", "com.stoatworks.ferric", "--size", args.size,
               "--out", ofx_path]
        for name, value in ofx_settings.items():
            # A preset has to arrive as an EDIT, through kOfxActionInstanceChanged,
            # or the plugin's changedParam never copies it into the controls.
            flag = "--edit" if name == "preset" else "--set"
            cmd += [flag, "%s=%s" % (name, value)]
        run(cmd)

        given, rendered = read_probe_bmp(ofx_path)

        # The claim is "the same input", so check it rather than assume it:
        # ofxprobe's input half must be frtest's ramp exactly, or every number
        # below is comparing two different pictures -- an orientation slip
        # included.
        source = compare(ramp, given)
        if source[0] != 0:
            raise RuntimeError("ofxprobe's input is not frtest's ramp (worst %d/255)" % source[0])

        return compare(read_png(gpu_path), rendered)

    print("  %-36s %6s %10s %10s" % ("case", "worst", "differing", ">1/255"))
    for index, (name, settings) in enumerate(cases):
        ofx = {k: v for k, v in settings.items() if not k.startswith("_")}
        if "preset" in settings:
            ffgl = {"Preset": settings["_ffgl_preset"]}
        else:
            ffgl = {NAMES[k]: v for k, v in settings.items()}
        worst, differing, over_one, total = render_pair("case%d" % index, ofx, ffgl)
        print("  %-36s %3d/255 %10d %10d" % (name, worst, differing, over_one))
        if worst > WORST_TOLERANCE or over_one > OVER_ONE_TOLERANCE * total:
            print("FAIL  '%s': the OpenFX plugin and the FFGL plugin disagree" % name)
            failures += 1

    worst, differing, over_one, total = render_pair("control", {"flutter": 0.5}, {})
    print("  %-36s %3d/255 %10d %10d  (must FAIL)" % ("control: Flutter 0.50 vs 0.45", worst, differing, over_one))
    if worst <= WORST_TOLERANCE and over_one <= OVER_ONE_TOLERANCE * total:
        print("FAIL  the control passed -- this comparison cannot tell two settings apart")
        failures += 1

    if failures:
        print("%d failure(s); renders kept in %s" % (failures, scratch))
        return 1

    print("PASS  ofxcheck  (%d cases within %d/255 worst; control fails)" % (len(cases), WORST_TOLERANCE))
    return 0


if __name__ == "__main__":
    sys.exit(main())
