/*=============================================================================
 * probe_main - host driver for the reactions-pass pair-matrix restructure
 * (bd esp32c6-iu5). Ten scenes: the two liquid-free controls plus every
 * scene the task's own gate names - water, mixed flip, lava stress, four
 * liquids, wet earth, vent spam, every-material flip, smoke+steam.
 *
 * Same shape as perf_probe's own probe_main.c (worktree-agent-
 * a167459b17fb4db71, commit 9de21aa) - deliberately NOT suites_run_all(),
 * for the same reason: that would also compile and run every other
 * DEVICE_BUILD scene in suite_sand.c, most of which have nothing to do with
 * this round's ten scenes and would cost minutes per build variant for no
 * attribution value. RUN_TEST on the ten SAND_HOST_PROBE wrapper functions
 * (declared in suite_sand.c, right after the real, unmodified test bodies
 * they call) runs exactly the official scenes this round measures, through
 * Unity's normal protected-call path so a TEST_ASSERT failure (expected
 * here - host timing has no reason to land inside a budget pegged from a
 * device capture) unwinds cleanly instead of crashing on an uninitialised
 * setjmp target.
 *===========================================================================*/
#include <stdio.h>

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
extern void sand_host_probe_run_vent_spam(void);
extern void sand_host_probe_run_every_material_flip(void);
extern void sand_host_probe_run_smoke_and_steam(void);

int
main(void) {
    UNITY_BEGIN();
    RUN_TEST(sand_host_probe_run_full_step_control);
    RUN_TEST(sand_host_probe_run_settled_flip_control);
    RUN_TEST(sand_host_probe_run_water);
    RUN_TEST(sand_host_probe_run_mixed_flip);
    RUN_TEST(sand_host_probe_run_lava_stress);
    RUN_TEST(sand_host_probe_run_four_liquids);
    RUN_TEST(sand_host_probe_run_wet_earth);
    RUN_TEST(sand_host_probe_run_vent_spam);
    RUN_TEST(sand_host_probe_run_every_material_flip);
    RUN_TEST(sand_host_probe_run_smoke_and_steam);
    return UNITY_END();
}
