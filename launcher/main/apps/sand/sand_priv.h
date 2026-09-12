/*
 * sand_priv - internals shared across sand.c, sand_liquid.c, sand_gas.c,
 * sand_reactions.c, sand_plants.c and sand_impulse.c.
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
 * THE CRITERION FOR WHAT ELSE LIVES HERE: not purity (this header is
 * app-internal and portable either way, so nothing structurally stops a
 * `static` helper from moving here) but whether a test needs to call it
 * directly. blocker_normal(), reflect_off_normal() and impulse_drag_of() are
 * here because the sand test suite (suite_sand_*.c) cannot reach a function
 * `static` inside a .c file, only one declared where it can include it.
 * can_impulse_enter(), can_impulse_enter_gravity_ward() and
 * impulse_gravity_candidates() stay `static` in sand_impulse.c, next to
 * step_impulses(), because nothing has yet needed to drive one in isolation
 * - despite being equally pure, and despite a documented history of their
 * own two call sites disagreeing about what they compute (see
 * impulse_gravity_candidates()'s own comment in sand_impulse.c; bd
 * esp32c6-w2h), exactly what a direct test would have caught sooner. Move a
 * helper here when something actually needs to test it directly.
 */
#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "sand.h"

/* NULL if off grid; vertical bounds checked per row, not per grain. */
static inline uint8_t*
dest_row(const sand_t* s, int y) {
    if (y < 0 || y >= s->h) {
        return NULL;
    }
    return s->cells + (size_t)y * (size_t)s->w;
}

/* ALSO THE BOARD-CHANGED SIGNAL, not only a repaint request: a changed cell
 * that is not repainted is a visible bug, so every writer already comes
 * through here - the one place a content change can be seen without auditing
 * them all. See faller_may_move in sand.h for what rests on that. */
static inline void
mark_rows(sand_t* s, int y0, int y1) {
    s->faller_may_move = true;
    if (s->dirty_rows != NULL) {
        if ((unsigned)y0 < (unsigned)s->h) {
            s->dirty_rows[y0] = 1;
        }
        if ((unsigned)y1 < (unsigned)s->h) {
            s->dirty_rows[y1] = 1;
        }
    }
}

/* Only pour_into()'s was_empty triggers this: the only event that moves
 * a puddle's surface, so the only one that can make LOCAL DEPTH stale.
 * Mass between already-liquid cells never calls it - dirtying for that
 * would repaint a settled reservoir every step something levels out.
 * Marks a BAND, not two points: anything within
 * MATERIAL_LIQUID_DEPTH_BAND of the surface can read differently,
 * further out already saturates. Direction-agnostic, avoiding coupling
 * to app_sand.c's gravity bookkeeping. */
static inline void
mark_depth_band(sand_t* s, int y) {
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

/* Not sand.h API: a test hook for the gas spread pass's row skip. With it on,
 * every board a suite already runs becomes a check that no mover moved gas
 * into a row the skip had written off. Counts rather than aborts, so one
 * failure does not hide the rest. */
void sand_gas_row_audit_enable(bool on);
extern unsigned sand_gas_row_audit_failures;
extern unsigned sand_gas_row_audit_skippable;

#define BLOCK_SETTLED_NEAREST 0x1
#define BLOCK_SETTLED_OTHER   0x2
#define BLOCK_ACTIVE          0x4

/* THE INVARIANT the skip rests on: every liquid cell sits in a block
 * whose NEAR bit is set. Either the sweep saw it, or it arrived from
 * within SAND_LIQUID_SIGHT (under SAND_BLOCK_W), so its source block is
 * this one or a neighbour, already covered by NEAR. New liquid must go
 * through mark_move() (clears settled bits on a 3x3) via place_cell()'s
 * latch_content_flags() for this to hold. Contrapositive:
 * equalise_liquids() sees NEAR clear as provably no liquid, so skipping
 * those cells changes nothing. */
#define BLOCK_HAS_LIQUID      0x8
#define BLOCK_LIQUID_NEAR     0x10

static inline uint16_t
liquid_mask(void) {
    uint16_t mask = 0;
    for (int m = 0; m < MATERIAL_MAX; m++) {
        if (material_by_id((material_id_t)m)->kind == KIND_LIQUID) {
            mask |= (uint16_t)(1u << m);
        }
    }
    return mask;
}

/* What a liquid could still put moisture INTO: something that drinks it
 * directly, or ground that soaks. A board holding a liquid and none of these
 * has no way to make moisture at all, which is what lets the reaction pass
 * skip a screen of water outright. MAT_EXTENDED is one bit over sixteen
 * codes, so it joins if any of them qualifies - the conservative direction. */
static inline uint16_t
wettable_mask(void) {
    uint16_t mask = 0;
    for (int m = 1; m < MAT_COUNT; m++) {
        if (reactions[m].soaks != 0 || reactions[m].drinks != 0) {
            mask |= (uint16_t)(1u << m);
        }
    }
    for (int k = 0; k < MATERIAL_EXTENDED_CODES; k++) {
        if (extended_reactions[k].soaks != 0 || extended_reactions[k].drinks != 0) {
            mask |= (uint16_t)(1u << MAT_EXTENDED);
        }
    }
    return mask;
}

/* A full cell, a foreign material and a wall all refuse mass alike, so this
 * answers for every liquid at once without being told which one is asking.
 * Breaks on the first cell that could take mass: a span still moving costs a
 * handful of loads, not its length. */
static inline bool
span_has_no_liquid_room(const uint8_t* row, int x0, int x1, uint16_t is_liquid) {
    for (int x = x0; x < x1; x++) {
        const cell_t c = row[x];
        if (CELL_IS_EMPTY(c)) {
            return false;
        }
        if (((is_liquid >> CELL_MATERIAL(c)) & 1u) != 0 && CELL_VARIANT(c) < MASS_MAX) {
            return false;
        }
    }
    return true;
}

static inline int
block_of(const sand_t* s, int x, int y) {
    return (y / SAND_BLOCK_H) * s->block_cols + (x / SAND_BLOCK_W);
}

/* Has this cell's block come to rest? The same test sand_block_settled()
 * makes, by cell rather than by block index. False when sleeping is off,
 * because then nothing is ever known to be settled and a rule gated on rest
 * must not fire. */
static inline bool
cell_settled(const sand_t* s, int x, int y) {
    if (s->block_state == NULL) {
        return false;
    }
    return (s->block_state[block_of(s, x, y)]
            & (BLOCK_SETTLED_NEAREST | BLOCK_SETTLED_OTHER)) != 0;
}

/* Is there liquid in this cell's own block or any block touching it?
 *
 * NOT s->may_have_liquid, which is board-wide: one water cell anywhere arms
 * every liquid-adjacent behaviour on the whole grid. BLOCK_LIQUID_NEAR asks
 * the same question locally.
 *
 * SOUND FOR ANY FOUR-NEIGHBOUR TEST: a neighbour is one cell away, so it
 * lies in this block or one touching it, and NEAR covers exactly that.
 * Falls back to the flag when block state is off. */
static inline bool
liquid_near(const sand_t* s, int x, int y) {
    if (!s->may_have_liquid) {
        return false;
    }
    if (s->block_state == NULL) {
        return true;
    }
    return (s->block_state[block_of(s, x, y)] & BLOCK_LIQUID_NEAR) != 0;
}

static inline void
wake_blocks_range(sand_t* s, int bx0, int by0, int bx1, int by1) {
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
            s->block_state[by * s->block_cols + bx] &= (uint8_t)~(BLOCK_SETTLED_NEAREST | BLOCK_SETTLED_OTHER);
        }
    }
    s->block_state[by0 * s->block_cols + bx0] |= BLOCK_ACTIVE;
    s->block_state[by1 * s->block_cols + bx1] |= BLOCK_ACTIVE;
}

static inline bool
any_neighbor_active(const sand_t* s, int bx, int by) {
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
static inline bool
block_or_neighbour_has_liquid(const sand_t* s, int bx, int by) {
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

/* Used only for touches OUTSIDE the gravity sweep: no moved_here-style
 * bookkeeping exists for any_neighbor_active() to observe next step,
 * unlike a sweep-internal move where the destination is always the
 * source's own neighbour. Unconditional 3x3, not edge-aware like
 * point_reach(): this runs at interaction rate, not per-grain-move, so
 * precision is not needed - see
 * test_undermining_a_sleeping_pile_collapses_it: erasing must wake a
 * NEIGHBOURING block's resting pile, with no sweep-internal fallback. */
static inline void
wake_block_and_neighbors(sand_t* s, int x, int y) {
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
            s->block_state[ny * s->block_cols + nx] &= (uint8_t)~(BLOCK_SETTLED_NEAREST | BLOCK_SETTLED_OTHER);
        }
    }
    s->block_state[by * s->block_cols + bx] |= BLOCK_ACTIVE;
}

/* ONE copy: sand_set()/try_spawn_one() once each carried their own,
 * drifted. Independent ifs, not else-if: fire is BOTH KIND_GAS (rises)
 * AND reactions[].burns (reacts), needing both flags. Takes a CELL:
 * cold glass needs no flag yet, snow always does regardless of variant.
 * Ring order below: neighbours of any direction are the entries either
 * side, working at any gravity angle. Shared with sand.c: "either side
 * of up" is NOT up plus a perpendicular except when axis-aligned. */
static inline const int*
ring_dir(int i) {
    static const int ring8[8][2] = {
        {0, 1},   /* 0  down            */
        {1, 1},   /* 1  down-right      */
        {1, 0},   /* 2  right           */
        {1, -1},  /* 3  up-right        */
        {0, -1},  /* 4  up              */
        {-1, -1}, /* 5  up-left         */
        {-1, 0},  /* 6  left            */
        {-1, 1},  /* 7  down-left       */
    };
    return ring8[i & 7];
}

static inline int
ring_of(int dx, int dy) {
    for (int i = 0; i < 8; i++) {
        const int* d = ring_dir(i);
        if (d[0] == dx && d[1] == dy) {
            return i;
        }
    }
    return 0; /* unreachable for a unit direction */
}

/* KIND_STATIC wall-bounce geometry for step_impulses() (sand_impulse.c). Same
 * 3-cell arc as cover_mask(), same reason. sand_at()'s out-of-bounds-is-
 * STONE makes board edges bounce too. Quantised by MAGNITUDE, not sign:
 * sign-per-axis makes every diagonal dir degenerate to a plain reverse
 * (centre cell contributes -1 both axes), so diagonal throws could never
 * glance. Comparing magnitudes - larger axis wins, both count within 2x -
 * lets diagonal hits glance while a flat wall still reverses exactly. */
static inline int
blocker_normal(const sand_t* s, int x, int y, int dir) {
    int sx = 0;
    int sy = 0;
    bool any = false;

    for (int i = -1; i <= 1; i++) {
        const int* d = ring_dir(dir + i);
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

static inline int
reflect_off_normal(int dir, int normal) {
    const int* d = ring_dir(dir);
    const int* n = ring_dir(normal);
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
neighbor_smothers(const sand_t* s, int nx, int ny, int w, int h, uint8_t density) {
    if ((unsigned)nx >= (unsigned)w || (unsigned)ny >= (unsigned)h) {
        return false;
    }
    const cell_t n = s->cells[(size_t)ny * (size_t)w + (size_t)nx];
    if (CELL_IS_EMPTY(n)) {
        return false;
    }
    const material_t* nm = material_of(n);
    return nm->kind != KIND_LIQUID && nm->density > density;
}

/* Replaces cover_count() (sand_reactions.c), which counted screen-fixed
 * cardinals and could never fire for a wide pool sealed by a crust (only
 * the cell directly above ever counted). The lid is the three cells
 * centred on anti-gravity - opposite gravity plus its two diagonals -
 * ALL THREE must cover; the two perpendiculars alone (five-cell
 * semi-disc) read a hand-drawn wall notch as a seal at brush radii 2-4,
 * bursting basins that should hold. */
#define COVER_LID 0x7u

/* Covering is neighbor_smothers(): in bounds, not liquid, denser than
 * the asking cell; out of bounds never counts. Uses SETTLED gravity
 * (s->last_load_dx/dy), not the dithered per-step direction - dithering
 * would swing the lid between two orientations every step, flickering a
 * cell that is genuinely sealed. */
static inline unsigned
cover_mask(const sand_t* s, int x, int y, int w, int h, uint8_t density) {
    const int anti = ring_of(s->last_load_dx, s->last_load_dy) + 4;
    unsigned mask = 0;
    for (int i = 0; i < 3; i++) {
        const int* d = ring_dir(anti - 1 + i);
        if (neighbor_smothers(s, x + d[0], y + d[1], w, h, density)) {
            mask |= 1u << i;
        }
    }
    return mask;
}

/* Checks if (x, y) has a complete lid over it, gravity-relative. */
static inline bool
covered_at(const sand_t* s, int x, int y, int w, int h, uint8_t density) {
    return cover_mask(s, x, y, w, h, density) == COVER_LID;
}

/* One cullet grain, at a random shade from sand's reserved band. Shared by
 * every path that breaks glass - a crack, and a pane knocked loose by an
 * impulse - so they cannot drift apart on which band they land in. */
static inline cell_t cullet_cell(sand_t *s)
{
    return CELL_MAKE(MAT_SAND,
                     (uint8_t)(SAND_CULLET_BASE + rng_below(&s->rng, SAND_CULLET_SHADES)));
}

static inline uint8_t impulse_drag_of(cell_t displaced)
{
    const material_t *m = material_of(displaced);
    unsigned d = (unsigned)m->density;

    if (m->kind == KIND_LIQUID) {
        return 0u; /* a fluid parts around a mover - see the constant */
    }
    if (m->kind == KIND_POWDER) {
        d <<= SAND_IMPULSE_DRAG_POWDER_SHIFT;
    }
    return (uint8_t)(d > 255u ? 255u : d);
}

static inline void
clear_content_flags(sand_t* s) {
    s->may_have_liquid = false;
    s->may_have_gas = false;
    s->may_have_burning = false;
    s->may_have_dissolver = false;
    s->may_have_temperature = false;
    s->may_have_moisture = false;
    s->may_have_faller = false;
    /* The pessimistic direction, unlike the flags around it: a board filled
     * by writing s->cells directly latches no presence either, so this one
     * being true changes nothing until something says a faller is there. */
    s->faller_may_move = true;
    s->may_have_heat_holder = false;

    s->may_have_condenser = false;

    /* EVERY material, and the only one here that starts set. The others gate
     * work that is merely wasted when the flag is wrong; this one gates work
     * being SKIPPED, so a false negative loses a reaction. A board filled by
     * writing s->cells directly - which tests and tools do - latches nothing,
     * so it starts pessimistic and sand_step_reactions() narrows it. */
    s->may_have_materials = 0xFFFFu;
}

static inline void
latch_content_flags(sand_t* s, cell_t cell) {
    if (CELL_IS_EMPTY(cell)) {
        return;
    }
    const material_t* mat = material_of(cell);
    const reaction_t* r = reaction_of(cell); /* decodes MAT_EXTENDED */

    if (mat->kind == KIND_LIQUID) {
        s->may_have_liquid = true;
    }
    if (mat->kind == KIND_GAS) {
        s->may_have_gas = true;
    }
    /* One OR, and deliberately no predicate: what this material IMPLIES is
     * decided in sand_step_reactions() where build_reaction_tables()' table
     * exists. Duplicating those predicates here is what this replaced, and it
     * had already drifted into two copies. */
    s->may_have_materials |= (uint16_t)(1u << CELL_MATERIAL(cell));
    if (cell_is_burning(cell)) {
        s->may_have_burning = true;
    }
    if (r->dissolves) {
        s->may_have_dissolver = true;
    }
    if (r->falls != 0) {
        s->may_have_faller = true;
        s->faller_may_move = true;
    }
    if (r->condenses != 0) {
        s->may_have_condenser = true;
    }
    if (r->chills != 0 || r->warms != 0 || (r->heat_ramp != 0 && CELL_VARIANT(cell) != SAND_AMBIENT_HEAT)) {
        s->may_have_temperature = true;
    }
    if (r->heat_ramp != 0 || (r->chills != 0 && r->heats_to != 0 && r->heat_chance != 0)) {
        s->may_have_heat_holder = true;
    }
    if (mat->kind == KIND_LIQUID || (r->dries != 0 && moisture_of(cell, r) != 0)) {
        s->may_have_moisture = true;
    }
}

/* The four cardinals every per-cell reaction pass walks - fire chemistry and
 * tree growth both need it, unlike ring_dir()'s 8-way table above.
 *
 * int8_t, not int, is what fs7 bought: 8 bytes a copy, not 32. `static` pays
 * one extra copy for the four `#pragma GCC unroll 4` walks in
 * sand_reactions.c - behind `extern` those pragmas are worth under half as
 * much. Neither form folds the offsets (objdump: `lb 0(a5)` survives every
 * unrolled copy), so `extern` is not a way to keep the win. */
static const int8_t reaction_dirs[4][2] = {
    {0, -1},
    {0, 1},
    {-1, 0},
    {1, 0},
};

/* Use precomputed `at` index to write `mat` into cell. Every cell creation
 * goes through here. */

/* may_have_* latching needed; different cell kinds created */

/* may_have_* flag required; forget leads to frozen cell */

static inline void
place_cell(sand_t* s, int x, int y, size_t at, cell_t c) {
    s->cells[at] = c;
    latch_content_flags(s, c);
    mark_rows(s, y, y);
    wake_block_and_neighbors(s, x, y);
}

static inline void
place_reacted(sand_t* s, int x, int y, size_t at, uint8_t spec) {
    if (spec >= (MAT_EXTENDED << 4)) {
        place_cell(s, x, y, at, (cell_t)spec); /* identity IS low nibble for
                                                 * static (0xF0-F7) */
        return;
    }
    const material_id_t mat = (material_id_t)spec;
    /* HEAT starts at zero. MATERIAL_VARIANTS - 1 turns sand to lava, wood to
     * flame. Cold tracks exposure. Use place_cell() for non-fire. */
    place_cell(s, x, y, at, CELL_MAKE(mat, reactions[mat].heat_ramp != 0 ? SAND_AMBIENT_HEAT : MATERIAL_VARIANTS - 1));
}

/* Cell pays 1 mass. Stops fire from draining. Preserves slow quench. Follows
 * give_mass(). */

/* Fire's own helper, but sand_plants.c's step_one_drinking_cell() calls it
 * too, when a tree drinks from a puddle. */
static inline void
pay_quench_cost(sand_t* s, int nx, int ny, int w) {
    const size_t at = (size_t)ny * (size_t)w + (size_t)nx;
    const cell_t n = s->cells[at];
    const int mass = CELL_VARIANT(n) - 1;
    s->cells[at] = (mass > 0) ? CELL_MAKE(CELL_MATERIAL(n), mass) : CELL_EMPTY;
    mark_rows(s, ny, ny);
    wake_block_and_neighbors(s, nx, ny);
}

/* Dry front marks zero moisture, new tone, skips flat soil. See
 * CELL_WITH_MOISTURE(). */

/* nearby_moisture: moisture level at last watering hand-off or root sink */

/* Pass 0 for no neighbour; cell dries to same look. */

/* SCALED THROUGH `dry_tone_from_moisture()` - `nearby_moisture` equals tone
 * if ranges match. */

static inline cell_t
soil_dry_out(cell_t c, uint8_t nearby_moisture) {
    /* Handles any material; avoids MAT_EXTENDED index error. Byte-identical
     * to CELL_SOIL() for dirt. */
    const reaction_t* r = reaction_of(c);
    return soil_cell(c, dry_tone_from_moisture(nearby_moisture, r), 0, r);
}

_Static_assert(SOIL_DRY_TONES - 1 == SOIL_MOISTURE_MAX, "dry_tone_from_moisture() is identity for dirt only because "
                                                        "these two ranges are the same width - the fingerprint gate "
                                                        "is what actually proves soil_dry_out() stayed byte-"
                                                        "identical for dirt after this stopped being a direct read");

/* All sites changing soil moisture call this instead of CELL_WITH_MOISTURE().
 * Calls soil_dry_out() at zero moisture, adjusted by `nearby_moisture`.
 * Fire's soaking/heat-transform passes and sand_plants.c's root conduction
 * and growth-cost spending all change soil moisture through here. */
static inline cell_t
soil_set_moisture(cell_t c, uint8_t new_moisture, uint8_t nearby_moisture) {
    return new_moisture != 0 ? with_moisture(c, new_moisture, reaction_of(c)) : soil_dry_out(c, nearby_moisture);
}

static inline void
mark_move(sand_t* s, int x0, int y0, int x1, int y1) {
    mark_rows(s, y0, y1);
    wake_block_and_neighbors(s, x0, y0);
    wake_block_and_neighbors(s, x1, y1);
}

/* tick_decay(): returns whether the cell is occupied - caller must not
 * treat a vanished grain as still there. Writes the value back before
 * the caller uses *grain again, since move_to() trusts its argument
 * over re-reading row[x]. Shared between sand_gas.c/sand_reactions.c:
 * generic, not duplicated. tick_decay_at(): split for materials whose
 * variant is life but decay 0 (wood, burning). Burns at r->lit_from,
 * not hardcoded 1 - gunpowder needs this since lit is not always the
 * whole nibble. */
static inline bool
tick_decay_at(sand_t* s, uint8_t* row, int x, int y, cell_t* grain, const reaction_t* r, int decay) {
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

/* Takes the rate with s->decay already applied, not the material row: a
 * caller holding it as a per-material fact must not be made to load
 * materials[] again for it. */
static inline bool
tick_decay(sand_t* s, uint8_t* row, int x, int y, cell_t* grain, uint8_t mat_id, int decay) {
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

/* Per-pass volatile gates (bd esp32c6-8zx, sand.c), default enabled,
 * opt-in via CONFIG_LAUNCHER_SAND_PASS_GATES (dev-only). SCAFFOLDING,
 * removed by scripts/strip-pass-gates.py - volatile is load-bearing: an
 * #if would let the compiler delete the walk it guards.
 * sand_step_reactions() (sand_reactions.c): a step's fire chemistry,
 * called after sand_step_gas(), before finalize_settling(). Gated on
 * may_have_burning alone; takes only `s` - boiling happens at the heat
 * source, no interest in gravity. */
void sand_step_reactions(sand_t* s);

/* Defined in sand_plants.c: the tree/root/leaf growth stages of
 * step_one_reacting_row()'s (sand_reactions.c) per-cell dispatch, called
 * across the file boundary the same way sand_step_reactions() above is
 * called from sand.c.
 *
 * sand_disc_count(): exact lattice-cell count for a disc of radius r
 * (sand_impulse.c). Declared here rather than left static so the suite
 * can check the shipped table against a direct count, the only way that
 * table is verified. */
int sand_disc_count(int radius);

/* The fall stage's own question, minus the roll and the write. Declared here
 * rather than left static because may_have_faller is only allowed to be clear
 * while this answers no everywhere, and a suite cannot check that without
 * asking the same question the pass asks. */
bool faller_can_move(sand_t* s, int x, int y, int w, int h, const reaction_t* r);

bool step_one_falling_cell(sand_t* s, int x, int y, int w, int h, const reaction_t* r);
bool step_one_conducting_cell(sand_t* s, int x, int y, int w, int h, const reaction_t* r);
bool step_one_rooting_cell(sand_t* s, int x, int y, int w, int h, const reaction_t* r);
bool step_one_drinking_cell(sand_t* s, int x, int y, int w, int h, const reaction_t* r, cell_t self);
bool step_one_sprouting_cell(sand_t* s, int x, int y, int w, int h, const reaction_t* r);
bool step_one_budding_cell(sand_t* s, int x, int y, int w, int h, const reaction_t* r);
bool step_one_growing_cell(sand_t* s, int x, int y, int w, int h, const reaction_t* r);

/* A pool's true perpendicular to gravity rarely lines up with a ring
 * direction - bracketed between an axis ray (ax) and the diagonal
 * beside it (dg). q_q8 picks which ray a column takes, as a fixed SPACE
 * pattern, not dithered in TIME: time-dithering let a settled pool see
 * a different axis - and verdict on down - almost every step, swinging
 * mass back and forth. Space-dithering gives every column the same
 * answer every step, while the MIX across the pool still reads as the
 * true angle. */
typedef struct {
    int ax[2];      /* the axis ray - perpendicular to the dominant axis */
    int dg[2];      /* the diagonal ray beside it, the way the tilt leans */
    int q_q8;       /* how often a column takes the diagonal ray, 0-256   */
    int bias_ax_q8; /* mass a level surface gains per step of each ray,   */
    int bias_dg_q8; /*   in 1/256 units - zero when the ray is level      */
} xflow_t;

/* Cross-flow levelling. Called from `sand_step()`. `flow` levels, `dx`/`dy`
 * gravity direction. */
void sand_step_liquids(sand_t* s, const xflow_t* flow, int dx, int dy);

void sand_step_gas(sand_t* s, int gx, int gy, int dx, int dy, const int* slide_a, const int* slide_b, const int* perp_a,
                   const int* perp_b, int load_dx, int load_dy, int x_step, int jostle);

/* The flight pass - explosions, debris, splash pushback - lives in
 * sand_impulse.c since it moves OUTWARD, not gravity-ward. Called once
 * from sand_step(), the same seam sand_step_liquids()/sand_step_gas() use;
 * must run LAST - see sand_impulse.c's own banner. */
void step_impulses(sand_t *s, int dx, int dy);

/* try_fall_or_scatter()/try_slide() live here, static inline, same
 * reason as dest_row()/mark_rows(): hottest path, called once per grain
 * per step. sand_gas.c calls thin non-inline wrappers in sand.c instead
 * of un-static-ing these or duplicating the chain - both regressed a
 * frame-budget test (lost inlining, or duplicated flash). See
 * docs/sand/Simulation-Lessons.md. */

/* Static materials never yield regardless of density, so a wall stays a
 * wall - the general "yields to denser" rule below has this one
 * exception. */
static inline bool
can_enter(uint8_t mover_density, uint8_t mover_id, cell_t target) {
    if (CELL_IS_EMPTY(target)) {
        return true;
    }

    const material_t* t = material_of(target);

    if (!(t->kind == KIND_LIQUID || t->kind == KIND_GAS) || mover_density <= t->density) {
        return false;
    }

    return !(mover_id == MAT_SAND && CELL_MATERIAL(target) == MAT_OIL);
}

/* Check cell existence for scatter decision. Avoids redundant random number
 * generation. */
static inline bool
cell_open(const uint8_t* row, int nx, int w, uint8_t density, uint8_t mat_id) {
    return row != NULL && (unsigned)nx < (unsigned)w && can_enter(density, mat_id, row[nx]);
}

/* Single comparison catches nx < 0 by wrapping. */
static inline bool
move_to(uint8_t* from_row, uint8_t* to_row, int x, int nx, int w, cell_t mover, uint8_t density) {
    if (to_row == NULL || (unsigned)nx >= (unsigned)w || !can_enter(density, CELL_MATERIAL(mover), to_row[nx])) {
        return false;
    }

    const cell_t displaced = to_row[nx];

    to_row[nx] = mover;
    from_row[x] = displaced;
    return true;
}

static inline int
slide_chance(const material_t* m, int load, int jostle) {
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

static inline bool
try_scatter(sand_t* s, uint8_t* row, uint8_t* prow, uint8_t* arow, uint8_t* brow, int x, int y, int w, int dx,
            const int* slide_a, const int* slide_b, cell_t grain, uint8_t density, int scatter) {
    if (scatter == 0 || !cell_open(prow, x + dx, w, density, CELL_MATERIAL(grain))) {
        return false;
    }

    const uint32_t r = rng_next(&s->rng);
    if ((int)(r & 0xFF) >= scatter) {
        return false;
    }

    if ((r & 0x100) == 0) {
        const bool pick_a = (r & 0x200) != 0;
        uint8_t* drow = pick_a ? arow : brow;
        const int ddx = pick_a ? slide_a[0] : slide_b[0];
        const int ddy = pick_a ? slide_a[1] : slide_b[1];

        if (move_to(row, drow, x, x + ddx, w, grain, density)) {
            mark_rows(s, y, y + ddy);
        }
    }
    return true;
}

static inline bool
try_fall_or_scatter_impl(sand_t* s, uint8_t* row, uint8_t* prow, uint8_t* arow, uint8_t* brow, int x, int y, int w,
                         int dx, int dy, const int* slide_a, const int* slide_b, cell_t grain, uint8_t density,
                         int scatter) {
    if (try_scatter(s, row, prow, arow, brow, x, y, w, dx, slide_a, slide_b, grain, density, scatter)) {
        return true;
    }

    if (move_to(row, prow, x, x + dx, w, grain, density)) {
        mark_rows(s, y, y + dy);
        return true;
    }
    return false;
}

static inline void
pick_slide_order(uint32_t r, uint8_t* arow, uint8_t* brow, const int* slide_a, const int* slide_b, uint8_t driven_row,
                 bool driven[][2], uint8_t** first_row, int* first_dx, int* first_dy, bool* first_driven,
                 uint8_t** second_row, int* second_dx, int* second_dy, bool* second_driven) {
    if (r & 1) {
        *first_row = arow;
        *first_dx = slide_a[0];
        *first_dy = slide_a[1];
        *first_driven = driven[driven_row][0];
        *second_row = brow;
        *second_dx = slide_b[0];
        *second_dy = slide_b[1];
        *second_driven = driven[driven_row][1];
    } else {
        *first_row = brow;
        *first_dx = slide_b[0];
        *first_dy = slide_b[1];
        *first_driven = driven[driven_row][1];
        *second_row = arow;
        *second_dx = slide_a[0];
        *second_dy = slide_a[1];
        *second_driven = driven[driven_row][0];
    }
}

/* Friction on slides: grain shuffle depends on surface. Reached if
 * gravity-ward move fails. Grain skips this in open air. */
static inline bool
try_slide_pair(sand_t* s, uint8_t* row, int x, int y, int w, cell_t grain, uint8_t density, const material_t* mat,
               int load_dx, int load_dy, int jostle, uint32_t r, uint8_t* first_row, int first_dx, int first_dy,
               bool first_driven, uint8_t* second_row, int second_dx, int second_dy, bool second_driven) {
    const int load = sand_load_above(s, x, y, load_dx, load_dy);
    const int allowance = slide_chance(mat, load, jostle);
    if (allowance < 256 && (int)((r >> 16) & 0xFF) >= allowance) {
        return false;
    }

    if (first_driven && move_to(row, first_row, x, x + first_dx, w, grain, density)) {
        mark_rows(s, y, y + first_dy);
        return true;
    }
    if (second_driven && move_to(row, second_row, x, x + second_dx, w, grain, density)) {
        mark_rows(s, y, y + second_dy);
        return true;
    }
    return false;
}

static inline bool
try_slide_impl(sand_t* s, uint8_t* row, uint8_t* prow, uint8_t* arow, uint8_t* brow, int x, int y, int w, int dx,
               int dy, const int* slide_a, const int* slide_b, int load_dx, int load_dy, int jostle, cell_t grain,
               uint8_t driven_row, uint8_t density, const material_t* mat, bool driven[][2]) {
    const uint32_t r = rng_next(&s->rng);

    uint8_t *first_row, *second_row;
    int first_dx, second_dx;
    int first_dy, second_dy;
    bool first_driven, second_driven;
    pick_slide_order(r, arow, brow, slide_a, slide_b, driven_row, driven, &first_row, &first_dx, &first_dy,
                     &first_driven, &second_row, &second_dx, &second_dy, &second_driven);

    /* Shaken grain spreads sideways before dropping. Every destination stays
     * inside already-swept half. */
    const bool shaken = jostle > 0 && (int)((r >> 8) & 0xFF) < jostle;

    if (!shaken && jostle > 0 && move_to(row, prow, x, x + dx, w, grain, density)) {
        mark_rows(s, y, y + dy);
        return true;
    }

    if (try_slide_pair(s, row, x, y, w, grain, density, mat, load_dx, load_dy, jostle, r, first_row, first_dx, first_dy,
                       first_driven, second_row, second_dx, second_dy, second_driven)) {
        return true;
    }

    if (shaken && move_to(row, prow, x, x + dx, w, grain, density)) {
        mark_rows(s, y, y + dy);
        return true;
    }

    return false;
}

bool try_fall_or_scatter(sand_t* s, uint8_t* row, uint8_t* prow, uint8_t* arow, uint8_t* brow, int x, int y, int w,
                         int dx, int dy, const int* slide_a, const int* slide_b, cell_t grain, uint8_t density,
                         int scatter);

/* `driven_row`/`bool driven[][2]`, not `mat_id`/`driven[MATERIAL_MAX][2]`:
 * step_one_grain() (sand.c) backs this with driven[MATERIAL_ROWS][2]
 * indexed by row (grain >> 3), while sand_gas.c backs it with
 * driven_gas[MATERIAL_MAX][2] indexed by material id - two different real
 * bounds behind the same shape, so naming either one here was never
 * accurate for both callers. */
bool try_slide(sand_t* s, uint8_t* row, uint8_t* prow, uint8_t* arow, uint8_t* brow, int x, int y, int w, int dx,
               int dy, const int* slide_a, const int* slide_b, int load_dx, int load_dy, int jostle, cell_t grain,
               uint8_t driven_row, uint8_t density, const material_t* mat, bool driven[][2]);

/* Moved from sand.c. sand_gas.c uses REVERSED gravity vector. */
static inline bool
driven_by_gravity(int mx, int my, int gx, int gy, int repose) {
    const int descent = mx * gx + my * gy;
    if (descent <= 0) {
        return false; /* uphill, or across a level slope */
    }
    if (repose == 0) {
        return true; /* no friction angle at all - a liquid */
    }

    int lateral = mx * gy - my * gx;
    if (lateral < 0) {
        lateral = -lateral;
    }

    return (int64_t)descent * 10 > (int64_t)lateral * repose;
}

/* RSTAGE_BURN_ANY must stay 0: the per-material burn plans (sand_reactions.c)
 * are zero-initialised .bss, so an unwritten slot lands here - see
 * step_one_reacting_row()'s stage_burn_any: label for why it has to be this
 * stage and not one of the other two burn stages. */
enum {
    RSTAGE_BURN_ANY,
    RSTAGE_BURN_ALWAYS,
    RSTAGE_BURN_CHECK,
    RSTAGE_DISSOLVE,
    RSTAGE_ACID_RAIN,
    RSTAGE_CONDENSE,
    RSTAGE_HEAT_RAMP,
    RSTAGE_CRUST,
    RSTAGE_CHILL,
    RSTAGE_WARM,
    RSTAGE_SOAK_DRY,
    RSTAGE_FALL,
    RSTAGE_DRINK,
    RSTAGE_ROOT,
    RSTAGE_GROW,
    RSTAGE_SPROUT,
    RSTAGE_BUD,
    RSTAGE_END,
    RSTAGE_COUNT
};

/* What a burning cell is charged before it looks at a single neighbour. Every
 * field is a property of the MATERIAL and the step's overrides, never of the
 * cell, so a screen of one material re-derived them all per cell out of two
 * flash tables. Rebuilt once a step beside the board-wide facts one of them
 * folds in, so it can never be staler than those.
 *
 * `stage` rides along because the dispatch loop indexes a per-material table
 * to pick a stage anyway: one index answers both. */
#define BURN_LIT      (1u << 0) /* the reaction burns at its own rate; the material's decay stays 0 */
#define BURN_SMOTHERS (1u << 1) /* smothered() could find something - the whole gate, board fact included */
#define BURN_LAVA     (1u << 2) /* a liquid that quenches: the burst roll and the cool-off chain are its alone */

typedef struct {
    uint8_t stage;
    uint8_t tick_rate;
    uint8_t flare;
    uint8_t flags;
} burn_plan_t;

/* A power-of-two stride keeps the dispatch loop's index a shift; the byte
 * array of stages this replaced indexed for free, and a multiply would hand
 * that saving straight back. */
_Static_assert(sizeof(burn_plan_t) == 4, "burn_plan_t must stay four bytes");

/* Declared rather than left static so a suite can check the shipped plan
 * against the material and reaction rows it is derived from - every field
 * feeds a skip or a rate, so a wrong row loses behaviour silently, and the
 * eleven fingerprint scenes cannot reach all thirty-two of them.
 *
 * sand_smothering_ceiling(): the board fact BURN_SMOTHERS folds in, exposed
 * for the same reason. Both read what the last sand_step_reactions() built. */
const burn_plan_t* sand_burn_plan_of(cell_t c);
uint8_t sand_smothering_ceiling(void);

/* `is_acid_rain_material` gates material-specific stage, not reaction type. */
static inline uint8_t
reaction_first_stage(const reaction_t* r, bool is_acid_rain_material) {
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
    /* BEFORE chills, because stage_chill ends in `continue` and never falls
     * through - snow chills, so a crust stage after it would be unreachable
     * for the one material that has it. */
    if (r->crusts != 0) {
        return RSTAGE_CRUST;
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
