/*=============================================================================
 * oracle_main - bare-metal driver, one scene per image (bd oracle spike).
 *
 * Modelled on launcher/main/apps/sand/tools/perf_probe/probe_main.c's own
 * SCENES table (kept in sync by hand, the same way that file's own table
 * is not discovered automatically - see its header). The difference is the
 * selection mechanism: probe_main picks a scene from argv, because a host
 * process has one. This image has no argv - QEMU's "-kernel" boot protocol
 * hands a bare-metal image a hart id and a device-tree pointer, not a
 * command line - so the scene is baked in at BUILD time via -DORACLE_SCENE
 * (build_oracle.sh's first argument becomes this macro). That also happens
 * to be the only correct shape for what comes after the build: a TCG plugin
 * (libinsn/libcache) totals its counters once, at QEMU process exit, so
 * measuring N scenes would require either N processes (this) or resetting
 * the plugin's counters between scenes from inside the image - state this
 * image has no way to reach into QEMU to request. One scene per invocation
 * is not a limitation being worked around; it is the simplest shape that is
 * still correct.
 *===========================================================================*/
#include <stdio.h>
#include <string.h>

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
extern void sand_host_probe_run_lava_stress(void);
extern void sand_host_probe_run_four_liquids(void);
extern void sand_host_probe_run_wet_earth(void);
extern void sand_host_probe_run_water_over_lava(void);
extern void sand_host_probe_run_every_material_flip(void);
extern void sand_host_probe_run_smoke_and_steam(void);

typedef struct {
    const char* name;
    UnityTestFunction fn;
} oracle_scene_t;

/* One row per SAND_HOST_PROBE wrapper in suite_sand.c - see probe_main.c's
 * own table for the same list; re-verify against that file whenever a scene
 * is added or renamed there, this one does not discover it. */
static const oracle_scene_t SCENES[] = {
    {"full_step_control", sand_host_probe_run_full_step_control},
    {"settled_flip_control", sand_host_probe_run_settled_flip_control},
    {"water", sand_host_probe_run_water},
    {"mixed_flip", sand_host_probe_run_mixed_flip},
    {"lava_stress", sand_host_probe_run_lava_stress},
    {"four_liquids", sand_host_probe_run_four_liquids},
    {"wet_earth", sand_host_probe_run_wet_earth},
    {"water_over_lava", sand_host_probe_run_water_over_lava},
    {"every_material_flip", sand_host_probe_run_every_material_flip},
    {"smoke_and_steam", sand_host_probe_run_smoke_and_steam},
};
#define SCENE_COUNT (int)(sizeof(SCENES) / sizeof(SCENES[0]))

#ifndef ORACLE_SCENE
#error                                                                                                                 \
    "ORACLE_SCENE must be set at build time, e.g. -DORACLE_SCENE=\"water\" - see this file's own header for why a bare-metal image can't take it from argv."
#endif

int
main(void) {
    const char* want = ORACLE_SCENE;

    UNITY_BEGIN();

    int found = 0;
    for (int i = 0; i < SCENE_COUNT; i++) {
        if (strcmp(SCENES[i].name, want) == 0) {
            suite_run_test_timed(SCENES[i].fn, SCENES[i].name, 0);
            found = 1;
            break;
        }
    }
    if (!found) {
        printf("oracle: unknown scene '%s'\n", want);
    }

    return UNITY_END();
}
