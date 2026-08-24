# Real-hardware tunable sweeps

PowerShell scripts that answer "was this constant ever actually measured,
or is it just the first value that happened to work" - by editing it,
building, flashing, and reading real numbers back off the device, for
every candidate value in turn. Each one restores the original value and
reflashes `build.release` when done, regardless of outcome.

## Prerequisites

- ESP-IDF environment (`export.ps1`) - path defaults to this project's
  usual install location; pass `-IdfExportPath` if yours differs.
- The device connected over serial - port defaults to `COM3`; pass
  `-ComPort` if yours differs.
- Git for Windows, for the host test suite's `bash test/run_tests.sh`
  gate. Scripts call `bash.exe` directly with `--login`, not through an
  interactive shell - without `--login`, the profile that puts
  `coreutils` (`dirname`, etc.) on `PATH` never runs, and
  `run_tests.sh` fails with a `command not found` that has nothing to do
  with the actual sweep.
- `pyserial` (`pip install pyserial`) for `capture_selftest.py`.

## What's here

- `capture_selftest.py` - resets the device via an RTS pulse (not
  `esptool`'s own reset - see its own docstring for why) and captures
  serial output to a file until `SELFTEST_COMPLETE` appears.
- `block_size_sweep.ps1` - `SAND_BLOCK_W`/`SAND_BLOCK_H`
  (`main/apps/sand/sand.h`).
- `row_leaf_sweep.ps1` - `ROW_MAX_RUNS`/`LEAF_REFINE_MAX_RUNS`
  (`main/apps/sand/row_runs.h`, `main/gfx_dirty.h`).
- `gather_pixels_sweep.ps1` - `GATHER_MAX_PIXELS` (`main/gfx_dirty.h`).

Each writes a `results/<name>_sweep_results.csv` plus one raw serial
capture per candidate value into `results/` (gitignored - these are
scratch output, not something to commit).

## Running one

```powershell
.\block_size_sweep.ps1
.\row_leaf_sweep.ps1 -ComPort COM7
.\gather_pixels_sweep.ps1 -IdfExportPath C:\esp-idf\export.ps1
```

Only run one at a time - they share `build.diag` and the serial port, so
two running concurrently will corrupt each other's builds or fight over
the port.

## Findings so far

Recorded where the relevant constant lives, not just here - see
`sand.h`'s own comment above `SAND_BLOCK_W`/`SAND_BLOCK_H` for the full
six-pair table, and:

- `docs/Notes/Simulation-Lessons.md`, "The sixth attempt" - the block-
  size sweep's full story, including two real bugs it surfaced (a
  device-only stack overflow, two test fixtures that broke under this
  same tuning) and the sweep-tooling bugs worth remembering if you're
  writing a fourth script on this pattern (the `bash --login`
  requirement above, a PowerShell `-notmatch`-against-an-array trap, and
  Windows PowerShell 5.1's `-Encoding utf8` writing a BOM - all three
  are already worked around in every script here, but worth knowing why
  if something in this pattern ever needs changing).
- `docs/Notes/Display-and-Rendering.md`, "The cap sweeps" - the
  `ROW_MAX_RUNS`/`LEAF_REFINE_MAX_RUNS`/`GATHER_MAX_PIXELS` results.

## Adding a new sweep

Copy whichever existing script is closest in shape (a single-file
constant like `gather_pixels_sweep.ps1`, or two files that move together
like `row_leaf_sweep.ps1`). The parts that generalise: the host-test
gate, the build/flash/capture/restore sequence, and the `finally` block.
The part that's genuinely different each time: which `ESP_LOGI` lines in
the self-test output are worth capturing, and what device test (if any)
needs writing first to make the constant's effect actually measurable -
see the `GATHER_MAX_PIXELS` sweep's own git history for what happens
when the existing device tests are too small to ever approach the
budget being swept: a sweep that runs cleanly and reports nothing real.
