"""The demo's shaders must be the plugin's shaders, character for character.

    python3 demo/tools/check_shaders.py           compare; exit 1 on any drift
    python3 demo/tools/check_shaders.py --write   regenerate demo/shaders.js

Called from `tools/verify.sh`. Exit code 1 means a copy has drifted.

------------------------------------------------------------------- why

`demo/shaders.js` holds the GLSL pieces of `source/shaders/*.cpp` -- the vertex
stage, the mirrored transport block (`kTbeFunctions`), the compander
(`kCompandFunctions`), the trace overlay and the three passes' mains, plus the
`kHeader` string every fragment starts with. That is two copies of the same
text, and two copies drift -- quietly, because a picture put through a
*plausible* tape looks exactly like one put through the right one. The page's
whole claim is that it runs the plugin's own shaders, so the claim needs
something enforcing it. `frtest` drives the real plugin class and has never
heard of this page.

------------------------------------------------------------------- what it does

Pulls each piece out of `source/shaders/*.cpp` and the matching backtick literal
out of `demo/shaders.js`, and compares them exactly -- no whitespace
normalisation, no comment stripping: a comment updated on one side only is
exactly the drift worth catching. `kHeader` is an ordinary string literal, so
its C escapes (`\\n`) are decoded first.

It fails when the shader files grow a raw string this script does not list (a
new pass the page would silently lack). The one raw string deliberately not on
the page is the unnamed `main()` of `TbeProbeFragment()` in Passes.cpp: the
`--tbe` probe, a test shader rather than a pass, so it is counted rather than
copied.

It also checks the JOIN. `EncodeFragment()`, `TapeFragment()` and
`DecodeFragment()` in Passes.cpp concatenate the pieces in a fixed order, and
`compileShaders()` in Ferric.cpp pairs each with `kVertex`. demo/ferric.js must
assemble them the same way; a change to either side's assembly fails here and
needs a reader.

The one transformation is a decode, not a normalisation: a backtick cannot
appear raw inside a template literal, so --write escapes one as \\`. This
undoes that escape and REJECTS any other backslash or any `${` on the JS side;
there are none in the C++ raw strings, so either could only be somebody hiding
a difference.

------------------------------------------------------------------- what it cannot

Nothing here checks the PORTED half: Transport.cpp (`phaseAt`, `error`,
`weightedPercent`), Controls.cpp, Compander.cpp's `uniforms`, Drive.cpp and the
frame logic of Ferric.cpp's ProcessOpenGL are a hand translation in
demo/ferric.js. demo/tools/crosscheck/, which verify.sh also runs, holds them
to the C++. When you change one of those, change the port too.
"""
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.abspath(os.path.join(HERE, "..", ".."))
sys.dont_write_bytecode = True

SHADER_FILES = [
    "source/shaders/Vertex.cpp",
    "source/shaders/Tbe.cpp",
    "source/shaders/Compand.cpp",
    "source/shaders/Passes.cpp",
]

# JS constant, C++ symbol, file -- in the order the files are listed above.
RAW = [
    ("VERTEX", "kVertex", "source/shaders/Vertex.cpp"),
    ("TBE_FUNCTIONS", "kTbeFunctions", "source/shaders/Tbe.cpp"),
    ("COMPAND_FUNCTIONS", "kCompandFunctions", "source/shaders/Compand.cpp"),
    ("ENCODE_MAIN", "kEncodeMain", "source/shaders/Passes.cpp"),
    ("TAPE_MAIN", "kTapeMain", "source/shaders/Passes.cpp"),
    ("DECODE_MAIN", "kDecodeMain", "source/shaders/Passes.cpp"),
    ("TRACE_FUNCTIONS", "kTraceFunctions", "source/shaders/Passes.cpp"),
]

# The ordinary string literal every fragment opens with.
PLAIN = [("HEADER", "kHeader", "source/shaders/Passes.cpp")]

# Raw strings that are deliberately not on the page: (file, how many unnamed).
UNNAMED = {"source/shaders/Passes.cpp": 1}  # TbeProbeFragment()'s main, the --tbe probe

# How the pieces are joined, on each side. Each entry is (file, text that must
# appear in it verbatim). The C++ lines are the plugin's assembly; the JS lines
# are ferric.js's copy of it.
ASSEMBLY = [
    ("source/shaders/Passes.cpp",
     "return std::string( kHeader ) + kCompandFunctions + kEncodeMain;"),
    ("source/shaders/Passes.cpp",
     "return std::string( kHeader ) + kTbeFunctions + kTapeMain;"),
    ("source/shaders/Passes.cpp",
     "return std::string( kHeader ) + kCompandFunctions + kTbeFunctions + kTraceFunctions + kDecodeMain;"),
    ("source/Ferric.cpp", '{ &encodeShader, shaders::EncodeFragment(), "encode" },'),
    ("source/Ferric.cpp", '{ &tapeShader, shaders::TapeFragment(), "tape" },'),
    ("source/Ferric.cpp", '{ &decodeShader, shaders::DecodeFragment(), "decode" },'),
    ("source/Ferric.cpp", "stage.shader->Compile( shaders::kVertex, stage.fragment.c_str() )"),
    ("demo/ferric.js", "const encodeFragment = S.HEADER + S.COMPAND_FUNCTIONS + S.ENCODE_MAIN;"),
    ("demo/ferric.js", "const tapeFragment = S.HEADER + S.TBE_FUNCTIONS + S.TAPE_MAIN;"),
    ("demo/ferric.js",
     "const decodeFragment = S.HEADER + S.COMPAND_FUNCTIONS + S.TBE_FUNCTIONS + S.TRACE_FUNCTIONS + S.DECODE_MAIN;"),
    ("demo/ferric.js", "encode: new Program(gl, S.VERTEX, encodeFragment, 'encode'),"),
    ("demo/ferric.js", "tape: new Program(gl, S.VERTEX, tapeFragment, 'tape'),"),
    ("demo/ferric.js", "decode: new Program(gl, S.VERTEX, decodeFragment, 'decode'),"),
]

HEADER = """// GENERATED from source/shaders/*.cpp by demo/tools/check_shaders.py --write.
// Do not edit: tools/verify.sh fails if a character of this differs from the
// plugin's. The one escape allowed is \\` for a backtick inside a comment.
"""


def read(path):
    with open(os.path.join(REPO, *path.split("/"))) as handle:
        return handle.read()


def raw_piece(source, symbol):
    match = re.search(
        r'(?:const char\* const|constexpr const char\*) ' + symbol + r' = R"\((.*?)\)";', source, re.S)
    return None if match is None else match.group(1)


def plain_piece(source, symbol):
    match = re.search(
        r'(?:const char\* const|constexpr const char\*) ' + symbol + r' = "((?:[^"\\\n]|\\.)*)";', source)
    if match is None:
        return None
    return match.group(1).encode().decode("unicode_escape")


def raw_inventory(source):
    """(named symbols, count of unnamed) for every raw string in a file."""
    named = re.findall(r'(?:const char\* const|constexpr const char\*) (k\w+) = R"\(', source)
    total = len(re.findall(r'R"\(', source))
    return named, total - len(named)


def js_piece(source, name):
    match = re.search(r'^export const ' + name + r' = `(.*?)`;$', source, re.S | re.M)
    if match is None:
        return None, None
    body = match.group(1)
    stray = re.search(r"\\(?!`)", body)
    if stray is not None:
        line = body[: stray.start()].count("\n") + 1
        return None, f"backslash that is not an escaped backtick, at line {line}"
    if "${" in body:
        return None, "template substitution"
    return body.replace("\\`", "`"), None


def js_plain(source, name):
    match = re.search(r"^export const " + name + r" = (\"(?:[^\"\\\n]|\\.)*\");$", source, re.M)
    if match is None:
        return None
    import json
    return json.loads(match.group(1))


def write():
    out = [HEADER]
    import json
    for name, symbol, path in PLAIN:
        body = plain_piece(read(path), symbol)
        if body is None:
            print(f"FAIL  {symbol} not found in {path}")
            return 1
        out.append(f"\n// {symbol}, {path} (an ordinary string literal, escapes decoded)\n"
                   f"export const {name} = {json.dumps(body)};\n")
    for name, symbol, path in RAW:
        body = raw_piece(read(path), symbol)
        if body is None:
            print(f"FAIL  {symbol} not found in {path}")
            return 1
        if "\\" in body or "${" in body:
            print(f"FAIL  {symbol} holds a backslash or ${{; the escape scheme cannot carry it")
            return 1
        out.append(f"\n// {symbol}, {path}\nexport const {name} = `{body.replace('`', chr(92) + '`')}`;\n")
    with open(os.path.join(REPO, "demo", "shaders.js"), "w") as handle:
        handle.write("".join(out))
    print("wrote demo/shaders.js")
    return 0


def first_difference(a, b):
    left, right = a.splitlines(), b.splitlines()
    for i in range(max(len(left), len(right))):
        x = left[i] if i < len(left) else "<missing>"
        y = right[i] if i < len(right) else "<missing>"
        if x != y:
            return i + 1, x, y
    return None


def check(js):
    problems = 0

    listed = {symbol for _, symbol, _ in RAW}
    for path in SHADER_FILES:
        named, unnamed = raw_inventory(read(path))
        for symbol in named:
            if symbol not in listed:
                print(f"FAIL  {path} defines {symbol}, which this check does not list -- "
                      "add it to RAW (and to the page, or say on the page why not)")
                problems += 1
        if unnamed != UNNAMED.get(path, 0):
            print(f"FAIL  {path} has {unnamed} unnamed raw string(s), expected {UNNAMED.get(path, 0)} "
                  "-- a new anonymous piece needs a reader")
            problems += 1

    for name, symbol, path in PLAIN:
        cpp_text = plain_piece(read(path), symbol)
        js_text = js_plain(js, name)
        if cpp_text is None or js_text is None:
            print(f"FAIL  {name} / {symbol} missing on one side")
            problems += 1
        elif cpp_text == js_text:
            print(f"ok    {name:<18} matches {symbol} ({len(cpp_text)} chars)")
        else:
            print(f"FAIL  {name} has drifted from {symbol}")
            problems += 1

    for name, symbol, path in RAW:
        cpp_text = raw_piece(read(path), symbol)
        js_text, complaint = js_piece(js, name)
        if cpp_text is None:
            print(f"FAIL  {symbol} not found in {path}")
            problems += 1
        elif complaint is not None:
            print(f"FAIL  {name} in demo/shaders.js has a {complaint}")
            problems += 1
        elif js_text is None:
            print(f"FAIL  {name} not found in demo/shaders.js")
            problems += 1
        elif cpp_text == js_text:
            print(f"ok    {name:<18} matches {symbol} ({len(cpp_text)} chars)")
        else:
            problems += 1
            print(f"FAIL  {name} has drifted from {symbol}")
            where = first_difference(cpp_text, js_text)
            if where:
                print(f"        first difference at line {where[0]}")
                print(f"          C++: {where[1]}")
                print(f"          js : {where[2]}")

    for path, text in ASSEMBLY:
        try:
            body = read(path)
        except FileNotFoundError:
            print(f"FAIL  {path} is missing")
            problems += 1
            continue
        if text in body:
            print(f"ok    assembly: {path} still says {text[:64]}")
        else:
            print(f"FAIL  assembly: {path} no longer says: {text}")
            problems += 1
    return problems


def main(argv):
    if "--write" in argv:
        return write()
    try:
        js = read("demo/shaders.js")
    except FileNotFoundError:
        print("FAIL  demo/shaders.js is missing -- run demo/tools/check_shaders.py --write")
        return 1
    problems = check(js)
    print()
    if problems:
        print(f"{problems} problem(s) -- rerun demo/tools/check_shaders.py --write for a drifted copy "
              "(never edit shaders.js by hand); an assembly or unlisted-piece failure needs a reader")
        return 1
    print(f"all {len(RAW) + len(PLAIN)} shader pieces and their assembly are the plugin's")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
