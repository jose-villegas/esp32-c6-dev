# Diagnostics and Debugging

Part of the platform notes for the Waveshare ESP32-C6-Touch-AMOLED-1.8 - see
[`README.md`](README.md) for the full set.

What to reach for depends on what is actually wrong. This is organised by
symptom, not by tool - skim the table, jump to the matching section.

---

## Which tool, for what

| Symptom | Reach for |
|---|---|
| Board is unresponsive / will not flash | [Board won't boot](#board-wont-boot-or-wont-flash) |
| Logic might be wrong in code you're writing | [Host test suite](#is-the-logic-right---host-test-suite) - sub-second loop |
| Passes on host, not sure it holds on the real chip | [On-device test suite](#does-it-still-hold-on-the-real-chip---on-device-suite) |
| Need to see exactly what's on screen right now | [Screenshot + device state](#what-does-the-screen-look-like-right-now---screenshotsh) |
| Need live logs, or a crash to resolve to file:line | [monitor.sh](#live-logs-and-crash-backtraces---monitorsh) |
| Typing into `monitor.sh` does nothing | [Console channel](#the-console-is-usb-serial-jtag-not-uart0) / [mintty](#typing-into-monitorsh-under-git-bash--msys2) |
| A render looks wrong - stale pixels, wrong region sent | [gfx debug overlays](#rendering-looks-wrong---gfx-debug-overlays) |
| Frame rate / performance seems off | [Performance](#performance-seems-off) |
| Orientation or the IMU seems wrong | [Orientation and IMU](#orientation-or-the-imu-seems-wrong) |
| Suspected memory pressure | [Memory](#suspected-memory-pressure) |

---

## Board won't boot, or won't flash

Not a diagnosis question so much as a recovery one - see
[`Flashing-and-Toolchain.md`](Flashing-and-Toolchain.md) for the BOOT-button
recovery sequence and why auto-reset stops working once firmware goes idle.

Every build, release included, also runs POST at boot (`main/boot/post.c`):
I2C peripheral probes, flash size, heap headroom, MAC validity, the on-die
temperature sensor, the SD card. Silent when everything passes; on a
**failure** it holds the report on screen for 8 seconds or until touched, so
a board with a genuinely faulty component says so even with nobody attached
to a serial console. The full report is always available on demand from the
Diagnostics app (`--dev`/`--diag` builds only - see the next sections) if you
want to see it without waiting for a failure.

## Is the logic right? - host test suite

```bash
./launcher/test/run_tests.sh
```

Under a second, runs on this machine (not the chip), and covers every
*portable* suite - anything with no hardware dependency. This is the loop for
red-green-refactor; reach for it first for anything that is a question about
logic rather than about the actual board. See
[`../Testing-Guide.md`](../Testing-Guide.md).

## Does it still hold on the real chip? - on-device suite

```bash
./launcher/test/run_device_tests.sh              # build, flash, collect results
./launcher/tools/report_test_results.sh           # same, plus a markdown report
```

Builds the diagnostics variant, flashes it, and runs *every* registered
suite - portable ones included - actually compiled by the RISC-V toolchain
and executed on the chip, which a host run cannot vouch for. Needs a
`CONFIG_LAUNCHER_SELFTEST` build (`build_flash_diag.sh` /
`build_flash.sh --diag`); see [`../Testing-Guide.md`](../Testing-Guide.md)
for what that flag carries versus `--dev`.

## What does the screen look like right now? - `screenshot.sh`

```bash
./launcher/tools/screenshot.sh
```

Captures whatever is currently on screen as an uncompressed `.bmp`, plus a
same-named `.json` snapshot of device state at that exact frame - uptime,
heap (current and low-water mark), CPU clock, on-die temperature,
orientation, the IMU, and that frame's touch/button state. Good for anything
where you need to see the actual pixels, or correlate a visual glitch
against memory/sensor conditions at that instant - see
`main/util/screenshot.h` and `main/util/device_state.h` for the mechanism
and the full field list.

- **Development-only** (`--dev` or `--diag` build) - a release build carries
  none of it.
- **Slow by design**: a full 368x448 frame is roughly 650 KB of base64 over
  115200 baud, taking the better part of a minute. The script prints
  progress every few seconds so this does not read as a hang.
- **Does not reset the board** - opens the port with DTR/RTS held low so a
  capture shows whatever app was already running, not a restarted boot
  animation.
- Only one process can hold the serial port at a time - close `monitor.sh`
  first.

To test the listener in isolation from the host script, attach `monitor.sh`
and type `SCREENSHOT` (then Enter) directly - the firmware logs `screenshot:
trigger received` (or `ignoring line: '...'` if something else arrived),
the cleanest way to tell a firmware-side problem from a host-script one.

## Live logs and crash backtraces - `monitor.sh`

```bash
./monitor.sh
```

Attaches to the console without paying ESP-IDF's ~90s environment-activation
cost. Picks up whichever `launcher/build*/launcher.elf` was most recently
built automatically - `idf.py build`, `build_flash.sh`,
`build_flash_dev.sh` and `build_flash_diag.sh` each write to a differently
named directory (`build/`, `build.release/`, `build.dev/`, `build.diag/`),
so there is no single fixed default to guess; pass `-e path/to/other.elf` to
pin a specific one. Passing the right `.elf` matters for more than
bookkeeping - it carries the debug symbols that turn a crash address into a
file and line number.

## The console is USB-Serial-JTAG, not UART0

**This board's single USB-C port is the ESP32-C6's own native USB-Serial/JTAG
peripheral.** Waveshare's own documentation says so directly: "USB Type-C
port - ESP32-C6 USB interface, for program flashing and log printing." UART0
exists on this board too, but only broken out on separate solder pads -
nothing a USB cable ever reaches.

ESP-IDF's own default for a chip with this peripheral assumes the OTHER
common board design instead: UART0 as the primary console (read AND
written), USB-Serial-JTAG as a write-only secondary mirror (see
`esp_system/Kconfig`'s own `ESP_CONSOLE_SECONDARY` help text, which
describes this exact mismatch and names the fix). Left at that default,
logging over the one cable this board actually has looks completely normal -
every line shows up as expected - while anything sent the OTHER direction (a
typed idf_monitor command, `screenshot.sh`'s trigger, anything) goes
nowhere: console reads only ever come from the primary channel, and
USB-Serial-JTAG was only ever the secondary.

**Fixed in `sdkconfig.defaults`**: `CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG=y`
makes USB-Serial-JTAG the primary channel, matching what this board is
actually wired to. If idf_monitor ever prints

    Writing to serial is timing out. Please make sure that your application
    supports an interactive console and that you have picked the correct
    console for serial communication.

this is the first thing to check - `grep CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG
sdkconfig` should show `=y`. A build directory generated before this was
fixed has the wrong choice baked into its own `sdkconfig`; delete the
directory and rebuild rather than expecting `sdkconfig.defaults` alone to
retroactively fix one that already exists.

## Typing into `monitor.sh` under Git Bash / MSYS2

idf_monitor.py's keypress capture on Windows uses `msvcrt`, which needs a
real Win32 console. Raw mintty (the terminal Git Bash / MSYS2 opens) is its
own pty emulation, not one - so keystrokes can silently fail to reach
idf_monitor at all unless the session is wrapped in `winpty`. Modern Git for
Windows usually does this automatically; if typing into a running
`monitor.sh` session produces no reaction whatsoever, try:

```bash
winpty sh monitor.sh -e launcher/build.dev/launcher.elf
```

idf_monitor also does not locally echo what you type, working or not - do
not expect characters to visibly appear as you type them either way; watch
the device's own log response instead.

## Rendering looks wrong - gfx debug overlays

`--dev`/`--diag` builds carry two runtime overlays, toggled from the
Diagnostics app's developer-toggle page (`gfx_set_debug_overlay()` /
`gfx_set_leaf_overlay()` in `main/gfx/gfx.h`):

- **Dirty-region overlay** - draws a border around whatever rectangle
  `gfx_present()` is about to send, so a stale patch of screen (something
  drawn but never marked dirty) or an over-wide send (marked dirty when it
  should not have been) is visible directly rather than inferred from
  symptoms.
- **Leaf-grid overlay** - draws the fixed leaf-grid boundaries a gathered
  send covers, in green, inside whatever the dirty-region overlay already
  shows - only has an effect with that overlay also on.

Both are off by default even in a development build, since they draw
directly over real content.

## Performance seems off

The shell logs frames-per-second on a fixed timer (`report_fps()` in
`main/main.c`) - unconditionally, in every build including release, not
gated behind `CONFIG_LAUNCHER_DEVELOPMENT` the way other instrumentation is
(worth knowing if you go looking for it and expect it gated the same way as
everything else on this page - see the note in
[`../Testing-Guide.md`](../Testing-Guide.md) on what should be gated and
why). `monitor.sh` shows it directly, no special build needed.

For anything deeper than an fps number: `app_sand.c` carries its own
`CONFIG_LAUNCHER_DEVELOPMENT`-gated rolling averages (step/draw timing,
awake-cell counts) logged periodically - see
`main/apps/sand/tools/report_performance.sh` for the host-side report
generator. The cube app has a dedicated on-device performance suite
(`main/apps/cube/suite_cube_perf.c`) for phase-by-phase timing (logic /
rasterise / HUD / present) against a 60fps budget, run the same way as any
other on-device suite (see [above](#does-it-still-hold-on-the-real-chip---on-device-suite))
- it has no separate host-side report script of its own yet.

## Orientation or the IMU seems wrong

Two ways to see raw sensor readings without adding any code:

- **Diagnostics app's "show orientation" toggle** (`--dev`/`--diag` build) -
  shows the raw accelerometer counts, the derived gx/gy display orientation
  is actually computed from, and the shell's current quarter-turn, all at
  once, so a physical hold can be pinned to an exact number.
- **A `screenshot.sh` capture's `.json`** - the `imu` object (raw
  accelerometer + gyroscope counts) and `orientation_quarter` field are a
  snapshot at one specific frame, useful when the question is "what was the
  board reading at the moment this visual bug happened" rather than a live
  reading.

## Suspected memory pressure

- **POST's boot-time check** - fails outright (not just a warning) below
  `MIN_FREE_HEAP` in `main/boot/post.c`, and reports free heap plus the
  largest free DMA-capable block on every boot, release included.
- **A `screenshot.sh` capture's `.json`** - `heap_free_bytes` (current) and
  `heap_min_free_bytes` (the low-water mark since boot - shows a transient
  allocation that already freed again, which `heap_free_bytes` alone
  cannot).

---

## Related

- [`../Testing-Guide.md`](../Testing-Guide.md) - what
  `CONFIG_LAUNCHER_DEVELOPMENT` and `CONFIG_LAUNCHER_SELFTEST` actually
  gate, and the three build variants (release/dev/diag).
- [`../Launcher-Architecture.md`](../Launcher-Architecture.md) - the
  Diagnostics app, and its planned split into a Settings app.
- [`../Settings-App-Plan.md`](../Settings-App-Plan.md) - that planned
  split, and the SELFTEST/"diagnostics" naming mismatch it would resolve.
- [`Flashing-and-Toolchain.md`](Flashing-and-Toolchain.md) - board recovery,
  and the toolchain details `monitor.sh` depends on.
