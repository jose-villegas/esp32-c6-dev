#!/usr/bin/env python3
"""Local dev server for tools/boot_anim_editor.html - real C-code rendering.

    python tools/boot_anim_editor_server.py [port]

Serves the editor page and answers POST /render with an actual frame of the
boot animation, rendered by the REAL firmware code (main/gfx/gfx.c +
main/boot/boot_anim.c, unmodified drawing logic - see their own
ESP_PLATFORM comments for how they compile on a host at all) rather than a
JS reimplementation of it.

HOW A REQUEST IS HANDLED

POST /render body is {"timing": {...}, "camera_focal": 512, "keyframes":
[...], "ms": 1234} - boot_anim_timeline.json's own shape, plus which
millisecond to show.

  1. Hash {timing, camera_focal, keyframes}. If it matches the last build,
     skip straight to step 4 - scrubbing/playback (ms alone changing) never
     recompiles, it is one process spawn of an already-built binary.
  2. Otherwise: write that JSON to a SCRATCH copy of boot_anim_timeline.json
     (never the real, committed one - only this repo's tools/
     boot_anim_editor.html's own Bake button writes anything meant to be
     committed) and run tools/gen_boot_anim_timeline.py against it. A
     validation failure there (curve still drawing when the fade starts,
     etc.) is reported back as a 400 with the generator's own message.
  3. Compile tools/boot_anim_render_host.c + main/gfx/gfx.c +
     main/boot/boot_anim.c, with the scratch directory (holding the DRAFT
     boot_anim_timeline.h from step 2) and components/small3dlib/include
     (the camera/space transform math boot_anim.h now builds on) on the
     include path, the scratch one AHEAD of `main` so it shadows the real
     committed header without ever touching it. A compile failure is
     reported back as a 500 with the compiler's own stderr.
  4. Run the (cached or freshly built) binary with the requested `ms` and
     stream its stdout - a BMP - back as the response body, with the
     origin readout it printed to stderr (see boot_anim_render_host.c's
     own comment on that line) passed through as an X-Origin header.

Single-threaded on purpose: this is a local, single-user tool, and every
render already serializes through one compiler/one binary anyway.
"""

import hashlib
import http.server
import json
import os
import shutil
import subprocess
import sys
import tempfile

TOOLS_DIR = os.path.dirname(os.path.abspath(__file__))
LAUNCHER_DIR = os.path.dirname(TOOLS_DIR)
MAIN_DIR = os.path.join(LAUNCHER_DIR, "main")
SMALL3DLIB_DIR = os.path.join(LAUNCHER_DIR, "components", "small3dlib", "include")
EDITOR_HTML = os.path.join(TOOLS_DIR, "boot_anim_editor.html")
GENERATOR = os.path.join(TOOLS_DIR, "gen_boot_anim_timeline.py")

DEFAULT_PORT = 8934


def find_cc():
    """Mirrors tools/find_cc.sh's own search order - see its top comment."""
    env_cc = os.environ.get("CC")
    if env_cc:
        return env_cc
    for name in ("cc", "gcc", "clang"):
        found = shutil.which(name)
        if found:
            return found
    local_appdata = os.environ.get("LOCALAPPDATA")
    if local_appdata:
        winlibs = os.path.join(
            local_appdata, "Microsoft", "WinGet", "Packages",
            "BrechtSanders.WinLibs.POSIX.UCRT_Microsoft.Winget.Source_8wekyb3d8bbwe",
            "mingw64", "bin", "gcc.exe")
        if os.path.isfile(winlibs):
            return winlibs
    return None


class RenderError(Exception):
    def __init__(self, status, message):
        super().__init__(message)
        self.status = status
        self.message = message


class Renderer:
    """Owns the scratch directory and the cached compiled binary."""

    def __init__(self, cc):
        self.cc = cc
        # A FIXED name/location, not tempfile.mkdtemp()'s random one each
        # run: a stray .exe freshly written under a Windows Defender-watched
        # path (the whole system temp dir is) gets scanned on execution,
        # sometimes costing the better part of a second - a real-world
        # measurement, not a guess (see this script's own commit message).
        # A stable path is what makes a ONE-TIME exclusion
        # (Add-MpPreference -ExclusionPath, run by the user, never this
        # script - that is a system security setting) actually pay off
        # across restarts instead of needing to be redone every time.
        self.scratch = os.path.join(tempfile.gettempdir(), "boot_anim_editor_scratch")
        # Cleared and recreated on every server start - stale contents from
        # a previous run (or, worse, a previous run's still-running process
        # that got killed uncleanly) are exactly what produced the
        # FileNotFoundError this replaces: a fresh start means nothing here
        # can be stale.
        shutil.rmtree(self.scratch, ignore_errors=True)
        os.makedirs(os.path.join(self.scratch, "boot"), exist_ok=True)
        self.binary = os.path.join(
            self.scratch, "boot_anim_render_host.exe"
            if os.name == "nt" else "boot_anim_render_host")
        self.built_hash = None
        print("scratch dir:", self.scratch, file=sys.stderr)
        if os.name == "nt":
            print(
                "  Windows Defender scans a freshly-run .exe here on every "
                "launch, which can cost the better part of a second per "
                "frame. For faster playback, run this once in an elevated "
                "PowerShell (admin - a system security setting, so it's "
                "yours to run, not this script's):\n"
                '    Add-MpPreference -ExclusionPath "%s"' % self.scratch,
                file=sys.stderr)

    def _regenerate_and_compile(self, payload_hash, timing, camera_focal, keyframes):
        scratch_json = os.path.join(self.scratch, "boot_anim_timeline.json")
        scratch_header = os.path.join(self.scratch, "boot", "boot_anim_timeline.h")

        with open(scratch_json, "w", encoding="utf-8") as f:
            json.dump({"timing": timing, "camera_focal": camera_focal,
                      "keyframes": keyframes}, f)

        gen = subprocess.run(
            [sys.executable, GENERATOR, scratch_json],
            capture_output=True, text=True)
        if gen.returncode != 0:
            raise RenderError(400, gen.stderr.strip() or
                              "gen_boot_anim_timeline.py failed with no message")
        with open(scratch_header, "w", encoding="utf-8", newline="\n") as f:
            f.write(gen.stdout)

        sources = [
            os.path.join(TOOLS_DIR, "boot_anim_render_host.c"),
            os.path.join(MAIN_DIR, "gfx", "gfx.c"),
            os.path.join(MAIN_DIR, "boot", "boot_anim.c"),
        ]
        cmd = [
            self.cc, "-std=c11", "-Wall", "-Wextra",
            "-Wno-unused-parameter", "-Wno-unused-function",
            "-Wno-unused-variable", "-O1",
            "-I", self.scratch, "-I", MAIN_DIR, "-I", SMALL3DLIB_DIR,
            *sources, "-o", self.binary,
        ]
        cc_result = subprocess.run(cmd, capture_output=True, text=True)
        if cc_result.returncode != 0:
            raise RenderError(500, cc_result.stderr.strip() or
                              "compile failed with no message")

        self.built_hash = payload_hash

    def render(self, timing, camera_focal, keyframes, ms):
        """Returns (bmp_bytes, (origin_x, origin_y) or None)."""
        payload_hash = hashlib.sha256(
            json.dumps({"timing": timing, "camera_focal": camera_focal,
                      "keyframes": keyframes},
                      sort_keys=True).encode("utf-8")).hexdigest()

        if payload_hash != self.built_hash:
            self._regenerate_and_compile(payload_hash, timing, camera_focal, keyframes)

        run = subprocess.run([self.binary, str(int(ms))], capture_output=True)
        if run.returncode != 0:
            raise RenderError(
                500,
                (run.stderr.decode("utf-8", "replace").strip() or
                 "renderer exited with code %d" % run.returncode))

        origin = None
        for line in run.stderr.decode("utf-8", "replace").splitlines():
            if line.startswith("ORIGIN "):
                parts = line.split()
                if len(parts) == 3:
                    origin = (int(parts[1]), int(parts[2]))
                break
        return run.stdout, origin


class Handler(http.server.BaseHTTPRequestHandler):
    renderer = None   # set in main()

    def log_message(self, fmt, *args):
        sys.stderr.write("%s - %s\n" % (self.address_string(), fmt % args))

    def do_GET(self):
        if self.path in ("/", "/index.html"):
            self._serve_file(EDITOR_HTML, "text/html; charset=utf-8")
            return
        self.send_error(404)

    def do_POST(self):
        if self.path != "/render":
            self.send_error(404)
            return

        length = int(self.headers.get("Content-Length", "0"))
        try:
            body = json.loads(self.rfile.read(length) or b"{}")
            timing = body["timing"]
            camera_focal = body["camera_focal"]
            keyframes = body["keyframes"]
            ms = body["ms"]
        except (ValueError, KeyError) as exc:
            self._send_json_error(400, "bad request body: %s" % exc)
            return

        try:
            bmp, origin = self.renderer.render(timing, camera_focal, keyframes, ms)
        except RenderError as exc:
            self._send_json_error(exc.status, exc.message)
            return
        except Exception as exc:   # noqa: BLE001 - surfaced to the browser
            self._send_json_error(500, "%s: %s" % (type(exc).__name__, exc))
            return

        self.send_response(200)
        self.send_header("Content-Type", "image/bmp")
        self.send_header("Content-Length", str(len(bmp)))
        if origin is not None:
            self.send_header("X-Origin", "%d,%d" % origin)
        self.end_headers()
        self.wfile.write(bmp)

    def _serve_file(self, path, content_type):
        with open(path, "rb") as f:
            data = f.read()
        self.send_response(200)
        self.send_header("Content-Type", content_type)
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    def _send_json_error(self, status, message):
        body = json.dumps({"error": message}).encode("utf-8")
        self.send_response(status)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)


def main():
    port = int(sys.argv[1]) if len(sys.argv) > 1 else DEFAULT_PORT

    cc = find_cc()
    if cc is None:
        sys.exit(
            "boot_anim_editor_server.py: no C compiler found.\n"
            "  Windows: winget install BrechtSanders.WinLibs.POSIX.UCRT\n"
            "  Debian:  sudo apt install build-essential\n"
            "  macOS:   xcode-select --install")
    print("using compiler:", cc, file=sys.stderr)

    Handler.renderer = Renderer(cc)

    server = http.server.HTTPServer(("127.0.0.1", port), Handler)
    print("boot_anim_editor_server: http://127.0.0.1:%d/" % port, file=sys.stderr)
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
