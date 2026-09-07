/*=============================================================================
 * probe_main - host driver for every frame-budget scene that has a
 * SAND_HOST_PROBE wrapper in suite_sand.c (bd esp32c6-o2s: the canonical
 * host attribution probe, replacing the per-round copies this tree
 * accumulated - see this directory's own build_probe.sh for the history).
 *
 * Deliberately NOT suites_run_all(): that would also compile and run every
 * other DEVICE_BUILD scene in suite_sand.c (fire, gas, boiler, thermal
 * shock, the present-cost section...), most of which have nothing to do
 * with whatever pass a given round is attributing and would cost minutes
 * per build variant for no value. Instead this file keeps its own name ->
 * function table below, one
 * entry per SAND_HOST_PROBE wrapper (declared in suite_sand.c, right after
 * the real, unmodified test body each one calls), and runs only the scenes
 * asked for.
 *
 * Each scene still goes through suite_run_test_timed() - what RUN_TEST()
 * itself expands to for every suite in this tree, forced in by the build's
 * -include timing.h (see timing.h's own top comment) - so a TEST_ASSERT
 * failure (expected: host timing has no reason to land inside a budget
 * pegged from a device capture) unwinds cleanly through Unity's protected
 * call path instead of crashing on an uninitialised setjmp target, and this
 * driver's own output stays byte-comparable with a real suite run.
 * RUN_TEST() itself can't be used here because it stringifies its argument
 * at the call site, which does not work through a table of function
 * pointers - suite_run_test_timed() is the plain function it calls, minus
 * that macro sugar, so the scene's own name can be passed as a runtime
 * string instead.
 *
 * Usage:
 *   probe                       run every scene, in table order
 *   probe --list                print scene names, one per line, and exit
 *   probe SCENE [SCENE...]      run exactly the named scenes, in the order
 *                               given (repeats allowed) - what
 *                               launcher/main/apps/sand/tools/perf_probe/
 *                               run_probe.py drives, one child process per
 *                               (scene, round) pair, for interleaved best-of-N
 *                               timing.
 *   probe --counters SCENE...   reset sand_work_counters (bd esp32c6-8zx)
 *                               before each named scene and dump it after -
 *                               "scene NAME" then one "counter value" line
 *                               per field. Counting, not timing: exact and
 *                               deterministic, so tools/perf_probe/
 *                               compare_counters.py can bisect a regression
 *                               without a device. See sand_work_counters.h.
 *
 * Each scene reports its own per-step microsecond figure via the same
 * ESP_LOGI() line the device build prints (see suite_sand.c) - this driver
 * does not re-time anything itself. run_probe.py parses that line back out
 * of captured stdout.
 *===========================================================================*/
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "timing.h"
#include "unity.h"

#include "apps/sand/sand_work_counters.h"

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

/* One row per SAND_HOST_PROBE wrapper in suite_sand.c. Adding a scene there
 * means adding it here too - nothing discovers these automatically, the
 * same way suite_sand.c's own RUN_TEST list isn't automatic either. */
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

#if CONFIG_LAUNCHER_DEVELOPMENT
static void
print_counter_line(const char* name, uint32_t value) {
    printf("%s %u\n", name, (unsigned)value);
}
#endif

int
main(int argc, char** argv) {
    if (argc == 2 && strcmp(argv[1], "--list") == 0) {
        for (int i = 0; i < SCENE_COUNT; i++) {
            printf("%s\n", SCENES[i].name);
        }
        return 0;
    }

    bool counters_mode = false;
    int first_arg = 1;
    if (argc >= 2 && strcmp(argv[1], "--counters") == 0) {
        counters_mode = true;
        first_arg = 2;
    }
#if !CONFIG_LAUNCHER_DEVELOPMENT
    if (counters_mode) {
        fprintf(stderr, "probe --counters: built without "
                "CONFIG_LAUNCHER_DEVELOPMENT, counters do not exist\n");
        return 1;
    }
#endif
    if (counters_mode && first_arg == argc) {
        fprintf(stderr, "probe --counters: needs at least one scene name\n");
        return 1;
    }

    UNITY_BEGIN();

    if (!counters_mode && argc == 1) {
        /* No scenes named: run everything, table order - the old fixed
         * behaviour every per-round probe_main.c used to hardcode. */
        for (int i = 0; i < SCENE_COUNT; i++) {
            suite_run_test_timed(SCENES[i].fn, SCENES[i].name, 0);
        }
    } else {
        for (int a = first_arg; a < argc; a++) {
            const probe_scene_t* scene = find_scene(argv[a]);
            if (!scene) {
                fprintf(stderr, "probe: unknown scene '%s' (try --list)\n", argv[a]);
                return 1;
            }
#if CONFIG_LAUNCHER_DEVELOPMENT
            if (counters_mode) {
                sand_work_counters_reset();
                suite_run_test_timed(scene->fn, scene->name, 0);
                printf("scene %s\n", scene->name);
                sand_work_counters_dump(print_counter_line);
                continue;
            }
#endif
            suite_run_test_timed(scene->fn, scene->name, 0);
        }
    }

    return UNITY_END();
}
