# Flashing and Toolchain

Part of the platform notes for the Waveshare ESP32-C6-Touch-AMOLED-1.8 - see
[`README.md`](README.md) for the full set.

---

## Flashing and recovery

**The chip only accepts auto-reset while an app is actively running.** Once
firmware returns from `app_main` and goes idle, reset signalling stops working
entirely — `Hard resetting via RTS pin` does nothing, esptool reports
`No serial data received`, and manual DTR/RTS pulses produce zero bytes.

This is why the launcher's frame loop never exits, and why its error paths park
in a sleep loop rather than returning from `app_main`: the device has to stay
flashable even when startup fails.

If the board becomes unreachable:

1. Unplug USB-C
2. **Hold BOOT**
3. Plug USB-C back in while still holding
4. Keep holding ~2 s, release

That forces the ROM bootloader regardless of firmware state. Confirm you are in
download mode with:

```bash
esptool.py --chip esp32c6 -p COM3 --before no_reset flash_id
```

Connecting almost instantly (a few dots) means the chip is sitting in the
bootloader.

**After flashing this way, the board will not boot on its own** — `--after
hard_reset` uses the same non-functional RTS reset, so it stays in download
mode, silent, running nothing. **Unplug and replug normally** (no BOOT) to
start the app.

If it vanishes from USB entirely — no COM port, no `VID_303A` device — check
the cable first, then the PWR button: this board's power is managed by an
**AXP2101 PMIC**, so a long press cuts system power.

---

## Toolchain

- **ESP-IDF v5.5+ is required.** The Waveshare BSP declares `idf: ">=5.5"`;
  v5.4 will not resolve it. Both can coexist — they are keyed by `IDF_PATH`.
- BSP component: `waveshare/esp32_c6_touch_amoled_1_8` `^1.0.0`, which pulls 14
  dependencies including `lvgl` 9.5 and the display/touch drivers for *both*
  board variants.
- `sdkconfig.defaults` worth keeping: `CONFIG_ESPTOOLPY_FLASHSIZE_16MB=y` —
  without it the image header says 2 MB and the bootloader warns on every boot.

Console output reaches the USB CDC port because
`CONFIG_ESP_CONSOLE_SECONDARY_USB_SERIAL_JTAG=y`, even though the primary
console is UART0 on GPIO 16/17. The `GPIO 17 and 16 are used as console UART
I/O pins` line in every boot log refers to that primary, and is not evidence
that USB logging is off.

---

## The build was on -Og until it was measured

ESP-IDF defaults `CONFIG_COMPILER_OPTIMIZATION` to **Debug (-Og)**, and this
project sat there without anyone checking. It is the wrong default for a device
whose every frame is rasterising, cellular automata and pixel loops.

Switching to `CONFIG_COMPILER_OPTIMIZATION_PERF` (-O2):

| | -Og | -O2 |
|---|---|---|
| Falling-sand step, 184x224, 10304 grains | 9035 us | **6664 us** |
| `gfx_present()` | 18683 us | 17602 us |
| Display suspend/resume round trip | 730 us | 636 us |
| Shell framerate | 40.0 fps | **43.5 fps** |
| Image size | 0x5c140 | **0x5ae50** |

Faster *and* smaller, which is not the usual trade — -O2 inlines away enough
call overhead to more than pay for what it unrolls. `gfx_present()` barely
moves because it is waiting on DMA, not computing - see
[Display-and-Rendering.md](Display-and-Rendering.md).

Nothing was lost: there is no debugger attached to this board, and the on-device
suite passes identically at either level.

**This regressed silently, and was caught late.** The committed `sdkconfig`
drifted back to `CONFIG_COMPILER_OPTIMIZATION_DEBUG` at some point after the
numbers above were taken - `sdkconfig.defaults` kept saying `PERF`, but a
generated file already checked into git is not re-derived from its defaults
just because they changed, so nothing forced the two back into agreement.
Every plain `idf.py build` since then shipped at -Og again, undetected
because the diagnostics variant regenerates its own `sdkconfig` from the
defaults on every build and so never drifted - it kept measuring the -O2
numbers this doc assumed, while the actual release image quietly did not.
Found while device-verifying an unrelated refactor; fixed by deleting the
stale file and letting `idf.py reconfigure` rebuild it from the defaults.
Worth remembering on its own: a generated file checked into git can go
stale exactly like this, silently, with no diff pointing at it - the fix
is to occasionally check what is actually committed rather than trust that
it was once right.

---

## Release and diagnostics run at the same speed

Worth writing down because the first measurement said otherwise. The shell ran
at **41.7 fps in release and 40.0 fps in diagnostics**, which looks like the
test code costing ~4%. (Those figures are from the -Og era; the conclusion is
unchanged at -O2, only the numbers moved.)

It is not. Release registers one app and diagnostics registers two, so the two
builds were drawing a different number of buttons. Registering `app_cube` twice
in a release build reproduces 40.0 fps exactly — the entire difference is one
microui button.

The suites only ever run at boot; nothing test-related executes in the frame
loop. What the diagnostics build actually costs:

| | Release | Diagnostics |
|---|---|---|
| Shell framerate | 41.7 fps | 41.7 fps (like for like) |
| Boot to ready | 1029 ms | 1696 ms |
| Image size | 0x55f20 | 0x58d10 |

Two things this does expose:

- **One microui button costs about a millisecond.** The shell is not free, and
  the menu gets slower as apps are added.
- **The 1 ms tick quantises everything.** The frame loop ends in `vTaskDelay(1)`
  (`main.c`), so frame time is work rounded up to a whole tick. 24 vs 25 ms is
  one tick, which is why a small difference showed up as a clean 1.7 fps step.
  Any framerate comparison here is quantised to ~1.7 fps near 40 fps — compare
  microseconds of work, not the fps figure.

---

## Related

- [Display-and-Rendering.md](Display-and-Rendering.md) — the render-path
  numbers these build settings affect.
- [Simulation-Lessons.md](Simulation-Lessons.md) — the sand-step numbers these
  build settings affect.
