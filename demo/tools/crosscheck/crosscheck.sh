#!/usr/bin/env bash
#
# The browser demo's port of the CPU half, against the plugin's own C++.
#
#     demo/tools/crosscheck/crosscheck.sh
#
# Builds printer.cpp against source/Transport.cpp, Controls.cpp, Compander.cpp
# and Drive.cpp, runs it and demo/ferric.js (through frames.mjs) over the same
# scenario, and compares every value they print. Needs a C++17 compiler and
# Node; tools/verify.sh runs it and skips without Node.
#
# Then a control: the comparison is run again against a copy of the port's
# output with one uniform moved, and must FAIL. A comparer that cannot fail
# proves nothing.
set -euo pipefail

cd "$(dirname "$0")/../../.."
HERE=demo/tools/crosscheck
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

c++ -std=c++17 -O2 -ffp-contract=off -Isource -o "$WORK/printer" "$HERE/printer.cpp" \
    source/Transport.cpp source/Controls.cpp source/Compander.cpp source/Drive.cpp

python3 "$HERE/scenario.py" > "$WORK/scenario.txt"
"$WORK/printer" < "$WORK/scenario.txt" > "$WORK/cpp.txt"
node "$HERE/frames.mjs" < "$WORK/scenario.txt" > "$WORK/js.txt"

python3 "$HERE/compare.py" "$WORK/cpp.txt" "$WORK/js.txt"

# The control: one phase in one frame pushed a long way off.
python3 - "$WORK/js.txt" "$WORK/mutant.txt" <<'PY'
import re, struct, sys
lines = open(sys.argv[1]).read().splitlines()
for i, line in enumerate(lines):
    m = re.search(r" TbePhaseFlut1=([0-9a-f]{8})", line)
    if m and line.startswith("U ") and " tape " in line and m.group(1) != "00000000":
        v = struct.unpack(">f", bytes.fromhex(m.group(1)))[0] * 1.001
        lines[i] = line.replace(m.group(0), " TbePhaseFlut1=" + struct.pack(">f", v).hex(), 1)
        break
open(sys.argv[2], "w").write("\n".join(lines) + "\n")
PY
if python3 "$HERE/compare.py" "$WORK/cpp.txt" "$WORK/mutant.txt" > "$WORK/control.txt" 2>&1; then
    echo "FAIL  control: a flutter phase moved by 0.1% still compared equal -- the comparer cannot fail"
    exit 1
fi
echo "ok    control: a flutter phase moved by 0.1% fails the comparison"
