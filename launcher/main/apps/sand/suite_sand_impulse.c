/*=============================================================================
 * Portable suite: the falling-sand automaton - explosions, impulse rungs, and
 * free-fall shaking.
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

/* THE ONLY CHECK THE BAKED DISC-COUNT TABLE GETS, and deliberately by a
 * different algorithm: this counts lattice points one cell at a time instead
 * of restating the closed form the table was generated from. A table checked
 * against its own generator's arithmetic proves only that the arithmetic was
 * copied. Runs past DISC_COUNT_MAX_RADIUS so the division-free walk that
 * answers out-of-range radii is covered too. */
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

/* Written FIRST, because it is what the plan calls out as forcing the actual
 * design decision: "stop when blocked" (what this implements) versus a
 * radial line-of-sight raycast from the centre (the obvious first instinct
 * the plan rejects) would both pass every other test in this file, but only
 * the first one keeps a blast that starts inside a sealed container from
 * reaching outside it.
 *
 * HALF OF A TWO-PART GUARANTEE, not the whole of it, since a wall gained a
 * density-scaled chance to be dislodged (see queue_flying_grain()'s own
 * comment in sand.c). This half is the one that still has to hold
 * absolutely: a WEAK OR DISTANT blast - radius 1, here, against a wall
 * three cells away - never reaches the wall's own candidate cells at all,
 * so the density roll never gets a turn and containment stays exact, the
 * same as it always did. See test_a_strong_close_blast_can_breach_a_wall,
 * right after this one, for the other half - proof the wall CAN give way
 * when a blast is pointed directly at it with enough force, so that
 * capability has real coverage instead of being an unverified side effect
 * of the density roll's existence. */
static void test_a_blast_inside_a_sealed_vessel_stays_inside_it(void)
{
    fixture();
    sand_enable_impulses(&s, impulse_buf, W * H);

    /* A stone box drawn on the grid itself, not merely relying on the grid's
     * own edge (sand_at()'s off-grid-is-STONE convention is exercised by the
     * separate bounds test below) - x=0/W-1 and y=0/H-1. The payload sits at
     * its centre with two or three empty cells of clearance on every side,
     * so a thrown grain has real room to fly before it ever meets the wall. */
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

    /* sand_explode() fills a small core with fire before it queues
     * anything - see SAND_EXPLODE_CORE_DIVISOR - so the centre must be fire
     * right away, with no step required to see it. Checked before
     * anything else runs, since fire is KIND_GAS and may well have risen
     * away by the time later assertions run (that is expected - see the
     * wall check below, which is what actually matters once it has), which
     * would hide a core that was never filled at all behind a coincidence. */
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
    /* Measured AFTER the explode, not before it, and bounded rather than
     * exact - see test_a_blast_conserves_grains's own comment for why: the
     * core's fire can genuinely be smothered and vanish if it never finds
     * an escape route, which this sealed vessel is a plausible place for.
     * Nothing here can ever push the count the OTHER way, though - see
     * that same comment for why an increase is a hard bug regardless of
     * geometry. */
    TEST_ASSERT_LESS_OR_EQUAL_INT_MESSAGE(after_explode, sand_count(&s),
        "outside the core, a blast only ever loses cells to fire being "
        "smothered, never creates or duplicates one, even when it is "
        "fully contained");
}

/* THE OTHER HALF - see test_a_blast_inside_a_sealed_vessel_stays_inside_it's
 * own comment just above for the first. A small box (walls at x=1/x=6 and
 * y=1/y=6, a 4x4 open interior) with genuine empty MARGIN outside its own
 * walls (x=0, x=7, y=0, y=7 - not just the grid's implicit edge, which
 * would give a dislodged cell nowhere to actually go and prove nothing),
 * detonated close to one corner at a radius that reaches every wall cell
 * at least once: centre (3,3), radius 4, so the nearest wall (x=1, two
 * cells away) sits well inside the annulus and the farthest (x=6, three
 * cells away) still does. Every wall cell this radius reaches gets its own
 * independent density roll (see queue_flying_grain()'s own comment in
 * sand.c) - stone's chance is 55-in-256 (~21%) per cell, and enough of the
 * box wall falls inside this annulus that at least one succeeding is the
 * expected outcome, not a coin flip on a single cell.
 *
 * CHECKED AGAINST THE WALL'S OWN ORIGINAL CELLS, not "did anything land
 * outside the box" - a dislodged KIND_STATIC entry now ALSO falls under
 * gravity every step it is airborne (step_impulses()'s own comment, sand.
 * c, "AIRBORNE SOLIDS FALL TOO"), same as a thrown grain of sand always
 * has. That is a real, wanted change: a chunk knocked off a wall by a
 * blast that also opens a hole right behind it can tumble back into that
 * hole instead of sailing cleanly away, exactly as a rock actually would.
 * fixture()'s own fixed seed (12345) still deterministically dislodges
 * the corner at (6,1) - confirmed by running the real, shipped code, not
 * derived by hand - but WHERE that corner ends up once gravity has a say
 * is no longer a single pinned coordinate worth asserting on its own; the
 * capability this test exists to prove is that the density roll actually
 * fires and moves real stone off the wall, which "the wall's own (6,1)
 * cell is no longer stone" demonstrates directly regardless of where the
 * dislodged material lands afterward. */
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

/* A ROLL FAILING IS NOT THE SAME AS LANDING - step_impulses()'s own
 * comment (sand.c, the block right before the outward-push roll) spells
 * out the bug this guards against: dropping a KIND_STATIC entry from
 * impulse tracking the instant its per-turn outward-push roll fails,
 * regardless of whether it has actually reached anything to rest on.
 * Since that roll is a per-turn coin flip on `speed` rather than a
 * threshold, it can fail on literally the FIRST turn - speed 0, forced
 * here, guarantees it - while the dislodged cell is still hanging over
 * open space with nothing beneath it. The only thing that ever makes a
 * KIND_STATIC cell fall at all is the unconditional gravity-drift that
 * runs "while an entry is tracked here at all" (that block's own
 * comment) - so an entry dropped while still airborne would previously
 * freeze exactly where gravity-drift happened to leave it after ONE
 * step, floating there for the rest of the run with nothing left to
 * ever revisit it.
 *
 * dir DOES NOT MATTER HERE - speed 0 means the outward-push roll can
 * never succeed, so the only thing moving this cell at all is the
 * unconditional gravity-drift, which ignores `dir` entirely. */
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

/* THE OTHER HALF OF sand_explode()'s OWN SPLIT (see sand_displace()'s own
 * comment in sand.h for the two reasons a caller might want the push
 * without the fire - correctness, for a future pure-pressure event like
 * confined steam, and cost, since fire latches `may_have_burning` and
 * keeps the whole reactions pass alive until it burns out). Wood placed
 * EXACTLY at the centre is the sharpest possible check: sand_explode()'s
 * own core fill (SAND_EXPLODE_CORE_DIVISOR) would flash that exact cell
 * into fire unconditionally, occupied or not, material or not.
 * sand_displace() has no core concept to do that with at all - the centre
 * offset is skipped for the ordinary "no direction to throw it in" reason
 * every other test in this file already relies on (see queue_outward_
 * impulse()'s own comment in sand.c), not because anything here decided
 * to spare it. If that wood is still wood, nothing tried to burn it. */
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
            /* Smoke, in this simulation, is physically the same material
             * a kettle's own steam is (see MAT_FIRE's own `.residue`
             * comment in material.c) - a burnt-out flame or a finished
             * log leaves MAT_STEAM behind, not a separate "smoke"
             * material. Nothing in this scene ever boils water either,
             * so any MAT_STEAM found here could only have come from
             * something burning out - which nothing did. */
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

    /* Measured AFTER the explode, not before it. sand_explode() clears a
     * small core outright before it queues anything - see
     * SAND_EXPLODE_CORE_DIVISOR - so the grain count genuinely, deliberately
     * drops once, right here: that is a real removal, exactly like any
     * other sand_erase() call, not something the flight pass did. The
     * invariant from here on is that nothing ELSE may touch the count -
     * outside the core, a blast only ever relocates a cell. */
    const int expected = sand_count(&s);

    /* Checked every step, not just at the end - the same idiom as
     * test_dithering_still_conserves_grains - so a bug that briefly
     * duplicates or drops a cell mid-flight cannot cancel itself out before
     * a final comparison would ever see it.
     *
     * BOUNDED, NOT EXACT - and this is the honest invariant, not a
     * loosened one. The flight pass itself only ever relocates a cell, so
     * by itself it could never move the count at all, in either
     * direction - but the core it just filled with fire is a real burning
     * cell now, sitting in a bed of ordinary sand that is denser than
     * fire (see can_enter()'s displacement rule): sand directly above a
     * fire cell sinks straight through it via the ordinary sweep, which
     * is what usually lets fire rise clear before anything can trap it -
     * but if the geometry ever leaves it with nowhere to rise TO, it gets
     * fully surrounded by strictly denser material and smothered()
     * (sand_reactions.c) puts it out, which is a real, deliberate loss of
     * one cell, not a bug. Measured, not assumed: a materially identical
     * scene detonated at a packed grid CORNER (see the bounds test below)
     * hit exactly this on 136 of 20,000 independent seeds. What can never
     * legitimately happen, from any of this, is the count going UP - and
     * that half of the invariant is checked as strictly as ever. */
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
            /* Bounded, not exact - see test_a_blast_conserves_grains's own
             * comment for why. This is in fact the scene that FIRST
             * surfaced it: a corner blast in a grid packed solid on every
             * side can leave the core's fire with nowhere to rise into at
             * all, and smothered() (sand_reactions.c) then puts it out for
             * real - measured at 136 of 20,000 seeds across these five
             * spots. An INCREASE past `expected`, from off-grid cells or
             * anywhere else, remains a hard bug regardless. */
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
    /* The centre itself, (3,4), is now fire - sand_explode() fills its
     * core before it queues anything (see SAND_EXPLODE_CORE_DIVISOR). That
     * makes (3,4) an honest burning neighbour of the stone placed below,
     * which is why this checks MATERIAL rather than the exact byte -
     * see the comment on the assertion itself. */

    /* Something else claims the exact cell the entry still names, before it
     * ever gets another turn - a reaction or a second paint stroke would do
     * this on the real board just as easily as this test does it directly. */
    sand_set(&s, 4, 4, CELL_MAKE(MAT_STONE, SAND_AMBIENT_HEAT));

    sand_step(&s, 0, 1000, 0);

    /* Material only, not the exact byte: (4,4) is now directly beside the
     * fire the core-fill just lit at (3,4), and stone banks heat from a
     * burning neighbour (reaction_t.heat_ramp) - so its own heat variant
     * legitimately drifts off SAND_AMBIENT_HEAT within this one step on
     * some seeds. That drift is real physics happening to the stone that
     * proves the entry was dropped, not a sign it was not: an entry that
     * had wrongly RE-ACQUIRED and relocated the stone would still trigger
     * it identically. What actually distinguishes "dropped" from "wrongly
     * moved" is exactly this test's other assertion below. */
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(MAT_STONE, CELL_MATERIAL(sand_at(&s, 4, 4)),
        "the entry must have been dropped - the cell it named no longer "
        "holds the grain it threw");
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(SAND_EMPTY, sand_at(&s, 5, 4),
        "relocating the stone would have moved it exactly here - a dropped "
        "entry must not move whatever now sits in its old cell instead");
}

static impulse_t tiny_impulse_buf[2];

/* Sized to exactly one full ring (see sand_explode()'s own "QUEUED BY
 * RING" comment in sand.h) around a centre with radius >= 2, so a radius-3
 * blast's ring 1 - all 8 of a centre's immediate Chebyshev neighbours,
 * corners included - fits with nothing left over. Used by
 * test_a_blast_queues_impulses_on_every_side_of_the_centre below, which
 * needs the cap to actually bind for scan order to matter at all - a
 * buffer as generous as the standard impulse_buf[] never truncates a
 * radius-3 disc in an 8x8 grid, so it could not have told ring order from
 * the old row-order bug this pins. */
static impulse_t axis_impulse_buf[8];

static void test_the_cap_degrades_gracefully(void)
{
    fixture();
    sand_enable_impulses(&s, tiny_impulse_buf, 2);

    /* Three FULL-WIDTH rows, the same shape the sleeping tests settle - not
     * a free-floating block. Full width matters here specifically: every
     * cell's sliding diagonals are either another occupied cell in the same
     * rows or off-grid (which sand_at() reads as solid too), so nothing in
     * it can move under ordinary gravity AT ALL, on any edge. A free block
     * narrower than its own support looked simpler but was not - its
     * corner cells had an open diagonal past their own footprint and slid
     * away under plain gravity regardless of the blast, which is exactly
     * the false failure this shape rules out. */
    for (int y = 5; y <= 7; y++) {
        for (int x = 0; x < W; x++) {
            sand_set(&s, x, y, SAND_FIRST_SHADE);
        }
    }

    /* Centre (3,6), radius 1: the four cardinal neighbours all qualify -
     * radius 1's true disc is exactly 5 cells (the centre plus the four
     * cardinals; the four diagonals fail the r2 <= 1 test) - see
     * exact_disc_count()'s own comment in sand.c. The buffer holds 2, so
     * queue_outward_impulse()'s accumulator THINS 5 candidates down to 2,
     * evenly rather than truncating to "however many the scan reaches
     * first" - see that function's own comment for the accumulator
     * itself. Worked by hand for this exact case (keep=2, disc_count=5,
     * scan order centre/UP/DOWN/LEFT/RIGHT - see sand_explode()'s own
     * "QUEUED BY RING" comment in sand.h for why that is the order):
     * accum starts at 0 and gains 2 per candidate that passes the r2
     * test, firing whenever it reaches 5 -
     *   centre: accum 0->2, no fire (and no direction to throw it in
     *           regardless)
     *   UP:     accum 2->4, no fire
     *   DOWN:   accum 4->6, FIRES (accum -> 1) - 1st entry queued
     *   LEFT:   accum 1->3, no fire
     *   RIGHT:  accum 3->5, FIRES (accum -> 0) - 2nd entry queued
     * DOWN and RIGHT are what a buffer of 2 affords here, not UP and DOWN
     * the way a first-come truncation would have picked - the whole point
     * of thinning by density instead of by scan position. Radius 1 also
     * means the filled core (radius 1 / SAND_EXPLODE_CORE_DIVISOR = 0) is
     * only the centre cell itself, (3,6) - none of the four cardinal
     * neighbours is it. */
    sand_explode(&s, 3, 6, 1);

    /* Checked directly against the queue itself, before a single step has
     * run, rather than inferred from where anything ends up on the board
     * afterward - DOWN and RIGHT are both structurally unable to move in
     * this scene regardless of whether they were queued (DOWN by the
     * grid's own bottom edge, RIGHT by the packed bed beside it), which
     * would make "did it move" the wrong question for THEM. "Was it
     * queued at all" is what the accumulator's own arithmetic above
     * already answers exactly. */
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

    /* Measured AFTER the explode - see test_a_blast_conserves_grains's own
     * comment on why the core's removal is real and everything past this
     * point is the invariant under test: UP specifically, since it is the
     * one candidate here with an actually open landing cell (row 4 above
     * the packed bed is empty - see this file's own comment on
     * test_a_blast_wakes_the_blocks_it_touches for the same geometry) and
     * was NOT queued, must survive completely untouched - a bug that
     * queued it anyway would show up here as a real, visible move, not
     * just a wrong index. LEFT gets the same check for good measure, even
     * though the packed bed beside it already makes "did it move" a weak
     * question on its own. */
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

/* THE BUG THIS PINS: sand_explode()'s density math used to size `keep`
 * against `s->impulse_max` - the buffer's TOTAL capacity - not against
 * how much of it a still-in-flight EARLIER explosion had already spent.
 * sand_impulse() itself never overflows regardless (its own
 * `impulse_count >= impulse_max` guard is unconditional), but the
 * DENSITY the accumulator aims for was computed as if the whole buffer
 * were free, so a second explosion fired before a first one's grains
 * finish would seed entries the buffer no longer had room for, and
 * sand_impulse() would silently refuse them one by one - reintroducing
 * the exact lopsided, one-sided truncation the accumulator exists to
 * prevent, just from CONTENTION between two blasts instead of bias
 * within one. See sand_explode()'s own comment on `room` in sand.c for
 * the fix and the full reasoning; this proves it holds.
 *
 * THE SCENE: axis_impulse_buf[8] (8 entries total, no more), grid fully
 * packed with sand so every disc candidate either explosion visits is
 * occupied and queueable - no gravity-direction or empty-cell exits to
 * complicate which candidates are "true" disc members. Two blasts, far
 * enough apart (x 0-2 versus x 3-7) that neither's core or ring ever
 * touches the other's cells, fired back to back with NO sand_step() in
 * between - the first blast's four grains are still exactly where they
 * were queued, "in flight" in every sense this bug cares about.
 *
 * FIRST BLAST: centre (1,1), radius 1 - the same shape test_the_cap_
 * degrades_gracefully already works out by hand: disc_count 5, buffer
 * fully free (room 8), so `keep` == 5 and all 4 real neighbours (the
 * centre itself has no direction to throw it in) queue cleanly.
 * impulse_count is 4 afterward - checked below as this test's own
 * precondition, not really the property under test.
 *
 * SECOND BLAST: centre (5,2), radius 2 - true disc_count 13 (see
 * exact_disc_count()'s own comment in sand.c), fired with only
 * `s->impulse_max - s->impulse_count` == 8 - 4 == 4 entries of room
 * actually left. Worked by hand against THAT room, in sand_explode()'s
 * own ring-then-edge scan order (centre, then ring 1's four diagonals-
 * then-cardinals interleaved per column, then ring 2's own edges - see
 * sand_explode()'s "QUEUED BY RING" comment in sand.h): accum starts at
 * 0, gains keep=4 per true disc member (r2 <= 4) visited, fires whenever
 * it reaches disc_count=13 -
 *   (0,0) centre:  accum  0->4,  no fire (no direction either way)
 *   (-1,-1):       accum  4->8,  no fire
 *   (-1,1):        accum  8->12, no fire
 *   (0,-1) UP:     accum 12->16, FIRES (->3) - 1st: (5,1), index 13
 *   (0,1):         accum  3->7,  no fire
 *   (1,-1):        accum  7->11, no fire
 *   (1,1):         accum 11->15, FIRES (->2) - 2nd: (6,3), index 30
 *   (-1,0):        accum  2->6,  no fire
 *   (1,0):         accum  6->10, no fire
 *   ring 2, (0,-2) - the only ring-2 top/bottom cell inside r2<=4:
 *                  accum 10->14, FIRES (->1) - 3rd: (5,0), index 5
 *   (0,2):         accum  1->5,  no fire
 *   ring 2, (-2,0) - the only ring-2 left cell inside r2<=4:
 *                  accum  5->9,  no fire
 *   ring 2, (2,0) - the only ring-2 right cell inside r2<=4:
 *                  accum  9->13, FIRES (->0) - 4th: (7,2), index 23
 * Exactly 4 fires for a `keep` of 4, ending with accum back at 0 - the
 * whole disc visited, the whole `room` spent, nothing wasted and nothing
 * overrun. Against the OLD, buggy `keep` (computed from `s->impulse_max`
 * == 8, not `room` == 4), the SAME accumulator instead fires 8 times,
 * and the first 4 of those 8 - (-1,-1) index 12, (0,-1) index 13,
 * (0,1) index 29, (1,1) index 30 - are what actually queue before
 * sand_impulse()'s own hard cap silently swallows the remaining 4: a
 * completely different, ring-1-only set that never reaches ring 2 at
 * all, which is exactly the lopsided shape this test exists to catch. */
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

/* The failure this guards against is invisible to every test above: a
 * grain thrown into open air above a settled, sleeping pile freezes there
 * forever if the block it landed in is never told it is worth examining
 * again - see Adding-a-Material.md's own lesson on exactly this shape of
 * bug. Reuses settle_with_sleeping()/assert_nothing_left_to_do() from the
 * "sleeping" section above, which already embody the right check: run the
 * same final grid again with sleeping OFF, and require that nothing at all
 * moves. */
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

    /* PAST THE DETERMINISTIC FLIGHT-TIME BOUND, not a bare 60 any more - the
     * same derivation test_the_sand_dune_scene_throws_grains_beyond_its_own_
     * footprint (this file) already uses for a KIND_STATIC entry's own full
     * decay to zero. A thrown KIND_POWDER grain can now stay TRACKED (kept,
     * not merely still falling under the ordinary sweep) for as long as its
     * speed is at or above SAND_IMPULSE_BOUNCE_MIN_SPEED - see step_impulses
     * ()'s own "!rolled_move" KIND_POWDER branch, sand.c - so a fixed low
     * step budget risks taking this test's snapshot while a grain is still
     * legitimately being nudged by its own fading impulse (a real, bounded
     * bounce near the top of the bed) rather than by anything sleeping got
     * wrong. That is exactly what the old fixed 60 hit: measured directly,
     * this scene's own s.impulse_count did not reach zero until step 111 of
     * 200 sampled, so a snapshot at 60 could still show a grain hovering
     * mid-bounce - which the fresh, impulse-free "awake" replay inside
     * assert_nothing_left_to_do() (no impulses enabled on it at all) then
     * naturally continues falling under plain gravity, misreading a real,
     * still-in-flight grain as one sleeping had wrongly frozen. */
    const int max_lifetime = (SAND_EXPLODE_INITIAL_SPEED +
                              SAND_IMPULSE_SPEED_RAMP - 1) /
                             SAND_IMPULSE_SPEED_RAMP;
    for (int i = 0; i < max_lifetime + 20; i++) {
        sand_step(&s, 0, 1000, 0);
    }

    assert_nothing_left_to_do(0, 1000);
}

/* A SECOND REGRESSION GUARD - the identity check's own blind spot, not
 * sand_explode()'s. A real device confirmed the crater above finally
 * worked, then reported the very next thing: "now there's a crater but
 * the grains don't arc."
 *
 * The cause was the identity check itself, not sand_explode(). Per step
 * the order is sweep, then liquids, then gas, then reactions, then
 * step_impulses() (see sand_step()) - so by the time this pass gets a turn,
 * ordinary gravity has already had ITS turn, on every cell, including
 * ones this list still has an eye on. A grain sitting in open air is not
 * special to the sweep: gravity moves it down one cell before step_impulses()
 * ever looks at it, the stored index it is still watching is empty, the
 * old check read that as "gone", and the entry was dropped - meaning any
 * airborne grain lost its impulse after exactly one flight move and spent
 * the rest of its fall as an ordinary grain with no further push. Lateral
 * scatter out of a crater, not an arc.
 *
 * Several independent single-grain trials, not one: SAND_EXPLODE_INITIAL_SPEED
 * currently gives roughly a 1 in 5 chance that a single grain's very
 * first speed roll fails outright, unrelated to the identity mechanism
 * entirely, which would make a one-shot version of this test flaky across
 * the seed space even though the mechanism it is actually checking is
 * completely deterministic once that roll succeeds. */
static void test_a_flying_grain_keeps_its_outward_push_while_falling(void)
{
    /* fixture() ONCE, outside the trial loop - not once per trial. Calling
     * it every trial would re-seed s->rng to 12345 each time, making every
     * "independent" trial replay the exact same random sequence from the
     * exact same starting state: not several trials at all, just one
     * trial performed several times identically. sand_clear() between
     * trials instead wipes the grid but leaves s->rng exactly where the
     * previous trial's rolls left it, so each trial's speed rolls come
     * from a genuinely different point in the one long sequence a fixed
     * seed still deterministically produces.
     *
     * Confirmed by measurement, not assumed. The fixture()-per-trial
     * version of this test, at 8 trials, measured a 2347/20000 (~11.7%)
     * failure rate across a seed sweep - every one of the 8 "independent"
     * trials was in fact identical, so a seed whose very first roll failed
     * failed all 8 at once. Fixed to sand_clear() between trials, the same
     * 8-trial version measured 1/20000. Widened to 16 trials here for
     * margin rather than trusting that single result alone. */
    fixture();
    sand_enable_impulses(&s, impulse_buf, W * H);

    /* Computed from the constants themselves, not a bare number - see the
     * comment on the assertion below for what this bounds and why it must
     * track SAND_EXPLODE_INITIAL_SPEED/SAND_IMPULSE_SPEED_RAMP rather than
     * assume whatever value they happened to hold when this was written.
     * +5 is slack for the loop itself: the ramp guarantees a roll with a
     * zero numerator by this many steps, but that roll's failure is what
     * actually drops the entry, so the step AT max_lifetime can still
     * succeed on a small nonzero `speed` one decrement shy of zero - see
     * step_impulses()'s own comment on the roll happening before the
     * decay is applied. */
    const int max_lifetime =
        (SAND_EXPLODE_INITIAL_SPEED + SAND_IMPULSE_SPEED_RAMP - 1) /
        SAND_IMPULSE_SPEED_RAMP;
    const int steps = max_lifetime + 5;

    int max_x = 1;

    for (int trial = 0; trial < 16; trial++) {
        sand_clear(&s);

        /* A single grain already in open air - nothing above, below or
         * beside it - so gravity's sweep claims it on literally every
         * step from the first, which is the worst case for the identity
         * check: if step_impulses() cannot re-acquire a grain gravity just
         * moved, this entry dies on turn one and the grain falls dead
         * straight down from then on. */
        sand_set(&s, 1, 1, SAND_FIRST_SHADE);
        /* Centre one cell to the left, radius 1: (1,1) is the RIGHT
         * neighbour, so the only way it ever gains x is the flight pass -
         * gravity here only ever pulls straight down, column 1. */
        sand_explode(&s, 0, 1, 1);

        for (int i = 0; i < steps; i++) {
            sand_step(&s, 0, 1000, 0);
        }

        /* THE CURVATURE ITSELF, not just sustained motion: `steps` is
         * past ceil(SAND_EXPLODE_INITIAL_SPEED / SAND_IMPULSE_SPEED_RAMP),
         * the fixed step count at which `speed` is guaranteed to have
         * ramped all the way to zero - see SAND_EXPLODE_INITIAL_SPEED's own
         * comment in sand.h. Once that happens, rng_chance() with a zero
         * numerator can never succeed again, so the entry MUST have been
         * dropped by now, on every single one of these 16 independent
         * rolls of the dice - not "probably", not "on average", but always,
         * regardless of what any of them individually rolled. This is
         * exactly the guarantee the old SAND_BLAST_DECAY could not make:
         * a fixed chance every turn only ever shrinks the ODDS of still
         * being airborne, it never actually bounds how long that can
         * last. Checking impulse_count directly, rather than inferring
         * "stopped flying" from where the grain ended up on the board, is
         * what makes this a check of the RAMP'S OWN TERMINATION rather
         * than a check of gravity having settled it - a grain wedged
         * against something would keep its x unchanged too, for a
         * completely different reason. */
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

/* THE CORE BUG, IN ITS SIMPLEST FORM: a thrown KIND_STATIC chunk directly
 * over a powder column. can_impulse_enter() (sand.c) only ever refuses
 * KIND_STATIC - a powder is not static, so the gravity-drift ("AIRBORNE
 * SOLIDS FALL TOO", step_impulses(), sand.c) has always been willing to
 * swap the falling chunk straight into the sand beneath it, one row per
 * step. The OLD settled check disagreed with its own drift: it asked only
 * CELL_IS_EMPTY() on that same straight-down cell, saw sand (not empty),
 * and declared the chunk settled - dropping it from impulse tracking after
 * a single row of fall, even though the very next step's drift would have
 * happily swapped it another row down had it still been tracked.
 *
 * TWO TESTS NOW, NOT ONE - device feedback for rung 4 ("a thrown chunk
 * entering a powder bank does not stop") added SAND_IMPULSE_SINK_MIN_SPEED
 * (sand.h), which narrows can_impulse_enter_gravity_ward() (sand.c, the one
 * predicate the drift and this settled check both call) so a SPENT
 * KIND_STATIC entry may only continue into a genuinely CELL_IS_EMPTY()
 * cell. The single test that used to live here drove a speed-0 entry -
 * spent from its very first step, by design, to isolate drift-vs-settle
 * disagreement from any lateral push - and asserted it reached the bottom
 * row. That assertion is now exactly backwards for a spent entry: rung 4
 * says a spent chunk over a packed bank settles right there instead of
 * swapping through the whole thing. Splitting keeps BOTH halves of the
 * history alive rather than picking one: an ENERGETIC chunk must still
 * sink all the way through - the original bug this section guards against,
 * a chunk settling after only one row - while a SPENT one must now rest
 * instead of sinking forever, this rung's own new guarantee. Neither test
 * touches the OTHER concern (drift/settle agreement on the CANDIDATE list
 * itself), which test_a_chunk_stacked_on_an_in_flight_chunk_waits_instead_
 * of_settling_and_both_eventually_land, below, still covers on its own.
 *
 * BOTH SCENES FILL THE ENTIRE ROW THE CHUNK SITS OVER, not just the one
 * column beneath it, and both call sand_clear() first rather than trusting
 * `cells` to already be blank - two things the original single test got
 * away without. Neither matters for an ENERGETIC entry, which can swap
 * through any non-static occupant regardless of what is either side of it,
 * but a SPENT entry can now be turned aside by an EMPTY diagonal exactly
 * as readily as it is stopped by a full one: a single narrow column (the
 * original scene's own shape) leaves both diagonal neighbours of a spent
 * chunk empty, and CELL_IS_EMPTY() there is true, so the chunk slides
 * sideways into open air and free-falls the rest of the way down - not
 * "no opening anywhere," which is what the spent test actually needs to
 * exercise. Measured directly: the narrow-column shape under the spent
 * test below settles at row 5, not row SETTLE_TOP_ROW, purely from that
 * sideways escape route - whatever residue `cells` happened to still hold
 * from an earlier test decided which column it escaped into. Filling every
 * column removes both problems at once.
 *
 * "STILL SINKS ALL THE WAY THROUGH" IS NO LONGER THE CLAIM, as of a later
 * rung (step_impulses(), sand.c: impulse_charge_displacement(), shared by
 * this gravity-drift and the ordinary push move) - device evidence was a
 * sand pyramid with a stone column beside it: a chunk thrown from CLOSE
 * moved sand visibly, one thrown from FAR tunnelled straight through and
 * ejected nothing, penetrating exactly as deep regardless of how far it
 * had already flown. The cause was this test's own "energetic chunk sinks
 * to the bottom" behaviour, taken to its logical extreme: the drift used
 * to charge NO drag at all, so an energetic KIND_STATIC mover swapped
 * through an entire packed bank for free, one row a step, with nothing to
 * show for it on the way. Charging the same drag (and firing the same
 * TRANSFER) at the drift site that the push site already charged is what
 * fixes the device bug, and it means an energetic chunk over a packed
 * powder bank now stops within the first few layers instead - see
 * SAND_IMPULSE_DRAG_POWDER_SHIFT's own comment in sand.h for the measured
 * push-site figure ("dirt stopping inside one cell is the point, not an
 * overshoot") this now matches from the drift side too. The test below is
 * renamed and its assertion rewritten to say so; the SPENT test right
 * after it needed no change at all, since a spent (speed 0) entry never
 * reaches this call in the first place - can_impulse_enter_gravity_ward()
 * already refuses it a non-empty candidate before any drag would be
 * charged. */
enum { SETTLE_COL = 3, SETTLE_TOP_ROW = 0 };

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

    /* MEASURED: row 2 (of 8), deterministically, for this exact scene - two
     * rows of fall before SAND's own drag (density 60, doubled twice by
     * SAND_IMPULSE_DRAG_POWDER_SHIFT) saturates the chunk's speed to 0. Not
     * pinned to that exact row here: what this test actually guards against
     * is regressing toward either extreme this rung sits between - staying
     * at the rim (drag charged so hard, or so early, that the chunk never
     * moves at all) or sinking to the bottom again (drag silently not
     * charged at the drift site, reopening the exact device bug this rung
     * fixes) - so a range comfortably inside those two failure shapes is
     * the more durable check. */
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
 * A SECOND, SEPARATE BUG from the ones above - device evidence was a sand
 * pyramid with a stone column beside it: blasted so its chunks flew into
 * the pyramid, a chunk thrown from CLOSE moved sand visibly, one thrown
 * from FAR tunnelled straight through and ejected nothing, penetrating
 * exactly as deep regardless of how far it had already flown.
 *
 * THE CAUSE: the gravity-drift move just above (the "AIRBORNE SOLIDS FALL
 * TOO" block, step_impulses(), sand.c) performs a swap that DISPLACES a
 * cell, and until impulse_charge_displacement() existed it charged no drag
 * and fired no transfer - only the push move at the bottom of that same
 * loop did either, and only when rolled_move (whose own chance IS
 * entry.speed) succeeds. A chunk near a blast arrives fast, so its push
 * rolls mostly succeed and it pays for what it hits; a chunk that has
 * flown a long way arrives slow and mostly FALLING, so nearly every
 * displacement it makes is the drift's own then-free swap - it tunnels in
 * silently, no drag to stop it and no transfer to show for it, however far
 * it had already travelled to get there.
 *
 * ISOLATED FROM THE PUSH SITE ON PURPOSE - `DIR_UP` queues the chunk's own
 * push in a direction that can only ever nudge it into open air above
 * where it started, so every displacement this scene measures is
 * attributable to the gravity-drift's own swap and nothing else; a push
 * direction that could ALSO usefully displace into the bed (down, or
 * sideways through a full-width bed) let the push site's own,
 * already-correct accounting quietly cover for a broken drift in an
 * earlier draft of this exact scene, masking precisely the bug this test
 * exists to catch.
 *
 * A FLOOR DIRECTLY UNDER THE BED, NOT SOME FIXED ROW FAR BELOW IT - a bed
 * with open air beneath it is not a bed, it is a second falling body: an
 * earlier draft rested the bed on a floor 175 rows below it and measured
 * the bed free-falling in lockstep with the chunk chasing it, at the same
 * one-row-a-step rate, never actually touched at all (a "penetration" of
 * 200+ rows into air the bed had long since vacated - a scene bug, not a
 * sand.c one).
 *
 * MEASURED, 40 seeds, FAR_SINK_DISTANCE (50 rows of open-air fall before
 * the chunk ever reaches the bed): mean penetration 6.775 rows into a
 * 20-row bed with the drift's own charge hard-mutated back out (the
 * live-mutation check this file uses elsewhere), 0.000 with this rung's
 * fix - the bed is FAR_SINK_BED_DEPTH deep specifically so a regression
 * back to the old, unconditional drift has real room to bury the chunk in
 * rather than hitting a floor that would mask it. */
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

/* THE SAME SPLIT, OVER A LIQUID - and the user's own explicit call (see
 * step_impulses()'s own comment on the gravity-drift block, sand.c): an
 * ENERGETIC chunk sinks into water (and lava, its own dedicated test
 * below) rather than resting on the surface the way an ordinary,
 * never-thrown KIND_STATIC cell would (see
 * test_an_ordinary_static_solid_still_does_not_sink_into_liquid_or_powder,
 * this file, for that other half of the distinction) - while a SPENT one
 * now rests on the surface instead, the KNOWN, CHOSEN TRADE named in
 * SAND_IMPULSE_SINK_MIN_SPEED's own comment (sand.h): this floor is not
 * kind-aware, so a spent chunk stalls on a liquid exactly as it does on a
 * powder, not only several cells down as an energetic one would. */
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

/* THE INVARIANT GUARD. Deleting the drift's liquid exclusion is only safe
 * because the move it gates is a SWAP, not an overwrite - four lines below
 * the can_impulse_enter() check in step_impulses(), `gdisplaced` is read
 * out of the target cell and written back into the entry's OLD cell, the
 * same move_to() trick every other kind here already relies on. A lava
 * cell a thrown chunk swaps into does not stop existing, it changes which
 * cell it occupies. This test is what turns that claim from an argument
 * into a measurement.
 *
 * MASS, NOT CELL COUNT, IS THE EXACT INVARIANT - see mass_of()'s own
 * comment for why: a liquid's cell count is free to change even under
 * ordinary, unrelated movement (one full cell splitting into several
 * shallower ones as it spreads is not a leak), while the sum of every
 * lava cell's own CELL_VARIANT is not. MEASURED, not assumed: an early
 * version of this test asserted the cell count unchanged too, and it
 * failed - 48 before, 55 after - which looked at first like exactly the
 * duplication this test exists to catch. It was not. Instrumenting the
 * scene (a full material-by-material dump, kept out of the final test)
 * showed mass staying exactly 720 throughout, and the extra 7 cells were
 * two genuinely unrelated, pre-existing mechanics, neither one anything
 * to do with step_impulses(): the swap itself displaces lava upward one
 * cell at a time as the chunk sinks, ordinary sand_step_liquids()
 * equalisation then spreads that displaced lava sideways across the row
 * (see mass_of()'s own comment on this being expected for ANY liquid,
 * lava included), and once spread that lava's own top surface sits
 * exposed to open air for the first time - which is exactly what lets
 * reaction_t.flare (sand_reactions.c, "the flame licking up off it") roll
 * and place a handful of fresh MAT_FIRE cells above it, cells that were
 * never lava mass in the first place. Cell count is therefore checked
 * only as "did not go DOWN" below - a genuine deletion would still be
 * caught, a legitimate spread or an unrelated flare above the exposed
 * surface would not be mistaken for one. The row assertion first confirms
 * the scene actually exercises a sink (not "stayed on the surface because
 * the exclusion is still there," which would make the conservation
 * assertions vacuous).
 *
 * SPEED 255, NOT 0, SINCE RUNG 4 - this scene originally drove a speed-0
 * entry, harmless before SAND_IMPULSE_SINK_MIN_SPEED existed because the
 * gravity-drift was unconditional regardless of speed. It is not harmless
 * now: a spent entry (speed 0, below that floor) only continues into a
 * genuinely CELL_IS_EMPTY() cell (can_impulse_enter_gravity_ward(),
 * sand.c), and this scene's own pool has no empty cell anywhere for it to
 * find, so a speed-0 chunk here would settle at row 0 and never exercise
 * the sink this test measures at all. 255 keeps the chunk ENERGETIC for
 * the whole 8-row fall (the plain ramp only costs 2 speed a step), which
 * is what the swap-conservation claim below was always actually about -
 * see SAND_IMPULSE_SINK_MIN_SPEED's own comment (sand.h) for why an
 * energetic chunk still sinks into a liquid exactly as before. */
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

/* A SUPPORT THAT IS ITSELF IN FLIGHT MEANS WAIT, NOT SETTLE. Two KIND_
 * STATIC chunks, A directly above B, both dislodged into impulse tracking
 * this same step - A queued first, so step_impulses()'s own loop gives A
 * its turn before B gets a chance to move out from under it. A's three
 * gravity-ward candidates are ALL blocked: straight down is B (still
 * sitting exactly where it started, its own turn not yet taken), and both
 * diagonals are walled off with WOOD (KIND_STATIC, but never dislodged -
 * an ordinary immovable wall, not a competing impulse entry) so A cannot
 * simply slide around the problem. Naively, "no opening this step" reads
 * as settled - but B is not a wall, it is a chunk that is about to move
 * out of the way in this very same step (open floor below it, columns 4-7
 * clear). Settling A right there would freeze it stacked in mid-air over
 * B forever, since nothing else in this engine ever revisits a settled
 * KIND_STATIC cell.
 *
 * WOOD, not more STONE, for the side walls - so a stray STONE cell can
 * never be mistaken for one of the two chunks this test is actually
 * tracking when scanning the board afterward. */
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
     * An earlier version of this test asserted the upper chunk lands
     * DIRECTLY ON TOP of the lower one (same column, one row up) and it
     * failed: measurement (a full-board dump, not left in this test)
     * showed both chunks side by side in the bottom row instead, columns
     * 2 and 3. That is not a bug - once B has landed at (COL, H-1), A's
     * OWN straight-down candidate at that same cell is blocked (B is
     * KIND_STATIC and, once settled, no longer a tracked entry for
     * impulse_index_still_tracked() to find), so the SAME shared
     * candidate list this whole feature is built on tries the next
     * candidate in line - the diagonal slide - exactly as it would for
     * any other obstacle, and that cell is open. Sliding around a landed
     * neighbour instead of freezing above it is the correct behaviour of
     * the shared predicate, not a special case; this test only ever
     * existed to prove neither chunk freezes mid-air, not to pin an exact
     * final column, so it checks exactly that. */
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

/* AN ORDINARY, NEVER-THROWN KIND_STATIC CELL gets none of the above - no
 * sand_impulse()/sand_impulse_dislodge() call on it here at all, so it is
 * never added to s->impulse_buf and step_impulses() never looks at it. The
 * main sweep (sand_step(), sand.c) skips KIND_STATIC outright by design -
 * that is what makes stone or glass hold its shape - so a static cell
 * placed directly on top of a liquid or a powder, with no impulse behind
 * it, has no mechanism in this engine that could ever move it at all, let
 * alone sink it. This is the test that fails if a future change moves the
 * new can_impulse_enter()-based sinking rule out of step_impulses() and
 * into ordinary movement instead - see the drift block's own comment
 * (step_impulses(), sand.c) for why that generalisation is explicitly not
 * what this feature is. */
/* BOTH SWEEP PARITIES, because one proves nothing. sand_step_liquids()
 * flips s->liquid_flip every step and cross-flow takes its row order from
 * it, so a scene reaches both orders on a real board; a fixture starting at
 * the default only ever exercises one. Run with `flip` both ways. */
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
 * step_impulses() (sand.c) now charges extra `speed` loss at the move site,
 * per non-empty cell a KIND_STATIC mover displaces, proportional to that
 * cell's own `density` (impulse_drag_of(), sand_priv.h) - a thrown wall
 * chunk should cross a bank of dirt slower than it crosses open air, not
 * teleport through it at the same rate. KIND_STATIC only; see SAND_IMPULSE_
 * DRAG_POWDER_SHIFT's own comment (sand.h) for why liquid and powder
 * movers are out of scope for this rung.
 *
 * TWO SHAPES OF TEST, NOT ONE. The two below actually measure horizontal
 * distance travelled - the thing the feature exists to change, and the
 * thing a name like "travels less far" has to mean, following the
 * interfacial-drag idiom above (test_water_does_not_drill_into_oil_when_
 * tilted): averaged over PLOW_SEEDS seeds, because "a single run of a
 * chaotic scene is not evidence" is exactly as true here as it is there.
 * The two after that are single-step, single-displaced-cell pins on the
 * exact arithmetic (speed == 255 - SAND_IMPULSE_SPEED_RAMP - density) -
 * asserting on s.impulse_buf[0].speed directly
 * is honest ONLY there, because for that one test the formula itself IS
 * the claim, not a stand-in for it.
 *
 * A FIRST VERSION OF THE DISTANCE TESTS FLOORED THE FLIGHT ROW DIRECTLY
 * BENEATH ITSELF, which was wrong twice over. First: with no vertical
 * opening ever available, a single missed push roll - a roughly 1-in-256
 * event every step, `rolled_move`, step_impulses() - ended the flight
 * immediately instead of costing one step's worth of distance the way it
 * would in the open field sand_explode() actually throws into, turning
 * "distance travelled" into the stopping point of a race against a
 * shrinking success chance rather than a measurement of drag. Second, and
 * worse: a wide packed medium is not inert while it waits to be hit -
 * try_slide_impl() (sand_priv.h) draws one s->rng number for EVERY resting
 * powder cell EVERY step regardless of whether it moves, so a wide bank of
 * dirt measurably shifts which random numbers the impulse's own roll
 * consumes relative to open air. Confirmed by running that version against
 * the UNMODIFIED file: dirt already "won" against water there, averaged
 * over seeds and all, with no drag mechanism to explain it.
 *
 * THE FIX IS OPEN SPACE, NOT A FLOOR UNDER THE FLIGHT ROW. `medium` fills
 * ONLY row 0, the row the chunk is thrown along; every row below it is
 * empty, down to a single STONE floor at the very bottom of a grid tall
 * enough to give real room to fall. A missed push roll now finds a real
 * opening on the very next gravity-drift check (`has_opening`,
 * step_impulses()) and simply keeps falling instead of settling - exactly
 * the "still airborne" path the code already has for this. The medium
 * itself is not inert either: with nothing under IT, ordinary gravity
 * (the ordinary sweep, which runs before step_impulses() every step) pulls
 * unsupported dirt or water down into the open space below just as the
 * falling chunk arrives there, so the chunk keeps encountering displaceable
 * medium for several steps of its fall rather than only the one row it
 * started in - confirmed by tracing a single seed step by step: the chunk
 * left row 0 after its very first move, and the cell it swapped into on
 * EVERY later step was already the falling medium material, not open air.
 * That is a real, mechanism-driven difference between dirt and water
 * (materials that fall) and open air (nothing to fall), not an artefact of
 * scene geometry.
 *
 * PLOW_STEPS IS SHORT ON PURPOSE - long enough to cover the fall to the
 * floor (PLOW_H - 1 rows) plus a few, not long enough to let post-landing
 * wandering (still tracked, still rolling sideways through open ground with
 * nothing left to displace) dilute the signal. Measured both ways, back
 * when this rung first shipped at SAND_IMPULSE_DRAG_SHIFT 2: at 80 steps the
 * three averages over PLOW_SEEDS seeds sat at roughly 14.2 (air) / 8.9
 * (water) / 7.3 (dirt) cells and the unmodified file's own equivalent
 * numbers already clustered within a few percent of each other by chance,
 * which was the right shape pre-fix but left too little room for genuine
 * noise on either side of the assertions. At 15 steps the unmodified file's
 * three averages sat at 12.5 / 12.0 / 12.6 - indistinguishable, exactly as
 * they must be with no drag mechanism to tell dirt from water from air -
 * while the fixed file's separated out to 12.5 / 8.8 / 7.3, the same
 * mechanism-driven ordering, just measured where it was least diluted.
 *
 * RESHARPENED, NOT JUST RECONFIRMED, AT DRAG_SHIFT 0 - device feedback
 * (rung 4) said even that separation was not enough: a chunk still crossed
 * a real bank of dirt too far to read as an impact. At the new figure,
 * this same 15-step measurement now sits at 12.5 (air) / 3.6 (dirt) / 5.2
 * (water) - roughly the "first few layers" and "sink partway into a pool"
 * the device report asked for, not the old 7.3 / 8.8 that merely ordered
 * dirt behind water correctly. PLOW_STEPS itself did not need to change -
 * the separation only got sharper. */
#define PLOW_W 40
#define PLOW_H 10
#define PLOW_SEEDS 32
#define PLOW_STEPS 15

/* `medium` fills row 0 (unless empty) for `mover`, thrown right from
 * (1, 0), to fly and fall through; a single STONE floor spans the very
 * bottom row. STATIC movers go through sand_impulse_dislodge()
 * (unconditional, matching this suite's other thrown-chunk tests);
 * anything else through plain sand_impulse(), which a KIND_STATIC cell
 * would just refuse. `cells` is memset here rather than by each caller,
 * matching test_water_does_not_drill_into_oil_when_tilted's own
 * drag_cells above - a fresh sand_init() alone never clears the array.
 *
 * A DIRT medium must be built with a DRY variant (0 .. SOIL_DRY_TONES-1,
 * material.h) - the callers below use 0, the same tone every other dirt
 * scene in this file uses. Anything from SOIL_DRY_TONES up is WET, and wet
 * soil carries moisture that percolates and dries while the measurement is
 * running: passing variant 8 here measures a medium that is quietly
 * changing state under the chunk, and moves dirt's own averaged distance
 * from 7.25 cells to 6.66. Drag itself reads per-material density and does
 * not care, so nothing fails - the scene just stops being the one the test
 * name claims. */
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

/* THE ENERGY EXIT'S OWN PIN (finding 1, bd esp32c6-w2h - an adversarial
 * architecture review of step_impulses()). push_count (the DISTANCE
 * budget) is computed once, from the speed the mover carried BEFORE this
 * step's own drag ever charges - so a chunk that pays most of its speed on
 * hop 0 still had every remaining hop of that budget to spend, drag or no
 * drag, because nothing inside the loop ever asked whether there was any
 * energy left to justify another one. Measured before this rung existed
 * (32 seeds, plow_total_distance(), the same harness the test above uses):
 * dirt penetration averaged 3.0 cells, where SAND_IMPULSE_DRAG_POWDER_
 * SHIFT's own comment states the intent as "stop at the rim" - roughly
 * one. `PLOW_SEEDS * 2` as the threshold asserts an average comfortably
 * under 2 cells, a wide margin below the pre-fix ~3.0 and above the
 * roughly-1.0 target, so this does not double as a tuning pin for
 * whatever exact figure the drag sweep below eventually lands on. */
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
 * this rung where asserting on s.impulse_buf[0].speed directly is honest
 * rather than a stand-in for the thing the feature actually does, because
 * here the arithmetic IS the claim. A floor under the row (matching
 * test_a_flying_water_grain_does_not_swap_into_dirt_in_its_path above)
 * keeps ordinary gravity from pulling the dirt cell (or the mover) out of
 * the row before the impulse gets its one step.
 *
 * impulse_count IS 2, NOT 1, NOW - rung 3's TRANSFER (step_impulses()'s own
 * comment at the transfer site) queues a second entry for the struck dirt
 * cell itself once this scene's own numbers clear SAND_IMPULSE_TRANSFER_
 * MIN_SPEED: the mover's post-drag speed here is 255 - SAND_IMPULSE_SPEED_
 * RAMP - dirt's own density, comfortably above 64.
 * Entry 0 is still the mover - the deferred transfer is appended only AFTER
 * this loop's own compaction finishes (step_impulses()'s own top comment),
 * so it lands at entry 1, after everything this test actually pins. */
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
    /* Its own geometric decay, applied once PER CELL of travel - an entry
     * at full speed covers more than one, and the decay is charged for each
     * the same way the ramp is for everything else. Derived from the
     * constants rather than restated, so retuning how far a step reaches
     * cannot quietly turn this pin into a different claim.
     *
     * THE FIRST CELL IS CHARGED BEFORE push_count IS COMPUTED, so the
     * budget comes from the speed left AFTER it, not from 255 - mirrored
     * here in the same order step_impulses() runs it. Deriving the count
     * from the raw 255 instead happened to agree while the decay was
     * gentle enough that both readings landed on 3 cells, and stopped
     * agreeing the moment SAND_SPLASH_SPEED_DECAY_SHIFT was retuned - the
     * exact silent drift the paragraph above meant to rule out. */
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
 * same reason powders had to be let into drag. A blast against a stone
 * wall puts far more sand in the air than it ever dislodges stone, and
 * every one of those grains used to stall against the wall and sit there
 * until its flight aged out. Thrown right into a wall with the floor
 * carrying on beneath it: the arc is wall + floor-corner, which dominance-
 * quantises to a normal of `left` and reflects head-on back to `left`,
 * paying the head-on half. */
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

/* OPEN AIR MUST COST NOTHING BUT THE RAMP - the pin that says
 * sand_explode()'s own swept tuning (SAND_IMPULSE_SPEED_RAMP,
 * SAND_EXPLODE_CORE_DIVISOR, sand.h) did not move when drag arrived, and
 * the reason the two distance tests above can compare a medium against air
 * at all. Structural rather than a measured figure: a blast throws most of
 * its grains through empty space, so if a future change ever makes drag a
 * flat per-move charge instead of a density-scaled one, every explosion in
 * the app changes shape at once and this is what says so. The same one-step
 * shape as the two pins above, with an EMPTY cell ahead instead of dirt. */
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
 * Before this rung, every displacing move in step_impulses() - the push
 * roll's own move included - advanced an entry exactly one cell per
 * successful roll, identical to how far the ordinary gravity sweep moves a
 * falling grain in that same step. An impulse could therefore never outrun
 * gravity: a horizontal throw sank at close to 45 degrees, and ejecta
 * thrown off a powder volume could only ever reposition material, never
 * visibly leave it. See SAND_IMPULSE_CELLS_PER_STEP_DIVISOR's own comment
 * (sand.h) for the formula and the measured before/after numbers.
 *
 * TWO CLAIMS, TWO TESTS - a change like this is only really pinned by
 * BOTH ends of it: the negative case (nothing changes below the divisor)
 * right below, and the positive case (several cells at once, above it)
 * just after. */

#define SUBDIV_W 40
#define SUBDIV_H 80
#define SUBDIV_STEPS 60
/* One under SAND_IMPULSE_CELLS_PER_STEP_DIVISOR - the highest speed that
 * still computes 1 + speed / SAND_IMPULSE_CELLS_PER_STEP_DIVISOR == 1. */
#define SUBDIV_SPEED (SAND_IMPULSE_CELLS_PER_STEP_DIVISOR - 1)

/* THE NEGATIVE CASE, swept over many steps rather than pinned as a single
 * arithmetic value: SUBDIV_SPEED is comfortably below SAND_IMPULSE_BOUNCE_
 * MIN_SPEED and SAND_IMPULSE_TRANSFER_MIN_SPEED, so a one-step scene like
 * the drag pins above would only ever exercise the plain, undramatic
 * "roll then maybe move" path - which is exactly what needs sweeping,
 * since a bug that let push_cells creep above 1 at low speed would not
 * show up in a single sample. Run for SUBDIV_STEPS steps and check EVERY
 * one of them: the mover must never move more than one cell in a single
 * step, matching exactly what every entry already did before SAND_IMPULSE_
 * CELLS_PER_STEP_DIVISOR existed.
 *
 * A FULL FLOOR keeps the KIND_STATIC gravity-drift (the "AIRBORNE SOLIDS
 * FALL TOO" block, step_impulses(), sand.c) from ever touching X: gravity
 * is straight down here, so the drift's own first candidate is always
 * straight down too, and a floor spanning the grid's full width keeps that
 * candidate open for every one of SUBDIV_STEPS steps (the grid is tall
 * enough, SUBDIV_H, that the mover never gets anywhere near it) - so any
 * horizontal movement observed below can only be this rung's own push,
 * never the drift confusing the measurement the way it would near a floor
 * or wall (see the diagonal-drift case test_an_energetic_static_chunk_
 * over_a_powder_bank... exercises elsewhere in this file). ramp is passed
 * as 0 so `speed`, and so push_cells, stays fixed at SUBDIV_SPEED for
 * every step of the sweep - what is being pinned is the formula at one
 * fixed speed, not its decay. */
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
 * step, the entire point of SAND_IMPULSE_CELLS_PER_STEP_DIVISOR. Same
 * one-step, single-scene shape as the drag pins above (a full floor
 * beneath keeps the KIND_STATIC gravity-drift from ever touching X, so any
 * horizontal movement observed here is this rung's own push and nothing
 * else) - open air ahead, so nothing is displaced and no TRANSFER entry
 * gets queued alongside the mover (contrast the dirt-displacing pins
 * above, whose impulse_count is 2 for exactly that reason). */
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
 * step_impulses()'s blocked branch (sand.c) now turns a KIND_STATIC mover's
 * "wait against the wall" into "bounce off it, along the reflection of its
 * own travel direction about the blocking surface's approximate normal" -
 * blocker_normal() reads the wall, reflect_off_normal() turns that into a
 * new ring direction, and restitution (half speed on a head-on reflection,
 * a quarter on a glancing one) plus the SAND_IMPULSE_BOUNCE_MIN_SPEED floor
 * (sand.h) are what keep this from reopening the bounce-in-place pathology
 * step_impulses()'s own roll comment records the history of. Water/acid
 * keep their existing flat 180 flip untouched - this only widens
 * KIND_STATIC.
 *
 * QUANTISED BY DOMINANCE, NOT BY SIGN - blocker_normal()'s own comment
 * (sand.c) has the full account, but the shape of it matters here too: a
 * first version summed the arc's negated unit vectors and took the SIGN of
 * each axis independently, which degenerates for every diagonal `dir` (the
 * arc's centre cell is always covered - that is what "blocked" means - and
 * on a diagonal throw it alone already pins both signs to the mover's own
 * reverse, so a diagonal bounce could only ever reverse, never glance,
 * whatever the wall looked like). Comparing MAGNITUDES instead - the axis
 * with the larger push wins outright, both count only when they are close
 * - fixes that: a chunk thrown down-right into a flat floor (down AND
 * down-right covered, right open) now reads a normal of pure "up", the
 * vertical push winning over the single-cell horizontal one, and glances
 * up-right rather than reversing. Axis-aligned throws still always
 * reverse off a flat wall - a flat wall's own normal has no other axis to
 * weigh against - which is the correct physics, not a remaining gap. See
 * test_blocker_normal_and_reflect_off_normal_match_the_exhaustive_arc_table
 * below for the full 8-direction x 4-configuration ground truth this was
 * checked against. */

/* THE EXHAUSTIVE ARC TABLE - blocker_normal()/reflect_off_normal()'s own
 * counterpart to test_cover_primitive_matches_the_exhaustive_shape_table
 * above: a primitive whose whole point is a geometric rule checked at only
 * one or two hand-picked directions is not really tested, since exactly
 * that gap is what let a degenerate quantisation (sign instead of
 * dominance - see blocker_normal()'s own comment in sand_priv.h) through
 * for every diagonal direction while every axis-aligned one kept working.
 * Every one of the 8 ring directions, crossed with all 4 ways the 3-cell
 * arc (L = flank at dir-1, C = centre at dir, R = flank at dir+1) can be
 * covered - C is always covered, since that is the blocked branch's own
 * precondition (see blocker_normal()'s comment) - is 32 cases, and this
 * checks the primitive DIRECTLY, the same way the cover-mask table above
 * calls cover_mask()/covered_at() directly rather than only ever
 * exercising them through a stochastic scene.
 *
 * THE GROUND TRUTH BELOW IS NOT RE-DERIVED FROM THE FORMULA THIS PINS -
 * it would be circular, and it is exactly how the original sign-quantised
 * bug shipped clean: every hand test agreed with itself. These 32 values
 * were computed independently (by hand, cross-checked component by
 * component against blocker_normal()'s own dominance rule and
 * reflect_off_normal()'s r = d*|n|^2 - 2(d.n)n) before being written down
 * here - if a future change to either primitive disagrees with a row
 * below, the change is what is wrong, not the table. */
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

/* HEAD-ON REVERSAL - single step, exact arithmetic (the formula itself is
 * the claim, following rung 1's own drag-pin tests): a KIND_STATIC chunk
 * thrown square into a flat, wide floor must end up travelling the exact
 * OPPOSITE direction, at exactly half its already-ramped speed. The floor
 * spans the full grid width so all three of blocker_normal()'s arc cells
 * are covered and the quantised sum reduces to pure "up" - the head-on
 * case (see this section's own worked-table cross-check in the code
 * review, table row 1). */
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

/* GLANCING DEFLECTION - same single-step, exact-arithmetic shape as the
 * head-on test above, and now the SPEC'S OWN scene: a chunk thrown
 * DOWN-RIGHT into a FLAT floor (not a corner - blocker_normal()'s
 * dominance rule is what makes a flat floor produce a glance here rather
 * than a reverse; see this section's own intro comment and
 * blocker_normal()'s in sand.c). The mover sits on stone directly below it
 * AND down-right of it (both part of the floor), with open space to its
 * right - the vertical push from two covered cells dominates the single
 * horizontal one, so the normal reads as pure "up" rather than "up-left",
 * and reflecting a down-right travel off a pure-up normal sends the chunk
 * UP-RIGHT: neither reversed (that would be up-left) nor still travelling
 * down-right. This is the test that proves the normal is real rather than
 * a dressed-up 180 flip. */
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

/* THE FLOOR GUARD - the pin against the bounce-in-place pathology
 * step_impulses()'s own roll comment records the history of: below
 * SAND_IMPULSE_BOUNCE_MIN_SPEED, a blocked KIND_STATIC entry must just
 * wait, exactly as it always has, not bounce at all. A deliberately
 * oversized `ramp` (not a realistic caller) is the cleanest way to land
 * the roll - which reads entry.speed BEFORE this step's own ramp - at its
 * reliable, near-certain value of 255, while still landing entry.speed
 * AFTER the ramp comfortably under the floor for this one step. */
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

/* GRID EDGE REFLECTS - sand_at()'s off-grid-is-STONE convention
 * (step_impulses()'s own comment above) has to fold into this bounce for
 * free, the same way it already folds into can_impulse_enter()'s own
 * blocking rule: a chunk thrown at the edge of the board must bounce off
 * it exactly as it would off a real wall there, not wait at the edge
 * forever. BEHAVIOURAL, not a whitebox pin - this is not one of the three
 * tests where asserting on impulse_buf directly is honest (see this file's
 * own drag-rung precedent), so this reads the BOARD instead.
 *
 * DELIBERATELY AIRBORNE, NOT RESTING ON A FLOOR AT THE EDGE ITSELF - a
 * first version of this test put the mover on a full-width floor directly
 * under it, the same shape test_a_thrown_chunk_reverses_direction_
 * bouncing_off_a_flat_floor uses. That measurably broke: with genuine
 * gravity-relative support right there, step_impulses()'s own SETTLED
 * check (its "SUPPORTED, NOT MERELY ROLLED" block) drops the entry from
 * tracking the very first time a single push-roll fails - usually the
 * NEXT step, since a bounce more than halves `speed` and the roll is
 * literally that number out of 256 - freezing it back at EDGE_X before it
 * ever gets a second chance to execute its new (bounced) direction. That
 * is a real, separate interaction (a chunk resting on solid ground gets
 * roughly ONE shot to skid before "settled" wins), not a rung 2 bug, and
 * it is why THIS scene instead drops the mover from open air well above a
 * distant floor: while genuinely still falling, a failed push-roll never
 * reads as settled (the gravity-relative candidates below it are open, so
 * step_impulses() keeps it tracked and falling every step, per the "A
 * SUPPORT THAT IS ITSELF IN FLIGHT" / has_opening logic - here it is
 * simpler still, the opening is just open air), so it keeps getting fresh
 * chances at its current direction on the way down - plenty of opportunity
 * for the post-bounce leftward push to actually fire before it lands.
 * Confirmed with a throwaway host probe (same production code, gcc -O1):
 * across 20 seeds this exact scene always ended up strictly left of
 * EDGE_X, settling in 13 to 15 steps. */
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

/* ANTI-PINBALL, BEHAVIOURAL - a chunk thrown into a fully closed stone box
 * must come to rest (s.impulse_count reaches 0) within a BOUNDED number of
 * steps, not rattle around the box indefinitely - the exact failure mode
 * step_impulses()'s own roll comment's reverted-attempt history warns a
 * bounce mechanism can reopen if left undamped and unfloored.
 *
 * BOX_MAX_STEPS (50) is measured against the ACTUAL 16 seeds this test
 * runs (k = 1..BOX_SEEDS, throw direction k % 8), not a separate sample -
 * a throwaway host probe (same production sand.c/sand.h/sand_priv.h this
 * test links, gcc -O1, no test-only shortcuts) ran exactly this scene at
 * exactly these seeds and measured settle-step counts of 3 to 27, worst
 * case 27 (seed 12, throw direction 4 - straight up). 50 is under 2x that
 * measured worst case - real headroom, not an order of magnitude padding
 * it can never trip.
 *
 * A WIDER SWEEP WAS ALSO RUN TO CHECK THE TAIL BEHAVIOUR THIS FIXED SET
 * MIGHT BE HIDING: 300,000 seeds, same box-and-throw shape, found a slow-
 * growing worst case (44 at seed 76, 47 at 2,532, 48 at 11,316, 49 at
 * 34,412, 53 at 49,844, 54 at 57,300, nothing higher in the remaining
 * ~242,700 seeds) - a heavy but visibly converging tail, always the same
 * "thrown straight up" direction. This test does not chase that tail: it
 * runs the same fixed 16 seeds every time (deterministic, no run-to-run
 * variance to guard against), and 50 is chosen with real margin over
 * THOSE seeds' own worst case, not the wider distribution's.
 *
 * THIS BOUND DOES NOT RELIABLY CATCH A RESTITUTION REGRESSION, AND SAYING
 * SO IS MORE HONEST THAN PRETENDING OTHERWISE. In a fully enclosed box,
 * unlike the two open-scene tests below, the entry bounces many times
 * before it can possibly settle, so the PRE-EXISTING linear ramp
 * (SAND_IMPULSE_SPEED_RAMP, sand.h - not this rung's own code) ends up
 * the dominant thing bounding settle time here, not restitution. Deleting
 * restitution's speed charge entirely (a throwaway mutation, reverted
 * immediately) moved this test's OWN 16-seed worst case from 27 to only
 * 30 - both comfortably under 50, so no bound in the "real headroom, not
 * an order of magnitude" range this rung's other two tests use can split
 * the two. What THIS bound DOES catch, confirmed the same way: deleting
 * the RAMP decrement entirely - simulating the literal reverted-attempt
 * pathology step_impulses()'s own comment describes, "never settle" -
 * broke this exact test (seed 4, throw direction 4, still rattling past
 * 50 steps, and past the old 150 too) while every other test in this rung
 * stayed silent about it. So this bound is a genuine backstop against
 * that specific historical failure mode, not a restitution pin - the two
 * open-scene tests below cover restitution instead, where it is actually
 * the dominant mechanism.
 * REUSES THE 8x8 GLOBAL FIXTURE ARRAYS (cells[], impulse_buf[]) rather
 * than a heap allocation - the box is exactly W x H, so nothing bigger is
 * needed, unlike the wider drag-rung and wall-notch scenes elsewhere in
 * this file. Averaged in the sense of "checked against every seed", not
 * summed - "a single run of a chaotic scene is not evidence", same idiom
 * as test_water_does_not_drill_into_oil_when_tilted above, but this
 * property (bounded, not a distance) is a per-seed pass/fail rather than
 * something to sum. */
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
 * landing into bouncing. A chunk thrown straight down through open air
 * onto a plain floor must still come to rest close to where gravity alone
 * would put it (directly on the floor), within a bounded number of steps,
 * not just eventually.
 *
 * OPEN_MAX_STEPS and the expected landing row are both measured, same
 * probe methodology as BOX_MAX_STEPS above: across 64 seeds this exact
 * scene (a lone floor at the bottom of a tall, otherwise open grid, mover
 * dropped from near the top at full speed) landed on OPEN_H - 2 - directly
 * on the floor - every single time, whatever the settle time.
 *
 * RE-MEASURED, RAISED FROM 20 TO 52, alongside SAND_IMPULSE_CELLS_PER_
 * STEP_DIVISOR (sand.h) - a chunk now reaches the floor in far fewer
 * steps (this exact scene's fastest seed dropped from 7 to 4), but that is
 * not the whole story: the ordinary per-step ramp still only sheds
 * SAND_IMPULSE_SPEED_RAMP of speed per step regardless of how many cells
 * that step covered, so arriving in fewer steps also means arriving with
 * far more speed still on the clock - the reflection at the floor now has
 * more to dissipate, and does so over more steps of its own, not fewer.
 * The worst case in this same 64-seed sweep rose from 13 to 34 steps. 52
 * is over 1.5x that new worst case, the same margin convention the old
 * bound used over its own worst case. */
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
 * real shape-dependent bugs in this simulation before (see
 * test_lava_in_a_wall_notch_never_bursts above, and cover_mask()'s own
 * comment in sand_priv.h). Four overlapping stone discs, radius 2 to 4,
 * centres about 3 cells apart - the way a person actually draws a wall
 * with a round brush, notches and bulges included - rather than a flat
 * one-cell-thick line. A chunk thrown into it must still come to rest,
 * conserving itself exactly (nothing created or destroyed by a bounce),
 * within a bounded number of steps. Direction is deliberately NOT
 * asserted - the wall's own irregular shape makes the exact bounce path
 * uninteresting; conservation and boundedness are the properties that
 * matter here.
 *
 * WALL_MAX_STEPS (50) is measured the same way as BOX_MAX_STEPS above:
 * this exact scene, run across 64 seeds against the production code, took
 * 17 to 38 steps to settle, every seed conserving the thrown chunk
 * exactly. 50 is about 1.3x that measured worst case - and, same as
 * OPEN_MAX_STEPS above, deliberately tight rather than padded: the same
 * probe with restitution's speed charge temporarily deleted rose to a
 * 60-step worst case over the same 64 seeds, so a restitution regression
 * here trips this test too. */
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

/* THE REGRESSION GUARD. Every test above this line already existed the day
 * a real device reported: "in water nothing happens, in sand also no
 * holes, i can see some faint movement when near pixels they do move but
 * that's it." None of them caught it, because none of them detonated
 * somewhere with no adjacent empty cell anywhere inside the radius - a
 * packed bed, or a body of water - which is the one scene the plan's v1
 * "stop on any obstruction" rule could never move a single grain in: every
 * queued entry's very first target was already occupied, so every entry
 * died on turn one, and the mechanic was silently a no-op everywhere it
 * was actually supposed to matter.
 *
 * Packed on every side, deliberately, with only ONE piece of open space
 * anywhere on the board (row 0) and it nowhere near the blast: this is
 * exactly the scene that read as nothing happening. */
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

    /* Grains reached beyond the original radius. Row 1 is 4 cells from the
     * centre, well outside radius 2, and started this test fully packed
     * (all 8 columns).
     *
     * The core is fire now, not a hole - fire is far LIGHTER than sand
     * (density 15 against sand's 60), and can_enter()'s ordinary "a denser
     * mover displaces a lighter fluid" rule, the same one that lets sand
     * sink through water or gas, applies here with no special-casing at
     * all: a sand grain directly above a fire cell simply swaps through
     * it via the main sweep, exactly as it would sink through smoke. That
     * turns out to be enough on its own - measured on a 20,000-seed sweep
     * with decay left OFF (this fixture's default), the row-1 disturbance
     * checked below appears on literally the first step, every time, with
     * no dependence on fire ever rising or decaying away first. So there
     * is no path for row 1 to stay fully packed except something above
     * the core swapping down through the fire that filled it, one cell at
     * a time, propagating exactly the way
     * test_undermining_a_sleeping_pile_collapses_it already proves a hole
     * propagates - and row 0 has nothing above it to refill whichever
     * column runs out of material first, so that column's row-1 cell is
     * left empty once things settle.
     *
     * Which column that turns out to be is NOT fixed to directly above
     * the centre: the core's own diagonal-adjacent cells (3,4) and (5,4)
     * are themselves queued flight entries (see SAND_EXPLODE_CORE_DIVISOR),
     * and retrying-until-clear (see step_impulses()'s "blocked means wait")
     * can walk the disturbance sideways by the time it reaches this far
     * up - column 4 collapsing was only ever the simplest of several
     * columns that could plausibly hollow out first. So this checks the
     * row generally rather than one hand-picked cell: some column in the
     * blast's own horizontal span must have given up material this far
     * out, not necessarily the one directly above where it started. A
     * "stop on any obstruction" rule with no filled core could never have
     * produced this from a fully packed bed at all, regardless of which
     * column ends up being the one that shows it. */
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

/* THE SPECIFIC REGRESSION THAT JUST BIT US, pinned directly. A device
 * pass on the first real-radius detonation reported it as "barely
 * noticeable" with "solids barely move" - traced to sand_explode()'s
 * OLD scan order (dy-outer, dx-inner, top row to bottom row) handing an
 * undersized cap entirely to the disc's top nine or ten rows before the
 * scan ever reached the core or the lower half. Every test above this
 * line happens to detonate with a buffer at least as big as the disc
 * (impulse_buf[] is W*H, and an 8x8 grid's largest disc never comes
 * close), so none of them ever truncate at all - they would pass exactly
 * as they do now even with the old row-order scan restored, because
 * nothing was ever cut off for the order to be unfair ABOUT. This is the
 * one test in the file where the cap must actually bind for the fix to
 * be tested at all.
 *
 * axis_impulse_buf[8] is sized to exactly one full ring - see its own
 * comment above - so a radius-3 blast's immediate ring of 8 neighbours
 * fits with nothing to spare, and ring 2 and beyond never get a slot.
 * Under ring order that ring is EVERY direction at once - all four axes,
 * all four diagonals - so all four axis checks below must pass. Under
 * the old row order, the same cap of 8 is exhausted while still working
 * through the rows above the centre (dy = -3 contributes 1 candidate, dy
 * = -2 contributes 5, already 6 of the 8 slots, and dy = -1 supplies the
 * rest before the scan reaches dy = 0 at all) - so DOWN, LEFT and RIGHT
 * would all still be waiting for a slot that never comes, while UP alone
 * succeeds. A regression to row order would fail exactly three of these
 * four assertions, never all four and never zero - which is what makes
 * this a test of ORDER specifically, not merely of whether the cap
 * exists.
 *
 * Checked directly against the queue itself (s.impulse_count entries in
 * s.impulse_buf), not inferred from where anything ends up on the board -
 * inferring it from the board is what let the old bug ship in the first
 * place, since a test that only watches for "something moved" cannot
 * tell a fair ring from a lopsided crescent. ring_dir()'s own numbering
 * (sand_priv.h, not included here - this suite only sees the public dir
 * byte) is 0 down, 2 right, 4 up, 6 left; 1/3/5/7 are the diagonals
 * between them and are not checked here, since the four axes are the
 * ones whose presence or absence actually distinguishes ring order from
 * row order at this cap. */
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

    /* Mirrors sand_explode()'s own `core_radius` in sand.c EXACTLY,
     * clamp included - not just the bare division. Plain `3 /
     * SAND_EXPLODE_CORE_DIVISOR` used to agree with the real, clamped
     * value at every divisor this constant had ever held (2, then 3),
     * purely by coincidence: raising it to 5 made 3 / 5 round down to 0,
     * while sand_explode() itself still clamps a radius-3 blast's core to
     * 1 (see SAND_EXPLODE_CORE_DIVISOR's own comment in sand.h) - so the
     * unclamped copy here started asserting SAND_EMPTY over four cells
     * that are, correctly, fire. A local recomputation that quietly
     * assumes away a documented clamp is exactly the kind of thing that
     * only breaks the next time a constant moves, which is now. */
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
 * Two full-height MAT_STONE walls facing each other across a 42-cell gap, a
 * blast at the left wall, and a per-step count of how many times each
 * dislodged chunk's OWN `dir` changes before it finally settles - a
 * ricochet reads as more than one change, a single stop as zero or one.
 * This does not exercise anything new: rungs 1 and 2 (drag, reflection) are
 * already shipped. It exists to tell rung 3's tuning apart from a guess -
 * see the numbers below, and step_impulses()'s own "FLOORED AT SAND_
 * IMPULSE_BOUNCE_MIN_SPEED" comment for the mechanism being measured.
 *
 * RADIUS IS THE APP'S OWN FIGURE, CONVERTED TO CELLS - DETONATE_RADIUS_PX
 * is 50 (app_sand.c) and the default quality is NORMAL, 4 px/cell
 * (QUALITY_DEFAULT, qualities[], app_sand.c), so the app's own detonate
 * call computes (DETONATE_RADIUS_PX + cell / 2) / cell = (50 + 2) / 4 =
 * 13 - the exact arithmetic app_sand.c's DETONATE handler uses.
 *
 * THE BLAST CENTRE IS OFF-GRID, BEHIND THE WALL, NOT A TYPO. A queued
 * grain's direction is "away from the blast centre, through the grain's own
 * cell" (queue_outward_impulse(), sand.c) - so a wall standing to the RIGHT
 * of the centre gets thrown further right, into the gap, while a wall to
 * the LEFT of the centre gets thrown further left, away from the gap and
 * off the grid. Centring the blast IN the gap, next to the wall, throws the
 * wall the wrong way - straight into itself. Centring it AT x = -11 puts
 * the left wall (x = 0) 11 cells to its right, which is what "an explosion
 * AT the wall" has to mean for the wall's own rubble to end up flying
 * toward the far one: a charge set against/behind the wall's face, blowing
 * it into the room. This placement has a second, welcome effect: the
 * unconditional fire-filled core (radius / SAND_EXPLODE_CORE_DIVISOR = 13 /
 * 5 = 2 around the centre) sits entirely off-grid too (x in [-13, -9]), so
 * nothing here ever creates a MAT_FIRE entry - every tracked entry is
 * genuine dislodged wall stone, not core debris.
 *
 * IDENTITY IS TRACKED BY POSITION, NOT BY CELL BYTE, and that is a real
 * finding of its own, not a style choice: MAT_STONE's variant is a
 * TEMPERATURE nibble now (heat_ramp = 32, cools = 5, material.c, "Stone
 * carries a temperature exactly as glass does"), and it drifts toward
 * ambient every step even with no heat source anywhere on the board - an
 * early version of this test gave every wall row a distinct variant to use
 * as a per-entry id and measured MORE distinct bytes appearing over a run
 * than were ever actually dislodged at explode time, purely from that
 * drift. Position survives this cleanly, but RICOCHET_MATCH_RADIUS had to
 * grow well past the raw per-step bound to keep doing so once entries
 * started covering more ground per step (SAND_IMPULSE_CELLS_PER_STEP_
 * DIVISOR, sand.h): the INSTANTANEOUS bound is SAND_IMPULSE_CELLS_PER_
 * STEP_MAX + 1 cells in one step (5 at today's cap of 4 - a multi-cell
 * push-roll move plus, for a KIND_STATIC mover, one more unconditional
 * gravity-drift move, both in step_impulses(), sand.c), but this GREEDY,
 * array-order tracker is not immune to a single early ambiguous link: once
 * two candidates are plausibly close to the same lineage (far more likely
 * now that several concurrently-flying entries cover 4-5x the ground per
 * step they used to, closing the "several rows apart" gap this scene
 * relies on faster than before), one wrong pick compounds every step after
 * it, and recovering needs slack well beyond the instantaneous bound - a
 * radius of 6 (5 + 1 slack, the same margin convention the old radius-3
 * used over the old 2-cell bound) left 236 of the 100 seeds' candidates
 * unmatched; 15 leaves none. MEASURED, NOT GUESSED: swept 6/8/10/12/15,
 * unmatched counts of 236/156/60/28/0 - 15 is the smallest of those that
 * actually clears every seed, not a round number picked to be safe.
 * Checked, not merely assumed: the measurement below counts entries that
 * ever failed to find any match at all (would mean a genuinely new impulse
 * got queued mid-run, which nothing in this liquid/gas/fire-free scene
 * should ever do, OR that this tracker's own radius is still too tight) -
 * zero, every seed, at RICOCHET_MATCH_RADIUS 15.
 *
 * RE-MEASURED AFTER SAND_IMPULSE_CELLS_PER_STEP_DIVISOR (sand.h) LANDED -
 * the arithmetic walkthrough this comment used to carry (a 42-cell gap
 * costing 42 * SAND_IMPULSE_SPEED_RAMP speed to cross) assumed exactly one
 * cell of travel per step, which is no longer true: a fast entry now
 * crosses the same gap in roughly a quarter as many steps, so it arrives
 * having paid roughly a quarter as much ramp - THE RAMP SPENT CROSSING THE
 * GAP IS NO LONGER THE LIMITER IT WAS. SEEDS 1..100, this exact scene: 284
 * tracked stone entries total (unchanged - the blast's own dislodge count
 * has nothing to do with how far anything travels afterward), 160 (56.3%)
 * changed direction at least once, 45 (15.8%) at least twice, 3 (1.1%)
 * three times - see docs/sand/Sand-Simulation.md for the before/after
 * table this rung is judged against; a real shift in the >= 1 bucket (150
 * before, 160 now) from entries that used to run out of ramp mid-gap and
 * now cross with speed to spare.
 *
 * THE OLD "GENTLER RESTITUTION" ARITHMETIC (a speculative exploration of
 * lowering SAND_IMPULSE_BOUNCE_MIN_SPEED or equalising the head-on/glancing
 * split, never acted on) is dropped rather than reworked here - it was
 * built entirely on the now-stale one-cell-per-step gap-crossing cost
 * above, and rederiving it against multi-cell travel is a separate
 * exploration nobody has asked for. Nothing it discussed was ever changed
 * by this rung: SAND_IMPULSE_BOUNCE_MIN_SPEED and the head-on/glancing
 * split are exactly what they were. */
#define RICOCHET_W 44
#define RICOCHET_H 40
#define RICOCHET_RADIUS 13
#define RICOCHET_CX (-11)
#define RICOCHET_CY (RICOCHET_H / 2)
#define RICOCHET_SEEDS 100
#define RICOCHET_MAX_STEPS 300
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

    /* static, not a stack array - see check_stack_usage.py's own gate
     * (docs/sand/Performance-Tuning-Attempts.md): this helper runs on the
     * device's 3584-byte main task stack too (the on-device selftest
     * links every suite), and RICOCHET_MAX_TRACK copies of every array
     * below pushed a stack-local version of this function well past the
     * 1024-byte ceiling. One call is in flight at a time (a plain
     * sequential loop over seeds in the test below, no re-entrancy), so
     * moving these to static costs nothing but the .bss they already
     * would have cost on the stack. */
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
 * Today a flying cell swaps with whatever it displaces and the volume
 * closes behind it - nothing else in it ever learns it was hit. This is
 * what lets a struck cell pick up impulse of its own instead (see the
 * TRANSFER site's own comment at the move site in step_impulses(), sand.c,
 * right after the swap and right where the drag charge already sits) -
 * including flying clean out of the volume it was sitting in.
 *
 * EJECTA IS MEASURED, NOT ASSUMED, and DIRT and WATER measure completely
 * differently - a real finding, not a shape mismatch between the two
 * scenes below. DIRT (KIND_POWDER) is inert once packed: an undisturbed
 * bank does not spread on its own, so "cells found outside the bank's own
 * original footprint" is a clean signal - transfer on, or transfer off via
 * a temporary mutation confirmed live (see below), the difference is
 * exactly zero vs a real number. WATER (KIND_LIQUID) is not inert: even a
 * perfectly flat, resting layer spreads sideways on its own, at a rate
 * (measured: exactly 1.0 mass-equivalent cell per seed per step, dead
 * linear, confirmed by running the SAME scene with no mover in it at all)
 * that is comparable to whatever transfer adds on top. "Zero with transfer
 * removed" is consequently the wrong bar for water and was checked, not
 * assumed: at 200 seeds, this exact scene measures 387 cells outside its
 * own original footprint with transfer's own gate hard-mutated to `false`,
 * not zero - that is water's own ordinary equalisation, present with or
 * without this rung's own change, and no geometry tried (a taller block, a
 * walled and fully-settled basin, a basin with a genuine overflow gap, an
 * upward throw) separated it from transfer's own contribution any better;
 * every one of those either reproduced the same baseline or, walled tightly
 * enough to suppress it, blocked transfer too (a transferred entry can
 * never breach a KIND_STATIC wall regardless - can_impulse_enter()'s own
 * rule, unconditional for a KIND_STATIC target). What the SAME hard-mutated
 * run does show is a real, reproducible MARGIN over that baseline - 454
 * with transfer against 387 without, both at the same 200 seeds - so the
 * water test below asserts against that margin (a threshold clearly above
 * 387, clearly below 454) rather than against zero.
 *
 * RECONFIRMED, RUNG 4: both numbers moved from the rung 3 measurement (260
 * baseline, 422 with transfer) once SAND_IMPULSE_DRAG_SHIFT dropped to 0 and
 * the transfer direction became a backward cone rather than the mover's own
 * heading (see the TRANSFER site's own comment, step_impulses(), sand.c) -
 * this is a different mechanism now, so it earned a fresh measurement rather
 * than trusting the old one to still be in the right place. */

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

/* DIRT, packed against the floor, no gap underneath it - a floating bank
 * would slump under its own ordinary gravity before the mover ever arrived
 * (measured on an earlier version of this scene: a bank left floating a
 * few rows above the real floor had already relocated itself, whole,
 * before firing), which would read as ejecta this feature never caused.
 * VARIANT 0, NOT 8 - see plow_build()'s own comment a few hundred lines up
 * for why 8 is WET soil under main's re-encoding and quietly changes state
 * mid-measurement; this scene does not need dirt's moisture at all, so it
 * never risks it.
 *
 * THE MOVER STARTS INSIDE THE BANK'S OWN FOOTPRINT, not adjacent to it -
 * an earlier version placed it one cell to the left (outside), and the
 * very first ordinary swap (mover in, one bank cell out to the mover's old
 * - now outside - cell) counted as "ejecta" on EVERY seed whether transfer
 * fired or not, since that swap is rung 1's own mechanic, not this rung's.
 * Starting inside means the first several swaps relocate bank material to
 * cells still inside the original footprint, so anything found outside
 * afterward is actually attributable to a cell having FLOWN there under
 * its own power. */
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
 * footprint (was 31 before rung 4 dropped SAND_IMPULSE_DRAG_SHIFT to 0 and
 * pointed the transfer at a backward cone instead of the mover's own
 * heading - see the TRANSFER site's own comment, step_impulses(), sand.c -
 * both raise how much of what a struck cell picks up actually clears the
 * bank's own footprint, so a bigger number here is the fix working, not
 * drift). RED CHECK, live: with the transfer site's own gate in
 * step_impulses() (sand.c) hard-mutated to `false` and reverted after,
 * this exact scene measures exactly 0 - every seed, no exceptions. Dirt
 * packed and resting does not spread on its own; every one of the 50 is a
 * cell transfer flung. */
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
 * A DIFFERENT SIGNAL FROM THE EJECTA TESTS ABOVE, on purpose: "outside the
 * bank's own footprint" (ejecta_count_outside(), above) counts a cell that
 * merely relocated, however it got there or however briefly it was ever
 * off the ground. AIRBORNE - every one of a cell's eight neighbours
 * genuinely CELL_IS_EMPTY() at the instant sampled - is the stricter claim
 * the maintainer actually asked for: material visibly FLYING, suspended in
 * open air, not just shuffled sideways within a settling pile. Before this
 * rung, every displacing move in step_impulses() advanced an entry exactly
 * one cell per successful roll - identical to how far the ordinary gravity
 * sweep moves a falling grain in the same step - so a thrown chunk could
 * never outrun gravity: ejecta could reposition but not visibly leave the
 * volume it came from.
 *
 * THE SCENE: an 8-cell-wide row of MAT_STONE, thrown together at a
 * diagonal (down-right, dir 1 - "at an angle", not straight down or
 * straight along the surface) into a settled MAT_SAND bed resting on a
 * solid floor. Airborne sand is counted every step for AIRBORNE_STEPS,
 * peak kept per seed, over AIRBORNE_SEEDS seeds.
 *
 * MEASURED, this exact scene, seeds 1..40: BEFORE this rung, 26 of 40
 * seeds ever showed any airborne sand at all, peaking at 3 simultaneously
 * airborne cells. AFTER, all 40 seeds show airborne sand, peaking at 8 -
 * both more often and more of it at once. Different numbers from the
 * maintainer's own hand-measured scene (12 of 40, peak 1, before) - a
 * different bed/chunk/angle, not a discrepancy - but the same shape: a
 * capped, modest peak before this rung existed no matter the scene, a
 * clearly higher one after. */

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
 * THE BUG: step_impulses()'s `!rolled_move` branch (the push-roll failed
 * this turn) only ever kept a KIND_STATIC entry tracked once it verified
 * the entry was still genuinely airborne - every OTHER kind, KIND_POWDER
 * included, fell straight through to the plain `continue` right after and
 * was DROPPED, whether or not it had actually landed. The roll's own
 * chance IS entry.speed (that loop's own comment), and the ramp erodes it
 * every step, so a thrown powder grain's odds of still being tracked
 * collapsed with `speed` long before its actual energy did: roughly 62% at
 * 10 steps, 16% at 20, 2% at 30, 0.1% at 40 - while `speed` at step 30 is
 * still 193, nearly three times SAND_IMPULSE_TRANSFER_MIN_SPEED. The grain
 * kept flying regardless (the ordinary sweep falls a powder grain every
 * step whether or not this loop still has it tracked), but it arrived with
 * no impulse entry attached, so the TRANSFER block never ran on impact and
 * nothing was ever ejected - harmless before TRANSFER existed, which is
 * exactly why only KIND_STATIC was ever checked here, and not any more.
 *
 * A TALL WALL, NOT A BANK RESTING ON A SHARED FLOOR - the mover has to
 * stay genuinely AIRBORNE for its whole flight, which a floor would end
 * early: the instant a KIND_POWDER mover is actually supported,
 * can_impulse_enter_gravity_ward() correctly finds no opening in any of
 * its three gravity-ward candidates and it settles, exactly as it should
 * (that is the intended floor, not a bug to route around). Resting the
 * wall's own footprint on the SAME floor as the open lane made the mover
 * land on that floor itself long before covering any real distance, which
 * measured as "no ejecta at every distance" even for the FIXED code - a
 * scene bug, not a sand.c one. A wall tall enough to span whatever row the
 * mover has fallen to by the time it crosses the wall's own column (it
 * falls under the ordinary sweep the whole flight, independent of this
 * mechanism) keeps it genuinely airborne until the moment of impact,
 * whatever that row turns out to be. */
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

/* MEASURED, 60 seeds, at EJECTA_FAR_DISTANCE (30 cells of open-air flight
 * before impact - kept smaller than the full 50 cells the diagnosis itself
 * used, so the wall this scene needs to stay tall enough for fits inside
 * the host heap arena, DP_FREE_HEAP_BYTES, alongside this test's own grid):
 * 60 dirt cells ejected (one per seed - a single struck cell's own
 * TRANSFER, every time) with this rung's fix; 0 with the bug (KIND_POWDER
 * hard-mutated back out of the `!rolled_move` branch, the live-mutation
 * check this file uses elsewhere). Every distance in the diagnosis's own
 * range measured for the maintainer's own record (not asserted
 * individually here, to keep this one assertion cheap): 0.933/0.367/0.000/
 * 0.000 mean ejecta per seed before the fix at 5/15/30/50 cells, 0.900/
 * 1.000/1.000/1.000 after - 30 cells (this test's own distance) is already
 * comfortably past where tracking used to collapse entirely. */
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

/* WATER, a single row deep, resting directly on the floor - see this
 * section's own top comment for why a taller block was tried and
 * rejected: multiple rows of standing liquid trigger splash_displace()
 * (sand_liquid.c) internally as they settle into a flatter shape, which
 * has nothing to do with this rung and swamps it. A single row cannot
 * "fall onto occupied liquid" within itself, so that path never fires
 * here - confirmed by watching s.impulse_count stay at exactly 1 (the
 * mover alone) for the first several steps with transfer's own gate
 * hard-mutated off, the same live-mutation check this file uses
 * elsewhere.
 *
 * ONLY 4 STEPS - short on purpose, the same reason PLOW_STEPS is short
 * (that constant's own comment, a few hundred lines up): water's own
 * ordinary sideways equalisation is dead linear in step count (measured:
 * exactly matches a control scene with no mover in it at all, seed for
 * seed), so it keeps closing the gap on every step this runs - the margin
 * over that baseline that transfer actually adds is at its widest early
 * and shrinks the longer this scene keeps stepping. */
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
 * hard-mutated to `false` (this section's own top comment has the reasoning
 * for why 387, not 0, is the right baseline here, and why both this and the
 * 454 figure moved from rung 3's 260/422 once drag and the transfer
 * direction changed). EJECTA_WATER_THRESHOLD sits comfortably above that
 * measured baseline and comfortably below the measured total - a regression
 * that silently disabled transfer for liquids would drop this scene back to
 * the 387 baseline and trip it. RAISED FROM 350, WHICH THIS RUNG'S OWN
 * NUMBERS BROKE - 350 sat above the OLD 260 baseline, but the new 387
 * baseline sits above 350 too, which would have made the threshold pass
 * even with transfer silently disabled; re-measuring rather than reusing
 * the old figure is what caught it. */
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

/* THE DETERMINISTIC HALF OF THE SAME CLAIM, and the reason the threshold
 * above is allowed to be a threshold. That test measures the BEHAVIOUR -
 * water leaving the pool - across 200 chaotic seeds, and its floor sits
 * between two measured numbers rather than at zero, because water spreads
 * on its own whatever this rung does. This one measures the MECHANISM in a
 * single step with one struck cell: the transfer entry has to exist, at the
 * cell the mover just vacated, carrying half the mover's post-drag speed
 * and a direction in the BACKWARD CONE (one of `dir+3`, `dir+4`, `dir+5` -
 * see the TRANSFER site's own comment, step_impulses(), sand.c, for why
 * that replaced the mover's own heading: queuing along the mover's own
 * direction drove struck material deeper into whatever it hit instead of
 * spraying it back out, which is why nothing visibly ejected on device).
 * Between them, a regression has nowhere to hide - if water physics on
 * main ever drifts the threshold's own numbers, this still says whether
 * transfer itself is alive.
 *
 * MEMBERSHIP, NOT EQUALITY, on direction - rng_below(&s->rng, 3) (the
 * transfer site) picks which of the three cone directions fires, so this
 * cannot pin one exact value the way DIR_RIGHT alone once could; asserting
 * membership in the 3-direction set is the honest version of the same
 * claim, not a weaker stand-in for it.
 *
 * Found by scanning rather than by index: the liquid passes run before the
 * flight pass and splash_displace() (sand_liquid.c) queues entries of its
 * own, so nothing guarantees which slot the transfer lands in. */
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
 * SAND_CASCADE_MAX_PER_STEP caps how many deferred entries (cascade
 * relays AND transfers alike, since rung 3 - see step_impulses()'s own
 * comment on that array) can be QUEUED in any one step, but impulse_buf
 * itself (APP_IMPULSE_MAX in app_sand.c on the device) is a separate,
 * FIXED-SIZE budget shared with whatever explosion or splash is already
 * using it - sand_impulse() refuses silently once it is full. A long plow
 * through a wide, low-density bank (most cells clear SAND_IMPULSE_
 * TRANSFER_MIN_SPEED easily) is exactly the shape that could queue one
 * transfer per cell touched if the per-step cap were not there; this test
 * is what proves the cap actually holds the line rather than merely
 * assuming it does. */
/* SAME SHAPE AS plow_build() (a few hundred lines up) - dirt fills ONLY
 * row 0, everything below is OPEN except a single floor far at the
 * bottom, and the mover is thrown from (1, 0). NOT a full-width floor
 * directly under the mover's own row - an earlier version of this test
 * used exactly that, and the mover (KIND_STATIC) settled on literally the
 * SECOND step: fully supported with nowhere to fall, step_impulses()'s
 * own "SUPPORTED, NOT MERELY ROLLED" rule (sand.c) drops a KIND_STATIC
 * entry from tracking the instant one push-roll fails, and a floor
 * spanning the whole width guarantees that happens almost immediately at
 * this speed. plow_build()'s own open-below shape is what keeps the
 * mover - and the dirt feeding it from behind, which also falls once it
 * is unsupported, see that function's own comment - genuinely engaged for
 * many steps instead of freezing after one. Measured with this shape,
 * this exact scene (200-cell bank): activity (impulse_count > 0) for 27
 * steps, never above 3 entries concurrently even with the transfer cap
 * completely unbounded - BUDGET_IMPULSE_MAX is deliberately far below
 * what an ordinary run ever needs, so the assertion below is a real gate,
 * not a number this scene could never approach anyway. */
#define BUDGET_W 200
#define BUDGET_H 30
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
 * Same shape as test_a_thrown_static_chunk_conserves_lava_mass_on_sink
 * (a few thousand lines up): every transferred entry is queued through
 * plain sand_impulse() (step_impulses()'s own append loop, sand.c), which
 * only ever moves a cell that is ALREADY on the board - nothing here
 * manufactures a fresh one. DIRT is checked by CELL COUNT, exactly - a
 * KIND_POWDER cell's own count never legitimately changes from ordinary
 * movement the way a liquid's can (mass_of()'s own comment). WATER is
 * checked by MASS, not cell count, for the same reason that test checks
 * lava by mass: a liquid's cell count is free to change as it spreads or
 * merges without a drop being lost. */
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
