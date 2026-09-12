/*
 * counters_scene_main - the "current probe support file" compare_counters.py
 * (this directory) carries unmodified into every scratch tree it extracts
 * with `git archive` (bd esp32c6-8zx).
 *
 * probe_main.c cannot fill this role: it drives a scene through a
 * SAND_HOST_PROBE wrapper declared beside the real test body in
 * suite_sand*.c, and that convention (bd esp32c6-o2s) postdates several
 * commits this tool needs to reach - ef87042, the very regression bisected
 * to prove this counter set works, has no such wrapper. Reusing suite_sand.c
 * at all would also mean matching its Unity/timing.h scaffolding version for
 * version across the whole range being bisected - exactly the tooling drift
 * that burned a full day chasing this same regression by capture (see
 * docs/sand/Perf-Round-Guide.md's "check artifacts exist for the SPECIFIC
 * ref" note).
 *
 * So this file bypasses the suite entirely and drives sand_step() through
 * nothing but sand.h's own public, long-stable API - sand_init(),
 * sand_enable_sleeping(), sand_set(), sand_step(). The one scene wired up is
 * the water screen test_a_screen_of_water_fits_in_the_frame_budget
 * (suite_sand_perf.c) sets up: seed 11u, half a screen of water dropped in
 * as an uneven slab, 184x224, 20 steps. Verified byte-identical against that
 * function's own source at every commit this tool has actually been run
 * against - re-verify before trusting a scene beyond that range, the same
 * way any counter bisect must (Perf-Round-Guide.md again: an unmeasured
 * endpoint is exactly what cost a day here the first time).
 *
 * Deliberately not linked against unity/timing/suites.c - none of that
 * exists to serve this file's one job, which is counting, not asserting or
 * timing.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "material.h"
#include "sand.h"
#include "sand_work_counters.h"

/* Mirrors suite_sand_common.h's REAL_W/REAL_H - the shipped grid size, not
 * a tunable of this tool's own. Held as a literal here rather than pulled
 * from the extracted ref's own header: some refs this tool must reach
 * predate that header existing at all (suite_sand.c was still one file),
 * and the figure itself has been the same 184x224 across the whole range
 * this counter set has been checked against. */
#define REAL_W          184
#define REAL_H          224
#define REAL_BLOCK_COLS ((REAL_W + SAND_BLOCK_W - 1) / SAND_BLOCK_W)
#define REAL_BLOCK_ROWS ((REAL_H + SAND_BLOCK_H - 1) / SAND_BLOCK_H)

#if !CONFIG_LAUNCHER_DEVELOPMENT
#error "counters_scene_main needs CONFIG_LAUNCHER_DEVELOPMENT=1 (sand_work_counters is compiled out otherwise)"
#endif

static void
print_counter_line(const char* name, uint32_t value) {
    printf("%s %u\n", name, (unsigned)value);
}

/* Same setup test_a_screen_of_water_fits_in_the_frame_budget uses - see this
 * file's own header comment for why it is reproduced here rather than
 * called there. */
static int
run_water_scene(int steps) {
    uint8_t* big = malloc((size_t)REAL_W * (size_t)REAL_H);
    uint8_t* blocks = malloc((size_t)REAL_BLOCK_COLS * (size_t)REAL_BLOCK_ROWS);
    if (big == NULL || blocks == NULL) {
        fprintf(stderr, "counters_scene_main: grid allocation failed\n");
        free(big);
        free(blocks);
        return 1;
    }

    sand_t real;
    sand_init(&real, big, REAL_W, REAL_H, 11u);
    sand_enable_sleeping(&real, blocks);

    for (int y = 0; y < REAL_H / 2; y++) {
        for (int x = REAL_W / 4; x < (REAL_W * 3) / 4; x++) {
            sand_set(&real, x, y, CELL_MAKE(MAT_WATER, MASS_MAX));
        }
    }

    sand_work_counters_reset();
    for (int i = 0; i < steps; i++) {
        sand_step(&real, 0, 1000, 0);
    }
    printf("scene water\n");
    sand_work_counters_dump(print_counter_line);

    free(big);
    free(blocks);
    return 0;
}

int
main(int argc, char** argv) {
    const char* scene = (argc > 1) ? argv[1] : "water";
    const int steps = (argc > 2) ? atoi(argv[2]) : 20;

    if (strcmp(scene, "water") != 0) {
        fprintf(stderr,
                "counters_scene_main: unknown scene '%s' "
                "(only 'water' is wired up)\n",
                scene);
        return 1;
    }
    return run_water_scene(steps);
}
