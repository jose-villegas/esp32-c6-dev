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

/* How many tiling slots all_pairs_spawn_cell() below can fill - the n_mats
 * every caller of all_pairs_material_at() passes. Derived from MAT_COUNT
 * and MATX_ROOT (material_extended_t, material.h), not hand-counted, so a
 * new material or MATX_* slot grows this on its own. Smoke is deliberately
 * not seeded - see ALL_PAIRS_SKIPPED_ORDINARY. */
#define ALL_PAIRS_ORDINARY_COUNT ((int)MAT_COUNT - (int)(MAT_EMPTY + 1) - 1)
#define ALL_PAIRS_EXTENDED_COUNT ((int)MATX_ROOT + 1)
#define ALL_PAIRS_SPAWN_COUNT \
    (ALL_PAIRS_ORDINARY_COUNT + ALL_PAIRS_EXTENDED_COUNT + 1 /* gunpowder */)

/* THE SLOT COUNT MUST STAY PRIME, and that is not numerology.
 * all_pairs_material_at() gives row y the stride (y % (n-1)) + 1, so a row
 * of stride s visits only n / gcd(s, n) distinct values - where gcd > 1,
 * each pair at that difference needs its own differently-offset row. */

/* Measured at n = 20: stride 10 covered ONE antipodal pair per row and
 * needed ten rows; a 151-row band carries stride 10 eight times, so
 * MAT_SMOKE never met MATX_METAL. Prime n makes every stride coprime, one
 * row per stride suffices, and the band needs n-1 = 18 rows against 151. */
_Static_assert(ALL_PAIRS_SPAWN_COUNT == 19,
               "the tiling's slot count must stay PRIME or full pair "
               "coverage stops being guaranteed by construction - see the "
               "comment above. Adding a material means picking the next "
               "prime and choosing what else to seed or skip, not just "
               "letting this grow.");

/* Smoke is the one ordinary material the tiling does not seed, chosen
 * because fire produces it constantly - its own reactions still fire, it
 * simply arrives as a product instead of a seed. Dropping it is what makes
 * the count 19 rather than 20. */
#define ALL_PAIRS_SKIPPED_ORDINARY MAT_SMOKE

int all_pairs_material_at(int x, int y, int first, int n_mats);

/* Maps a tiling index (0 .. ALL_PAIRS_SPAWN_COUNT - 1) to the cell spec to
 * hand sand_spawn_cell() - see the definition in suite_sand_scenes.c for
 * the ordering. */
cell_t all_pairs_spawn_cell(int index);

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
