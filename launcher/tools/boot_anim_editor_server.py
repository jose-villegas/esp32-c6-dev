#!/usr/bin/env python3
"""Local dev server for tools/boot_anim_editor.html - real C-code rendering.

    python tools/boot_anim_editor_server.py [port]

Serves the editor page and answers POST /render with an actual frame of the
boot animation, rendered by the REAL firmware code (main/gfx/gfx.c +
main/boot/boot_anim.c, unmodified drawing logic - see their own
ESP_PLATFORM comments for how they compile on a host at all) rather than a
JS reimplementation of it.

HOW A REQUEST IS HANDLED

POST /render body is {"timing": {...}, "camera_focal": 512, "grid_step_m":
0.25, "wave_height_m": 0, "wave_decay_m": 1, "wave_ease": "linear",
"keyframes": [...], "ms": 1234} - boot_anim_timeline.json's own shape
(PAYLOAD_KEYS below), plus which millisecond to show. Handled and passed
around as one `payload` dict throughout this file, not one parameter per
field - adding a field to the JSON means adding it to PAYLOAD_KEYS, nothing
else here.

  1. Hash `payload` (PAYLOAD_KEYS only - not `ms`). If it matches the last
     build, skip straight to step 4 - scrubbing/playback (ms alone
     changing) never recompiles, it is one process spawn of an
     already-built binary.
  2. Otherwise: write that JSON to a SCRATCH copy of boot_anim_timeline.json
     (never the real, committed one - see build_and_flash() below for the
     one thing here that does write it) and run
     tools/gen_boot_anim_timeline.py against it. A validation failure there
     (curve still drawing when the fade starts, etc.) is reported back as a
     400 with the generator's own message.
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

POST /build_flash is the only other endpoint, and the only thing here that
writes anything meant to be committed or touches the real device: same body
shape as /render minus `ms`, plus `port` (a serial port, e.g. "COM3"). It
validates exactly like step 2 above (into a throwaway scratch file first, so
a bad edit never reaches the real files), then overwrites the REAL
main/boot/boot_anim_timeline.json and boot_anim_timeline.h - what
tools/boot_anim_editor.html's Bake button downloads, written here instead of
copied in by hand - and runs tools/build_flash_dev.sh, which builds the
development image and flashes it to `port`. Its combined stdout/stderr comes
back as the response body (200) or as the error (500) if the build or the
flash failed.

Single-threaded on purpose: this is a local, single-user tool, and every
render already serializes through one compiler/one binary anyway. A
/build_flash request blocks the server for as long as the build+flash
takes (tens of seconds) - nothing else this tool does needs to run at the
same time.
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

# The REAL, committed files - see build_and_flash()'s own comment on why
# these, unlike everything render() touches, are not disposable.
TIMELINE_JSON = os.path.join(MAIN_DIR, "boot", "boot_anim_timeline.json")
TIMELINE_HEADER = os.path.join(MAIN_DIR, "boot", "boot_anim_timeline.h")
BUILD_FLASH_SCRIPT = os.path.join(TOOLS_DIR, "build_flash_dev.sh")
BUILD_FLASH_TIMEOUT_S = 300

DEFAULT_PORT = 8934

# boot_anim_timeline.json's own top-level keys, minus "ms"/"port" which are
# per-request rather than per-timeline. The one place a new top-level field
# (wave_decay_m, grid_spokes's own parent "timing", ...) needs to be added
# for it to reach the renderer at all.
PAYLOAD_KEYS = ("timing", "camera_focal", "grid_step_m", "wave_height_m",
               "wave_decay_m", "wave_ease", "keyframes")


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


def find_bash():
    """build_flash_dev.sh is POSIX sh, written to run under Git Bash (see its
    own top comment) - idf.py itself cannot run under Git Bash on Windows
    (see the project's CLAUDE.md), but build_flash.sh already routes around
    that itself (tools/idf.sh -> idf_shim.bat), so running the .sh under
    Git Bash's own bash.exe is the one thing this needs to get right."""
    found = shutil.which("bash")
    if found:
        return found
    for candidate in (r"C:\Program Files\Git\bin\bash.exe",
                      r"C:\Program Files\Git\usr\bin\bash.exe"):
        if os.path.isfile(candidate):
            return candidate
    return None


class RenderError(Exception):
    def __init__(self, status, message):
        super().__init__(message)
        self.status = status
        self.message = message


class Renderer:
    """Owns the scratch directory and the cached compiled binary."""

    def __init__(self, cc, port):
        self.cc = cc
        # A FIXED name/location per PORT, not tempfile.mkdtemp()'s random
        # one each run: a stray .exe freshly written under a Windows
        # Defender-watched path (the whole system temp dir is) gets scanned
        # on execution, sometimes costing the better part of a second - a
        # real-world measurement, not a guess (see this script's own commit
        # message). A stable path is what makes a ONE-TIME exclusion
        # (Add-MpPreference -ExclusionPath, run by the user, never this
        # script - that is a system security setting) actually pay off
        # across restarts instead of needing to be redone every time.
        # Keyed by port, not shared across every instance: two servers
        # running at once (e.g. verifying a change on an alternate port
        # without disturbing a live session on the default one) used to
        # both point at the SAME directory - the second one's startup
        # rmtree() below would delete the first one's already-compiled
        # binary out from under it mid-session, breaking its very next
        # render with a plain FileNotFoundError.
        self.scratch = os.path.join(
            tempfile.gettempdir(), "boot_anim_editor_scratch_%d" % port)
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

    def _regenerate_and_compile(self, payload_hash, payload):
        scratch_json = os.path.join(self.scratch, "boot_anim_timeline.json")
        scratch_header = os.path.join(self.scratch, "boot", "boot_anim_timeline.h")

        with open(scratch_json, "w", encoding="utf-8") as f:
            json.dump(payload, f)

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

    def render(self, payload, ms):
        """`payload` is exactly boot_anim_timeline.json's own shape (see
        PAYLOAD_KEYS) - one dict, not one parameter per top-level field, so
        a new one of those (wave_decay_m, say) never means touching this
        signature again. Returns (bmp_bytes, (origin_x, origin_y) or
        None)."""
        payload_hash = hashlib.sha256(
            json.dumps(payload, sort_keys=True).encode("utf-8")).hexdigest()

        if payload_hash != self.built_hash:
            self._regenerate_and_compile(payload_hash, payload)

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

    def build_and_flash(self, payload, port):
        """Overwrites the REAL main/boot/boot_anim_timeline.json and
        boot_anim_timeline.h - unlike render()'s scratch copy, not
        disposable - then builds and flashes the development image. Returns
        the build+flash log (str) on success; raises RenderError otherwise.

        Validated into a THROWAWAY scratch file first, the same way render()
        validates into the scratch directory - a bad edit (the curve still
        drawing when the fade starts, say) must never leave the committed
        json/header half-written."""
        fd, scratch_json = tempfile.mkstemp(suffix=".json",
                                            prefix="boot_anim_timeline_")
        try:
            with os.fdopen(fd, "w", encoding="utf-8") as f:
                json.dump(payload, f)
            gen = subprocess.run(
                [sys.executable, GENERATOR, scratch_json],
                capture_output=True, text=True)
            if gen.returncode != 0:
                raise RenderError(400, gen.stderr.strip() or
                                  "gen_boot_anim_timeline.py failed with no message")
            header_text = gen.stdout
        finally:
            os.remove(scratch_json)

        with open(TIMELINE_JSON, "w", encoding="utf-8") as f:
            json.dump(payload, f, indent=4)
            f.write("\n")
        with open(TIMELINE_HEADER, "w", encoding="utf-8", newline="\n") as f:
            f.write(header_text)

        bash = find_bash()
        if bash is None:
            raise RenderError(
                500, "no bash.exe found - build_flash_dev.sh needs Git Bash "
                "(see the project's CLAUDE.md). The timeline files were "
                "still written to main/boot/ above.")

        # stdin=DEVNULL: build_flash_dev.sh ends with an interactive "press
        # Enter to close" (it doubles as a double-clickable script) that
        # would otherwise hang this request forever - see its own comment
        # on the `|| true` that makes stdin-at-EOF there a no-op, not a
        # reported failure.
        proc = subprocess.run(
            [bash, BUILD_FLASH_SCRIPT, port],
            cwd=LAUNCHER_DIR, stdin=subprocess.DEVNULL,
            capture_output=True, text=True, timeout=BUILD_FLASH_TIMEOUT_S)
        log = proc.stdout + proc.stderr
        if proc.returncode != 0:
            raise RenderError(500, log.strip() or
                              "build_flash_dev.sh exited with code %d" %
                              proc.returncode)
        return log


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
        if self.path == "/render":
            self._do_render()
        elif self.path == "/build_flash":
            self._do_build_flash()
        else:
            self.send_error(404)

    def _do_render(self):
        length = int(self.headers.get("Content-Length", "0"))
        try:
            body = json.loads(self.rfile.read(length) or b"{}")
            payload = {key: body[key] for key in PAYLOAD_KEYS}
            ms = body["ms"]
        except (ValueError, KeyError) as exc:
            self._send_json_error(400, "bad request body: %s" % exc)
            return

        try:
            bmp, origin = self.renderer.render(payload, ms)
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

    def _do_build_flash(self):
        length = int(self.headers.get("Content-Length", "0"))
        try:
            body = json.loads(self.rfile.read(length) or b"{}")
            payload = {key: body[key] for key in PAYLOAD_KEYS}
            port = body.get("port") or "COM3"
        except (ValueError, KeyError) as exc:
            self._send_json_error(400, "bad request body: %s" % exc)
            return

        try:
            log = self.renderer.build_and_flash(payload, port)
        except RenderError as exc:
            self._send_json_error(exc.status, exc.message)
            return
        except subprocess.TimeoutExpired:
            self._send_json_error(
                500, "build_flash_dev.sh did not finish within %ds" %
                BUILD_FLASH_TIMEOUT_S)
            return
        except Exception as exc:   # noqa: BLE001 - surfaced to the browser
            self._send_json_error(500, "%s: %s" % (type(exc).__name__, exc))
            return

        body = json.dumps({"log": log}).encode("utf-8")
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

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

    Handler.renderer = Renderer(cc, port)

    server = http.server.HTTPServer(("127.0.0.1", port), Handler)
    print("boot_anim_editor_server: http://127.0.0.1:%d/" % port, file=sys.stderr)
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
