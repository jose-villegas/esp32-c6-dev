/*=============================================================================
 * Portable suite: the falling-sand automaton - dune blast scenes and their
 * variants - water pool, vessel, wood floor, layered dune.
 *
 * Split out of suite_sand.c (bd esp32c6 test-suite-refactor), which had grown
 * past 32,000 lines across 500+ tests. Shared fixtures and assertion helpers
 * live in suite_sand_common.{c,h} - see that header.
 *===========================================================================*/
#include <math.h>   /* not every file in the split still needs atan2()/M_PI,
                     * but every file inherited suite_sand.c's own include
                     * block rather than being pruned by hand, to keep the
                     * split itself mechanical and low-risk */
#include <stdlib.h>
#include <string.h>

#ifndef M_PI
/* Not every libc defines this in <math.h> without a feature-test macro this
 * file has no other reason to set (MinGW's, notably, on the host build) -
 * cheaper to supply it directly than to widen this file's own feature-test
 * exposure for one constant. */
#define M_PI 3.14159265358979323846
#endif

#include "unity.h"
#include "suites.h"

#include "sand.h"
#include "sand_priv.h"
#include "util/intmath.h"
#include "suite_sand_common.h"

/* =========================================================================
 * BLAST SCENES - a settled dune and a detonation at its centre, following
 * the same builder / host-guard-test / device-log-lines shape as
 * build_lava_stress_scene(), build_thermal_shock_scene() and
 * build_boiler_scene() above.
 *
 * WHY THIS EXISTS. Every blast test above this line checks an internal
 * detail of the mechanism - a specific cell's material, an entry's queue
 * order, a count that must stay under some bound - and every one of them
 * passed for four straight rounds while a real detonation on a real
 * device only ever disturbed the top tenth of its own disc (see c0e01a1's
 * own commit message for the full account). None of that internal
 * correctness is proof that a blast LOOKS like a blast - an outcome a
 * player can actually watch happen. This is the first blast test in the
 * file that measures the outcome itself: does material end up outside
 * where it started, how far, and how much of it is gone rather than
 * moved.
 * ========================================================================= */

/* How many steps make one "has anything changed" batch, and how many
 * batches settle_fully() below will spend looking for an unchanged one
 * before giving up. 20 steps a batch keeps the memcmp cost - one pass
 * over the whole grid - proportionate to the stepping it is checking; 300
 * batches (6,000 steps) is a safety rail against an actual bug turning
 * this into an infinite loop, not a number any real dune has come close
 * to needing. */
#define DUNE_SETTLE_BATCH_STEPS 20
#define DUNE_SETTLE_MAX_BATCHES 300

/* A 64-bit FNV-1a fold of the whole grid - settle_fully() below uses two of
 * these, one before a batch of steps and one after, to answer "did
 * anything change" without keeping a second copy of the grid around to
 * memcmp against.
 *
 * That second copy is exactly what this replaced, and the replacement is
 * not a nicety: `scratch`, a caller-owned REAL_W*REAL_H byte buffer, sat
 * alongside `big` for the whole settling phase of every one of this
 * section's five tests - 41,216 bytes each, 82,432 together, MORE than
 * the roughly 66,632 bytes this device has free once the display
 * framebuffer is carved out of the heap, before a single other buffer
 * (the block map, the impulse buffer, a footprint mask) is even in the
 * picture. No amount of shrinking those other buffers can fix that - two
 * full-grid buffers simply do not fit in a heap smaller than either one
 * doubled - so unlike the footprint mask below (still a mask, just a
 * bitset instead of a byte array), the fix here is to not keep a second
 * full-grid buffer at all.
 *
 * A hash cannot prove two DIFFERENT grids are different the way a byte
 * compare can - a collision is possible in principle, and this trades
 * that certainty for one that is merely overwhelming: FNV-1a's avalanche
 * behaviour means a single flipped cell changes most of the 64 output
 * bits, not one, so two genuinely different 41,216-byte grids landing on
 * the same 64-bit fold by chance is far less likely than an actual defect
 * turning up somewhere else in this file's other 500-plus tests. This
 * project already accepts probabilistic reasoning at exactly this scale
 * elsewhere (rng_next() itself, or the "1 in 2^32" a hash this size
 * implies); nothing about a settling check demands stronger proof than a
 * live simulation's own RNG already carries. */
static uint64_t grid_checksum(const uint8_t *cells, size_t len)
{
    uint64_t h = 0xcbf29ce484222325ULL;    /* FNV-1a 64-bit offset basis */
    for (size_t i = 0; i < len; i++) {
        h ^= cells[i];
        h *= 0x100000001b3ULL;             /* FNV-1a 64-bit prime */
    }
    return h;
}

/* Steps `s` until one whole batch of DUNE_SETTLE_BATCH_STEPS produces
 * literally no change to the grid, which is what "settled" has to mean
 * for a scene a blast is about to be measured against. A fixed step count
 * can only ever be a guess at how long a pile this size takes to stop
 * moving - a guess that undershoots would silently start measuring a pile
 * that was still falling, confusing the blast's own throw with gravity
 * still finishing its own job.
 *
 * Returns whether it actually converged within the budget above - a
 * caller measuring a scene against this dune must assert on that rather
 * than trust it silently, since a dune that never finished settling is
 * not the scene the rest of the test thinks it is. */
static bool settle_fully(sand_t *s, size_t cells_len)
{
    for (int batch = 0; batch < DUNE_SETTLE_MAX_BATCHES; batch++) {
        const uint64_t before = grid_checksum(s->cells, cells_len);
        for (int i = 0; i < DUNE_SETTLE_BATCH_STEPS; i++) {
            sand_step(s, 0, 1000, 0);
        }
        if (grid_checksum(s->cells, cells_len) == before) {
            return true;
        }
    }
    return false;
}

/* The settled-footprint mask, ONE BIT PER CELL rather than a bool[] - the
 * identical treatment 565f72e already gave the thermal shock scene's
 * ever_cullet mask, and for the identical reason: this is a byte-per-cell
 * flag that only ever holds 0 or 1, on the same REAL_W*REAL_H grid, on the
 * same device budget. A bool[] here would cost 41,216 bytes; the bitset
 * costs 5,152. See EVER_CULLET_BYTES's own comment above for the fuller
 * accounting - this is the same fix, applied to this section's own mask
 * rather than reusing that one, since the two masks answer unrelated
 * questions (settled footprint here, sticky cullet there) and have no
 * reason to share storage or a lifetime. */
#define DUNE_FOOTPRINT_BYTES \
    (((size_t)REAL_W * (size_t)REAL_H + 7) / 8)

static inline bool footprint_get(const uint8_t *mask, size_t idx)
{
    return (mask[idx >> 3] >> (idx & 7)) & 1u;
}

static inline void footprint_set(uint8_t *mask, size_t idx)
{
    mask[idx >> 3] |= (uint8_t)(1u << (idx & 7));
}

/* HOW FAR PAST THE DUNE'S OWN EDGE, not how far from an arbitrary point
 * inside it - see this file's own dune-scene tests for why "distance from
 * the detonation centre" turned out to be the wrong question. Found by
 * searching outward from (x, y) in expanding square rings - the same
 * eight-directions-at-once shape ring_dir() in sand_priv.h moves grains
 * in - checking only each ring's own perimeter against `footprint` until
 * one of its bits is set. That is a Chebyshev (8-connected) distance
 * transform, the exact same value a multi-source breadth-first flood
 * fill seeded from every footprint cell would compute for this one cell -
 * two ways of answering the identical question, not two different
 * questions - so switching to it changes nothing about WHAT a grain that
 * merely slid down the dune's own slope reads as versus one genuinely
 * thrown clear (see this file's own top-of-section comment for why that
 * distinction is the entire point).
 *
 * WHY NOT THE FLOOD FILL THIS REPLACED: it answered the question for
 * every one of the grid's 41,216 cells whether or not anything downstream
 * ever asked, which needed `dist` and `queue`, REAL_W*REAL_H ints each -
 * 164,864 bytes apiece, 329,728 together. This scene only ever asks for
 * the handful of cells that turn out to be outside the settled footprint
 * after a blast - 105 of them in a measured run, against the 41,216 the
 * flood fill priced itself for regardless. A bounded RECTANGLE around the
 * footprint - the fix this section's dune scene otherwise follows for
 * `footprint` itself, just scoped down instead of reshaped - could not
 * replace it either: this scene's own settled dune measured 127 cells
 * wide (69% of the grid's own 184-cell width, a wide, short pile rather
 * than a tall narrow one), and even a ZERO-margin box exactly matching
 * that footprint's bounding rectangle costs 24,003 bytes at the smallest
 * correct per-cell types (a 1-byte Chebyshev distance, a 2-byte queue
 * slot - see this file's own commit message for both derivations) against
 * roughly 7,952 bytes left once `big`, `blocks`, `impulses` and the
 * footprint bitset above are accounted for. A per-query outward search
 * has no rectangle to fit into at all: its cost is proportional to how
 * FAR a query cell sits from the nearest footprint cell, not to the
 * footprint's own size, so this pile's actual queries - every one of them
 * resolving within two or three rings, measured - stay cheap regardless
 * of how wide the pile itself gets, and it needs no storage beyond this
 * function's own local variables.
 *
 * `cap` bounds the search so a genuinely pathological grid still
 * terminates - callers pass the largest Chebyshev distance any two cells
 * on this grid could possibly have, (max(REAL_W, REAL_H) - 1) = 223, so
 * the cap can never itself produce a wrong answer for a cell this grid
 * actually contains; it only bounds the search, the same role CRACK_MAX
 * plays for crack_run() in sand_reactions.c, not a correctness knob. */
static int nearest_footprint_distance(const uint8_t *footprint, int w, int h,
                                      int x, int y, int cap)
{
    if (footprint_get(footprint, (size_t)y * (size_t)w + (size_t)x)) {
        return 0;
    }

    for (int r = 1; r <= cap; r++) {
        const int x0 = x - r, x1 = x + r;
        const int y0 = y - r, y1 = y + r;

        for (int xx = x0; xx <= x1; xx++) {
            if (xx < 0 || xx >= w) {
                continue;
            }
            if (y0 >= 0 &&
                footprint_get(footprint, (size_t)y0 * (size_t)w + (size_t)xx)) {
                return r;
            }
            if (y1 < h &&
                footprint_get(footprint, (size_t)y1 * (size_t)w + (size_t)xx)) {
                return r;
            }
        }
        for (int yy = y0 + 1; yy <= y1 - 1; yy++) {
            if (yy < 0 || yy >= h) {
                continue;
            }
            if (x0 >= 0 &&
                footprint_get(footprint, (size_t)yy * (size_t)w + (size_t)x0)) {
                return r;
            }
            if (x1 < w &&
                footprint_get(footprint, (size_t)yy * (size_t)w + (size_t)x1)) {
                return r;
            }
        }
    }
    return cap + 1;   /* not found within cap - see this function's own comment */
}

/* The largest Chebyshev distance any two cells on this grid could ever
 * have - see nearest_footprint_distance()'s own comment for why this is
 * the cap it is called with, and why that makes the cap a search bound
 * rather than a correctness one. */
#define NEAREST_FOOTPRINT_CAP ((REAL_W > REAL_H ? REAL_W : REAL_H) - 1)

/* Mirrors app_sand.c's DETONATE_RADIUS_PX/APP_IMPULSE_MAX exactly, at the
 * same CELL_MIN scale REAL_W/REAL_H already represent (see their own
 * comment above) - so a sweep run against this scene, and the numbers it
 * reports, read on the same scale a real device detonation does, not
 * some arbitrary test-only radius.
 *
 * WAS 24 (48 px), DOUBLED TO 48 (96 px) - following a device request,
 * "it needs a much bigger radius in general" - and THAT DOUBLING BRIEFLY
 * BROKE THE FEATURE OUTRIGHT on real hardware, TWICE, for two different
 * reasons caught by two different device flashes. First: the impulse
 * buffer used to be sized FROM this radius
 * (`(355*r*r)/113 + 5*r + 3` entries), so doubling it demanded a ~43.8 KB
 * allocation nothing on this board could satisfy. Second, after that got
 * fixed by decoupling buffer size from radius (see below) and sizing the
 * fixed budget against a ~76 KB TOTAL free-heap boot-log figure instead:
 * a live serial capture at that "fixed" budget still failed, showing
 * `heap_caps_get_largest_free_block()` stuck at an identical 14,592
 * bytes across three different quality settings - proof the relevant
 * number was never total free heap at all, but the single largest
 * contiguous run, which can be far smaller than the sum of everything
 * technically free. Neither failure was visible to this test's fixed RNG
 * seed and unlimited host `malloc()` - nothing here ever fails to
 * allocate, which is exactly why this bug needed a device twice to be
 * believed. See SAND_IMPULSE_BUDGET_BYTES's own comment in app_sand.c
 * for the full arithmetic of both incidents.
 *
 * NEITHER FIX WAS A SMALLER RADIUS. Both were decoupling buffer size
 * from radius entirely: APP_IMPULSE_MAX (app_sand.c) is now a FIXED
 * entry count chosen once from the device's own heap budget - now
 * against the observed largest-contiguous-block number, not total free
 * heap - and sand_explode() itself (sand.c) now THINS its own seeding
 * density automatically whenever a disc's true cell count would exceed
 * whatever buffer it was actually given - evenly, across the whole disc,
 * rather than truncating its shape - see queue_outward_impulse()'s own
 * comment in sand.c. That decoupling is what let the radius become a
 * genuinely free choice again - which is exactly what it became next:
 * a real device confirmed 96 px allocating and detonating without a
 * crash, at a visibly thinned density, and the user chose to trade that
 * size back down for a SMALLER radius at FULL density instead, on the
 * actual measured numbers (see DETONATE_RADIUS_PX's own comment in
 * app_sand.c for the full account and the "why 25 cells, not a round
 * number" derivation). This constant follows DETONATE_RADIUS_PX's own
 * value rather than drifting from it, same as before - the point of the
 * fix was never that the radius COULDN'T shrink, only that it no longer
 * HAD to just to keep the buffer allocating. */
#define DUNE_BLAST_RADIUS 25

/* A FIXED ENTRY COUNT MIRRORING APP_IMPULSE_MAX EXACTLY, not a formula in
 * DUNE_BLAST_RADIUS - see APP_IMPULSE_MAX's own comment in app_sand.c for
 * why the two constants split apart: this scene's own impulse buffer no
 * longer needs to be "big enough for whatever DUNE_BLAST_RADIUS's disc
 * requires", because sand_explode() now degrades its own seeding density
 * to fit whatever buffer it is actually given. What this DOES still need
 * to mirror is the app's real device budget, not the app's radius - a
 * host test buffer sized any differently would measure a blast fighting
 * a different memory ceiling than the one the device actually has, which
 * defeats the entire point of this scene reading "on the same scale a
 * real device detonation does" (this file's own top comment, above). */
#define DUNE_IMPULSE_MAX  2048

/* A settled dune, poured rather than painted - the same way app_sand.c's
 * own starting heap is: sand_spawn() dropped from height and left to find
 * its own angle of repose under ordinary gravity, exactly what a player's
 * finger produces. A painted rectangle would not be a dune - it has no
 * slope for a blast to disturb, and its own square corners would slide
 * under plain gravity before an explosion ever got a turn, which would
 * muddy "the blast displaced this" with "gravity was already going to".
 *
 * Settling is deliberately NOT done here - see settle_fully() above and
 * this file's other build_*_scene() functions, none of which step at
 * all: a builder places material, and whatever steps a caller needs
 * (rest, in this file's usual case; convergence, in this scene's) is the
 * caller's own job, so the builder stays reusable exactly as it is by a
 * caller that wants a MID-fall dune instead of a settled one. */
static void build_sand_dune_scene(sand_t *s)
{
    sand_spawn(s, REAL_W / 2, REAL_H / 4, REAL_W / 5, MAT_SAND);
}

/* THE OUTCOME THIS ROUND WAS MISSING - see this section's own top comment.
 * Three numbers, not a boolean, because a boolean cannot tell power from
 * reach from destruction apart, and conflating them is exactly how a
 * change that helps one and hurts another would go unnoticed:
 *
 *   grains outside footprint   did anything escape the dune AT ALL - the
 *                              user's own criterion, and the real
 *                              pass/fail test
 *   maximum throw distance     how FAR the furthest grain got, which a
 *                              plain yes/no on "outside" cannot
 *                              distinguish from "barely"
 *   material destroyed         how much was converted or lost rather
 *                              than thrown - the core's own fire cost,
 *                              not a mistake to chase out
 *
 * "Outside the footprint" is measured against the SETTLED footprint,
 * recorded once and only once, right after settle_fully() returns and
 * before sand_explode() is ever called - a cell counts as displaced only
 * if it holds MAT_SAND now and was NOT already occupied by the dune
 * before the blast touched anything. Checking material specifically
 * excludes the fire the core itself becomes (see SAND_EXPLODE_CORE_
 * DIVISOR's own comment in sand.h) from counting as an "escaped grain" -
 * fire landing outside the footprint is the fireball's own edge doing
 * exactly what it is supposed to, not sand flying off. */
static void test_the_sand_dune_scene_throws_grains_beyond_its_own_footprint(void)
{
    const size_t cells_len = (size_t)REAL_W * REAL_H;
    uint8_t   *big      = malloc(cells_len);
    uint8_t   *blocks   = malloc(((REAL_W + SAND_BLOCK_W - 1) / SAND_BLOCK_W) *
                                 ((REAL_H + SAND_BLOCK_H - 1) / SAND_BLOCK_H));
    uint8_t   *footprint = malloc(DUNE_FOOTPRINT_BYTES);
    impulse_t *impulses  = malloc((size_t)DUNE_IMPULSE_MAX * sizeof(impulse_t));
    const bool have_all = (big != NULL && blocks != NULL &&
                          footprint != NULL && impulses != NULL);
    if (!have_all) {
        free(big); free(blocks); free(footprint); free(impulses);
        TEST_FAIL_MESSAGE("need a grid, a block map, a one-bit-per-cell "
                          "footprint mask, and an impulse buffer for the "
                          "dune scene, and at least one failed to allocate");
    }
    memset(footprint, 0, DUNE_FOOTPRINT_BYTES);

    sand_t real;
    sand_init(&real, big, REAL_W, REAL_H, 51u);
    sand_enable_sleeping(&real, blocks);
    sand_set_scatter(&real, SAND_SCATTER_PER_MATERIAL);
    sand_set_decay(&real, SAND_DECAY_PER_MATERIAL);
    sand_set_mobility(&real, SAND_MOBILITY_PER_MATERIAL);
    sand_enable_impulses(&real, impulses, DUNE_IMPULSE_MAX);

    build_sand_dune_scene(&real);
    const bool settled = settle_fully(&real, cells_len);

    /* The settled footprint, and its bounding box - "detonate at its
     * centre" means the centre of what actually settled, which is lower
     * and narrower than where sand_spawn() dropped it, not the drop
     * point itself. */
    int before = 0;
    int min_x = REAL_W, max_x = -1, min_y = REAL_H, max_y = -1;
    for (int y = 0; y < REAL_H; y++) {
        for (int x = 0; x < REAL_W; x++) {
            const bool occupied = sand_at(&real, x, y) != SAND_EMPTY;
            if (occupied) {
                footprint_set(footprint, (size_t)y * REAL_W + x);
                before++;
                if (x < min_x) min_x = x;
                if (x > max_x) max_x = x;
                if (y < min_y) min_y = y;
                if (y > max_y) max_y = y;
            }
        }
    }

    const int cx = (min_x + max_x) / 2;
    const int cy = (min_y + max_y) / 2;

    /* AT THE 25-CELL RADIUS THIS BLASTS AT (DUNE_BLAST_RADIUS - a
     * deliberate, user-chosen retune toward "small and dense" after a
     * real device confirmed a bigger, thinned blast working, not a value
     * forced down by a memory bug - see that constant's own comment for
     * the full account and the tradeoff it was chosen over). A settled
     * dune's own bounding-box centre sits only ~30 cells above the true
     * floor (a wide, short pile with height well under a 48-cell radius,
     * the value this scene used before), so unlike that larger radius
     * this smaller one does not reliably reach REAL_H - the grid's own
     * bottom edge - and does not need to: full-density seeding at this
     * radius is what the scene is measuring, not edge contact. Verified
     * this still measures something real rather than a degenerate scene:
     * "outside"/"destroyed" stayed small fractions of `before` (an
     * 800-seed sweep against the real, shipped sand_explode() - at full
     * density, zero thinning, since this radius's true disc fits inside
     * DUNE_IMPULSE_MAX entirely - averaged 2.63% and 1.94% at this
     * radius and budget, not a plurality of the dune, still less all of
     * it) - see this test's own assertions below, unchanged, for the
     * actual bar. */
    sand_explode(&real, cx, cy, DUNE_BLAST_RADIUS);

    /* Past the deterministic flight-time bound (see SAND_IMPULSE_SPEED_
     * RAMP's own comment in sand.h), computed from the constants rather
     * than a bare number so this keeps measuring the same thing after
     * either one is retuned, plus margin for gravity to bring a landed
     * grain to rest and for a water/collapse scene's own refill to
     * finish. */
    const int max_lifetime = (SAND_EXPLODE_INITIAL_SPEED +
                              SAND_IMPULSE_SPEED_RAMP - 1) /
                             SAND_IMPULSE_SPEED_RAMP;
    for (int i = 0; i < max_lifetime + 20; i++) {
        sand_step(&real, 0, 1000, 0);
    }

    /* Distance to the NEAREST footprint cell, not to the detonation
     * centre - see nearest_footprint_distance()'s own comment for why: a
     * straight-line distance from one fixed interior point conflates a
     * grain genuinely thrown clear with one that merely slid down the
     * dune's own slope and stopped at its base, since both can end up
     * geometrically far from the centre for reasons that have nothing
     * to do with how hard the blast pushed. Computed per outside cell
     * rather than once for the whole grid - see that function's own
     * comment for why this scene's own footprint shape makes a
     * precomputed distance field, bounded or not, the wrong tool here. */
    int outside = 0;
    int max_throw = 0;
    for (int y = 0; y < REAL_H; y++) {
        for (int x = 0; x < REAL_W; x++) {
            if (footprint_get(footprint, (size_t)y * REAL_W + x)) {
                continue;   /* inside the original dune - not an escape */
            }
            if (CELL_MATERIAL(sand_at(&real, x, y)) != MAT_SAND) {
                continue;   /* fire, not a grain - see this test's own comment */
            }
            outside++;
            const int d = nearest_footprint_distance(footprint, REAL_W, REAL_H,
                                                      x, y, NEAREST_FOOTPRINT_CAP);
            if (d > max_throw) {
                max_throw = d;
            }
        }
    }
    const int after = sand_count(&real);
    const int destroyed = before - after;

    free(big);
    free(blocks);
    free(footprint);
    free(impulses);

    TEST_ASSERT_TRUE_MESSAGE(settled,
        "the dune must actually stop moving within the settle budget - a "
        "pile still falling is not a dune, it is a rectangle in the "
        "middle of becoming one");
    TEST_ASSERT_GREATER_THAN_MESSAGE(0, before,
        "the dune must have settled into SOMETHING - an empty footprint "
        "means sand_spawn() itself failed, not that the blast did");
    TEST_ASSERT_GREATER_THAN_MESSAGE(0, outside,
        "at least one grain must land outside the dune's own settled "
        "footprint - the user's own criterion, and the one no existing "
        "test checked: a blast that only ever disturbs its own footprint "
        "reads as a shuffle, not a throw");
    TEST_ASSERT_GREATER_OR_EQUAL_INT_MESSAGE(1, max_throw,
        "the furthest grain must land at least one step past the dune's "
        "own edge, by the corrected (nearest-footprint-cell) distance - "
        "this bar is deliberately low for now: measured at exactly 1 "
        "with today's constants, which is the same finding that motivates "
        "the retune and the displacement work queued right after this "
        "commit, and it should rise once either lands");
    TEST_ASSERT_GREATER_OR_EQUAL_INT_MESSAGE(0, destroyed,
        "destruction is bounded below by zero - sand_count() must never "
        "rise from a blast, whatever else changes about it");
    TEST_ASSERT_LESS_THAN_MESSAGE(before / 2, destroyed,
        "losing more than half the dune to the core's own fire is a sign "
        "the core divisor has drifted back toward eating the blast "
        "rather than flashing it - see SAND_EXPLODE_CORE_DIVISOR's own "
        "comment in sand.h");
}

/* =========================================================================
 * VARIANTS ON THE SAME DUNE - water pool, stone vessel, wood, layered
 * dune - each reusing settle_fully()/DUNE_BLAST_RADIUS/DUNE_IMPULSE_MAX
 * above rather than inventing its own settling or sizing rules.
 * ========================================================================= */

/* The base dune, plus a deep pool of water along the right third of the
 * grid - deep enough that a blast thrown into it still leaves plenty of
 * water to flow back in with, not just a thin sheet that boils away
 * entirely. Detonating INSIDE the pool (see the guard test below) is
 * what actually exercises "does the cavity collapse and refill", not
 * detonating in the dune and merely having water somewhere on the same
 * screen. */
static void build_dune_beside_water_scene(sand_t *s)
{
    sand_spawn(s, REAL_W / 2, REAL_H / 4, REAL_W / 5, MAT_SAND);

    /* Laid down at roughly the depth this volume settles to anyway,
     * spread across the basin, rather than stacked in the right-hand
     * third. The old shape had its left face open, so the pool spent 2,480
     * steps - 124 settle batches against 29-39 for every other scene in
     * this file, and ~98% of this test's runtime - travelling sideways to
     * reach the same equilibrium. Same water, same basin, same claims;
     * it simply starts where it was always going to end up. */
    const int pool_depth = 38;
    for (int y = REAL_H - pool_depth; y < REAL_H; y++) {
        for (int x = 0; x < REAL_W; x++) {
            sand_set(s, x, y, CELL_MAKE(MAT_WATER, MASS_MAX));
        }
    }
}

/* A cavity in a liquid is not a cavity in sand: nothing here needed the
 * ring-order fix or the cap sizing at all, but it is the one place in
 * this file that checks the claim from Impulse-Mechanics.md's own device
 * checklist - "detonate in water: the cavity should collapse and
 * slosh" - at more than a hand-wave. Detonating inside the pool, not the
 * dune, is deliberate: the dune already has its own scene above, and
 * mixing the two claims into one scene would leave neither checked
 * cleanly. */
/* How much WATER, and how much EMPTY, sits within `r` of (cx, cy). The
 * refill claim below needs both, and needs them counted by material: the
 * cavity a disturbance opens in a pool gets filled by falling sand or by
 * impulse-thrown debris whether or not the liquid can flow at all, so
 * "something is there now" is not evidence of anything. */
static int water_within(const sand_t *s, int cx, int cy, int r)
{
    int n = 0;
    for (int y = cy - r; y <= cy + r; y++) {
        for (int x = cx - r; x <= cx + r; x++) {
            if ((unsigned)x >= (unsigned)s->w || (unsigned)y >= (unsigned)s->h) {
                continue;
            }
            const int dx = x - cx, dy = y - cy;
            if (dx * dx + dy * dy > r * r) {
                continue;
            }
            if (CELL_MATERIAL(sand_at(s, x, y)) == MAT_WATER) {
                n++;
            }
        }
    }
    return n;
}

static int empty_within(const sand_t *s, int cx, int cy, int r)
{
    int n = 0;
    for (int y = cy - r; y <= cy + r; y++) {
        for (int x = cx - r; x <= cx + r; x++) {
            if ((unsigned)x >= (unsigned)s->w || (unsigned)y >= (unsigned)s->h) {
                continue;
            }
            const int dx = x - cx, dy = y - cy;
            if (dx * dx + dy * dy > r * r) {
                continue;
            }
            if (sand_at(s, x, y) == SAND_EMPTY) {
                n++;
            }
        }
    }
    return n;
}

static void test_the_water_pool_scene_refills_its_own_cavity(void)
{
    const size_t cells_len = (size_t)REAL_W * REAL_H;
    uint8_t   *big     = malloc(cells_len);
    uint8_t   *blocks  = malloc(((REAL_W + SAND_BLOCK_W - 1) / SAND_BLOCK_W) *
                                ((REAL_H + SAND_BLOCK_H - 1) / SAND_BLOCK_H));
    impulse_t *impulses = malloc((size_t)DUNE_IMPULSE_MAX * sizeof(impulse_t));
    const bool have_all = (big != NULL && blocks != NULL && impulses != NULL);
    if (!have_all) {
        free(big); free(blocks); free(impulses);
        TEST_FAIL_MESSAGE("need a grid, a block map and an impulse buffer "
                          "for the water pool scene, and at least one "
                          "failed to allocate");
    }

    sand_t real;
    sand_init(&real, big, REAL_W, REAL_H, 61u);
    sand_enable_sleeping(&real, blocks);
    sand_set_scatter(&real, SAND_SCATTER_PER_MATERIAL);
    sand_set_decay(&real, SAND_DECAY_PER_MATERIAL);
    sand_set_mobility(&real, SAND_MOBILITY_PER_MATERIAL);
    sand_enable_impulses(&real, impulses, DUNE_IMPULSE_MAX);

    build_dune_beside_water_scene(&real);
    const bool settled = settle_fully(&real, cells_len);

    int water_before = 0;
    for (int y = 0; y < REAL_H; y++) {
        for (int x = 0; x < REAL_W; x++) {
            if (CELL_MATERIAL(sand_at(&real, x, y)) == MAT_WATER) {
                water_before++;
            }
        }
    }

    /* Well inside the pool, away from its own edges - see this function's
     * own top comment for why detonating in the dune instead would not
     * exercise the claim this test exists for. */
    /* FOUND, not hardcoded. A fixed row only lands inside the pool for one
     * particular water level, so it silently becomes a precondition on
     * where the pool happened to settle - and then any unrelated change to
     * how water or sand comes to rest fails this test for a reason that has
     * nothing to do with what it tests. Descending from the surface keeps
     * the centre genuinely submerged whatever the level turns out to be. */
    const int cx = (REAL_W * 5) / 6;
    int surface_y = -1;
    for (int y = 0; y < REAL_H; y++) {
        if (CELL_MATERIAL(sand_at(&real, cx, y)) == MAT_WATER) {
            surface_y = y;
            break;
        }
    }
    TEST_ASSERT_GREATER_THAN_MESSAGE(-1, surface_y,
        "the pool must have a water surface in the column this test "
        "detonates in, or there is no pool to test");
    const int cy = surface_y + 12 < REAL_H - 2 ? surface_y + 12 : REAL_H - 2;
    const int centre_material_before = CELL_MATERIAL(sand_at(&real, cx, cy));

    sand_explode(&real, cx, cy, DUNE_BLAST_RADIUS);

    const int max_lifetime = (SAND_EXPLODE_INITIAL_SPEED +
                              SAND_IMPULSE_SPEED_RAMP - 1) /
                             SAND_IMPULSE_SPEED_RAMP;
    for (int i = 0; i < max_lifetime + 40; i++) {
        sand_step(&real, 0, 1000, 0);
    }

    const bool centre_refilled = sand_at(&real, cx, cy) != SAND_EMPTY;

    int water_after = 0;
    for (int y = 0; y < REAL_H; y++) {
        for (int x = 0; x < REAL_W; x++) {
            if (CELL_MATERIAL(sand_at(&real, x, y)) == MAT_WATER) {
                water_after++;
            }
        }
    }

    /* THE CLAIM THIS TEST IS NAMED FOR, asserted at last.
     *
     * Until 2026-09-01 the only refill check was "the blast's own centre
     * is not empty" - which cannot fail, for two compounding reasons. The
     * blast never empties that centre (queue_outward_impulse() skips it by
     * design, sand.c), and in a fully packed region an impulse SWAPS two
     * occupied cells, so occupancy is conserved and no hole is opened
     * anywhere. Measured: with move_liquid_grain() stubbed to return false,
     * so liquids cannot move AT ALL, this test still passed.
     *
     * So the cavity is carved directly rather than hoped for from the
     * blast, and the assertion asks for WATER back, not merely for
     * something. Measured over 113 carved cells: 89 refill normally, 32
     * with liquids immobile; half the carved count sits between them with
     * roughly 50% margin either way. Disabling cross-flow levelling instead
     * refills 113 and passes, correctly - water falling vertically is a
     * different mechanism, and test_a_pool_settles_at_the_angle_it_is_
     * tilted_to is the test that dies when cross-flow does.
     *
     * Carved AFTER water_after is counted, so the conservation assertion
     * above still measures the blast alone. */
    const int carve_r = 6;
    for (int y = cy - carve_r; y <= cy + carve_r; y++) {
        for (int x = cx - carve_r; x <= cx + carve_r; x++) {
            if ((unsigned)x >= (unsigned)REAL_W ||
                (unsigned)y >= (unsigned)REAL_H) {
                continue;
            }
            const int ddx = x - cx, ddy = y - cy;
            if (ddx * ddx + ddy * ddy <= carve_r * carve_r) {
                sand_set(&real, x, y, SAND_EMPTY);
            }
        }
    }
    const int carved_empty = empty_within(&real, cx, cy, carve_r);
    for (int refill_step = 0; refill_step < 100; refill_step++) {
        sand_step(&real, 0, 1000, 0);
    }
    const int carved_water_after = water_within(&real, cx, cy, carve_r);

    free(big);
    free(blocks);
    free(impulses);

    TEST_ASSERT_TRUE_MESSAGE(settled,
        "the dune and the pool must both stop moving within the settle "
        "budget before anything is measured against them");
    TEST_ASSERT_GREATER_THAN_MESSAGE(1000, water_before,
        "the pool must actually hold a good depth of water before the "
        "blast touches it, or 'still has water after' proves nothing");
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(MAT_WATER, centre_material_before,
        "the chosen centre must actually be inside the pool, or this "
        "is not testing what it claims to");
    TEST_ASSERT_TRUE_MESSAGE(centre_refilled,
        "the blast's own centre must not be left an empty void once "
        "everything has settled - a liquid closes over a disturbance, "
        "it does not leave a permanent hole in itself");
    TEST_ASSERT_GREATER_THAN_MESSAGE(carved_empty / 2, carved_water_after,
        "a cavity carved into the pool must fill back up with WATER, not "
        "merely with something - this is the claim this test is named for, "
        "and for a long time nothing here checked it: the old assertion "
        "asked only that the blast's centre be non-empty, which held even "
        "with liquids unable to move at all");
    TEST_ASSERT_GREATER_THAN_MESSAGE(water_before / 2, water_after,
        "the pool must still hold most of its own water after settling - "
        "a blast in water should slosh and refill, not boil the whole "
        "pool away");
}

/* The base dune, walled inside a sealed stone vessel with real empty
 * space left OUTSIDE the vessel (not just the grid's own implicit
 * boundary, which is solid for free and would make "contained" trivially
 * true regardless of whether the vessel itself does anything). Detonating
 * inside must leave that outside margin exactly as empty as it started -
 * the inverse of the base scene's own claim, checked at the same real
 * scale rather than the tiny hand-built vessel the mechanism-level tests
 * above already cover.
 *
 * STILL THE "WEAK OR DISTANT" HALF of the two-part guarantee a wall's
 * density-scaled dislodge chance now leaves (see test_a_strong_close_
 * blast_can_breach_a_wall in the mechanism-level section above, and
 * queue_flying_grain()'s own comment in sand.c, for the other half) -
 * DUNE_BLAST_RADIUS against VESSEL_MARGIN's own distance is a genuinely
 * weak comparison at this real scale (a 25-cell blast against a wall
 * `VESSEL_MARGIN` cells away, VESSEL_MARGIN chosen well past that
 * radius), so the annulus here never actually reaches a wall cell to
 * roll against - this measures the common case, a built container
 * still working as a container against an ordinary detonation, not a
 * claim that no wall can ever be breached at any radius or distance. */
#define VESSEL_MARGIN 20
#define VESSEL_WALL   3
static void build_dune_in_a_vessel_scene(sand_t *s)
{
    for (int y = VESSEL_MARGIN; y < REAL_H - VESSEL_MARGIN; y++) {
        for (int x = VESSEL_MARGIN; x < REAL_W - VESSEL_MARGIN; x++) {
            const bool on_wall =
                x < VESSEL_MARGIN + VESSEL_WALL ||
                x >= REAL_W - VESSEL_MARGIN - VESSEL_WALL ||
                y < VESSEL_MARGIN + VESSEL_WALL ||
                y >= REAL_H - VESSEL_MARGIN - VESSEL_WALL;
            if (on_wall) {
                sand_set(s, x, y, CELL_MAKE(MAT_STONE, SAND_AMBIENT_HEAT));
            }
        }
    }

    sand_spawn(s, REAL_W / 2, REAL_H / 4, REAL_W / 5, MAT_SAND);
}

static void test_the_vessel_scene_lets_nothing_reach_outside_it(void)
{
    const size_t cells_len = (size_t)REAL_W * REAL_H;
    uint8_t   *big     = malloc(cells_len);
    uint8_t   *blocks  = malloc(((REAL_W + SAND_BLOCK_W - 1) / SAND_BLOCK_W) *
                                ((REAL_H + SAND_BLOCK_H - 1) / SAND_BLOCK_H));
    impulse_t *impulses = malloc((size_t)DUNE_IMPULSE_MAX * sizeof(impulse_t));
    const bool have_all = (big != NULL && blocks != NULL && impulses != NULL);
    if (!have_all) {
        free(big); free(blocks); free(impulses);
        TEST_FAIL_MESSAGE("need a grid, a block map and an impulse buffer "
                          "for the vessel scene, and at least one failed "
                          "to allocate");
    }

    sand_t real;
    sand_init(&real, big, REAL_W, REAL_H, 71u);
    sand_enable_sleeping(&real, blocks);
    sand_set_scatter(&real, SAND_SCATTER_PER_MATERIAL);
    sand_set_decay(&real, SAND_DECAY_PER_MATERIAL);
    sand_set_mobility(&real, SAND_MOBILITY_PER_MATERIAL);
    sand_enable_impulses(&real, impulses, DUNE_IMPULSE_MAX);

    build_dune_in_a_vessel_scene(&real);
    const bool settled = settle_fully(&real, cells_len);

    /* The dune's own centre, from its SAND footprint specifically - the
     * walls are also "occupied" and would skew a plain min/max scan. */
    int min_x = REAL_W, max_x = -1, min_y = REAL_H, max_y = -1;
    for (int y = 0; y < REAL_H; y++) {
        for (int x = 0; x < REAL_W; x++) {
            if (CELL_MATERIAL(sand_at(&real, x, y)) == MAT_SAND) {
                if (x < min_x) min_x = x;
                if (x > max_x) max_x = x;
                if (y < min_y) min_y = y;
                if (y > max_y) max_y = y;
            }
        }
    }

    const int cx = (min_x + max_x) / 2;
    const int cy = (min_y + max_y) / 2;

    sand_explode(&real, cx, cy, DUNE_BLAST_RADIUS);

    const int max_lifetime = (SAND_EXPLODE_INITIAL_SPEED +
                              SAND_IMPULSE_SPEED_RAMP - 1) /
                             SAND_IMPULSE_SPEED_RAMP;
    for (int i = 0; i < max_lifetime + 20; i++) {
        sand_step(&real, 0, 1000, 0);
    }

    int outside_occupied = 0;
    for (int y = 0; y < REAL_H; y++) {
        for (int x = 0; x < REAL_W; x++) {
            const bool outside_vessel =
                x < VESSEL_MARGIN || x >= REAL_W - VESSEL_MARGIN ||
                y < VESSEL_MARGIN || y >= REAL_H - VESSEL_MARGIN;
            if (outside_vessel && sand_at(&real, x, y) != SAND_EMPTY) {
                outside_occupied++;
            }
        }
    }

    free(big);
    free(blocks);
    free(impulses);

    TEST_ASSERT_TRUE_MESSAGE(settled,
        "the dune inside the vessel must stop moving within the settle "
        "budget before anything is measured against it");
    TEST_ASSERT_GREATER_OR_EQUAL_INT_MESSAGE(0, max_x,
        "the vessel must actually contain a settled dune to detonate, or "
        "this is not testing containment against anything");
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, outside_occupied,
        "at a blast this weak relative to this vessel's own distance, "
        "nothing may occupy the margin outside its walls - this is the "
        "inverse of the base dune scene's own claim, for the common case "
        "a built container is meant to survive; see test_a_strong_close_"
        "blast_can_breach_a_wall for why 'never, at any radius' is no "
        "longer the claim this project makes");
}

/* The base dune, with a strip of wood forming the floor it settles onto -
 * guaranteeing contact between the settled dune and the wood regardless
 * of the exact shape settling leaves, unlike wood planted mid-air before
 * the falling sand has even reached it. Checks the plan's own claim -
 * "no material explodes... [but the trigger is] easier to judge after
 * seeing it than before" - by proving the one direction that already
 * works today: a blast's own fire reaching nearby fuel, exactly as
 * painted fire already would. */
static void build_dune_over_wood_scene(sand_t *s)
{
    sand_spawn(s, REAL_W / 2, REAL_H / 4, REAL_W / 5, MAT_SAND);

    /* CELL_MAKE(MAT_WOOD, 0), not MASS_MAX - wood's own variant is burn
     * life remaining (see cell_is_burning()'s own comment in material.h),
     * not a fill level the way a liquid's is. MASS_MAX there would have
     * planted this floor already on fire, which is what the thermal-
     * shock and lava-stress scenes above deliberately want as their own
     * trigger - this scene wants the opposite: unlit wood, waiting for
     * THIS test's blast to be the first thing that ever lights it. */
    for (int y = REAL_H - 12; y < REAL_H; y++) {
        for (int x = REAL_W / 2 - REAL_W / 5; x < REAL_W / 2 + REAL_W / 5; x++) {
            sand_set(s, x, y, CELL_MAKE(MAT_WOOD, 0));
        }
    }
}

static void test_the_wood_floor_scene_catches_fire(void)
{
    const size_t cells_len = (size_t)REAL_W * REAL_H;
    uint8_t   *big     = malloc(cells_len);
    uint8_t   *blocks  = malloc(((REAL_W + SAND_BLOCK_W - 1) / SAND_BLOCK_W) *
                                ((REAL_H + SAND_BLOCK_H - 1) / SAND_BLOCK_H));
    impulse_t *impulses = malloc((size_t)DUNE_IMPULSE_MAX * sizeof(impulse_t));
    const bool have_all = (big != NULL && blocks != NULL && impulses != NULL);
    if (!have_all) {
        free(big); free(blocks); free(impulses);
        TEST_FAIL_MESSAGE("need a grid, a block map and an impulse buffer "
                          "for the wood floor scene, and at least one "
                          "failed to allocate");
    }

    sand_t real;
    sand_init(&real, big, REAL_W, REAL_H, 83u);
    sand_enable_sleeping(&real, blocks);
    sand_set_scatter(&real, SAND_SCATTER_PER_MATERIAL);
    sand_set_decay(&real, SAND_DECAY_PER_MATERIAL);
    sand_set_mobility(&real, SAND_MOBILITY_PER_MATERIAL);
    sand_enable_impulses(&real, impulses, DUNE_IMPULSE_MAX);

    build_dune_over_wood_scene(&real);
    const bool settled = settle_fully(&real, cells_len);

    int wood_before = 0;
    for (int y = 0; y < REAL_H; y++) {
        for (int x = 0; x < REAL_W; x++) {
            if (CELL_MATERIAL(sand_at(&real, x, y)) == MAT_WOOD) {
                wood_before++;
            }
        }
    }

    /* The CORE's own bottom edge placed right at the wood floor's top
     * surface, not the dune's geometric centre - fire has to actually
     * touch (or nearly touch) the wood to ignite it, and fire is LIGHTER
     * than sand (see SAND_EXPLODE_CORE_DIVISOR's own comment on
     * can_enter()'s displacement rule), so it rises up through the pile
     * rather than sinking down toward a floor beneath it. A centre placed
     * at the dune's own middle - tried first, and measured, not assumed -
     * left the core entirely inside sand, several cells short of the
     * wood, and ignited nothing at all: this is why "detonated somewhere
     * in the dune" is not the same claim as "detonated where its fire
     * can actually reach the fuel". */
    const int cx = REAL_W / 2;
    const int cy = (REAL_H - 12) - (DUNE_BLAST_RADIUS / SAND_EXPLODE_CORE_DIVISOR) - 1;

    sand_explode(&real, cx, cy, DUNE_BLAST_RADIUS);

    const int max_lifetime = (SAND_EXPLODE_INITIAL_SPEED +
                              SAND_IMPULSE_SPEED_RAMP - 1) /
                             SAND_IMPULSE_SPEED_RAMP;
    for (int i = 0; i < max_lifetime + 20; i++) {
        sand_step(&real, 0, 1000, 0);
    }

    int burning_wood = 0;
    for (int y = 0; y < REAL_H; y++) {
        for (int x = 0; x < REAL_W; x++) {
            const cell_t c = sand_at(&real, x, y);
            if (CELL_MATERIAL(c) == MAT_WOOD && cell_is_burning(c)) {
                burning_wood++;
            }
        }
    }

    free(big);
    free(blocks);
    free(impulses);

    TEST_ASSERT_TRUE_MESSAGE(settled,
        "the dune over its wood floor must stop moving within the "
        "settle budget before anything is measured against it");
    TEST_ASSERT_GREATER_THAN_MESSAGE(0, wood_before,
        "the wood floor must have survived settling - if sand displaced "
        "all of it before the blast even happens, this proves nothing");
    TEST_ASSERT_GREATER_THAN_MESSAGE(0, burning_wood,
        "a blast detonated against a wood floor must leave at least "
        "some of it burning - the core's own fire reaching nearby fuel "
        "exactly as painted fire already would, not a special case a "
        "blast needs of its own");
}

/* The base dune, poured in three bands of decreasing radius with real
 * settling time between each - not one uniform pour - so pour_phase (see
 * its own comment on sand_t in sand.h) has genuinely moved on between
 * bands and each one settles with a visibly different shade, the same
 * way two pours a few seconds apart on the real app would. A wedding-
 * cake dune with real, distinguishable layers, not a paint job.
 *
 * 40 steps between pours, not a much longer rest - measured, not
 * guessed: at 150 steps the dune had that much longer for scatter to
 * random-walk its base sideways between every pour, and by the time all
 * three had landed the footprint had spread across 86% of the grid's own
 * width - so wide that DUNE_BLAST_RADIUS's own disc around the centre
 * never reached any genuinely empty ground to throw material into, and
 * the guard test below measured zero grains outside the footprint,
 * every time. 40 steps is enough for pour_phase to still land each band
 * on a visibly different shade (5 distinct shades in the settled dune,
 * against 150 steps' own 7 - plenty either way) while keeping the dune
 * itself narrow enough for its own blast radius to still reach past its
 * edge. */
static void build_layered_dune_scene(sand_t *s)
{
    sand_spawn(s, REAL_W / 2, REAL_H / 4, REAL_W / 5, MAT_SAND);
    for (int i = 0; i < 40; i++) {
        sand_step(s, 0, 1000, 0);
    }
    sand_spawn(s, REAL_W / 2, REAL_H / 4, (REAL_W / 5) * 2 / 3, MAT_SAND);
    for (int i = 0; i < 40; i++) {
        sand_step(s, 0, 1000, 0);
    }
    sand_spawn(s, REAL_W / 2, REAL_H / 4, (REAL_W / 5) / 3, MAT_SAND);
}

/* Displaced layers, not just displaced sand - the base scene above
 * already proves grains escape the footprint at all; this proves the
 * blast reaches deep enough to mix bands that would otherwise never
 * meet, which is what "throw is visible as displaced layers" actually
 * means on the panel. Counted by distinct shade (CELL_VARIANT), not by
 * tracking any one band's own identity: three pours spaced by real
 * settling time land in different parts of MATERIAL_VARIANTS' shade
 * range (see random_cell()'s own use of pour_phase), so more than one
 * distinct shade appearing outside the original footprint is direct
 * evidence that more than one band contributed to what escaped, not
 * just the most recent, surface-most pour skimming off the top. */
static void test_the_layered_dune_scene_throws_more_than_one_band(void)
{
    const size_t cells_len = (size_t)REAL_W * REAL_H;
    uint8_t   *big      = malloc(cells_len);
    uint8_t   *blocks   = malloc(((REAL_W + SAND_BLOCK_W - 1) / SAND_BLOCK_W) *
                                 ((REAL_H + SAND_BLOCK_H - 1) / SAND_BLOCK_H));
    uint8_t   *footprint = malloc(DUNE_FOOTPRINT_BYTES);
    impulse_t *impulses  = malloc((size_t)DUNE_IMPULSE_MAX * sizeof(impulse_t));
    const bool have_all = (big != NULL && blocks != NULL &&
                          footprint != NULL && impulses != NULL);
    if (!have_all) {
        free(big); free(blocks); free(footprint); free(impulses);
        TEST_FAIL_MESSAGE("need a grid, a block map, a one-bit-per-cell "
                          "footprint mask and an impulse buffer for the "
                          "layered dune scene, and at least one failed "
                          "to allocate");
    }
    memset(footprint, 0, DUNE_FOOTPRINT_BYTES);

    sand_t real;
    sand_init(&real, big, REAL_W, REAL_H, 97u);
    sand_enable_sleeping(&real, blocks);
    sand_set_scatter(&real, SAND_SCATTER_PER_MATERIAL);
    sand_set_decay(&real, SAND_DECAY_PER_MATERIAL);
    sand_set_mobility(&real, SAND_MOBILITY_PER_MATERIAL);
    sand_enable_impulses(&real, impulses, DUNE_IMPULSE_MAX);

    build_layered_dune_scene(&real);
    const bool settled = settle_fully(&real, cells_len);

    int min_x = REAL_W, max_x = -1, min_y = REAL_H, max_y = -1;
    bool seen_variant_before[SAND_SHADE_COUNT] = { false };
    for (int y = 0; y < REAL_H; y++) {
        for (int x = 0; x < REAL_W; x++) {
            const cell_t c = sand_at(&real, x, y);
            const bool occupied = c != SAND_EMPTY;
            if (occupied) {
                footprint_set(footprint, (size_t)y * REAL_W + x);
                if (x < min_x) min_x = x;
                if (x > max_x) max_x = x;
                if (y < min_y) min_y = y;
                if (y > max_y) max_y = y;
                if (CELL_MATERIAL(c) == MAT_SAND) {
                    seen_variant_before[CELL_VARIANT(c)] = true;
                }
            }
        }
    }
    int distinct_bands = 0;
    for (int v = 0; v < SAND_SHADE_COUNT; v++) {
        if (seen_variant_before[v]) distinct_bands++;
    }

    const int cx = (min_x + max_x) / 2;
    const int cy = (min_y + max_y) / 2;

    sand_explode(&real, cx, cy, DUNE_BLAST_RADIUS);

    const int max_lifetime = (SAND_EXPLODE_INITIAL_SPEED +
                              SAND_IMPULSE_SPEED_RAMP - 1) /
                             SAND_IMPULSE_SPEED_RAMP;
    for (int i = 0; i < max_lifetime + 20; i++) {
        sand_step(&real, 0, 1000, 0);
    }

    bool seen_variant_outside[SAND_SHADE_COUNT] = { false };
    for (int y = 0; y < REAL_H; y++) {
        for (int x = 0; x < REAL_W; x++) {
            if (footprint_get(footprint, (size_t)y * REAL_W + x)) {
                continue;
            }
            const cell_t c = sand_at(&real, x, y);
            if (CELL_MATERIAL(c) == MAT_SAND) {
                seen_variant_outside[CELL_VARIANT(c)] = true;
            }
        }
    }
    int distinct_bands_outside = 0;
    for (int v = 0; v < SAND_SHADE_COUNT; v++) {
        if (seen_variant_outside[v]) distinct_bands_outside++;
    }

    free(big);
    free(blocks);
    free(footprint);
    free(impulses);

    TEST_ASSERT_TRUE_MESSAGE(settled,
        "the layered dune must stop moving within the settle budget "
        "before anything is measured against it");
    TEST_ASSERT_GREATER_THAN_MESSAGE(1, distinct_bands,
        "three pours spaced by real settling time must have left more "
        "than one distinct shade in the settled dune - if they did not, "
        "the bands never separated and this scene is not testing what "
        "it claims to");
    TEST_ASSERT_GREATER_THAN_MESSAGE(1, distinct_bands_outside,
        "more than one shade band must appear outside the original "
        "footprint - a single band escaping would just be the base "
        "scene's own claim again, not displaced LAYERS specifically");
}

void run_sand_dune_blast_suite(void)
{
    RUN_TEST(test_the_sand_dune_scene_throws_grains_beyond_its_own_footprint);
    RUN_TEST(test_the_water_pool_scene_refills_its_own_cavity);
    RUN_TEST(test_the_vessel_scene_lets_nothing_reach_outside_it);
    RUN_TEST(test_the_wood_floor_scene_catches_fire);
    RUN_TEST(test_the_layered_dune_scene_throws_more_than_one_band);
}

SUITE_REGISTER(run_sand_dune_blast_suite);
