# launcher

[![Host Tests](https://github.com/jose-villegas/esp32-c6-dev/actions/workflows/host-tests.yml/badge.svg)](https://github.com/jose-villegas/esp32-c6-dev/actions/workflows/host-tests.yml)
[![Build (Release)](https://github.com/jose-villegas/esp32-c6-dev/actions/workflows/build-release.yml/badge.svg)](https://github.com/jose-villegas/esp32-c6-dev/actions/workflows/build-release.yml)
[![Build (Diagnostics)](https://github.com/jose-villegas/esp32-c6-dev/actions/workflows/build-diagnostics.yml/badge.svg)](https://github.com/jose-villegas/esp32-c6-dev/actions/workflows/build-diagnostics.yml)
[![Shell Scripts](https://github.com/jose-villegas/esp32-c6-dev/actions/workflows/shell-scripts.yml/badge.svg)](https://github.com/jose-villegas/esp32-c6-dev/actions/workflows/shell-scripts.yml)

A custom app shell for the [Waveshare
ESP32-C6-Touch-AMOLED-1.8](https://www.waveshare.com/) board — no PSRAM, a
368×448 AMOLED panel, capacitive touch, and a 6-axis IMU. Everything here
drives the hardware directly rather than through a display framework: LVGL
ships as a transitive dependency of the board support package but is never
called, saving the ~67 KiB of RAM it costs before drawing anything.

## What's inside

A minimal shell (`main/`) that lists and switches between self-contained
apps, each living entirely in its own `main/apps/<name>/` folder — adding or
removing one touches no other file. Currently:

- **Falling Sand** — a cellular-automaton sandbox with sand, water and
  stone, steered by tilting the board and poured with a touch. The most
  substantial piece of engineering in this repo: a flash-resident material
  system, a hybrid mass-diffusion water model, gyroscope-driven momentum for
  a wall-rebound splash, and a device-verified performance budget for every
  hot path. See `docs/Sand/Sand-Simulation.md`.
- **3D Cube** — a Gouraud-shaded software rasterizer, no GPU.
- **Diagnostics** — a bench-only hardware self-test report; ships only in
  the diagnostics build, the same one that carries the test suites, never
  in release.

A power-on self-test (`launcher/main/boot/post.c`) runs in every build, release included,
and checks storage, memory, sensors and the display on every boot.

## Quick start

Requires [ESP-IDF](https://docs.espressif.com/projects/esp-idf/) v5.5+.

```bash
cd launcher && idf.py build            # release — no test code, ships to the board
idf.py -p <PORT> flash monitor

./launcher/test/run_tests.sh           # host tests, portable suites, <1 s
./launcher/test/run_device_tests.sh    # builds the diagnostics variant, flashes it,
                                        # runs every suite on the actual chip
```

`idf.py` cannot run under Git Bash, so on Windows use the wrappers below -
`.sh` scripts that shell out to PowerShell and write a markdown report into
their own `tools/results/`:

```bash
./launcher/tools/build_flash.sh        # build + flash the release firmware
./launcher/tools/report_test_results.sh # every suite, pass/fail
./launcher/main/apps/sand/tools/report_performance.sh  # frame-budget numbers
```

`./monitor.sh` (repo root) attaches to the console without paying ESP-IDF's
~90s environment-activation cost on every call.

`./launcher/tools/screenshot.sh` captures whatever the device currently has
on screen to an uncompressed `.bmp`, plus a same-named `.json` snapshot of
device state at that exact frame (sensors, memory, clock), over that same
serial connection - no SD card, no button on the device, just the running
firmware and a cable already plugged in. Needs neither `idf.py` nor
PowerShell. Development-only (`build_flash_dev.sh` / `build_flash.sh --diag`)
- a release build carries none of it, see
[`docs/Testing-Guide.md`](docs/Testing-Guide.md).

## Documentation

Each doc earns its length — these are working notes from actually building
this, not a tour. Start wherever your question is:

| | |
|---|---|
| [`docs/Launcher-Architecture.md`](docs/Launcher-Architecture.md) | How the shell and its apps fit together; the three rules that shape everything; how to add an app; why the UI toolkit is microui, not LVGL. |
| [`docs/Sand/Sand-Simulation.md`](docs/Sand/Sand-Simulation.md) | The falling-sand app in depth: materials, the water model, momentum, and the performance numbers behind every design choice. |
| [`docs/Notes/`](docs/Notes/README.md) | Board-specific hardware notes: the memory budget, panel and touch gotchas, flashing and recovery. Split by topic - start at the index. |
| [`docs/Testing-Guide.md`](docs/Testing-Guide.md) | How the host and on-device test suites work, and why release builds carry none of the test code. |
| [`docs/Model-Delegation-Workflow.md`](docs/Model-Delegation-Workflow.md) | Delegating feature implementation to local Ollama models or free-tier models through OmniRoute, with review and verification kept on the driving session. |

## Status

Actively developed, single-maintainer, not affiliated with Waveshare or
Espressif. Board-specific enough that most of this will not transfer
directly to other hardware, but the *reasoning* in the docs above — sweep
order in a cellular automaton, why a data cache assumption doesn't hold on
this chip, how to keep test code out of a release image — should.
