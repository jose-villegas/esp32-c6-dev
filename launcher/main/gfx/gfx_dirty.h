/*=============================================================================
 * gfx_dirty - the grid dirty-region tracker behind gfx_present(), as a
 * standalone, ESP-IDF-free module.
 *
 * Header-only, all functions static (some static inline), by necessity, not
 * by convenience: mark_band() sits on gfx_fill_rect()/gfx_pixel()'s hot
 * path - gfx_text_scaled() alone calls it once per set font pixel - and
 * routing that through a real cross-translation-unit call once cost about
 * 5% of the launcher's framerate (see docs/Notes/Display-and-Rendering.md's
 * "Partial updates"). A traditional .c/.h split would put mark_band() back
 * behind exactly that kind of call for every file that includes this one,
 * silently reintroducing a regression this project already measured and
 * fixed once. Keeping everything static and header-only means gfx.c gets
 * it inlined into its own translation unit, exactly as before, while a
 * host test file gets its own independent, fully working copy just by
 * including this file directly - no separate .c to link, no ESP-IDF
 * dependency to satisfy.
 *
 * gfx.c is the only place in the real firmware that ever includes this -
 * every other file goes through gfx.h's public gfx_mark_dirty()/
 * gfx_mark_all_dirty()/gfx_region_dirty(), which gfx.c implements as thin
 * wrappers around the dirty_*() functions here. Those three are the only
 * names in this file with a public-API counterpart to avoid colliding
 * with; everything else keeps the name it always had inside gfx.c.
 *
 * See docs/Notes/Display-and-Rendering.md's "Partial updates" and "Still
 * untapped" for the full reasoning behind the grid, the leaf layer
 * underneath it, and the two real bugs (MALLOC_CAP_DMA, the semaphore's
 * lack of per-transfer identity) this design surfaced.
 *===========================================================================*/
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Mirrors gfx.h's GFX_WIDTH/GFX_HEIGHT (BSP_LCD_H_RES/V_RES) as plain
 * literals - this module must stay free of ESP-IDF/BSP headers to compile
 * on a host. gfx.c carries a _Static_assert tying these back together, so
 * drift between the two becomes a compile error, not a silent mismatch. */
#define GFX_DIRTY_WIDTH  368
#define GFX_DIRTY_HEIGHT 448

/* The frame is sent in full-width bands. Full width matters: it makes each
 * band a contiguous run inside the framebuffer, so the DMA reads it in
 * place with no copy. 448 / 64 = 7 bands exactly. */
#define STRIP_HEIGHT 64
#define STRIP_COUNT  (GFX_DIRTY_HEIGHT / STRIP_HEIGHT)

/* The screen as a fixed grid of cells, ROWS tall bands x COLS wide columns.
 * Both dimensions are a fixed partition, not just rows with an adaptive box
 * inside: two separate clusters of activity in the same row but different
 * columns get to skip the gap between them instead of one box being forced
 * to span both. 368 / 4 = 92 and 448 / 64 = 7, both exact.
 *
 * One bit per cell in cell_dirty: set means "this cell changed and must be
 * considered for sending". The panel refreshes from its own GRAM, so
 * anything not sent simply stays on screen. */
#define GRID_COLS  4
#define COL_WIDTH  (GFX_DIRTY_WIDTH / GRID_COLS)
#define CELL_COUNT (STRIP_COUNT * GRID_COLS)
static uint32_t cell_dirty;

/* Set once dirty_mark_all() has run this frame, cleared alongside
 * cell_dirty by dirty_frame_sent(). Every cell is already claimed at that
 * point, so any further mark_band()/dirty_mark() call this frame can only
 * ever repeat work already done - real for an app that clears then draws
 * many small primitives on top (microui, the cube renderer): each one
 * otherwise pays the per-cell intersection math for a result gfx_clear()
 * already decided. */
static bool all_dirty;

/* Per cell, the (x0,x1) x (y0,y1) box actually known to be dirty this
 * frame, in absolute panel pixels, not cell-relative.
 *
 * Anything that marks a row dirty without knowing which pixels changed
 * (mark_band(), below) has to claim every cell in that row, each full width
 * for its own column, or a caller that really did only touch a few pixels
 * could leave someone else's stale pixels unsent beside them. Only
 * dirty_mark() - used by callers that DO know a real box - is allowed to
 * narrow a cell, and narrowing is always a min/max union against whatever
 * is already there, never an overwrite: that is what makes the two ways of
 * marking a cell order-independent within one frame, whichever runs
 * first. */
static int cell_x0[CELL_COUNT];
static int cell_x1[CELL_COUNT];
static int cell_y0[CELL_COUNT];
static int cell_y1[CELL_COUNT];

/* A second, finer level underneath the cell grid above: each cell is also
 * split into a fixed 4x4 grid of LEAF_W x LEAF_H leaves - 92/4=23 and
 * 64/4=16, both exact, which is why this stops at one extra level rather
 * than recursing further: 23 is prime, so nothing below it divides evenly.
 *
 * Unlike the cell boxes above, leaf geometry is never stored, only derived
 * (col * LEAF_W, etc.) - a leaf is already small enough that tracking a
 * tighter box inside one buys nothing. One bit per leaf is enough to know
 * whether it needs to be considered when narrowing a send.
 *
 * LEAF_COLS is 16, so one row of leaves - spanning all 4 cells in a grid
 * row - fits exactly in one uint16_t; no further indexing math is needed
 * to test "which leaf columns are dirty in this grid row". */
#define LEAF_SUB   4
#define LEAF_W     (COL_WIDTH / LEAF_SUB)
#define LEAF_H     (STRIP_HEIGHT / LEAF_SUB)
#define LEAF_COLS  (GRID_COLS * LEAF_SUB)
static uint16_t leaf_dirty[STRIP_COUNT * LEAF_SUB];

/* Bounds gather_buf, the scratch space gfx.c packs one gathered run's box
 * into edge-to-edge before sending it as one draw_bitmap() call. A run
 * bigger than this is not worth gathering at all, at which point the row
 * is sent whole instead. Also the size budget plan_run()'s leaf-refined
 * splits must respect - see its own comment. */
#define GATHER_MAX_PIXELS (128 * 64)

#define LEAF_REFINE_MAX_RUNS 2   /* mirrors the two-far-corners case - a
                                    tunable needing real device measurement,
                                    same status GATHER_MAX_PIXELS had. */

/* Sets exactly the leaf bits an already-narrowed box spans, in every leaf
 * row it touches. No descent, no recursion - one loop bounded by how many
 * leaf rows the box's own height covers.
 *
 * Only ever called from dirty_mark(), never from mark_band() - this is the
 * "optional" half of the tracking: a caller that only knows a whole band
 * (mark_band()) never pays for this, only one that hands over a real box
 * does. See dirty_mark()'s own comment for the correctness invariant this
 * depends on. */
static inline void mark_leaves(int x0, int y0, int x1, int y1)
{
    const int col_first = x0 / LEAF_W;
    const int col_last  = (x1 - 1) / LEAF_W;
    const int row_first = y0 / LEAF_H;
    const int row_last  = (y1 - 1) / LEAF_H;
    const uint16_t bits =
        (uint16_t)(((1u << (col_last - col_first + 1)) - 1u) << col_first);

    for (int row = row_first; row <= row_last; row++) {
        leaf_dirty[row] |= bits;
    }
}

/* One dirty leaf's rectangle, absolute panel pixels - what dirty_leaf_rects()
 * below hands back, one per dirty leaf intersecting the caller's box. */
typedef struct {
    int x0, y0, x1, y1;
} dirty_leaf_rect_t;

/* Worst case for one strip row: every leaf column dirty in every leaf row of
 * the row, LEAF_COLS * LEAF_SUB. Callers size their output array from this,
 * not a hand-counted number. */
#define LEAF_RECTS_PER_ROW_MAX (LEAF_COLS * LEAF_SUB)

/* Enumerates the dirty leaves of strip `row` that intersect [x0,x1) x
 * [y0,y1), as absolute-pixel rects into `out`, capped at `max_out`. Returns
 * how many were written. Backs the leaf debug-overlay layer in gfx.c
 * (drawn straight into what gets sent, so it needs real rects, not just a
 * bitmask) and is exercised directly by suite_gfx_dirty.c.
 *
 * One rect per dirty leaf, never merged into runs - merging adjacent leaves
 * would hide the very subdivision this layer exists to show. Each rect
 * starts at the leaf's own full LEAF_W x LEAF_H extent, then is clipped to
 * the caller's box: a leaf only partly inside the box yields a clipped
 * rect, a leaf entirely outside yields nothing. Leaf bits are only ever set
 * by dirty_mark() (see mark_leaves()) - a row marked solely by mark_band()
 * has no leaf information, so this legitimately returns nothing for it;
 * that is a consequence of the design, not a bug to fix here.
 *
 * static inline, not plain static: every other function in this file is
 * used unconditionally by gfx.c, but every call site of this one is inside
 * an `#if CONFIG_LAUNCHER_DEVELOPMENT` block, so a release build's gfx.c
 * includes this header and never calls it - the one case in this file the
 * file header's "some static inline" already allows for. Plain static
 * would warn -Wunused-function in exactly that build. */
static inline int dirty_leaf_rects(int row, int x0, int y0, int x1, int y1,
                                   dirty_leaf_rect_t *out, int max_out)
{
    int n = 0;

    for (int sub = 0; sub < LEAF_SUB; sub++) {
        const int leaf_row = row * LEAF_SUB + sub;
        const int ly0 = leaf_row * LEAF_H;
        const int ly1 = ly0 + LEAF_H;
        if (ly1 <= y0 || ly0 >= y1) {
            continue;
        }

        const uint16_t bits = leaf_dirty[leaf_row];
        for (int col = 0; col < LEAF_COLS; col++) {
            if (!(bits & (1u << col))) {
                continue;
            }
            const int lx0 = col * LEAF_W;
            const int lx1 = lx0 + LEAF_W;
            if (lx1 <= x0 || lx0 >= x1) {
                continue;
            }
            if (n == max_out) {
                return n;
            }
            out[n].x0 = (lx0 > x0) ? lx0 : x0;
            out[n].x1 = (lx1 < x1) ? lx1 : x1;
            out[n].y0 = (ly0 > y0) ? ly0 : y0;
            out[n].y1 = (ly1 < y1) ? ly1 : y1;
            n++;
        }
    }
    return n;
}

/* Marks every cell spanned by an ALREADY-CLIPPED row range, full width and
 * full strip height - mark_band() gets no x information at all, so every
 * column in the affected rows has to be assumed dirty across its own full
 * width, which collectively covers the row exactly as the old per-strip
 * version did.
 *
 * Unguarded on purpose beyond the all_dirty/empty-range checks - see the
 * file header comment for why this has to stay inlinable. STRIP_HEIGHT is
 * a power of two, so the divisions become shifts. */
static inline void mark_band(int y0, int y1)
{
    if (all_dirty || y0 >= y1) {
        return;
    }
    const int first = y0 / STRIP_HEIGHT;
    const int last  = (y1 - 1) / STRIP_HEIGHT;

    for (int row = first; row <= last; row++) {
        for (int col = 0; col < GRID_COLS; col++) {
            const int idx = row * GRID_COLS + col;
            cell_dirty |= (1u << idx);
            cell_x0[idx] = col * COL_WIDTH;
            cell_x1[idx] = (col + 1) * COL_WIDTH;
            cell_y0[idx] = row * STRIP_HEIGHT;
            cell_y1[idx] = (row + 1) * STRIP_HEIGHT;
        }
    }
}

/* gfx_mark_all_dirty()'s implementation - named distinctly here only
 * because gfx.c must itself export the public gfx_mark_all_dirty symbol as
 * a thin wrapper around this. */
static inline void dirty_mark_all(void)
{
    if (all_dirty) {
        return;
    }
    all_dirty = true;
    cell_dirty = (CELL_COUNT >= 32) ? 0xFFFFFFFFu : (1u << CELL_COUNT) - 1u;
    for (int row = 0; row < STRIP_COUNT; row++) {
        for (int col = 0; col < GRID_COLS; col++) {
            const int idx = row * GRID_COLS + col;
            cell_x0[idx] = col * COL_WIDTH;
            cell_x1[idx] = (col + 1) * COL_WIDTH;
            cell_y0[idx] = row * STRIP_HEIGHT;
            cell_y1[idx] = (row + 1) * STRIP_HEIGHT;
        }
    }
}

/* Unions the part of (x0,x1) that falls within cell idx's own column - the
 * call's x-range may span several columns, or only part of one, so what
 * belongs to this cell is the intersection with its column, not the call's
 * range as a whole. */
static inline void union_cell_x(int idx, int x0, int x1, int col)
{
    const int col_x0 = col * COL_WIDTH;
    const int col_x1 = col_x0 + COL_WIDTH;
    const int part_x0 = (x0 > col_x0) ? x0 : col_x0;
    const int part_x1 = (x1 < col_x1) ? x1 : col_x1;
    if (part_x0 < cell_x0[idx]) { cell_x0[idx] = part_x0; }
    if (part_x1 > cell_x1[idx]) { cell_x1[idx] = part_x1; }
}

/* Same as union_cell_x(), for the part of (y0,y1) within cell idx's row. */
static inline void union_cell_y(int idx, int y0, int y1, int row)
{
    const int row_y0 = row * STRIP_HEIGHT;
    const int row_y1 = row_y0 + STRIP_HEIGHT;
    const int part_y0 = (y0 > row_y0) ? y0 : row_y0;
    const int part_y1 = (y1 < row_y1) ? y1 : row_y1;
    if (part_y0 < cell_y0[idx]) { cell_y0[idx] = part_y0; }
    if (part_y1 > cell_y1[idx]) { cell_y1[idx] = part_y1; }
}

/* gfx_mark_dirty()'s implementation. Unlike mark_band(), this one knows a
 * real box - callers that draw a shape smaller than a whole cell can say
 * so, in either dimension, and gfx_present() may then send only what
 * actually changed instead of a whole row. Still safe if a caller passes
 * the full width and a whole strip's height like the old callers all did:
 * that just claims every cell in the row, same as before this existed.
 *
 * Also the only place that ever narrows leaf_dirty (see mark_leaves()) -
 * which is what makes "does this cell's box still equal its own full
 * extent" a reliable test of "was this cell touched only by mark_band()
 * this frame, never by a real box": mark_band() always claims a cell at
 * its full COL_WIDTH x STRIP_HEIGHT extent, this union can only ever
 * shrink that towards a real box, and whenever it does, it always narrows
 * the matching leaves in the same call. A cell strictly smaller than its
 * full extent therefore has leaf bits that are always trustworthy for it,
 * with nothing extra to track to know that. */
static inline void dirty_mark(int x, int y, int w, int h)
{
    if (all_dirty) {
        return;
    }

    int x0 = x;
    int x1 = x + w;
    int y0 = y;
    int y1 = y + h;

    if (x0 < 0) x0 = 0;
    if (x1 > GFX_DIRTY_WIDTH) x1 = GFX_DIRTY_WIDTH;
    if (y0 < 0) y0 = 0;
    if (y1 > GFX_DIRTY_HEIGHT) y1 = GFX_DIRTY_HEIGHT;
    if (x0 >= x1 || y0 >= y1) {
        return;
    }

    const int row_first = y0 / STRIP_HEIGHT;
    const int row_last  = (y1 - 1) / STRIP_HEIGHT;
    const int col_first = x0 / COL_WIDTH;
    const int col_last  = (x1 - 1) / COL_WIDTH;

    for (int row = row_first; row <= row_last; row++) {
        for (int col = col_first; col <= col_last; col++) {
            const int idx = row * GRID_COLS + col;
            cell_dirty |= (1u << idx);
            union_cell_x(idx, x0, x1, col);
            union_cell_y(idx, y0, y1, row);
        }
    }

    mark_leaves(x0, y0, x1, y1);
}

/* gfx_region_dirty()'s implementation - x and w are accepted for API
 * symmetry but ignored, matching the public function's own documented
 * contract (see gfx.h). */
static inline bool dirty_region_dirty(int y, int h)
{
    int y0 = y;
    int y1 = y + h;

    if (y0 < 0) y0 = 0;
    if (y1 > GFX_DIRTY_HEIGHT) y1 = GFX_DIRTY_HEIGHT;
    if (y0 >= y1) {
        return false;
    }

    const int first = y0 / STRIP_HEIGHT;
    const int last  = (y1 - 1) / STRIP_HEIGHT;

    for (int row = first; row <= last; row++) {
        for (int col = 0; col < GRID_COLS; col++) {
            if (cell_dirty & (1u << (row * GRID_COLS + col))) {
                return true;
            }
        }
    }
    return false;
}

/* True if any cell in `row` is dirty - what gfx_present() checks before
 * bothering to send anything for it at all. */
static inline bool dirty_row_is_dirty(int row)
{
    return (cell_dirty >> (row * GRID_COLS)) & ((1u << GRID_COLS) - 1u);
}

/* Collects the contiguous set bits of `mask` (bits 0..width-1) into
 * [start,end) ranges - shared by the cell-level and leaf-level run finders
 * below, same shape, different width and mask. Returns how many runs were
 * found, or -1 if there would have been more than `max_runs` - the caller
 * can then tell "fits" from "too fragmented to be worth it" without a
 * second pass. */
static int collect_runs_from_mask(uint32_t mask, int width, int *start,
                                  int *end, int max_runs)
{
    int n = 0;
    int bit = 0;

    while (bit < width) {
        if (!(mask & (1u << bit))) {
            bit++;
            continue;
        }
        if (n == max_runs) {
            return -1;
        }
        start[n] = bit;
        while (bit < width && (mask & (1u << bit))) {
            bit++;
        }
        end[n] = bit;
        n++;
    }
    return n;
}

/* Collects row's dirty columns into contiguous runs - [start,end) column
 * ranges - so cells that are actually adjacent merge into one transaction
 * instead of paying per-transaction overhead once per cell, while a
 * genuine gap of clean columns between two dirty regions keeps them
 * separate rather than gathering a box that spans the untouched middle for
 * no reason. GRID_COLS columns bound this at GRID_COLS/2 runs at most - a
 * run needs at least one gap column to separate it from the next, so the
 * -1 "too fragmented" case from collect_runs_from_mask can never actually
 * trigger here with max_runs == GRID_COLS. */
static int collect_dirty_runs(int row, int *run_start, int *run_end)
{
    const uint32_t bits = (cell_dirty >> (row * GRID_COLS)) &
                          ((1u << GRID_COLS) - 1u);
    const int n = collect_runs_from_mask(bits, GRID_COLS, run_start,
                                         run_end, GRID_COLS);
    return (n < 0) ? 0 : n;
}

/* True only if no cell in [col_first,col_last) of `row` is at its own full
 * COL_WIDTH x STRIP_HEIGHT extent - see dirty_mark()'s comment for why
 * that is both necessary and sufficient to know the leaf bits under this
 * run are trustworthy. Deliberately conservative: one coarse cell in the
 * run disables refinement for the whole run, not just that cell - a run
 * mixing coarse and tight cells is rare in practice, and getting this
 * simple and always-correct matters more than squeezing out that case. */
static bool run_is_leaf_eligible(int row, int col_first, int col_last)
{
    for (int col = col_first; col < col_last; col++) {
        const int idx = row * GRID_COLS + col;
        if (cell_x1[idx] - cell_x0[idx] >= COL_WIDTH &&
           cell_y1[idx] - cell_y0[idx] >= STRIP_HEIGHT) {
            return false;
        }
    }
    return true;
}

/* ORs the run's leaf-rows together, masked to the leaf-columns this run's
 * cells actually span - collapses the vertical dimension on purpose. v1
 * only refines x; the run's own tight cell_y0/cell_y1 union is already
 * exact for the sand app's real 2px-tall rows, which is the case this
 * exists for. */
static uint16_t leaf_mask_for_run(int row, int col_first, int col_last)
{
    const int lc0 = col_first * LEAF_SUB;
    const int lc1 = col_last * LEAF_SUB;
    const uint16_t span = (uint16_t)(((1u << (lc1 - lc0)) - 1u) << lc0);
    uint16_t mask = 0;

    for (int sub = 0; sub < LEAF_SUB; sub++) {
        mask |= leaf_dirty[row * LEAF_SUB + sub];
    }
    return mask & span;
}

/* Tries to split [col_first,col_last) of `row` into up to
 * LEAF_REFINE_MAX_RUNS tighter x-ranges via the leaf layer. Returns 0 -
 * "use the coarse box, there is nothing safe or worthwhile to split on" -
 * whenever the run is not leaf-eligible, has no real internal gap, or is
 * too fragmented for the cap. */
static int refine_run(int row, int col_first, int col_last, int *sx0,
                      int *sx1)
{
    if (!run_is_leaf_eligible(row, col_first, col_last)) {
        return 0;
    }

    const uint16_t mask = leaf_mask_for_run(row, col_first, col_last);
    int lstart[LEAF_REFINE_MAX_RUNS], lend[LEAF_REFINE_MAX_RUNS];
    const int n = collect_runs_from_mask(mask, LEAF_COLS, lstart, lend,
                                         LEAF_REFINE_MAX_RUNS);
    if (n <= 1) {
        return 0;
    }

    for (int i = 0; i < n; i++) {
        sx0[i] = lstart[i] * LEAF_W;
        sx1[i] = lend[i] * LEAF_W;
    }
    return n;
}

/* Wraps refine_run() with the same size budget send_one_row() already
 * enforces for a coarse box. Required, not a nicety: a run with a small
 * gap can still split into pieces each individually bigger than
 * gather_buf's fixed GATHER_MAX_PIXELS allocation - skipping this check
 * risks a buffer overflow into DMA-mapped memory, not a graceful
 * degradation. Falls back to the coarse box (0) if any split fails it. */
static int plan_run(int row, int col_first, int col_last, int y0, int y1,
                    int *sx0, int *sx1)
{
    const int n = refine_run(row, col_first, col_last, sx0, sx1);

    for (int i = 0; i < n; i++) {
        const size_t area = (size_t)(sx1[i] - sx0[i]) * (size_t)(y1 - y0);
        if (area > GATHER_MAX_PIXELS) {
            return 0;
        }
    }
    return n;
}

/* The union box across columns [start,end) of row - every column in a
 * contiguous run is already confirmed dirty, so this only needs to widen,
 * never test. */
static void run_box(int row, int start, int end, int *x0, int *x1, int *y0,
                    int *y1)
{
    *x0 = GFX_DIRTY_WIDTH;
    *x1 = 0;
    *y0 = row * STRIP_HEIGHT + STRIP_HEIGHT;
    *y1 = row * STRIP_HEIGHT;

    for (int col = start; col < end; col++) {
        const int idx = row * GRID_COLS + col;
        if (cell_x0[idx] < *x0) { *x0 = cell_x0[idx]; }
        if (cell_x1[idx] > *x1) { *x1 = cell_x1[idx]; }
        if (cell_y0[idx] < *y0) { *y0 = cell_y0[idx]; }
        if (cell_y1[idx] > *y1) { *y1 = cell_y1[idx]; }
    }
}

/* Resets one row's cell boxes and leaves to "nothing yet", once
 * gfx_present() has sent it - next frame's marking calls union against
 * this, so each cell has to start from empty rather than keep growing
 * forever. The leaf reset matters just as much: a stale leaf bit from a
 * past frame would make a cell that is genuinely fully dirty this frame
 * look like it has a gap that is not real, silently dropping pixels that
 * do need sending. */
static inline void dirty_row_sent(int row)
{
    for (int col = 0; col < GRID_COLS; col++) {
        const int idx = row * GRID_COLS + col;
        cell_x0[idx] = (col + 1) * COL_WIDTH;
        cell_x1[idx] = col * COL_WIDTH;
        cell_y0[idx] = row * STRIP_HEIGHT + STRIP_HEIGHT;
        cell_y1[idx] = row * STRIP_HEIGHT;
    }
    for (int sub = 0; sub < LEAF_SUB; sub++) {
        leaf_dirty[row * LEAF_SUB + sub] = 0;
    }
}

/* Resets whole-frame state once gfx_present() has finished sending
 * everything it found dirty. */
static inline void dirty_frame_sent(void)
{
    cell_dirty = 0;
    all_dirty = false;
}
