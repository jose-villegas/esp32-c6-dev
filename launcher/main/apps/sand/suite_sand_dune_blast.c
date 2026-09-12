/*
 * Portable suite: the falling-sand automaton - dune blast scenes and their
 * variants - water pool, vessel, wood floor, layered dune.
 *
 * Split out of suite_sand.c (bd esp32c6 test-suite-refactor), which had grown
 * past 32,000 lines across 500+ tests. Shared fixtures and assertion helpers
 * live in suite_sand_common.{c,h} - see that header.
 */
#include <math.h> /* not every file in the split still needs atan2()/M_PI,
                     * but every file inherited suite_sand.c's own include
                     * block rather than being pruned by hand, to keep the
                     * split itself mechanical and low-risk */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef M_PI
/* Not every libc defines this in <math.h> without a feature-test macro this
 * file has no other reason to set (MinGW's, notably, on the host build) -
 * cheaper to supply it directly than to widen this file's own feature-test
 * exposure for one constant. */
#define M_PI 3.14159265358979323846
#endif

#include "suites.h"
#include "unity.h"

#include "sand.h"
#include "sand_priv.h"
#include "suite_sand_common.h"
#include "util/intmath.h"

/* BLAST SCENES - a settled dune and a detonation at its centre.
 *
 * Every blast test above this line checks an internal detail of the
 * mechanism, and every one of them passed while a real detonation on a
 * real device only disturbed the top tenth of its own disc. Internal
 * correctness is not proof that a blast LOOKS like a blast, so these
 * measure the outcome instead: does material end up outside where it
 * started, how far, and how much of it is gone rather than moved. */

/* How many steps make one "has anything changed" batch, and how many
 * batches settle_fully() below will spend looking for an unchanged one
 * before giving up. 20 steps a batch keeps the memcmp cost - one pass
 * over the whole grid - proportionate to the stepping it is checking; 300
 * batches (6,000 steps) is a safety rail against an actual bug turning
 * this into an infinite loop, not a number any real dune has come close
 * to needing. */
#define DUNE_SETTLE_BATCH_STEPS 20
#define DUNE_SETTLE_MAX_BATCHES 300

/* A 64-bit FNV-1a fold of the whole grid, folded before and after a batch
 * of steps to answer "did anything change" without a second copy of the
 * grid to memcmp against. That copy would be another 41,216 bytes beside
 * `big`'s own, against the roughly 66,632 this device has free once the
 * display framebuffer is carved out of the heap. A collision is possible
 * in principle, and accepted. */
static uint64_t
grid_checksum(const uint8_t* cells, size_t len) {
    uint64_t h = 0xcbf29ce484222325ULL; /* FNV-1a 64-bit offset basis */
    for (size_t i = 0; i < len; i++) {
        h ^= cells[i];
        h *= 0x100000001b3ULL; /* FNV-1a 64-bit prime */
    }
    return h;
}

/* A fixed step count could only guess how long a pile this size takes to
 * stop moving, and a guess that undershoots confuses the blast's own
 * throw with gravity still finishing its job. The caller must assert on
 * the returned convergence: a dune still settling is not the scene the
 * rest of the test thinks it is. */
static bool
settle_fully(sand_t* s, size_t cells_len) {
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

/* The settled-footprint mask, one bit per cell: a bool[] on this grid
 * would cost 41,216 bytes, the bitset 5,152. It stays separate from the
 * thermal shock scene's own mask, which answers an unrelated question and
 * has no reason to share storage or a lifetime with it. */
#define DUNE_FOOTPRINT_BYTES (((size_t)REAL_W * (size_t)REAL_H + 7) / 8)

static inline bool
footprint_get(const uint8_t* mask, size_t idx) {
    return (mask[idx >> 3] >> (idx & 7)) & 1u;
}

static inline void
footprint_set(uint8_t* mask, size_t idx) {
    mask[idx >> 3] |= (uint8_t)(1u << (idx & 7));
}

/* Distance past the dune's own edge, not from the detonation centre:
 * searches outward in expanding Chebyshev rings against `footprint`
 * rather than flood-filling all 41,216 grid cells when only ~105 queries
 * ever happen (each resolves within two or three rings, measured; no
 * storage beyond locals). `cap` bounds the search to the largest
 * possible Chebyshev distance on this grid, so it only bounds the
 * search, never affects correctness. */
static int
nearest_footprint_distance(const uint8_t* footprint, int w, int h, int x, int y, int cap) {
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
            if (y0 >= 0 && footprint_get(footprint, (size_t)y0 * (size_t)w + (size_t)xx)) {
                return r;
            }
            if (y1 < h && footprint_get(footprint, (size_t)y1 * (size_t)w + (size_t)xx)) {
                return r;
            }
        }
        for (int yy = y0 + 1; yy <= y1 - 1; yy++) {
            if (yy < 0 || yy >= h) {
                continue;
            }
            if (x0 >= 0 && footprint_get(footprint, (size_t)yy * (size_t)w + (size_t)x0)) {
                return r;
            }
            if (x1 < w && footprint_get(footprint, (size_t)yy * (size_t)w + (size_t)x1)) {
                return r;
            }
        }
    }
    return cap + 1; /* not found within cap - see this function's own comment */
}

/* The largest Chebyshev distance any two cells on this grid could ever
 * have - see nearest_footprint_distance()'s own comment for why this is
 * the cap it is called with, and why that makes the cap a search bound
 * rather than a correctness one. */
#define NEAREST_FOOTPRINT_CAP ((REAL_W > REAL_H ? REAL_W : REAL_H) - 1)

/* Mirrors app_sand.c's DETONATE_RADIUS_PX at the same CELL_MIN scale
 * REAL_W/REAL_H represent, so a sweep reads on the real device's own
 * scale. APP_IMPULSE_MAX is a fixed entry count sized from the device's
 * largest-contiguous-free-block budget, decoupled from this radius;
 * sand_explode() thins its seeding density automatically, evenly across
 * the disc, whenever it exceeds the buffer given, rather than truncating
 * the shape. See DETONATE_RADIUS_PX's comment in app_sand.c for why 25
 * cells. */
#define DUNE_BLAST_RADIUS     25

/* A FIXED ENTRY COUNT MIRRORING APP_IMPULSE_MAX, not a formula in
 * DUNE_BLAST_RADIUS - see APP_IMPULSE_MAX's own comment in app_sand.c for
 * why the two split apart: sand_explode() degrades its seeding density
 * to fit whatever buffer it is given, so this buffer need not scale with
 * the disc DUNE_BLAST_RADIUS implies. It DOES need to mirror the app's
 * real device budget, not its radius - a differently-sized buffer would
 * measure a blast against a different memory ceiling than the device
 * actually has. */
#define DUNE_IMPULSE_MAX      2048

/* Poured rather than painted: a painted rectangle is not a dune - no
 * slope for a blast to disturb, and square corners that would slide under
 * plain gravity before the explosion got a turn, muddying "the blast
 * displaced this" with "gravity was already going to". Settling is the
 * caller's job, as with every other builder here, so a caller wanting a
 * MID-fall dune can still use this one. */
static void
build_sand_dune_scene(sand_t* s) {
    sand_spawn(s, REAL_W / 2, REAL_H / 4, REAL_W / 5, MAT_SAND);
}

/* Three numbers - escaped grains, furthest throw, material destroyed -
 * rather than a boolean, which cannot tell power from reach from
 * destruction apart, and conflating them is how a change that helps one
 * and hurts another goes unnoticed. "Outside" is measured against the
 * settled footprint, recorded once before sand_explode() is called.
 * Checking for MAT_SAND excludes the fire the core itself becomes: fire
 * landing outside the footprint is the fireball's own edge, not a grain
 * flying off. */
static void
test_the_sand_dune_scene_throws_grains_beyond_its_own_footprint(void) {
    const size_t cells_len = (size_t)REAL_W * REAL_H;
    uint8_t* big = malloc(cells_len);
    uint8_t* blocks =
        malloc(((REAL_W + SAND_BLOCK_W - 1) / SAND_BLOCK_W) * ((REAL_H + SAND_BLOCK_H - 1) / SAND_BLOCK_H));
    uint8_t* footprint = malloc(DUNE_FOOTPRINT_BYTES);
    impulse_t* impulses = malloc((size_t)DUNE_IMPULSE_MAX * sizeof(impulse_t));
    const bool have_all = (big != NULL && blocks != NULL && footprint != NULL && impulses != NULL);
    if (!have_all) {
        free(big);
        free(blocks);
        free(footprint);
        free(impulses);
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
                if (x < min_x) {
                    min_x = x;
                }
                if (x > max_x) {
                    max_x = x;
                }
                if (y < min_y) {
                    min_y = y;
                }
                if (y > max_y) {
                    max_y = y;
                }
            }
        }
    }

    const int cx = (min_x + max_x) / 2;
    const int cy = (min_y + max_y) / 2;

    /* A settled dune's bounding-box centre sits only ~30 cells above the
     * true floor, so at DUNE_BLAST_RADIUS the blast does not reliably
     * reach the grid's bottom edge - and does not need to: full-density
     * seeding at this radius is what the scene measures, not edge
     * contact. An 800-seed sweep against the shipped sand_explode()
     * averaged 2.63% of the dune outside and 1.94% destroyed, small
     * fractions rather than a degenerate scene. */
    sand_explode(&real, cx, cy, DUNE_BLAST_RADIUS);

    /* Past the deterministic flight-time bound (see SAND_IMPULSE_SPEED_
     * RAMP's own comment in sand.h), computed from the constants rather
     * than a bare number so this keeps measuring the same thing after
     * either one is retuned, plus margin for gravity to bring a landed
     * grain to rest and for a water/collapse scene's own refill to
     * finish. */
    const int max_lifetime = (SAND_EXPLODE_INITIAL_SPEED + SAND_IMPULSE_SPEED_RAMP - 1) / SAND_IMPULSE_SPEED_RAMP;
    for (int i = 0; i < max_lifetime + 20; i++) {
        sand_step(&real, 0, 1000, 0);
    }

    /* Distance to the NEAREST footprint cell, not to the detonation
     * centre: from one fixed interior point, a grain genuinely thrown
     * clear and one that merely slid down the dune's own slope both read
     * as far. */
    int outside = 0;
    int max_throw = 0;
    for (int y = 0; y < REAL_H; y++) {
        for (int x = 0; x < REAL_W; x++) {
            if (footprint_get(footprint, (size_t)y * REAL_W + x)) {
                continue; /* inside the original dune - not an escape */
            }
            if (CELL_MATERIAL(sand_at(&real, x, y)) != MAT_SAND) {
                continue; /* fire, not a grain - see this test's own comment */
            }
            outside++;
            const int d = nearest_footprint_distance(footprint, REAL_W, REAL_H, x, y, NEAREST_FOOTPRINT_CAP);
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

    TEST_ASSERT_TRUE_MESSAGE(settled, "the dune must actually stop moving within the settle budget - a "
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
static void
build_dune_beside_water_scene(sand_t* s) {
    sand_spawn(s, REAL_W / 2, REAL_H / 4, REAL_W / 5, MAT_SAND);

    /* Laid down at roughly the depth this volume settles to anyway,
     * spread across the basin, rather than stacked in one third with an
     * open face: that shape costs 2,480 settle steps (124 batches, ~98%
     * of this test's runtime) against 29-39 for every other scene here,
     * just to reach the same equilibrium sideways. Same water, same
     * basin, same claims - it simply starts where it was always going to
     * end up. */
    const int pool_depth = 38;
    for (int y = REAL_H - pool_depth; y < REAL_H; y++) {
        for (int x = 0; x < REAL_W; x++) {
            sand_set(s, x, y, CELL_MAKE(MAT_WATER, MASS_MAX));
        }
    }
}

/* The one place in this file checking Impulse-Mechanics.md's device
 * checklist claim - "detonate in water: the cavity should collapse and
 * slosh" - at more than a hand-wave.
 *
 * The refill claim below needs the cavity counted BY MATERIAL: it gets
 * filled by falling sand or by impulse-thrown debris whether or not the
 * liquid can flow at all, so "something is there now" is not evidence of
 * anything. */
static int
water_within(const sand_t* s, int cx, int cy, int r) {
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

static int
empty_within(const sand_t* s, int cx, int cy, int r) {
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

static void
test_the_water_pool_scene_refills_its_own_cavity(void) {
    const size_t cells_len = (size_t)REAL_W * REAL_H;
    uint8_t* big = malloc(cells_len);
    uint8_t* blocks =
        malloc(((REAL_W + SAND_BLOCK_W - 1) / SAND_BLOCK_W) * ((REAL_H + SAND_BLOCK_H - 1) / SAND_BLOCK_H));
    impulse_t* impulses = malloc((size_t)DUNE_IMPULSE_MAX * sizeof(impulse_t));
    const bool have_all = (big != NULL && blocks != NULL && impulses != NULL);
    if (!have_all) {
        free(big);
        free(blocks);
        free(impulses);
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

    /* FOUND, not hardcoded. A fixed row only lands inside the pool for
     * one particular water level, so it becomes a silent precondition on
     * where the pool happened to settle, and any unrelated change to how
     * water comes to rest then fails this test for an unrelated reason. */
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

    const int max_lifetime = (SAND_EXPLODE_INITIAL_SPEED + SAND_IMPULSE_SPEED_RAMP - 1) / SAND_IMPULSE_SPEED_RAMP;
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

    /* Asking only that the blast's own centre be non-empty cannot fail:
     * an impulse SWAPS two occupied cells, so a packed region never opens
     * a hole - with move_liquid_grain() stubbed to return false, that
     * check still passed. Hence a directly carved cavity, and an
     * assertion for WATER back: of 113 carved cells, 89 refill normally
     * against 32 with liquids immobile, putting the half-count bar
     * between them. */
    const int carve_r = 6;
    for (int y = cy - carve_r; y <= cy + carve_r; y++) {
        for (int x = cx - carve_r; x <= cx + carve_r; x++) {
            if ((unsigned)x >= (unsigned)REAL_W || (unsigned)y >= (unsigned)REAL_H) {
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

    TEST_ASSERT_TRUE_MESSAGE(settled, "the dune and the pool must both stop moving within the settle "
                                      "budget before anything is measured against them");
    TEST_ASSERT_GREATER_THAN_MESSAGE(1000, water_before,
                                     "the pool must actually hold a good depth of water before the "
                                     "blast touches it, or 'still has water after' proves nothing");
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(MAT_WATER, centre_material_before,
                                    "the chosen centre must actually be inside the pool, or this "
                                    "is not testing what it claims to");
    TEST_ASSERT_TRUE_MESSAGE(centre_refilled, "the blast's own centre must not be left an empty void once "
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

/* Real empty space is left OUTSIDE the vessel: the grid's own boundary is
 * solid for free and would make "contained" trivially true whatever the
 * vessel does.
 *
 * The WEAK OR DISTANT half of the two-part guarantee a wall's
 * density-scaled dislodge chance leaves - VESSEL_MARGIN sits well past
 * DUNE_BLAST_RADIUS, so the annulus never reaches a wall cell to roll
 * against. This is the common case, not a claim that no wall can ever be
 * breached. */
#define VESSEL_MARGIN 20
#define VESSEL_WALL   3

static void
build_dune_in_a_vessel_scene(sand_t* s) {
    for (int y = VESSEL_MARGIN; y < REAL_H - VESSEL_MARGIN; y++) {
        for (int x = VESSEL_MARGIN; x < REAL_W - VESSEL_MARGIN; x++) {
            const bool on_wall = x < VESSEL_MARGIN + VESSEL_WALL || x >= REAL_W - VESSEL_MARGIN - VESSEL_WALL
                                 || y < VESSEL_MARGIN + VESSEL_WALL || y >= REAL_H - VESSEL_MARGIN - VESSEL_WALL;
            if (on_wall) {
                sand_set(s, x, y, CELL_MAKE(MAT_STONE, SAND_AMBIENT_HEAT));
            }
        }
    }

    sand_spawn(s, REAL_W / 2, REAL_H / 4, REAL_W / 5, MAT_SAND);
}

static void
test_the_vessel_scene_lets_nothing_reach_outside_it(void) {
    const size_t cells_len = (size_t)REAL_W * REAL_H;
    uint8_t* big = malloc(cells_len);
    uint8_t* blocks =
        malloc(((REAL_W + SAND_BLOCK_W - 1) / SAND_BLOCK_W) * ((REAL_H + SAND_BLOCK_H - 1) / SAND_BLOCK_H));
    impulse_t* impulses = malloc((size_t)DUNE_IMPULSE_MAX * sizeof(impulse_t));
    const bool have_all = (big != NULL && blocks != NULL && impulses != NULL);
    if (!have_all) {
        free(big);
        free(blocks);
        free(impulses);
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
                if (x < min_x) {
                    min_x = x;
                }
                if (x > max_x) {
                    max_x = x;
                }
                if (y < min_y) {
                    min_y = y;
                }
                if (y > max_y) {
                    max_y = y;
                }
            }
        }
    }

    const int cx = (min_x + max_x) / 2;
    const int cy = (min_y + max_y) / 2;

    sand_explode(&real, cx, cy, DUNE_BLAST_RADIUS);

    const int max_lifetime = (SAND_EXPLODE_INITIAL_SPEED + SAND_IMPULSE_SPEED_RAMP - 1) / SAND_IMPULSE_SPEED_RAMP;
    for (int i = 0; i < max_lifetime + 20; i++) {
        sand_step(&real, 0, 1000, 0);
    }

    int outside_occupied = 0;
    for (int y = 0; y < REAL_H; y++) {
        for (int x = 0; x < REAL_W; x++) {
            const bool outside_vessel =
                x < VESSEL_MARGIN || x >= REAL_W - VESSEL_MARGIN || y < VESSEL_MARGIN || y >= REAL_H - VESSEL_MARGIN;
            if (outside_vessel && sand_at(&real, x, y) != SAND_EMPTY) {
                outside_occupied++;
            }
        }
    }

    free(big);
    free(blocks);
    free(impulses);

    TEST_ASSERT_TRUE_MESSAGE(settled, "the dune inside the vessel must stop moving within the settle "
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

/* The wood is the floor the dune settles onto, which guarantees contact
 * whatever shape settling leaves - unlike wood planted mid-air before the
 * falling sand has reached it. What is proved is the direction that works
 * today: a blast's own fire reaching nearby fuel, exactly as painted fire
 * already would. */
static void
build_dune_over_wood_scene(sand_t* s) {
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

/* The CORE's bottom edge sits at the wood floor's top surface, not the
 * dune's geometric centre: fire must actually touch the wood to ignite it,
 * and fire is LIGHTER than sand (SAND_EXPLODE_CORE_DIVISOR's comment on
 * can_enter()'s displacement rule), so it rises through the pile rather than
 * sinking to a floor beneath it. A centre at the dune's middle leaves the
 * core entirely inside sand, short of the wood, igniting nothing. */
static int
dune_over_wood_burning(uint32_t seed, bool* settled_out, int* wood_before_out) {
    const size_t cells_len = (size_t)REAL_W * REAL_H;
    uint8_t* big = malloc(cells_len);
    uint8_t* blocks =
        malloc(((REAL_W + SAND_BLOCK_W - 1) / SAND_BLOCK_W) * ((REAL_H + SAND_BLOCK_H - 1) / SAND_BLOCK_H));
    impulse_t* impulses = malloc((size_t)DUNE_IMPULSE_MAX * sizeof(impulse_t));
    const bool have_all = (big != NULL && blocks != NULL && impulses != NULL);
    if (!have_all) {
        free(big);
        free(blocks);
        free(impulses);
        TEST_FAIL_MESSAGE("need a grid, a block map and an impulse buffer "
                          "for the wood floor scene, and at least one "
                          "failed to allocate");
    }

    sand_t real;
    sand_init(&real, big, REAL_W, REAL_H, seed);
    sand_enable_sleeping(&real, blocks);
    sand_set_scatter(&real, SAND_SCATTER_PER_MATERIAL);
    sand_set_decay(&real, SAND_DECAY_PER_MATERIAL);
    sand_set_mobility(&real, SAND_MOBILITY_PER_MATERIAL);
    sand_enable_impulses(&real, impulses, DUNE_IMPULSE_MAX);

    build_dune_over_wood_scene(&real);
    *settled_out = settle_fully(&real, cells_len);

    *wood_before_out = 0;
    for (int y = 0; y < REAL_H; y++) {
        for (int x = 0; x < REAL_W; x++) {
            if (CELL_MATERIAL(sand_at(&real, x, y)) == MAT_WOOD) {
                (*wood_before_out)++;
            }
        }
    }

    const int cx = REAL_W / 2;
    const int cy = (REAL_H - 12) - (DUNE_BLAST_RADIUS / SAND_EXPLODE_CORE_DIVISOR) - 1;

    sand_explode(&real, cx, cy, DUNE_BLAST_RADIUS);

    const int max_lifetime = (SAND_EXPLODE_INITIAL_SPEED + SAND_IMPULSE_SPEED_RAMP - 1) / SAND_IMPULSE_SPEED_RAMP;
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
    return burning_wood;
}

/* SEVERAL BOARDS, NOT ONE: ignition here is sampled, not a law - whether any
 * of a blast's ~70 fire cells lands against the floor before burning out is
 * decided by sweep order. Over 30 seeds, 24 boards light at block 32x64 and
 * 23 at 16x32, landing opposite ways on seed 83 alone. A broken ignition
 * path takes every board to zero, which this still catches. */
#define DUNE_WOOD_SEEDS {83u, 85u, 87u, 89u}

static void
test_the_wood_floor_scene_catches_fire(void) {
    const uint32_t seeds[] = DUNE_WOOD_SEEDS;
    const int n = (int)(sizeof seeds / sizeof seeds[0]);
    int lit_boards = 0;
    int total_burning = 0;
    char why[280];

    for (int i = 0; i < n; i++) {
        bool settled = false;
        int wood_before = 0;
        const int burning = dune_over_wood_burning(seeds[i], &settled, &wood_before);

        snprintf(why, sizeof why,
                 "the dune over its wood floor must stop moving within the "
                 "settle budget before anything is measured against it - "
                 "seed %u",
                 (unsigned)seeds[i]);
        TEST_ASSERT_TRUE_MESSAGE(settled, why);

        snprintf(why, sizeof why,
                 "the wood floor must have survived settling - if sand "
                 "displaced all of it before the blast even happens, this "
                 "proves nothing - seed %u, %d wood",
                 (unsigned)seeds[i], wood_before);
        TEST_ASSERT_GREATER_THAN_MESSAGE(0, wood_before, why);

        total_burning += burning;
        if (burning > 0) {
            lit_boards++;
        }
    }

    snprintf(why, sizeof why,
             "a blast detonated against a wood floor must leave some of it "
             "burning on at least one of %d boards - the core's own fire "
             "reaching nearby fuel exactly as painted fire already would, "
             "not a special case a blast needs of its own - %d lit, %d "
             "cells",
             n, lit_boards, total_burning);
    TEST_ASSERT_GREATER_THAN_MESSAGE(0, lit_boards, why);
}

/* Three bands of decreasing radius with real settling time between them,
 * so pour_phase moves on and each band lands on a visibly different
 * shade.
 *
 * 40 steps between pours rather than longer, measured: at 150 steps
 * scatter random-walks the base sideways until the footprint spans 86% of
 * the grid's width, and DUNE_BLAST_RADIUS's disc then never reaches empty
 * ground - the guard test below measured zero grains outside it, every
 * time. 40 steps still leaves 5 distinct shades against 150's 7. */
static void
build_layered_dune_scene(sand_t* s) {
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

/* The base scene above already proves grains escape the footprint; this
 * proves the blast reaches deep enough to mix bands that would otherwise
 * never meet. Counted by distinct shade (CELL_VARIANT), since pours
 * spaced by real settling time land in different parts of the shade
 * range: more than one shade outside the footprint is evidence that more
 * than one band contributed, not just the surface-most pour skimming
 * off. */
static void
test_the_layered_dune_scene_throws_more_than_one_band(void) {
    const size_t cells_len = (size_t)REAL_W * REAL_H;
    uint8_t* big = malloc(cells_len);
    uint8_t* blocks =
        malloc(((REAL_W + SAND_BLOCK_W - 1) / SAND_BLOCK_W) * ((REAL_H + SAND_BLOCK_H - 1) / SAND_BLOCK_H));
    uint8_t* footprint = malloc(DUNE_FOOTPRINT_BYTES);
    impulse_t* impulses = malloc((size_t)DUNE_IMPULSE_MAX * sizeof(impulse_t));
    const bool have_all = (big != NULL && blocks != NULL && footprint != NULL && impulses != NULL);
    if (!have_all) {
        free(big);
        free(blocks);
        free(footprint);
        free(impulses);
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
    bool seen_variant_before[SAND_SHADE_COUNT] = {false};
    for (int y = 0; y < REAL_H; y++) {
        for (int x = 0; x < REAL_W; x++) {
            const cell_t c = sand_at(&real, x, y);
            const bool occupied = c != SAND_EMPTY;
            if (occupied) {
                footprint_set(footprint, (size_t)y * REAL_W + x);
                if (x < min_x) {
                    min_x = x;
                }
                if (x > max_x) {
                    max_x = x;
                }
                if (y < min_y) {
                    min_y = y;
                }
                if (y > max_y) {
                    max_y = y;
                }
                if (CELL_MATERIAL(c) == MAT_SAND) {
                    seen_variant_before[CELL_VARIANT(c)] = true;
                }
            }
        }
    }
    int distinct_bands = 0;
    for (int v = 0; v < SAND_SHADE_COUNT; v++) {
        if (seen_variant_before[v]) {
            distinct_bands++;
        }
    }

    const int cx = (min_x + max_x) / 2;
    const int cy = (min_y + max_y) / 2;

    sand_explode(&real, cx, cy, DUNE_BLAST_RADIUS);

    const int max_lifetime = (SAND_EXPLODE_INITIAL_SPEED + SAND_IMPULSE_SPEED_RAMP - 1) / SAND_IMPULSE_SPEED_RAMP;
    for (int i = 0; i < max_lifetime + 20; i++) {
        sand_step(&real, 0, 1000, 0);
    }

    bool seen_variant_outside[SAND_SHADE_COUNT] = {false};
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
        if (seen_variant_outside[v]) {
            distinct_bands_outside++;
        }
    }

    free(big);
    free(blocks);
    free(footprint);
    free(impulses);

    TEST_ASSERT_TRUE_MESSAGE(settled, "the layered dune must stop moving within the settle budget "
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

void
run_sand_dune_blast_suite(void) {
    RUN_TEST(test_the_sand_dune_scene_throws_grains_beyond_its_own_footprint);
    RUN_TEST(test_the_water_pool_scene_refills_its_own_cavity);
    RUN_TEST(test_the_vessel_scene_lets_nothing_reach_outside_it);
    RUN_TEST(test_the_wood_floor_scene_catches_fire);
    RUN_TEST(test_the_layered_dune_scene_throws_more_than_one_band);
}

SUITE_REGISTER(run_sand_dune_blast_suite);
