# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this is

A custom firmware shell for the Waveshare ESP32-C6-Touch-AMOLED-1.8 board (no
PSRAM, 368×448 AMOLED, capacitive touch, 6-axis IMU). Everything drives the
hardware directly rather than through a display framework — LVGL is a
transitive dependency of the board support package but is never called (its
~67 KiB RAM cost is not affordable; confirmed zero `lv_*` symbols in the
linked binary).

The shell (`launcher/main/`) lists and switches between self-contained apps,
each living entirely in its own `launcher/main/apps/<name>/` folder:

- **sand** — a cellular-automaton falling-sand sandbox: materials, a
  mass-diffusion liquid model, gyroscope momentum, chemistry/reactions. The
  most substantial code in the repo. See `docs/Sand/Sand-Simulation.md`.
- **cube** — a Gouraud-shaded software rasterizer (small3dlib), no GPU.
- **diagnostics** — bench tool: hardware self-test (POST) report plus a
  developer-toggles page (gfx debug overlays, interlace, show-orientation);
  compiled into any development build (`--dev` or `--diag`), never release.
  The on-device self-test *runner* (the button, its result line,
  `selftest_run()`) is narrower still — `CONFIG_LAUNCHER_SELFTEST` only.

## Commands

```sh
cd launcher && idf.py build            # release firmware — no test code
idf.py -p <PORT> flash monitor

./launcher/test/run_tests.sh           # host tests, portable suites, <1 s
./launcher/test/run_device_tests.sh    # diagnostics build + flash + on-device run of every suite
```

**Windows: `idf.py` cannot run under Git Bash.** Use the PowerShell wrapper
scripts instead — `.sh` files that shell out to PowerShell and write a
markdown report into their own `tools/results/`:

```sh
./launcher/tools/build_flash.sh                          # build + flash release firmware
./launcher/tools/build_flash_select.sh                    # interactive: variant + any worktree/branch, creating one if needed
./launcher/tools/report_test_results.sh                  # every suite, pass/fail
./launcher/main/apps/sand/tools/report_performance.sh    # sand's frame-budget numbers
```

`./monitor.sh` (repo root) attaches to the console without paying ESP-IDF's
~90 s environment-activation cost.

`./launcher/tools/screenshot.sh` captures the device's current screen to a
`.bmp` plus a `.json` state snapshot, over the same serial connection — needs
neither `idf.py` nor PowerShell, but only works on development builds
(`build_flash_dev.sh` / `build_flash.sh --diag`), not release.

Formatting (only on files you just wrote or edited — this repo has no house
C style guide, match the surrounding file; do not reformat pre-existing
files, `.clang-format` disagrees with the current style in places):

```sh
scripts/check-format.sh <file.c> [<file.h> ...]         # format in place
scripts/check-format.sh --check <file.c> [<file.h> ...]  # verify only
```

Requires a **host** compiler (not the ESP32 toolchain) for the host tests:
Windows `winget install BrechtSanders.WinLibs.POSIX.UCRT`, Debian/Ubuntu
`apt install build-essential`, macOS `xcode-select --install`.

## Architecture

### Three rules that shape everything (`docs/Launcher-Architecture.md`)

1. **Exactly one framebuffer.** 368×448×2 = 322 KiB out of ~424 KiB RAM.
   `gfx.c` owns it; nothing else allocates pixels.
2. **Exactly one frame loop, owned by the shell.** An app's `frame()` draws
   and returns — no looping, blocking, presenting, or `vTaskDelay`. The shell
   presents; apps never do.
3. **Apps are callbacks, not processes.** One binary, one address space, no
   isolation — the accepted trade for instant app switching.

### Adding/removing an app touches no other file

Write `launcher/main/apps/<name>/app_<name>.c` implementing `enter()` /
`frame(dt_ms, input)` / `exit()` as an `app_t`, then self-register with
`APP_REGISTER(app_yours)` from inside that same file. No edits to `main.c` or
any `CMakeLists.txt` — the build globs `apps/**/*.c` and `APP_REGISTER`
emits an `.init_array` constructor ESP-IDF runs before `app_main()`.
Deleting the folder deletes the app, its logic, and its tests, cleanly.

**Naming convention the test runner relies on:** `app_*.c` is the
hardware-facing entry point (NOT host-portable); everything else in an app's
folder (`material.c`, `sand.c`, `tilt.c`, ...) is pure portable logic,
compiled into both the firmware and the host test runner. This split is what
makes a falling-sand automaton testable on a laptop.

`apps/*/tools/` is excluded from the glob (host-only sweep/report scripts) —
`WHOLE_ARCHIVE` force-links whatever the glob finds, so a stray `main()`
under `tools/` would otherwise get compiled into firmware.

`apps/diagnostics/` is excluded by folder when `CONFIG_LAUNCHER_DEVELOPMENT`
is off (bench-only; re-entering it re-runs POST and cycles the audio rail) —
release never sees it. Within the app itself, the self-test runner (button,
result line, `selftest_run()`) is guarded further, on
`CONFIG_LAUNCHER_SELFTEST`, since that symbol only exists in a SELFTEST
build.

### Includes are layer-qualified

`"gfx/gfx.h"`, not `"gfx.h"`, even between files in the same folder — so an
app reaching past `ui` into `gfx` is visible at the include line. Layers:
`boot/` (runs once, before the frame loop exists), `gfx/` (the one
framebuffer + primitives), `ui/` (microui integration), `input/` (touch,
gesture), `util/` (pure arithmetic), `apps/`.

### Terminology that is not interchangeable

- **shell** — the frame loop and app switching, `main.c` (log tag `shell`)
- **launcher** — the home screen the shell draws when no app is running,
  `ui_launcher.c`
- **boot** — what runs once before the loop exists and never again, `boot/`

### UI: microui, not LVGL

`launcher/main/ui/ui.c` wraps microui (vendored + patched in
`launcher/components/microui/` — upstream sizes `mu_Context` for desktop,
256 KiB just for the command list, cut down in the header itself since it
affects struct layout). Immediate-mode command list, hashed per-window each
frame to skip repainting/transferring unchanged canvases (dirty-band system).
Touch needs a synthesized hover frame (`feed_input()`) since a touchscreen
never produces microui's mouse-shaped "point, then click" sequence — costs
one frame (~24 ms) of tap latency, applies to every control. See
`docs/Launcher-Architecture.md` for the full mechanism and the styling system
(`ui_style.h`, `UI_BUTTON_FLAT` vs `UI_BUTTON_BEZEL`).

### Build variants (one Kconfig `choice`, exactly one true)

`CONFIG_LAUNCHER_RELEASE` / `CONFIG_LAUNCHER_DEVELOPMENT` /
`CONFIG_LAUNCHER_SELFTEST` in `main/Kconfig.projbuild`. `SELFTEST` implies
`DEVELOPMENT` but not vice versa. Guard anything whose only reader is a
developer (log lines, rolling averages, debug overlays, the Diagnostics app
itself) with `CONFIG_LAUNCHER_DEVELOPMENT`; guard the test suites — and the
self-test *runner* inside Diagnostics (button, result line,
`selftest_run()`) specifically — with `CONFIG_LAUNCHER_SELFTEST`. Release
strips all of it entirely (not `#ifdef` — simply never compiled), verified
by symbol counts in the two `.elf` files. `unity` must stay **unconditional**
in `REQUIRES` (not gated on
`CONFIG_LAUNCHER_SELFTEST`) — Kconfig-gated `REQUIRES` is evaluated before
`CONFIG_*` exists and silently no-ops, which only breaks on a clean build
directory. `build/` is release, `build.diag/` is diagnostics — separate
directories so testing never reconfigures the normal build.

### Testing (`docs/Testing-Guide.md`)

One set of suites (`test/suites/`, plus each app's own suites beside it, e.g.
`main/apps/sand/suite_sand.c`) compiles into **two** runners: the host runner
(`<1 s`, portable suites only, the TDD loop) and the on-device selftest
(every suite, including portable ones — proves the RISC-V build behaves
identically to x86, not just that the logic is right on a laptop).

- A suite registers itself: `SUITE_REGISTER(run_<name>_suite)` — no central
  list. Shell suites are listed in `CMakeLists.txt`/`run_tests.sh`; app
  suites are globbed.
- No suite owns `setUp`/`tearDown`/`UNITY_BEGIN` — several share one binary.
  Each suite has its own `fixture()` helper called at the top of every test.
- Guard hardware-only sections with `#ifdef DEVICE_BUILD`, including the
  `RUN_TEST` line.
- Two techniques make code testable: **pass time in** (`now_us` as a
  parameter, never call `esp_timer_get_time()` inside pure logic) and **pass
  the environment in** (e.g. screen height as a parameter instead of
  including `gfx.h`). Hardware access and interpretation are split into
  separate files; the hardware side calls into the pure side, never the
  reverse.
- When adding a test, watch it fail before making it pass — a test never
  seen red might assert nothing.

### Generated files

Four generated files live in the tree, each with its own generator in
`launcher/tools/` and its checked-in output in the tree it belongs to:
`boot_anim_curve.h` (`gen_zeta_curve.py`), `boot_anim_timeline.h`
(`gen_boot_anim_timeline.py`, from `boot_anim_timeline.json`),
`boot_anim_image.h` (`gen_boot_anim_image.py`, from `design/boot/boot.png`),
and `gfx/fonts/font_lmroman_40.h` (`gen_font.py`, from
`design/fonts/LatinModern/lmroman10-bold.otf` - there is no TrueType
rasterizer on the chip, so glyphs are rendered once on a host into an 8bpp
coverage atlas with a proportional advance table).
The convention they all follow: banner naming the exact regenerate command,
generator validates itself before emitting, and the shipped artifact is
tested independently of the generator (against the underlying math where
there is one, or - for an asset like the photo or a font, with no math to
check pixel content against - against structural facts a `_Static_assert`
can pin down, or by using the shipped metrics for real, plus visual
verification; never against the generator's own logic).

A font atlas is 274 KiB, so which fonts a build REFERENCES is a real flash
decision, not bookkeeping: call sites ask `gfx/gfx_font_roles.h` for a role
(`gfx_font_ui()`) rather than naming a typeface, and roles resolve at
compile time specifically so the linker drops an atlas nothing selected. See
`docs/Launcher-Architecture.md`'s "Text and fonts".

## Documentation map

Docs are working notes, not a tour — read the one your question is about,
don't read all of them per session:

| | |
|---|---|
| [`docs/Launcher-Architecture.md`](docs/Launcher-Architecture.md) | Shell/app contract, frame loop, adding an app, microui integration, why not LVGL |
| [`docs/Sand/Sand-Simulation.md`](docs/Sand/Sand-Simulation.md) | The sand app: materials, liquid model, momentum, performance budget |
| [`docs/Sand/Adding-a-Material.md`](docs/Sand/Adding-a-Material.md) | Checklist for adding a new sand material |
| [`docs/Sand/Reaction-Table.md`](docs/Sand/Reaction-Table.md) | Current material-interaction/reaction rules |
| [`docs/Sand/Tuning-At-a-Glance.md`](docs/Sand/Tuning-At-a-Glance.md) | Sand constants and their current values |
| [`docs/Notes/README.md`](docs/Notes/README.md) | Index into board-specific hardware notes (memory budget, panel/touch gotchas, flashing/recovery, optimization playbook) |
| [`docs/Testing-Guide.md`](docs/Testing-Guide.md) | Host/device test suites, why release builds carry no test code |
| [`docs/Settings-App-Plan.md`](docs/Settings-App-Plan.md) | Planned: split Diagnostics' dev-toggle page into its own Settings app |
| [`docs/Log-Level-Plan.md`](docs/Log-Level-Plan.md) | Planned: per-build-variant log-severity ceiling |
| [`docs/Autana-Rendering-Roadmap.md`](docs/Autana-Rendering-Roadmap.md) | Proposal: the rendering/engine roadmap (band-mode framebuffer, span rasterizer, raycaster, the three target games, S3 port) |

`scripts/` also has OmniRoute/Ollama-backed doc/code audit automation --
`audit-docs.sh` and `update-docs.sh` at the core, plus `fix-audited-code.sh`
/ `fix-audited-docs.sh` (the find/replace-patch fixers, each scopable to one
app or the whole project) and their single-click launchers
(`fix-audited-code-free.sh`, `-local.sh`, `-choose-app.sh`;
`fix-audited-docs-free.sh`, `-local.sh`, `-choose-app.sh`) — each pushes a
branch for review rather than touching `main` directly; read the header
comment of the one you need before running it, they're self-documenting.
`scripts/resolve-conflicts-local.sh` auto-resolves git merge conflicts the
same local-Ollama way, one hunk at a time with a reviewer second opinion,
but only ever commits if this repo's real test gate (`run_tests.sh` +
`check_app_sources.sh`) passes on the result. `scripts/write-test-local.sh`
delegates writing one Unity test *body* the same way, from a spec you write
(exact scene + exact assertions) -- the model only renders it into house
style, and `--regression-commit <SHA>` can prove the test actually fails on
the pre-fix code, automating this repo's own "watch it fail before it
passes" rule. See `docs/Model-Delegation-Workflow.md`'s "Related, narrower
tooling" section for both.

## Status

Actively developed, single-maintainer, not affiliated with Waveshare or
Espressif. Requires ESP-IDF v5.5+.


## Beads Issue Tracker

This project uses **bd (beads)** for cross-session backlog tracking —
banked ideas, deferred decisions, known bugs not being fixed right now.
Full policy (what it's for vs TodoWrite/the memory system, when to file an
issue, git/sync rules, known rough edges) lives in `.beads/PRIME.md` and is
injected automatically at the start of every session via the SessionStart
hook — read it there rather than duplicating it here, since that copy is
what actually reaches an agent every session; this pointer is for humans
browsing the docs. `bd ready` / `bd show <id>` / `bd graph --all --compact`
for a quick look without waiting for the hook.
