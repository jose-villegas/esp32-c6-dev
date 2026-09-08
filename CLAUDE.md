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
  most substantial code in the repo. See `docs/sand/Sand-Simulation.md`.
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

**Comment the WHY, not the WHAT — and only when the code doesn't already say
it.** Clean, well-named code mostly speaks for itself; a comment exists for
context, a decision, or a non-obvious constraint, not to restate what the
next line does. Keep comments accurate — an outdated one is worse than none,
so update it in the same edit that changes the code it describes. Length
follows from this, not the other way around: **300 characters is the aim,
500 the hard ceiling** for a comment that still needs the room after cutting
everything the code already says and everything that's really change history
(git log owns that — dates, old values, "raised from X to Y", a bug's own
incident report all belong there, not in the source). A short remainder, or
none at all, is the normal, correct outcome for most fields and functions —
not a sign the cut fell short. A run of consecutive own-line `//` lines, or
of consecutive own-line `/* */` blocks with no code between them, counts as
one comment for scoring; length is the prose, markers and `*` gutters
stripped, so re-wrapping never changes the score and chopping one
explanation into several adjacent blocks doesn't dodge it either. File and
section header banners (`/*====`) are exempt — asked to fit, a model deletes
the rule rather than the prose. The rule is aimed at comments beside code.

```sh
scripts/check-comment-length.sh                 # whole repo, 20 worst listed
scripts/check-comment-length.sh --changed main  # only comments a change touches
scripts/check-comment-length.sh --files         # per-file counts
```

Enforcement is a PostToolUse hook (`scripts/hooks/comment_length_hook.py`)
that blocks an Edit or Write whose *own new text* carries a comment past the
500-char ceiling — the backlog in the tree is somebody's cleanup, not the
current edit's problem, and the hook stays silent on anything at or under it
even if above the 300 aim. The hook script is tracked; the settings entry
pointing at it is not (`.claude/*` is gitignored), so a fresh clone has the
rule without the enforcement until a settings file names the script. Put
that entry in `~/.claude/settings.json` rather than per-worktree — every
worktree gets its own `.claude/`, and one user-level entry covers all of
them. Guard it so a project without the script is a no-op:

```sh
f="$CLAUDE_PROJECT_DIR/scripts/hooks/comment_length_hook.py"
if [ -f "$f" ] && command -v python >/dev/null 2>&1; then python "$f"; else exit 0; fi
```

The guard is not cosmetic: `python <missing file>` exits 2, and 2 is the code
that blocks the edit, so an unguarded user-level entry would refuse every
write in every other project. The tree holds 1,394 comments over the 300
aim (748 over the 500 ceiling), 67% of them in the sand app; `--comments-only
<ref>` proves a bulk trim moved no code.

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
`CONFIG_LAUNCHER_SELFTEST` in `main/Kconfig.projbuild`. `SELFTEST` *depends
on* `DEVELOPMENT` - a config that asks for SELFTEST without it gets neither,
so every defaults file sets both. Guard anything whose only reader is a
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
directories so testing never reconfigures the normal build. Every
`idf.py build` also runs `tools/check_static_ram.py` and fails if the
framebuffer plus one grid would no longer fit in the device's contiguous
heap.

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
| [`docs/sand/README.md`](docs/sand/README.md) | Index into the sand app's own doc set |
| [`docs/sand/Sand-Simulation.md`](docs/sand/Sand-Simulation.md) | The sand app: materials, liquid model, momentum, performance budget |
| [`docs/sand/Architecture.md`](docs/sand/Architecture.md) | Single-page map of `main/apps/sand/`'s shape - the grid byte, material table, file split |
| [`docs/sand/Impulse-Mechanics.md`](docs/sand/Impulse-Mechanics.md) | Explosions, thrown chunks, liquid splash - one mechanism, three call sites |
| [`docs/sand/Adding-a-Material.md`](docs/sand/Adding-a-Material.md) | Checklist for adding a new sand material |
| [`docs/sand/Reaction-Table.md`](docs/sand/Reaction-Table.md) | Current material-interaction/reaction rules |
| [`docs/sand/Tuning-At-a-Glance.md`](docs/sand/Tuning-At-a-Glance.md) | Sand constants and their current values |
| [`docs/sand/Perf-Round-Guide.md`](docs/sand/Perf-Round-Guide.md) | Entry point for a fresh session told to run a sand performance round |
| [`docs/notes/README.md`](docs/notes/README.md) | Index into board-specific hardware notes (memory budget, panel/touch gotchas, flashing/recovery, optimization playbook) |
| [`docs/Testing-Guide.md`](docs/Testing-Guide.md) | Host/device test suites, why release builds carry no test code |
| [`docs/Autana-Rendering-Roadmap.md`](docs/Autana-Rendering-Roadmap.md) | Proposal: the rendering/engine roadmap (band-mode framebuffer, span rasterizer, raycaster, the three target games, S3 port) |
| [`docs/plans/`](docs/plans) | Not-yet-built plans: `Settings-App-Plan.md`, `Log-Level-Plan.md`, `Metal-Smelting-Plan.md`, `Reaction-Doc-Generator-Plan.md` |
| [`docs/workflows/Model-Delegation-Workflow.md`](docs/workflows/Model-Delegation-Workflow.md) | Delegating a feature's implementation to a local/free-tier model, review kept on the driving session |

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
passes" rule. See `docs/workflows/Model-Delegation-Workflow.md`'s "Related, narrower
tooling" section for both. `scripts/trim-comments-local.sh` shortens
over-long comments the same local-Ollama way, but hands the model one
comment's PROSE and never a line of code — the rewrite goes back into that
comment's own span, so a bad generation can only produce a bad sentence, and
a file that ends up differing in anything but comments is discarded. A
comment it cannot get under the limit keeps its original text. `--via
hybrid` tries a free OmniRoute model first, in parallel across the whole
file, for reasoning this machine cannot run locally at no local-GPU cost —
but never blindly: an automated check (dropped facts, a fabricated number,
wholesale unrelated content — OmniRoute's free routing produces all three)
gates every answer, and anything that fails falls back to the local model.
`--review` then checks each rewrite for dropped numbers, dropped named
functions and dropped negations — no model needed for any of that — before
asking a reviewer model for a verdict on meaning; `--review-packet` writes the
prose-only pairs out for a reviewer the script cannot call itself. Expect
~45 s per comment, and read the report: a local model does occasionally drop
a WHY.

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
