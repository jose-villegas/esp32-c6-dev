/*=============================================================================
 * Portable suite: the falling-sand automaton - water's foam, gathered at
 * crevices.
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

#include "material_palette.h"
#include "sand.h"
#include "sand_priv.h"
#include "util/intmath.h"
#include "suite_sand_common.h"

/*=============================================================================
 * WATER'S FOAM - gathered at crevices, never on a flat run.
 *
 * material_colours()'s own top comment (material.c) has the full account of
 * why curvature - and only curvature - decides this: measured on real
 * sloshing water, a flat pool's rim is non-flat in 4% of its cells and a
 * pool two steps into a 75 degree tilt is non-flat in 94% of them, which is
 * why no separate "is it moving" signal is wired in here or anywhere else.
 *
 * None of these four tests can reach into material.c's own `water_foam`
 * constant - it is file-static, the same way stone_speckle already is,
 * and these tests reach material_colours() only through
 * material.h same as any other caller. Instead they lean on
 * material_set_gravity(0, 0), which zeroes liquid_spec[] entirely (see that
 * function's own free-fall branch), so that "did NOT foam" has an exact,
 * checkable answer: the plain fill-indexed palette entry, with no shift
 * applied at all. Foam is the one thing left that can make a rim cell
 * disagree with that value once gravity contributes nothing. */

/* Every test below reads this rim cell's own fill, at variant 12 - deep
 * enough that the pale/dark ends of the ramp are not near either clamp,
 * so a coincidental match with the foam colour is not a risk worth
 * chasing down. */
#define FOAM_TEST_FILL 12

/* THE CURVATURE GATE ITSELF - the property every other foam test assumes
 * without re-checking. A FLAT rim (exactly 3 of 8 neighbours empty, the
 * shape of the top of an ordinary settled pool - one cardinal side plus
 * the two diagonals that lean against it) must never foam, at ANY hash;
 * sweeping all 8 values is what tells "never" apart from "not at this one
 * hash I happened to try". A cell exposed on all 8 sides - as curved as a
 * rim on this board can get - must foam for AT LEAST SOME hashes: foam is
 * a dither (see water_foam_threshold's own comment in material.c), so it
 * will not be every hash either, and asserting that would be asserting
 * something the design never promised. */
static void test_water_foams_where_its_rim_is_curved(void)
{
    material_set_gravity(0, 0);
    const gfx_color_t *pal = material_palette();
    const gfx_color_t plain = pal[CELL_MAKE(MAT_WATER, FOAM_TEST_FILL)];

    const unsigned flat_mask = MATERIAL_EDGE_UP | MATERIAL_EDGE_UP_LEFT |
                               MATERIAL_EDGE_UP_RIGHT;
    const unsigned spike_mask =
        MATERIAL_EDGE_LEFT | MATERIAL_EDGE_RIGHT | MATERIAL_EDGE_UP |
        MATERIAL_EDGE_DOWN | MATERIAL_EDGE_UP_LEFT | MATERIAL_EDGE_UP_RIGHT |
        MATERIAL_EDGE_DOWN_LEFT | MATERIAL_EDGE_DOWN_RIGHT;

    int spike_foamed = 0;
    for (unsigned hash = 0; hash < 8u; hash++) {
        gfx_color_t flat_col[3], spike_col[3];
        material_colours(CELL_MAKE(MAT_WATER, FOAM_TEST_FILL), hash,
                         flat_mask, 255u, flat_col);
        material_colours(CELL_MAKE(MAT_WATER, FOAM_TEST_FILL), hash,
                         spike_mask, 255u, spike_col);

        char why[192];
        snprintf(why, sizeof why,
                 "a flat water rim (curvature 0) must never foam, at hash "
                 "%u - foam appearing on a straight run means the gate is "
                 "reading something other than curvature", hash);
        TEST_ASSERT_EQUAL_MESSAGE(plain, flat_col[0], why);

        if (spike_col[0] != plain) {
            spike_foamed++;
        }
    }

    TEST_ASSERT_GREATER_THAN_INT_MESSAGE(0, spike_foamed,
        "a rim cell exposed on all 8 sides is as curved as this board's "
        "rims get, and must foam for at least some of the 8 hash values - "
        "if none of them do, curvature is not reaching the foam gate at "
        "all");
}

/* GUARDS CHANGE 4 - raising water_foam_threshold's non-zero entries (change
 * 4: { 0, 2, 4, 6 } to { 0, 3, 5, 7 }, to make the alternating foam actually
 * visible) must not touch the ONE entry that is not a tuning knob at all:
 * curvature 0, a flat rim, has to stay exactly 0. That is the one shape on
 * this board that must never sprout foam - the top of a still pool - and
 * raising the OTHER three thresholds is exactly the kind of edit that could
 * bump this one too by a slip of the same find-and-replace, since all four
 * entries sit in one small table (see water_foam_threshold in material.c).
 *
 * test_water_foams_where_its_rim_is_curved above already sweeps this same
 * flat shape across all 8 hashes, but always at whatever the foam phase
 * happened to be left at. That is not enough here: the dither compares
 * `hash + foam_phase * 0x9E37u` against the threshold, so a mistake that
 * raised the flat entry from 0 to something small - say 1 - would still
 * read as "never foams" for MOST hash/phase combinations and only show up
 * at the few where the mixed value happens to land under it. Sweeping the
 * full 8x8 grid of hash and phase is what makes "never" mean never rather
 * than "not at the one combination this test happened to try". */
static void test_a_flat_rim_still_never_foams(void)
{
    material_set_gravity(0, 0);   /* no specular term to confuse a
                                            * pure foam comparison with */
    const gfx_color_t *pal = material_palette();
    const gfx_color_t plain = pal[CELL_MAKE(MAT_WATER, FOAM_TEST_FILL)];

    /* Curvature 0: exactly 3 of 8 neighbours empty - one cardinal side plus
     * the two diagonals that lean against it, the shape of the top of an
     * ordinary settled pool. Same shape test_water_foams_where_its_rim_is_
     * curved already uses for its own flat check. */
    const unsigned flat_mask = MATERIAL_EDGE_UP | MATERIAL_EDGE_UP_LEFT |
                               MATERIAL_EDGE_UP_RIGHT;

    for (unsigned phase = 0; phase < 8u; phase++) {
        material_set_foam_phase(phase);
        for (unsigned hash = 0; hash < 8u; hash++) {
            gfx_color_t col[3];
            material_colours(CELL_MAKE(MAT_WATER, FOAM_TEST_FILL), hash,
                             flat_mask, 255u, col);

            char why[192];
            snprintf(why, sizeof why,
                     "a flat water rim (curvature 0) must never foam, at "
                     "hash %u and phase %u - if this ever foams, the "
                     "raised thresholds from change 4 have bled into the "
                     "one entry that must stay exactly 0", hash, phase);
            TEST_ASSERT_EQUAL_MESSAGE(plain, col[0], why);
        }
    }

    material_set_foam_phase(0);   /* leave global state as later tests
                                   * assume it */
}

/* Oil, lava and acid share water's rim code path - the fill-indexed lookup
 * shifted by liquid_spec[] - right up until the id check that hands water
 * off into foam. This is the "water only" constraint, and the failure mode
 * it exists to catch is specific: putting the id check one level too high
 * (or leaving it out) would foam every liquid's rim alike, since curvature
 * itself does not know or care which liquid it is measuring.
 *
 * Same high-curvature shape as the previous test's spike_mask, and the
 * same hash sweep - if water can be made to foam by this shape, these
 * three must be provably immune to it under the exact same inputs, not
 * just "probably fine" under whatever the loop's default happened to be. */
static void test_only_water_foams(void)
{
    material_set_gravity(0, 0);
    const gfx_color_t *pal = material_palette();
    const unsigned spike_mask =
        MATERIAL_EDGE_LEFT | MATERIAL_EDGE_RIGHT | MATERIAL_EDGE_UP |
        MATERIAL_EDGE_DOWN | MATERIAL_EDGE_UP_LEFT | MATERIAL_EDGE_UP_RIGHT |
        MATERIAL_EDGE_DOWN_LEFT | MATERIAL_EDGE_DOWN_RIGHT;
    static const uint8_t non_water[] = { MAT_OIL, MAT_LAVA, MAT_ACID };

    for (unsigned k = 0; k < sizeof non_water / sizeof non_water[0]; k++) {
        const uint8_t id = non_water[k];
        const gfx_color_t plain = pal[CELL_MAKE(id, FOAM_TEST_FILL)];

        for (unsigned hash = 0; hash < 8u; hash++) {
            gfx_color_t col[3];
            material_colours(CELL_MAKE(id, FOAM_TEST_FILL), hash, spike_mask,
                             255u,
                             col);

            char why[128];
            snprintf(why, sizeof why,
                     "%s at maximum rim curvature must paint exactly what "
                     "it painted before foam existed, at hash %u - only "
                     "water may foam",
                     material_by_id((material_id_t)id)->name, hash);
            TEST_ASSERT_EQUAL_MESSAGE(plain, col[0], why);
        }
    }
}

/* Guards the interior fix 6a05faa exists for: `mask == 0` (no cardinal
 * side open) must keep painting the flat body colour regardless of what
 * the diagonal bits say, because an interior cell is never a rim and
 * foam is a rim-only decoration. Swept across masks that are pure
 * diagonal - no cardinal bit at all - specifically because that is the
 * shape a broken cardinal test would miss: a mistake that gated foam (or
 * the rim split generally) on `mask != 0` instead of
 * `mask & MATERIAL_EDGE_CARDINAL` would light these up as rim cells, and
 * every one of them must still read as plain interior water instead. */
static void test_a_liquid_interior_never_foams(void)
{
    const gfx_color_t *pal = material_palette();
    const gfx_color_t body = pal[CELL_MAKE(MAT_WATER, MASS_MAX)];

    static const unsigned interior_masks[] = {
        0u,
        MATERIAL_EDGE_UP_LEFT,
        MATERIAL_EDGE_UP_RIGHT,
        MATERIAL_EDGE_DOWN_LEFT,
        MATERIAL_EDGE_DOWN_RIGHT,
        MATERIAL_EDGE_UP_LEFT | MATERIAL_EDGE_UP_RIGHT |
            MATERIAL_EDGE_DOWN_LEFT | MATERIAL_EDGE_DOWN_RIGHT,
    };

    for (unsigned k = 0;
         k < sizeof interior_masks / sizeof interior_masks[0]; k++) {
        for (unsigned hash = 0; hash < 8u; hash++) {
            gfx_color_t col[3];
            material_colours(CELL_MAKE(MAT_WATER, FOAM_TEST_FILL), hash,
                             interior_masks[k], 255u, col);

            char why[192];
            snprintf(why, sizeof why,
                     "an interior water cell (mask %#x, hash %u) must "
                     "paint the flat body colour - diagonal bits with no "
                     "cardinal side open do not make a cell a rim, and "
                     "only a rim may foam", interior_masks[k], hash);
            TEST_ASSERT_EQUAL_MESSAGE(body, col[0], why);
        }
    }
}

/* THE REGRESSION GUARD for the trap the diagonal bits opened. Before this
 * change, `mask != 0` was "is this cell an edge at all", and it was
 * correct because the mask held nothing but the four cardinals. Adding
 * bits 4-7 makes that test silently wrong: a cell with every cardinal
 * neighbour occupied and exactly one diagonal empty would newly read as
 * an edge, and glass and stone would start outlining cells they used to
 * paint as solid interior - a change to two materials nobody asked to
 * touch, from a mistake nowhere near either of their own code.
 *
 * Checked by comparing a diagonal-only mask directly against mask 0 for
 * glass, stone, and a liquid: if MATERIAL_EDGE_CARDINAL is not what gates
 * the edge test (or the mask ever changes shape again), this is the test
 * that goes red, not some unrelated glass or stone test that merely
 * happens to exercise an edge. */
static void test_a_diagonal_neighbour_alone_is_not_an_edge(void)
{
    const unsigned diagonal_only = MATERIAL_EDGE_UP_LEFT;

    {
        gfx_color_t interior[3], diagonal[3];
        const material_pattern_t pat_i = material_colours(
            CELL_MAKE(MAT_GLASS, 5), 1u, 0u, 255u, interior);
        const material_pattern_t pat_d = material_colours(
            CELL_MAKE(MAT_GLASS, 5), 1u, diagonal_only, 255u, diagonal);

        TEST_ASSERT_EQUAL_MESSAGE(pat_i, pat_d,
            "a lone diagonal neighbour must not change glass's pattern - "
            "if this differs, glass just grew an outline nobody drew");
        TEST_ASSERT_EQUAL_MESSAGE(interior[0], diagonal[0],
            "glass's body colour must be identical with a lone diagonal "
            "neighbour empty - the cardinal test is what decides an edge, "
            "not `mask != 0`");
    }

    {
        gfx_color_t interior[3], diagonal[3];
        material_colours(CELL_MAKE(MAT_STONE, 5), 1u, 0u, 255u,
                         interior);
        material_colours(CELL_MAKE(MAT_STONE, 5), 1u, diagonal_only,
                         255u,
                         diagonal);

        TEST_ASSERT_EQUAL_MESSAGE(interior[0], diagonal[0],
            "a lone diagonal neighbour must not switch stone onto its "
            "edge speckle - stone_edge_speckle vs stone_speckle must both "
            "still read as `mask & MATERIAL_EDGE_CARDINAL`, not `mask`");
    }

    {
        gfx_color_t interior[3], diagonal[3];
        material_colours(CELL_MAKE(MAT_WATER, FOAM_TEST_FILL), 1u, 0u,
                         255u,
                         interior);
        material_colours(CELL_MAKE(MAT_WATER, FOAM_TEST_FILL), 1u,
                         diagonal_only, 255u, diagonal);

        TEST_ASSERT_EQUAL_MESSAGE(interior[0], diagonal[0],
            "a lone diagonal neighbour must not turn an interior water "
            "cell into a rim - the interior/rim split reads "
            "MATERIAL_EDGE_CARDINAL exactly like glass and stone do, and "
            "a rim wrongly declared here could even start foaming");
    }
}

/*=============================================================================
 * FOAM ANIMATES: a phase, mixed into the dither, so the same shape keeps
 * showing a DIFFERENT set of foamed cells from one frame to the next.
 *
 * Before this, foam was gated purely by (hash & 7u) against curvature's
 * threshold - stable for as long as the shape held still, which read as a
 * texture painted onto the water rather than something moving on it.
 * material_set_foam_phase() (material.h) adds a second, frame-global input
 * that material_colours() XORs into the hash before the same threshold test,
 * so the same cell's answer keeps changing while its curvature does not.
 *
 * All three tests below share the same high-curvature mask - all eight
 * neighbours empty, as curved as a rim on this board gets - because a flat
 * mask's threshold is 0 (see water_foam_threshold's own comment in
 * material.c) and `(anything) & 7u < 0` can never be true: a flat cell
 * cannot be made to foam by ANY hash or phase, which would make it useless
 * for pinning that phase changes the answer. */
static const unsigned foam_spike_mask =
    MATERIAL_EDGE_LEFT | MATERIAL_EDGE_RIGHT | MATERIAL_EDGE_UP |
    MATERIAL_EDGE_DOWN | MATERIAL_EDGE_UP_LEFT | MATERIAL_EDGE_UP_RIGHT |
    MATERIAL_EDGE_DOWN_LEFT | MATERIAL_EDGE_DOWN_RIGHT;

/* THE PHASE ITSELF CHANGES THE ANSWER, for one cell whose shape and hash
 * never change. A fixed hash at maximum curvature is swept across sixteen
 * phase values - two full periods of the 3-bit dither the mixing formula
 * cycles through, so a period this test happened to straddle badly cannot
 * hide either outcome - and both a foaming and a non-foaming phase must
 * turn up. Missing either half is a real, different failure: never foaming
 * means material_set_foam_phase() is not reaching the dither at all; always
 * foaming means something ELSE (the fixed hash, the fixed curvature) is
 * deciding this and the phase is doing nothing. Pins CHANGE 1 - the phase
 * existing and actually being read. */
static void test_foam_moves_between_frames(void)
{
    const gfx_color_t *pal = material_palette();
    const gfx_color_t plain = pal[CELL_MAKE(MAT_WATER, FOAM_TEST_FILL)];
    const unsigned fixed_hash = 3u;   /* arbitrary - any value works except
                                       * one that happens to sit exactly on
                                       * the threshold boundary for every
                                       * phase in the sweep, which 3 does
                                       * not */

    bool ever_foamed = false;
    bool ever_plain = false;

    for (unsigned phase = 0; phase < 16u; phase++) {
        material_set_foam_phase(phase);

        gfx_color_t col[3];
        material_colours(CELL_MAKE(MAT_WATER, FOAM_TEST_FILL), fixed_hash,
                         foam_spike_mask, 255u, col);

        if (col[0] != plain) {
            ever_foamed = true;
        } else {
            ever_plain = true;
        }
    }
    material_set_foam_phase(0);   /* leave global state as later tests
                                   * assume it, the same as material_set_
                                   * gravity(0, 0) does at the top of other
                                   * tests in this file */

    TEST_ASSERT_TRUE_MESSAGE(ever_foamed,
        "a fixed hash at maximum curvature must foam for at least one of "
        "the sixteen phases swept here - if it never does, "
        "material_set_foam_phase() is not reaching the dither at all");
    TEST_ASSERT_TRUE_MESSAGE(ever_plain,
        "and the same fixed hash, same shape, must ALSO read as plain rim "
        "for at least one of those sixteen phases - foaming at every one of "
        "them means the cell's shape is what decided this, not the phase, "
        "and the animation this test exists to pin is not happening");
}

/* THE WINDOW MUST ROTATE, NOT STALL - the property ADD buys and XOR
 * broke, and the one that actually matters to how foam reads on the panel.
 *
 * An earlier version of this test used XOR and checked a different,
 * WRONG property: that two widely-separated phases (0 and 6) disagreed
 * about a handful of hashes sharing one blob. That is not a unison bug -
 * cells inside the same 2x2 blob are SUPPOSED to agree, by design (see
 * test_foam_blobs_are_bigger_than_one_cell) - and it never caught the
 * actual defect, which is that XOR's mixing can leave the foaming set
 * IDENTICAL between two phases RIGHT NEXT TO EACH OTHER. Measured on a
 * real sloshing scene at medium curvature, phase 1 to phase 2 changed
 * exactly zero cells out of 635 - foam that is supposed to shimmer every
 * tick instead sat there unchanged for a full step, indistinguishable
 * from the stable dither this whole change exists to replace.
 *
 * So this test checks the two properties that actually separate a
 * shimmer from either failure mode, swept across a full cycle of all 8
 * phases and at each of the three curvatures the threshold table
 * distinguishes (masks chosen for empty-neighbour counts of 4, 1 and 8 -
 * curvature 1, 2 and 3 respectively; see material_colours()'s own comment
 * on curvature for the count-to-curvature arithmetic), against a spread of
 * eight DISTINCT hash values (0 through 7, a complete residue set) rather
 * than a handful of real coordinates that could incidentally land in one
 * blob:
 *
 *   NEITHER DEGENERATE. At any single phase, the foaming subset of the
 *   eight hashes must be neither all of them nor none of them - the
 *   genuine unison guard. This was never actually broken by XOR (with
 *   only water_foam_threshold[curvature] of 8 values ever under the
 *   threshold, the rim cannot turn wholly on or off under any mixing that
 *   only permutes those 8 values) but is worth pinning in its own right.
 *
 *   NEVER STALLS. No two phases NEXT TO EACH OTHER may produce the
 *   identical foaming subset, over the full 8-phase cycle. This is the
 *   property XOR actually failed, at all three curvatures, worst at
 *   medium (see the long comment on the mixing site in material.c for the
 *   measured 4-of-8, 6-of-8, 4-of-8 breakdown) - and the one a future
 *   change back to XOR would break again, which is exactly what this
 *   assertion exists to catch. */
static void test_foam_never_stalls_between_frames(void)
{
    const gfx_color_t *pal = material_palette();
    const gfx_color_t plain = pal[CELL_MAKE(MAT_WATER, FOAM_TEST_FILL)];

    /* Empty-neighbour counts of 4, 1 and 8 give curvatures 1, 2 and 3 -
     * one mask per row of water_foam_threshold[] that the flat entry
     * (curvature 0, threshold 0) does not already cover trivially. */
    const unsigned curvature1_mask =
        MATERIAL_EDGE_UP | MATERIAL_EDGE_DOWN | MATERIAL_EDGE_UP_LEFT |
        MATERIAL_EDGE_UP_RIGHT;                        /* count 4 */
    const unsigned curvature2_mask = MATERIAL_EDGE_UP;  /* count 1 */
    const unsigned masks[3] = { curvature1_mask, curvature2_mask,
                               foam_spike_mask /* count 8 */ };
    static const unsigned curvatures[3] = { 1, 2, 3 };

    for (unsigned m = 0; m < 3; m++) {
        bool foamed[8][8];   /* [phase][hash] */

        for (unsigned phase = 0; phase < 8u; phase++) {
            material_set_foam_phase(phase);
            for (unsigned hash = 0; hash < 8u; hash++) {
                gfx_color_t col[3];
                material_colours(CELL_MAKE(MAT_WATER, FOAM_TEST_FILL), hash,
                                 masks[m], 255u, col);
                foamed[phase][hash] = (col[0] != plain);
            }
        }

        for (unsigned phase = 0; phase < 8u; phase++) {
            unsigned count = 0;
            for (unsigned hash = 0; hash < 8u; hash++) {
                if (foamed[phase][hash]) {
                    count++;
                }
            }

            char why[192];
            snprintf(why, sizeof why,
                     "at curvature %u, phase %u: the foaming subset of all "
                     "8 hashes must be neither every one of them nor none "
                     "of them, or the rim is pulsing as a whole instead of "
                     "shimmering cell by cell", curvatures[m], phase);
            TEST_ASSERT_TRUE_MESSAGE(count > 0 && count < 8u, why);
        }

        for (unsigned phase = 0; phase < 8u; phase++) {
            const unsigned next = (phase + 1u) % 8u;
            bool differs = false;
            for (unsigned hash = 0; hash < 8u; hash++) {
                if (foamed[phase][hash] != foamed[next][hash]) {
                    differs = true;
                }
            }

            char why[256];
            snprintf(why, sizeof why,
                     "at curvature %u, phase %u to phase %u: the foaming "
                     "set must change - two phases next to each other "
                     "producing the identical set is exactly the stall "
                     "XOR mixing introduced, measured as zero changed "
                     "cells out of 635 on a real sloshing scene",
                     curvatures[m], phase, next);
            TEST_ASSERT_TRUE_MESSAGE(differs, why);
        }
    }

    material_set_foam_phase(0);   /* leave global state as later tests
                                   * assume it */
}

/* Mirrors FOAM_BLOB_SHIFT in app_sand.c. Duplicated rather than shared,
 * because paint_row_n() - the only thing that actually applies the shift -
 * is static to that file and this suite links against material.c alone, on
 * the host. If FOAM_BLOB_SHIFT ever moves, this has to move with it by
 * hand; there is no way around that without exposing a knob that exists
 * only to be tuned by eye on the device. */
#define TEST_FOAM_BLOB_SHIFT 3

/* FOAM'S BLOBS ARE ACTUALLY BIGGER THAN ONE CELL - the coarse-sampling half
 * of change 2, checked directly against material_grain_hash() rather than
 * through paint_row_n(), which cannot be linked into a host test (it is
 * `static` in app_sand.c).
 *
 * Two claims, both necessary. WITHIN an 8x8 block, cells must collapse to
 * the identical shifted coordinate and therefore the identical hash -
 * shifting cx and cy right by three turns every coordinate 0-7 into the
 * same value, and likewise 8-15 - which is the entire mechanism a blob
 * rests on: paint_row_n() hands every cell of a block this same hash, so
 * they can only ever agree about whether to foam. The four cells sampled
 * below are the four CORNERS of that 8x8 block - (0,0), (7,0), (0,7) and
 * (7,7) relative to the block's own start - rather than an adjacent pair:
 * an 8x8 block is 4x the area a 4x4 one was, so the adjacent-corner
 * sub-sample that used to stand in for "the whole block" upstairs would
 * now cover only a sliver of it. Testing the actual extremes proves the
 * WHOLE block agrees, corner to corner, not just two cells that happen to
 * sit next to each other. BETWEEN two blocks that hash must generally
 * differ, or the "coarse grid" has collapsed to one giant block covering
 * the whole board instead of a grid of small ones - checked at two block
 * starts eight cells apart, exactly one block width, so an off-by-one in
 * where a block begins cannot hide behind a coincidence. */
static void test_foam_blobs_are_bigger_than_one_cell(void)
{
    static const int block_starts[] = { 0, 8 };
    unsigned block_hash[2];
    const int cy0 = 0;   /* 0 and cy0+7 = 7 both floor to the same block
                         * only when the block starts at 0 - the corners
                         * are the block's own first and last row, so
                         * there is no mod-arithmetic edge case to reason
                         * about the way an interior cy would need. */

    for (unsigned b = 0; b < 2; b++) {
        const int cx0 = block_starts[b];
        const unsigned top_left = material_grain_hash(
            cx0 >> TEST_FOAM_BLOB_SHIFT, cy0 >> TEST_FOAM_BLOB_SHIFT);
        const unsigned top_right = material_grain_hash(
            (cx0 + 7) >> TEST_FOAM_BLOB_SHIFT, cy0 >> TEST_FOAM_BLOB_SHIFT);
        const unsigned bottom_left = material_grain_hash(
            cx0 >> TEST_FOAM_BLOB_SHIFT, (cy0 + 7) >> TEST_FOAM_BLOB_SHIFT);
        const unsigned bottom_right = material_grain_hash(
            (cx0 + 7) >> TEST_FOAM_BLOB_SHIFT, (cy0 + 7) >> TEST_FOAM_BLOB_SHIFT);

        char why[256];
        snprintf(why, sizeof why,
                 "all four corner cells of the 8x8 block starting at "
                 "(%d,%d) must feed foam the identical hash, or foam "
                 "speckles single cells the way every other material's "
                 "grain does instead of clustering into the blob it is "
                 "supposed to", cx0, cy0);
        TEST_ASSERT_EQUAL_MESSAGE(top_left, top_right, why);
        TEST_ASSERT_EQUAL_MESSAGE(top_left, bottom_left, why);
        TEST_ASSERT_EQUAL_MESSAGE(top_left, bottom_right, why);

        block_hash[b] = top_left;
    }

    TEST_ASSERT_TRUE_MESSAGE(block_hash[0] != block_hash[1],
        "two blocks eight cells apart must generally get DIFFERENT hashes, "
        "or the coarse sampling has collapsed to one giant block instead "
        "of a grid of small ones");
}

void run_sand_foam_suite(void)
{
    RUN_TEST(test_water_foams_where_its_rim_is_curved);
    RUN_TEST(test_a_flat_rim_still_never_foams);
    RUN_TEST(test_only_water_foams);
    RUN_TEST(test_a_liquid_interior_never_foams);
    RUN_TEST(test_a_diagonal_neighbour_alone_is_not_an_edge);
    RUN_TEST(test_foam_moves_between_frames);
    RUN_TEST(test_foam_never_stalls_between_frames);
    RUN_TEST(test_foam_blobs_are_bigger_than_one_cell);
}

SUITE_REGISTER(run_sand_foam_suite);
