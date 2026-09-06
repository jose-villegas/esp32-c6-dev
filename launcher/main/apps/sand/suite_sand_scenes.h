/*=============================================================================
 * Shared benchmark scenes - see suite_sand_scenes.c.
 *
 * Each of these builds a scene once, both for its own correctness test in
 * suite_sand_scenes.c (does the scene actually keep reacting/boiling/
 * percolating/shattering the way it claims to?) and for the frame-budget
 * perf tests in suite_sand_perf.c (how expensive is a real device step
 * against this exact scene?) - one scene, two questions, so a regression in
 * either the mechanism or its performance shows up against the same
 * fixture rather than two that could quietly drift apart.
 *===========================================================================*/
#pragma once

#include "sand.h"

/* How much of the mixed all-pairs scene is left empty, so a gravity flip
 * has somewhere to launch into. */
#define EMPTY_SHARE_PERCENT 33

int all_pairs_material_at(int x, int y, int first, int n_mats);

void build_four_liquid_scene(sand_t *s);
void build_lava_stress_scene(sand_t *s);
void build_smoke_and_steam_scene(sand_t *s);
void build_thermal_shock_scene(sand_t *s);
void build_boiler_scene(sand_t *s);
void build_wet_earth_scene(sand_t *s);

/* Same real device impulse budget the vent-spam scene this replaced used -
 * the app's own buffer is sized APP_IMPULSE_MAX (2048), and this scene
 * should be fighting the same memory ceiling a real device pour actually
 * has, not a looser one a differently-sized test buffer would hide. */
#define WATER_LAVA_IMPULSE_MAX 2048
void build_water_over_lava_scene(sand_t *s);

/* Same reasoning as WATER_LAVA_IMPULSE_MAX above and DUNE_IMPULSE_MAX
 * (suite_sand_dune_blast.c) - the app's own fixed, device-heap-sized
 * APP_IMPULSE_MAX, not a formula in this scene's own blast radius. */
#define GUNPOWDER_BASIN_IMPULSE_MAX 2048

/* The coverage test's own measured window, reused by the frame-budget
 * test beside it - see either test's comment (suite_sand_scenes.c) for
 * why 90 steps, no settling. */
#define GUNPOWDER_BASIN_MEASURED_STEPS 90

void build_gunpowder_basin_scene(sand_t *s);
