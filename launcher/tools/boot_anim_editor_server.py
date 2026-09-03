#!/usr/bin/env python3
"""Local dev server for tools/boot_anim_editor.html - real C-code rendering.

    python tools/boot_anim_editor_server.py [port]

Serves the editor page and answers POST /render with an actual frame of the
boot animation, rendered by the REAL firmware code (main/gfx/gfx.c +
main/boot/boot_anim.c, unmodified drawing logic - see their own
ESP_PLATFORM comments for how they compile on a host at all) rather than a
JS reimplementation of it.

HOW A REQUEST IS HANDLED

Step 0, before any of the below: if design/boot/boot.png is newer than the
committed main/boot/boot_anim_image.h, regenerate that header from it (see
_ensure_image_current()) - the one asset here that is not part of the
browser's own live payload, so there is no JSON to hash it against the way
step 1 hashes `payload`; an mtime comparison against the real, checked-in
header is what stands in for that. Unlike the timeline JSON below, this
writes the REAL header directly, not a scratch copy - there is no "draft"
concept for a photo, and regenerating in place is exactly what running
tools/gen_boot_anim_image.py by hand already does.

POST /render body is {"timing": {...}, "camera_focal": 512, "grid_step_m":
0.25, "wave_height_m": 0, "wave_wavelength_m": 0.75, "wave_period_ms": 3000,
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

GET /timeline returns the REAL, committed main/boot/boot_anim_timeline.json
verbatim - what the editor page fetches on open instead of relying solely on
its own hand-duplicated DEFAULT_STATE (see boot_anim_editor.html's own
comment on that constant), so opening the page always reflects what is
actually committed, and skipping Load before Build & Flash no longer
silently reverts it to a stale copy.

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

# The photograph half of the same "keep the generated header in sync"
# story, but with no live payload to compare it against - unlike the
# timeline, boot.png is a real file on disk, not something typed into
# the browser, so _ensure_image_current() below compares mtimes instead
# of hashing a request body. Always the REAL, committed header (never a
# scratch copy the way TIMELINE_HEADER gets one for a live-edited
# timeline) - there is no "draft" concept for a photo the way there is
# for in-progress timing values; regenerating in place is exactly what
# running the generator by hand already does.
GEN_IMAGE = os.path.join(TOOLS_DIR, "gen_boot_anim_image.py")
BOOT_PNG = os.path.join(os.path.dirname(LAUNCHER_DIR), "design", "boot", "boot.png")
BOOT_ANIM_IMAGE_HEADER = os.path.join(MAIN_DIR, "boot", "boot_anim_image.h")
BUILD_FLASH_SCRIPT = os.path.join(TOOLS_DIR, "build_flash_dev.sh")
# 300 measured too short in practice - a from-scratch (or even mostly-
# cached) dev build going through this script's own Git-Bash -> cmd-shim
# -> idf.py chain (see build_flash_dev.sh/idf.sh's own comments on why
# that chain exists at all) runs noticeably slower than the same build
# invoked directly, and blew past 300s on a machine with a real device
# attached. 600 is a guess at "generous enough", not a measurement of a
# worst case - raise it again if it is still not enough.
BUILD_FLASH_TIMEOUT_S = 600

DEFAULT_PORT = 8934

# boot_anim_timeline.json's own top-level keys, minus "ms"/"port" which are
# per-request rather than per-timeline. The one place a new top-level field
# (wave_wavelength_m, grid_spokes's own parent "timing", ...) needs to be
# added for it to reach the renderer at all.
PAYLOAD_KEYS = ("timing", "camera_focal", "grid_step_m", "wave_height_m",
               "wave_wavelength_m", "wave_period_ms", "keyframes")

# Every DIRECTORY the compile in _regenerate_and_compile() reads a .c or
# .h from - the render cache (Renderer.built_hash) is keyed on every file
# under these (see _watched_source_paths()) alongside the payload hash,
# precisely so that editing any of them (the actual point of this tool -
# iterating on boot_anim.c itself, not just the timeline) invalidates the
# cache instead of silently serving a binary built from whatever they
# looked like at the LAST payload change. A payload-only hash used to
# miss this entirely: editing boot_anim.c with the server already running
# and re-requesting the same `ms`/timeline kept hitting the cached
# binary, no different from a payload that had not changed.
#
# A whole-directory glob, not a hand-picked file list - an earlier version
# of this hard-coded the exact .c sources plus one header (boot_anim.h)
# that happened to matter at the time, which is exactly the kind of list
# that goes stale the next time a source gains a new #include: a
# forgotten entry would silently go back to never invalidating for that
# one file, the same bug this exists to fix. A narrower version of this
# same mistake, walking only main/boot|gfx|util, missed it again -
# boot_anim_render_host.c pulls in util/screenshot.h, which pulls in
# main/app.h (for app_t/input_t - see screenshot_dump()'s own signature),
# which pulls in main/input/buttons.h, none of them under those three
# subdirectories. The whole of MAIN_DIR (~80 files, trivial to stat every
# render) is the actual honest answer to "everything under here MIGHT be
# a compile dependency, so watch all of it" rather than trying to name
# every subdirectory this build's own transitive #includes happen to
# reach today - the same reasoning that makes this a glob instead of a
# file list in the first place, just not stopped short at the first
# level of it. SMALL3DLIB_DIR alongside it for the identical reason -
# boot_anim.h includes small3dlib.h directly (and it is on the compile's
# own -I path, per _regenerate_and_compile() below), so editing IT (a
# vendored, but still locally-modifiable, file) is exactly the kind of
# edit that should invalidate a cached render too. Not a real dependency
# scan (gcc -M and friends) either - simpler, and the false positives it
# costs (recompiling on an edit to an unrelated file elsewhere in the
# tree) are free on a tool nothing else times, unlike the false negative
# a narrower watch risks. Walked RECURSIVELY.
WATCHED_SOURCE_DIRS = (
    MAIN_DIR,
    SMALL3DLIB_DIR,
)

# Listed NON-recursively (see _watched_source_paths()) - unlike the above,
# tools/ also holds results/screenshots/sweeps/__pycache__ subdirectories
# (sweep/report scratch output, not source), so recursing here the same
# way would work but for the wrong reason - only boot_anim_render_host.c
# actually lives directly in this one.
WATCHED_TOP_LEVEL_DIRS = (TOOLS_DIR,)

# boot_anim_timeline.h under main/boot is the one file in that directory
# that must NOT be watched - it is the REAL, committed one (see
# TIMELINE_HEADER below), never what a render actually compiles against
# (the scratch copy in Renderer.scratch, rewritten fresh on every payload
# change and already covered by the payload hash itself). Watching it too
# would mean every edit to the committed timeline invalidates the cache
# for a render that never reads that file - harmless, but pointless
# extra compiles, and confusing to reason about when it fires.
WATCHED_EXCLUDE = frozenset([
    os.path.join(MAIN_DIR, "boot", "boot_anim_timeline.h"),
])


def _watched_source_paths():
    """Every .c/.h file under WATCHED_SOURCE_DIRS (recursively) and
    WATCHED_TOP_LEVEL_DIRS (top level only), minus WATCHED_EXCLUDE - see
    those constants' own comments for why a glob, not a fixed list."""
    paths = []
    for d in WATCHED_SOURCE_DIRS:
        for root, _dirs, files in os.walk(d):
            for name in files:
                if name.endswith((".c", ".h")):
                    path = os.path.join(root, name)
                    if path not in WATCHED_EXCLUDE:
                        paths.append(path)
    for d in WATCHED_TOP_LEVEL_DIRS:
        for name in os.listdir(d):
            path = os.path.join(d, name)
            if name.endswith((".c", ".h")) and os.path.isfile(path):
                if path not in WATCHED_EXCLUDE:
                    paths.append(path)
    return paths


def _source_signature(sources):
    """One hash covering every watched file's path AND mtime - order-
    stable (sorted) so the same set of files always signs the same way
    regardless of how `sources` was built. A missing file (renamed or
    deleted mid-session, after _watched_source_paths() already listed it)
    signs as absent rather than raising - the file no longer being there
    for the SIGNATURE isn't this function's problem to solve; if it was
    actually needed, the compile invocation itself is what surfaces
    that, as a normal compiler error."""
    parts = []
    for path in sorted(sources):
        try:
            parts.append("%s:%d" % (path, os.stat(path).st_mtime_ns))
        except OSError:
            parts.append("%s:MISSING" % path)
    return hashlib.sha256("|".join(parts).encode("utf-8")).hexdigest()


def _ensure_image_current():
    """Regenerates the REAL main/boot/boot_anim_image.h from design/boot/
    boot.png whenever the PNG is newer than the header - the one asset in
    this pipeline that lives outside the browser's own payload, so there
    is no JSON hash to compare it against the way render()'s own
    payload_hash covers the timeline. mtime is the cheap, honest signal:
    a PNG resave always bumps it, and comparing it costs one stat() call
    on every request, not just the ones that actually changed anything.

    Called before _source_signature() runs (see render()'s own call
    site) so a freshly-regenerated header's own new mtime is what that
    signature - and so the render cache - actually sees; calling it
    after would mean the FIRST render following a photo edit still
    served the stale cached binary.

    Raises RenderError the same way _regenerate_and_compile() does for a
    timeline validation failure - a self-check gen_boot_anim_image.py
    itself refuses to pass (wrong PNG dimensions, say) has to reach the
    browser as a clear message, not a silent skip that leaves the OLD
    photo showing with no explanation. No PNG at all is not that kind
    of failure - it means this checkout has never generated the header
    from a photo (or the file was moved), and the already-committed
    header is exactly what every other generated file in this tree does
    when its own source input is not being actively edited: nothing to
    regenerate from, so nothing happens.

    Written via a temp-file-plus-rename, not straight into the real path:
    unlike _regenerate_and_compile()'s own scratch copy (a few KB, disposable
    if torn), this is the REAL, committed 1.37 MB header - a Ctrl-C or
    disk-full mid-write here would leave a truncated file whose first
    symptom is a C syntax error mid-array, not a clear message. os.replace()
    is atomic on both POSIX and Windows, so the real path only ever sees a
    complete write or the old file, never a partial one. Logged to stderr
    too - this can fire just from opening the editor after resaving the PNG,
    with no explicit save action from the user, so the rewrite of a
    committed source file needs to be visible somewhere."""
    try:
        png_mtime = os.stat(BOOT_PNG).st_mtime_ns
    except OSError:
        return

    try:
        header_mtime = os.stat(BOOT_ANIM_IMAGE_HEADER).st_mtime_ns
    except OSError:
        header_mtime = -1

    if png_mtime <= header_mtime:
        return

    result = subprocess.run(
        [sys.executable, GEN_IMAGE, BOOT_PNG],
        capture_output=True, text=True)
    if result.returncode != 0:
        raise RenderError(400, result.stderr.strip() or
                          "gen_boot_anim_image.py failed with no message")

    fd, tmp_path = tempfile.mkstemp(
        suffix=".h.tmp", dir=os.path.dirname(BOOT_ANIM_IMAGE_HEADER))
    try:
        with os.fdopen(fd, "w", encoding="utf-8", newline="\n") as f:
            f.write(result.stdout)
        os.replace(tmp_path, BOOT_ANIM_IMAGE_HEADER)
    except BaseException:
        try:
            os.remove(tmp_path)
        except OSError:
            pass
        raise
    print("boot_anim_editor_server: regenerated %s from %s" %
          (BOOT_ANIM_IMAGE_HEADER, BOOT_PNG), file=sys.stderr)


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
    Git Bash's own bash.exe is the one thing this needs to get right.

    Git Bash's own known install locations are checked BEFORE shutil.which,
    not after - Windows ships its own "bash.exe" stub in System32 that
    launches WSL, not Git Bash, and if that directory happens to sit
    earlier on PATH than Git's own bin/usr\\bin (confirmed happening in
    practice, not just theoretically possible), shutil.which silently
    hands back the WSL one instead. A WSL bash has no concept of a
    C:/Users/... path at all - it mounts drives under /mnt/c/... instead -
    so every path this script ever passes it reports "No such file or
    directory" no matter how correct that path actually is, and nothing
    about that failure looks any different from a genuinely missing file
    until you think to check $PWD/$MSYSTEM inside it (see build_and_flash()
    below's own probe, added chasing exactly this)."""
    for candidate in (r"C:\Program Files\Git\bin\bash.exe",
                      r"C:\Program Files\Git\usr\bin\bash.exe"):
        if os.path.isfile(candidate):
            return candidate
    found = shutil.which("bash")
    if found and os.path.dirname(found).rstrip("\\/").lower() != \
            r"c:\windows\system32":
        return found
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
        a new one of those (wave_wavelength_m, say) never means touching
        this signature again. Returns (bmp_bytes, (origin_x, origin_y) or
        None)."""
        _ensure_image_current()

        payload_hash = hashlib.sha256(
            json.dumps(payload, sort_keys=True).encode("utf-8")).hexdigest() \
            + ":" + _source_signature(_watched_source_paths())

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
        # Same reason render() calls this: idf.py is about to compile
        # main/boot/boot_anim.c fresh, reading main/boot/boot_anim_image.h
        # straight off disk - a photo edit that has not been regenerated
        # yet would otherwise ship the OLD picture to the device with no
        # warning at all.
        _ensure_image_current()

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
        if not os.path.isfile(BUILD_FLASH_SCRIPT):
            raise RenderError(
                500, "build_flash_dev.sh not found at %s - the timeline "
                "files were still written to main/boot/ above." %
                BUILD_FLASH_SCRIPT)

        # Forward slashes, not BUILD_FLASH_SCRIPT's own native backslashes -
        # a "/bin/bash: <mangled path>: No such file or directory" turned
        # up once with every backslash in the path gone, root cause not
        # pinned down (reproducing find_bash()'s own resolution and the
        # script's own dirname/cd/pwd logic directly did not reproduce it),
        # but Git Bash accepts C:/... unambiguously and this removes the
        # entire class of risk regardless of the exact mechanism.
        script_for_bash = BUILD_FLASH_SCRIPT.replace(os.sep, "/")

        # A fast (milliseconds, not a build) sanity probe using the exact
        # same bash binary and exact same path the real invocation below
        # uses - "/bin/bash: <path>: No such file or directory" turned up
        # from the real invocation even after fixing the one concrete
        # backslash-mangling issue already found (see script_for_bash's
        # own comment above), for a file os.path.isfile() just confirmed
        # exists - a second, still-unexplained failure mode. Only
        # surfaced below if the real invocation ALSO fails with that same
        # message, so a genuine compile/flash failure's own log stays
        # undiluted the rest of the time.
        # Its own try/except, separate from the real invocation's below -
        # a probe timeout is itself diagnostic (bash launched but never
        # returned), not a stand-in for "the build timed out"; letting
        # TimeoutExpired propagate unguarded here used to surface as
        # exactly that wrong, confusing message ("build_flash_dev.sh did
        # not finish within 600s") for a run that had not even reached
        # the real invocation yet.
        try:
            probe = subprocess.run(
                [bash, "-c",
                 'if [ -f "$1" ]; then echo FOUND; else echo MISSING; fi; '
                 'echo "MSYSTEM=$MSYSTEM"; echo "PWD=$(pwd)"; echo "PATH=$PATH"',
                 "probe", script_for_bash],
                capture_output=True, text=True, timeout=10)
            probe_info = ("bash's own view of that path just before this "
                          "attempt:\n" + (probe.stdout + probe.stderr).strip())
        except subprocess.TimeoutExpired:
            probe_info = ("bash's own view of that path just before this "
                          "attempt: the probe itself did not return within "
                          "10s - bash launched but hung before even echoing "
                          "FOUND/MISSING, a stronger signal than a normal "
                          "probe failure.")

        # stdin=DEVNULL: build_flash_dev.sh ends with an interactive "press
        # Enter to close" (it doubles as a double-clickable script) that
        # would otherwise hang this request forever - see its own comment
        # on the `|| true` that makes stdin-at-EOF there a no-op, not a
        # reported failure.
        proc = subprocess.run(
            [bash, script_for_bash, port],
            cwd=LAUNCHER_DIR, stdin=subprocess.DEVNULL,
            capture_output=True, text=True, timeout=BUILD_FLASH_TIMEOUT_S)
        log = proc.stdout + proc.stderr
        if proc.returncode != 0:
            if "No such file or directory" in log and script_for_bash in log:
                log = log.strip() + "\n\n" + probe_info
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
        if self.path == "/timeline":
            self._do_get_timeline()
            return
        self.send_error(404)

    def _do_get_timeline(self):
        """The REAL, committed boot_anim_timeline.json - see this file's
        own top comment on why the editor page loads this on open rather
        than opening on its own hand-duplicated DEFAULT_STATE (a
        Build & Flash before ever pressing Load used to silently
        overwrite the committed timeline with that stale copy)."""
        try:
            with open(TIMELINE_JSON, "r", encoding="utf-8") as f:
                data = f.read()
        except OSError as exc:
            self._send_json_error(
                500, "could not read %s: %s" % (TIMELINE_JSON, exc))
            return
        body = data.encode("utf-8")
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

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
