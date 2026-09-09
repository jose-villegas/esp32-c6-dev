# A Second Target: ESP32-S3 Alongside the C6

**DRAFT.** Nothing here was verified on a board — no S3 has been built for,
flashed or captured. Every row is marked **[spec]** (Espressif documentation
or ESP-IDF v5.5 on this machine), **[repo]** (measured here, on the C6), or
**[derived]** (arithmetic from the other two, to be tested).

*This file previously drafted an ESP32-C3 second target. That was the wrong
chip. Almost every conclusion in it inverts here, and the sections that
survived are marked where they appear.*

The question is not "port it". It is: what does a second target cost, what
breaks, and which sand findings are the chip's rather than the algorithm's.

---

## 1. The delta that matters

| | ESP32-S3 | ESP32-C6 (this board) | |
|---|---|---|---|
| ISA | **Xtensa LX7**, windowed ABI | RV32IMAC | [spec] |
| Cores | **2** @ 240 MHz | 1 HP @ 160 MHz (+ LP RV32 @ 20 MHz) | [spec] |
| FPU | single-precision | none | [spec] |
| SIMD | PIE 128-bit, inline asm only | none | [spec] |
| Instruction cache | 16 or **32 KB**, 4/8-way, 16/32 B line — Kconfig | 32 KB, 4-way, 32 B block, fixed | [spec] |
| **Data cache** | **16/32/64 KB, 4/8-way, 16/32/64 B line** | **none** | [spec] |
| Cache serves | flash *and* PSRAM; internal SRAM direct | flash only; SRAM direct | [spec] |
| Cache comes from | the SRAM pool — bigger cache, smaller heap | separate | [spec] |
| Internal SRAM | 512 KB; DRAM window 480 KiB | 512 KB; DRAM window 512 KiB | [spec] |
| PSRAM | quad or octal, up to 32 MB mapped | **none, ever** | [spec] |
| Display bus | same SH8601 QSPI panel on the sibling board | QSPI, 40 MHz stable | [repo] |

IDF cache defaults for the S3: `ESP32S3_INSTRUCTION_CACHE_16KB`, 8-way,
32 B line; `ESP32S3_DATA_CACHE_32KB`, 8-way, 32 B line.
[spec: `components/esp_system/port/soc/esp32s3/Kconfig.cache`]

Four of those rows carry the whole document.

**There is a data cache, and that is the load-bearing change.** This
campaign's most repeated winning shape — replace a per-cell flash-resident
`const` read with a flat SRAM word — exists because on the C6 that read is
served by *the same* 32 KB cache the hot loop's code lives in. Two
mechanisms in one trade: the read stops missing, and it stops evicting the
loop. On the S3 those two mechanisms separate. §2 works it through.

**The ISA changes.** Every disassembly-derived finding in this campaign
(`bd esp32c6-fs7`, `esp32c6-jin`, the round-6 register-pressure control) was
read off `riscv32-esp-elf-objdump` of rv32imac. Xtensa has shift-add
(`addx2/4/8`), PC-relative literal loads (`l32r`), zero-overhead loops and
windowed register calls. Those are four different codegen shapes for exactly
the constructs this campaign has been reading. **Not one disassembly finding
transfers; all of them must be re-read.** The *conclusions* they support may
still hold — see §2 — but the evidence does not.

**Two cores at 240 MHz.** 1.5× the clock, and a place to put
`gfx_present()`. `bd esp32c6-91i` (present pipelining) exists only because
the C6 has no second core; on the S3 the 17.6 ms present leaves the frame
budget outright.

**PSRAM changes the memory rule, but not the way it looks.** §3.

---

## 2. Which sand findings survive an S3

### General — algorithmic, survives any core

| Finding | Source | Why it transfers |
|---|---|---|
| Not doing the work at all beats doing it cheaper — cross-flow level-block skip, **50.8%** off a settled basin (10737 → 5285 us); packed-row gas skip 9.6% | `28b931a`, `bd esp32c6-0t7` | Removes *N cells × per-cell cost*. Both terms scale together on any core, so the **percentage** is roughly microarchitecture-invariant even though the microseconds are not |
| Which pass owns a scene inverts by scene — fire is gas 56% + reactions 43%, sweep+cross-flow 0.09%; water is the exact opposite | `bd esp32c6-dp8`, `esp32c6-2u7` | A property of the automaton, not the chip |
| Reactions cost what they *do* (bodies 55%, dispatch 4%); cross-flow costs what it *looks at* | `bd esp32c6-jin`, `esp32c6-61h` | Same |
| The row form of a skip is a benchmark artefact unless the scene has air beside the pool | `28b931a`, memory `test-basins-are-clean...` | Scene design, not hardware |
| No 64-bit divide, no signed `/ 2^n` in a hot loop | Playbook 7 & 11 | **Survives verbatim.** The S3's FPU is *single-precision*: `__divdi3` is still a call and `double` is still soft-float |
| Statics tax the heap because DIRAM is one pool | Board-and-Memory.md | Same on S3, and **worse** — cache size comes out of that pool too |

### Board-bound — do not carry the number, and here also not the reasoning

| Finding | Why it is the C6's |
|---|---|
| **"~29–36 cycles per bounds-checked neighbour probe"** (`bd esp32c6-fs7`) | Not the same question on S3. That figure is `lui/addi/lw/lw/mul/lw/add/lbu` on rv32imac with no data cache. On Xtensa the address materialises with `l32r`, the index folds into `addx2`, and the table sits in a 32 KB **data** cache. Re-derive from a real S3 objdump before quoting a cycle count at all |
| "A plain call costs ~27 cycles here" | Xtensa windowed calls price differently, and a register-window overflow spills eight registers to the stack. **Variance is worse, not better** |
| Host understates device by **2.0×–6.6×** (`bd esp32c6-vk4`) | Per-board. Expect it to **narrow** on the S3 — which is itself a measurement (§5) |
| The ±6.7% flash-layout spread (Perf-Round-Guide) | Mechanism exists on both (code is flash-resident behind a cache), geometry does not: 16 KB/8-way default vs 32 KB/4-way. Re-derive from two identical-source builds |
| Free heap 63,952 / grid 41,216 / `check_static_ram.py`'s literals | This chip, this build, this boot |
| Every `suite_sand_perf.c` budget (`measured × 0.9`) | Wall-clock, pegged from C6 captures at 160 MHz |
| QSPI 40 MHz, 17.6 ms present, 80 MHz corner corruption (`bd esp32c6-kfg`) | Panel and board wiring — the sibling S3 board carries the same panel, so this is the one number that might survive, and only by coincidence |

### The one that needs its own argument: the SRAM-mask trade

`gas_kind_mask`, `sweep_skip_mask`/`sweep_liquid_mask`, `gas_walk_offset[32]`
— all the same shape, and `sand.c:736` states the C6 mechanism plainly: *"there
is no data cache, so that dereference is a real flash read."*

```
  C6   [core] --> one 32 KB read-only cache --> flash
                    ^          ^
                    |          |
                 hot loop   materials[]      they EVICT EACH OTHER
                   code      (384 B)

  S3   [core] --> 16 KB icache --> flash   (hot loop code)
       [core] --> 32 KB dcache --> flash   (materials[], resident, no contention)
```

`materials[]` is 32 rows; the whole table is a few hundred bytes and cannot
miss a 32 KB data cache twice. So on the S3 the trade collapses to *dcache
hit vs direct SRAM load* plus *one shift vs one load-and-extract* — small,
possibly zero. **[derived] Expect these wins to shrink toward noise on an
S3, and expect a naive re-measurement to read as "the optimisation did
nothing".**

Three caveats that keep it honest, and they matter:

1. The **skip-work** wins (`bd esp32c6-vk4`'s open question) are about not
   *fetching code bytes* — the **i**cache, not the d-cache. The S3's icache
   defaults to **16 KB, half the C6's**. If the fetch hypothesis is right,
   those wins get **larger** at IDF defaults on the S3, not smaller.
2. The sand grid (41,216 B) is internal SRAM, direct-access on both chips,
   so it puts no pressure on the S3 dcache. **If the grid or the framebuffer
   ever moves to PSRAM, this whole analysis inverts** — then every per-cell
   access is a cache line and the mask trick matters more than it ever did
   on the C6.
3. Nothing above is measured. It is the mechanism argument; the S3 has
   hit/miss counters and they would settle it.

### Measurement method — the four rules

| Rule (Perf-Round-Guide, "Instrumenting a round") | Survives? |
|---|---|
| Only within-capture comparisons are trustworthy | **Rule yes, number no.** ±6.7% is C6; re-derive |
| Only size-neutral changes attribute cleanly | **Yes.** Same mechanism, any flash-resident-code target |
| Check `nm` for a new out-of-line symbol | **Yes**, and more important — Xtensa call cost is *less* predictable |
| An identical fingerprint proves nothing until the oracle reaches the new path | **Yes, verbatim.** No hardware content |

The single-step technique (rebuild, warm with every gate on, flip one gate,
time exactly one step, take the min) is about scene determinism. It survives
verbatim. So does step 0: start from a capture you took yourself.

---

## 3. What an S3 would actually run

**This is where the C3 draft was wrong in the useful direction.** A C3 could
not hold the framebuffer at all. An S3 can — but the margin is not generous,
and PSRAM is not the free answer it looks like.

### Without PSRAM

```
  framebuffer 329,728 B (368 x 448 x 2) = 322 KiB, non-negotiable [repo]

  C6  DRAM window 512 KiB   heap after .bss ~424 KiB   [fb 322][grid 41][free 62]
  S3  DRAM window 480 KiB   minus cache carve 48 KiB (IDF defaults 16i + 32d)
                            minus dual-core + WiFi statics (> the C6's)
                            -> [fb 322][grid 41][ ~tight, unproven ]
```

**[derived] It fits, and the margin is comparable to or tighter than the
C6's — settle it by linking, not by arguing.** Two S3-specific traps the C6
does not have: selecting 32 KB icache + 64 KB dcache costs **another 48 KiB
of heap**, and `check_static_ram.py` would need S3 addresses and its own
captured constants.

### With PSRAM — and the bandwidth question

Octal PSRAM at 80 MHz DDR is 160 MB/s theoretical, **~84 MB/s measured read
past the cache and ~50–60 MB/s for a copy** [spec, public S3 benchmarks].
Against the frame:

| | bytes | time | vs ~24 ms frame |
|---|---:|---:|---:|
| Write one full frame, internal SRAM @ 240 MHz | 329,728 | **~0.7 ms** [derived] | 3% |
| Write one full frame into PSRAM @ 80 MB/s | 329,728 | **~4.1 ms** [derived] | 17% |
| ...at 50 MB/s (copy-shaped access) | 329,728 | **~6.6 ms** [derived] | 27% |
| Present it over QSPI @ 20 MB/s | 329,728 | 16.5 ms theory, 17.6 measured [repo] | 73% |

Two conclusions, and they point opposite ways:

- **PSRAM as a present source is fine.** 80 MB/s is 4× the display bus, so
  DMA streaming a frame out of PSRAM is never PSRAM-limited. The roadmap's
  Phase 6 plan (double-buffer in PSRAM, present from core 1) is sound.
- **PSRAM as a draw target is a trap.** The framebuffer is 322 KiB against a
  32 KB dcache — 10× over. A rasterizer's scattered writes miss constantly,
  and each miss is a line read-modify-write. 6–10× slower than SRAM for the
  same fill, before any z-buffer.

So the honest S3 answer is: **keep the working buffer in SRAM, use PSRAM for
the presented copy.** Which is band mode again, arrived at from a different
direction — the roadmap's §3.3 stands on the S3 for a *different reason* than
it stands on the C6.

### What is unknown

- **Which board.** No S3 board is named anywhere in this repo's config or
  notes. The roadmap assumes the Waveshare ESP32-S3-Touch-AMOLED-1.8
  (ESP32-S3R8, 8 MB octal PSRAM, **same SH8601 panel**, FT3168 touch,
  QMI8658 IMU). If it is that board, this is a real port and most of the BSP
  work is a driver swap. If it is a bare devkit with no panel, §4 phase A/B
  is all that is available.
- **Whether it has PSRAM, and quad or octal.** Quad PSRAM is roughly half
  the bandwidth and would move the table above the wrong way.
- **The cache configuration that build would use.** It is a Kconfig choice,
  not a chip fact, and it changes both the heap and every cache conclusion
  in §2.

---

## 4. The name

**Unchanged from the previous draft — the roadmap already answers this.**
[`../Autana-Rendering-Roadmap.md`](../Autana-Rendering-Roadmap.md) §5 states
"per-target platform folders, one binary per board", with `platform/<board>/`
owning bus clocks, PSRAM policy and input wiring, and Phase 6
(`bd esp32c6-ems.7`) *is* the S3 port, already written, already scoped.

Recommendation: **rename the repo to `autana` when the `platform/` split in
§5 actually lands, not before** — the name should follow the shape, and
today the shape is one board's firmware. **Leave the `esp32c6-` beads prefix
alone regardless.** Issue ids are opaque keys; they are cited by the hundred
across `docs/` and `git log`, and re-keying to match a directory name breaks
every reference for no gain.

---

## 5. What it would take

Align with Phase 6 (`bd esp32c6-ems.7`); do not invent a parallel plan. Its
acceptance criteria already cover the platform split, the PSRAM double
buffer, present on core 1, and FPU/PIE paths behind the same interfaces.
What follows is the *smallest-useful-first* ordering underneath it.

**A — finish `device_profiles/esp32s3.sh`.** Hours, no device. The skeleton
exists and is honest: every capture-derived field is the literal
`unmeasured`, and the loader refuses to hand one to a gate. Fill the icache
fields from the *target build's own sdkconfig* (they are Kconfig, not chip
facts), add data-cache fields the format does not have yet, correct
`DP_ARCH_FLAGS` from a real S3 `compile_commands.json`. **Proves the profile
abstraction takes a second architecture. Proves nothing about the simulation.**

**B — build the sand suites + `grid_fingerprint` as an S3 selftest image.**
Unlike the C3 case, this is **not** expected to be a formality: it crosses
RISC-V → Xtensa, and it is the first thing in this project's history to do
so. It proves the automaton is byte-identical across two ISAs and two
compilers. Expect it to pass; if it does not, that is a real bug the C6 has
been hiding.

**C — the measurement that pays, and it is not the same one the C3 offered.**
The C3 would have been a cache-*halved control*. The S3 is a
**cache-*separated* control**, which is a sharper instrument for the same
question (`bd esp32c6-vk4`):

```
  run one known SRAM-mask win (sweep kind mask) and one known
  skip-work win (cross-flow block skip) on both boards

  mask win shrinks on S3, skip win holds or grows
        -> the mask wins were DATA-cache/contention effects
           and the skip wins are FETCH effects.  vk4 answered.

  both shrink together
        -> both were execution effects the C6's 160 MHz exaggerated

  mask win holds
        -> the mechanism is neither cache; go read counters
```

Cheaper and less invasive than vk4's route 3 (padding the skipped span,
which perturbs the code it measures). Caveats up front: two ISAs, two
compilers and two clocks move at once, so this is a **sign** test, never a
magnitude one, and each board needs the within-capture discipline of §2
applied separately. The S3 also exposes cache hit/miss counters — if those
are reachable from a diag build, they end the argument outright and C is
redundant.

**D — the real port.** Only with the AMOLED board. It is Phase 6 and it
should be sequenced there, after band mode (Phase 1), because §3's bandwidth
table says the S3's PSRAM wants band mode as much as the C6's RAM shortage
does.

---

## 6. On the premise

> *"some of these optimizations might be hardware bound but I believe we
> might be generally fine, instead the new hardware will just open new paths
> to optimize instead of making old ones invalid"*

**Half right, and the wrong half is the half that matters more on an S3 than
it would have on a C3. Stated without softening:**

- **The algorithmic findings are fine.** §2's first table transfers, and the
  biggest wins this campaign has (the skips) are in it. That part of the
  premise holds.
- **Every number is C6-and-this-panel** — cycles per probe, cycles per call,
  the host/device factor, the layout spread, every frame budget in the tree.
  A second board's first job is re-deriving constants, not re-litigating
  conclusions. Same as the C3 draft said.
- **But one whole family of shipped optimisations is at genuine risk, and it
  is the family this campaign repeats most.** The SRAM-mask trade
  (`gas_kind_mask`, the sweep masks, `gas_walk_offset`) is written in the
  source as *"there is no data cache"*. The S3 has one. Those changes do not
  become *wrong* — they stay correct, cheap, and never worse — but they
  plausibly stop being *wins*, and any S3 round that re-derives them from
  first principles would not invent them. That is a real invalidation, not a
  re-scaling.
- **And every disassembly reading is void**, because it is a different
  instruction set. The claims survive as hypotheses; the evidence does not.

The second half of the premise is right and undersold. The S3 opens three
paths the C6 structurally cannot: a **second core** for present (which
deletes `bd esp32c6-91i` as a workaround and hands ~17 ms back), **1.5× the
clock**, and **PSRAM** — with the caveat that PSRAM is a streaming resource,
not a scratchpad (§3).

One thing to hold onto: **the S3 makes the C6 more interesting, not less.**
The C6 is the constrained target that forces the fixed-point, no-allocation,
skip-the-work engine the roadmap wants. If development moves to the S3
first, that discipline goes away quietly and the C6 stops booting. The
roadmap already says this — *C6 first, fixed-point only, S3 paths behind the
same interfaces* — and it is the rule most at risk once a faster board is on
the desk.

---

## Related

- [`../Autana-Rendering-Roadmap.md`](../Autana-Rendering-Roadmap.md) — §2's
  hardware table and §5/Phase 6 already carry the S3 plan; `bd esp32c6-ems.7`
- [`Board-and-Memory.md`](Board-and-Memory.md) — the C6 memory budget §3
  compares against
- [`../sand/Perf-Round-Guide.md`](../sand/Perf-Round-Guide.md) — the four
  measurement rules classified in §2
- `launcher/tools/device_profiles/esp32s3.sh` — the skeleton §5 phase A fills
- `bd esp32c6-vk4` — the question §5 phase C is shaped to answer
