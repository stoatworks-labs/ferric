"""The scenario both halves of the port check run: printer.cpp and frames.mjs.

    python3 demo/tools/crosscheck/scenario.py > scenario.txt

Verbs, one a line:
    labels | machines        the dropdowns, defaults, preset table and machine table
    new                      a fresh instance at the constructor's defaults
    size W H                 the render size (H is the scanline count)
    set <field> <hex32>      a host value, as the float the host holds
    preset <element>         a factory preset written over the covered fields
    frame <hex64>            one frame at that clock, in seconds

Deterministic: a fixed seed, so a difference is a difference in the code.
"""
import random
import struct
import sys

out = []


def h32(v):
    return struct.pack(">f", v).hex()


def h64(v):
    return struct.pack(">d", v).hex()


def frame(t):
    out.append(f"frame {h64(t)}")


SLIDERS = ["wow", "wowRate", "flutter", "flutterRate", "scrape", "drift", "tapeSpeed", "amount", "vertical",
           "hiss", "dropouts", "headWear", "mistracking", "nrStrength", "beatDepth", "beatDecay", "levelDepth",
           "bandDepth", "mix"]
OPTIONS = {"machine": 4, "nrType": 3, "nrMode": 3, "sync": 2, "division": 6, "route": 4}
TRANSPORT = {"machine", "wow", "wowRate", "flutter", "flutterRate", "scrape", "drift", "amount"}

out.append("labels")
out.append("machines")

# Every slider across its travel, one frame each; the transport ones with the
# trace on so the weighted figure is measured at every position.
for name in SLIDERS:
    for k in range(11):
        out.append("new")
        out.append("size 960 540")
        if name in TRANSPORT:
            out.append(f"set showTrace {h32(1.0)}")
        if name in ("beatDepth", "beatDecay"):
            out.append(f"set sync {h32(1.0)}")
        out.append(f"set {name} {h32(k / 10.0)}")
        frame(1.25 + k * 0.37)

# Every element of every dropdown, with the trace on.
for name, count in OPTIONS.items():
    for k in range(count + 1):  # one past the end: the clamp
        out.append("new")
        out.append(f"set showTrace {h32(1.0)}")
        out.append(f"set beatDepth {h32(0.6)}")
        out.append(f"set bandDepth {h32(0.5)}")
        out.append(f"set {name} {h32(float(k))}")
        frame(2.5)

# Every factory preset, two seconds at 60 fps with the trace on: the beat
# envelope through two bars for the two that lock to the tempo.
for element in range(1, 10):
    out.append("new")
    out.append("size 1280 720")
    out.append(f"preset {element}")
    out.append(f"set showTrace {h32(1.0)}")
    for n in range(120):
        frame(n / 60.0)

# The clock: Resolume's absolute time (499,217,238 ms measured on Arena),
# hours into a set, before zero, and the hiss through a stall and a scrub.
for t in (499217.238, 3600.0 * 5 + 0.123, 1.0e6 + 0.5, -12.3, 0.0):
    out.append("new")
    out.append("size 1920 1080")
    out.append(f"set showTrace {h32(1.0)}")
    out.append(f"set dropouts {h32(0.4)}")
    frame(t)
    frame(t + 1 / 60)
    frame(t + 2 / 60)
out.append("new")
out.append("size 640 360")
out.append(f"set dropouts {h32(0.3)}")
for n in range(60):
    frame(n / 60.0)
frame(60 / 60.0 + 0.5)   # a stall: clamped to a tenth of a second
frame(0.0)               # a scrub backwards: no time passes
frame(1 / 60.0)

# The beat: every division at three decays, four seconds at 60 fps.
for division in range(6):
    for decay in (0.0, 0.45, 1.0):
        out.append("new")
        out.append(f"set sync {h32(1.0)}")
        out.append(f"set beatDepth {h32(0.85)}")
        out.append(f"set division {h32(float(division))}")
        out.append(f"set beatDecay {h32(decay)}")
        for n in range(240):
            frame(n / 60.0)

# Random states, a few frames each.
rng = random.Random(20261028)
for case in range(300):
    out.append("new")
    w, hgt = rng.choice([(640, 360), (960, 540), (1280, 720), (1920, 1080), (1024, 1024), (721, 403)])
    out.append(f"size {w} {hgt}")
    for name in SLIDERS:
        if rng.random() < 0.7:
            out.append(f"set {name} {h32(rng.random())}")
    for name, count in OPTIONS.items():
        if rng.random() < 0.7:
            out.append(f"set {name} {h32(float(rng.randrange(count)))}")
    if rng.random() < 0.5:
        out.append(f"set showTrace {h32(1.0)}")
    t = rng.choice([0.0, rng.uniform(0, 30), rng.uniform(0, 7200), rng.uniform(4.9e5, 5.0e5)])
    for n in range(3):
        frame(t + n * rng.choice([1 / 60, 1 / 30, 1 / 144, 0.2]))

sys.stdout.write("\n".join(out) + "\n")
