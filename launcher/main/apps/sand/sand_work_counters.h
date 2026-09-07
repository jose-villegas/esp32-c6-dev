/*=============================================================================
 * sand_work_counters - exact, deterministic per-pass work counts for the
 * liquid cross-flow path (bd esp32c6-8zx). Counting, not timing: a full day
 * was lost attributing a device regression because host wall-clock timing
 * carries a 7-15% cross-binary noise floor that swamped the effect being
 * chased, while these counters proved bit-for-bit reproducible between two
 * builds of the same source and needed no device at all - see
 * docs/Sand/Perf-Round-Guide.md's "Count, do not time" section.
 *
 * One field per counter named in bd esp32c6-8zx's attribution comments - the
 * set that actually separated a real regression from the noise, there. Every
 * field is a TOTAL across however many sand_step() calls the caller made,
 * not a per-step average: reset once before a scene and dump once after: the
 * caller divides by its own step count for a rate.
 *
 * OPT-IN, AND NOT MERELY DEVELOPMENT-ONLY. Guarding these on
 * CONFIG_LAUNCHER_DEVELOPMENT alone would compile them into build.diag -
 * which is the build every frame-budget capture is taken on. Measured with
 * codegen_diff.py: the counters cost sand_step_liquids +39 instructions and
 * +120 bytes when compiled in. This campaign chases 2-3% effects, so an
 * instrument that silently shifts every future capture is worse than no
 * instrument. SAND_WORK_COUNTERS must therefore be asked for explicitly
 * (-DSAND_WORK_COUNTERS=1), and a plain diag build is byte-identical to one
 * without this header.
 *
 * CONFIG_LAUNCHER_DEVELOPMENT is still required on top, so a release build
 * cannot enable them even by accident. codegen_diff.py proves the off case:
 * release and diag both come out at 1,184 instructions, 0 differing lines.
 *===========================================================================*/
#pragma once

#include <stdint.h>

#ifndef SAND_WORK_COUNTERS
#define SAND_WORK_COUNTERS 0
#endif

#if SAND_WORK_COUNTERS && CONFIG_LAUNCHER_DEVELOPMENT

typedef struct {
    uint32_t xflow_calls;             /* sand_step_liquids() actually ran the pass */
    uint32_t rows_walked;             /* equalise_liquids()'s row loop */
    uint32_t blocks_considered;       /* block-columns equalise_one_row() iterates */
    uint32_t blocks_examined;         /* ...of those, not skipped by BLOCK_LIQUID_NEAR */
    uint32_t cells_examined;          /* equalise_one_block()'s cell loop */
    uint32_t cells_passing_mask;      /* ...of those, non-empty and liquid_mask()-set */
    uint32_t equalise_one_cell_calls; /* kept distinct from cells_passing_mask so a
                                        * future second call site would show up as a
                                        * divergence between the two rather than hiding */
    uint32_t find_shallowest_calls;
    uint32_t find_shallowest_iters;   /* find_shallowest()'s own k-loop trips, summed */
    uint32_t transfers;               /* equalise_one_cell() actually moved mass */
    uint32_t move_liquid_grain_calls; /* the main sweep's liquid grains - sand.c calls
                                        * into move_liquid_grain(), sand_liquid.c */
} sand_work_counters_t;

extern sand_work_counters_t sand_work_counters;

void sand_work_counters_reset(void);

/* One call per field, in struct-declaration order, through the caller's own
 * sink - kept free of printf/ESP_LOG so a host driver (stdio) and a future
 * device probe (ESP_LOGI) can both read it without a second copy of the
 * field list living in each. */
void sand_work_counters_dump(void (*emit)(const char *name, uint32_t value));

#define SAND_WORK_COUNT(field) (sand_work_counters.field++)

#else

#define SAND_WORK_COUNT(field) ((void)0)

#endif /* CONFIG_LAUNCHER_DEVELOPMENT */
