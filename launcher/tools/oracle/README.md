# The instruction-count oracle: feasibility spike, addressed to whoever picks this up next

Read this before touching anything else in this directory. It is a working
note from one time-boxed spike (bd oracle spike), not a tour.

**The prize, restated**: exact per-benchmark RISC-V *instruction counts* and
modelled i-cache misses for sand's frame-budget scenes — a deterministic
work-quantity number that separates "the code does more work" from "the code
landed badly against the 32 KB icache", obtainable with no hardware. QEMU's
TCG interpreter is **not cycle-accurate**. Whatever this pipeline eventually
prints, it must never be labelled microseconds, milliseconds, or "time" of
any kind — counts only. Every script and doc in this directory should repeat
that warning in its own header; if you add one that doesn't, that's a bug.

## Route A (real device image under QEMU) is dead

Espressif's QEMU fork, at IDF v5.5, has no ESP32-C6 machine model:

- `tools/tools.json`: `qemu-riscv32` → `supported_targets: ["esp32c3"]`
- `tools/idf_py_actions/qemu_ext.py`: `QEMU_TARGETS` contains
  `esp32, esp32c3, esp32s2, esp32s3` — no `esp32c6`.

There is no C6 machine model to boot the real diagnostics image against.
This is recorded as fact in `launcher/tools/device_profiles/esp32c6.sh`
(`DP_QEMU_ROUTE=generic-virt`) so it can't quietly drift back into "maybe
try QEMU" without someone re-deriving this.

## Route B (this one): cross-compile the portable sim, run it bare-metal on generic QEMU

The sand simulation is pure portable C with zero peripherals (see the repo
`CLAUDE.md`'s testing section: "pass time in", "pass the environment in" —
the whole reason it's host-testable at all). It doesn't need a C6 model.
It needs an ELF, an ISA (`rv32imac_zicsr_zifencei`/`ilp32` — the device's
own, from the device profile), and somewhere to put a stack.

**This spike proves the bring-up. It builds.** `build_oracle.sh <scene>`
cross-compiles the portable sand sources, `suite_sand.c`'s
`SAND_HOST_PROBE` scenes, Unity, and a hand-written bare-metal floor
(`crt0.S` + `virt.ld` + `console.c`) into a freestanding ELF that links
clean against this toolchain's newlib, with only the ISA/codegen flags the
device profile already carries — nothing hand-copied, nothing guessed.

### Evidence this is real device-shaped code, not a host build wearing a costume

```
$ ./build_oracle.sh water
built: .../out/oracle_water.elf
   text    data     bss     dec     hex filename
 411766     684   11024  423474   67632 oracle_water.elf
```

**.text = 411,766 bytes against a 32 KB icache** (`DP_ICACHE_BYTES` — see
that field's own caution in the device profile about the C6's icache size
being unverified against the TRM). That ratio — not the byte count itself —
is the whole reason this oracle is worth building: with `.text` twelve-plus
times the cache, which scene's working set actually fits, and where the
compiler placed each function relative to a 32 B line / 8-way set, decides
more of the device's real cost than the instruction count alone does.

Disassembly of a real sand function, showing genuine `rv32imac` code
(compressed 16-bit forms mixed with 32-bit ones, real `jal` calls):

```
80027aa0 <sand_step>:
80027aa0:  0e452783   lw     a5,228(a0)
80027aa4:  d3010113   addi   sp,sp,-720
...
80027ac2:  8baa       mv     s7,a0          # c.mv - compressed form
```

Codegen fidelity — the device profile's `DP_CODEGEN_FLAGS` includes
`-fno-jump-tables -fno-tree-switch-conversion` (from ESP-IDF's own top-level
`CMakeLists.txt`, target-independent), so a `switch` must compile to a
compare chain, never an indexed jump. `material.c`'s `material_colours()`
(the only portable `switch` currently in the sand sources — none survive
in `sand_reactions.c` on `main` right now) confirms it: every case is a
sequential `li` + `beq`/`bne`/`bltu`/`bgeu`, and there is no `jr`/`jalr`
anywhere in the function:

```
800311fe: 06f80563   beq   a6,a5,80031278 <material_colours+0x8a>
80031214: 12d88163   beq   a7,a3,80031336 <material_colours+0x148>
80031218: 0316e863   bltu  a3,a7,80031248 <material_colours+0x5a>
8003121c: 0ef88863   beq   a7,a5,8003130c <material_colours+0x11e>
80031222: 02f89b63   bne   a7,a5,80031258 <material_colours+0x6a>
```

Whenever the pair-matrix work (bd esp32c6-iu5, see "Validation case (b)"
below) lands a `switch` back into `sand_reactions.c`, re-run this same check
against it specifically — that's the one place a jump-table regression
would actually matter for this project's own history (attempt 19).

### What had to be worked out, and what's still a guess

- **`crt0.S`/`virt.ld`**: QEMU's riscv `virt` machine, given `-bios none
  -kernel <elf>`, jumps straight to the ELF entry in machine mode — no boot
  ROM, no OpenSBI. RAM base `0x80000000` is `virt`'s own fixed layout, not a
  C6 fact. This is standard bare-metal-newlib practice, not oracle-specific
  invention.
- **`console.c`**: the newlib syscall floor (`_write`/`_sbrk`/`_read`/
  `_close`/`_fstat`/`_isatty`/`_lseek`/`_exit`/`_kill`/`_getpid`) plus one
  detail that was NOT obvious going in and cost real link-time debugging:
  this toolchain's newlib is built **reentrant** (`sys/reent.h`: `_REENT`
  expands to `__getreent()`, not a plain global), so `errno` and stdio
  locking silently resolve to a linker-synthesised stub that always fails
  ("`__getreent is not implemented and will always fail`") unless something
  defines it. Fixed with one function returning `_impure_ptr` (single hart,
  no threads — the simplest possible reentrancy story).
- **The UART address (`0x10000000`, a 16550) and the boot protocol** are
  `virt`'s documented fixed layout, not guesses.
- **`esp_timer_oracle_stub.c` always returns 0.** perf_probe's own
  `esp_timer_host.c` (a *real* host implementation) needs `<windows.h>` or
  POSIX `<time.h>` — neither exists freestanding, so it could not be reused
  as-is, per this spike's own instructions. Returning 0 is not a shortcut
  taken lightly: every `esp_timer_get_time()` call site inside
  `suite_sand.c`'s `DEVICE_BUILD` block is a `FULL_STEP_BUDGET_US`-style
  per-step budget assertion (`TEST_ASSERT_LESS_THAN_MESSAGE(budget,
  per_step, ...)`) — a constant 0 makes every one of them trivially true
  instead of failing on a number that was never meaningful under an
  interpreter anyway. If a future scene ever asserts on elapsed time for a
  reason that ISN'T "device wall-clock budget", re-check this stub first.
- **Heap sizing**: `_sbrk` is a bump allocator bounded by `_heap_start`/
  `_heap_limit`, where `HEAP_SIZE` is injected via `--defsym` straight from
  `DP_FREE_HEAP_BYTES` (`device_profile_require`, never a literal). A
  fixture that overallocates fails here with `ENOMEM` the same way it fails
  on the real board, instead of quietly succeeding against `virt`'s much
  larger emulated RAM.
- **One scene per image, chosen with `-DORACLE_SCENE` at build time**,
  because a bare-metal image has no `argv` and a TCG plugin totals its
  counters once, at QEMU process exit — see `oracle_main.c`'s own header.
- **`libsys_qemu.a`/`sys.qemu.specs`** already ship inside this same
  toolchain (`riscv32-esp-elf/lib/*/sys.qemu.specs`, pulling
  `--whole-archive -lsys_qemu`). Not used here — the task's own crt0/
  console approach gives exact control over what the plugin sees — but
  worth knowing it exists: the archived object only defines `__getreent`
  for the multilib checked, so whatever else it was meant to provide isn't
  present in this toolchain's build of it. Don't assume it's a working
  shortcut without testing it separately.

## What is still missing: no QEMU at all

**No `qemu-system-riscv32` and no bare `qemu-riscv32` binary exists on this
machine** — checked `PATH`, `~/.espressif/tools/`, `C:/Program Files/qemu`,
choco, msys64. This spike proves the bring-up; it does not prove the run,
because there's nothing here to run it on.

`winget` offers `SoftwareFreedomConservancy.QEMU 11.1.0` — **do not install
it** (out of scope for this spike, and not obviously sufficient anyway):
official QEMU Windows builds have historically shipped **without** TCG
plugin support (`--enable-plugins` is a build-time flag, off by default on
many prebuilt Windows packages), and the `contrib/plugins/` tree
(`libinsn.so`/`libcache.so` — instruction counts and modelled i-cache
misses, per this spike's brief) is **not shipped prebuilt at all** — it's
built from the QEMU source tree via `make -C contrib/plugins` after QEMU
itself is configured with `--enable-plugins`. The practical route a human
would need to take is a Linux/WSL QEMU built from source with plugins
enabled, not a Windows installer. Confirming whether any given Windows QEMU
build has plugins enabled, and whether the plugin ABI even matches without a
from-source build, is exactly the kind of thing to check before spending
more time here — don't assume either way.

### The commands that would run this, once that QEMU exists

Instruction count:

```sh
qemu-system-riscv32 -M virt -bios none -kernel out/oracle_water.elf -nographic \
    -plugin ./contrib/plugins/libinsn.so -d plugin
```

i-cache misses, with the cache geometry read from the device profile
(`DP_ICACHE_BYTES=32768`, `DP_ICACHE_LINE_BYTES=32`, `DP_ICACHE_WAYS=8` —
see that field's own caution about the *size* specifically being unverified
against the C6 TRM):

```sh
qemu-system-riscv32 -M virt -bios none -kernel out/oracle_water.elf -nographic \
    -plugin ./contrib/plugins/libcache.so,icachesize=32768,iblksize=32,iassoc=8 -d plugin
```

**The exact argument spelling for `libcache.so` (`icachesize=`/`iblksize=`/
`iassoc=`, versus something like `cachesize=`/`blksize=`/`assoc=`, versus
separate i-only/d-only flags) is NOT verified against that plugin's actual
source or a `-plugin libcache.so,help=on` run — nobody here has a QEMU to
check it against.** Treat it as the best-guess shape, not a confirmed
interface; re-derive it from the plugin's own `qemu_plugin_install()` /
argument table the moment a real QEMU is available, before trusting a
number that came out of it.

One more thing worth knowing before running any of the above: `crt0.S`'s
halt is `for(;;) j 3b` — nothing here calls a shutdown device, because that
would be one more MMIO address to guess. The process has to be killed (a
`timeout` wrapper, or Ctrl-C) rather than exiting on its own; a TCG plugin's
totals are dumped from its uninstall/atexit hook regardless of *how* the
process ends, so this should not affect the counts, but it does mean "just
run it and wait for it to finish" won't work as written.

## The two validation cases this oracle must reproduce before anyone trusts it

Recorded here so the next person doesn't have to rediscover them by reading
through `docs/Sand/Performance-Tuning-Attempts.md` and old branches:

**(a)** Commit `8a20c86` ("sand reactions: reject non-reacting neighbours
before the RNG, not after") vs its parent `57ff991` must show **fewer**
instructions on the wet-earth and lava scenes. The device itself measured
-6.9% on wet earth for this change — if the oracle disagrees in direction
(not just magnitude) on this pair, something in the pipeline is wrong before
anything else it says can be trusted.

**(b)** Commit `fcf329b` vs its parent `58b1f42`, on branch
`sand-pair-matrix` (a `switch`-based reaction dispatcher — the shape
`DP_CODEGEN_FLAGS`'s `-fno-jump-tables`/`-fno-tree-switch-conversion`
comment already warns cost attempt 19 a regression) must show **more**
instructions and/or more i-cache misses on the reaction scenes. The device
measured +12% to +25% across fifteen rows for this change. If the oracle
shows a smaller effect or none, suspect either a host-vs-device codegen
mismatch (check the compare-chain evidence above still holds on that
branch) or a real difference between "more instructions" and "more misses"
worth separating out.

Neither of these can be checked without a working QEMU + plugins — they are
the acceptance test for Route B once one exists, not something this spike
attempted.

## Files in this directory

| | |
|---|---|
| `build_oracle.sh` | Cross-compiles one scene into a bare-metal ELF. `./build_oracle.sh <scene> [output-elf]`. Reads everything chip-specific from the device profile — never edit a flag into this script directly. |
| `crt0.S` | Reset path: stack pointer, zero `.bss`, call `main()`, halt. |
| `virt.ld` | Linker script for QEMU's generic riscv32 `virt` machine (RAM at `0x80000000`), heap sized from `DP_FREE_HEAP_BYTES` via `--defsym HEAP_SIZE=...`. |
| `console.c` | The newlib syscall floor plus a byte-banged 16550 UART `_write`. |
| `esp_timer_oracle_stub.c` | `esp_timer_get_time()` → always 0. See "counts, never microseconds" above for why that's correct, not lazy. |
| `oracle_main.c` | Picks exactly one `SAND_HOST_PROBE` scene, chosen at build time via `-DORACLE_SCENE`, and runs it through the same `suite_run_test_timed()` path every suite in this tree already uses. |

Nothing here is on the host test path (`run_tests.sh`) or the device
selftest path — this is a standalone cross-build, invoked by hand, until
someone reports the QEMU half working end to end.
