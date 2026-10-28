"""Frames drawn by the page against frames drawn by the plugin's own harness.

    python3 demo/tools/crosscheck/pixels.py --base http://127.0.0.1:PORT/ [--frtest build/frtest]
                                            [--gl metal|swiftshader] [--out DIR]

Needs `demo/` served at --base (any static server; `python3 -m http.server`
from demo/ will do), a built `frtest`, and Google Chrome. Not run by verify.sh:
it needs a browser and a GPU, and its answer is a measurement to read, not a
pass/fail. AGENTS.md records what it said.

For each case: frtest's own test card goes through `frtest --pipe` -- the REAL
plugin class, in a GL 4.1 context, 60 frames at 60 fps -- and through the
page's renderer (demo/tools/crosscheck/pixels.mjs, which draws with
demo/ferric.js exactly as the page does) in headless Chrome, same frames, same
clock, same size. The last frame of each is compared pixel for pixel.

What is held equal and what is not:
  * frtest's pipe writes a synthetic spectrum and a 120 BPM transport into the
    plugin every frame. The cases leave every reactive depth at zero and Show
    Trace off, where the spectrum changes nothing, so the comparison is fair.
  * The GPUs differ: the plugin runs on macOS's OpenGL, the page on Chrome's
    ANGLE (Metal or SwiftShader). `sin()` and texture filtering are each
    driver's own, so a few levels of 255 here and there are expected; a whole
    region that differs is not.

What it said on 2026-10-28 (M4 Max, ANGLE Metal) is in AGENTS.md: eight of the
nine cases with every pixel within 1/255, and Head Clog off by up to 3/255
because the Apple Silicon build of the plugin fuses one multiply-add in the hiss
width. Do not add a case that sets Preset AND a control it covers: the plugin's
override rule keeps the preset's value (AGENTS.md, "Traps met building it").

Uses stoatworks-backend's release/cdpshot.py for the DevTools socket
(STOATWORKS_BACKEND overrides where that checkout is).
"""
import argparse
import base64
import io
import json
import os
import subprocess
import sys
import tempfile
import time

sys.dont_write_bytecode = True
BACKEND = os.environ.get("STOATWORKS_BACKEND", os.path.expanduser("~/Projects/infrastructure/stoatworks-backend"))
sys.path.insert(0, os.path.join(BACKEND, "release"))
from cdpshot import WS, free_port  # noqa: E402

from PIL import Image  # noqa: E402

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.abspath(os.path.join(HERE, "..", "..", ".."))
CHROME = os.environ.get("CHROME", "/Applications/Google Chrome.app/Contents/MacOS/Google Chrome")
GL = {
    "swiftshader": ["--use-gl=angle", "--use-angle=swiftshader", "--enable-unsafe-swiftshader"],
    "metal": ["--use-gl=angle", "--use-angle=metal", "--enable-gpu", "--ignore-gpu-blocklist"],
}

W, H, FRAMES, FPS = 640, 360, 60, 60

# (name, frtest cue lines, page host values, page preset element)
CASES = [
    ("defaults", [], {}, 0),
    ("Clean Deck", [("Preset", 1)], {}, 1),
    ("Chewed Tape", [("Preset", 3)], {}, 3),
    ("Undecoded", [("Preset", 4)], {}, 4),
    ("Wrong Deck", [("Preset", 5)], {}, 5),
    ("Head Clog", [("Preset", 6)], {}, 6),
    ("Ribbons", [("Preset", 7)], {}, 7),
    ("Type C, Encode Only, heavy flutter",
     [("NR Type", 2), ("NR Mode", 2), ("Flutter", 0.9), ("Tape Speed", 0.2), ("Amount", 0.5), ("Machine", 2)],
     {"nrType": 2, "nrMode": 2, "flutter": 0.9, "tapeSpeed": 0.2, "amount": 0.5, "machine": 2}, 0),
    ("Mix half, vertical roll",
     [("Mix", 0.5), ("Vertical", 1.0), ("Wow", 0.9), ("Drift", 0.8), ("Amount", 0.6)],
     {"mix": 0.5, "vertical": 1.0, "wow": 0.9, "drift": 0.8, "amount": 0.6}, 0),
]


class Chrome:
    def __init__(self, gl):
        self.profile = tempfile.mkdtemp(prefix="ferric-pixels-")
        self.port = free_port()
        self.proc = subprocess.Popen(
            [CHROME, "--headless=new", "--no-first-run", "--no-default-browser-check", *GL[gl],
             f"--user-data-dir={self.profile}", f"--remote-debugging-port={self.port}", "about:blank"],
            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        import http.client
        deadline = time.time() + 30
        url = None
        while time.time() < deadline and url is None:
            try:
                conn = http.client.HTTPConnection("127.0.0.1", self.port, timeout=2)
                conn.request("GET", "/json/list")
                for t in json.loads(conn.getresponse().read()):
                    if t.get("type") == "page" and t.get("webSocketDebuggerUrl"):
                        url = t["webSocketDebuggerUrl"]
            except Exception:
                time.sleep(0.3)
        self.ws = WS(url)
        self.n = 0

    def call(self, method, params=None):
        self.n += 1
        want = self.n
        self.ws.send(json.dumps({"id": want, "method": method, "params": params or {}}))
        while True:
            msg = json.loads(self.ws.recv())
            if msg.get("id") == want:
                if "error" in msg:
                    raise RuntimeError(f"{method}: {msg['error']}")
                return msg.get("result", {})

    def evaluate(self, expression):
        r = self.call("Runtime.evaluate", {"expression": expression, "returnByValue": True, "awaitPromise": True})
        if "exceptionDetails" in r:
            raise RuntimeError(json.dumps(r["exceptionDetails"])[:600])
        return r.get("result", {}).get("value")

    def close(self):
        self.ws.close()
        self.proc.terminate()
        self.proc.wait(timeout=10)
        import shutil
        shutil.rmtree(self.profile, ignore_errors=True)


def frtest_frames(frtest, card, cues, work):
    script = os.path.join(work, "cues.txt")
    with open(script, "w") as handle:
        for name, value in cues:
            handle.write(f"0 {name} {value}\n")
    result = subprocess.run([frtest, "--pipe", "--size", f"{W}x{H}", "--fps", str(FPS), "--script", script],
                            input=card * FRAMES, capture_output=True, check=True)
    size = W * H * 4
    out = result.stdout
    if len(out) != size * FRAMES:
        raise RuntimeError(f"frtest gave {len(out)} bytes, wanted {size * FRAMES}: {result.stderr.decode()[-300:]}")
    return out[size * (FRAMES - 1):]


def compare(a, b):
    worst = 0
    over1 = over4 = over16 = 0
    for i in range(0, len(a), 4):
        d = max(abs(a[i] - b[i]), abs(a[i + 1] - b[i + 1]), abs(a[i + 2] - b[i + 2]), abs(a[i + 3] - b[i + 3]))
        if d > worst:
            worst = d
        if d > 1:
            over1 += 1
        if d > 4:
            over4 += 1
        if d > 16:
            over16 += 1
    return worst, over1, over4, over16


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--base", required=True)
    ap.add_argument("--frtest", default=os.path.join(REPO, "build", "frtest"))
    ap.add_argument("--gl", default="metal", choices=sorted(GL))
    ap.add_argument("--out", default=None, help="write each pair and a difference image here")
    args = ap.parse_args()

    work = tempfile.mkdtemp(prefix="ferric-pixels-")
    card_png = os.path.join(work, "card.png")
    subprocess.run([args.frtest, "--scene", card_png, "--size", f"{W}x{H}"], check=True, capture_output=True)
    card = Image.open(card_png).convert("RGBA").tobytes()

    chrome = Chrome(args.gl)
    try:
        chrome.call("Page.enable")
        chrome.call("Page.navigate", {"url": args.base.rstrip("/") + "/tools/crosscheck/pixels.html"})
        deadline = time.time() + 30
        while time.time() < deadline:
            try:
                if chrome.evaluate("window.ferricPixelsReady === true"):
                    break
            except RuntimeError:
                pass
            time.sleep(0.25)
        rc = 0
        pixels = W * H
        for name, cues, host, preset in CASES:
            plugin = frtest_frames(args.frtest, card, cues, work)
            job = json.dumps({"rgba": base64.b64encode(card).decode(), "width": W, "height": H, "host": host,
                              "preset": preset, "frames": FRAMES, "fps": FPS})
            got = chrome.evaluate(f"window.ferricPixels({job})")
            page = base64.b64decode(got["shots"][str(FRAMES - 1)])
            worst, over1, over4, over16 = compare(plugin, page)
            same = 100.0 * (pixels - over1) / pixels
            print(f"{name:<36} {same:7.3f}% within 1/255   >4: {over4:6d}   >16: {over16:6d}   worst {worst:3d}/255")
            if args.out:
                os.makedirs(args.out, exist_ok=True)
                slug = name.lower().replace(" ", "-").replace(",", "")
                Image.frombytes("RGBA", (W, H), plugin).save(os.path.join(args.out, f"{slug}-plugin.png"))
                Image.frombytes("RGBA", (W, H), page).save(os.path.join(args.out, f"{slug}-page.png"))
                diff = bytes(min(255, 8 * abs(plugin[i] - page[i])) if i % 4 != 3 else 255 for i in range(len(page)))
                Image.frombytes("RGBA", (W, H), diff).save(os.path.join(args.out, f"{slug}-diff.png"))
        print(f"page on: {got['renderer']}")
        return rc
    finally:
        chrome.close()


if __name__ == "__main__":
    sys.exit(main())
