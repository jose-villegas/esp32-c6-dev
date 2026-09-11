/*
 * Portable suite: the falling-sand automaton - explosions, impulse rungs, and
 * free-fall shaking.
 *
 * Split out of suite_sand.c (bd esp32c6 test-suite-refactor), which had grown
 * past 32,000 lines across 500+ tests. Shared fixtures and assertion helpers
 * live in suite_sand_common.{c,h} - see that header.
 */
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

/* --- explosions -----------------------------------------------------------
 *
 * sand_explode() throws grains outward one cell per step, in a bounded
 * transient list rather than a per-cell velocity field - see
 * docs/sand/Impulse-Mechanics.md, which this whole section implements.
 */

/* One entry per cell of the 8x8 fixture grid - big enough that no test below
 * needs to think about the cap, except the one written specifically to
 * exercise it (which uses its own, deliberately tiny buffer instead). */
static impulse_t impulse_buf[W * H];

/* CHECKS DISC-COUNT TABLE DIFFERENTLY THAN GENERATION ALGORITHM. COVERS
 * OUT-OF-RANGE RADII. */
static void test_the_disc_count_table_matches_a_direct_lattice_count(void)
{
    int first_bad = -1;
    int expected_there = 0, got_there = 0;

    for (int r = 0; r <= 40; r++) {
        int expect = 0;
        for (int dy = -r; dy <= r; dy++) {
            for (int dx = -r; dx <= r; dx++) {
                if (dx * dx + dy * dy <= r * r) {
                    expect++;
                }
            }
        }
        const int got = sand_disc_count(r);
        if (got != expect && first_bad < 0) {
            first_bad = r;
            expected_there = expect;
            got_there = got;
        }
    }

    /* Reported as the RADIUS, not the count - "Expected -1 Was 17" names the
     * entry to go and look at, which a mismatched cell total would not. */
    TEST_ASSERT_EQUAL_INT_MESSAGE(-1, first_bad,
        "sand_disc_count() disagrees with a direct lattice count at this "
        "radius - the value printed as 'Was' is the radius that failed");
    TEST_ASSERT_EQUAL_INT_MESSAGE(expected_there, got_there,
        "and these are the counts it disagreed by, at that radius");

    TEST_ASSERT_EQUAL_INT_MESSAGE(0, sand_disc_count(-1),
        "a negative radius encloses no cells - displace_disc() leans on this "
        "instead of guarding its own caller");
}

/* HALF OF A TWO-PART GUARANTEE. WEAK OR DISTANT BLAST - radius 1 - never
 * reaches the wall's own candidate cells, ensuring exact containment. See
 * test_a_strong_close_blast_can_breach_a_wall for the other half. */
static void test_a_blast_inside_a_sealed_vessel_stays_inside_it(void)
{
    fixture();
    sand_enable_impulses(&s, impulse_buf, W * H);

    /* Payload centre, clearance ensures room to fly. */
    for (int y = 0; y < H; y++) {
        for (int x = 0; x < W; x++) {
            if (x == 0 || x == W - 1 || y == 0 || y == H - 1) {
                sand_set(&s, x, y, CELL_MAKE(MAT_STONE, SAND_AMBIENT_HEAT));
            }
        }
    }
    sand_set(&s, 3, 3, SAND_FIRST_SHADE);
    sand_set(&s, 4, 3, SAND_FIRST_SHADE);
    sand_set(&s, 3, 4, SAND_FIRST_SHADE);
    sand_set(&s, 4, 4, SAND_FIRST_SHADE);

    /* Centred on one corner of the 2x2 payload, radius 1: its two occupied
     * cardinal neighbours (RIGHT and DOWN) each get thrown towards a wall
     * that is three cells away - not an immediate bounce, an actual flight. */
    sand_explode(&s, 3, 3, 1);

    /* Checked before anything else runs, fire is KIND_GAS and may rise,
     * hiding a core that was never filled. */
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(MAT_FIRE, CELL_MATERIAL(sand_at(&s, 3, 3)),
        "the blast's own centre must flash into fire, not four grains still "
        "occupying their original footprint");
    const int after_explode = sand_count(&s);

    for (int i = 0; i < 30; i++) {
        sand_step(&s, 0, 1000, 0);
    }

    for (int y = 0; y < H; y++) {
        for (int x = 0; x < W; x++) {
            if (x == 0 || x == W - 1 || y == 0 || y == H - 1) {
                TEST_ASSERT_EQUAL_UINT8_MESSAGE(MAT_STONE,
                    CELL_MATERIAL(sand_at(&s, x, y)),
                    "the wall must still be exactly the wall - a flying grain "
                    "that reached it must have stopped, not passed through "
                    "or displaced it");
            }
        }
    }
    /* Measured after explode, bounded, see test_a_blast_conserves_grains.
     * Core fire can vanish in sealed vessel. Increase count is a hard bug. */
    TEST_ASSERT_LESS_OR_EQUAL_INT_MESSAGE(after_explode, sand_count(&s),
        "outside the core, a blast only ever loses cells to fire being "
        "smothered, never creates or duplicates one, even when it is "
        "fully contained");
}

/* The box has genuine empty MARGIN outside its walls, not just the grid's
 * implicit edge, which would give a dislodged cell nowhere to go. Radius 4
 * from (3,3) reaches every wall cell, each rolling stone's 74-in-256
 * independently, so one succeeding is expected rather than a coin flip.
 *
 * Checked against the wall's own original cell, not a landing coordinate: a
 * dislodged KIND_STATIC entry falls under gravity while airborne and can
 * tumble back into the hole behind it. */
static void test_a_strong_close_blast_can_breach_a_wall(void)
{
    fixture();
    sand_enable_impulses(&s, impulse_buf, W * H);

    for (int y = 1; y <= 6; y++) {
        for (int x = 1; x <= 6; x++) {
            const bool on_wall = (x == 1 || x == 6 || y == 1 || y == 6);
            if (on_wall) {
                sand_set(&s, x, y, CELL_MAKE(MAT_STONE, SAND_AMBIENT_HEAT));
            }
        }
    }

    sand_explode(&s, 3, 3, 4);

    const int max_lifetime = (SAND_EXPLODE_INITIAL_SPEED +
                              SAND_IMPULSE_SPEED_RAMP - 1) /
                             SAND_IMPULSE_SPEED_RAMP;
    for (int i = 0; i < max_lifetime + 20; i++) {
        sand_step(&s, 0, 1000, 0);
    }

    TEST_ASSERT_NOT_EQUAL_INT_MESSAGE(MAT_STONE,
        CELL_MATERIAL(sand_at(&s, 6, 1)),
        "a strong enough blast pointed directly at a close wall must be "
        "able to dislodge the corner cell at (6,1) - the density roll's "
        "own capability, confirmed against this exact deterministic seed "
        "- and it needs to be seen actually happening, not just assumed "
        "from the roll's own arithmetic; where it lands afterward is no "
        "longer pinned, see this test's own top comment for why");
}

static void test_a_dislodged_wall_keeps_falling_even_if_its_first_push_roll_fails(void)
{
    fixture();
    sand_enable_impulses(&s, impulse_buf, W * H);

    sand_set(&s, 3, 0, CELL_MAKE(MAT_STONE, SAND_AMBIENT_HEAT));
    sand_impulse_dislodge(&s, 3, 0, 0, 0, SAND_IMPULSE_SPEED_RAMP);

    for (int i = 0; i < H; i++) {
        sand_step(&s, 0, 1000, 0);
    }

    TEST_ASSERT_EQUAL_INT_MESSAGE(H - 1, first_row_holding(MAT_STONE),
        "a dislodged KIND_STATIC cell whose very first outward-push roll "
        "fails (speed 0 here, guaranteeing it on every turn) must still "
        "keep falling under the unconditional gravity-drift every step "
        "until it is genuinely supported - stopping partway down means "
        "the failed roll dropped it from impulse tracking while it was "
        "still airborne, exactly the bug this test exists to catch");
}

/* Glass that is actually thrown arrives as cullet, still carrying the push.
 *
 * The conversion happens as the entry is queued, so grid and entry have to
 * agree on the exact byte - flight matches `cell` against what is really
 * there, and drops a shard it no longer recognises. */
static void test_a_pane_knocked_loose_is_queued_as_cullet(void)
{
    fixture();
    sand_enable_impulses(&s, impulse_buf, W * H);

    sand_set(&s, 1, 1, GLASS);
    sand_impulse_dislodge(&s, 1, 1, ring_of(1, 0), SAND_EXPLODE_INITIAL_SPEED,
                          SAND_IMPULSE_SPEED_RAMP);

    TEST_ASSERT_EQUAL_INT_MESSAGE(1, s.impulse_count,
        "the dislodge has to have queued the pane in the first place");

    const cell_t c = sand_at(&s, 1, 1);
    TEST_ASSERT_EQUAL_INT_MESSAGE(MAT_SAND, CELL_MATERIAL(c),
        "a pane the impulse actually took hold of must have broken");
    TEST_ASSERT_TRUE_MESSAGE(CELL_VARIANT(c) >= SAND_CULLET_BASE,
        "and must land in the cullet band, the same as a cracked pane - "
        "broken glass reads as broken glass, not as beach");
    TEST_ASSERT_EQUAL_INT_MESSAGE(c, s.impulse_buf[0].cell,
        "the entry has to carry the byte now on the grid, or the flight "
        "pass drops it as stale on its very first step");
    TEST_ASSERT_EQUAL_INT_MESSAGE(SAND_EXPLODE_INITIAL_SPEED,
        s.impulse_buf[0].speed, "and must keep the speed it was given");
    TEST_ASSERT_EQUAL_INT_MESSAGE(ring_of(1, 0), s.impulse_buf[0].dir,
        "and the direction - breaking must not cost the shard its push");
}

/* And that push is still spent flying, not merely recorded.
 *
 * The queue-time asserts above would all pass on a shard that dropped
 * straight down - gravity-drift alone still moves a tracked cell. Sideways
 * travel only happens if the entry survived re-acquisition. */
static void test_a_shattered_pane_flies_the_way_it_was_pushed(void)
{
    fixture();
    sand_enable_impulses(&s, impulse_buf, W * H);

    sand_set(&s, 1, 1, GLASS);
    sand_impulse_dislodge(&s, 1, 1, ring_of(1, 0), SAND_EXPLODE_INITIAL_SPEED,
                          SAND_IMPULSE_SPEED_RAMP);

    for (int i = 0; i < H; i++) {
        sand_step(&s, 0, 1000, 0);
    }

    int landed = -1;
    for (int y = 0; y < H; y++) {
        for (int x = 0; x < W; x++) {
            if (CELL_MATERIAL(sand_at(&s, x, y)) == MAT_SAND) {
                landed = x;
            }
        }
    }
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, count_cells_of(MAT_GLASS),
        "no pane may survive the throw");
    TEST_ASSERT_TRUE_MESSAGE(landed > 1,
        "the shard has to have travelled east, the way it was pushed - "
        "landing back under where the pane stood means the entry was "
        "dropped and only gravity ever moved it");
}

/* A pane no impulse reaches is not a broken pane.
 *
 * Glass is KIND_STATIC, so an ordinary sand_impulse() refuses it outright -
 * and a refusal is not a push. Without this, shattering could just as well
 * have been wired to "something tried", which would break every pane a
 * blast merely happened near. */
static void test_a_pane_that_refuses_the_push_stays_a_pane(void)
{
    fixture();
    sand_enable_impulses(&s, impulse_buf, W * H);

    sand_set(&s, 1, 1, GLASS);
    sand_impulse(&s, 1, 1, ring_of(1, 0), SAND_EXPLODE_INITIAL_SPEED);

    TEST_ASSERT_EQUAL_INT_MESSAGE(0, s.impulse_count,
        "sand_impulse() has to have refused the KIND_STATIC pane");
    TEST_ASSERT_EQUAL_INT_MESSAGE(MAT_GLASS,
        CELL_MATERIAL(sand_at(&s, 1, 1)),
        "and a pane that was never thrown must still be a pane");
}

static void test_sand_displace_alone_never_creates_fire_or_smoke(void)
{
    fixture();
    sand_enable_impulses(&s, impulse_buf, W * H);

    for (int y = 1; y <= 5; y++) {
        for (int x = 1; x <= 5; x++) {
            sand_set(&s, x, y, SAND_FIRST_SHADE);
        }
    }
    sand_set(&s, 3, 3, CELL_MAKE(MAT_WOOD, 0));

    sand_displace(&s, 3, 3, 3);

    TEST_ASSERT_EQUAL_UINT8_MESSAGE(MAT_WOOD, CELL_MATERIAL(sand_at(&s, 3, 3)),
        "sand_displace() has nothing that fills a core with fire - the "
        "centre cell must be exactly what it was before the call");
    TEST_ASSERT_GREATER_THAN_MESSAGE(0, s.impulse_count,
        "the surrounding sand must actually have been queued to fly, or "
        "this scene never exercised the displacement half of this "
        "function at all - a test proving 'no fire' means nothing if "
        "nothing else happened either");

    for (int y = 0; y < H; y++) {
        for (int x = 0; x < W; x++) {
            TEST_ASSERT_NOT_EQUAL_MESSAGE(MAT_FIRE, CELL_MATERIAL(sand_at(&s, x, y)),
                "sand_displace() must never place fire anywhere on the "
                "board - that is sand_explode()'s own addition on top of "
                "this function, not something this function does itself");
        }
    }

    for (int i = 0; i < 40; i++) {
        sand_step(&s, 0, 1000, 0);
    }

    for (int y = 0; y < H; y++) {
        for (int x = 0; x < W; x++) {
            const cell_t c = sand_at(&s, x, y);
            TEST_ASSERT_NOT_EQUAL_MESSAGE(MAT_FIRE, CELL_MATERIAL(c),
                "still no fire anywhere after settling - nothing sand_"
                "displace() did should have given the reactions pass "
                "anything to ignite");
            TEST_ASSERT_NOT_EQUAL_MESSAGE(MAT_STEAM, CELL_MATERIAL(c),
                "and no smoke either - smoke/steam residue is what a "
                "burnt-out fire or finished log leaves behind, and "
                "nothing here was ever set alight to finish burning");
        }
    }
    TEST_ASSERT_FALSE_MESSAGE(cell_is_burning(sand_at(&s, 3, 3)),
        "the wood at the centre must not have caught fire from anything "
        "sand_displace() did, however long the simulation runs "
        "afterward");
}

static void test_a_blast_conserves_grains(void)
{
    fixture();
    sand_enable_impulses(&s, impulse_buf, W * H);

    for (int y = 2; y < 5; y++) {
        for (int x = 2; x < 6; x++) {
            sand_set(&s, x, y, SAND_FIRST_SHADE);
        }
    }

    sand_explode(&s, 3, 3, 2);

    const int expected = sand_count(&s);

    /* BOUNDED, NOT EXACT - core adds fire, sand sinks through. See
     * sand_reactions.c. Corner test hit this on 136/20,000 seeds. Count never
     * goes up. */
    for (int i = 0; i < 60; i++) {
        sand_step(&s, 0, 1000, 0);
        TEST_ASSERT_LESS_OR_EQUAL_INT_MESSAGE(expected, sand_count(&s),
            "the flight pass itself must never create or duplicate a cell - "
            "a count ABOVE the post-explode value is always a bug");
    }
}

static void test_a_blast_at_the_edge_stays_in_bounds(void)
{
    /* Every corner, and the middle of one edge - each centred exactly on the
     * grid boundary, so most of the disc is off-grid and every direction is
     * exercised against the wall sand_at() makes of it, not a painted one. */
    static const struct { int cx, cy; } spots[] = {
        { 0, 0 }, { W - 1, 0 }, { 0, H - 1 }, { W - 1, H - 1 }, { W / 2, 0 },
    };

    for (size_t i = 0; i < sizeof(spots) / sizeof(spots[0]); i++) {
        fixture();
        sand_enable_impulses(&s, impulse_buf, W * H);
        for (int y = 0; y < H; y++) {
            for (int x = 0; x < W; x++) {
                sand_set(&s, x, y, SAND_FIRST_SHADE);
            }
        }

        sand_explode(&s, spots[i].cx, spots[i].cy, 3);

        /* Measured AFTER the explode - see test_a_blast_conserves_grains's
         * own comment on why the core's removal is real and everything
         * past this point is the invariant under test. */
        const int expected = sand_count(&s);

        for (int step = 0; step < 20; step++) {
            sand_step(&s, 0, 1000, 0);
            /* Bounded, not exact - see test_a_blast_conserves_grains.
             * INCREASE past `expected` is a hard bug. */
            TEST_ASSERT_LESS_OR_EQUAL_INT_MESSAGE(expected, sand_count(&s),
                "a blast centred on the grid edge must never manufacture a "
                "cell from the off-grid space it can never queue an entry "
                "for - CRACK_MAX exists for the same worst-case reason");
        }
    }
}

static void test_a_dropped_entry_never_moves_someone_elses_cell(void)
{
    fixture();
    sand_enable_impulses(&s, impulse_buf, W * H);

    sand_set(&s, 4, 4, SAND_FIRST_SHADE);
    sand_explode(&s, 3, 4, 1);   /* (4,4) is the RIGHT neighbour of centre */
    /* Checks MATERIAL for burning neighbour logic. */

    /* Something else claims the exact cell the entry still names, before it
     * ever gets another turn - a reaction or a second paint stroke would do
     * this on the real board just as easily as this test does it directly. */
    sand_set(&s, 4, 4, CELL_MAKE(MAT_STONE, SAND_AMBIENT_HEAT));

    sand_step(&s, 0, 1000, 0);

    /* Material only: the cell now neighbours the core fire, and stone
     * banks heat from a burning neighbour, so its heat variant drifts
     * legitimately within this step. */
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(MAT_STONE, CELL_MATERIAL(sand_at(&s, 4, 4)),
        "the entry must have been dropped - the cell it named no longer "
        "holds the grain it threw");
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(SAND_EMPTY, sand_at(&s, 5, 4),
        "relocating the stone would have moved it exactly here - a dropped "
        "entry must not move whatever now sits in its old cell instead");
}

static impulse_t tiny_impulse_buf[2];

static impulse_t axis_impulse_buf[8];

static void test_the_cap_degrades_gracefully(void)
{
    fixture();
    sand_enable_impulses(&s, tiny_impulse_buf, 2);

    /* Three FULL-WIDTH rows: no free-floating block. Full width ensures all
     * cells' sliding diagonals are either occupied or off-grid, preventing
     * movement under gravity. */
    for (int y = 5; y <= 7; y++) {
        for (int x = 0; x < W; x++) {
            sand_set(&s, x, y, SAND_FIRST_SHADE);
        }
    }

    sand_explode(&s, 3, 6, 1);

    TEST_ASSERT_EQUAL_INT_MESSAGE(2, s.impulse_count,
        "the buffer holds 2, so exactly 2 of the 5 true disc members "
        "must have been queued - not fewer, and the rest must not have "
        "silently bumped one of the first two out");
    TEST_ASSERT_EQUAL_UINT16_MESSAGE((uint16_t)(7 * W + 3),
        s.impulse_buf[0].index,
        "the accumulator's own arithmetic (see this test's top comment) "
        "fires on DOWN (3,7) first, not UP - even thinning, not a "
        "first-come truncation");
    TEST_ASSERT_EQUAL_UINT16_MESSAGE((uint16_t)(6 * W + 4),
        s.impulse_buf[1].index,
        "and on RIGHT (4,6) second - the last of the buffer's 2 slots");

    const int expected = sand_count(&s);

    for (int i = 0; i < 20; i++) {
        sand_step(&s, 0, 1000, 0);
    }

    TEST_ASSERT_NOT_EQUAL_MESSAGE(SAND_EMPTY, sand_at(&s, 3, 5),
        "UP did not fit in a buffer of 2 and must not fly - and unlike "
        "the other three candidates, UP had a genuinely open path to fly "
        "through if it had been wrongly queued");
    TEST_ASSERT_NOT_EQUAL_MESSAGE(SAND_EMPTY, sand_at(&s, 2, 6),
        "LEFT did not fit either, for the same reason");
    TEST_ASSERT_EQUAL_INT_MESSAGE(expected, sand_count(&s),
        "over the cap is a smaller-looking blast, not a bug - nothing may "
        "be lost");
}

/* THE BUG THIS PINS: sand_explode()'s `keep` was sized against total
 * buffer capacity, not room left by an in-flight earlier explosion - a
 * second blast before the first's grains flew got silently truncated
 * lopsided by CONTENTION between blasts (fix: sand_explode()'s `room`,
 * sand.c).
 *
 * Two non-overlapping blasts, no sand_step() between. The second blast's
 * fire order (room=4) matches sand_explode()'s ring-then-edge scan order
 * (sand.h's "QUEUED BY RING") - see the assertions below. */
static void test_two_overlapping_blasts_share_the_buffer_evenly(void)
{
    fixture();
    sand_enable_impulses(&s, axis_impulse_buf, 8);

    for (int y = 0; y < H; y++) {
        for (int x = 0; x < W; x++) {
            sand_set(&s, x, y, SAND_FIRST_SHADE);
        }
    }

    sand_explode(&s, 1, 1, 1);
    TEST_ASSERT_EQUAL_INT_MESSAGE(4, s.impulse_count,
        "precondition: the first blast's own 4 real neighbours must all "
        "have queued cleanly against a fully free buffer, or this test "
        "is not actually exercising a second blast with only 4 slots "
        "left");

    /* NO sand_step() HERE - the first blast's grains stay exactly where
     * they were queued, still "in flight" by every measure sand_explode()
     * itself can see (s->impulse_count unchanged), which is the whole
     * scenario this test exists to create. */
    sand_explode(&s, 5, 2, 2);

    TEST_ASSERT_EQUAL_INT_MESSAGE(8, s.impulse_count,
        "the second blast had exactly 4 slots of real room left and must "
        "have filled every one of them, no more and no less");
    TEST_ASSERT_EQUAL_UINT16_MESSAGE((uint16_t)(1 * W + 5),
        s.impulse_buf[4].index,
        "worked by hand against room=4 (see this test's own top comment): "
        "UP, (5,1), fires first");
    TEST_ASSERT_EQUAL_UINT16_MESSAGE((uint16_t)(3 * W + 6),
        s.impulse_buf[5].index,
        "down-right, (6,3), fires second");
    TEST_ASSERT_EQUAL_UINT16_MESSAGE((uint16_t)(0 * W + 5),
        s.impulse_buf[6].index,
        "ring 2's own UP cell, (5,0), fires third - reaching ring 2 at "
        "all is exactly what the old, buggy keep (sized from the WHOLE "
        "buffer instead of what was actually left) never did, because it "
        "ran out of real room while still inside ring 1");
    TEST_ASSERT_EQUAL_UINT16_MESSAGE((uint16_t)(2 * W + 7),
        s.impulse_buf[7].index,
        "and RIGHT, (7,2), fires fourth and last - the buffer's own "
        "final slot, spent evenly across the whole disc rather than "
        "concentrated in the first ring the scan happened to reach");
}

/* Grain freezes if block never reexamined; see Adding-a-Material.md. Uses
 * settle_with_sleeping()/assert_nothing_left_to_do(). */
static void test_a_blast_wakes_the_blocks_it_touches(void)
{
    static const char *bed[] = {
        "........",
        "........",
        "........",
        "........",
        "........",
        "oooooooo",
        "oooooooo",
        "oooooooo",
    };
    settle_with_sleeping(bed, 8, 100, 0, 1000);
    sand_enable_impulses(&s, impulse_buf, W * H);

    /* Centre inside the now-asleep bed, radius 1: the UP neighbour is
     * thrown off the bed's own surface into the open air above it - exactly
     * the displaced-grain-freezes-mid-air scenario this test exists for, if
     * the block it lands in is never woken back up. */
    sand_explode(&s, 3, 6, 1);

    const int max_lifetime = (SAND_EXPLODE_INITIAL_SPEED +
                              SAND_IMPULSE_SPEED_RAMP - 1) /
                             SAND_IMPULSE_SPEED_RAMP;
    for (int i = 0; i < max_lifetime + 20; i++) {
        sand_step(&s, 0, 1000, 0);
    }

    assert_nothing_left_to_do(0, 1000);
}

/* A SECOND REGRESSION GUARD - identity check's blind spot. Grains don't arc
 * due to gravity moving them before step_impulses().
 * SAND_EXPLODE_INITIAL_SPEED affects initial speed roll, not identity
 * mechanism. */
static void test_a_flying_grain_keeps_its_outward_push_while_falling(void)
{
    /* ONCE, outside the trial loop below: fixture() re-seeds the rng, so
     * calling it per trial would replay one identical trial sixteen times.
     * sand_clear() wipes the grid and leaves the rng where it was. */
    fixture();
    sand_enable_impulses(&s, impulse_buf, W * H);

    const int max_lifetime =
        (SAND_EXPLODE_INITIAL_SPEED + SAND_IMPULSE_SPEED_RAMP - 1) /
        SAND_IMPULSE_SPEED_RAMP;
    const int steps = max_lifetime + 5;

    int max_x = 1;

    for (int trial = 0; trial < 16; trial++) {
        sand_clear(&s);

        /* Worst case for identity check: step_impulses() must re-acquire
         * grain or entry dies on turn one. */
        sand_set(&s, 1, 1, SAND_FIRST_SHADE);
        /* Centre one cell to the left, radius 1: (1,1) is the RIGHT
         * neighbour, so the only way it ever gains x is the flight pass -
         * gravity here only ever pulls straight down, column 1. */
        sand_explode(&s, 0, 1, 1);

        for (int i = 0; i < steps; i++) {
            sand_step(&s, 0, 1000, 0);
        }

        TEST_ASSERT_EQUAL_INT_MESSAGE(0, s.impulse_count,
            "flight must have ended within a fixed, deterministic step "
            "count once speed ramps to zero - not merely become "
            "improbable, as the old fixed-chance decay left it");

        for (int y = 0; y < H; y++) {
            for (int x = 0; x < W; x++) {
                if (CELL_MATERIAL(sand_at(&s, x, y)) == MAT_SAND && x > max_x) {
                    max_x = x;
                }
            }
        }
    }

    /* Gravity alone could never move any of these eight grains sideways
     * at all - straight down is the only direction it ever pulls on its
     * own. Any of the eight ending up past x=1 proves the outward push
     * survived past the very first step, which is exactly where the old
     * identity check would have dropped it the moment gravity claimed the
     * grain out from under it - this is the exact regression that made a
     * crater with no arc. */
    TEST_ASSERT_GREATER_THAN_MESSAGE(1, max_x,
        "a flying grain must keep drifting outward across more than one "
        "step, not lose its push the instant gravity also touches it");
}

/* --- KIND_STATIC support agreeing with the drift, not merely CELL_IS_EMPTY
 * on the one cell straight below (bd - "a thrown static chunk settles too
 * eagerly") -------------------------------------------------------------- */

/* can_impulse_enter() only refuses KIND_STATIC, so gravity-drift always
 * swaps into powder beneath a falling chunk; the old settled check
 * disagreed, declaring it settled after one row. Split into two tests:
 * ENERGETIC must still sink deep, SPENT (below SAND_IMPULSE_SINK_MIN_SPEED,
 * sand.h) must rest instead. */
enum { SETTLE_COL = 3, SETTLE_TOP_ROW = 0 };

/* The drift used to charge no drag, so an energetic chunk tunnelled an
 * entire powder bank for free regardless of how far it had travelled. Now
 * charges the same drag the push site does (SAND_IMPULSE_DRAG_POWDER_SHIFT,
 * sand.h), stopping within the first few layers instead. */
static void test_an_energetic_static_chunk_over_a_powder_bank_now_stops_within_the_first_few_layers(void)
{
    fixture();
    sand_clear(&s);
    sand_enable_impulses(&s, impulse_buf, W * H);

    sand_set(&s, SETTLE_COL, SETTLE_TOP_ROW, STONE);
    for (int y = SETTLE_TOP_ROW + 1; y < H; y++) {
        for (int x = 0; x < W; x++) {
            sand_set(&s, x, y, SAND);
        }
    }
    sand_impulse_dislodge(&s, SETTLE_COL, SETTLE_TOP_ROW, 0, 255,
                          SAND_IMPULSE_SPEED_RAMP);

    for (int i = 0; i < H; i++) {
        sand_step(&s, 0, 1000, 0);
    }

    /* MEASURED: row 2 of 8, deterministically, but graded as a range rather
     * than pinned - the range is what catches the two failure shapes this
     * rung sits between, staying at the rim (drag charged so hard or so
     * early the chunk never moves) and sinking to the bottom again (drag
     * not charged at the drift site at all). */
    const int landed_row = first_row_holding(MAT_STONE);
    TEST_ASSERT_GREATER_THAN_MESSAGE(SETTLE_TOP_ROW, landed_row,
        "an ENERGETIC thrown KIND_STATIC chunk must still fall AT LEAST one "
        "row into the bank - a chunk stuck exactly at the rim would mean "
        "drag is now charged so eagerly the drift cannot move it at all, "
        "which is not this rung's own fix either");
    TEST_ASSERT_LESS_THAN_INT_MESSAGE(H / 2, landed_row,
        "and it must stop well short of the bottom (measured row 2 of 8) - "
        "reaching anywhere near H-1 again would mean the drift's own "
        "displacement is silently paying no drag, exactly the device bug "
        "(a far-thrown chunk tunnels through a sand pyramid and ejects "
        "nothing, however deep, regardless of how far it travelled to get "
        "there) impulse_charge_displacement() exists to close");
}

/* Both scenes fill the entire row, not one column: a SPENT chunk's empty
 * diagonal lets it slide sideways and free-fall past the bed instead of
 * resting there - measured, a narrow column let it escape to row 5 instead
 * of settling at the rim. */
static void test_a_spent_static_chunk_rests_on_a_powder_bank_instead_of_sinking_forever(void)
{
    fixture();
    sand_clear(&s);
    sand_enable_impulses(&s, impulse_buf, W * H);

    sand_set(&s, SETTLE_COL, SETTLE_TOP_ROW, STONE);
    for (int y = SETTLE_TOP_ROW + 1; y < H; y++) {
        for (int x = 0; x < W; x++) {
            sand_set(&s, x, y, SAND);
        }
    }
    sand_impulse_dislodge(&s, SETTLE_COL, SETTLE_TOP_ROW, 0, 0,
                          SAND_IMPULSE_SPEED_RAMP);

    for (int i = 0; i < H; i++) {
        sand_step(&s, 0, 1000, 0);
    }

    TEST_ASSERT_EQUAL_INT_MESSAGE(SETTLE_TOP_ROW, first_row_holding(MAT_STONE),
        "a SPENT thrown KIND_STATIC chunk - speed 0, below SAND_IMPULSE_"
        "SINK_MIN_SPEED from its very first step, forcing every push-roll "
        "to fail so the scene is driven entirely by gravity-drift plus the "
        "settled check - must rest right where it landed once every "
        "gravity-ward candidate is genuinely occupied, rather than "
        "swapping through the entire bank the way an unconditional "
        "gravity-drift once did (device feedback: a thrown chunk entering "
        "a powder bank did not stop)");
}

/* --- PENETRATION MUST STOP BEING DISTANCE-DEPENDENT ---------------------
 *
 * Only the push move charges drag, and only when rolled_move (whose chance
 * IS entry.speed) succeeds. A chunk that has flown a long way arrives slow
 * and mostly FALLING, so nearly every displacement it makes is the gravity
 * drift's swap instead - without impulse_charge_displacement() it tunnels in
 * silently. MEASURED, 40 seeds: mean penetration 6.775 rows with that charge
 * mutated back out, 0.000 with it. */
#define FAR_SINK_W 20
#define FAR_SINK_BED_DEPTH 20
#define FAR_SINK_DISTANCE 50
#define FAR_SINK_H (FAR_SINK_DISTANCE + FAR_SINK_BED_DEPTH + 10)
#define FAR_SINK_SEEDS 40
static long far_sink_penetration_total(uint8_t *cells, impulse_t *buf)
{
    long total = 0;
    for (uint32_t seed = 1; seed <= (uint32_t)FAR_SINK_SEEDS; seed++) {
        const int bed_y0    = FAR_SINK_DISTANCE;
        const int bed_y1    = bed_y0 + FAR_SINK_BED_DEPTH - 1;
        const int floor_row = bed_y1 + 1;
        sand_t g;
        memset(cells, 0, (size_t)FAR_SINK_W * FAR_SINK_H);
        sand_init(&g, cells, FAR_SINK_W, FAR_SINK_H, seed);
        sand_enable_impulses(&g, buf, FAR_SINK_W * FAR_SINK_H);

        /* The floor sits directly under the bed: with open air beneath it a
         * bed is a second falling body, free-falling in lockstep with the
         * chunk chasing it and never actually touched. */
        for (int x = 0; x < FAR_SINK_W; x++) {
            sand_set(&g, x, floor_row, STONE);
        }
        for (int y = bed_y0; y <= bed_y1; y++) {
            for (int x = 0; x < FAR_SINK_W; x++) {
                sand_set(&g, x, y, SAND);
            }
        }

        const int mover_x = FAR_SINK_W / 2;
        sand_set(&g, mover_x, 0, STONE);
        /* DIR_UP can only ever nudge the chunk into open air above where it
         * started, so every displacement measured here is the drift's own. A
         * direction that could also displace into the bed lets the push
         * site's already-correct accounting cover for a broken drift. */
        enum { DIR_UP = 4 };
        sand_impulse_dislodge(&g, mover_x, 0, DIR_UP, 255,
                              SAND_IMPULSE_SPEED_RAMP);

        /* PAST THE DETERMINISTIC FLIGHT-TIME BOUND - see this file's own
         * max_lifetime derivation elsewhere for the same reasoning: a
         * regression back to the unconditional drift can keep this entry
         * tracked (and sinking) for up to a full ramp's worth of steps
         * after it enters the bed, on top of the open-air fall it already
         * took to get there. A shorter budget here (measured directly)
         * still shows the chunk mid-sink in some seeds under the bug,
         * understating exactly the depth this test exists to catch. */
        for (int i = 0; i < FAR_SINK_DISTANCE + 150; i++) {
            sand_step(&g, 0, 1000, 0);
        }

        int landed_row = -1;
        for (int y = 0; y < floor_row && landed_row < 0; y++) {
            for (int x = 0; x < FAR_SINK_W; x++) {
                if (CELL_MATERIAL(sand_at(&g, x, y)) == MAT_STONE) {
                    landed_row = y;
                    break;
                }
            }
        }
        /* landed_row < bed_y0 means the chunk stopped short of the bed
         * entirely (its own ramp exhausted before it ever arrived) - zero
         * penetration, nothing to add, rather than a negative figure. */
        if (landed_row >= bed_y0) {
            total += (landed_row - bed_y0);
        }
    }
    return total;
}

static void test_a_static_chunk_thrown_far_still_stops_shallow_in_the_bed_it_hits(void)
{
    uint8_t *cells = malloc((size_t)FAR_SINK_W * FAR_SINK_H);
    TEST_ASSERT_NOT_NULL_MESSAGE(cells,
        "far-sink grid must fit in what the framebuffer leaves");
    impulse_t *buf = malloc((size_t)(FAR_SINK_W * FAR_SINK_H) * sizeof *buf);
    TEST_ASSERT_NOT_NULL_MESSAGE(buf,
        "far-sink impulse queue must fit in what the framebuffer leaves");

    const long total = far_sink_penetration_total(cells, buf);

    free(buf);
    free(cells);

    TEST_ASSERT_LESS_THAN_MESSAGE(FAR_SINK_SEEDS * 2, total,
        "a KIND_STATIC chunk thrown 50 rows of open air before it ever "
        "reaches a packed bed must still stop within a couple of rows of "
        "the surface, same as one thrown from right beside it - measured "
        "a mean of 6.775 rows deep (271 summed across 40 seeds) with the "
        "gravity-drift's own drag/transfer charge hard-mutated out, "
        "against 0 with this rung's fix: the whole point of "
        "impulse_charge_displacement() is that a chunk cannot silently "
        "bury itself just because it arrived slow and mostly falling "
        "instead of fast and mostly pushing");
}

/* THE SAME SPLIT, OVER A LIQUID: an energetic chunk sinks into water rather
 * than resting on its surface the way a never-thrown one would, while a
 * SPENT one rests there instead - the chosen trade named in
 * SAND_IMPULSE_SINK_MIN_SPEED's own comment, whose floor is not kind-aware,
 * so a spent chunk stalls on a liquid exactly as on a powder. */
static void test_an_energetic_static_chunk_still_sinks_into_water_instead_of_resting_on_its_surface(void)
{
    fixture();
    sand_clear(&s);
    sand_enable_impulses(&s, impulse_buf, W * H);

    /* CELL_MAKE(MAT_WATER, MASS_MAX), NOT THE WATER MACRO - the WATER
     * macro is only half mass (variant 8 of MASS_MAX's own 15), which
     * leaves ordinary liquid equalisation room to reshuffle it into a
     * genuinely empty cell somewhere in the column within a step or two,
     * exactly the sideways-escape problem this section's own top comment
     * describes for a spent entry. A fully packed column has no such gap
     * to find. */
    sand_set(&s, SETTLE_COL, SETTLE_TOP_ROW, STONE);
    for (int y = SETTLE_TOP_ROW + 1; y < H; y++) {
        for (int x = 0; x < W; x++) {
            sand_set(&s, x, y, CELL_MAKE(MAT_WATER, MASS_MAX));
        }
    }
    sand_impulse_dislodge(&s, SETTLE_COL, SETTLE_TOP_ROW, 0, 255,
                          SAND_IMPULSE_SPEED_RAMP);

    for (int i = 0; i < H; i++) {
        sand_step(&s, 0, 1000, 0);
    }

    TEST_ASSERT_EQUAL_INT_MESSAGE(H - 1, first_row_holding(MAT_STONE),
        "an ENERGETIC thrown KIND_STATIC chunk over a water column must "
        "still sink all the way to the bottom rather than stopping at the "
        "surface - the old drift's own liquid exclusion stays deleted, "
        "and rung 4's new SPENT narrowing must not apply to anything that "
        "still has push left");
}

static void test_a_spent_static_chunk_still_sinks_through_water_to_the_bottom(void)
{
    fixture();
    sand_clear(&s);
    sand_enable_impulses(&s, impulse_buf, W * H);

    /* CELL_MAKE(MAT_WATER, MASS_MAX), NOT THE WATER MACRO - see the
     * energetic test just above for why the half-mass WATER macro leaves
     * ordinary equalisation room to open a genuine gap on its own. */
    sand_set(&s, SETTLE_COL, SETTLE_TOP_ROW, STONE);
    for (int y = SETTLE_TOP_ROW + 1; y < H; y++) {
        for (int x = 0; x < W; x++) {
            sand_set(&s, x, y, CELL_MAKE(MAT_WATER, MASS_MAX));
        }
    }
    sand_impulse_dislodge(&s, SETTLE_COL, SETTLE_TOP_ROW, 0, 0,
                          SAND_IMPULSE_SPEED_RAMP);

    for (int i = 0; i < H; i++) {
        sand_step(&s, 0, 1000, 0);
    }

    TEST_ASSERT_EQUAL_INT_MESSAGE(H - 1, first_row_holding(MAT_STONE),
        "a SPENT thrown KIND_STATIC chunk over a water column must still "
        "sink all the way down - the settle rule IS kind-aware now, and "
        "only packed grain holds an exhausted chunk up. A fluid parts "
        "around a solid whether or not the solid has energy left, and on "
        "device a chunk stalled mid-pool read as wrong where sinking to "
        "the floor had always looked right");
}

/* Deleting the drift's liquid exclusion is safe: the move is a SWAP
 * (move_to()), not an overwrite, so lava swapped into changes which cell it
 * occupies, not whether it exists. MASS is the exact invariant (mass_of());
 * cell count is only checked as "did not go down", since spread can split
 * one cell and an exposed surface can flare. SPEED 255: a spent chunk needs
 * an empty cell to continue (below SAND_IMPULSE_SINK_MIN_SPEED), and this
 * pool has none, so it would settle without exercising the sink. */
static void test_a_thrown_static_chunk_conserves_lava_mass_on_sink(void)
{
    fixture();
    sand_clear(&s);
    sand_enable_impulses(&s, impulse_buf, W * H);

    enum { POOL_TOP = 2 };
    sand_set(&s, 3, 0, STONE);
    for (int y = POOL_TOP; y < H; y++) {
        for (int x = 0; x < W; x++) {
            sand_set(&s, x, y, CELL_MAKE(MAT_LAVA, MASS_MAX));
        }
    }

    const long lava_mass_before  = mass_of(&s, W, H, MAT_LAVA);
    const int  lava_count_before = count_of(MAT_LAVA);

    sand_impulse_dislodge(&s, 3, 0, 0, 255, SAND_IMPULSE_SPEED_RAMP);

    /* H * 3, not H: how many steps a chunk needs to clear the pool now
     * depends on SAND_IMPULSE_CELLS_PER_STEP_DIVISOR, and this scene only
     * needs ENOUGH steps, never exactly H. A budget tied to the grid was
     * really a budget tied to that constant without saying so. */
    for (int i = 0; i < H * 3; i++) {
        sand_step(&s, 0, 1000, 0);
    }

    TEST_ASSERT_GREATER_THAN_MESSAGE(POOL_TOP, first_row_holding(MAT_STONE),
        "the chunk must actually have sunk past the pool's own surface for "
        "the conservation assertions below to mean anything - stopping at "
        "or above POOL_TOP would mean the exclusion never actually got "
        "exercised by this scene");

    TEST_ASSERT_EQUAL_INT_MESSAGE(lava_mass_before,
        mass_of(&s, W, H, MAT_LAVA),
        "total lava mass must be exactly conserved - a thrown chunk "
        "swapping into a lava cell relocates that cell, it does not "
        "destroy it, and this is the measurement that makes dropping the "
        "drift's old liquid exclusion safe rather than merely assumed");
    TEST_ASSERT_GREATER_OR_EQUAL_INT_MESSAGE(lava_count_before, count_of(MAT_LAVA),
        "and no lava cell may have been deleted outright either - checked "
        "as 'did not go down' rather than 'stayed identical', because a "
        "displaced lava cell legitimately splitting across more, shallower "
        "cells as it spreads (ordinary sand_step_liquids() equalisation,"
        " same as any liquid) is not a deletion - see this test's own top "
        "comment for the measurement that ruled out a genuine duplication");
}

/* A support that is itself in flight means WAIT, not settle. A is queued
 * first, so step_impulses() gives it its turn before B moves out from under
 * it, and A's three gravity-ward candidates are all blocked - which naively
 * reads as settled. Settling A there would freeze it in mid-air over B
 * forever, since nothing in this engine revisits a settled KIND_STATIC cell.
 *
 * WOOD, not more STONE, for the side walls, so a stray STONE cell can never
 * be mistaken for one of the two chunks being tracked. */
static void test_a_chunk_stacked_on_an_in_flight_chunk_waits_instead_of_settling_and_both_eventually_land(void)
{
    fixture();
    sand_enable_impulses(&s, impulse_buf, W * H);

    enum { COL = 3, TOP_ROW = 2, BOTTOM_ROW = 3 };

    sand_set(&s, COL, TOP_ROW, STONE);
    sand_set(&s, COL, BOTTOM_ROW, STONE);
    sand_set(&s, COL - 1, BOTTOM_ROW, WOOD);
    sand_set(&s, COL + 1, BOTTOM_ROW, WOOD);

    /* A first, THEN B - see this test's own top comment for why the order
     * matters: it is what puts A's turn before B's in step_impulses()'s
     * own loop. */
    sand_impulse_dislodge(&s, COL, TOP_ROW, 0, 0, SAND_IMPULSE_SPEED_RAMP);
    sand_impulse_dislodge(&s, COL, BOTTOM_ROW, 0, 0, SAND_IMPULSE_SPEED_RAMP);

    /* One step is enough to show the bug: A's own support check runs
     * before B has moved, so a settle-on-first-failure design drops A
     * from tracking on literally this first call. */
    sand_step(&s, 0, 1000, 0);

    TEST_ASSERT_EQUAL_UINT8_MESSAGE(MAT_STONE,
        CELL_MATERIAL(sand_at(&s, COL, TOP_ROW)),
        "the upper chunk must still be exactly where it started after "
        "just one step - it was blocked on every side this step, so it "
        "must have WAITED, not moved; the regression this guards against "
        "is not motion, it is losing impulse TRACKING while waiting");
    TEST_ASSERT_GREATER_THAN_MESSAGE(0, s.impulse_count,
        "and it must still be TRACKED - dropped from s->impulse_buf here "
        "means settled, which is exactly what must not happen while its "
        "own support (the lower chunk) is itself still in flight and "
        "about to move out from under it");

    /* Now run it out. Both chunks have a clear path to the bottom - B
     * immediately, A one step behind once B has moved - so both must
     * eventually come to rest with nothing left flying. */
    for (int i = 0; i < 4 * H; i++) {
        sand_step(&s, 0, 1000, 0);
    }

    TEST_ASSERT_EQUAL_INT_MESSAGE(0, s.impulse_count,
        "both chunks must eventually land - if the upper one were still "
        "frozen waiting on a support that can never again change, "
        "tracking would never clear");
    TEST_ASSERT_NOT_EQUAL_MESSAGE(MAT_STONE,
        CELL_MATERIAL(sand_at(&s, COL, TOP_ROW)),
        "and the upper chunk must not have settled back at its OWN "
        "starting cell either - both chunks had a clear run to the "
        "bottom, so ending up frozen at the top is itself a sign "
        "something settled prematurely");

    /* BOTH REACHED THE FLOOR - checked by ROW alone, not by column too.
     * Once B has landed, A's own straight-down candidate is blocked and the
     * shared candidate list tries the diagonal slide, so the two end up side
     * by side in the bottom row rather than stacked. Sliding around a landed
     * neighbour is the shared predicate working, not a special case, and
     * this test exists to prove neither chunk freezes mid-air. */
    int found_rows[2];
    int found = 0;
    for (int y = 0; y < H && found < 2; y++) {
        for (int x = 0; x < W && found < 2; x++) {
            if (CELL_MATERIAL(sand_at(&s, x, y)) == MAT_STONE) {
                found_rows[found++] = y;
            }
        }
    }
    TEST_ASSERT_EQUAL_INT_MESSAGE(2, found,
        "both KIND_STATIC chunks must still exist somewhere on the board "
        "- this test is about WHEN they settle, not whether either one "
        "survives");
    TEST_ASSERT_EQUAL_INT_MESSAGE(H - 1, found_rows[0],
        "both chunks had a clear run all the way to the floor once B "
        "moved out from under A, so both must have reached it - one "
        "still short of H - 1 would mean it froze somewhere on the way "
        "down instead of following once its support cleared");
    TEST_ASSERT_EQUAL_INT_MESSAGE(H - 1, found_rows[1],
        "same for the second chunk found - see the previous assertion");
}

/* --- the other half of the distinction: IMPULSE earns the sinking, being
 * a solid does not ------------------------------------------------------- */

/* The main sweep skips KIND_STATIC by design, so a never-thrown static cell
 * has no mechanism here that could move it. This is the test that fails if
 * the can_impulse_enter() sinking rule ever moves out of step_impulses()
 * into ordinary movement.
 *
 * Both sweep parities, because one proves nothing - sand_step_liquids()
 * flips s->liquid_flip every step and cross-flow takes its row order from
 * it, while a fixture starting at the default only reaches one. */
static void ordinary_static_solid_scene(bool flip)
{
    fixture();
    sand_enable_impulses(&s, impulse_buf, W * H);
    s.liquid_flip = flip;

    sand_set(&s, 2, 0, STONE);
    for (int y = 1; y < H; y++) {
        sand_set(&s, 2, y, WATER);
    }

    sand_set(&s, 5, 0, STONE);
    for (int y = 1; y < H; y++) {
        sand_set(&s, 5, y, SAND);
    }

    for (int i = 0; i < H; i++) {
        sand_step(&s, 0, 1000, 0);

        /* CHECKED EVERY STEP, AND BY KIND, not once at the end against a
         * count of zero. The water column here spreads, falls and lands on
         * itself, and water landing on water is exactly what
         * splash_displace() (sand_liquid.c) queues an impulse for - so the
         * buffer is legitimately non-empty mid-run whichever way the sweep
         * runs. What must never appear in it is a KIND_STATIC entry. */
        for (int q = 0; q < s.impulse_count; q++) {
            TEST_ASSERT_NOT_EQUAL_MESSAGE(KIND_STATIC,
                material_of(s.impulse_buf[q].cell)->kind,
                "nothing static here was ever thrown, so no KIND_STATIC "
                "cell should ever have been queued into impulse tracking "
                "- it is IMPULSE that earns the sinking this feature "
                "adds, not merely being a solid");
        }
    }

    TEST_ASSERT_EQUAL_UINT8_MESSAGE(MAT_STONE,
        CELL_MATERIAL(sand_at(&s, 2, 0)),
        "an ordinary KIND_STATIC cell resting on a liquid, with no "
        "impulse behind it, must not move at all - it is IMPULSE that "
        "earns the sinking this feature adds, not merely being a solid");
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(MAT_STONE,
        CELL_MATERIAL(sand_at(&s, 5, 0)),
        "same for a powder - an ordinary KIND_STATIC cell must still just "
        "sit exactly where it was placed");
}

static void test_an_ordinary_static_solid_still_does_not_sink_into_liquid_or_powder(void)
{
    ordinary_static_solid_scene(false);
    ordinary_static_solid_scene(true);
}

/* --- Rung 1: medium drag on a thrown KIND_STATIC chunk --------------------
 *
 * step_impulses() charges extra `speed` loss per non-empty cell a
 * KIND_STATIC mover displaces, proportional to density (impulse_drag_of()) -
 * KIND_STATIC only, powder/liquid out of scope (see SAND_IMPULSE_DRAG_
 * POWDER_SHIFT, sand.h). Two test shapes: horizontal distance travelled,
 * averaged over seeds (the claim), and single-step arithmetic pins on
 * speed == 255 - SAND_IMPULSE_SPEED_RAMP - density (the formula itself). */
#define PLOW_W 40
/* Fills only row 0 with `medium`; every row below stays open to a single
 * STONE floor. Flooring the flight row itself instead (an earlier scene
 * shape) is wrong twice over: a missed push roll then ends the flight
 * outright instead of costing one step, and a wide packed medium draws one
 * RNG number per resting cell every step regardless of motion, shifting
 * which numbers the impulse's own roll consumes. */
#define PLOW_H 10
#define PLOW_SEEDS 32
/* Short enough to cover the fall without post-landing wandering diluting
 * the signal. Measured over PLOW_SEEDS seeds: air/dirt/water average 12.5 /
 * 3.6 / 5.2 cells - roughly "first few layers" and "sink partway into a
 * pool", the separation device feedback asked for. */
#define PLOW_STEPS 15

/* `cells` is memset here rather than by each caller - a fresh sand_init()
 * alone never clears the array.
 *
 * A DIRT medium must be built with a DRY variant (0 .. SOIL_DRY_TONES-1,
 * material.h). Anything from SOIL_DRY_TONES up is WET, and wet soil
 * percolates and dries while the measurement runs: variant 8 moves dirt's
 * averaged distance from 7.25 cells to 6.66. Drag reads density and does not
 * care, so nothing fails - the scene just stops being the one the test name
 * claims. */
static void plow_build(sand_t *g, uint8_t *cells, impulse_t *buf, int buf_max,
                       uint32_t seed, material_id_t mover, cell_t medium)
{
    memset(cells, 0, (size_t)PLOW_W * PLOW_H);
    sand_init(g, cells, PLOW_W, PLOW_H, seed);
    sand_enable_impulses(g, buf, buf_max);

    for (int x = 0; x < PLOW_W; x++) {
        sand_set(g, x, PLOW_H - 1, STONE);
    }
    sand_set(g, 1, 0, CELL_MAKE(mover, 8));
    if (!CELL_IS_EMPTY(medium)) {
        for (int x = 2; x < PLOW_W; x++) {
            sand_set(g, x, 0, medium);
        }
    }

    enum { DIR_RIGHT = 2 };
    if (material_by_id((material_id_t)mover)->kind == KIND_STATIC) {
        sand_impulse_dislodge(g, 1, 0, DIR_RIGHT, 255, SAND_IMPULSE_SPEED_RAMP);
    } else {
        sand_impulse(g, 1, 0, DIR_RIGHT, 255);
    }
}

/* The mover's current column, wherever it ended up - flying or settled,
 * anywhere above the floor row. Scans every row EXCEPT the floor (which is
 * STONE across the whole width and would otherwise be mistaken for the
 * mover on any seed where it has not yet reached the bottom). */
static int plow_mover_x(sand_t *g)
{
    for (int y = 0; y < PLOW_H - 1; y++) {
        for (int x = 0; x < PLOW_W; x++) {
            if (CELL_MATERIAL(sand_at(g, x, y)) == MAT_STONE) {
                return x;
            }
        }
    }
    return -1;
}

/* Summed, not averaged, across PLOW_SEEDS seeds 1..PLOW_SEEDS - matching
 * test_water_does_not_drill_into_oil_when_tilted's own `total` shape
 * above. Every caller uses the SAME seed set (1..PLOW_SEEDS), so a
 * comparison between two calls is apples to apples. */
static long plow_total_distance(uint8_t *cells, impulse_t *buf, cell_t medium)
{
    long total = 0;
    for (uint32_t k = 1; k <= (uint32_t)PLOW_SEEDS; k++) {
        sand_t g;
        plow_build(&g, cells, buf, 4, k, MAT_STONE, medium);
        for (int i = 0; i < PLOW_STEPS; i++) {
            sand_step(&g, 0, 1000, 0);
        }
        total += plow_mover_x(&g) - 1;
    }
    return total;
}

static void test_a_thrown_chunk_travels_less_far_through_dirt_than_through_air(void)
{
    uint8_t *cells = malloc((size_t)PLOW_W * PLOW_H);
    TEST_ASSERT_NOT_NULL_MESSAGE(cells,
        "plow grid must fit in what the framebuffer leaves");
    impulse_t buf[4];

    const long air_total  = plow_total_distance(cells, buf, 0);
    const long dirt_total = plow_total_distance(cells, buf, CELL_MAKE(MAT_DIRT, 0));

    free(cells);

    TEST_ASSERT_LESS_THAN_INT_MESSAGE(air_total, dirt_total,
        "a thrown KIND_STATIC chunk must travel less far, summed across "
        "the same seeds, through a bank of dirt than through open air - "
        "today it crosses both at exactly the same rate and simply "
        "teleports the dirt behind it, which is the reported bug this "
        "rung fixes");
}

/* THE ENERGY EXIT'S OWN PIN. push_count, the DISTANCE budget, is computed
 * once from the speed the mover carried BEFORE this step's drag charges, so
 * a chunk that spends nearly all its speed on hop 0 still has every
 * remaining hop of that budget. Measured without the exit (32 seeds): dirt
 * penetration averaged 3.0 cells against an intended rim stop of roughly
 * one. The threshold asserts an average under 2, clear of both, so it does
 * not double as a tuning pin for the drag sweep below. */
static void test_a_thrown_chunk_stops_near_the_rim_of_a_dirt_bank(void)
{
    uint8_t *cells = malloc((size_t)PLOW_W * PLOW_H);
    TEST_ASSERT_NOT_NULL_MESSAGE(cells,
        "plow grid must fit in what the framebuffer leaves");
    impulse_t buf[4];

    const long dirt_total = plow_total_distance(cells, buf, CELL_MAKE(MAT_DIRT, 0));

    free(cells);

    TEST_ASSERT_LESS_THAN_INT_MESSAGE(PLOW_SEEDS * 2, dirt_total,
        "a chunk plowing into packed dirt must stop within about one cell "
        "of the rim, ON AVERAGE - not several. Before the energy exit, "
        "push_count's own DISTANCE budget was the hop loop's only exit "
        "besides a wall, so a mover that spent nearly all its speed on the "
        "very first hop still took every remaining hop that budget "
        "allowed, at whatever near-zero speed drag left it. Drag charging "
        "a hop must be able to end the push before push_count is "
        "exhausted, not merely slow the mover down while it keeps moving");
}

static void test_a_thrown_chunk_travels_less_far_through_dirt_than_through_water(void)
{
    uint8_t *cells = malloc((size_t)PLOW_W * PLOW_H);
    TEST_ASSERT_NOT_NULL_MESSAGE(cells,
        "plow grid must fit in what the framebuffer leaves");
    impulse_t buf[4];

    const long water_total = plow_total_distance(cells, buf, CELL_MAKE(MAT_WATER, MASS_MAX));
    const long dirt_total  = plow_total_distance(cells, buf, CELL_MAKE(MAT_DIRT, 0));

    free(cells);

    TEST_ASSERT_LESS_THAN_INT_MESSAGE(water_total, dirt_total,
        "drag must be ordered by density, not a flat penalty - dirt (62) "
        "is denser than water (30), so summed across the same seeds it "
        "must charge more speed per cell displaced and leave the chunk "
        "travelling less far");
}

/* THE FORMULA ITSELF, single step, single displaced cell - the one place in
 * this rung where asserting on s.impulse_buf[0].speed directly is the claim
 * rather than a stand-in for it. The floor under the row keeps gravity from
 * pulling either cell out before the impulse gets its step.
 *
 * impulse_count is 2: rung 3's transfer queues an entry for the struck dirt
 * cell, appended only after compaction, so the mover is still entry 0. */
static void test_a_thrown_chunk_loses_speed_proportional_to_the_density_it_displaces(void)
{
    fixture();
    sand_enable_impulses(&s, impulse_buf, W * H);

    enum { ROW = 4, SX = 1, TX = 2, DIR_RIGHT = 2 };
    for (int x = 0; x < W; x++) {
        sand_set(&s, x, ROW + 1, STONE);
    }
    sand_set(&s, SX, ROW, STONE);
    sand_set(&s, TX, ROW, CELL_MAKE(MAT_DIRT, 0));
    /* A wall one cell past the target, so the entry moves EXACTLY one
     * cell this step. This pin is about what ONE displacement costs;
     * since an entry can now cover several cells a step, an open lane
     * would have it paying several ramps and the number here would
     * stop meaning what the test says it means. */
    sand_set(&s, SX + 2, ROW, STONE);
    sand_impulse_dislodge(&s, SX, ROW, DIR_RIGHT, 255, SAND_IMPULSE_SPEED_RAMP);

    sand_step(&s, 0, 1000, 0);

    TEST_ASSERT_EQUAL_INT_MESSAGE(2, s.impulse_count,
        "the single push roll here succeeds on the order of 99.6% of the "
        "time at speed 255, so the mover (entry 0) losing tracking would "
        "mean the scene itself is broken - and rung 3's transfer adds "
        "exactly one more entry (the struck dirt cell, entry 1) whenever "
        "the mover's post-drag speed clears SAND_IMPULSE_TRANSFER_MIN_"
        "SPEED, which this scene's own numbers do");
    const uint8_t expected = (uint8_t)(255 - SAND_IMPULSE_SPEED_RAMP -
                             impulse_drag_of(CELL_MAKE(MAT_DIRT, 0)));
    TEST_ASSERT_EQUAL_INT_MESSAGE(expected, s.impulse_buf[0].speed,
        "speed lost displacing a single dirt cell must equal exactly the "
        "plain ramp plus impulse_drag_of()'s own density-derived cost - "
        "this is the formula step_impulses() actually implements, not an "
        "inequality standing in for it");
}

/* THE SAME PIN, KIND_POWDER THIS TIME: a thrown grain pays the identical
 * density-scaled drag a thrown chunk does - see SAND_IMPULSE_DRAG_POWDER_
 * SHIFT's own comment in sand.h for when powders joined drag's scope (they
 * were briefly out; this comment used to say so and was stale by the time
 * this rung's own name says otherwise). impulse_count is 2 for the same
 * transfer reason as the test just above - see its own comment. */
static void test_a_thrown_powder_grain_pays_drag_displacing_dirt(void)
{
    fixture();
    sand_enable_impulses(&s, impulse_buf, W * H);

    enum { ROW = 4, SX = 1, TX = 2, DIR_RIGHT = 2 };
    for (int x = 0; x < W; x++) {
        sand_set(&s, x, ROW + 1, STONE);
    }
    sand_set(&s, SX, ROW, CELL_MAKE(MAT_SAND, 8));
    sand_set(&s, TX, ROW, CELL_MAKE(MAT_DIRT, 0));
    /* A wall one cell past the target, so the entry moves EXACTLY one
     * cell this step. This pin is about what ONE displacement costs;
     * since an entry can now cover several cells a step, an open lane
     * would have it paying several ramps and the number here would
     * stop meaning what the test says it means. */
    sand_set(&s, SX + 2, ROW, STONE);
    sand_impulse(&s, SX, ROW, DIR_RIGHT, 255);

    sand_step(&s, 0, 1000, 0);

    TEST_ASSERT_EQUAL_INT_MESSAGE(2, s.impulse_count,
        "the single push roll here succeeds on the order of 99.6% of the "
        "time at speed 255, so the mover (entry 0) losing tracking would "
        "mean the scene itself is broken - and rung 3's transfer adds "
        "exactly one more entry (the struck dirt cell, entry 1) whenever "
        "the mover's post-drag speed clears SAND_IMPULSE_TRANSFER_MIN_"
        "SPEED, which this scene's own numbers do");
    const uint8_t expected = (uint8_t)(255 - SAND_IMPULSE_SPEED_RAMP -
                             impulse_drag_of(CELL_MAKE(MAT_DIRT, 0)));
    TEST_ASSERT_EQUAL_INT_MESSAGE(expected, s.impulse_buf[0].speed,
        "a KIND_POWDER mover pays the same density-scaled drag a "
        "KIND_STATIC one does - a thrown grain is most of what a blast "
        "actually puts in the air, so leaving powders out made the whole "
        "mechanism imperceptible on the device even though it worked");
}

/* THE SCOPE PIN, now that powders are in: the boundary is LIQUIDS, and it
 * has to stay where it is. Water and acid decay geometrically
 * (SAND_SPLASH_SPEED_DECAY_SHIFT) instead of on the plain ramp, and the
 * splash and cascade features are tuned around that shape - charging them
 * drag on top would move a tuned feature nobody asked to move. A liquid
 * mover only ever displaces another liquid (can_impulse_enter(), sand.c),
 * so water into water is the whole of the case. */
static void test_a_thrown_liquid_grain_pays_no_drag_displacing_water(void)
{
    fixture();
    sand_enable_impulses(&s, impulse_buf, W * H);

    enum { ROW = 4, SX = 1, TX = 2, DIR_RIGHT = 2 };
    for (int x = 0; x < W; x++) {
        sand_set(&s, x, ROW + 1, STONE);
    }
    sand_set(&s, SX, ROW, CELL_MAKE(MAT_WATER, MASS_MAX));
    sand_set(&s, TX, ROW, CELL_MAKE(MAT_WATER, MASS_MAX));
    sand_impulse(&s, SX, ROW, DIR_RIGHT, 255);

    sand_step(&s, 0, 1000, 0);

    /* Not == 1, unlike the two pins above: this is the only one of the
     * three whose mover is a liquid, and the liquid passes run BEFORE the
     * flight pass every step - splash_displace() (sand_liquid.c) queues
     * impulses of its own, so the buffer legitimately holds more than the
     * one this test put there. Ours is still entry 0: it was queued first,
     * and step_impulses()'s compaction keeps surviving entries in order. */
    TEST_ASSERT_GREATER_OR_EQUAL_INT_MESSAGE(1, s.impulse_count,
        "the single push roll here succeeds on the order of 99.6% of the "
        "time at speed 255 - losing tracking on this one step means the "
        "scene itself is broken, not that this run was merely unlucky");
    /* The geometric decay is charged once PER CELL of travel, and the first
     * cell is charged BEFORE push_count is computed - so the budget comes
     * from the speed left after it, not from 255, and this mirrors that
     * order. Derived from the constants rather than restated: reading the
     * count off the raw 255 agreed only while the decay was gentle enough
     * for both to land on 3 cells. */
    unsigned expect_speed = 255u;
    expect_speed -= expect_speed >> SAND_SPLASH_SPEED_DECAY_SHIFT;
    const int extra_cells =
        (int)expect_speed / SAND_IMPULSE_CELLS_PER_STEP_DIVISOR;
    for (int c = 0; c < extra_cells; c++) {
        expect_speed -= expect_speed >> SAND_SPLASH_SPEED_DECAY_SHIFT;
    }
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        (int)expect_speed, s.impulse_buf[0].speed,
        "a KIND_LIQUID mover must lose exactly its own geometric decay and "
        "no drag term at all - drag stops at liquids on purpose, and this "
        "is what says so");
}

/* A THROWN GRAIN BOUNCES OFF A WALL TOO, not just a dislodged chunk - the
 * same reason powders had to be let into drag: a blast against a stone wall
 * puts far more sand in the air than it ever dislodges stone, and a grain
 * that cannot bounce stalls against the wall until its flight ages out. */
static void test_a_thrown_powder_grain_bounces_off_a_wall_instead_of_waiting(void)
{
    fixture();
    sand_enable_impulses(&s, impulse_buf, W * H);

    enum { ROW = 4, SX = 1, TX = 2, DIR_RIGHT = 2, DIR_LEFT = 6 };
    for (int x = 0; x < W; x++) {
        sand_set(&s, x, ROW + 1, STONE);
    }
    sand_set(&s, SX, ROW, CELL_MAKE(MAT_SAND, 8));
    sand_set(&s, TX, ROW, STONE);
    sand_impulse(&s, SX, ROW, DIR_RIGHT, 255);

    sand_step(&s, 0, 1000, 0);

    TEST_ASSERT_EQUAL_INT_MESSAGE(1, s.impulse_count,
        "a blocked entry waits rather than being dropped, so the grain "
        "must still be tracked whether or not it bounced");
    TEST_ASSERT_EQUAL_INT_MESSAGE(DIR_LEFT, s.impulse_buf[0].dir,
        "a thrown KIND_POWDER grain must reflect off the wall it hits, "
        "not keep its original heading and stall against it for the rest "
        "of its flight");
    TEST_ASSERT_EQUAL_INT_MESSAGE((255 - SAND_IMPULSE_SPEED_RAMP) >> 1,
        s.impulse_buf[0].speed,
        "and it must pay the head-on half of its remaining speed for the "
        "bounce, exactly as a thrown chunk does");
}

/* OPEN AIR MUST COST NOTHING BUT THE RAMP - what lets the two distance tests
 * above compare a medium against air at all. A blast throws most of its
 * grains through empty space, so drag becoming a flat per-move charge rather
 * than a density-scaled one would change every explosion in the app at once,
 * and this is what says so. */
static void test_a_thrown_chunk_displacing_nothing_loses_only_the_plain_ramp(void)
{
    fixture();
    sand_enable_impulses(&s, impulse_buf, W * H);

    enum { ROW = 4, SX = 1, DIR_RIGHT = 2 };
    for (int x = 0; x < W; x++) {
        sand_set(&s, x, ROW + 1, STONE);
    }
    sand_set(&s, SX, ROW, STONE);
    sand_impulse_dislodge(&s, SX, ROW, DIR_RIGHT, 255, SAND_IMPULSE_SPEED_RAMP);

    sand_step(&s, 0, 1000, 0);

    TEST_ASSERT_EQUAL_INT_MESSAGE(1, s.impulse_count,
        "the single push roll here succeeds on the order of 99.6% of the "
        "time at speed 255 - losing tracking on this one step means the "
        "scene itself is broken, not that this run was merely unlucky");
    /* The ramp is charged PER CELL of travel, and an entry at full speed
     * covers several - so the pin is ramp times the cells it was entitled
     * to, derived from the constants rather than restated. The claim is
     * unchanged: open air costs the ramp and nothing else, no drag, because
     * there was nothing there to displace. */
    const int air_cells = 1 + (255 - SAND_IMPULSE_SPEED_RAMP) /
                              SAND_IMPULSE_CELLS_PER_STEP_DIVISOR;
    TEST_ASSERT_EQUAL_INT_MESSAGE(255 - SAND_IMPULSE_SPEED_RAMP * air_cells,
        s.impulse_buf[0].speed,
        "a chunk moving into an EMPTY cell must lose exactly the plain "
        "ramp and not one speck more - drag is charged per cell displaced, "
        "so flight through open air has to stay byte-for-byte what it was "
        "before this rung existed");
}

/* --- Rung: multi-cell push, SAND_IMPULSE_CELLS_PER_STEP_DIVISOR -----------
 *
 * An entry advancing one cell per successful roll can never outrun the
 * ordinary gravity sweep: a horizontal throw sinks at close to 45 degrees,
 * and ejecta thrown off a powder volume can only reposition material, never
 * visibly leave it.
 *
 * Two claims, two tests: nothing changes below the divisor, several cells at
 * once above it. */

#define SUBDIV_W 40
#define SUBDIV_H 80
#define SUBDIV_STEPS 60
/* One under SAND_IMPULSE_CELLS_PER_STEP_DIVISOR - the highest speed that
 * still computes 1 + speed / SAND_IMPULSE_CELLS_PER_STEP_DIVISOR == 1. */
#define SUBDIV_SPEED (SAND_IMPULSE_CELLS_PER_STEP_DIVISOR - 1)

/* THE NEGATIVE CASE, swept rather than pinned as one arithmetic value:
 * SUBDIV_SPEED sits below both SAND_IMPULSE_BOUNCE_MIN_SPEED and
 * SAND_IMPULSE_TRANSFER_MIN_SPEED, so push_cells creeping above 1 at low
 * speed would not show in a single sample.
 *
 * The full floor keeps the gravity drift's own first candidate open for
 * every step, so horizontal movement here can only be this rung's push.
 * ramp is 0, fixing speed: the claim is the formula at one speed, not its
 * decay. */
static void test_a_sub_divisor_speed_impulse_never_moves_more_than_one_cell_a_step(void)
{
    uint8_t *cells = malloc((size_t)SUBDIV_W * SUBDIV_H);
    TEST_ASSERT_NOT_NULL_MESSAGE(cells,
        "sub-divisor sweep grid must fit in what the framebuffer leaves");
    /* Only ever one entry tracked (the lone mover, never near enough to a
     * wall or the floor to spawn a TRANSFER or CASCADE follow-up) - a small
     * stack array, not a grid-sized malloc(), same as this file's other
     * single-mover scenes. */
    impulse_t buf[8];

    sand_t g;
    memset(cells, 0, (size_t)SUBDIV_W * SUBDIV_H);
    sand_init(&g, cells, SUBDIV_W, SUBDIV_H, 4242u);
    sand_enable_impulses(&g, buf, 8);

    enum { SX = 1, SY = 2, DIR_RIGHT = 2 };
    for (int x = 0; x < SUBDIV_W; x++) {
        sand_set(&g, x, SUBDIV_H - 1, STONE);
    }
    sand_set(&g, SX, SY, STONE);
    sand_impulse_dislodge(&g, SX, SY, DIR_RIGHT, SUBDIV_SPEED, 0);

    int prev_x = SX;
    long total_moved = 0;
    for (int i = 0; i < SUBDIV_STEPS; i++) {
        sand_step(&g, 0, 1000, 0);

        TEST_ASSERT_EQUAL_INT_MESSAGE(1, g.impulse_count,
            "the mover must still be tracked every step of this sweep - it "
            "starts far from the floor and every wall, so nothing here "
            "should ever drop or settle it before SUBDIV_STEPS is up");

        const int x = (int)((unsigned)g.impulse_buf[0].index %
                            (unsigned)SUBDIV_W);
        const int dx = x - prev_x;
        char msg[192];
        snprintf(msg, sizeof msg,
                 "step %d: a speed-%d entry (one under SAND_IMPULSE_CELLS_"
                 "PER_STEP_DIVISOR) moved %d cells in this one step - it "
                 "must move at most one, exactly as every entry did before "
                 "that constant existed", i, SUBDIV_SPEED, dx);
        TEST_ASSERT_TRUE_MESSAGE(dx == 0 || dx == 1, msg);
        total_moved += dx;
        prev_x = x;
    }

    free(cells);

    TEST_ASSERT_GREATER_THAN_MESSAGE(0, total_moved,
        "the push roll must have succeeded at least once across "
        "SUBDIV_STEPS steps at speed SUBDIV_SPEED, or this sweep never "
        "actually exercised the one-cell-per-step path it claims to pin");
}

/* THE POSITIVE CASE - a full-speed push clears SEVERAL cells in a single
 * step, the entire point of SAND_IMPULSE_CELLS_PER_STEP_DIVISOR. Open air
 * ahead, so nothing is displaced and no TRANSFER entry is queued alongside
 * the mover. */
static void test_a_full_speed_static_chunk_moves_several_cells_in_one_push(void)
{
    fixture();
    sand_enable_impulses(&s, impulse_buf, W * H);

    enum { ROW = 4, SX = 1, DIR_RIGHT = 2 };
    for (int x = 0; x < W; x++) {
        sand_set(&s, x, ROW + 1, STONE);
    }
    sand_set(&s, SX, ROW, STONE);
    sand_impulse_dislodge(&s, SX, ROW, DIR_RIGHT, 255, SAND_IMPULSE_SPEED_RAMP);

    sand_step(&s, 0, 1000, 0);

    TEST_ASSERT_EQUAL_INT_MESSAGE(1, s.impulse_count,
        "the single push roll here succeeds on the order of 99.6% of the "
        "time at speed 255 - losing tracking on this one step means the "
        "scene itself is broken, not that this run was merely unlucky, and "
        "open air ahead means no TRANSFER entry should appear either");

    const int expected_cells = 1 + (255 - SAND_IMPULSE_SPEED_RAMP) /
                                   SAND_IMPULSE_CELLS_PER_STEP_DIVISOR;
    const int expected_index = ROW * W + (SX + expected_cells);
    TEST_ASSERT_EQUAL_INT_MESSAGE(expected_index, s.impulse_buf[0].index,
        "a full-speed KIND_STATIC push through open air must cover several "
        "cells in one step, not one - this is the whole point of "
        "SAND_IMPULSE_CELLS_PER_STEP_DIVISOR (sand.h)");
}

/* --- Rung 2: reflection off solids, with restitution ----------------------
 *
 * A blocked KIND_STATIC mover bounces about the blocking surface's normal;
 * restitution and the SAND_IMPULSE_BOUNCE_MIN_SPEED floor keep that from
 * reopening the bounce-in-place pathology. Water and acid keep their flat
 * 180 flip.
 *
 * The normal quantises by DOMINANCE, not sign: the arc's centre cell is
 * always covered, so signs alone pin a diagonal throw to its own reverse and
 * it could never glance. */

/* A geometric rule checked at one or two hand-picked directions is not
 * really tested: that gap is what let a sign-quantised normal through for
 * every diagonal while every axis-aligned direction kept working. All 32
 * cases, checked against the primitive directly.
 *
 * The ground truth is NOT re-derived from the formula it pins - that is
 * circular, and is how the sign-quantised bug shipped clean. If a change
 * disagrees with a row below, the change is what is wrong. */
static const int blocker_arc_table[8][4][2] = {
    /* dir 0: down. Axis-aligned - every config reverses (normal up,
     * reflect up), the flat-wall case: there is no other axis to weigh
     * a corner against. */
    { { 4, 4 }, { 4, 4 }, { 4, 4 }, { 4, 4 } },
    /* dir 1: down-right. */
    { { 5, 5 },   /* -C-: normal up-left,  reflect up-left  (head-on)   */
      { 4, 3 },   /* LC-: normal up,       reflect up-right (glances)  */
      { 6, 7 },   /* -CR: normal left,     reflect down-left           */
      { 5, 5 } }, /* LCR: normal up-left,  reflect up-left  (head-on)  */
    /* dir 2: right. Axis-aligned - always reverses. */
    { { 6, 6 }, { 6, 6 }, { 6, 6 }, { 6, 6 } },
    /* dir 3: up-right. */
    { { 7, 7 },   /* -C-: down-left,  down-left              */
      { 6, 5 },   /* LC-: left,       up-left                */
      { 0, 1 },   /* -CR: down,       down-right             */
      { 7, 7 } }, /* LCR: down-left,  down-left               */
    /* dir 4: up. Axis-aligned - always reverses. */
    { { 0, 0 }, { 0, 0 }, { 0, 0 }, { 0, 0 } },
    /* dir 5: up-left. */
    { { 1, 1 },   /* -C-: down-right, down-right              */
      { 0, 7 },   /* LC-: down,       down-left               */
      { 2, 3 },   /* -CR: right,      up-right                */
      { 1, 1 } }, /* LCR: down-right, down-right               */
    /* dir 6: left. Axis-aligned - always reverses. */
    { { 2, 2 }, { 2, 2 }, { 2, 2 }, { 2, 2 } },
    /* dir 7: down-left. */
    { { 3, 3 },   /* -C-: up-right,   up-right                */
      { 2, 1 },   /* LC-: right,      down-right              */
      { 4, 5 },   /* -CR: up,         up-left                 */
      { 3, 3 } }, /* LCR: up-right,   up-right                 */
};

static void test_blocker_normal_and_reflect_off_normal_match_the_exhaustive_arc_table(void)
{
    fixture();
    sand_clear(&s);
    const int cx = W / 2, cy = H / 2;

    for (int dir = 0; dir < 8; dir++) {
        const int *l = ring_dir(dir - 1);
        const int *c = ring_dir(dir);
        const int *r = ring_dir(dir + 1);

        for (int cfg = 0; cfg < 4; cfg++) {
            const bool has_l = (cfg & 1) != 0;
            const bool has_r = (cfg & 2) != 0;

            for (int i = 0; i < 8; i++) {
                const int *d = ring_dir(i);
                sand_set(&s, cx + d[0], cy + d[1], CELL_EMPTY);
            }
            sand_set(&s, cx + c[0], cy + c[1], STONE);   /* C: always covered */
            if (has_l) {
                sand_set(&s, cx + l[0], cy + l[1], STONE);
            }
            if (has_r) {
                sand_set(&s, cx + r[0], cy + r[1], STONE);
            }

            const int expected_normal  = blocker_arc_table[dir][cfg][0];
            const int expected_reflect = blocker_arc_table[dir][cfg][1];

            const int normal = blocker_normal(&s, cx, cy, dir);
            char msg[160];
            snprintf(msg, sizeof msg,
                     "dir=%d cfg=%d (L=%d R=%d): blocker_normal() must "
                     "match the ground-truth table exactly", dir, cfg,
                     has_l, has_r);
            TEST_ASSERT_EQUAL_INT_MESSAGE(expected_normal, normal, msg);

            const int reflect = reflect_off_normal(dir, normal);
            snprintf(msg, sizeof msg,
                     "dir=%d cfg=%d (L=%d R=%d): reflect_off_normal() must "
                     "match the ground-truth table exactly", dir, cfg,
                     has_l, has_r);
            TEST_ASSERT_EQUAL_INT_MESSAGE(expected_reflect, reflect, msg);
        }
    }
}

/* HEAD-ON REVERSAL - single step, exact arithmetic, the formula itself being
 * the claim. The floor spans the full grid width so all three of
 * blocker_normal()'s arc cells are covered and the quantised sum reduces to
 * pure "up": the head-on case. */
static void test_a_thrown_chunk_reverses_direction_bouncing_off_a_flat_floor(void)
{
    fixture();
    sand_enable_impulses(&s, impulse_buf, W * H);

    enum { ROW = 3, SX = 4, DIR_DOWN = 0, DIR_UP = 4 };
    for (int x = 0; x < W; x++) {
        sand_set(&s, x, ROW + 1, STONE);
    }
    sand_set(&s, SX, ROW, STONE);
    sand_impulse_dislodge(&s, SX, ROW, DIR_DOWN, 255, SAND_IMPULSE_SPEED_RAMP);

    sand_step(&s, 0, 1000, 0);

    TEST_ASSERT_EQUAL_INT_MESSAGE(1, s.impulse_count,
        "the single push roll here succeeds on the order of 99.6% of the "
        "time at speed 255 - losing tracking on this one step means the "
        "scene itself is broken, not that this run was merely unlucky");
    TEST_ASSERT_EQUAL_INT_MESSAGE(DIR_UP, s.impulse_buf[0].dir,
        "a chunk thrown straight down into a flat, wide floor must bounce "
        "back the exact opposite way it came, not merely lose speed while "
        "still travelling down - today it just waits there forever, same "
        "direction, until its flight ages out");
    const uint8_t expected = (uint8_t)((255 - SAND_IMPULSE_SPEED_RAMP) >> 1);
    TEST_ASSERT_EQUAL_INT_MESSAGE(expected, s.impulse_buf[0].speed,
        "a head-on bounce must cost exactly half the already-ramped speed "
        "- restitution's head-on branch - not the plain ramp alone and not "
        "the glancing branch's quarter");
}

/* GLANCING DEFLECTION - the same single-step, exact-arithmetic shape as the
 * head-on test above, on a FLAT floor rather than a corner. Two covered arc
 * cells push vertically against one horizontal, so dominance reads the normal
 * as pure "up" and a down-right travel reflects UP-RIGHT: neither reversed
 * nor still travelling down-right. This is what proves the normal is real
 * rather than a dressed-up 180 flip. */
static void test_a_thrown_chunk_deflects_off_a_flat_floor_instead_of_reversing(void)
{
    fixture();
    sand_enable_impulses(&s, impulse_buf, W * H);

    enum { ROW = 3, SX = 4, DIR_DOWN_RIGHT = 1, DIR_UP_RIGHT = 3, DIR_UP_LEFT = 5 };
    sand_set(&s, SX - 1, ROW + 1, STONE);   /* floor: down-left of the mover -
                                              * not in blocker_normal()'s arc
                                              * for this dir, only here so
                                              * gravity-drift finds no
                                              * opening and cannot be what
                                              * moves the mover */
    sand_set(&s, SX,     ROW + 1, STONE);   /* floor: down - arc flank L */
    sand_set(&s, SX + 1, ROW + 1, STONE);   /* floor: down-right - arc
                                              * centre C, and the move
                                              * target that blocks this
                                              * throw */
    /* (SX + 1, ROW), right - arc flank R - is left open on purpose: a
     * FLAT floor, not a corner, is the whole point of this scene. */
    sand_set(&s, SX, ROW, STONE);           /* the mover itself */
    sand_impulse_dislodge(&s, SX, ROW, DIR_DOWN_RIGHT, 255, SAND_IMPULSE_SPEED_RAMP);

    sand_step(&s, 0, 1000, 0);

    TEST_ASSERT_EQUAL_INT_MESSAGE(1, s.impulse_count,
        "the single push roll here succeeds on the order of 99.6% of the "
        "time at speed 255 - losing tracking on this one step means the "
        "scene itself is broken, not that this run was merely unlucky");
    TEST_ASSERT_NOT_EQUAL_MESSAGE(DIR_UP_LEFT, s.impulse_buf[0].dir,
        "must not be a flat 180 - DIR_UP_LEFT is straight back the way it "
        "came, which is what a dressed-up flip (or the water/acid rule "
        "this rung deliberately leaves alone) would produce here instead");
    TEST_ASSERT_NOT_EQUAL_MESSAGE(DIR_DOWN_RIGHT, s.impulse_buf[0].dir,
        "must not keep travelling in its original direction either - it "
        "was genuinely blocked, this is not a missed collision");
    TEST_ASSERT_EQUAL_INT_MESSAGE(DIR_UP_RIGHT, s.impulse_buf[0].dir,
        "reflecting a down-right travel about a flat floor's normal must "
        "send the chunk UP-RIGHT - the vertical push from the floor's two "
        "covered arc cells dominates the single horizontal one");
    const uint8_t ramped = (uint8_t)(255 - SAND_IMPULSE_SPEED_RAMP);
    const uint8_t expected = (uint8_t)(ramped - (ramped >> 2));
    TEST_ASSERT_EQUAL_INT_MESSAGE(expected, s.impulse_buf[0].speed,
        "a glancing (non-head-on) bounce must cost exactly a quarter of "
        "the already-ramped speed, not the head-on branch's half");
}

/* THE FLOOR GUARD against the bounce-in-place pathology: below
 * SAND_IMPULSE_BOUNCE_MIN_SPEED a blocked KIND_STATIC entry must just wait.
 * The oversized `ramp` is not a realistic caller - it is what lands the roll,
 * which reads entry.speed BEFORE the ramp, at a near-certain 255 while
 * leaving entry.speed after it well under the floor. */
static void test_a_low_speed_entry_below_the_bounce_floor_still_just_waits(void)
{
    fixture();
    sand_enable_impulses(&s, impulse_buf, W * H);

    enum { ROW = 3, SX = 4, DIR_DOWN = 0, BIG_RAMP = 230 };
    for (int x = 0; x < W; x++) {
        sand_set(&s, x, ROW + 1, STONE);
    }
    sand_set(&s, SX, ROW, STONE);
    sand_impulse_dislodge(&s, SX, ROW, DIR_DOWN, 255, BIG_RAMP);

    sand_step(&s, 0, 1000, 0);

    TEST_ASSERT_EQUAL_INT_MESSAGE(1, s.impulse_count,
        "the single push roll here succeeds on the order of 99.6% of the "
        "time at speed 255 - losing tracking on this one step means the "
        "scene itself is broken, not that this run was merely unlucky");
    const uint8_t expected_speed = (uint8_t)(255 - BIG_RAMP);
    TEST_ASSERT_TRUE_MESSAGE(expected_speed < SAND_IMPULSE_BOUNCE_MIN_SPEED,
        "fixture check: this scene must actually land under the floor, or "
        "the test proves nothing");
    TEST_ASSERT_EQUAL_INT_MESSAGE(DIR_DOWN, s.impulse_buf[0].dir,
        "below SAND_IMPULSE_BOUNCE_MIN_SPEED a blocked entry must keep "
        "waiting in its ORIGINAL direction, exactly as it did before this "
        "rung existed");
    TEST_ASSERT_EQUAL_INT_MESSAGE(expected_speed, s.impulse_buf[0].speed,
        "and must not pay any restitution either - the plain ramp only, "
        "same as the old plain wait, with no extra charge for a bounce "
        "that never happened");
}

/* GRID EDGE REFLECTS: sand_at()'s off-grid-is-STONE convention has to fold
 * into this bounce for free. Read from the BOARD, not impulse_buf.
 *
 * Deliberately airborne rather than resting on a floor at the edge: with
 * real support under it, step_impulses()'s SETTLED check drops the entry the
 * first time a push roll fails - usually the next step, since a bounce more
 * than halves speed - freezing it at EDGE_X before it can act on its new
 * direction. */
#define EDGE_W 8
#define EDGE_H 16
#define EDGE_SEEDS 8
#define EDGE_MAX_STEPS 60

static void test_a_chunk_bounces_off_the_grid_edge_instead_of_waiting_there_forever(void)
{
    uint8_t *edge_cells = malloc((size_t)EDGE_W * EDGE_H);
    TEST_ASSERT_NOT_NULL_MESSAGE(edge_cells,
        "grid-edge-bounce grid must fit in what the framebuffer leaves");
    impulse_t *buf = malloc((size_t)(EDGE_W * EDGE_H) * sizeof *buf);
    TEST_ASSERT_NOT_NULL_MESSAGE(buf,
        "grid-edge-bounce impulse queue must fit in what the framebuffer "
        "leaves");

    for (uint32_t k = 1; k <= (uint32_t)EDGE_SEEDS; k++) {
        memset(edge_cells, 0, (size_t)EDGE_W * EDGE_H);
        sand_t g;
        sand_init(&g, edge_cells, EDGE_W, EDGE_H, k);
        sand_enable_impulses(&g, buf, EDGE_W * EDGE_H);

        for (int x = 0; x < EDGE_W; x++) {
            sand_set(&g, x, EDGE_H - 1, STONE);
        }
        enum { EDGE_X = EDGE_W - 1, SY = 1, DIR_RIGHT = 2 };
        sand_set(&g, EDGE_X, SY, STONE);
        sand_impulse_dislodge(&g, EDGE_X, SY, DIR_RIGHT, 255, SAND_IMPULSE_SPEED_RAMP);

        int steps = 0;
        while (steps < EDGE_MAX_STEPS && g.impulse_count > 0) {
            sand_step(&g, 0, 1000, 0);
            steps++;
        }

        int fx = -1;
        for (int x = 0; x < EDGE_W; x++) {
            for (int y = 0; y < EDGE_H - 1; y++) {
                if (CELL_MATERIAL(sand_at(&g, x, y)) == MAT_STONE) {
                    fx = x;
                }
            }
        }
        char msg[320];
        snprintf(msg, sizeof msg,
                 "seed %u: fixture check - the thrown chunk must still be "
                 "on the board somewhere above the floor row, not vanished",
                 (unsigned)k);
        TEST_ASSERT_TRUE_MESSAGE(fx >= 0, msg);
        snprintf(msg, sizeof msg,
                 "seed %u: a chunk thrown at the grid edge must end up "
                 "STRICTLY LEFT of EDGE_X (%d) - the old behaviour left it "
                 "waiting exactly there, unmoved, for the rest of its "
                 "(linear-ramped, ~128-step) flight, because "
                 "can_impulse_enter() reads the off-grid target as STONE "
                 "and the plain wait never changes direction", (unsigned)k, EDGE_X);
        TEST_ASSERT_TRUE_MESSAGE(fx < EDGE_X, msg);
    }

    free(buf);
    free(edge_cells);
}

/* Anti-pinball: a chunk in a closed stone box must settle within a
 * bounded step count, not rattle indefinitely. BOX_MAX_STEPS=50 is under
 * 2x the measured worst case (27, across these 16 seeds) - real headroom;
 * a 300k-seed sweep found a converging tail up to 54, not chased here
 * since these seeds are fixed. In a closed box the pre-existing speed
 * ramp, not restitution, dominates settle time (open-scene tests below
 * cover restitution); this bound instead catches a broken ramp
 * decrement. */
#define BOX_SEEDS 16
#define BOX_MAX_STEPS 50

static void test_a_chunk_thrown_into_a_closed_box_comes_to_rest(void)
{
    for (uint32_t k = 1; k <= (uint32_t)BOX_SEEDS; k++) {
        memset(cells, 0, sizeof cells);
        sand_init(&s, cells, W, H, k);
        sand_enable_impulses(&s, impulse_buf, W * H);

        for (int x = 0; x < W; x++) {
            sand_set(&s, x, 0, STONE);
            sand_set(&s, x, H - 1, STONE);
        }
        for (int y = 0; y < H; y++) {
            sand_set(&s, 0, y, STONE);
            sand_set(&s, W - 1, y, STONE);
        }
        const int cx = W / 2, cy = H / 2;
        sand_set(&s, cx, cy, STONE);
        const int dir = (int)(k % 8);
        sand_impulse_dislodge(&s, cx, cy, dir, 255, SAND_IMPULSE_SPEED_RAMP);

        int steps = 0;
        while (steps < BOX_MAX_STEPS && s.impulse_count > 0) {
            sand_step(&s, 0, 1000, 0);
            steps++;
        }

        char msg[192];
        snprintf(msg, sizeof msg,
                 "seed %u, throw direction %d: still rattling after "
                 "BOX_MAX_STEPS (%d) steps in a fully closed box - this is "
                 "the bounce-in-place pathology this rung must not reopen",
                 (unsigned)k, dir, BOX_MAX_STEPS);
        TEST_ASSERT_EQUAL_INT_MESSAGE(0, s.impulse_count, msg);
    }
}

/* THE REGRESSION THAT MATTERS MOST - reflection must not turn ordinary
 * landing into bouncing.
 *
 * Both the bound and the landing row are measured: across 64 seeds this
 * scene lands on OPEN_H - 2 every time, worst-case settle 34 steps, and 52
 * is over 1.5x that. Reaching the floor in fewer steps means arriving with
 * more speed left, since the ramp sheds SAND_IMPULSE_SPEED_RAMP per step
 * whatever the distance. */
#define OPEN_W 8
#define OPEN_H 16
#define OPEN_SEEDS 8
#define OPEN_MAX_STEPS 52

static void test_a_chunk_dropped_on_flat_ground_still_settles_on_it(void)
{
    uint8_t *open_cells = malloc((size_t)OPEN_W * OPEN_H);
    TEST_ASSERT_NOT_NULL_MESSAGE(open_cells,
        "open-floor settle grid must fit in what the framebuffer leaves");
    impulse_t *buf = malloc((size_t)(OPEN_W * OPEN_H) * sizeof *buf);
    TEST_ASSERT_NOT_NULL_MESSAGE(buf,
        "open-floor settle impulse queue must fit in what the framebuffer "
        "leaves");

    for (uint32_t k = 1; k <= (uint32_t)OPEN_SEEDS; k++) {
        memset(open_cells, 0, (size_t)OPEN_W * OPEN_H);
        sand_t g;
        sand_init(&g, open_cells, OPEN_W, OPEN_H, k);
        sand_enable_impulses(&g, buf, OPEN_W * OPEN_H);

        for (int x = 0; x < OPEN_W; x++) {
            sand_set(&g, x, OPEN_H - 1, STONE);
        }
        enum { SX = OPEN_W / 2, SY = 1, DIR_DOWN = 0 };
        sand_set(&g, SX, SY, STONE);
        sand_impulse_dislodge(&g, SX, SY, DIR_DOWN, 255, SAND_IMPULSE_SPEED_RAMP);

        int steps = 0;
        while (steps < OPEN_MAX_STEPS && g.impulse_count > 0) {
            sand_step(&g, 0, 1000, 0);
            steps++;
        }

        char msg[192];
        snprintf(msg, sizeof msg,
                 "seed %u: still airborne or bouncing after OPEN_MAX_STEPS "
                 "(%d) steps of an ordinary drop onto open, flat ground",
                 (unsigned)k, OPEN_MAX_STEPS);
        TEST_ASSERT_EQUAL_INT_MESSAGE(0, g.impulse_count, msg);

        int landed_y = -1;
        for (int x = 0; x < OPEN_W; x++) {
            if (CELL_MATERIAL(sand_at(&g, x, OPEN_H - 2)) == MAT_STONE) {
                landed_y = OPEN_H - 2;
            }
        }
        snprintf(msg, sizeof msg,
                 "seed %u: the dropped chunk must rest directly on the "
                 "floor (row %d), not hover, sink through, or wander off "
                 "sideways to a different row", (unsigned)k, OPEN_H - 2);
        TEST_ASSERT_EQUAL_INT_MESSAGE(OPEN_H - 2, landed_y, msg);
    }

    free(buf);
    free(open_cells);
}

/* A BRUSH-DRAWN WALL, NOT A CLEAN ONE-CELL ONE - clean walls have hidden
 * real shape-dependent bugs in this simulation before. Direction is
 * deliberately NOT asserted: the wall's irregular shape makes the exact
 * bounce path uninteresting, while conservation and boundedness are the
 * properties that matter.
 *
 * Measured across 64 seeds: 17 to 38 steps, every seed conserving the chunk.
 * 50 is deliberately tight rather than padded - with restitution's speed
 * charge deleted the same probe rose to 60. */
#define WALL_W 24
#define WALL_H 16
#define WALL_SEEDS 16
#define WALL_MAX_STEPS 50

static void test_a_chunk_thrown_into_a_brush_drawn_wall_conserves_itself_and_settles(void)
{
    uint8_t *wall_cells = malloc((size_t)WALL_W * WALL_H);
    TEST_ASSERT_NOT_NULL_MESSAGE(wall_cells,
        "brush-wall settle grid must fit in what the framebuffer leaves");
    impulse_t *buf = malloc((size_t)(WALL_W * WALL_H) * sizeof *buf);
    TEST_ASSERT_NOT_NULL_MESSAGE(buf,
        "brush-wall settle impulse queue must fit in what the framebuffer "
        "leaves");

    for (uint32_t k = 1; k <= (uint32_t)WALL_SEEDS; k++) {
        memset(wall_cells, 0, (size_t)WALL_W * WALL_H);
        sand_t g;
        sand_init(&g, wall_cells, WALL_W, WALL_H, k);
        sand_enable_impulses(&g, buf, WALL_W * WALL_H);

        for (int x = 0; x < WALL_W; x++) {
            sand_set(&g, x, WALL_H - 1, STONE);
        }
        /* Four brush strokes, radius 2-4, centres ~3 apart - a hand-drawn
         * wall with real notches, not a flat line. */
        sand_spawn(&g, 8,  8, 3, MAT_STONE);
        sand_spawn(&g, 11, 7, 2, MAT_STONE);
        sand_spawn(&g, 14, 9, 4, MAT_STONE);
        sand_spawn(&g, 17, 6, 3, MAT_STONE);

        const int before = sand_count(&g);

        enum { SX = 2, SY = 2, DIR_RIGHT = 2 };
        sand_set(&g, SX, SY, STONE);
        sand_impulse_dislodge(&g, SX, SY, DIR_RIGHT, 255, SAND_IMPULSE_SPEED_RAMP);

        int steps = 0;
        while (steps < WALL_MAX_STEPS && g.impulse_count > 0) {
            sand_step(&g, 0, 1000, 0);
            steps++;
        }

        char msg[160];
        snprintf(msg, sizeof msg,
                 "seed %u: still rattling around the brush-drawn wall "
                 "after WALL_MAX_STEPS (%d) steps", (unsigned)k, WALL_MAX_STEPS);
        TEST_ASSERT_EQUAL_INT_MESSAGE(0, g.impulse_count, msg);

        snprintf(msg, sizeof msg,
                 "seed %u: exactly one cell (the thrown chunk) must have "
                 "been added relative to the wall alone - a bounce must "
                 "never create or destroy a cell", (unsigned)k);
        TEST_ASSERT_EQUAL_INT_MESSAGE(before + 1, sand_count(&g), msg);
    }

    free(buf);
    free(wall_cells);
}

/* THE REGRESSION GUARD, for the one scene a "stop on any obstruction" rule
 * can never move a single grain in: a blast with no adjacent empty cell
 * anywhere inside the radius, where every queued entry's first target is
 * already occupied and every entry dies on turn one. Packed on every side
 * deliberately, the only open space (row 0) nowhere near the blast - the
 * scene a device read as nothing happening. */
static void test_a_blast_in_a_packed_bed_opens_a_cavity_and_reaches_beyond_the_radius(void)
{
    fixture();
    sand_enable_impulses(&s, impulse_buf, W * H);

    for (int y = 1; y < H; y++) {
        for (int x = 0; x < W; x++) {
            sand_set(&s, x, y, SAND_FIRST_SHADE);
        }
    }

    sand_explode(&s, 4, 5, 2);

    /* A cavity exists - immediately, and independent of the flight pass,
     * the speed ramp, or a single step having run: sand_explode() fills
     * its core (here, the plus-shaped disc (4,5)/(3,5)/(5,5)/(4,4)/(4,6))
     * with fire before it ever queues an entry - see
     * SAND_EXPLODE_CORE_DIVISOR. An explosion flashes and leaves a plume; it
     * does not silently delete whatever was standing there. */
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(MAT_FIRE, CELL_MATERIAL(sand_at(&s, 4, 5)),
        "the blast's own core must flash into fire, not repositioned "
        "grains still filling the same footprint");

    for (int i = 0; i < 30; i++) {
        sand_step(&s, 0, 1000, 0);
    }

    /* Row 1 is 4 cells out, well outside radius 2, and started fully packed.
     * The core is fire, not a hole, and fire is lighter than sand (density
     * 15 against 60), so can_enter()'s ordinary density rule lets a grain
     * swap down through it - measured over 20,000 seeds, the row-1
     * disturbance appears on the first step every time.
     *
     * Which column gives up its material is not fixed: retry-until-clear can
     * walk the disturbance sideways, so the row is checked generally. */
    int empty_in_row1 = 0;
    for (int x = 2; x <= 6; x++) {
        if (sand_at(&s, x, 1) == SAND_EMPTY) {
            empty_in_row1++;
        }
    }
    TEST_ASSERT_GREATER_THAN_MESSAGE(0, empty_in_row1,
        "the disturbance must reach further than the blast radius itself - "
        "this is the exact scene that read as \"no holes\" on the panel");
}

/* THE REGRESSION THIS PINS: sand_explode()'s old row-order scan handed
 * the cap to the disc's top rows before reaching the core, undercutting a
 * real detonation. This is the one test where the cap actually binds
 * (others use buffers bigger than any disc). axis_impulse_buf[8] holds one
 * ring; row order starves DOWN/LEFT/RIGHT while UP alone succeeds, so a
 * regression fails exactly three of four - proving this tests ORDER,
 * checked against the queue directly rather than board state. */
static void test_a_blast_queues_impulses_on_every_side_of_the_centre(void)
{
    fixture();
    sand_enable_impulses(&s, axis_impulse_buf, 8);

    for (int y = 0; y < H; y++) {
        for (int x = 0; x < W; x++) {
            sand_set(&s, x, y, SAND_FIRST_SHADE);
        }
    }

    sand_explode(&s, 4, 4, 3);

    TEST_ASSERT_EQUAL_INT_MESSAGE(8, s.impulse_count,
        "the buffer holds exactly one ring's worth (8) and every one of "
        "a fully packed grid's neighbours qualifies, so all 8 slots must "
        "have been used");

    bool saw_up = false, saw_down = false, saw_left = false, saw_right = false;
    for (int i = 0; i < s.impulse_count; i++) {
        switch (s.impulse_buf[i].dir) {
        case 4: saw_up    = true; break;
        case 0: saw_down  = true; break;
        case 6: saw_left  = true; break;
        case 2: saw_right = true; break;
        default: break;
        }
    }

    TEST_ASSERT_TRUE_MESSAGE(saw_up,
        "an impulse above the centre must be queued - this direction "
        "worked even under the old bug, so its absence here would mean "
        "something else broke");
    TEST_ASSERT_TRUE_MESSAGE(saw_down,
        "an impulse below the centre must be queued - this is exactly "
        "the direction the old top-to-bottom scan order starved first, "
        "along with the entire core between it and the centre");
    TEST_ASSERT_TRUE_MESSAGE(saw_left,
        "an impulse left of the centre must be queued");
    TEST_ASSERT_TRUE_MESSAGE(saw_right,
        "an impulse right of the centre must be queued");
}

/* NOT a no-op any more, and deliberately so: an explosion in a vacuum
 * still flashes. sand_explode() fills its core with fire unconditionally
 * (see SAND_EXPLODE_CORE_DIVISOR) - occupied or already empty alike - so
 * detonating over nothing still lights the core; what stays a no-op is
 * everything BEYOND the core, since there is nothing there to queue a
 * flight entry for. This replaces the old
 * test_detonating_empty_space_is_a_no_op, which asserted exactly the
 * behaviour this round deliberately changed. */
static void test_detonating_empty_space_still_flashes_the_core(void)
{
    fixture();
    sand_enable_impulses(&s, impulse_buf, W * H);

    sand_explode(&s, 4, 4, 3);

    /* Mirrors sand_explode()'s own `core_radius` EXACTLY, clamp included,
     * not just the bare division: the unclamped form agreed only by
     * coincidence at the divisors this constant has held, and at 5 it rounds
     * a radius-3 blast's core to 0 where sand_explode() still clamps it
     * to 1. */
    const int core_radius_raw = 3 / SAND_EXPLODE_CORE_DIVISOR;
    const int core_radius = (core_radius_raw == 0 && 3 >= 2) ? 1 : core_radius_raw;
    const int core_r2 = core_radius * core_radius;
    int fire_cells = 0;
    for (int y = 0; y < H; y++) {
        for (int x = 0; x < W; x++) {
            const int dx = x - 4;
            const int dy = y - 4;
            if (dx * dx + dy * dy <= core_r2) {
                TEST_ASSERT_EQUAL_UINT8_MESSAGE(MAT_FIRE,
                    CELL_MATERIAL(sand_at(&s, x, y)),
                    "the core must flash into fire even where there was "
                    "nothing at all to convert");
                fire_cells++;
            } else {
                TEST_ASSERT_EQUAL_UINT8_MESSAGE(SAND_EMPTY, sand_at(&s, x, y),
                    "nothing beyond the core may appear from empty space - "
                    "there was nothing there to queue a flight entry for");
            }
        }
    }

    const int expected = fire_cells;
    TEST_ASSERT_EQUAL_INT_MESSAGE(expected, sand_count(&s),
        "the core's fire must be the only thing on the board");

    for (int i = 0; i < 10; i++) {
        sand_step(&s, 0, 1000, 0);
    }
    TEST_ASSERT_EQUAL_INT_MESSAGE(expected, sand_count(&s),
        "and stepping afterwards must not conjure or destroy anything "
        "either");
}

static void test_without_a_buffer_explode_does_nothing(void)
{
    fixture();
    /* sand_enable_impulses() deliberately never called. */

    sand_set(&s, 4, 4, SAND_FIRST_SHADE);
    sand_explode(&s, 4, 4, 2);
    TEST_ASSERT_NOT_EQUAL_MESSAGE(SAND_EMPTY, sand_at(&s, 4, 4),
        "with no buffer enabled, sand_explode() must be a pure no-op");

    for (int i = 0; i < 10; i++) {
        sand_step(&s, 0, 1000, 0);
    }
    /* Getting here at all is most of what this test is for - step_impulses()
     * must treat "no buffer" exactly like "nothing queued" rather than ever
     * touching a NULL impulse_buf. */
    TEST_ASSERT_EQUAL_INT_MESSAGE(1, sand_count(&s),
        "and ordinary gravity alone must still account for the one grain");
}

/* --- Rung 3, Part A: the ricochet scene (measurement, not a new feature) --
 *
 * Two stone walls across a 42-cell gap, blast at one wall, counting
 * direction changes per dislodged chunk before it settles - not a new
 * mechanism, exists to measure rung 3's tuning. See
 * docs/sand/Sand-Simulation.md for the numbers this scene reports. */
#define RICOCHET_W 44
#define RICOCHET_H 40
/* DETONATE_RADIUS_PX (50, app_sand.c) at NORMAL quality (4 px/cell):
 * (50 + 2) / 4 - the app's own DETONATE arithmetic, not a round number. */
#define RICOCHET_RADIUS 13
/* Off-grid, behind the wall, on purpose: a queued grain's direction is
 * away from the blast centre, so centring in the gap throws the wall into
 * itself. Also keeps the unconditional fire core off-grid, so nothing here
 * creates a stray MAT_FIRE entry. */
#define RICOCHET_CX (-11)
#define RICOCHET_CY (RICOCHET_H / 2)
#define RICOCHET_SEEDS 100
#define RICOCHET_MAX_STEPS 300
/* Position-tracked, not byte-tracked: MAT_STONE's variant drifts as
 * ambient temperature, so a byte id overcounts. Measured (swept 6/8/10/12/15
 * across 100 seeds): 15 is the smallest radius leaving zero unmatched
 * candidates. */
#define RICOCHET_MATCH_RADIUS 15
#define RICOCHET_MAX_TRACK 32

typedef struct {
    bool alive;
    int  x, y, dir;
    int  bounces;
} ricochet_lineage_t;

/* One seed of the ricochet scene, folded into `hist` (hist[k] += 1 for
 * every entry that changed direction at least k times before it stopped
 * being trackable, k = 0..RICOCHET_MAX_BOUNCES) and *total_entries -
 * exactly the measurement this test's own top comment reports. Position-
 * matched across steps, not byte-matched - see that comment for why. */
#define RICOCHET_MAX_BOUNCES 20
static void ricochet_measure_seed(uint8_t *cells, impulse_t *buf,
                                  uint32_t seed, long hist[RICOCHET_MAX_BOUNCES + 1],
                                  long *total_entries, int *any_unmatched_new)
{
    sand_t g;
    memset(cells, 0, (size_t)RICOCHET_W * RICOCHET_H);
    sand_init(&g, cells, RICOCHET_W, RICOCHET_H, seed);
    sand_enable_impulses(&g, buf, RICOCHET_W * RICOCHET_H);

    for (int x = 0; x < RICOCHET_W; x++) {
        sand_set(&g, x, RICOCHET_H - 1, STONE);
    }
    for (int y = 0; y < RICOCHET_H; y++) {
        sand_set(&g, 0,               y, STONE);
        sand_set(&g, RICOCHET_W - 1,  y, STONE);
    }

    sand_explode(&g, RICOCHET_CX, RICOCHET_CY, RICOCHET_RADIUS);

    /* static, not a stack array: this helper runs on the device's 3584-byte
     * main task stack too, and RICOCHET_MAX_TRACK copies of every array below
     * push a stack-local version of this function past check_stack_usage.py's
     * 1024-byte ceiling. Only one call is ever in flight. */
    static ricochet_lineage_t lin[RICOCHET_MAX_TRACK];
    int n_lin = 0;
    for (int i = 0; i < g.impulse_count && n_lin < RICOCHET_MAX_TRACK; i++) {
        if (CELL_MATERIAL(g.impulse_buf[i].cell) != MAT_STONE) {
            continue;
        }
        const int idx = g.impulse_buf[i].index;
        lin[n_lin].alive   = true;
        lin[n_lin].x       = idx % RICOCHET_W;
        lin[n_lin].y       = idx / RICOCHET_W;
        lin[n_lin].dir     = g.impulse_buf[i].dir;
        lin[n_lin].bounces = 0;
        n_lin++;
    }

    int steps = 0;
    while (g.impulse_count > 0 && steps < RICOCHET_MAX_STEPS) {
        sand_step(&g, 0, 1000, 0);
        steps++;

        static int cand_x[RICOCHET_MAX_TRACK], cand_y[RICOCHET_MAX_TRACK],
                   cand_dir[RICOCHET_MAX_TRACK];
        int n_cand = 0;
        for (int i = 0; i < g.impulse_count && n_cand < RICOCHET_MAX_TRACK; i++) {
            if (CELL_MATERIAL(g.impulse_buf[i].cell) != MAT_STONE) {
                continue;
            }
            const int idx = g.impulse_buf[i].index;
            cand_x[n_cand]   = idx % RICOCHET_W;
            cand_y[n_cand]   = idx / RICOCHET_W;
            cand_dir[n_cand] = g.impulse_buf[i].dir;
            n_cand++;
        }
        static bool cand_used[RICOCHET_MAX_TRACK];
        memset(cand_used, 0, sizeof cand_used);

        for (int li = 0; li < n_lin; li++) {
            if (!lin[li].alive) {
                continue;
            }
            int best = -1, best_d = RICOCHET_MATCH_RADIUS * 4 + 1;
            for (int ci = 0; ci < n_cand; ci++) {
                if (cand_used[ci]) {
                    continue;
                }
                const int dx = im_abs(cand_x[ci] - lin[li].x);
                const int dy = im_abs(cand_y[ci] - lin[li].y);
                if (dx > RICOCHET_MATCH_RADIUS || dy > RICOCHET_MATCH_RADIUS) {
                    continue;
                }
                const int d = dx + dy;
                if (d < best_d) {
                    best_d = d;
                    best   = ci;
                }
            }
            if (best < 0) {
                lin[li].alive = false;   /* settled, or lost to re-acquisition */
                continue;
            }
            cand_used[best] = true;
            if (cand_dir[best] != lin[li].dir) {
                lin[li].bounces++;
            }
            lin[li].x   = cand_x[best];
            lin[li].y   = cand_y[best];
            lin[li].dir = cand_dir[best];
        }
        for (int ci = 0; ci < n_cand; ci++) {
            if (!cand_used[ci]) {
                *any_unmatched_new = 1;
            }
        }
    }

    for (int li = 0; li < n_lin; li++) {
        (*total_entries)++;
        const int bc = lin[li].bounces > RICOCHET_MAX_BOUNCES
                           ? RICOCHET_MAX_BOUNCES
                           : lin[li].bounces;
        for (int k = 0; k <= bc; k++) {
            hist[k]++;
        }
    }
}

static void test_the_two_wall_explosion_scene_bounces_more_than_once_before_settling(void)
{
    uint8_t *ricochet_cells = malloc((size_t)RICOCHET_W * RICOCHET_H);
    TEST_ASSERT_NOT_NULL_MESSAGE(ricochet_cells,
        "ricochet grid must fit in what the framebuffer leaves");
    impulse_t *ricochet_buf =
        malloc((size_t)(RICOCHET_W * RICOCHET_H) * sizeof *ricochet_buf);
    TEST_ASSERT_NOT_NULL_MESSAGE(ricochet_buf,
        "ricochet impulse queue must fit in what the framebuffer leaves");

    long hist[RICOCHET_MAX_BOUNCES + 1];
    memset(hist, 0, sizeof hist);
    long total_entries = 0;
    int any_unmatched_new = 0;

    for (uint32_t seed = 1; seed <= (uint32_t)RICOCHET_SEEDS; seed++) {
        ricochet_measure_seed(ricochet_cells, ricochet_buf, seed, hist,
                              &total_entries, &any_unmatched_new);
    }

    free(ricochet_buf);
    free(ricochet_cells);

    TEST_ASSERT_GREATER_THAN_MESSAGE(0, total_entries,
        "the scene must actually have dislodged wall material across 100 "
        "seeds, or the measurement above is vacuous");
    TEST_ASSERT_FALSE_MESSAGE(any_unmatched_new,
        "a tracked entry appeared with no plausible predecessor within "
        "RICOCHET_MATCH_RADIUS - either the position-matching tracker "
        "above needs a wider radius, or something in this liquid/gas/fire-"
        "free scene queued a fresh impulse mid-run when nothing should");
    TEST_ASSERT_GREATER_THAN_MESSAGE(0, hist[1],
        "at least one tracked entry must have changed direction at least "
        "once somewhere across the 100 seeds - a ricochet, not a single "
        "stop; this is the floor of what this scene must show, not the "
        "measurement itself (see this test's own top comment for the full "
        "distribution)");
}

/* --- Rung 3, Part B: momentum transfer into a volume ----------------------
 *
 * A struck cell can pick up impulse of its own (TRANSFER, step_impulses())
 * and fly clean out of its volume. DIRT (packed, inert) gives a clean
 * zero-vs-real ejecta signal; WATER spreads on its own even with no mover
 * (measured ~1.0 cell/seed/step, linear), so its test asserts a MARGIN over
 * that baseline, not zero - 200 seeds: 387 cells outside the footprint with
 * transfer forced off, 454 with it on. */

#define EJECTA_W 40
#define EJECTA_H 20
#define EJECTA_SEEDS 200

/* Count cells of `mat` on the board that sit OUTSIDE [x0,x1) x [y0,y1) -
 * shared by both ejecta measurements below, the DIRT and the WATER scene
 * alike, so "outside the volume's own original footprint" means the exact
 * same thing in both. */
static int ejecta_count_outside(sand_t *g, int gw, int gh, uint8_t mat,
                                int x0, int x1, int y0, int y1)
{
    int n = 0;
    for (int y = 0; y < gh; y++) {
        for (int x = 0; x < gw; x++) {
            if (CELL_MATERIAL(sand_at(g, x, y)) != mat) {
                continue;
            }
            if (x >= x0 && x < x1 && y >= y0 && y < y1) {
                continue;
            }
            n++;
        }
    }
    return n;
}

/* Total cells of `mat` anywhere on the board - the conservation tests'
 * own "before" count, taken with no "inside" region at all rather than
 * reusing ejecta_count_outside() with a degenerate empty rectangle. */
static int ejecta_count_material(sand_t *g, int gw, int gh, uint8_t mat)
{
    int n = 0;
    for (int y = 0; y < gh; y++) {
        for (int x = 0; x < gw; x++) {
            if (CELL_MATERIAL(sand_at(g, x, y)) == mat) {
                n++;
            }
        }
    }
    return n;
}

/* DIRT, packed against the floor with no gap underneath: a floating bank
 * slumps under ordinary gravity and relocates itself whole before the mover
 * arrives, reading as ejecta this feature never caused.
 *
 * The mover starts INSIDE the bank's own footprint. Placed adjacent, its
 * first ordinary swap puts a bank cell in the mover's old outside cell and
 * counts as ejecta whether transfer fired or not - that swap is rung 1's
 * mechanic, not this one's. */
#define EJECTA_DIRT_W 2
#define EJECTA_DIRT_H 4
static long ejecta_dirt_total(uint8_t *cells, impulse_t *buf)
{
    long total = 0;
    for (uint32_t seed = 1; seed <= (uint32_t)EJECTA_SEEDS; seed++) {
        const int dx0 = 10, dx1 = dx0 + EJECTA_DIRT_W;
        const int dy1 = EJECTA_H - 1, dy0 = dy1 - EJECTA_DIRT_H;
        sand_t g;
        memset(cells, 0, (size_t)EJECTA_W * EJECTA_H);
        sand_init(&g, cells, EJECTA_W, EJECTA_H, seed);
        sand_enable_impulses(&g, buf, EJECTA_W * EJECTA_H);
        for (int x = 0; x < EJECTA_W; x++) {
            sand_set(&g, x, EJECTA_H - 1, STONE);
        }
        for (int y = dy0; y < dy1; y++) {
            for (int x = dx0; x < dx1; x++) {
                sand_set(&g, x, y, CELL_MAKE(MAT_DIRT, 0));
            }
        }
        sand_set(&g, dx0, dy1 - 1, STONE);
        enum { DIR_RIGHT = 2 };
        sand_impulse_dislodge(&g, dx0, dy1 - 1, DIR_RIGHT, 255,
                              SAND_IMPULSE_SPEED_RAMP);
        for (int i = 0; i < 150; i++) {
            sand_step(&g, 0, 1000, 0);
        }
        total += ejecta_count_outside(&g, EJECTA_W, EJECTA_H, MAT_DIRT,
                                      dx0, dx1, dy0, dy1);
    }
    return total;
}

/* MEASURED, 200 seeds: 50 dirt cells end up outside the bank's own 2x4
 * footprint. RED CHECK, live: with the transfer site's own gate hard-mutated
 * to `false`, this exact scene measures 0 on every seed - packed resting dirt
 * does not spread on its own, so all 50 were flung by a transfer. */
static void test_a_thrown_powder_grain_flings_dirt_out_of_the_bank_it_hits(void)
{
    uint8_t *cells = malloc((size_t)EJECTA_W * EJECTA_H);
    TEST_ASSERT_NOT_NULL_MESSAGE(cells,
        "ejecta grid must fit in what the framebuffer leaves");
    impulse_t *buf = malloc((size_t)(EJECTA_W * EJECTA_H) * sizeof *buf);
    TEST_ASSERT_NOT_NULL_MESSAGE(buf,
        "ejecta impulse queue must fit in what the framebuffer leaves");

    const long total = ejecta_dirt_total(cells, buf);

    free(buf);
    free(cells);

    TEST_ASSERT_GREATER_THAN_MESSAGE(0, total,
        "summed across 200 seeds, at least one dirt cell must end up "
        "outside the bank's own original footprint - a packed, resting "
        "dirt bank never spreads on its own (measured zero with transfer's "
        "own gate hard-mutated off), so any cell found outside it was "
        "flung there by the transfer this rung adds");
}

/* --- MATERIAL ACTUALLY FLIES, NOW - SAND_IMPULSE_CELLS_PER_STEP_DIVISOR ---
 *
 * AIRBORNE means every one of a cell's 8 neighbours is CELL_IS_EMPTY() -
 * stricter than ejecta_count_outside()'s "relocated". Before this rung
 * every displacing move advanced one cell per roll, same as gravity, so a
 * thrown chunk could never outrun it: ejecta could reposition but never
 * visibly fly. Measured, seeds 1..40: before, 26/40 seeds ever showed
 * airborne sand (peak 3 at once); after, all 40 do (peak 8). */

#define AIRBORNE_W 80
#define AIRBORNE_H 40
#define AIRBORNE_SEEDS 40
#define AIRBORNE_STEPS 60
#define AIRBORNE_SURFACE (AIRBORNE_H - 10)
#define AIRBORNE_CHUNK_W 8
#define AIRBORNE_CHUNK_X0 5
#define AIRBORNE_CHUNK_Y (AIRBORNE_SURFACE - 6)

/* Sand cells with all eight neighbours genuinely CELL_IS_EMPTY() - see
 * this section's own top comment for why that is a stricter, more
 * specific claim than "outside its original footprint". */
static int airborne_sand_count(sand_t *g)
{
    int n = 0;
    for (int y = 0; y < AIRBORNE_H; y++) {
        for (int x = 0; x < AIRBORNE_W; x++) {
            if (CELL_MATERIAL(sand_at(g, x, y)) != MAT_SAND) {
                continue;
            }
            bool all_empty = true;
            for (int dy = -1; dy <= 1 && all_empty; dy++) {
                for (int dx = -1; dx <= 1; dx++) {
                    if (dx == 0 && dy == 0) {
                        continue;
                    }
                    if (!CELL_IS_EMPTY(sand_at(g, x + dx, y + dy))) {
                        all_empty = false;
                        break;
                    }
                }
            }
            if (all_empty) {
                n++;
            }
        }
    }
    return n;
}

/* One seed: builds the scene, runs AIRBORNE_STEPS, returns the peak
 * airborne-sand count seen at any single step. */
static int airborne_bed_peak(uint8_t *cells, impulse_t *buf, uint32_t seed)
{
    sand_t g;
    memset(cells, 0, (size_t)AIRBORNE_W * AIRBORNE_H);
    sand_init(&g, cells, AIRBORNE_W, AIRBORNE_H, seed);
    sand_enable_impulses(&g, buf, AIRBORNE_W * AIRBORNE_H);

    for (int x = 0; x < AIRBORNE_W; x++) {
        sand_set(&g, x, AIRBORNE_H - 1, STONE);
    }
    for (int y = AIRBORNE_SURFACE; y < AIRBORNE_H - 1; y++) {
        for (int x = 0; x < AIRBORNE_W; x++) {
            sand_set(&g, x, y, SAND_FIRST_SHADE);
        }
    }

    enum { DIR_DOWN_RIGHT = 1 };
    for (int k = 0; k < AIRBORNE_CHUNK_W; k++) {
        sand_set(&g, AIRBORNE_CHUNK_X0 + k, AIRBORNE_CHUNK_Y, STONE);
        sand_impulse_dislodge(&g, AIRBORNE_CHUNK_X0 + k, AIRBORNE_CHUNK_Y,
                              DIR_DOWN_RIGHT, 255, SAND_IMPULSE_SPEED_RAMP);
    }

    int peak = 0;
    for (int i = 0; i < AIRBORNE_STEPS; i++) {
        sand_step(&g, 0, 1000, 0);
        const int n = airborne_sand_count(&g);
        if (n > peak) {
            peak = n;
        }
    }
    return peak;
}

static void test_a_stone_chunk_thrown_into_a_sand_bed_launches_sand_airborne(void)
{
    uint8_t *cells = malloc((size_t)AIRBORNE_W * AIRBORNE_H);
    TEST_ASSERT_NOT_NULL_MESSAGE(cells,
        "airborne-bed grid must fit in what the framebuffer leaves");
    impulse_t *buf = malloc((size_t)(AIRBORNE_W * AIRBORNE_H) * sizeof *buf);
    TEST_ASSERT_NOT_NULL_MESSAGE(buf,
        "airborne-bed impulse queue must fit in what the framebuffer leaves");

    int seeds_with_any = 0;
    int peak = 0;
    for (uint32_t seed = 1; seed <= (uint32_t)AIRBORNE_SEEDS; seed++) {
        const int p = airborne_bed_peak(cells, buf, seed);
        if (p > 0) {
            seeds_with_any++;
        }
        if (p > peak) {
            peak = p;
        }
    }

    free(buf);
    free(cells);

    char msg[420];
    snprintf(msg, sizeof msg,
             "peak airborne sand across %d seeds was %d, seeds_with_any "
             "%d/%d - this exact scene measured a peak of 3 (26/40 seeds "
             "with any) before SAND_IMPULSE_CELLS_PER_STEP_DIVISOR existed "
             "(sand.h) and a peak of 8 (40/40) after; the assertion sits "
             "strictly between those two measured figures so this test "
             "actually distinguishes the two, not merely confirms some "
             "airborne sand exists at all",
             AIRBORNE_SEEDS, peak, seeds_with_any, AIRBORNE_SEEDS);
    /* 4, not 1 - see this section's own top comment for why the OLD code
     * already clears a bar as low as 1 on this scene (a thrown chunk's own
     * gravity-drift plus rung 1/2's ordinary bounce can separate two
     * grains onto different steps even without this rung's multi-cell
     * push), so a threshold that low would pass on unfixed code and prove
     * nothing. 4 sits strictly between the measured 3 (before) and 8
     * (after). */
    TEST_ASSERT_GREATER_THAN_MESSAGE(4, peak, msg);
}

/* --- A THROWN GRAIN THAT HAS FLOWN A LONG WAY STILL EJECTS ON IMPACT ------
 *
 * The `!rolled_move` branch keeps an entry tracked only if it is still
 * airborne, and the roll's chance IS entry.speed, which the ramp erodes every
 * step - so tracking collapses long before a grain's energy does: 62% at 10
 * steps, 16% at 20, 2% at 30, 0.1% at 40, while speed at 30 is still 193,
 * nearly 3x SAND_IMPULSE_TRANSFER_MIN_SPEED. The grain flies on with no entry
 * attached, so TRANSFER never runs. */
#define EJECTA_FAR_W 45
#define EJECTA_FAR_H 70
#define EJECTA_FAR_WALL_X 35
#define EJECTA_FAR_WALL_W 6
#define EJECTA_FAR_SEEDS 60
#define EJECTA_FAR_DISTANCE 30
static long ejecta_far_thrown_powder_total(uint8_t *cells, impulse_t *buf)
{
    long total = 0;
    for (uint32_t seed = 1; seed <= (uint32_t)EJECTA_FAR_SEEDS; seed++) {
        const int mover_x = EJECTA_FAR_WALL_X - EJECTA_FAR_DISTANCE;
        sand_t g;
        memset(cells, 0, (size_t)EJECTA_FAR_W * EJECTA_FAR_H);
        sand_init(&g, cells, EJECTA_FAR_W, EJECTA_FAR_H, seed);
        sand_enable_impulses(&g, buf, EJECTA_FAR_W * EJECTA_FAR_H);
        /* A tall wall, not a bank sharing the open lane's floor: a supported
         * KIND_POWDER mover correctly settles, so a shared floor lands it
         * before it covers any distance and measures "no ejecta" even on
         * fixed code. The wall must span whatever row it has fallen to. */
        for (int y = 0; y < EJECTA_FAR_H; y++) {
            for (int x = EJECTA_FAR_WALL_X; x < EJECTA_FAR_WALL_X + EJECTA_FAR_WALL_W; x++) {
                sand_set(&g, x, y, CELL_MAKE(MAT_DIRT, 0));
            }
        }
        sand_set(&g, mover_x, 0, SAND_FIRST_SHADE);
        enum { DIR_RIGHT = 2 };
        sand_impulse(&g, mover_x, 0, DIR_RIGHT, 255);
        for (int i = 0; i < 160; i++) {
            sand_step(&g, 0, 1000, 0);
        }
        for (int y = 0; y < EJECTA_FAR_H; y++) {
            for (int x = 0; x < EJECTA_FAR_W; x++) {
                if (CELL_MATERIAL(sand_at(&g, x, y)) != MAT_DIRT) {
                    continue;
                }
                if (x >= EJECTA_FAR_WALL_X &&
                    x < EJECTA_FAR_WALL_X + EJECTA_FAR_WALL_W) {
                    continue;   /* still inside the wall's own footprint */
                }
                total++;
            }
        }
    }
    return total;
}

/* MEASURED, 60 seeds at EJECTA_FAR_DISTANCE: 60 dirt cells ejected, one per
 * seed, with the fix; 0 with KIND_POWDER hard-mutated back out of the
 * `!rolled_move` branch. 30 cells rather than the diagnosis's 50 so the wall
 * and this grid both fit the host heap arena (DP_FREE_HEAP_BYTES). Mean
 * ejecta per seed at 5/15/30/50 cells: 0.933/0.367/0.000/0.000 before,
 * 0.900/1.000/1.000/1.000 after. */
static void test_a_powder_grain_thrown_far_still_ejects_from_the_bank_it_hits(void)
{
    uint8_t *cells = malloc((size_t)EJECTA_FAR_W * EJECTA_FAR_H);
    TEST_ASSERT_NOT_NULL_MESSAGE(cells,
        "far-throw ejecta grid must fit in what the framebuffer leaves");
    impulse_t *buf = malloc((size_t)(EJECTA_FAR_W * EJECTA_FAR_H) * sizeof *buf);
    TEST_ASSERT_NOT_NULL_MESSAGE(buf,
        "far-throw ejecta impulse queue must fit in what the framebuffer "
        "leaves");

    const long total = ejecta_far_thrown_powder_total(cells, buf);

    free(buf);
    free(cells);

    TEST_ASSERT_GREATER_THAN_MESSAGE(EJECTA_FAR_SEEDS / 2, total,
        "a powder grain thrown 30 cells before it ever reaches a wall must "
        "still eject material on impact in most seeds - measured 0 across "
        "all 60 seeds with KIND_POWDER excluded from step_impulses()'s own "
        "`!rolled_move` tracking branch (the bug this test guards against: "
        "tracking died long before the grain's actual energy did), and 60 "
        "of 60 with it included");
}

/* WATER, a single row deep, resting directly on the floor: several rows of
 * standing liquid trigger splash_displace() internally as they settle into a
 * flatter shape, which has nothing to do with this rung and swamps it. A
 * single row cannot fall onto occupied liquid within itself.
 *
 * Only 4 steps: water's own sideways equalisation is dead linear in step
 * count, so the margin over that baseline that transfer adds is widest early
 * and shrinks the longer this runs. */
#define EJECTA_WATER_W 3
static long ejecta_water_total(uint8_t *cells, impulse_t *buf)
{
    long total = 0;
    for (uint32_t seed = 1; seed <= (uint32_t)EJECTA_SEEDS; seed++) {
        const int dx0 = 20, dx1 = dx0 + EJECTA_WATER_W;
        const int dy1 = EJECTA_H - 1, dy0 = dy1 - 1;
        sand_t g;
        memset(cells, 0, (size_t)EJECTA_W * EJECTA_H);
        sand_init(&g, cells, EJECTA_W, EJECTA_H, seed);
        sand_enable_impulses(&g, buf, EJECTA_W * EJECTA_H);
        for (int x = 0; x < EJECTA_W; x++) {
            sand_set(&g, x, EJECTA_H - 1, STONE);
        }
        for (int x = dx0; x < dx1; x++) {
            sand_set(&g, x, dy0, CELL_MAKE(MAT_WATER, MASS_MAX));
        }
        sand_set(&g, dx0, dy0, STONE);
        enum { DIR_RIGHT = 2 };
        sand_impulse_dislodge(&g, dx0, dy0, DIR_RIGHT, 255,
                              SAND_IMPULSE_SPEED_RAMP);
        for (int i = 0; i < 4; i++) {
            sand_step(&g, 0, 1000, 0);
        }
        total += ejecta_count_outside(&g, EJECTA_W, EJECTA_H, MAT_WATER,
                                      dx0, dx1, dy0, dy1);
    }
    return total;
}

/* MEASURED, 200 seeds: 454 water cells outside the layer's own original
 * footprint, against 387 for the SAME scene with transfer's own gate
 * hard-mutated to `false` - water's baseline sideways spread, which is why
 * the baseline is 387 rather than 0. The threshold sits between the two, so
 * silently disabling transfer for liquids drops the scene to 387 and trips
 * it. It has to be re-measured, never carried over: 350 sat above the old
 * baseline but below this one. */
#define EJECTA_WATER_THRESHOLD 420
static void test_a_thrown_powder_grain_flings_water_out_of_the_pool_it_hits(void)
{
    uint8_t *cells = malloc((size_t)EJECTA_W * EJECTA_H);
    TEST_ASSERT_NOT_NULL_MESSAGE(cells,
        "ejecta grid must fit in what the framebuffer leaves");
    impulse_t *buf = malloc((size_t)(EJECTA_W * EJECTA_H) * sizeof *buf);
    TEST_ASSERT_NOT_NULL_MESSAGE(buf,
        "ejecta impulse queue must fit in what the framebuffer leaves");

    const long total = ejecta_water_total(cells, buf);

    free(buf);
    free(cells);

    char why[512];
    snprintf(why, sizeof why,
        "summed across 200 seeds, water flung outside the pool's own "
        "original footprint must clear %d - measured 454 with transfer, "
        "387 for the same scene with transfer's own gate hard-mutated off "
        "(water's own baseline sideways spread, unrelated to this rung); "
        "this threshold sits between the two, so a regression that quietly "
        "disabled transfer for liquids falls back to the baseline and "
        "trips it", EJECTA_WATER_THRESHOLD);
    TEST_ASSERT_GREATER_THAN_MESSAGE(EJECTA_WATER_THRESHOLD, total, why);
}

/* THE DETERMINISTIC HALF OF THE SAME CLAIM, and why the threshold above is
 * allowed to be a threshold: this pins the MECHANISM in a single step, so a
 * drift in water physics cannot take both tests with it. The transfer aims
 * into the BACKWARD CONE because along the mover's own heading it drove
 * struck material deeper in instead of spraying it out.
 *
 * Membership, not equality, since rng_below(&s->rng, 3) picks the cone
 * direction; found by scanning, since splash_displace() queues entries of
 * its own first. */
static void test_a_struck_water_cell_is_handed_impulse_in_a_backward_cone_from_the_mover(void)
{
    fixture();
    sand_enable_impulses(&s, impulse_buf, W * H);

    enum { ROW = 4, SX = 1, TX = 2, DIR_RIGHT = 2 };
    for (int x = 0; x < W; x++) {
        sand_set(&s, x, ROW + 1, STONE);
    }
    sand_set(&s, SX, ROW, STONE);
    sand_set(&s, TX, ROW, CELL_MAKE(MAT_WATER, MASS_MAX));
    sand_impulse_dislodge(&s, SX, ROW, DIR_RIGHT, 255, SAND_IMPULSE_SPEED_RAMP);

    sand_step(&s, 0, 1000, 0);

    /* What the mover has left after one step: the plain ramp, plus drag for
     * the one water cell it displaced. The transfer is half of that. */
    const uint8_t mover_speed =
        (uint8_t)(255 - SAND_IMPULSE_SPEED_RAMP -
                  impulse_drag_of(CELL_MAKE(MAT_WATER, MASS_MAX)));
    const uint16_t vacated = (uint16_t)(ROW * W + SX);

    int found = -1;
    for (int i = 0; i < s.impulse_count; i++) {
        if (s.impulse_buf[i].index == vacated) {
            found = i;
            break;
        }
    }

    TEST_ASSERT_GREATER_OR_EQUAL_INT_MESSAGE(0, found,
        "the water cell the chunk shouldered aside must itself be tracked "
        "as a flying entry afterwards, at the cell the chunk vacated - "
        "that is the whole of what momentum transfer means here");
    TEST_ASSERT_EQUAL_INT_MESSAGE(MAT_WATER,
        CELL_MATERIAL(s.impulse_buf[found].cell),
        "and the entry queued there must be the WATER that was struck, not "
        "the chunk that struck it");

    const int cone[3] = { (DIR_RIGHT + 3) & 7, (DIR_RIGHT + 4) & 7,
                          (DIR_RIGHT + 5) & 7 };
    bool in_cone = false;
    for (int k = 0; k < 3; k++) {
        if (s.impulse_buf[found].dir == cone[k]) {
            in_cone = true;
            break;
        }
    }
    TEST_ASSERT_TRUE_MESSAGE(in_cone,
        "the struck cell's direction must be one of dir+3/dir+4/dir+5 - "
        "a backward cone, straight back and 45 degrees either side - not "
        "the mover's own heading: a struck cell squeezes back out through "
        "the surface it was hit through, it does not get shoved further "
        "along the exact path that just hit it");
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        (int)(((unsigned)mover_speed * SAND_IMPULSE_TRANSFER_KEEP) >> 8),
        s.impulse_buf[found].speed,
        "carrying exactly its share of the mover's own post-drag speed");
}

/* --- a KIND_POWDER mover's own distance, now that transfer is in scope
 * too - PLOW_W/H/SEEDS/STEPS, plow_build() and plow_mover_x() are rung 1's
 * own (a few hundred lines up); this reuses that exact shape and grid, not
 * a new one, following plow_total_distance()'s own idiom. That existing
 * helper hardcodes MAT_STONE as the mover, so a fresh pair - one to find a
 * DIFFERENT material's mover, one to total ITS distance - is what a
 * KIND_POWDER mover needs instead of a third copy of the whole scene. */
static int plow_mover_x_material(sand_t *g, uint8_t mat)
{
    for (int y = 0; y < PLOW_H - 1; y++) {
        for (int x = 0; x < PLOW_W; x++) {
            if (CELL_MATERIAL(sand_at(g, x, y)) == mat) {
                return x;
            }
        }
    }
    return -1;
}

static long plow_total_distance_powder(uint8_t *cells, impulse_t *buf,
                                       cell_t medium)
{
    long total = 0;
    for (uint32_t k = 1; k <= (uint32_t)PLOW_SEEDS; k++) {
        sand_t g;
        plow_build(&g, cells, buf, 4, k, MAT_SAND, medium);
        for (int i = 0; i < PLOW_STEPS; i++) {
            sand_step(&g, 0, 1000, 0);
        }
        total += plow_mover_x_material(&g, MAT_SAND) - 1;
    }
    return total;
}

static void test_a_thrown_powder_grain_travels_less_far_through_dirt_than_through_air(void)
{
    uint8_t *cells = malloc((size_t)PLOW_W * PLOW_H);
    TEST_ASSERT_NOT_NULL_MESSAGE(cells,
        "plow grid must fit in what the framebuffer leaves");
    impulse_t buf[4];

    const long air_total  = plow_total_distance_powder(cells, buf, 0);
    const long dirt_total = plow_total_distance_powder(cells, buf,
                                                       CELL_MAKE(MAT_DIRT, 0));

    free(cells);

    TEST_ASSERT_LESS_THAN_INT_MESSAGE(air_total, dirt_total,
        "a thrown KIND_POWDER grain must still travel less far, summed "
        "across the same seeds, through a bank of dirt than through open "
        "air - rung 1 already covers this for the single-step drag "
        "formula (test_a_thrown_powder_grain_pays_drag_displacing_dirt), "
        "but nothing until now measured it behaviourally, over real "
        "distance, the way test_a_thrown_chunk_travels_less_far_through_"
        "dirt_than_through_air already does for a KIND_STATIC mover - and "
        "KIND_POWDER movers are now also in scope for transfer, not just "
        "drag");
}

/* --- the budget guard: impulse_buf is shared, and it is finite ----------
 *
 * SAND_CASCADE_MAX_PER_STEP caps deferred entries queued per step, but
 * impulse_buf itself (APP_IMPULSE_MAX) is a separate fixed budget shared
 * with whatever else is using it - sand_impulse() refuses silently once
 * full. A long plow through a low-density bank could queue one transfer
 * per cell without that cap; this test proves it holds. */
#define BUDGET_W 200
#define BUDGET_H 30
/* Same open-below shape as plow_build(): a full floor under the mover's
 * row would settle it within two steps (SUPPORTED, NOT MERELY ROLLED,
 * sand.c). Measured: 27 active steps, never above 3 concurrent entries even
 * with the transfer cap unbounded - real headroom, not a number this scene
 * could ever approach. */
#define BUDGET_IMPULSE_MAX 8
#define BUDGET_STEPS 40

static void test_a_long_plow_through_a_wide_bank_never_exhausts_the_impulse_buffer(void)
{
    uint8_t *cells = malloc((size_t)BUDGET_W * BUDGET_H);
    TEST_ASSERT_NOT_NULL_MESSAGE(cells,
        "budget-guard grid must fit in what the framebuffer leaves");
    impulse_t *buf = malloc((size_t)BUDGET_IMPULSE_MAX * sizeof *buf);
    TEST_ASSERT_NOT_NULL_MESSAGE(buf,
        "budget-guard impulse queue must fit in what the framebuffer "
        "leaves");

    sand_t g;
    memset(cells, 0, (size_t)BUDGET_W * BUDGET_H);
    sand_init(&g, cells, BUDGET_W, BUDGET_H, 9u);
    sand_enable_impulses(&g, buf, BUDGET_IMPULSE_MAX);

    for (int x = 0; x < BUDGET_W; x++) {
        sand_set(&g, x, BUDGET_H - 1, STONE);
    }
    for (int x = 2; x < BUDGET_W; x++) {
        sand_set(&g, x, 0, CELL_MAKE(MAT_DIRT, 0));
    }
    sand_set(&g, 1, 0, STONE);
    enum { DIR_RIGHT = 2 };
    sand_impulse_dislodge(&g, 1, 0, DIR_RIGHT, 255, SAND_IMPULSE_SPEED_RAMP);

    int max_seen = 0;
    for (int i = 0; i < BUDGET_STEPS; i++) {
        sand_step(&g, 0, 1000, 0);
        if (g.impulse_count > max_seen) {
            max_seen = g.impulse_count;
        }
        char why[192];
        snprintf(why, sizeof why,
            "step %d: impulse_count (%d) exceeded the buffer's own "
            "capacity (%d) - sand_impulse() should refuse silently before "
            "this can ever happen, so reaching it means the per-step cap "
            "did not hold", i, g.impulse_count, BUDGET_IMPULSE_MAX);
        TEST_ASSERT_LESS_OR_EQUAL_INT_MESSAGE(BUDGET_IMPULSE_MAX,
            g.impulse_count, why);
    }

    TEST_ASSERT_GREATER_THAN_MESSAGE(0, max_seen,
        "the plow must actually have queued something across its own "
        "run, or the buffer-capacity assertion above never exercised "
        "anything");

    /* Still stepping normally afterwards - a fresh sand_impulse() must
     * still succeed once the plow's own entries have had room to clear,
     * proving this was never a permanently wedged buffer. */
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, g.impulse_count,
        "the plow's own entries must have fully cleared by the end of "
        "this run - otherwise the fresh sand_impulse() below is not "
        "actually testing an empty buffer draining, it is testing a "
        "still-busy one");
    sand_set(&g, BUDGET_W - 2, BUDGET_H - 2, CELL_MAKE(MAT_SAND, 8));
    sand_impulse(&g, BUDGET_W - 2, BUDGET_H - 2, DIR_RIGHT, 200);
    TEST_ASSERT_EQUAL_INT_MESSAGE(1, g.impulse_count,
        "a fresh sand_impulse() queued after the plow's own run must "
        "still succeed - the shared buffer must drain as entries settle, "
        "not fill up and stay full");

    free(buf);
    free(cells);
}

/* --- conservation: transfer relocates cells, it never creates or destroys
 * them --------------------------------------------------------------------
 *
 * DIRT is checked by CELL COUNT, exactly: a KIND_POWDER cell's count never
 * legitimately changes from ordinary movement. WATER is checked by MASS,
 * since a liquid's cell count is free to change as it spreads or merges
 * without a drop being lost. */
static void test_a_thrown_powder_grain_conserves_the_dirt_it_ejects(void)
{
    uint8_t *cells = malloc((size_t)EJECTA_W * EJECTA_H);
    TEST_ASSERT_NOT_NULL_MESSAGE(cells,
        "conservation grid must fit in what the framebuffer leaves");
    impulse_t *buf = malloc((size_t)(EJECTA_W * EJECTA_H) * sizeof *buf);
    TEST_ASSERT_NOT_NULL_MESSAGE(buf,
        "conservation impulse queue must fit in what the framebuffer "
        "leaves");

    const int dx0 = 10, dx1 = dx0 + EJECTA_DIRT_W;
    const int dy1 = EJECTA_H - 1, dy0 = dy1 - EJECTA_DIRT_H;
    sand_t g;
    memset(cells, 0, (size_t)EJECTA_W * EJECTA_H);
    sand_init(&g, cells, EJECTA_W, EJECTA_H, 5u);
    sand_enable_impulses(&g, buf, EJECTA_W * EJECTA_H);
    for (int x = 0; x < EJECTA_W; x++) {
        sand_set(&g, x, EJECTA_H - 1, STONE);
    }
    for (int y = dy0; y < dy1; y++) {
        for (int x = dx0; x < dx1; x++) {
            sand_set(&g, x, y, CELL_MAKE(MAT_DIRT, 0));
        }
    }
    const int dirt_count_before =
        ejecta_count_material(&g, EJECTA_W, EJECTA_H, MAT_DIRT);

    sand_set(&g, dx0, dy1 - 1, STONE);
    enum { DIR_RIGHT = 2 };
    sand_impulse_dislodge(&g, dx0, dy1 - 1, DIR_RIGHT, 255,
                          SAND_IMPULSE_SPEED_RAMP);
    for (int i = 0; i < 150; i++) {
        sand_step(&g, 0, 1000, 0);
    }

    const int dirt_count_after =
        ejecta_count_material(&g, EJECTA_W, EJECTA_H, MAT_DIRT);

    free(buf);
    free(cells);

    /* -1 for the one cell the mover itself occupied when the bank was
     * built (it was DIRT there for a moment before being overwritten with
     * STONE), matching plow_build()'s own precedent of accounting for the
     * mover's own starting cell explicitly rather than folding it into an
     * inequality. */
    TEST_ASSERT_EQUAL_INT_MESSAGE(dirt_count_before - 1, dirt_count_after,
        "total dirt cell count must be exactly conserved across the plow "
        "and every transfer it queues - transfer only ever relocates a "
        "cell already on the board, through plain sand_impulse(), so "
        "nothing here should create or destroy one");
}

static void test_a_thrown_powder_grain_conserves_the_water_mass_it_ejects(void)
{
    uint8_t *cells = malloc((size_t)EJECTA_W * EJECTA_H);
    TEST_ASSERT_NOT_NULL_MESSAGE(cells,
        "conservation grid must fit in what the framebuffer leaves");
    impulse_t *buf = malloc((size_t)(EJECTA_W * EJECTA_H) * sizeof *buf);
    TEST_ASSERT_NOT_NULL_MESSAGE(buf,
        "conservation impulse queue must fit in what the framebuffer "
        "leaves");

    const int dx0 = 20, dx1 = dx0 + EJECTA_WATER_W;
    const int dy1 = EJECTA_H - 1, dy0 = dy1 - 1;
    sand_t g;
    memset(cells, 0, (size_t)EJECTA_W * EJECTA_H);
    sand_init(&g, cells, EJECTA_W, EJECTA_H, 5u);
    sand_enable_impulses(&g, buf, EJECTA_W * EJECTA_H);
    for (int x = 0; x < EJECTA_W; x++) {
        sand_set(&g, x, EJECTA_H - 1, STONE);
    }
    for (int x = dx0; x < dx1; x++) {
        sand_set(&g, x, dy0, CELL_MAKE(MAT_WATER, MASS_MAX));
    }
    const long water_mass_before = mass_of(&g, EJECTA_W, EJECTA_H, MAT_WATER);

    sand_set(&g, dx0, dy0, STONE);
    enum { DIR_RIGHT = 2 };
    sand_impulse_dislodge(&g, dx0, dy0, DIR_RIGHT, 255, SAND_IMPULSE_SPEED_RAMP);
    for (int i = 0; i < 4; i++) {
        sand_step(&g, 0, 1000, 0);
    }

    const long water_mass_after = mass_of(&g, EJECTA_W, EJECTA_H, MAT_WATER);

    free(buf);
    free(cells);

    /* MASS_MAX, not an inequality - one full cell's worth was overwritten
     * by the mover before ever entering the pool, the same accounting
     * test_a_thrown_powder_grain_conserves_the_dirt_it_ejects uses. */
    TEST_ASSERT_EQUAL_INT_MESSAGE((int)(water_mass_before - MASS_MAX),
        (int)water_mass_after,
        "total water MASS must be exactly conserved across the plow and "
        "every transfer it queues, checked by mass rather than cell count "
        "- a liquid's own cell count is free to change as it spreads or "
        "merges without a drop being lost (mass_of()'s own comment), but "
        "the total amount must not move");
}

/* --- free fall and shaking ---------------------------------------------- */

static void test_nothing_moves_in_free_fall(void)
{
    fixture();
    sand_set(&s, 3, 0, SAND_FIRST_SHADE);

    sand_step(&s, 0, 0, 0);

    TEST_ASSERT_NOT_EQUAL_MESSAGE(SAND_EMPTY, sand_at(&s, 3, 0),
        "with no gravity there is no down, so nothing falls");
}

static void test_shaking_spreads_a_pile_sideways(void)
{
    fixture();

    /* A single tall column. Left alone it topples slowly; shaken hard it
     * should flatten, so its highest grain ends up lower. */
    for (int y = 2; y < H; y++) {
        sand_set(&s, 3, y, SAND_FIRST_SHADE);
    }

    for (int i = 0; i < 90; i++) {
        sand_step(&s, 0, 1, 255);
    }

    int highest = H;
    for (int y = 0; y < H; y++) {
        for (int x = 0; x < W; x++) {
            if (sand_at(&s, x, y) != SAND_EMPTY && y < highest) {
                highest = y;
            }
        }
    }

    TEST_ASSERT_GREATER_THAN_MESSAGE(2, highest,
        "shaking must flatten the pile, not leave the column standing");
    TEST_ASSERT_EQUAL_INT_MESSAGE(6, sand_count(&s),
        "and shaking must still conserve grains");
}

void run_sand_impulse_suite(void)
{
    RUN_TEST(test_the_disc_count_table_matches_a_direct_lattice_count);
    RUN_TEST(test_a_blast_inside_a_sealed_vessel_stays_inside_it);
    RUN_TEST(test_a_strong_close_blast_can_breach_a_wall);
    RUN_TEST(test_a_dislodged_wall_keeps_falling_even_if_its_first_push_roll_fails);
    RUN_TEST(test_a_pane_knocked_loose_is_queued_as_cullet);
    RUN_TEST(test_a_shattered_pane_flies_the_way_it_was_pushed);
    RUN_TEST(test_a_pane_that_refuses_the_push_stays_a_pane);
    RUN_TEST(test_sand_displace_alone_never_creates_fire_or_smoke);
    RUN_TEST(test_a_blast_conserves_grains);
    RUN_TEST(test_a_blast_at_the_edge_stays_in_bounds);
    RUN_TEST(test_a_dropped_entry_never_moves_someone_elses_cell);
    RUN_TEST(test_the_cap_degrades_gracefully);
    RUN_TEST(test_two_overlapping_blasts_share_the_buffer_evenly);
    RUN_TEST(test_a_blast_wakes_the_blocks_it_touches);
    RUN_TEST(test_a_flying_grain_keeps_its_outward_push_while_falling);
    RUN_TEST(test_an_energetic_static_chunk_over_a_powder_bank_now_stops_within_the_first_few_layers);
    RUN_TEST(test_a_spent_static_chunk_rests_on_a_powder_bank_instead_of_sinking_forever);
    RUN_TEST(test_a_static_chunk_thrown_far_still_stops_shallow_in_the_bed_it_hits);
    RUN_TEST(test_an_energetic_static_chunk_still_sinks_into_water_instead_of_resting_on_its_surface);
    RUN_TEST(test_a_spent_static_chunk_still_sinks_through_water_to_the_bottom);
    RUN_TEST(test_a_thrown_static_chunk_conserves_lava_mass_on_sink);
    RUN_TEST(test_a_chunk_stacked_on_an_in_flight_chunk_waits_instead_of_settling_and_both_eventually_land);
    RUN_TEST(test_an_ordinary_static_solid_still_does_not_sink_into_liquid_or_powder);
    RUN_TEST(test_a_thrown_chunk_travels_less_far_through_dirt_than_through_air);
    RUN_TEST(test_a_thrown_chunk_stops_near_the_rim_of_a_dirt_bank);
    RUN_TEST(test_a_thrown_chunk_travels_less_far_through_dirt_than_through_water);
    RUN_TEST(test_a_thrown_chunk_loses_speed_proportional_to_the_density_it_displaces);
    RUN_TEST(test_a_thrown_powder_grain_pays_drag_displacing_dirt);
    RUN_TEST(test_a_thrown_liquid_grain_pays_no_drag_displacing_water);
    RUN_TEST(test_a_thrown_powder_grain_bounces_off_a_wall_instead_of_waiting);
    RUN_TEST(test_a_thrown_chunk_displacing_nothing_loses_only_the_plain_ramp);
    RUN_TEST(test_a_sub_divisor_speed_impulse_never_moves_more_than_one_cell_a_step);
    RUN_TEST(test_a_full_speed_static_chunk_moves_several_cells_in_one_push);
    RUN_TEST(test_blocker_normal_and_reflect_off_normal_match_the_exhaustive_arc_table);
    RUN_TEST(test_a_thrown_chunk_reverses_direction_bouncing_off_a_flat_floor);
    RUN_TEST(test_a_thrown_chunk_deflects_off_a_flat_floor_instead_of_reversing);
    RUN_TEST(test_a_low_speed_entry_below_the_bounce_floor_still_just_waits);
    RUN_TEST(test_a_chunk_bounces_off_the_grid_edge_instead_of_waiting_there_forever);
    RUN_TEST(test_a_chunk_thrown_into_a_closed_box_comes_to_rest);
    RUN_TEST(test_a_chunk_dropped_on_flat_ground_still_settles_on_it);
    RUN_TEST(test_a_chunk_thrown_into_a_brush_drawn_wall_conserves_itself_and_settles);
    RUN_TEST(test_a_blast_in_a_packed_bed_opens_a_cavity_and_reaches_beyond_the_radius);
    RUN_TEST(test_a_blast_queues_impulses_on_every_side_of_the_centre);
    RUN_TEST(test_detonating_empty_space_still_flashes_the_core);
    RUN_TEST(test_without_a_buffer_explode_does_nothing);
    RUN_TEST(test_the_two_wall_explosion_scene_bounces_more_than_once_before_settling);
    RUN_TEST(test_a_thrown_powder_grain_flings_dirt_out_of_the_bank_it_hits);
    RUN_TEST(test_a_stone_chunk_thrown_into_a_sand_bed_launches_sand_airborne);
    RUN_TEST(test_a_powder_grain_thrown_far_still_ejects_from_the_bank_it_hits);
    RUN_TEST(test_a_thrown_powder_grain_flings_water_out_of_the_pool_it_hits);
    RUN_TEST(test_a_struck_water_cell_is_handed_impulse_in_a_backward_cone_from_the_mover);
    RUN_TEST(test_a_thrown_powder_grain_travels_less_far_through_dirt_than_through_air);
    RUN_TEST(test_a_long_plow_through_a_wide_bank_never_exhausts_the_impulse_buffer);
    RUN_TEST(test_a_thrown_powder_grain_conserves_the_dirt_it_ejects);
    RUN_TEST(test_a_thrown_powder_grain_conserves_the_water_mass_it_ejects);
    RUN_TEST(test_nothing_moves_in_free_fall);
    RUN_TEST(test_shaking_spreads_a_pile_sideways);
}

SUITE_REGISTER(run_sand_impulse_suite);
