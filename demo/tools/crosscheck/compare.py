"""Compare printer.cpp's output with frames.mjs's, field by field.

    python3 demo/tools/crosscheck/compare.py cpp.txt js.txt

Every value is a hex bit pattern, so equal means the same float. The run fails
on any difference in shape (a missing uniform, a different label, flag or
integer) and on any float further apart than the rules below allow.

------------------------------------------------------------- why not all zero

printer.cpp is built with `-ffp-contract=off`, which is how the plugin's
x86_64 slice computes (the arm64 slice fuses some multiply-adds and differs
from both in the last bit). With that, the port is plain IEEE arithmetic in
the same order, and the one thing left that cannot agree is libm: the plugin
calls Apple's `powf` and `sinf`, the port JavaScript's double `Math.pow` and
`Math.sin` rounded to float. Apple's `powf` is not correctly rounded -- about
one slider position in 170 comes out a unit in the last place (ULP) away from
the correctly rounded answer the port gets. So:

  * ROOTS -- `wowRate`, `flutterRate`, `seconds` (Tape Speed, through
    `expRange`'s powf), `hissWidth`, and the uniforms made from them -- may
    differ by 1 ULP.
  * PHASES are a rate times the clock. Where a case's rate differs by one ULP,
    its phase at half a million seconds (Resolume's clock is that big) is a
    long way off, and is ALLOWED to be -- but only where the case's own root
    differs. A phase that differs while its rate agrees fails: that would be a
    port computing the clock differently. Scrape's and drift's phases have no
    rate behind them and must be identical.
  * `NrMistrack` is `powf( 10, dB / 20 )`: 1 ULP.
  * `beat` goes through powf, and `lurch` multiplies it: 2 ULP.
  * `TraceWfPercent` is the RMS of twelve thousand filtered samples of the
    error signal, each through four `sinf`: 128 ULP, about 1e-5 of the figure,
    a thousand times finer than the two decimals the trace prints.

Everything else must be identical, and the summary says how much is.
"""
import struct
import sys

ROOTS = {"wowRate", "flutterRate", "seconds", "hissWidth", "TbeWowRate", "TbeFlutRate", "TbeSeconds",
         "HissWidth"}
EXPLAINED_BY = {
    "TbePhaseWow1": {"wowRate"}, "TbePhaseWow2": {"wowRate"},
    "TbePhaseFlut1": {"flutterRate"}, "TbePhaseFlut2": {"flutterRate"},
    "hissPhase": {"seconds", "hissWidth"}, "HissPhase": {"seconds", "hissWidth"},
    "dropPhase": {"seconds"}, "DropPhase": {"seconds"},
}
TOLERANCE = {"beat": 2, "TraceBeat": 2, "lurch": 2, "TbeLurch": 2, "TraceWfPercent": 128, "NrMistrack": 1}
for name in ROOTS:
    TOLERANCE[name] = 1


def ulp32(a, b):
    ia = struct.unpack(">i", bytes.fromhex(a))[0]
    ib = struct.unpack(">i", bytes.fromhex(b))[0]
    if ia < 0:
        ia = -0x80000000 - ia
    if ib < 0:
        ib = -0x80000000 - ib
    return abs(ia - ib)


def ulp64(a, b):
    ia = struct.unpack(">q", bytes.fromhex(a))[0]
    ib = struct.unpack(">q", bytes.fromhex(b))[0]
    if ia < 0:
        ia = -0x8000000000000000 - ia
    if ib < 0:
        ib = -0x8000000000000000 - ib
    return abs(ia - ib)


def fields(line):
    head, kv = [], {}
    for word in line.split(" "):
        if "=" in word:
            k, v = word.split("=", 1)
            kv[k] = v
        else:
            head.append(word)
    return " ".join(head), kv


def cases(lines):
    """Indices into `lines`, split into runs that start at each `N` (a new instance)."""
    run = []
    for i, line in enumerate(lines):
        if line == "N" and run:
            yield run
            run = []
        run.append(i)
    if run:
        yield run


def main(cpp_path, js_path):
    cpp = open(cpp_path).read().splitlines()
    js = open(js_path).read().splitlines()
    if len(cpp) != len(js):
        print(f"FAIL  {len(cpp)} lines from the plugin's C++, {len(js)} from the port")
        return 1

    pairs = list(zip(cpp, js))
    shape = 0
    failures = 0
    values = 0
    identical = 0
    explained = 0
    worst = {}

    for run in cases(cpp):
        # Which roots differ anywhere in this case?
        rooted = set()
        for i in run:
            a, b = pairs[i]
            if a.startswith("C ") and a != b:
                _, ka = fields(a)
                _, kb = fields(b)
                rooted |= {k for k in ("wowRate", "flutterRate", "seconds", "hissWidth")
                           if k in ka and ka.get(k) != kb.get(k)}

        for i in run:
            a, b = pairs[i]
            n = i + 1
            count = a.count("=") or 1
            values += count
            if a == b:
                identical += count
                continue
            ha, ka = fields(a)
            hb, kb = fields(b)
            if ha != hb or list(ka) != list(kb):
                shape += 1
                if shape <= 5:
                    print(f"FAIL  line {n} differs in shape:\n        C++: {a[:200]}\n        js : {b[:200]}")
                continue
            values -= count
            for k in ka:
                values += 1
                va, vb = ka[k], kb[k]
                if va == vb:
                    identical += 1
                    continue
                pa_list, pb_list = va.split(","), vb.split(",")
                if len(pa_list) != len(pb_list) or any(len(p) not in (8, 16) for p in pa_list + pb_list):
                    shape += 1
                    if shape <= 5:
                        print(f"FAIL  line {n} field {k}: {va} vs {vb}")
                    continue
                for pa, pb in zip(pa_list, pb_list):
                    if pa == pb:
                        continue
                    dist = ulp64(pa, pb) if len(pa) == 16 else ulp32(pa, pb)
                    if dist > worst.get(k, (0,))[0]:
                        worst[k] = (dist, n, len(pa) == 16)
                    if k in EXPLAINED_BY and EXPLAINED_BY[k] & rooted:
                        explained += 1
                        continue
                    if dist > TOLERANCE.get(k, 0):
                        failures += 1
                        if failures <= 10:
                            why = (f"its rate agrees, so this is not libm" if k in EXPLAINED_BY
                                   else f"over {TOLERANCE.get(k, 0)} ULP")
                            print(f"FAIL  line {n} {k}: C++ {pa} js {pb} -- {dist} ULP, {why}")

    print(f"      {len(cpp)} lines, {values} values: {identical} identical to the bit, "
          f"{explained} phase(s) off only where a rate is a libm ULP away")
    for key, (dist, n, is_double) in sorted(worst.items(), key=lambda kv: -kv[1][0]):
        unit = "ULP of a double" if is_double else "ULP"
        print(f"      {key:<16} differs by up to {dist} {unit} (line {n})")
    if shape or failures:
        print(f"FAIL  {shape} shape difference(s), {failures} value(s) outside the rules")
        return 1
    print("ok    the port computes what the plugin's C++ computes, to libm's last bit")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1], sys.argv[2]))
