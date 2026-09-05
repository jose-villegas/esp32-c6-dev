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
 * frame-budget tests in suite_sand.c, which is exactly what would catch it if
 * this ever stopped being true.
 *===========================================================================*/
#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "sand.h"

/* The row a move would land in, or NULL if that is off the grid.
 *
 * Worked out once per row rather than once per grain: every grain in a row
 * shares the same three destination rows, so the vertical bounds check is
 * done 224 times per step instead of 41,216 times. */
static inline uint8_t *dest_row(const sand_t *s, int y)
{
    if (y < 0 || y >= s->h) {
        return NULL;
    }
    return s->cells + (size_t)y * (size_t)s->w;
}

/* Marking is a pair because every move touches two rows: the one a grain left
 * and the one it arrived in. Both are guaranteed in range at every call
 * site - a move only happens once its destination row has been found to
 * exist.
 *
 * Row-shaped only, and now that means dirty_rows and nothing else. This used
 * to also wipe a three-row span of row_state through a wake_span() helper -
 * first because the settled bits lived there (they moved to block_state in
 * the fourth attempt), and after that purely to invalidate sand_liquid.c's
 * ROW_NO_LIQUID "this row is dry" cache. That cache is gone: maintaining it
 * cost more than the row scans it saved, measured on device - see the ninth
 * attempt in docs/Sand/Performance-Tuning-Attempts.md. What is left is what
 * the name always said: mark two rows dirty for the renderer.
 *
 * Kept alongside mark_move() below for the one call site (equalise_one_row()'s
 * deferred cross-flow marking) that needs the row-shaped bookkeeping without
 * a single pair of points to wake blocks from - see the comment there. */
static inline void mark_rows(sand_t *s, int y0, int y1)
{
    /* DEFENSIVE, ADDED 2026-09-01 - see wake_block_and_neighbors()'s own
     * matching guard, just above in this same file, for the real device
     * crash this was traced to (mark_move(), called from gas cell
     * movement, sand_gas.c). y0/y1 were "guaranteed in range at every
     * call site" by design, and a heap-poisoning-enabled rebuild reproduced
     * the identical crash, ruling out memory corruption from elsewhere as
     * the cause - something upstream is genuinely computing an
     * out-of-range row, not yet pinned to one exact line. This bound stops
     * the out-of-bounds write while that is still being tracked down. */
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

/* Settled-block bits, in block_state - the finer-grained sibling of
 * ROW_SETTLED_NEAREST/OTHER (which used to live in a row-shaped array; see
 * sand_enable_sleeping()'s comment in sand.h for why they moved, and why
 * that array is gone altogether now). Two
 * settled bits for the same reason a row needed two: gravity direction is
 * dithered between two ring directions each step, and a block settled
 * under one may not be settled under the other. BLOCK_ACTIVE is transient,
 * cleared at the start of every sand_step() and finalised into the settled
 * bits at the very end - see compute_settled_bit() and the finalisation
 * pass in sand_step(), both in sand.c. All three fit in one byte
 * (block_state is the only per-region byte left, and has nothing to share
 * with). */
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

/* Which materials are liquid, as a bitmask over the nibble.
 *
 * Shared by sand.c's sweep (to maintain BLOCK_HAS_LIQUID) and sand_liquid.c's
 * two passes. Asking materials[id].kind per cell instead is what this exists
 * to avoid: measured, that alone put a screen of motionless SAND from 17 us to
 * five and a half milliseconds. Sixteen bits in a register answers the same
 * question for nothing.
 *
 * Called a couple of times per STEP, never per cell, so the sixteen-entry
 * build is free - checked by reading the call sites during the eighth attempt
 * rather than assumed. */
static inline uint16_t liquid_mask(void)
{
    uint16_t mask = 0;
    for (int m = 0; m < MATERIAL_MAX; m++) {
        if (materials[m].kind == KIND_LIQUID) {
            mask |= (uint16_t)(1u << m);
        }
    }
    return mask;
}

static inline int block_of(const sand_t *s, int x, int y)
{
    return (y / SAND_BLOCK_H) * s->block_cols + (x / SAND_BLOCK_W);
}

/* Something changed at, or between, the two blocks covering (bx0,by0) and
 * (bx1,by1), given as BLOCK indices already. Used only by
 * equalise_one_row()'s deferred, already-block-shaped range wake, which
 * is called once per row rather than once per transfer. Clears
 * BLOCK_SETTLED_NEAREST|OTHER
 * for every block in the bounding box of the two, expanded by one block in
 * every direction and clipped to the grid, so a block bordering the one
 * actually touched also notices - the same reason a neighbouring row
 * noticing a move is what lets undermining wake a settled pile above it.
 * Also sets BLOCK_ACTIVE, unexpanded, on exactly the two touched blocks
 * themselves: that bit is how compute_settled_bit() later knows not to put
 * a genuinely-active block back to sleep, and only a block a move actually
 * happened in counts, not its neighbours (which merely became worth
 * re-examining, not proven active). */
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

/* Whether any of block (bx,by)'s up to 8 neighbours has BLOCK_ACTIVE set
 * this step - the pull-based replacement for the old per-move push
 * mechanism (wake_blocks_points()/point_reach()/reach_axis()/
 * reach_corner()/should_wake_neighbor()/cell_occupied(), all removed;
 * see docs/Sand/Simulation-Lessons.md for the two failed attempts at
 * cheapening that mechanism that led here instead).
 *
 * Called once per block per step, from sand_step()'s own finalisation
 * pass (sand.c) - not once per grain move, so unlike everything it
 * replaces, it can afford to check unconditionally rather than needing
 * edge-position/occupancy gating to stay cheap: the cost is already
 * bounded to O(block_count) regardless of how many grains moved inside
 * any of them. Not forced inline - it has exactly one caller, a loop
 * that already isn't inlined into anything hotter. */
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

/* Whether block (bx,by) or any of its up to 8 neighbours was seen holding
 * liquid this step - the expanded form of BLOCK_HAS_LIQUID that
 * sand_liquid.c's cross-flow pass actually skips on. Deliberately the same
 * shape as any_neighbor_active() above, and called from the same kind of
 * place: once per block per step, from a pass that is already O(blocks). It
 * counts the block ITSELF as well, which that one does not - "did a NEIGHBOUR
 * move" and "is there liquid anywhere near" are different questions. */
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
    /* DEFENSIVE, ADDED 2026-09-01 - a real device crash (Guru Meditation,
     * Store/Load access fault) traced here through mark_move(), called
     * from equalise_gas_one_cell() (sand_gas.c) with a gas grain's own
     * destination coordinates. The unsigned cast just below this comment
     * assumes x,y are always in-range grid coordinates - true of every
     * OTHER call site, but the exact upstream source of an out-of-range
     * (x, y) reaching this one was not pinned down before this landed;
     * see mark_rows()'s own matching guard, just below in this same file,
     * and the git history around both for the investigation. Cheap
     * unsigned-compare, same idiom the rest of this file already uses -
     * this does not paper over the real bug, it just stops it from
     * reading/writing block_state out of bounds while that bug is still
     * being tracked down. */
    if ((unsigned)x >= (unsigned)s->w || (unsigned)y >= (unsigned)s->h) {
        return;
    }

    /* Unsigned cast for the same reason as elsewhere in this file (see
     * docs/Notes/Optimization-Playbook.md's division lesson) - x/y are
     * always non-negative grid coordinates, the compiler just cannot
     * prove it from a plain int parameter. Low-frequency call, so this
     * matters far less here than it did on the sweep's old hot path,
     * but there is no reason to leave it slower than free. */
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
 * (suite_sand.c) for the full 8-direction x 4-configuration ground truth
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

/* THE REFLECTION ITSELF - r = d*|n|^2 - 2*(d.n)*n, then sign-quantised back
 * onto the ring the same way blocker_normal() above quantises its own sum.
 * The |n|^2 factor is what lets this skip normalising `n` first: an
 * axis-aligned normal has |n|^2 == 1 and a diagonal one has |n|^2 == 2, and
 * scaling `d` by that factor before subtracting keeps the whole thing
 * integer arithmetic with no square root. Dropping that factor would
 * silently give every diagonal-normal bounce the wrong angle.
 *
 * Returns -1 - same "no real bounce, caller falls through to the plain
 * wait" meaning as blocker_normal()'s own -1 - if the result sign-quantises
 * to (0,0) (kept as a guard; should not happen for unit `d` and `n`) or
 * lands back on the incoming direction itself, which would read as the
 * mover tunnelling forward through the wall it just hit rather than
 * rebounding off it. */
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

/* Whether (nx, ny) is in bounds and holds something strictly denser than
 * `density`, and is not a liquid - a liquid never counts as covering
 * anything, whether the question is true burial (smothered(),
 * sand_reactions.c) or gravity-relative coverage (cover_mask() below): a
 * neighbour at the prober's own density or below (more fire, more gas,
 * more of the same liquid) never counts, or a large pocket of any of
 * those would seal itself from the inside out - only genuinely being
 * buried under something heavier (sand, stone) should.
 *
 * Moved here from sand_reactions.c (bd esp32c6-a2j) - cover_mask() needed
 * it too, and the brief for that change was explicit that this file
 * should not grow a second predicate meaning the same thing. Its
 * existing caller at the time (smothered(), still in sand_reactions.c)
 * was unaffected: same name, same signature, same body, just visible
 * from one file earlier in the include chain now. */
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
 * test_cover_primitive_matches_the_exhaustive_shape_table (suite_sand.c).
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
 * (test_a_wide_pool_under_a_sideways_crust_bursts, suite_sand.c).
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

/* The common case: does (x, y) have a complete lid over it, gravity-
 * relative - for a caller with no reason to look at the mask itself. */
static inline bool
covered_at(const sand_t *s, int x, int y, int w, int h, uint8_t density)
{
    return cover_mask(s, x, y, w, h, density) == COVER_LID;
}

/* What one cell of `displaced` costs a mover ploughing through it - see
 * SAND_IMPULSE_DRAG_SHIFT and its two per-kind companions in sand.h.
 *
 * KIND MATTERS ON TOP OF DENSITY, on device evidence: packed grain jams
 * against itself and stops a chunk in a couple of layers, where a fluid
 * parts around one and lets it sink a good way in before it loses the
 * energy to keep going. Density alone gave both the same shape of
 * resistance, so a bank of dirt read as too soft and a pool as too stiff
 * at the same time - no single shift could fix both, which is what these
 * two exist to say. Saturating: a doubled density can exceed a byte, and
 * a cost above 255 means the same thing 255 does, since speed is one. */
static inline uint8_t impulse_drag_of(cell_t displaced)
{
    const material_t *m = material_of(displaced);
    unsigned d = (unsigned)m->density >> SAND_IMPULSE_DRAG_SHIFT;

    if (m->kind == KIND_LIQUID) {
        return 0u;   /* a fluid parts around a mover - see the constants */
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
    /* Off ambient in EITHER direction is work to do: a frosted pane has to
     * warm back up just as a hot one has to cool down. At ambient exactly
     * there is nothing to relax towards, and snow reaching it is what
     * starts it moving again. */
    if (r->chills != 0 || r->warms != 0 ||
        (r->heat_ramp != 0 && CELL_VARIANT(cell) != SAND_AMBIENT_HEAT)) {
        s->may_have_temperature = true;
    }
    /* Something a hot gas could ACT on, which is a different question
     * from something that has a temperature now - ambient counts.
     * Convection's gate.
     *
     * Two shapes of it, because convection has two things it can do.
     * Something that banks heat climbs a level; something cold that
     * cannot bank it thaws outright. Ice was invisible here while this
     * asked only about heat_ramp, and structurally always would have
     * been - an extended material's low nibble is which material it is,
     * so it has no variant left to hold a temperature in. A sheet of ice
     * over a boiler sat there untouched unless some unrelated stone
     * happened to be on the board to open the gate for it. */
    if (r->heat_ramp != 0 ||
        (r->chills != 0 && r->heats_to != 0 && r->heat_chance != 0)) {
        s->may_have_heat_holder = true;
    }
    /* Wet, or something to get wet from. A liquid arms it because that is
     * when a soaker has anything to do; a cell already holding moisture
     * arms it because drying has to outlive the puddle.
     *
     * CELL_MOISTURE(), not the raw variant - a dry cell's variant is a
     * TONE (material.h's own comment on soil's state split), and testing
     * the whole nibble latched this for SOIL_DRY_TONES - 1 of every
     * SOIL_DRY_TONES dry cells for good, arming the soak/dry pass forever
     * on soil that was never wet at all. */
    if (mat->kind == KIND_LIQUID ||
        (r->dries != 0 && CELL_MOISTURE(cell) != 0)) {
        s->may_have_moisture = true;
    }
}

/* The row-shaped bookkeeping mark_rows() always did, plus waking the
 * touched blocks - see wake_block_and_neighbors() above for why this is
 * only used outside the sweep (sand_set()/sand_erase()/try_spawn_one(),
 * and liquid's equalise_one_cell()). The sweep's own
 * move-reporting call sites (sand.c's try_scatter()/try_fall_or_scatter()/
 * try_slide()/try_slide_pair(), and move_liquid_grain() in
 * sand_liquid.c) call mark_rows() directly instead - they need no block
 * wake of their own at all, since step_one_block()'s existing
 * `moved_here` bookkeeping already marks the source block active, and
 * any_neighbor_active() picks that up for its neighbours (including any
 * different block a move actually landed in, always one of them) on its
 * own the moment sand_step()'s finalisation pass runs. */
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
 * one by anything else, but does count down while it is burning. */
static inline bool tick_decay_at(sand_t *s, uint8_t *row, int x, int y,
                                 cell_t *grain, uint8_t mat_id, int decay)
{
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

static inline bool tick_decay(sand_t *s, uint8_t *row, int x, int y,
                              cell_t *grain, const material_t *mat,
                              uint8_t mat_id)
{
    /* s->decay mirrors s->scatter's own override (see sand_set_decay()):
     * 0, the default, means immortal regardless of the table, so placing
     * a decaying material in a test does not risk it quietly vanishing
     * mid-test. */
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

/* Defined in sand_reactions.c: the whole of a step's fire-chemistry work
 * for every burning cell (reaction_t.burns - fire and ember today) -
 * ignition of adjacent flammable neighbours, extinguishing by adjacent
 * liquid, burning out via tick_decay() above, (ember only) flaring a
 * flame upward, and now heat conduction through a material like stone
 * (reaction_t.conducts - see conduct_heat() in sand_reactions.c). Called
 * once from sand_step(), after sand_step_gas() finishes and before
 * finalize_settling() - same slot, same reasoning as sand_step_liquids()/
 * sand_step_gas() before it: BLOCK_ACTIVE has to reflect the whole step.
 * Gated on s->may_have_burning alone (not may_have_gas too) - a burning
 * cell is the only actor here; gas is passive fuel with nothing to do on
 * its own.
 *
 * Takes only `s`. It briefly took (gx, gy) too, while boiling walked
 * against gravity to find a liquid's surface; boiling happens at the
 * heat source now and the steam bubbles up by itself, so this pass has
 * no interest in gravity at all. That also restores the original reason
 * may_have_burning is checked INSIDE rather than at the call site:
 * there are no arguments to marshal for a call that will immediately
 * return. */
void sand_step_reactions(sand_t *s);

/* Defined in sand_liquid.c, called from sand.c's per-cell sweep. The one
 * piece of liquid movement that has to live inside that sweep rather than in
 * its own pass: it is gravity-ward, so it shares that sweep's own guarantee
 * against double-moving a cell, the same way a grain's fall does. See
 * sand_liquid.c for down-then-slope and why cross-flow cannot join it there.
 * Returns whether it moved anything. */
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

/* Defined in sand_liquid.c: the whole of a step's liquid work that does NOT
 * belong inside the main sweep - cross-flow levelling. Called once from
 * sand_step(), after that sweep finishes. `flow` describes how this liquid
 * levels - see xflow_t; `dx`/`dy` is gravity's own dithered direction this
 * step. */
void sand_step_liquids(sand_t *s, const xflow_t *flow, int dx, int dy);

/* Defined in sand_gas.c: a gas grain's whole step - rising (reusing
 * try_fall_or_scatter()/try_slide() below, direction-inverted) then
 * perpendicular spread (mirroring sand_step_liquids()'s cross-flow, but
 * whole-grain instead of mass-based). Called once from sand_step(), after
 * sand_step_liquids() and before finalize_settling() - same slot, same
 * reason: BLOCK_ACTIVE has to reflect the whole step. Every argument here
 * is something sand_step() already computed for the main sweep/liquid
 * pass; this negates whatever needs negating internally rather than
 * recomputing from scratch. */
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

/* Whether `mover` may occupy the cell currently holding `target`.
 *
 * Empty space always yields. Anything else yields only to something denser,
 * which is how sand sinks through water while water cannot push its way back
 * up through sand. Static materials never yield whatever the arithmetic says,
 * so a wall stays a wall.
 *
 * `mover_id` exists only for the one-pairing exception below, and costs
 * nothing on every other call: it is a plain uint8_t already sitting in a
 * register at every call site (CELL_MATERIAL() of a cell_t already in
 * hand), and the branch that reads it is reached only once density has
 * already said yes - the common misses (empty target aside, already
 * handled above; static target; insufficient density) never touch it. */
static inline bool can_enter(uint8_t mover_density, uint8_t mover_id, cell_t target)
{
    if (CELL_IS_EMPTY(target)) {
        return true;
    }

    const material_t *t = material_of(target);

    /* Only a FLUID gets displaced. A grain can push its way down through
     * water or through smoke, and cannot push its way through packed
     * grains however heavy it is - which is what grains do.
     *
     * This used to be `kind != KIND_STATIC`, so any powder sank through
     * any lighter powder: dirt fell through snow, sand fell through snow,
     * dirt fell through sand. Reported as dirt passing through a snowbank
     * rather than landing on it, which is exactly what it looked like.
     * Density still decides fluids - sand sinks in water, snow floats on
     * it - and stops deciding anything between solids. */
    if (!(t->kind == KIND_LIQUID || t->kind == KIND_GAS) ||
        mover_density <= t->density) {
        return false;
    }

    /* Sand does not sink into oil, despite being denser (60 against 22 -
     * see material.c). Oil's density has to stay low so it floats on
     * every OTHER liquid it meets there, which makes it look lighter than
     * sand too - a side effect of the one shared scalar, not something
     * true about sand and oil specifically. Named directly by material id
     * rather than given its own field: this is the only pairing density
     * gets wrong, so a generic mechanism would cost more than it explains.
     * Reached only past the density check above, so every other mover and
     * every other target pay nothing for it. */
    return !(mover_id == MAT_SAND && CELL_MATERIAL(target) == MAT_OIL);
}

/* Whether a cell exists and can be entered, without moving anything into it.
 *
 * Needed so scatter can be decided ONLY for cells that could actually fall.
 * Drawing a random number for every cell regardless would undo the single
 * biggest saving in this loop - see the note on the common path below. */
static inline bool cell_open(const uint8_t *row, int nx, int w, uint8_t density,
                             uint8_t mat_id)
{
    return row != NULL && (unsigned)nx < (unsigned)w &&
           can_enter(density, mat_id, row[nx]);
}

/* Move a grain into `to_row` at column `nx`, if that cell exists and is free.
 *
 * A NULL row or an out-of-range column is a wall, so both simply fail. The
 * single unsigned comparison catches nx < 0 as well, by wrapping it to a huge
 * value - the usual trick, worth it in a loop this hot. */
static inline bool move_to(uint8_t *from_row, uint8_t *to_row,
                           int x, int nx, int w, cell_t mover, uint8_t density)
{
    if (to_row == NULL || (unsigned)nx >= (unsigned)w ||
        !can_enter(density, CELL_MATERIAL(mover), to_row[nx])) {
        return false;
    }

    /* A SWAP, not an overwrite. Where the target was empty this is exactly the
     * old behaviour; where it held something lighter, that lighter thing takes
     * the vacated cell and is displaced upward.
     *
     * Safe with respect to sweep order: the displaced cell lands in the very
     * cell being processed, which the sweep has just finished with, so it
     * cannot move a second time this step. */
    const cell_t displaced = to_row[nx];

    to_row[nx]  = mover;
    from_row[x] = displaced;
    return true;
}

/* Chance in 256 that a loaded grain may still slide sideways.
 *
 * A surface grain is free. Each grain above halves the chance, and past the cap
 * it is nil. Shaking overrides the lot - a shaken pile flows regardless of how
 * deeply buried its grains are, which is the whole reason shaking a jar of
 * sand levels it. */
static inline int slide_chance(const material_t *m, int load, int jostle)
{
    /* A material whose slip is 255 is never held by load at all. That is most
     * of what separates a liquid from a powder: water at the bottom of a deep
     * pool carries just as much weight as sand at the bottom of a dune, and
     * flows anyway. */
    if (load == 0 || m->slip >= 255) {
        return 256;
    }

    const int chance = (load >= SAND_LOAD_CAP) ? 0 : (m->slip >> (load - 1));

    return chance > jostle ? chance : jostle;
}

/* The common case by a wide margin - on a screen of falling sand almost every
 * grain simply moves the way gravity points. Taking it before drawing a
 * random number matters: the generator was the single most expensive thing
 * in this loop, and most grains never needed it.
 *
 * Only called when jostle == 0: scatter only applies to a grain that could
 * fall, and a shaken grain has to reach the slide logic in try_slide()
 * regardless of whether the plain fall is open.
 *
 * Whether a scattering grain drifted, lagged, or the scatter roll simply did
 * not apply - in every one of those cases the grain is spoken for, so the
 * caller must not also attempt a plain fall on it. */
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

    /* Drift: sideways as well as down, if that way is open. Spreads the
     * stream horizontally. Blocked or not chosen, it lags instead: nothing
     * at all this step, so the grains around it pull ahead and the stream
     * spreads vertically.
     *
     * Either way still counts as activity. A grain that CHOSE not to move is
     * not a settled grain, and letting the row sleep here would strand it in
     * mid-air. */
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

/* Named _impl, not called directly outside this header: step_one_grain()
 * in sand.c calls this inline version straight, so the main sweep's own
 * call site stays fully inlined. sand_gas.c instead calls the real,
 * ordinary try_fall_or_scatter()/try_slide() defined in sand.c (declared
 * further down) - a single genuine function, wrapping this same inline
 * body ONCE, rather than sand_gas.c inlining a second full copy of it.
 * Measured why this split exists: giving sand_step_gas() its own fully
 * inlined copy of the whole chain (the first version of this fix) grew it
 * to ~3.9 KB - bigger than sand_step() itself - and that alone was enough
 * to regress test_a_full_size_step_fits_in_the_frame_budget from ~7000us
 * to over 15000us, reproduced exactly across captures, even though that
 * test never places a single gas cell. One ordinary function call from
 * sand_gas.c costs far less than carrying a second copy of this code in
 * flash at all. See docs/Sand/Simulation-Lessons.md for the full story. */
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

/* Which of the two slides to try first, and which second - without
 * randomising it the sand develops a visible grain with everything leaning
 * the same way. */
static inline void pick_slide_order(uint32_t r, uint8_t *arow, uint8_t *brow,
                                    const int *slide_a, const int *slide_b,
                                    uint8_t mat_id, bool driven[MATERIAL_MAX][2],
                                    uint8_t **first_row, int *first_dx,
                                    int *first_dy, bool *first_driven,
                                    uint8_t **second_row, int *second_dx,
                                    int *second_dy, bool *second_driven)
{
    if (r & 1) {
        *first_row  = arow; *first_dx  = slide_a[0]; *first_dy  = slide_a[1];
        *first_driven = driven[mat_id][0];
        *second_row = brow; *second_dx = slide_b[0]; *second_dy = slide_b[1];
        *second_driven = driven[mat_id][1];
    } else {
        *first_row  = brow; *first_dx  = slide_b[0]; *first_dy  = slide_b[1];
        *first_driven = driven[mat_id][1];
        *second_row = arow; *second_dx = slide_a[0]; *second_dy = slide_a[1];
        *second_driven = driven[mat_id][0];
    }
}

/* Friction, and only on the slides. The grain could not fall, so whether it
 * may SHUFFLE depends on what is sitting on it. Reached only once the
 * gravity-ward move has already failed, so a grain in open air never pays
 * for this. */
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

/* Blocked, or being shaken. _impl for the same reason
 * try_fall_or_scatter_impl() is - see its own comment above. */
static inline bool try_slide_impl(sand_t *s, uint8_t *row, uint8_t *prow,
                                  uint8_t *arow, uint8_t *brow, int x, int y,
                                  int w, int dx, int dy, const int *slide_a,
                                  const int *slide_b, int load_dx,
                                  int load_dy, int jostle, cell_t grain,
                                  uint8_t mat_id, uint8_t density,
                                  const material_t *mat,
                                  bool driven[MATERIAL_MAX][2])
{
    const uint32_t r = rng_next(&s->rng);

    uint8_t *first_row,  *second_row;
    int      first_dx,    second_dx;
    int      first_dy,    second_dy;
    bool     first_driven, second_driven;
    pick_slide_order(r, arow, brow, slide_a, slide_b, mat_id, driven,
                     &first_row, &first_dx, &first_dy, &first_driven,
                     &second_row, &second_dx, &second_dy, &second_driven);

    /* Shaking reorders the attempts rather than adding a new move: a shaken
     * grain prefers to spread sideways before it drops. Every destination
     * stays inside the already-swept half, so the no-double-move guarantee
     * above still holds. */
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

/* Ordinary, non-inline functions, defined once in sand.c, each wrapping
 * one of the _impl versions above exactly once - what sand_gas.c calls
 * instead of the inline versions directly. See this header's own top
 * comment (above try_fall_or_scatter_impl()) for why: giving
 * sand_step_gas() its own fully inlined copy of this chain measured a
 * real, exactly-reproducible regression on a worst-case frame-budget
 * test that never even places a gas cell - flash footprint, not runtime
 * gas activity, was the cost. One ordinary function call from sand_gas.c
 * is far cheaper than that second copy. */
bool try_fall_or_scatter(sand_t *s, uint8_t *row, uint8_t *prow,
                         uint8_t *arow, uint8_t *brow, int x, int y,
                         int w, int dx, int dy, const int *slide_a,
                         const int *slide_b, cell_t grain,
                         uint8_t density, int scatter);

bool try_slide(sand_t *s, uint8_t *row, uint8_t *prow, uint8_t *arow,
               uint8_t *brow, int x, int y, int w, int dx, int dy,
               const int *slide_a, const int *slide_b, int load_dx,
               int load_dy, int jostle, cell_t grain, uint8_t mat_id,
               uint8_t density, const material_t *mat,
               bool driven[MATERIAL_MAX][2]);

/* Whether a grain may slide in direction (mx, my) at all, given gravity
 * (gx, gy) and its material's angle of repose. Moved here from sand.c
 * (still `static inline`, so it stays free to call inside the main
 * sweep's own hot loop) because sand_gas.c needs it too, to build its own
 * driven[][] table against the REVERSED gravity vector - reusing the
 * main sweep's own driven[][] (built against the forward vector) would
 * make every gas slide's descent dot-product come out negative, since
 * gas's slide vectors point away from real gravity by construction. See
 * sand_gas.c's own comment for the full reasoning. */
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

    /* `repose` is mu times ten, so 7 is the ~35 degrees of dry sand. Kept as
     * a ratio so this stays in integers. */
    return (int64_t)descent * 10 > (int64_t)lateral * repose;
}
