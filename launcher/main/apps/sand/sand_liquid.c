/*=============================================================================
 * sand_liquid - everything about a liquid that is not the powder sweep.
 *
 * sand_step(), in sand.c, moves every grain the same way whatever it is made
 * of: try to fall, then try the two slides. A liquid needs that exact
 * treatment too - it is still gravity-ward, still bound by the sweep's
 * no-double-move guarantee - so move_liquid_grain() is called FROM inside
 * that sweep rather than living here as a separate pass.
 *
 * Everything else in a liquid's behaviour is NOT gravity-ward, and so cannot
 * safely live in that sweep at all - see the comment above equalise_liquids()
 * for why. sand_step_liquids() is the one thing sand_step() calls after its
 * sweep finishes: cross-flow levelling, and the wall-rebound splash on top of
 * it.
 *===========================================================================*/

#include "sand_priv.h"

/* Set by equalise_liquids() on a row it has scanned and found no liquid in,
 * so it need not walk that row again.
 *
 * Safe because it is cleared by ANY change to the row - mark_rows() wipes the
 * whole byte through wake_span(), and every write to a cell goes through
 * mark_rows(). So the flag can only ever be stale in the harmless direction:
 * a row that has just gained liquid has already had it cleared.
 *
 * 0x4, not 0x1 or 0x2: sand.c's own ROW_SETTLED_NEAREST and ROW_SETTLED_OTHER
 * live in the same byte, one per file, and must not collide. */
#define ROW_NO_LIQUID 0x4

/* Which materials are liquid, as a bitmask over the nibble.
 *
 * Both passes below look at every cell in the grid, and asking
 * materials[id].kind is a read from a table in flash - a likely cache miss,
 * per cell, per step. Measured, that alone put a screen of motionless SAND
 * from 17 us to five and a half milliseconds. Sixteen bits in a register
 * answers the same question for nothing. */
static uint16_t liquid_mask(void)
{
    uint16_t mask = 0;
    for (int m = 0; m < MATERIAL_MAX; m++) {
        if (materials[m].kind == KIND_LIQUID) {
            mask |= (uint16_t)(1u << m);
        }
    }
    return mask;
}

/* Add `amount` of `id` to a cell that is either empty or already that same
 * material. Mass is only ever moved, never made: every caller subtracts the
 * same figure from somewhere else in the same breath. */
static inline void pour_into(cell_t *dst, uint8_t id, int amount)
{
    const int had = CELL_IS_EMPTY(*dst) ? 0 : CELL_VARIANT(*dst);

    *dst = CELL_MAKE(id, had + amount);
}

/* How much of `id` a cell will accept, 0 if it holds something else. */
static inline int room_in(cell_t c, uint8_t id)
{
    if (CELL_IS_EMPTY(c)) {
        return MASS_MAX;
    }
    if (CELL_MATERIAL(c) != id) {
        return 0;
    }
    return MASS_MAX - CELL_VARIANT(c);
}

/*---------------------------------------------------------------------------
 * The one piece of liquid movement inside the main sweep.
 *-------------------------------------------------------------------------*/

/* Liquids: move an AMOUNT between touching cells.
 *
 * Nothing here looks further than one cell in any direction, and that is the
 * whole point. A cell that is either full or empty cannot split, so a full
 * one beside an empty one has no legal move and a wide pool sets into a
 * staircase - a real fixed point of any local rule, which is why
 * equalise_liquids() below has to go hunting further than one cell for
 * somewhere lower. Carrying an amount instead, the same pair becomes 15 and
 * 0, settles to 8 and 7, and the difference spreads outward a neighbour at a
 * time until the surface is level.
 *
 * Two rules, in order: fill the cell below, then share what is left with the
 * one beside it. Called from sand_step()'s own sweep - not a pass of its
 * own - because both of those are gravity-ward, and only the main sweep's
 * order guarantees a destination has not been visited yet. */
bool move_liquid_grain(sand_t *s, uint8_t *row, uint8_t *prow,
                       int x, int y, int dx, int dy,
                       const int *slide_a, const int *slide_b,
                       cell_t grain, uint8_t mat_id)
{
    const int w = s->w;
    int mass = CELL_VARIANT(grain);
    bool moved = false;

    /* DOWN first, so a liquid falls before it spreads. */
    const int bx = x + dx;
    if (prow != NULL && (unsigned)bx < (unsigned)w) {
        const int room = room_in(prow[bx], mat_id);
        const int give = mass < room ? mass : room;
        if (give > 0) {
            pour_into(&prow[bx], mat_id, give);
            mass -= give;
            mark_rows(s, y, y + dy);
            moved = true;
        }
    }

    /* Then DOWN THE SLOPE, both ways.
     *
     * Still only immediate neighbours, and still gravity-ward, so they carry
     * the same guarantee the fall does. Leaving these out was a real
     * mistake: without them water on a slope can only shuffle sideways and
     * then fall, so a mound collapses at the speed of diffusion - a dome was
     * still 7 cells proud after five thousand steps. Running down the slope
     * directly is what makes it settle in a moment instead. */
    for (int d = 0; d < 2 && mass > 0; d++) {
        const int *slide = (d == 0) ? slide_a : slide_b;
        uint8_t *srow = dest_row(s, y + slide[1]);
        const int sx = x + slide[0];

        if (srow == NULL || (unsigned)sx >= (unsigned)w) {
            continue;
        }
        const int room = room_in(srow[sx], mat_id);
        const int give = mass < room ? mass : room;
        if (give > 0) {
            pour_into(&srow[sx], mat_id, give);
            mass -= give;
            mark_rows(s, y, y + slide[1]);
            moved = true;
        }
    }

    /* Nothing left means no cell left. Written as CELL_EMPTY rather than a
     * zero variant, which would leave the material nibble claiming an
     * occupied cell holding nothing. */
    row[x] = (mass > 0) ? CELL_MAKE(mat_id, mass) : CELL_EMPTY;
    return moved;
}

/*---------------------------------------------------------------------------
 * Everything that is NOT gravity-ward, and so cannot live in that sweep.
 *-------------------------------------------------------------------------*/

/* Liquid cross-flow, as a second sweep with its own direction.
 *
 * WHY IT CANNOT LIVE IN THE MAIN LOOP
 *
 * Every move in the main sweep is gravity-ward, so sweeping against gravity
 * guarantees a cell's destination has already been visited and nothing can be
 * moved twice. Once gravity is tilted that pins the sweep on BOTH axes - and
 * then, of the two directions across the flow, only the one the sweep has
 * already passed is ever safe to use.
 *
 * Water could therefore cross a slope one way and never back. A tilted pool
 * did not level at all: it walked into the low corner and set there in steps.
 * Measured, a 45-degree pool sat at a spread of 525 units and had not improved
 * three thousand steps later. Tilting the board back releases it, which reads
 * convincingly like stored momentum and is nothing of the sort - it is the
 * gravity direction changing, and with it which way water is allowed to move.
 *
 * A separate pass has its own order, so it can be swept whichever way suits
 * the direction it is using, and that direction alternates every step. It only
 * ever moves liquid across the flow, so it cannot disturb anything the first
 * pass concluded.
 */
static void equalise_liquids(sand_t *s, const int *perp, int sight,
                             int dx, int dy)
{
    bool found_any = false;

    const int px = perp[0];
    const int py = perp[1];
    const int w  = s->w;
    const int h  = s->h;

    const uint16_t is_liquid = liquid_mask();

    /* Swept so that whatever is being given to has already been visited. */
    const int y_from = (py > 0) ? h - 1 : 0;
    const int y_to   = (py > 0) ? -1    : h;
    const int y_step = (py > 0) ? -1    : 1;

    const int x_from = (px > 0) ? w - 1 : 0;
    const int x_to   = (px > 0) ? -1    : w;
    const int x_step = (px > 0) ? -1    : 1;

    for (int y = y_from; y != y_to; y += y_step) {
        /* Known dry, and nothing has touched it since. */
        if (s->row_state != NULL && (s->row_state[y] & ROW_NO_LIQUID)) {
            continue;
        }

        uint8_t *row = s->cells + (size_t)y * (size_t)w;
        bool any_liquid = false;
        bool touched = false;

        for (int x = x_from; x != x_to; x += x_step) {
            const cell_t c = row[x];
            if (CELL_IS_EMPTY(c)) {
                continue;
            }

            const uint8_t id = CELL_MATERIAL(c);
            if (((is_liquid >> id) & 1u) == 0) {
                continue;
            }
            any_liquid = true;
            found_any  = true;

            const int mass = CELL_VARIANT(c);

            /* Falling water does not spread. If there is somewhere to fall,
             * it will go there and nothing needs deciding here.
             *
             * One comparison, and it is what makes an avalanche affordable:
             * while a body of water is collapsing almost every cell has room
             * beneath it, so almost every cell leaves at this line instead of
             * searching along a surface it does not yet have. */
            {
                const int fx = x + dx;
                const int fy = y + dy;
                if ((unsigned)fx < (unsigned)w && (unsigned)fy < (unsigned)h) {
                    const cell_t below = s->cells[(size_t)fy * w + fx];
                    if (CELL_IS_EMPTY(below) ||
                        (CELL_MATERIAL(below) == id &&
                         CELL_VARIANT(below) < MASS_MAX)) {
                        continue;
                    }
                }
            }

            /* Nothing to give unless the cell right beside it is lower.
             *
             * This is what keeps the search off the bill. Inside a body of
             * water every neighbour holds the same amount, so the overwhelming
             * majority of cells answer in one comparison instead of walking
             * thirty-two. Only the cells along an imbalance look further - and
             * they are the only ones with anywhere to send anything.
             *
             * Nothing is lost by it. A flat stretch of water needs no flow;
             * where it ends, the cells there do the work, and the dip they
             * leave behind draws the next ones after it. */
            {
                const int nx = x + px;
                const int ny = y + py;

                if ((unsigned)nx >= (unsigned)w ||
                    (unsigned)ny >= (unsigned)h) {
                    continue;
                }
                const cell_t n = s->cells[(size_t)ny * w + nx];
                if (!CELL_IS_EMPTY(n)) {
                    if (CELL_MATERIAL(n) != id ||
                        CELL_VARIANT(n) >= mass) {
                        continue;
                    }
                }
            }

            int lowest = mass;
            int at = 0;

            /* The shallowest place it can reach. Flow stops at anything that
             * is not this same liquid, so it cannot reach through a wall. */
            for (int k = 1; k <= sight; k++) {
                const int sx = x + px * k;
                const int sy = y + py * k;

                if ((unsigned)sx >= (unsigned)w ||
                    (unsigned)sy >= (unsigned)h) {
                    break;
                }
                const cell_t o = s->cells[(size_t)sy * w + sx];
                int there;

                if (CELL_IS_EMPTY(o)) {
                    there = 0;
                } else if (CELL_MATERIAL(o) == id) {
                    there = CELL_VARIANT(o);
                } else {
                    break;
                }

                if (there < lowest) {
                    lowest = there;
                    at = k;
                    if (there == 0) {
                        break;      /* nothing is lower than dry */
                    }
                }
            }

            /* Half the difference. Handing over everything would only move the
             * imbalance rather than settle it - the two would trade places for
             * ever. */
            const int give = (mass - lowest) / 2;
            if (at > 0 && give > 0) {
                const int tx = x + px * at;
                const int ty = y + py * at;

                pour_into(&s->cells[(size_t)ty * w + tx], id, give);
                row[x] = (mass - give > 0) ? CELL_MAKE(id, mass - give)
                                           : CELL_EMPTY;

                /* Marking is deferred when the flow stays inside one row,
                 * which is every orientation where gravity has no sideways
                 * component - much the commonest case. mark_rows() rewrites
                 * several bytes of row state, and doing that per transfer
                 * rather than per row was most of what this pass cost. */
                if (py == 0) {
                    touched = true;
                } else {
                    mark_rows(s, y, ty);
                }
            }
        }

        if (touched) {
            mark_rows(s, y, y);
        }

        /* Nothing liquid in the whole row - say so, and skip it next time.
         * Written after mark_rows may have cleared the byte, so a row that
         * received liquid during this very pass is not wrongly marked dry. */
        if (!any_liquid && s->row_state != NULL) {
            s->row_state[y] |= ROW_NO_LIQUID;
        }
    }

    /* Looked everywhere and found none, so stop looking until some is placed.
     * Only valid because a full sweep happened - rows skipped by ROW_NO_LIQUID
     * were themselves proved dry by an earlier sweep. */
    if (!found_any) {
        s->may_have_liquid = false;
    }
}

/* One wall's share of the rebound splash - see SAND_REBOUND_GAIN.
 *
 * `edge` is the column (for a left/right wall) or row (top/bottom) that
 * touches the wall; `count` is how many cells run along it; `to` is the
 * one-cell step into the grid, away from the wall. `push_q8` is how hard
 * momentum is currently driving INTO this particular wall - already resolved
 * to a single non-negative number by the caller, since each wall only cares
 * about one sign of one axis.
 *
 * Every source cell here is unique to this wall and every destination is one
 * fixed step inward, so no two transfers this call can ever touch the same
 * cell - unlike equalise_liquids, this needs no sweep order at all. */
static void rebound_wall(sand_t *s, int edge, int count, int to,
                         bool vertical, int push_q8, uint16_t is_liquid)
{
    if (push_q8 <= SAND_REBOUND_THRESHOLD) {
        return;
    }
    int kick = ((push_q8 - SAND_REBOUND_THRESHOLD) * SAND_REBOUND_GAIN) >> 8;
    if (kick > SAND_REBOUND_MAX) {
        kick = SAND_REBOUND_MAX;
    }
    if (kick <= 0) {
        return;
    }

    const int w = s->w;

    for (int i = 0; i < count; i++) {
        const int x = vertical ? edge : i;
        const int y = vertical ? i : edge;

        const size_t at = (size_t)y * (size_t)w + (size_t)x;
        const cell_t c = s->cells[at];
        if (CELL_IS_EMPTY(c)) {
            continue;
        }
        const uint8_t id = CELL_MATERIAL(c);
        if (((is_liquid >> id) & 1u) == 0) {
            continue;
        }

        const int nx = vertical ? x + to : x;
        const int ny = vertical ? y      : y + to;
        if ((unsigned)nx >= (unsigned)w || (unsigned)ny >= (unsigned)s->h) {
            continue;
        }

        const int mass = CELL_VARIANT(c);
        const size_t nat = (size_t)ny * (size_t)w + (size_t)nx;
        const int room = room_in(s->cells[nat], id);
        const int give = kick < mass ? kick : mass;
        const int moved = give < room ? give : room;
        if (moved <= 0) {
            continue;
        }

        pour_into(&s->cells[nat], id, moved);
        s->cells[at] = (mass - moved > 0) ? CELL_MAKE(id, mass - moved)
                                          : CELL_EMPTY;
        mark_rows(s, y, ny);
    }
}

void sand_step_liquids(sand_t *s, const int *perp_a, const int *perp_b,
                       int dx, int dy)
{
    if (!s->may_have_liquid) {
        return;
    }

    /* Cross-flow, in its own pass and alternating which way each step - so a
     * tilted pool levels both ways instead of walking into one corner. See
     * equalise_liquids(). */
    equalise_liquids(s, s->liquid_flip ? perp_a : perp_b,
                     SAND_LIQUID_SIGHT, dx, dy);
    s->liquid_flip = !s->liquid_flip;

    /* And let a hard flick splash liquid back off whichever wall it just
     * turned into. Four walls, but each is a no-op below its own threshold,
     * so this costs nothing on the vastly more common calm step - see
     * SAND_REBOUND_GAIN. */
    if (SAND_REBOUND_GAIN != 0) {
        const uint16_t is_liquid = liquid_mask();

        rebound_wall(s, 0,        s->h, 1,  true,
                    (int)-s->mom_x_q8, is_liquid);   /* left wall   */
        rebound_wall(s, s->w - 1, s->h, -1, true,
                    (int) s->mom_x_q8, is_liquid);   /* right wall  */
        rebound_wall(s, 0,        s->w, 1,  false,
                    (int)-s->mom_y_q8, is_liquid);   /* top wall    */
        rebound_wall(s, s->h - 1, s->w, -1, false,
                    (int) s->mom_y_q8, is_liquid);   /* bottom wall */
    }
}

/*---------------------------------------------------------------------------
 * Momentum accessors - see SAND_REBOUND_GAIN.
 *-------------------------------------------------------------------------*/

void sand_momentum(const sand_t *s, int32_t *mx_q8, int32_t *my_q8)
{
    *mx_q8 = s->mom_x_q8;
    *my_q8 = s->mom_y_q8;
}

void sand_set_flick(sand_t *s, int flick)
{
    s->flick = flick < 0 ? 0 : (flick > 255 ? 255 : flick);
}
