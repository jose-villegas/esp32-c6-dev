/*=============================================================================
 * coverage_scene_main - host driver for reaction_coverage.py.
 *
 * A twin of probe_main.c (same SCENES table, same suite_run_test_timed()
 * call path through the real, unmodified SAND_HOST_PROBE wrappers in
 * suite_sand_perf.c - see that file's own header for why RUN_TEST() itself
 * can't be used here), with one addition: it runs against a scratch tree
 * whose sand_reactions.c/sand_plants.c have been instrumented with
 * slot_note_r() calls (reaction_coverage.py's own apply_edits.py step), and
 * it prints which (material, reaction field) slots actually fired, between
 * sentinel lines the driver parses.
 *
 * Kept separate from probe_main.c rather than sharing one binary, the same
 * way compare_counters.py's counters_scene_main.c is its own file rather
 * than a probe_main.c option: this file's whole reason to exist is a header
 * (reaction_slots.h) and a call shape (slot_rows_init()/slot_dump()) that a
 * plain perf capture must never carry, and reaction_coverage.py's own
 * apply_edits.py step means this binary is ALWAYS built from an
 * instrumented scratch tree, never the one build_probe.sh produces.
 *
 * The SCENES table below is a deliberate duplicate of probe_main.c's own -
 * see that file's comment on why nothing discovers these automatically.
 * Adding a scene to one and not the other means reaction_coverage.py silently
 * stops seeing it; if a scene's coverage looks stale, diff the two tables
 * first.
 *
 * Usage:
 *   coverage_probe --list           print scene names, one per line, exit
 *   coverage_probe SCENE [SCENE...] run exactly the named scenes, in order,
 *                                   then print the fired-slot report once
 *                                   for the union of everything just run
 *===========================================================================*/
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "reaction_slots.h"
#include "timing.h"
#include "unity.h"

void
setUp(void) {}

void
tearDown(void) {}

extern void sand_host_probe_run_full_step_control(void);
extern void sand_host_probe_run_settled_flip_control(void);
extern void sand_host_probe_run_water(void);
extern void sand_host_probe_run_mixed_flip(void);
extern void sand_host_probe_run_settled_pool_to_landscape(void);
extern void sand_host_probe_run_lava_stress(void);
extern void sand_host_probe_run_four_liquids(void);
extern void sand_host_probe_run_wet_earth(void);
extern void sand_host_probe_run_water_over_lava(void);
extern void sand_host_probe_run_every_material_flip(void);
extern void sand_host_probe_run_smoke_and_steam(void);
extern void sand_host_probe_run_gunpowder_basin(void);

typedef struct {
    const char* name;
    UnityTestFunction fn;
} probe_scene_t;

static const probe_scene_t SCENES[] = {
    {"full_step_control", sand_host_probe_run_full_step_control},
    {"settled_flip_control", sand_host_probe_run_settled_flip_control},
    {"water", sand_host_probe_run_water},
    {"mixed_flip", sand_host_probe_run_mixed_flip},
    {"settled_pool_to_landscape", sand_host_probe_run_settled_pool_to_landscape},
    {"lava_stress", sand_host_probe_run_lava_stress},
    {"four_liquids", sand_host_probe_run_four_liquids},
    {"wet_earth", sand_host_probe_run_wet_earth},
    {"water_over_lava", sand_host_probe_run_water_over_lava},
    {"every_material_flip", sand_host_probe_run_every_material_flip},
    {"smoke_and_steam", sand_host_probe_run_smoke_and_steam},
    {"gunpowder_basin", sand_host_probe_run_gunpowder_basin},
};
#define SCENE_COUNT (int)(sizeof(SCENES) / sizeof(SCENES[0]))

static const probe_scene_t*
find_scene(const char* name) {
    for (int i = 0; i < SCENE_COUNT; i++) {
        if (strcmp(SCENES[i].name, name) == 0) {
            return &SCENES[i];
        }
    }
    return NULL;
}

int
main(int argc, char** argv) {
    if (argc == 2 && strcmp(argv[1], "--list") == 0) {
        for (int i = 0; i < SCENE_COUNT; i++) {
            printf("%s\n", SCENES[i].name);
        }
        return 0;
    }
    if (argc < 2) {
        fprintf(stderr, "usage: coverage_probe SCENE [SCENE...] | --list\n");
        return 1;
    }

    slot_rows_init();

    UNITY_BEGIN();

    for (int a = 1; a < argc; a++) {
        const probe_scene_t* scene = find_scene(argv[a]);
        if (!scene) {
            fprintf(stderr, "coverage_probe: unknown scene '%s' (try --list)\n",
                    argv[a]);
            return 1;
        }
        suite_run_test_timed(scene->fn, scene->name, 0);
    }

    const int failures = UNITY_END();

    /* Sentinel lines, not just the bare TOTAL line slot_dump() already
     * prints: probe_main.c's own Unity/timing chatter (PASS/FAIL lines,
     * TEST_TIME) shares this same stdout, so reaction_coverage.py needs an
     * unambiguous span to parse rather than a regex over the whole mix. */
    printf("===SLOTS===\n");
    slot_dump(stdout);
    printf("===END SLOTS===\n");

    /* A budget assertion failing is expected and uninteresting here (see
     * this file's own header) - the simulation already ran by the time any
     * TEST_ASSERT_LESS_THAN fires, so the slots it fired are already
     * counted. Never fail the process on that account. */
    (void)failures;
    return 0;
}
