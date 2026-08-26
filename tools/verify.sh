#!/usr/bin/env bash
#
# Everything that can be checked without a human, in one command.
#
#     tools/verify.sh
#
# ---------------------------------------------------------------- the point
#
# Part of this file checks things the RELEASE job checks, and that is
# deliberate. It is the fleet's most expensive lesson: **a check that only ever
# runs in CI, after a tag, is a check that will catch you after the tag.** The
# bundle layout, the plist, the architectures and the signature can all be
# verified here in a second; the alternative is a failed release and a
# force-moved v0.1.0.
#
# The two that have actually bitten this fleet, both in repos started by copying
# another one -- which is exactly how this repo started:
#
#   * `CFBundleExecutable` carrying the PREVIOUS plugin's name, because a plist
#     template was copied. Nothing fails: the bundle assembles, the binary is
#     universal, `nm` finds the entry point, a probe renders a correct frame.
#     Then codesign says "code object is not signed at all" and mentions nothing
#     about a plist.
#
#   * A macOS build that is quietly arm64-only, because CMAKE_OSX_ARCHITECTURES
#     was set after the first target existed. The build log calls that a
#     success. Only `lipo` knows.
#
set -uo pipefail

cd "$(dirname "$0")/.."

PASS=0
FAIL=0
SKIP=0

ok()    { printf '  \033[32mok\033[0m    %s\n' "$1"; PASS=$((PASS+1)); }
bad()   { printf '  \033[31mFAIL\033[0m  %s\n' "$1"; FAIL=$((FAIL+1)); }
skip()  { printf '  \033[33mskip\033[0m  %s\n' "$1"; SKIP=$((SKIP+1)); }
head_() { printf '\n\033[1m%s\033[0m\n' "$1"; }

#---------------------------------------------------------------------------
# Every shader, through a real GLSL compiler, before a host has to find out.
#
# A shader that will not compile presents to an operator as "the effect does
# nothing", with the real message buried in the diagnostics log -- so without
# this it is caught at run time, in a host, or not at all.
#
# --target-env=opengl4.5 with -fauto-map-locations: glslc targets SPIR-V, which
# demands an explicit layout( location ) on every uniform and varying. Those are
# Vulkan rules and not GLSL ones, and without the flag every shader "fails" for
# reasons that have nothing to do with the code.
#
# glslc is optional -- `brew install shaderc` -- so a machine without it skips
# rather than fails.
#---------------------------------------------------------------------------
shaders_compile() {
    local dir bad=0 n=0 shader

    if ! command -v glslc >/dev/null 2>&1; then
        printf '   skipped: glslc not installed (brew install shaderc)\n'
        return 0
    fi

    dir="$( mktemp -d )"

    python3 - "$dir" <<'SHADERS_PY'
import re, sys, pathlib
out = pathlib.Path( sys.argv[ 1 ] )

# Where this repo keeps its GLSL.
FILES = [
	"source/shaders/Compand.cpp",
	"source/shaders/Passes.cpp",
	"source/shaders/Tbe.cpp",
	"source/shaders/Vertex.cpp",
]

# Shaders the plugin assembles at run time.
# Mirrors EncodeFragment()/TapeFragment()/DecodeFragment()/TbeProbeFragment()
# in Passes.cpp. 0 is the one unnamed raw string in that file -- the probe's main().
ASSEMBLED = {
	"EncodeFragment":   [ "kHeader", "kCompandFunctions", "kEncodeMain" ],
	"TapeFragment":     [ "kHeader", "kTbeFunctions", "kTapeMain" ],
	"DecodeFragment":   [ "kHeader", "kCompandFunctions", "kTbeFunctions", "kTraceFunctions", "kDecodeMain" ],
	"TbeProbeFragment": [ "kHeader", "kTbeFunctions", 0 ],
}

named, unnamed = {}, []
for f in FILES:
	text = pathlib.Path( f ).read_text()
	for m in re.finditer( r'(?:(\w+)\s*(?:\[\s*\])?\s*=\s*)?R"\((.*?)\)"', text, re.S ):
		if m.group( 1 ): named[ m.group( 1 ) ] = m.group( 2 )
		else:            unnamed.append( m.group( 2 ) )
	for m in re.finditer( r'(\w+)\s*=\s*((?:"(?:[^"\\\n]|\\.)*"\s*)+);', text ):
		named.setdefault( m.group( 1 ), "".join(
			s.encode().decode( "unicode_escape" )
			for s in re.findall( r'"((?:[^"\\\n]|\\.)*)"', m.group( 2 ) ) ) )

def emit( name, body ):
	# The vertex shader is the one that writes gl_Position; everything else is a
	# fragment shader. glslc takes the stage from the extension.
	ext = ".vert" if re.search( r"\bgl_Position\s*=", body ) else ".frag"
	( out / ( name + ext ) ).write_text( body )

def piece( p ):
	# An int indexes the raw strings that are not assigned to a name, in source
	# order. A literal starts with #version. Anything else names a constant
	# above -- and a name that has moved is a KeyError here, not a silent skip.
	if isinstance( p, int ):       return unnamed[ p ]
	if p.startswith( "#version" ): return p
	return named[ p ]

for name, body in named.items():
	if body.lstrip().startswith( "#version" ) and "void main" in body:
		emit( name, body )

for name, parts in ASSEMBLED.items():
	emit( name, "".join( piece( p ) for p in parts ) )
SHADERS_PY

    for shader in "$dir"/*.vert "$dir"/*.frag; do
        [ -e "$shader" ] || continue
        n=$(( n + 1 ))
        if ! glslc --target-env=opengl4.5 -fauto-map-locations \
               "$shader" -o /dev/null 2>"$dir/err"; then
            printf '   %s does not compile\n' "$( basename "$shader" )"
            sed "s|$dir/||; s|^|      |" "$dir/err"
            bad=$(( bad + 1 ))
        fi
    done

    if [ "$n" -eq 0 ]; then
        # No shaders at all is a FAILURE, not a pass. It means the extraction
        # above has lost track of where this repo keeps its GLSL, and a check
        # that silently looks at nothing is worse than no check.
        printf '   no shaders were extracted -- the extraction has gone stale\n'
        rm -rf "$dir"
        return 1
    fi

    if [ "$bad" -eq 0 ]; then
        printf '   %d shaders, all compile\n' "$n"
    fi
    rm -rf "$dir"
    return "$bad"
}

#---------------------------------------------------------------------------
head_ "Shaders"
#---------------------------------------------------------------------------
if shaders_compile; then
    ok "every shader compiles"
else
    bad "a shader does not compile"
fi

# ---------------------------------------------------------------------------
head_ "Build (universal)"
# ---------------------------------------------------------------------------
# Its OWN build directory, and the architectures stated explicitly.
#
# Both halves matter. `cmake -B build` on an existing tree REUSES the cache, so a
# dev build configured with -DCMAKE_OSX_ARCHITECTURES=arm64 stays arm64 forever:
# CMakeLists only applies the universal default when the variable is unset, and
# in a warm cache it is set. Running this against a dev tree would verify the
# wrong binary -- and the failure would look like a broken build rather than a
# stale cache.
#
# A verify script should assert the configuration it claims to verify, not hope a
# default survived.
BUILD=build-verify

if cmake -B "$BUILD" -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_OSX_ARCHITECTURES="arm64;x86_64" >/tmp/ferric-configure.log 2>&1 \
   && cmake --build "$BUILD" >/tmp/ferric-build.log 2>&1; then
    ok "configured and built"
else
    bad "the build failed -- see /tmp/ferric-configure.log and /tmp/ferric-build.log"
    tail -20 /tmp/ferric-build.log
    exit 1
fi

BUNDLE="$BUILD/Ferric.bundle"
FFGL_BIN="$BUNDLE/Contents/MacOS/Ferric"

# ---------------------------------------------------------------------------
head_ "The bundle"
# ---------------------------------------------------------------------------
if [ -f "$FFGL_BIN" ]; then
    ok "the binary is where the bundle says it is"
else
    bad "no binary at $FFGL_BIN"
fi

archs=$(lipo -archs "$FFGL_BIN" 2>/dev/null)
if [ "$archs" = "x86_64 arm64" ] || [ "$archs" = "arm64 x86_64" ]; then
    ok "universal ($archs)"
else
    bad "NOT universal -- lipo says '$archs'"
fi

# ☠️ Neither of these uses `grep -q`, and that is not a style choice.
#
# This script runs under `set -o pipefail`. `grep -q` exits the moment it finds
# a match, which closes the pipe, which kills the upstream command with SIGPIPE,
# which makes the PIPELINE fail -- so the `if` takes the else branch precisely
# when the thing being looked for IS there. The build-stamp check was written
# that way first and reported a missing stamp on a binary that plainly had one:
# a check that fails only on success, which is the worst possible direction for
# a check to be wrong in.
#
# It bites in proportion to how much output the upstream command has left to
# write, so `nm` on a small symbol table survived it and `strings` on a
# universal binary did not. That is why the working one was not evidence of
# anything.
#
# Capture, then match.
symbols=$(nm -gU "$FFGL_BIN" 2>/dev/null)
case "$symbols" in
    *_plugMain*) ok "exports plugMain" ;;
    *)           bad "does NOT export plugMain -- the host will load nothing" ;;
esac

# The version stamp, so a shipped binary can be identified after the fact.
stamp=$(strings "$FFGL_BIN" 2>/dev/null | grep -m1 '^ferric [0-9]')
if [ -n "$stamp" ]; then
    ok "carries a build stamp ($stamp)"
else
    bad "no build stamp -- a shipped bundle could not be identified"
fi

# ⚠️ The copied-plist trap. CFBundleExecutable must name the binary that is
# actually there; a stale one from the donor repo passes everything else and
# fails codesign with a message that mentions no plist.
declared=$(/usr/libexec/PlistBuddy -c "Print :CFBundleExecutable" "$BUNDLE/Contents/Info.plist" 2>/dev/null)
if [ "$declared" = "Ferric" ]; then
    ok "CFBundleExecutable names the binary on disk"
else
    bad "CFBundleExecutable is '$declared' but the binary is 'Ferric'"
    echo "        this passes every build and every test, and fails codesign at release"
fi

ident=$(/usr/libexec/PlistBuddy -c "Print :CFBundleIdentifier" "$BUNDLE/Contents/Info.plist" 2>/dev/null)
if [ "$ident" = "com.stoatworks.ffgl.ferric" ]; then
    ok "bundle identifier is this plugin's"
else
    bad "bundle identifier is '$ident'"
fi

# The exact command the release job runs, against a COPY, so a local run does
# not leave a signature on the build tree.
SIGDIR=$(mktemp -d)
cp -R "$BUNDLE" "$SIGDIR/" 2>/dev/null
if codesign --force --sign - "$SIGDIR/Ferric.bundle" >/dev/null 2>&1 \
   && codesign --verify "$SIGDIR/Ferric.bundle" >/dev/null 2>&1; then
    ok "signs and verifies ad-hoc"
else
    bad "codesign refused the bundle -- the release job will fail after the tag"
fi
rm -rf "$SIGDIR"

# ---------------------------------------------------------------------------
head_ "Parameter names"
# ---------------------------------------------------------------------------
# ⚠️ FFGL's legacy FF_GET_PARAMETER_NAME gives the host a 16-character buffer
# and the SDK does not enforce it: the plugin stores the whole string, hands over
# a pointer to all of it, and Resolume copies sixteen. Every offline harness
# passes, --list prints the full name, and only the host is ever wrong. Six
# plugins in this fleet shipped a control called `Background Opaci`.
listing=$("$BUILD/frtest" --list 2>/dev/null)
long=$(printf '%s\n' "$listing" | grep -c 'TRUNCATED' || true)
if [ "$long" = "0" ]; then
    ok "no parameter name is longer than 16 characters"
else
    bad "$long parameter name(s) will be truncated by Resolume"
    printf '%s\n' "$listing" | grep 'TRUNCATED'
fi

# A name that is exactly 16 is indistinguishable, from the host's side, from one
# that was cut. Listing them is the review step, not a failure.
exact=$(printf '%s\n' "$listing" | grep 'exactly 16' || true)
if [ -n "$exact" ]; then
    printf '  \033[33mnote\033[0m  names at exactly 16 characters (complete, but check by eye):\n'
    echo "$exact" | sed 's/^/        /'
fi

# ---------------------------------------------------------------------------
head_ "Workflow flags"
# ---------------------------------------------------------------------------
# ☠️ Every `-D<NAME>` a workflow passes must be an option this project actually
# declares, or CMake ignores it with a warning nobody reads and the job builds
# something other than what was asked for.
#
# This repo was scaffolded from abomerration, whose release.yml had itself been
# renamed from tilter's. The rename used `s/\bTILTER_/.../`, and `\b` matches
# nothing between the `D` and the `T` of `-DTILTER_BUILD_TOOLS` — so the flag
# survived two renames and shipped here, where CMake duly reported "Manually-
# specified variables were not used by the project" into a green log.
#
# Found by a parallel session hitting the same thing in macroblock, where it was
# worse: there the ignored flag killed a whole job. Any repo forked from a
# sibling can have it.
if python3 - <<'PYFLAGS'
import re, pathlib, sys
wf = "".join(p.read_text() for p in pathlib.Path(".github/workflows").glob("*.yml"))
passed = set(re.findall(r"-D([A-Z][A-Z0-9_]*)", wf))
declared = set(re.findall(r"^option\(([A-Z][A-Z0-9_]*)",
                          pathlib.Path("CMakeLists.txt").read_text(), re.M))
# CMake's own, plus the toolchain variables the Windows job needs.
builtin = {"CMAKE_BUILD_TYPE", "CMAKE_OSX_ARCHITECTURES", "CMAKE_TOOLCHAIN_FILE",
           "CMAKE_INSTALL_PREFIX", "VCPKG_TARGET_TRIPLET", "BUILD_OFX"}
unknown = sorted(passed - declared - builtin)
if unknown:
    print("  unknown to CMakeLists.txt: " + ", ".join(unknown))
sys.exit(1 if unknown else 0)
PYFLAGS
then
    ok "every -D a workflow passes is a real option"
else
    bad "a workflow passes a -D this project does not declare -- CMake will ignore it"
fi

# ---------------------------------------------------------------------------
head_ "The harness"
# ---------------------------------------------------------------------------
for check in tbe weighting wf identity roundtrip denoise nr scan drive clock presets echo; do
    if "$BUILD/frtest" --"$check" >"/tmp/ferric-$check.log" 2>&1; then
        ok "$check"
    else
        bad "$check -- see /tmp/ferric-$check.log"
        grep -E '^(FAIL| )' "/tmp/ferric-$check.log" | head -8 | sed 's/^/        /'
    fi
done

# ---------------------------------------------------------------------------
head_ "Dead controls"
# ---------------------------------------------------------------------------
# None of the checks above can catch a control that is wired to nothing. A GLSL
# uniform whose name does not match the C++ is silently ignored --
# glGetUniformLocation returns -1 and glUniform(-1) is a documented no-op -- so a
# slider can be stone dead while everything compiles, links, loads and renders.
if python3 tools/sweep.py --binary "$BUILD/frtest" >/tmp/ferric-sweep.log 2>&1; then
    ok "every control changes the picture"
else
    bad "some controls do nothing -- see /tmp/ferric-sweep.log"
    grep -E 'DEAD|dead' /tmp/ferric-sweep.log | head -10 | sed 's/^/        /'
fi

# ---------------------------------------------------------------------------
head_ "Instantiation through plugMain"
# ---------------------------------------------------------------------------
# The only thing here that loads the built BUNDLE the way a host does, rather
# than driving the plugin class directly.
#
# ☠️ It is the only thing that can catch a missing SetTextParameter override. The
# SDK's instantiateGL sets every parameter's default on a fresh instance and
# DELETES the instance if any set returns FF_FAIL, and the base class's
# SetTextParameter is a stub returning exactly that -- so declaring the About
# text block without overriding it means no real host can instantiate the plugin
# at all, while every check above passes because they bypass plugMain entirely.
# It has shipped in this fleet before.
FFGLTEST="../resolume-ofx-bridge/build/ffgltest"
if [ -x "$FFGLTEST" ]; then
    if "$FFGLTEST" "$BUNDLE" >/tmp/ferric-ffgltest.log 2>&1 \
       && grep -Fc 'instantiated ok' /tmp/ferric-ffgltest.log >/dev/null \
       && grep -Fc 'ProcessOpenGL ok' /tmp/ferric-ffgltest.log >/dev/null; then
        ok "the bundle instantiates through plugMain and renders"
    else
        bad "the bundle does not instantiate through plugMain -- see /tmp/ferric-ffgltest.log"
        tail -10 /tmp/ferric-ffgltest.log | sed 's/^/        /'
    fi
else
    skip "ffgltest not built (../resolume-ofx-bridge) -- plugMain is UNVERIFIED"
fi

# ---------------------------------------------------------------------------
printf '\n\033[1m%d ok, %d failed, %d skipped\033[0m\n' "$PASS" "$FAIL" "$SKIP"
[ "$FAIL" -eq 0 ]
