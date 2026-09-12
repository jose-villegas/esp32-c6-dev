/*
 * Shared benchmark scenes - see suite_sand_scenes.c.
 *
 * Each of these builds a scene once, both for its own correctness test in
 * suite_sand_scenes.c (does the scene actually keep reacting/boiling/
 * percolating/shattering the way it claims to?) and for the frame-budget
 * perf tests in suite_sand_perf.c (how expensive is a real device step
 * against this exact scene?) - one scene, two questions, so a regression in
 * either the mechanism or its performance shows up against the same
 * fixture rather than two that could quietly drift apart.
 */
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

/* Paints the tiling plus the deliberate gunpowder patches (see
 * suite_sand_scenes.c) into an already-initialised sand_t - one copy,
 * shared by the timed device scene and the host coverage test, so neither
 * can drift from what the other actually builds. */
void build_all_pairs_scene(sand_t *s);

/* Same reasoning as WATER_LAVA_IMPULSE_MAX/GUNPOWDER_BASIN_IMPULSE_MAX
 * below. Without an impulse buffer sand_explode() has nowhere to write, so
 * Gunpowder.explodes cannot fire no matter how the patches are placed - a
 * caller wanting them to detonate must sand_enable_impulses() with this
 * ceiling first. */
#define ALL_PAIRS_IMPULSE_MAX 2048

void build_four_liquid_scene(sand_t *s);
void build_lava_stress_scene(sand_t *s);
void build_smoke_and_steam_scene(sand_t *s);
void build_thermal_shock_scene(sand_t *s);
void build_boiler_scene(sand_t *s);

/* A small fire on a board that mostly cannot react - see the builder for
 * why the other reaction scenes cannot answer the same question. */
void build_campfire_scene(sand_t *s);
void build_wet_earth_scene(sand_t *s);

/* A stone tank with a zigzag of shelves down it and a pool at the bottom,
 * poured into from the top corner. The companion to the free-falling water
 * slab in the perf suite, which reaches move_liquid_grain()'s sideways half
 * zero times in a window - see the builder. */
void build_filling_basin_scene(sand_t *s);

/* One refill onto the top shelf, separate from the builder so a caller can
 * keep the cascade running for a whole measured window - water that has
 * finished arriving puts its blocks to sleep and costs nothing. */
void filling_basin_pour(sand_t *s);

/* Long enough for the first sheet to have reached the pool, so the window
 * times a cascade already running end to end rather than one still on its
 * way down. */
#define FILLING_BASIN_SETTLE_STEPS 220
#define FILLING_BASIN_POUR_EVERY   10
#define FILLING_BASIN_MEASURED_STEPS 30

/* Snow drifting onto a bed of sand and dirt, the two partners it is measured
 * to be dearest against. Callers must sand_set_crust() it up - the shipped
 * rate ices a bank over in minutes, so a scene left at it holds no ice at
 * all, the same case the water-over-lava scene forces its lava rolls for. */
void build_snowfall_scene(sand_t *s);

/* One more fall of snow onto the bank, separate from the builder for the
 * reason plant_bed_rain() is - see that function. */
void snowfall_drift(sand_t *s);

/* Measured at sand_set_crust(CRUST_ROLL_MAX): the bank's first ice appears
 * around step 110 and reaches 1,217 cells by 150, against 4,048 snow. Less
 * settling and the scene is snow on bare earth, which is a different and
 * cheaper board. */
#define SNOWFALL_SETTLE_STEPS 150
#define SNOWFALL_DRIFT_EVERY   10
#define SNOWFALL_MEASURED_STEPS 30

/* A bed of sand capped with damp dirt, seeded and rained on - the only scene
 * here in which anything grows. See the builder for the spacing rule. */
/* A grove of bushy trees, painted rather than grown - the shape the wood/leaf
 * gust shading is for. Trunk wood away from leaves is the scan's worst case;
 * canopy wood beside leaves is the dirtying's. See the builder. */
#define TREE_GROVE_TREES     4
#define TREE_GROVE_HEIGHT   70
#define TREE_GROVE_CANOPY_R 18
void build_tree_grove_scene(sand_t *s);

void build_plant_bed_scene(sand_t *s);

/* The plant bed above, walled down the middle, with an acid pour waiting on
 * one side and a lava pour on the other - the arena's three unreached
 * plant-family interactions in one board. See the builder. */
void build_plant_ruin_scene(sand_t *s);

/* The two pours, separate from the builder and from each other: a caller
 * grows the bed first and only then attacks it, and the two halves do not
 * peak together. Measured from the moment each lands, the lava has burnt
 * nearly all the greenery it is going to within 60 steps, while the acid
 * spends its first 60 eating the canopy and the soil cap and only then
 * reaches the roots, 112 of them down to 33 over the next 60. Pouring both
 * at once gives a window with one side or the other already spent. */
void plant_ruin_acid_pour(sand_t *s);
void plant_ruin_lava_pour(sand_t *s);

/* Acid first, then lava this much later, so the timed window lands on acid
 * in the roots and lava in a canopy it has not touched yet. */
#define PLANT_RUIN_ACID_LEAD_STEPS 60

/* One acid pour does not reach the roots at all: measured, it is entirely
 * spent on the canopy and the soil cap, and the bed goes on growing behind
 * it. Only a sustained pour eats down. */
#define PLANT_RUIN_ACID_EVERY 15

/* The window the frame-budget test times and the coverage test beside it
 * checks - one number, so neither can drift from the other. */
#define PLANT_RUIN_MEASURED_STEPS 30

/* Another fall of rain onto an existing bed. One pour is drunk dry in a few
 * hundred steps and growth then stops - see the definition. */
void plant_bed_rain(sand_t *s);

/* The plant brush being POURED, which is a different scene from a bed that
 * grows: a loose heap in motion rather than a standing garden. Every other
 * plant scene reaches step_one_falling_cell()'s support walk a handful of
 * times a step, because a grown tree is anchored and never asks. */
void build_plant_pour_scene(sand_t *s);

/* One stamp of the brush, dragged. Separate from the builder for the reason
 * plant_bed_rain() is; `step` sweeps it across the board. */
void plant_pour_stamp(sand_t *s, int step);

/* Enough to settle the earth and no more - the heap is the scene, and it is
 * built inside the timed window by the stamps themselves. */
#define PLANT_POUR_SETTLE_STEPS 60
#define PLANT_POUR_MEASURED_STEPS 120

/* Long enough after the last stamp that the heap has stopped moving. What a
 * board LOOKS like most of the time - a player pours a garden and then leaves
 * it there - and the one state no other row covers: every plant scene here is
 * chosen for something still happening in it. */
void build_dry_plant_heap_scene(sand_t *s);

#define PLANT_IDLE_SETTLE_STEPS 200

/* The schedule is chosen so every stage is still doing work in the timed
 * window. Measured over candidate 20-step windows: a bed settled 400 steps
 * produces ZERO leaves - its canopy has saturated, so the row times a
 * reject rather than the stage. At 230 plants, leaves and roots are all
 * still being produced. Both pours must precede the window, and pouring
 * earlier is worse: at 80 and 160 the canopy stalls at 137 leaves and never
 * moves again. */
#define PLANT_BED_SETTLE_STEPS 230
#define PLANT_BED_RAIN_A       100
#define PLANT_BED_RAIN_B       170

/* THE SAME BED, FINISHED: no second pour, and left until the trees have both
 * stopped growing and drunk the ground dry - the state a garden spends the
 * rest of its life in, and the one every row above stops short of. Measured
 * on this bed and seed: structure stops changing at 759, the last damp cell
 * evaporates at 1,490. */
#define MATURE_TREE_SETTLE_STEPS 1600

/* Fresh soil laid over the canopy and rained on - the one thing a finished
 * tree will still answer, and the reason a skip here has to be shown resuming
 * rather than merely being cheap. Separate from the builder for the reason
 * plant_bed_rain() is. */
void mature_tree_replant(sand_t *s);

#define MATURE_TREE_REPLANT_STEPS 400

/* Same real device impulse budget the vent-spam scene this replaced used -
 * the app's own buffer is sized APP_IMPULSE_MAX (2048), and this scene
 * should be fighting the same memory ceiling a real device pour actually
 * has, not a looser one a differently-sized test buffer would hide. */
#define WATER_LAVA_IMPULSE_MAX 2048
void build_water_over_lava_scene(sand_t *s);

/* A block, not a single cell: fire is KIND_GAS, so under the random walk a
 * lone spark drifts away before it can light anything and the pile never
 * detonates at all - checked, still zero bursts over a window 6.7x
 * longer. */
#define GUNPOWDER_BASIN_SPARK 2

/* Same reasoning as WATER_LAVA_IMPULSE_MAX above - the app's own fixed,
 * device-heap-sized APP_IMPULSE_MAX, not a formula in this scene's own
 * blast radius. */
#define GUNPOWDER_BASIN_IMPULSE_MAX 2048

/* The coverage test's own measured window, reused by the frame-budget
 * test beside it - see either test's comment (suite_sand_scenes.c) for
 * why 90 steps, no settling. */
#define GUNPOWDER_BASIN_MEASURED_STEPS 90

void build_gunpowder_basin_scene(sand_t *s);

/* The landscape set. Every other scene here falls down grid +Y; the board
 * is played sideways, down grid +X (bd esp32c6-1z6). Not a relabelling: the
 * grid is 184x224, so a landscape pour drops 184 cells onto a floor 224
 * wide, and a 32x64 block turns its long side to the stream. Built for it,
 * not transposed. */
#define LANDSCAPE_GX 1000

/* The app's own brush - POUR_RADIUS_PX 10 at 2 px per cell - dragged along
 * the landscape ceiling, which is column 0. The stream it lays down is
 * about ten cells across, and that width against the block's extent across
 * it is the whole of what these scenes are here to expose. */
#define LANDSCAPE_POUR_RADIUS 5

/* How much of the board the bed holds before anything is poured onto it.
 * Both shares come from the interaction arena's landscape sweep, which
 * priced sand-and-water at exactly these two depths. */
#define LANDSCAPE_BED_PERCENT      40
#define LANDSCAPE_DEEP_BED_PERCENT 65

/* Pours the bed against the landscape floor and settles it, so the coverage
 * test and the frame-budget row beside it open on the same board. Poured,
 * not drawn: a drawn face is a plane at one column, where a poured one
 * carries repose slopes that reach 15 columns further toward the ceiling,
 * and the stream meets those, not a wall. */
void build_landscape_bed_scene(sand_t *s);
void build_landscape_deep_bed_scene(sand_t *s);

/* One step of the pour that follows, `step` sweeping the brush along the
 * ceiling. Separate from the builder for the reason plant_bed_rain() is. */
void landscape_water_pour(sand_t *s, int step);
void landscape_sand_pour(sand_t *s, int step);

/* Measured: the bed reaches its share in these many steps at eight stamps
 * apiece, and every block is asleep by the end of the settle. A dragged
 * brush this fast lays the same bed a single-stamp drag does - front column
 * 95 against 96 over five times the steps. */
#define LANDSCAPE_BED_STAMPS       8
#define LANDSCAPE_BED_STEPS      300
#define LANDSCAPE_DEEP_BED_STEPS 490
#define LANDSCAPE_SETTLE_STEPS   150

/* The window times a pour ALREADY RUNNING, the reason the filling basin
 * primes too: measured from first contact the cost climbs the whole way -
 * 105 us over 60 steps against 172 over 150, on the same board - because
 * the water is still reaching fresh bed. Past 150 steps of pour it is flat
 * within 2% whatever window follows. */
#define LANDSCAPE_PRIME_STEPS    150
#define LANDSCAPE_MEASURED_STEPS  90
#define LANDSCAPE_POUR_STAMPS      2
