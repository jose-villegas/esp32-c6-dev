/*=============================================================================
 * sand_priv - internals shared between sand.c and sand_liquid.c.
 *
 * Not a public header: nothing outside this module includes it, and nothing
 * in it is part of sand.h's API. It exists only because splitting the liquid
 * logic into its own file left a few things - marking a row dirty, finding
 * the row a move would land in - needed on both sides of that split.
 *
 * dest_row() and mark_rows() stay `static inline` here rather than becoming
 * ordinary functions defined once and declared extern: both sit on the
 * hottest path in the simulation - called per row, and per move,
 * respectively - and a call across translation units is not guaranteed to
 * inline the way a call within the same file is. A header of small inline
 * functions gives each .c file its own inlinable copy, which is what lets the
 * file split without also risking a performance regression for it - see the
 * frame-budget tests in suite_sand_perf.c, which is exactly what would catch it if
 * this ever stopped being true.
 *
 * THE REAL CRITERION FOR WHAT ELSE LIVES HERE, STATED HONESTLY: not every
 * `static` helper in sand.c that could sit here (this header is app-internal
 * and portable either way, so there is no layering reason it could not) does.
 * blocker_normal(), reflect_off_normal() and impulse_drag_of() are here
 * because a test needed to call them directly - the sand test suite (split
 * across suite_sand_*.c) cannot reach a
 * function `static` inside sand.c at all, only ones declared where it can
 * include them, and this header is that place. can_impulse_enter(),
 * can_impulse_enter_gravity_ward() and impulse_gravity_candidates() stay
 * `static` in sand.c, right next to step_impulses(), because nothing has yet
 * needed to drive one of them in isolation - every existing test reaches
 * them through step_impulses()'s own observable behaviour instead. All six
 * are equally pure - none touches anything this header's own functions do
 * not already touch - so "pure enough to live here" was never the actual
 * test being applied, whatever an earlier version of this comment implied.
 * An adversarial architecture review (bd esp32c6-w2h) named this directly:
 * the three left in sand.c are also the three with a documented history of
 * their own two call sites quietly disagreeing about what they compute (see
 * impulse_gravity_candidates()'s own comment in sand.c for that history) -
 * exactly the kind of bug a direct test would have caught sooner. Moving
 * them is not this fix: stating the true rule is, so the next helper this
 * file's own history repeats on is moved (or not) on purpose, by whoever
 * next needs to test it directly, rather than by a guess about purity that
 * was never really what decided the first six.
 *===========================================================================*/
#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "sand.h"

/* NULL if off grid; vertical bounds checked per row, not per grain. */
static inline uint8_t *dest_row(const sand_t *s, int y)
{
    if (y < 0 || y >= s->h) {
        return NULL;
    }
    return s->cells + (size_t)y * (size_t)s->w;
}

static inline void mark_rows(sand_t *s, int y0, int y1)
{
    if (s->dirty_rows != NULL) {
        if ((unsigned)y0 < (unsigned)s->h) {
            s->dirty_rows[y0] = 1;
        }
        if ((unsigned)y1 < (unsigned)s->h) {
            s->dirty_rows[y1] = 1;
        }
    }
}

/* A liquid cell at row `y` just turned from EMPTY into occupied - see
 * pour_into()'s `was_empty` return, its only caller - which is the only
 * event that can move where a puddle's surface is, and therefore the only
 * event that can make app_sand.c's LOCAL DEPTH render stale below it (see
 * that mechanism's own long comment in app_sand.c, "STALE READINGS UNDER
 * THE DIRTY-ROW OPTIMISATION ARE ACCEPTED"). Ordinary mass moving between
 * two ALREADY-liquid cells - the common case, every step a pool is settling
 * or sloshing - never calls this: it cannot change the depth topology, only
 * redistribute mass within it, so marking dirty for it would repaint a
 * settled reservoir on every step something merely levels out, exactly the
 * "updating all water just because of a pour" cost this exists to avoid.
 *
 * Marks a band of rows, not two points the way mark_rows() does - a settled
 * column's stored local depth for anything within MATERIAL_LIQUID_DEPTH_BAND
 * cells of the new surface can now read differently once repainted, and
 * anything further than that already saturates to the same flat body colour
 * whether the true depth is one cell more or a hundred, so there is nothing
 * further out worth invalidating. Direction-agnostic (both above and below
 * `y`) rather than reasoning about which way is "toward depth" this frame -
 * that answer lives in app_sand.c's own gravity-derived bookkeeping
 * (local_depth_v_reverse/local_depth_h_reverse), and coupling the
 * simulation to it here would be a layering mistake for a mark that is
 * already cheap enough to just cover both directions. */
static inline void mark_depth_band(sand_t *s, int y)
{
    if (s->dirty_rows == NULL) {
        return;
    }
    int y0 = y - MATERIAL_LIQUID_DEPTH_BAND;
    int y1 = y + MATERIAL_LIQUID_DEPTH_BAND;
    if (y0 < 0) {
        y0 = 0;
    }
    if (y1 >= s->h) {
        y1 = s->h - 1;
    }
    memset(&s->dirty_rows[y0], 1, (size_t)(y1 - y0 + 1));
}

#define BLOCK_SETTLED_NEAREST 0x1
#define BLOCK_SETTLED_OTHER   0x2
#define BLOCK_ACTIVE          0x4

/* Whether the main sweep saw a liquid cell in this block, and whether this
 * block or any of its 8 neighbours did - the pair that lets the cross-flow
 * pass skip whole block-wide spans instead of testing every cell of the grid.
 * Two bits, not one, because the expansion cannot be done at write time: the
 * sweep sets HAS_LIQUID per block as it goes, and a single pass over the
 * blocks turns that into NEAR before equalise_liquids() reads it. Writing the
 * expansion directly would need a "did I already expand from here" guard that
 * a neighbour's expansion would spoil.
 *
 * HAS_LIQUID is cleared each step (compute_settled_bit()) for every block the
 * sweep is about to examine, and left alone for a SETTLED block, which the
 * sweep skips and therefore cannot re-establish it for. That is safe because a
 * settled block's contents did not move.
 *
 * THE INVARIANT, which is what the skip actually rests on: every liquid cell
 * is in a block whose NEAR bit is set. Two cases. Either the sweep saw it, and
 * that block's own HAS_LIQUID is set; or it arrived in that block after the
 * sweep had already walked it, in which case it came from a source cell that
 * the sweep DID see, one cell away - or up to SAND_LIQUID_SIGHT (8) away for a
 * cross-flow transfer, still well under SAND_BLOCK_W - so the source block is
 * this block or an immediate neighbour, and the expansion covers it. Liquid
 * entering the grid from outside (sand_set(), try_spawn_one()) goes through
 * mark_move(), which clears the settled bits on a 3x3 of blocks, so the next
 * sweep is guaranteed to walk the block and see it. Nothing else in the
 * simulation creates a liquid cell without going through that same
 * latch: sand_reactions.c does now make one - snow melts into water -
 * but every placement it makes goes through place_cell(), which calls
 * latch_content_flags() exactly as sand_set() does. sand_gas.c only
 * moves gas. The claim that used to stand here, that sand_reactions.c
 * "only ever writes MAT_FIRE", stopped being true when snow arrived.
 *
 * The contrapositive is what equalise_liquids() uses: a block with NEAR clear
 * provably holds no liquid at all, so skipping its cells changes nothing -
 * including the found_any/may_have_liquid conclusion drawn from that pass. */
#define BLOCK_HAS_LIQUID      0x8
#define BLOCK_LIQUID_NEAR     0x10

static inline uint16_t liquid_mask(void)
{
    uint16_t mask = 0;
    for (int m = 0; m < MATERIAL_MAX; m++) {
        if (material_by_id((material_id_t)m)->kind == KIND_LIQUID) {
            mask |= (uint16_t)(1u << m);
        }
    }
    return mask;
}

static inline int block_of(const sand_t *s, int x, int y)
{
    return (y / SAND_BLOCK_H) * s->block_cols + (x / SAND_BLOCK_W);
}

static inline void wake_blocks_range(sand_t *s, int bx0, int by0, int bx1,
                                     int by1)
{
    if (s->block_state == NULL) {
        return;
    }

    int lo_x = (bx0 < bx1 ? bx0 : bx1) - 1;
    int hi_x = (bx0 > bx1 ? bx0 : bx1) + 1;
    int lo_y = (by0 < by1 ? by0 : by1) - 1;
    int hi_y = (by0 > by1 ? by0 : by1) + 1;

    if (lo_x < 0) {
        lo_x = 0;
    }
    if (lo_y < 0) {
        lo_y = 0;
    }
    if (hi_x >= s->block_cols) {
        hi_x = s->block_cols - 1;
    }
    if (hi_y >= s->block_rows) {
        hi_y = s->block_rows - 1;
    }

    for (int by = lo_y; by <= hi_y; by++) {
        for (int bx = lo_x; bx <= hi_x; bx++) {
            s->block_state[by * s->block_cols + bx] &=
                (uint8_t)~(BLOCK_SETTLED_NEAREST | BLOCK_SETTLED_OTHER);
        }
    }
    s->block_state[by0 * s->block_cols + bx0] |= BLOCK_ACTIVE;
    s->block_state[by1 * s->block_cols + bx1] |= BLOCK_ACTIVE;
}

static inline bool any_neighbor_active(const sand_t *s, int bx, int by)
{
    const int lo_x = (bx > 0) ? bx - 1 : bx;
    const int hi_x = (bx + 1 < s->block_cols) ? bx + 1 : bx;
    const int lo_y = (by > 0) ? by - 1 : by;
    const int hi_y = (by + 1 < s->block_rows) ? by + 1 : by;

    for (int ny = lo_y; ny <= hi_y; ny++) {
        for (int nx = lo_x; nx <= hi_x; nx++) {
            if (nx == bx && ny == by) {
                continue;
            }
            if (s->block_state[ny * s->block_cols + nx] & BLOCK_ACTIVE) {
                return true;
            }
        }
    }
    return false;
}

/* Expands BLOCK_HAS_LIQUID, counts itself too */
static inline bool block_or_neighbour_has_liquid(const sand_t *s, int bx,
                                                 int by)
{
    const int lo_x = (bx > 0) ? bx - 1 : bx;
    const int hi_x = (bx + 1 < s->block_cols) ? bx + 1 : bx;
    const int lo_y = (by > 0) ? by - 1 : by;
    const int hi_y = (by + 1 < s->block_rows) ? by + 1 : by;

    for (int ny = lo_y; ny <= hi_y; ny++) {
        for (int nx = lo_x; nx <= hi_x; nx++) {
            if (s->block_state[ny * s->block_cols + nx] & BLOCK_HAS_LIQUID) {
                return true;
            }
        }
    }
    return false;
}

/* Marks block (bx,by) - the one containing (x,y) - and its up to 8
 * neighbours unsettled, unconditionally. Used only by touches that
 * happen OUTSIDE the gravity sweep (sand_set(), sand_erase(),
 * try_spawn_one(), and liquid's cross-flow pass in sand_liquid.c), where
 * there is no `moved_here`-style bookkeeping for
 * the pull-based any_neighbor_active() check above to observe on its
 * own next step.
 *
 * Sweep-internal moves need no equivalent: step_one_block() already
 * sets BLOCK_ACTIVE on its own (source) block directly from
 * `moved_here`, independent of any wake call, and a grain only ever
 * moves one cell - so a destination block, if different from the
 * source, is always that source block's immediate neighbour, which
 * any_neighbor_active() will find active on its own the moment the
 * finalisation pass runs. An external touch has no such source block
 * whose own activity a neighbour could observe, which is why it needs
 * to expand to neighbours itself, right here, instead.
 *
 * Unconditional 3x3 expansion, not edge-aware the way point_reach() had
 * to be: these calls are user-interaction/cross-flow rate, not once per
 * grain move, so the precision that mechanism needed to stay cheap on
 * the sweep's hot path is not needed here - see
 * test_undermining_a_sleeping_pile_collapses_it, which is what would
 * catch this being narrowed later: erasing a grain must wake whatever
 * was resting on it in a NEIGHBOURING block, not just the block the
 * erased cell itself was in, and there is no sweep-internal activity of
 * its own to fall back on for a block that never gets examined at all. */
static inline void wake_block_and_neighbors(sand_t *s, int x, int y)
{
    if (s->block_state == NULL) {
        return;
    }
    if ((unsigned)x >= (unsigned)s->w || (unsigned)y >= (unsigned)s->h) {
        return;
    }

    /* Unsigned cast for division: x/y are non-negative grid coords. */
    const int bx = (int)((unsigned)x / SAND_BLOCK_W);
    const int by = (int)((unsigned)y / SAND_BLOCK_H);

    const int lo_x = (bx > 0) ? bx - 1 : bx;
    const int hi_x = (bx + 1 < s->block_cols) ? bx + 1 : bx;
    const int lo_y = (by > 0) ? by - 1 : by;
    const int hi_y = (by + 1 < s->block_rows) ? by + 1 : by;

    for (int ny = lo_y; ny <= hi_y; ny++) {
        for (int nx = lo_x; nx <= hi_x; nx++) {
            s->block_state[ny * s->block_cols + nx] &=
                (uint8_t)~(BLOCK_SETTLED_NEAREST | BLOCK_SETTLED_OTHER);
        }
    }
    s->block_state[by * s->block_cols + bx] |= BLOCK_ACTIVE;
}

/* Latch the may_have_* flags that a newly written cell implies.
 *
 * ONE copy of this list, because there were two and they drifted. sand_set()
 * and try_spawn_one() each carried their own identical run of ifs, and when
 * a fifth flag arrived only one of them learned about it: snow drawn with
 * the BRUSH never woke the reactions pass, so it sat in water forever and
 * never chilled anything, while snow placed by sand_set() melted correctly.
 * Every test used sand_set(), so every test passed. The duplication was
 * flagged in both copies' comments and duplicated anyway.
 *
 * Independent ifs, not an else-if chain: fire is BOTH kind == KIND_GAS
 * (rises through sand_step_gas()) AND reactions[].burns (reacts through
 * sand_step_reactions()) and needs both flags set. An else-if would let the
 * gas branch shadow the burns branch for every fire cell and strand
 * may_have_burning false forever.
 *
 * Takes a CELL rather than a material id because temperature depends on the
 * variant: cold glass has nothing to cool and needs no flag, and gets one
 * from try_heat_transform() the moment a flame reaches it. Snow is cold
 * whatever its variant, so it always sets the flag. */
/* The eight directions, in ring order, so that the two neighbours of any
 * direction are simply the entries either side of it. That is what lets
 * the movement rule work at any gravity angle without eight special cases.
 *
 * Shared rather than private to sand.c because growth needs it too, and
 * for a reason worth writing down: "the two cells either side of up" is
 * NOT up plus a perpendicular. That shortcut is right only while up is
 * axis-aligned. Let the board tilt until up is (-1,-1) and adding the
 * perpendicular (1,-1) gives (0,-2) - two cells away, skipping the one in
 * between - so a tree on a tilted board grew limbs with a gap under them
 * and branches that leapt. Stepping round the ring is the same idea done
 * correctly, and it is what the sweep has always done. */
static inline const int *ring_dir(int i)
{
    static const int ring8[8][2] = {
        {  0,  1 },   /* 0  down            */
        {  1,  1 },   /* 1  down-right      */
        {  1,  0 },   /* 2  right           */
        {  1, -1 },   /* 3  up-right        */
        {  0, -1 },   /* 4  up              */
        { -1, -1 },   /* 5  up-left         */
        { -1,  0 },   /* 6  left            */
        { -1,  1 },   /* 7  down-left       */
    };
    return ring8[i & 7];
}

static inline int ring_of(int dx, int dy)
{
    for (int i = 0; i < 8; i++) {
        const int *d = ring_dir(i);
        if (d[0] == dx && d[1] == dy) {
            return i;
        }
    }
    return 0;   /* unreachable for a unit direction */
}

/* THE BLOCKING SURFACE'S APPROXIMATE NORMAL - the geometry half of the
 * KIND_STATIC wall-bounce in step_impulses()'s blocked branch (sand.c). Looks
 * at the three ring cells centred on the MOVER'S OWN direction of travel -
 * `dir - 1`, `dir`, `dir + 1`, from (x, y) - the same three-cell arc shape
 * cover_mask() (sand_priv.h) settled on, and for the same reason its own
 * comment records: a hand-drawn stone wall bulges one cell past the one
 * below it at every brush step, so a WIDER arc reads that bulge as a
 * corner instead of the flat wall it actually is. Three, not five, here
 * too - do not widen it without the same kind of evidence that comment
 * documents.
 *
 * For every covering cell in the arc (KIND_STATIC only - sand_at()'s
 * out-of-bounds-is-STONE convention folds the grid edge into this for
 * free, which is deliberate, not an oversight: a chunk thrown at the edge
 * of the board bounces off it exactly as it would off a real wall there),
 * the surface is treated as pushing back along THAT cell's own negated
 * unit vector - directly ahead pushes straight back, a diagonal neighbour
 * pushes back-and-across.
 *
 * QUANTISED BY DOMINANCE, NOT BY SIGN - a first version of this took the
 * sign of each summed component independently, and that degenerates for
 * every DIAGONAL `dir`: the arc's centre cell (always covered, or this
 * would never have been called - see the blocked branch's own precondition)
 * contributes -1 to BOTH axes on a diagonal throw, and the two flanks
 * (axis-aligned) can only ever add 0 or -1 to one axis each - never enough
 * to flip a sign back across zero. So a sign-quantised sum for a diagonal
 * `dir` is always exactly the mover's own reverse, whatever the two flanks
 * look like - a diagonal-direction bounce could never glance, only ever
 * reverse, which is wrong: a chunk skimming down-right across a flat floor
 * (down AND down-right covered, right open) should glance up-right off it,
 * not bounce straight back up-left as if it had hit a corner. Comparing the
 * summed components' MAGNITUDES instead - the larger axis wins outright,
 * both axes count only when they are close (within a factor of 2) - fixes
 * this without abandoning integer arithmetic: for the flat-floor case above
 * the vertical push (from two covering cells) dominates the horizontal
 * push (from one), so the normal reads as pure "up" and the mover glances,
 * exactly as it should.
 *
 * This is still a documented APPROXIMATION, not an exact nearest-of-8
 * average, but it is the RIGHT approximation for a flat wall hit
 * square-on: axis-aligned `dir` still always reverses (a flat wall's own
 * normal has no other axis to weigh against), while diagonal `dir` can now
 * genuinely glance when the arc is asymmetric. See
 * test_blocker_normal_and_reflect_off_normal_match_the_exhaustive_arc_table
 * (suite_sand_impulse.c) for the full 8-direction x 4-configuration ground truth
 * this was checked against - do not touch the dominance rule below without
 * updating that table alongside it.
 *
 * Returns -1 - "no normal, the caller falls through to the plain wait" -
 * only when nothing in the arc is KIND_STATIC, or the summed push exactly
 * cancels (kept as a guard; not observed to happen off a 3-cell arc). */
static inline int blocker_normal(const sand_t *s, int x, int y, int dir)
{
    int sx = 0;
    int sy = 0;
    bool any = false;

    for (int i = -1; i <= 1; i++) {
        const int *d = ring_dir(dir + i);
        const cell_t c = sand_at(s, x + d[0], y + d[1]);
        if (CELL_IS_EMPTY(c) || material_of(c)->kind != KIND_STATIC) {
            continue;
        }
        sx -= d[0];
        sy -= d[1];
        any = true;
    }
    if (!any) {
        return -1;
    }
    if (sx == 0 && sy == 0) {
        return -1;
    }

    const int ax = (sx < 0) ? -sx : sx;
    const int ay = (sy < 0) ? -sy : sy;
    const int sgn_x = (sx > 0) - (sx < 0);
    const int sgn_y = (sy > 0) - (sy < 0);

    int nx, ny;
    if (ax >= 2 * ay) {
        nx = sgn_x;
        ny = 0;
    } else if (ay >= 2 * ax) {
        nx = 0;
        ny = sgn_y;
    } else {
        nx = sgn_x;
        ny = sgn_y;
    }
    return ring_of(nx, ny);
}

static inline int reflect_off_normal(int dir, int normal)
{
    const int *d = ring_dir(dir);
    const int *n = ring_dir(normal);
    const int n2 = n[0] * n[0] + n[1] * n[1];
    const int dn = d[0] * n[0] + d[1] * n[1];
    const int rx = d[0] * n2 - 2 * dn * n[0];
    const int ry = d[1] * n2 - 2 * dn * n[1];
    const int qx = (rx > 0) - (rx < 0);
    const int qy = (ry > 0) - (ry < 0);
    if (qx == 0 && qy == 0) {
        return -1;
    }
    const int r = ring_of(qx, qy);
    return (r == dir) ? -1 : r;
}

static inline bool
neighbor_smothers(const sand_t *s, int nx, int ny, int w, int h, uint8_t density)
{
    if ((unsigned)nx >= (unsigned)w || (unsigned)ny >= (unsigned)h) {
        return false;
    }
    const cell_t n = s->cells[(size_t)ny * (size_t)w + (size_t)nx];
    if (CELL_IS_EMPTY(n)) {
        return false;
    }
    const material_t *nm = material_of(n);
    return nm->kind != KIND_LIQUID && nm->density > density;
}

/* THE SHARED "IS THERE A LID OVER ME" PRIMITIVE - bd esp32c6-a2j,
 * replacing cover_count() (sand_reactions.c, shipped in b5e4a61 for
 * esp32c6-mqt), which counted the four SCREEN-fixed cardinals regardless
 * of which way is down and, being built on neighbor_smothers() (which
 * never counts a liquid neighbour, on purpose), could never fire for an
 * interior cell of a pool wider than one cell: at most the cell directly
 * above it ever counted, so a wide pool sealed by a crust never reached
 * the threshold no matter how complete the seal was.
 *
 * THE LID IS THE THREE CELLS CENTRED ON ANTI-GRAVITY - the cell directly
 * opposite gravity and the two diagonals either side of it - and ALL
 * THREE must be covering. Nothing else is ever looked at. What is below
 * a cell (gravity-relative) supports it, and what is beside it walls it
 * in; neither covers it, which is why this rotates with gravity instead
 * of being fixed screen directions, and why the two PERPENDICULARS are
 * left out. They were in, once: the first gravity-relative version
 * (2026-09-02) used a five-cell semi-disc - these three plus the two
 * perpendiculars - needing three covered in a contiguous run. A
 * hand-drawn stone wall is never flat: each brush disc bulges one cell
 * past the one below it, so its inner face has a notch every brush step,
 * and lava settling into a notch saw wall to its side, wall on the
 * diagonal above that side and wall directly above - three, contiguous,
 * "sealed" - while the pool's surface sat wide open one cell over. Every
 * hand-drawn basin blew its own sides out as the lava settled (bursts
 * within 16 steps on the host, wall breached by step 62 at natural odds;
 * a clean one-cell wall never produced a single eligible cell, which is
 * why no test saw it). Dropping the perpendiculars removed every
 * eligible cell in that scene at brush radii 2 to 4 and left the
 * wide-pool-under-a-crust case - the one this exists for - untouched.
 * A pocket with an open SIDE still qualifies as long as its lid is
 * complete; a lid with a gap in it is not a lid. See
 * test_lava_in_a_wall_notch_never_bursts and
 * test_cover_primitive_matches_the_exhaustive_shape_table
 * (suite_sand_lava_burial.c).
 *
 * `mask` is 3 bits: bit i set means ring_dir(anti - 1 + i) covers this
 * cell - bit 1 is anti-gravity itself, bits 0 and 2 the diagonals.
 * COVER_LID is all three. "Covering" is neighbor_smothers()'s test: in
 * bounds, not a liquid, strictly denser than the cell asking. Out of
 * bounds never counts - the board edge is not a container a player
 * built, the same rule gas_ignite_confined() states for its own scan.
 *
 * SETTLED GRAVITY, NOT THE RAW TILT AND NOT THE DITHERED STEP -
 * s->last_load_dx/dy (sand.h) is already an int pair and already one of
 * the eight ring directions, and it is the SETTLED one: the nearest
 * eighth, stable while the board is held still. Every other structural
 * question in the sweep reads it for exactly that reason - this
 * primitive, anchored(), growth's own "which way is up" - and this one
 * is pinned by a test that drives it under sideways gravity specifically
 * to stop it ever regressing to a fixed screen direction
 * (test_a_wide_pool_under_a_sideways_crust_bursts, suite_sand_lava_burial.c).
 *
 * NOT s->last_step_dx/dy, which is the DITHERED direction of one step: a
 * tilt falling between two eighths spends some steps on each, in
 * proportion. That is right for a thing that accumulates over time (a
 * growing stem at its true angle) and wrong here, because a seal is a
 * fact about the geometry rather than a sample of it - dithering would
 * swing the lid between two adjacent orientations every step, so a cell
 * genuinely sealed in one of them would read unsealed in the other and
 * the whole rule would turn into orientation noise. Working off the
 * already-quantised ring direction both matches what the rest of the
 * sweep does and cannot flicker: the eight ring directions are the only
 * inputs this ever sees. */
#define COVER_LID 0x7u

static inline unsigned
cover_mask(const sand_t *s, int x, int y, int w, int h, uint8_t density)
{
    const int anti = ring_of(s->last_load_dx, s->last_load_dy) + 4;
    unsigned mask = 0;
    for (int i = 0; i < 3; i++) {
        const int *d = ring_dir(anti - 1 + i);
        if (neighbor_smothers(s, x + d[0], y + d[1], w, h, density)) {
            mask |= 1u << i;
        }
    }
    return mask;
}

/* Checks if (x, y) has a complete lid over it, gravity-relative. */
static inline bool
covered_at(const sand_t *s, int x, int y, int w, int h, uint8_t density)
{
    return cover_mask(s, x, y, w, h, density) == COVER_LID;
}

static inline uint8_t impulse_drag_of(cell_t displaced)
{
    const material_t *m = material_of(displaced);
    unsigned d = (unsigned)m->density;

    if (m->kind == KIND_LIQUID) {
        return 0u;   /* a fluid parts around a mover - see the constant */
    }
    if (m->kind == KIND_POWDER) {
        d <<= SAND_IMPULSE_DRAG_POWDER_SHIFT;
    }
    return (uint8_t)(d > 255u ? 255u : d);
}

static inline void clear_content_flags(sand_t *s)
{
    s->may_have_liquid      = false;
    s->may_have_gas         = false;
    s->may_have_burning     = false;
    s->may_have_dissolver   = false;
    s->may_have_temperature = false;
    s->may_have_moisture    = false;
    s->may_have_faller      = false;
    s->may_have_heat_holder = false;

    s->may_have_withering   = false;
    s->may_have_condenser   = false;
}

static inline void latch_content_flags(sand_t *s, cell_t cell)
{
    if (CELL_IS_EMPTY(cell)) {
        return;
    }
    const material_t *mat = material_of(cell);
    const reaction_t *r = reaction_of(cell);   /* decodes MAT_EXTENDED */

    if (mat->kind == KIND_LIQUID) {
        s->may_have_liquid = true;
    }
    if (mat->kind == KIND_GAS) {
        s->may_have_gas = true;
    }
    if (cell_is_burning(cell)) {
        s->may_have_burning = true;
    }
    if (r->dissolves) {
        s->may_have_dissolver = true;
    }
    if (r->falls != 0) {
        s->may_have_faller = true;
    }
    if (r->withers != 0) {
        s->may_have_withering = true;
    }
    if (r->condenses != 0) {
        s->may_have_condenser = true;
    }
    if (r->chills != 0 || r->warms != 0 ||
        (r->heat_ramp != 0 && CELL_VARIANT(cell) != SAND_AMBIENT_HEAT)) {
        s->may_have_temperature = true;
    }
    if (r->heat_ramp != 0 ||
        (r->chills != 0 && r->heats_to != 0 && r->heat_chance != 0)) {
        s->may_have_heat_holder = true;
    }
    if (mat->kind == KIND_LIQUID ||
        (r->dries != 0 && moisture_of(cell, r) != 0)) {
        s->may_have_moisture = true;
    }
}

static inline void mark_move(sand_t *s, int x0, int y0, int x1, int y1)
{
    mark_rows(s, y0, y1);
    wake_block_and_neighbors(s, x0, y0);
    wake_block_and_neighbors(s, x1, y1);
}

/* Ticks a transient material's life down by one, per material.h's `decay`
 * field, or clears the cell outright if it was already down to its last
 * tick. Returns whether the cell is still occupied - the caller must not
 * go on to treat a grain that just vanished as still there. Writing the
 * ticked-down value back into `row[x]` before the caller does anything
 * else with `*grain` matters wherever `*grain` gets handed to something
 * like move_to(), which trusts its argument as the thing to place rather
 * than re-reading row[x] itself - a stale life value there would carry
 * an already-dead grain one more step before the next roll caught it.
 *
 * Shared between sand_gas.c (gas's own burn-down) and sand_reactions.c
 * (fire's burn-out) rather than duplicated - the body is entirely
 * generic, nothing here is gas- or fire-specific. Originally lived in
 * sand_gas.c as tick_gas_decay() with a single call site; extracting it
 * to a second call site is exactly the kind of change that regressed
 * try_fall_or_scatter()/try_slide() when done carelessly (see this
 * header's own comment above try_fall_or_scatter_impl() for the full
 * three-attempt story) - measured before and after this extraction on
 * device rather than assumed safe, since it is small enough that a
 * regression was thought unlikely but not impossible. */
/* The counting-down half of tick_decay(), at a rate the caller chooses.
 *
 * Split out for materials whose variant is life but whose movement row
 * says decay 0 - wood, which is not a transient and must not be treated as
 * one by anything else, but does count down while it is burning.
 *
 * Reads/writes through cell_code()/cell_with_code() (material.h), not
 * CELL_VARIANT()/CELL_MAKE(), and burns out at `r->lit_from` rather than a
 * hardcoded 1 - the general form gunpowder needs now that "lit" is not
 * always the whole nibble counting down to zero. Byte-identical for wood
 * (lit_from 1, and cell_code()/cell_with_code() agree with CELL_VARIANT()/
 * CELL_MAKE() for every non-gunpowder byte): `life <= 1` is exactly what
 * `life <= r->lit_from` reads as there. Takes the reaction row itself
 * rather than a bare material id, so the burn-out threshold is read once
 * from the same row the caller already loaded instead of re-deriving it
 * here. */
static inline bool tick_decay_at(sand_t *s, uint8_t *row, int x, int y,
                                 cell_t *grain, const reaction_t *r, int decay)
{
    if (decay == 0) {
        return true;
    }
    const uint32_t roll = rng_next(&s->rng);
    if ((int)(roll & 0xFF) >= decay) {
        return true;
    }

    const uint8_t life = cell_code(*grain);
    if (life <= r->lit_from) {
        row[x] = CELL_EMPTY;
        mark_rows(s, y, y);
        wake_block_and_neighbors(s, x, y);
        return false;
    }

    *grain = cell_with_code(*grain, (uint8_t)(life - 1));
    row[x] = *grain;
    mark_rows(s, y, y);
    return true;
}

static inline bool tick_decay(sand_t *s, uint8_t *row, int x, int y,
                              cell_t *grain, const material_t *mat,
                              uint8_t mat_id)
{
    const int decay = (s->decay >= 0) ? s->decay : mat->decay;
    if (decay == 0) {
        return true;
    }
    const uint32_t r = rng_next(&s->rng);
    if ((int)(r & 0xFF) >= decay) {
        return true;
    }

    const uint8_t life = CELL_VARIANT(*grain);
    if (life <= 1) {
        row[x] = CELL_EMPTY;
        mark_rows(s, y, y);
        wake_block_and_neighbors(s, x, y);
        return false;
    }

    *grain = CELL_MAKE(mat_id, life - 1);
    row[x] = *grain;
    mark_rows(s, y, y);
    return true;
}

void sand_step_reactions(sand_t *s);

bool move_liquid_grain(sand_t *s, uint8_t *row, uint8_t *prow,
                       int x, int y, int dx, int dy,
                       const int *slide_a, const int *slide_b,
                       cell_t grain, uint8_t mat_id);

/* How a liquid levels: the direction of the true surface, and what climbing
 * one step of it costs.
 *
 * A pool's true perpendicular to gravity almost never lines up with one of
 * the eight ring directions - it lies somewhere between an axis direction
 * and the diagonal beside it. `ax` is that axis ray, perpendicular to
 * whichever of gx/gy dominates; `dg` is the diagonal ray next to it, on the
 * side the tilt leans. Between the two of them, every tilt from dead flat to
 * exactly 45 degrees is bracketed.
 *
 * `q_q8` decides which ray a given column (or row, when gravity is mostly
 * sideways) takes, as a fixed pattern in SPACE, 0-256 for 0-100%. This is
 * the whole trick. Commit 30335ae pinned cross-flow to the nearest axis to
 * kill a flicker where dithering the axis in TIME let a settled pool see a
 * different axis - and so a different verdict on which way is down - on
 * almost every step, swinging half its mass back and forth. Dithering the
 * same choice in SPACE instead of in time gives a settled pool the identical
 * answer on every step, because each column always takes the same ray, so
 * nothing flickers - but the mix of rays across the pool still reads as the
 * true angle instead of snapping to one of eight.
 *
 * `bias_ax_q8`/`bias_dg_q8` are what one step of the matching ray costs in
 * gravitational potential: the mass a level surface gains per step of that
 * ray, in 1/256 units. A cell reached by a ray that is not exactly
 * perpendicular to gravity sits a little higher or lower along the true
 * "down" than the cell it started from, and so a level surface should hold
 * a little more or less mass there even though nothing moved; the bias is
 * that difference, and find_shallowest() (sand_liquid.c) carries the
 * running total so a multi-step walk compares LEVEL rather than raw mass.
 *
 * Both halves reduce, bit for bit, to the single-ray raw-mass rule that came
 * before, at the two gravities that rule already handled correctly: at
 * exactly axis-aligned gravity q_q8 is 0 and bias_ax_q8 is 0, so only the
 * axis ray is ever taken and it costs nothing extra; at exactly 45 degrees
 * q_q8 is 256 and bias_dg_q8 is 0, so only the diagonal ray is ever taken
 * and it too costs nothing extra. That is why every existing test at those
 * two gravities was unaffected by this change. */
typedef struct {
    int ax[2];        /* the axis ray - perpendicular to the dominant axis */
    int dg[2];        /* the diagonal ray beside it, the way the tilt leans */
    int q_q8;         /* how often a column takes the diagonal ray, 0-256   */
    int bias_ax_q8;   /* mass a level surface gains per step of each ray,   */
    int bias_dg_q8;   /*   in 1/256 units - zero when the ray is level      */
} xflow_t;

/* Cross-flow levelling. Called from `sand_step()`. `flow` levels, `dx`/`dy`
 * gravity direction. */
void sand_step_liquids(sand_t *s, const xflow_t *flow, int dx, int dy);

void sand_step_gas(sand_t *s, int gx, int gy, int dx, int dy,
                   const int *slide_a, const int *slide_b,
                   const int *perp_a, const int *perp_b,
                   int load_dx, int load_dy, int x_step, int jostle);

/* The whole grain-movement primitive stack - try_fall_or_scatter() and
 * try_slide(), and everything they call - moved here from sand.c, still
 * `static inline`, for the same reason dest_row()/mark_rows() above are:
 * both sit on the hottest path there is, called once per grain per step
 * for every powder cell.
 *
 * Getting this right took three attempts, each one measured on device,
 * not assumed - see docs/Sand/Simulation-Lessons.md for the full numbers:
 *
 * 1. Just remove `static` from try_fall_or_scatter()/try_slide() so
 *    sand_gas.c could call them, leaving everything else static in
 *    sand.c. Regressed the flip/water frame-budget tests by ~26%,
 *    exactly reproducible, even though neither test ever places a gas
 *    cell: turning a `static` function called once per grain into an
 *    ordinary extern one is enough, on its own, to stop the compiler
 *    inlining it into step_one_grain()'s dispatch, which that call site
 *    had been relying on.
 * 2. Move the WHOLE chain here as `static inline`, so both sand.c and
 *    sand_gas.c get their own independently inlinable copy - exactly the
 *    pattern this header already uses for dest_row()/mark_rows(). Fixed
 *    flip/water back to baseline, but grew sand_step_gas() to ~3.9 KB
 *    (bigger than sand_step() itself, from carrying a full second copy
 *    of this chain) and THAT regressed the worst-case, sleeping-off
 *    test_a_full_size_step_fits_in_the_frame_budget from ~7000us to over
 *    15000us - exactly reproducible too, and again without that test
 *    ever placing a gas cell. Flash footprint, not runtime gas activity,
 *    was the cost both times.
 * 3. What actually shipped: try_fall_or_scatter_impl()/try_slide_impl()
 *    stay `static inline` here, but only step_one_grain() in sand.c
 *    calls them directly - keeping the main sweep's hot path fully
 *    inlined, exactly as it always was. sand_gas.c instead calls the
 *    ordinary, non-inline try_fall_or_scatter()/try_slide() defined once
 *    in sand.c (declared below) - genuine functions that each wrap one
 *    of the _impl versions exactly once, so the shared logic exists in
 *    flash as at most two copies (the inlined one in sand_step(), and
 *    the one real out-of-line copy sand_gas.c calls into) rather than a
 *    third, duplicated one growing inside sand_step_gas() itself. Fixed
 *    both regressions at once. */

/* Static materials never yield regardless of density, so a wall stays a
 * wall - the general "yields to denser" rule below has this one
 * exception. */
static inline bool can_enter(uint8_t mover_density, uint8_t mover_id, cell_t target)
{
    if (CELL_IS_EMPTY(target)) {
        return true;
    }

    const material_t *t = material_of(target);

    if (!(t->kind == KIND_LIQUID || t->kind == KIND_GAS) ||
        mover_density <= t->density) {
        return false;
    }

    return !(mover_id == MAT_SAND && CELL_MATERIAL(target) == MAT_OIL);
}

/* Check cell existence for scatter decision. Avoids redundant random number
 * generation. */
static inline bool cell_open(const uint8_t *row, int nx, int w, uint8_t density,
                             uint8_t mat_id)
{
    return row != NULL && (unsigned)nx < (unsigned)w &&
           can_enter(density, mat_id, row[nx]);
}

/* Single comparison catches nx < 0 by wrapping. */
static inline bool move_to(uint8_t *from_row, uint8_t *to_row,
                           int x, int nx, int w, cell_t mover, uint8_t density)
{
    if (to_row == NULL || (unsigned)nx >= (unsigned)w ||
        !can_enter(density, CELL_MATERIAL(mover), to_row[nx])) {
        return false;
    }

    const cell_t displaced = to_row[nx];

    to_row[nx]  = mover;
    from_row[x] = displaced;
    return true;
}

static inline int slide_chance(const material_t *m, int load, int jostle)
{
    /* slip == 255 is never held by load at all - most of what separates a
     * liquid from a powder: water at the bottom of a deep pool carries
     * just as much weight as sand at the bottom of a dune, and flows
     * anyway. */
    if (load == 0 || m->slip >= 255) {
        return 256;
    }

    const int chance = (load >= SAND_LOAD_CAP) ? 0 : (m->slip >> (load - 1));

    return chance > jostle ? chance : jostle;
}

static inline bool try_scatter(sand_t *s, uint8_t *row, uint8_t *prow,
                               uint8_t *arow, uint8_t *brow, int x, int y,
                               int w, int dx, const int *slide_a,
                               const int *slide_b, cell_t grain,
                               uint8_t density, int scatter)
{
    if (scatter == 0 ||
        !cell_open(prow, x + dx, w, density, CELL_MATERIAL(grain))) {
        return false;
    }

    const uint32_t r = rng_next(&s->rng);
    if ((int)(r & 0xFF) >= scatter) {
        return false;
    }

    if ((r & 0x100) == 0) {
        const bool pick_a = (r & 0x200) != 0;
        uint8_t  *drow = pick_a ? arow : brow;
        const int ddx  = pick_a ? slide_a[0] : slide_b[0];
        const int ddy  = pick_a ? slide_a[1] : slide_b[1];

        if (move_to(row, drow, x, x + ddx, w, grain, density)) {
            mark_rows(s, y, y + ddy);
        }
    }
    return true;
}

static inline bool try_fall_or_scatter_impl(sand_t *s, uint8_t *row,
                                            uint8_t *prow, uint8_t *arow,
                                            uint8_t *brow, int x, int y,
                                            int w, int dx, int dy,
                                            const int *slide_a,
                                            const int *slide_b, cell_t grain,
                                            uint8_t density, int scatter)
{
    if (try_scatter(s, row, prow, arow, brow, x, y, w, dx, slide_a, slide_b,
                    grain, density, scatter)) {
        return true;
    }

    if (move_to(row, prow, x, x + dx, w, grain, density)) {
        mark_rows(s, y, y + dy);
        return true;
    }
    return false;
}

static inline void pick_slide_order(uint32_t r, uint8_t *arow, uint8_t *brow,
                                    const int *slide_a, const int *slide_b,
                                    uint8_t driven_row, bool driven[][2],
                                    uint8_t **first_row, int *first_dx,
                                    int *first_dy, bool *first_driven,
                                    uint8_t **second_row, int *second_dx,
                                    int *second_dy, bool *second_driven)
{
    if (r & 1) {
        *first_row  = arow; *first_dx  = slide_a[0]; *first_dy  = slide_a[1];
        *first_driven = driven[driven_row][0];
        *second_row = brow; *second_dx = slide_b[0]; *second_dy = slide_b[1];
        *second_driven = driven[driven_row][1];
    } else {
        *first_row  = brow; *first_dx  = slide_b[0]; *first_dy  = slide_b[1];
        *first_driven = driven[driven_row][1];
        *second_row = arow; *second_dx = slide_a[0]; *second_dy = slide_a[1];
        *second_driven = driven[driven_row][0];
    }
}

/* Friction on slides: grain shuffle depends on surface. Reached if
 * gravity-ward move fails. Grain skips this in open air. */
static inline bool try_slide_pair(sand_t *s, uint8_t *row, int x, int y, int w,
                                  cell_t grain, uint8_t density,
                                  const material_t *mat, int load_dx,
                                  int load_dy, int jostle, uint32_t r,
                                  uint8_t *first_row, int first_dx,
                                  int first_dy, bool first_driven,
                                  uint8_t *second_row, int second_dx,
                                  int second_dy, bool second_driven)
{
    const int load = sand_load_above(s, x, y, load_dx, load_dy);
    const int allowance = slide_chance(mat, load, jostle);
    if (allowance < 256 && (int)((r >> 16) & 0xFF) >= allowance) {
        return false;
    }

    if (first_driven &&
        move_to(row, first_row, x, x + first_dx, w, grain, density)) {
        mark_rows(s, y, y + first_dy);
        return true;
    }
    if (second_driven &&
        move_to(row, second_row, x, x + second_dx, w, grain, density)) {
        mark_rows(s, y, y + second_dy);
        return true;
    }
    return false;
}

static inline bool try_slide_impl(sand_t *s, uint8_t *row, uint8_t *prow,
                                  uint8_t *arow, uint8_t *brow, int x, int y,
                                  int w, int dx, int dy, const int *slide_a,
                                  const int *slide_b, int load_dx,
                                  int load_dy, int jostle, cell_t grain,
                                  uint8_t driven_row, uint8_t density,
                                  const material_t *mat,
                                  bool driven[][2])
{
    const uint32_t r = rng_next(&s->rng);

    uint8_t *first_row,  *second_row;
    int      first_dx,    second_dx;
    int      first_dy,    second_dy;
    bool     first_driven, second_driven;
    pick_slide_order(r, arow, brow, slide_a, slide_b, driven_row, driven,
                     &first_row, &first_dx, &first_dy, &first_driven,
                     &second_row, &second_dx, &second_dy, &second_driven);

    /* Shaken grain spreads sideways before dropping. Every destination stays
     * inside already-swept half. */
    const bool shaken = jostle > 0 && (int)((r >> 8) & 0xFF) < jostle;

    if (!shaken && jostle > 0 &&
        move_to(row, prow, x, x + dx, w, grain, density)) {
        mark_rows(s, y, y + dy);
        return true;
    }

    if (try_slide_pair(s, row, x, y, w, grain, density, mat, load_dx,
                       load_dy, jostle, r, first_row, first_dx, first_dy,
                       first_driven, second_row, second_dx, second_dy,
                       second_driven)) {
        return true;
    }

    if (shaken && move_to(row, prow, x, x + dx, w, grain, density)) {
        mark_rows(s, y, y + dy);
        return true;
    }

    return false;
}

bool try_fall_or_scatter(sand_t *s, uint8_t *row, uint8_t *prow,
                         uint8_t *arow, uint8_t *brow, int x, int y,
                         int w, int dx, int dy, const int *slide_a,
                         const int *slide_b, cell_t grain,
                         uint8_t density, int scatter);

/* `driven_row`/`bool driven[][2]`, not `mat_id`/`driven[MATERIAL_MAX][2]`:
 * step_one_grain() (sand.c) backs this with driven[MATERIAL_ROWS][2]
 * indexed by row (grain >> 3), while sand_gas.c backs it with
 * driven_gas[MATERIAL_MAX][2] indexed by material id - two different real
 * bounds behind the same shape, so naming either one here was never
 * accurate for both callers. */
bool try_slide(sand_t *s, uint8_t *row, uint8_t *prow, uint8_t *arow,
               uint8_t *brow, int x, int y, int w, int dx, int dy,
               const int *slide_a, const int *slide_b, int load_dx,
               int load_dy, int jostle, cell_t grain, uint8_t driven_row,
               uint8_t density, const material_t *mat,
               bool driven[][2]);

/* Moved from sand.c. sand_gas.c uses REVERSED gravity vector. */
static inline bool driven_by_gravity(int mx, int my, int gx, int gy,
                                     int repose)
{
    const int descent = mx * gx + my * gy;
    if (descent <= 0) {
        return false;              /* uphill, or across a level slope */
    }
    if (repose == 0) {
        return true;               /* no friction angle at all - a liquid */
    }

    int lateral = mx * gy - my * gx;
    if (lateral < 0) {
        lateral = -lateral;
    }

    return (int64_t)descent * 10 > (int64_t)lateral * repose;
}

/* RSTAGE_BURN_ANY must stay 0: material_first_stage[]/extended_first_stage[]
 * (sand_reactions.c) are zero-initialised .bss, so an unwritten slot lands
 * here - see step_one_reacting_row()'s stage_burn_any: label for why it
 * has to be this stage and not one of the other two burn stages. */
enum {
    RSTAGE_BURN_ANY,
    RSTAGE_BURN_ALWAYS,
    RSTAGE_BURN_CHECK,
    RSTAGE_DISSOLVE,
    RSTAGE_ACID_RAIN,
    RSTAGE_CONDENSE,
    RSTAGE_HEAT_RAMP,
    RSTAGE_CHILL,
    RSTAGE_WARM,
    RSTAGE_SOAK_DRY,
    RSTAGE_FALL,
    RSTAGE_WITHER,
    RSTAGE_DRINK,
    RSTAGE_ROOT,
    RSTAGE_GROW,
    RSTAGE_SPROUT,
    RSTAGE_BUD,
    RSTAGE_END,
    RSTAGE_COUNT
};

/* `is_acid_rain_material` gates material-specific stage, not reaction type. */
static inline uint8_t
reaction_first_stage(const reaction_t *r, bool is_acid_rain_material)
{
    if (r->burns != 0) {
        return RSTAGE_BURN_ALWAYS;
    }
    if (r->burn_decay != 0) {
        return RSTAGE_BURN_CHECK;
    }
    if (r->dissolves != 0) {
        return RSTAGE_DISSOLVE;
    }
    if (is_acid_rain_material) {
        return RSTAGE_ACID_RAIN;
    }
    if (r->condenses != 0) {
        return RSTAGE_CONDENSE;
    }
    if (r->heat_ramp != 0) {
        return RSTAGE_HEAT_RAMP;
    }
    if (r->chills != 0) {
        return RSTAGE_CHILL;
    }
    if (r->warms != 0) {
        return RSTAGE_WARM;
    }
    if (r->soaks != 0 || r->dries != 0) {
        return RSTAGE_SOAK_DRY;
    }
    if (r->falls != 0) {
        return RSTAGE_FALL;
    }
    if (r->withers != 0) {
        return RSTAGE_WITHER;
    }
    if (r->drinks != 0) {
        return RSTAGE_DRINK;
    }
    if (r->roots != 0) {
        return RSTAGE_ROOT;
    }
    if (r->grows != 0) {
        return RSTAGE_GROW;
    }
    if (r->sprouts != 0) {
        return RSTAGE_SPROUT;
    }
    if (r->buds != 0) {
        return RSTAGE_BUD;
    }
    return RSTAGE_END;
}
