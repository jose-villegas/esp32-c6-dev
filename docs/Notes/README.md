# Platform Notes

Working notes for the Waveshare ESP32-C6-Touch-AMOLED-1.8. Everything here was
verified on the actual board or read out of the actual source — nothing is
copied from a spec sheet unless it is marked as such. Numbers come from boot
logs and `esp_timer` measurements taken in this repo.

Living document: correct it when the hardware disagrees with it.

Split into six files, grown out of what was originally one - a seventh,
the falling-sand app's own discovery narrative, moved out to
[`../Sand/`](../Sand/) once that folder existed to hold it properly:

- **[Board-and-Memory.md](Board-and-Memory.md)** — the board's hardware
  inventory, the memory budget with no PSRAM, and the SPI2/SD-card
  time-multiplexing story.
- **[Display-and-Rendering.md](Display-and-Rendering.md)** — owning the
  panel directly, the QSPI clock history (including why 80 MHz is not
  usable and why an intermediate clock does not exist), and the
  dirty-region tracking that partial screen updates are built on.
- **[Input-and-Sensors.md](Input-and-Sensors.md)** — touch, the IMU's axes
  and its accelerometer/gyroscope split, and the two buttons that are not
  the same kind of device.
- **[Flashing-and-Toolchain.md](Flashing-and-Toolchain.md)** — recovering
  an unresponsive board, ESP-IDF version requirements, and the build-flag
  history (`-Og` vs `-O2`, release vs diagnostics).
- **[Optimization-Playbook.md](Optimization-Playbook.md)** — general-purpose
  performance techniques this board's work turned up, written to travel to
  other chips and projects rather than staying specific to this one:
  measuring by deleting code instead of reasoning about it, verifying
  `static inline` actually inlined with `objdump`, register-spilling call
  boundaries, and more.
- **[Diagnostics-and-Debugging.md](Diagnostics-and-Debugging.md)** — which
  tool to reach for depending on the symptom: the host and on-device test
  suites, `screenshot.sh`'s image-plus-device-state capture, `monitor.sh`,
  the gfx debug overlays, and the USB-Serial-JTAG console quirk that breaks
  typing into idf_monitor if you don't know to look for it.

## Related

- [`../Launcher-Architecture.md`](../Launcher-Architecture.md) — how the
  shell and its apps are built on top of the hardware facts here.
- [`../Sand/`](../Sand/) — the falling-sand app: how it works today
  (`Sand-Simulation.md`), the discovery narrative behind it
  (`Simulation-Lessons.md`), and how to add a material
  (`Adding-a-Material.md`). Its performance numbers and memory choices
  are shaped directly by the constraints here.
- [`../Testing-Guide.md`](../Testing-Guide.md) — how any of this gets
  verified on real hardware.
