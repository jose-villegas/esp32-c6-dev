/*
 * sand_plants - tree, root and leaf growth: from a bare seed cell to a
 * branching, thickening trunk with a canopy, fed by soil moisture a root
 * system draws down through itself.
 *
 * Shares almost no call graph with sand_reactions.c's fire chemistry -
 * nothing here reads pair_bits[]/PAIR_*, calls try_ignite_given(),
 * conduct_heat() or cool_off_chain(), and nothing over there calls a
 * grow/root/sprout/bud function. Both are reaction_t-driven per-cell
 * passes dispatched by the same step_one_reacting_row()
 * (sand_reactions.c); the handful of helpers genuinely needed by BOTH
 * halves (place_cell()/place_reacted(), reaction_dirs, pay_quench_cost(),
 * soil_set_moisture()) live in sand_priv.h instead, the same way sand.c
 * and sand_liquid.c already share what a gravity-ward move and a
 * cross-flow pass both need.
 *
 * A stem walk (stem_next()/find_water()) carries every grower's question -
 * is there water, is there room - down through however much of the plant
 * sits between a leaf and the ground. step_one_growing_cell() is the one
 * that actually shapes a tree: height, lean, branch or thicken, then
 * harden a mature run into wood with a canopy on top.
 */

#include "reaction_doc.h"
#include "sand_priv.h"

/* FALLS in cold pass, not sweep. Gravity-ward, checks space. Determines (ax,
 * ay) type. */
static inline bool
is_kin(cell_t a, cell_t self, const reaction_t* r) {
    return a == self || (r->clings_to != 0 && CELL_MATERIAL(a) == r->clings_to);
}

/* Leaf-to-roots mimic, efficient on crowded boards. Larger bodies shed outer
 * cells. */
#define SUPPORT_MAX 48

static bool
anchored(sand_t* s, int x, int y, int w, int h, cell_t self, const reaction_t* r) {
    uint16_t body[SUPPORT_MAX];
    int n = 0, head = 0;

    body[n++] = (uint16_t)((size_t)y * (size_t)w + (size_t)x);

    const int down = ring_of(s->last_load_dx, s->last_load_dy);

    while (head < n) {
        const int at = (int)body[head++];
        const int cx = at % w, cy = at / w;

        for (int d = 0; d < 8; d++) {
            const int* nd = ring_dir(down + d);
            const int nx = cx + nd[0], ny = cy + nd[1];
            if ((unsigned)nx >= (unsigned)w || (unsigned)ny >= (unsigned)h) {
                continue;
            }
            const size_t nat = (size_t)ny * (size_t)w + (size_t)nx;
            const cell_t c = s->cells[nat];
            if (CELL_IS_EMPTY(c)) {
                continue;
            }
            if (!is_kin(c, self, r)) {
                /* Diagonals not counted; wall sticks in narrow shafts. */
                if (d == 0) {
                    return true; /* this body is resting on something */
                }
                continue;
            }
            if (n >= SUPPORT_MAX) {
                continue; /* too big to finish; treat as loose */
            }
            bool known = false;
            for (int i = 0; i < n && !known; i++) {
                known = (body[i] == (uint16_t)nat);
            }
            if (!known) {
                body[n++] = (uint16_t)nat;
            }
        }
    }
    return false;
}

bool
step_one_falling_cell(sand_t* s, int x, int y, int w, int h, const reaction_t* r) {
    const int nx = x + s->last_load_dx;
    const int ny = y + s->last_load_dy;
    if ((unsigned)nx >= (unsigned)w || (unsigned)ny >= (unsigned)h) {
        return false;
    }
    const size_t at = (size_t)y * (size_t)w + (size_t)x;
    const size_t nat = (size_t)ny * (size_t)w + (size_t)nx;
    if (!CELL_IS_EMPTY(s->cells[nat])) {
        return false; /* landed */
    }
    if (anchored(s, x, y, w, h, s->cells[at], r)) {
        return false;
    }
    if ((int)(rng_next(&s->rng) & 0xFF) >= r->falls) {
        return true; /* still falling, just not now */
    }

    s->cells[nat] = s->cells[at];
    s->cells[at] = SAND_EMPTY;
    mark_rows(s, y, y);
    mark_rows(s, ny, ny);
    wake_block_and_neighbors(s, x, y);
    wake_block_and_neighbors(s, nx, ny);
    return true;
}

/* Growth stops when soil dries. Bound to prevent unnecessary searches. */
#define GROW_REACH  48

/* Three for crown, small enough for shaping. */
#define CANOPY_SPAN 3

/* See step_one_budding_cell() - water bounds bud. */
#define BUD_COST    3

/* Root depth balance: avoids board starvation, ensures survival. */
#define ROOT_REACH  6

/* Plant stems restrict water flow, capping tree height and branch spread,
 * guided by water paths or moisture availability. */
#define TREE_LIFT   10

/* Thickening turns sapling into trunk; cells nearest ground have unlimited
 * lift. */
#define TRUNK_WIDTH 3

/* Moisture soaks; bottom wet, top dry. Plant paused, watered two rows below. */

static int
find_water(sand_t* s, int x, int y, int w, int h, const reaction_t* r, cell_t self, int* lift, int* contact_at,
           int* root_depth, bool wants_room) {
    const int dx = s->last_load_dx, dy = s->last_load_dy;
    const int down = ring_of(dx, dy);

    *contact_at = -1;

    int cx = x, cy = y;
    int lift_count = 0;
    int roots_passed = 0;
    for (int step = 0; step < GROW_REACH; step++) {
        /* `lift_count` tracks STEM transitions; `step` reuses fails, counting
         * roots incorrectly. */
        *lift = lift_count;
        *root_depth = roots_passed;
        int nx = -1, ny = -1;
        bool on_soil = false;
        bool via_root = false;
        /* COMMITTED to root. Not `nx >= 0`. Testing nx stops scan on
         * fallback, missing ground. See lookahead. */
        bool took_root = false;

        /* ORDERING is critical; incorrect ordering led to single-row roots. */

        /* Fallback crosses root; contact drops row. Not reliable. */

        /* One cell lookahead fixes bed shifting. */
        if (r->roots_to != 0) {
            const int* fd = ring_dir(down);
            const int tx = cx + fd[0], ty = cy + fd[1];
            if ((unsigned)tx < (unsigned)w && (unsigned)ty < (unsigned)h
                && s->cells[(size_t)ty * (size_t)w + (size_t)tx] == (cell_t)r->roots_to) {
                const int ax = tx + dx, ay = ty + dy;
                if ((unsigned)ax < (unsigned)w && (unsigned)ay < (unsigned)h) {
                    const cell_t under = s->cells[(size_t)ay * (size_t)w + (size_t)ax];
                    if (!CELL_IS_EMPTY(under) && (reaction_of(under)->soil != 0 || under == (cell_t)r->roots_to)) {
                        nx = tx;
                        ny = ty;
                        via_root = true;
                        took_root = true;
                    }
                }
            }
        }

        for (int i = 0; !took_root && i < 3; i++) {
            const int* fd = ring_dir(down + (i == 0 ? 0 : i == 1 ? 1 : 7));
            const int tx = cx + fd[0], ty = cy + fd[1];
            if ((unsigned)tx >= (unsigned)w || (unsigned)ty >= (unsigned)h) {
                continue;
            }
            const cell_t c = s->cells[(size_t)ty * (size_t)w + (size_t)tx];
            if (CELL_IS_EMPTY(c)) {
                continue;
            }
            if (reaction_of(c)->soil != 0) {
                nx = tx;
                ny = ty;
                on_soil = true;
                break; /* ground: stop looking for stem */
            }
            if (nx < 0 && (c == self || (r->clings_to != 0 && CELL_MATERIAL(c) == r->clings_to))) {
                nx = tx;
                ny = ty; /* more stem, keep it as a fallback */
            } else if (nx < 0 && r->roots_to != 0 && c == (cell_t)r->roots_to) {
                /* ROOT counts as stem. Fixes bug where stem finds neither
                 * stem nor ground. */
                nx = tx;
                ny = ty;
                via_root = true;
            }
        }
        if (nx < 0) {
            return -1; /* neither stem nor ground below */
        }
        if (!on_soil) {
            if (via_root) {
                roots_passed++; /* below the water line - see this
                                  * function's own top comment */
            } else {
                lift_count++;
            }
            cx = nx;
            cy = ny; /* carry on down the stem */
            continue;
        }

        /* Into the soil. This is the collar. */
        cx = nx;
        cy = ny;
        *contact_at = (int)((size_t)cy * (size_t)w + (size_t)cx);
        for (int depth = 0; depth < ROOT_REACH; depth++) {
            if ((unsigned)cx >= (unsigned)w || (unsigned)cy >= (unsigned)h) {
                return -1;
            }
            const size_t at = (size_t)cy * (size_t)w + (size_t)cx;
            const cell_t c = s->cells[at];
            /* Root must be TRANSPARENT to avoid cutting off water. */
            if (r->roots_to != 0 && c == (cell_t)r->roots_to) {
                cx += dx;
                cy += dy;
                continue;
            }
            if (CELL_IS_EMPTY(c) || reaction_of(c)->soil == 0) {
                return -1;
            }
            /* growth seeks nutrient-rich soil, drinking seeks room to expand */
            {
                const reaction_t* cr = reaction_of(c);
                /* Lit cell never has room or water. */
                if (!cell_is_burning(c)
                    && (wants_room ? moisture_of(c, cr) < cr->moist_max : moisture_of(c, cr) != 0)) {
                    return (int)at;
                }
            }
            cx += dx;
            cy += dy;
        }
        return -1;
    }
    return -1;
}

/* See PART 1 of roots feature. Rolls CONTACT cell welding into FIRST root. */

/* `root_depth == 0` means tree not embedded, see reaction_t.roots in
 * material.h. */

/* PART 2 - step_one_rooting_cell() handles growth starting from the root. */

/* Gating shuts path to prevent root conflicts. */

/* soil_at skips conversion to avoid disconnected woody specks. */

static void
spend_soil_moisture(sand_t* s, int w, const reaction_t* r, int soil_at, uint8_t amount, int contact_at,
                    int root_depth) {
    const cell_t soil = s->cells[soil_at];
    s->cells[soil_at] = soil_set_moisture(soil, (uint8_t)(moisture_of(soil, reaction_of(soil)) - amount), 0);
    mark_rows(s, soil_at / w, soil_at / w);

    if (r->roots == 0 || contact_at < 0 || root_depth != 0) {
        return;
    }
    if ((int)(rng_next(&s->rng) & 0xFF) >= r->roots) {
        return;
    }
    place_reacted(s, contact_at % w, contact_at / w, (size_t)contact_at, r->roots_to);
}

/* Max neighbours; 2 ensures filamentary shape, not slab.
 * Docs/Sand/Sand-Simulation.md */
#define ROOT_SURFACE_MAX    2

/* DOWN/AWAY=2. Trunk=parent. Roots stop at dry soil. Weights guide moist cell
 * selection. */
#define ROOT_WEIGHT_AWAY    2
#define ROOT_WEIGHT_DOWN    2

#define ROOT_CONDUCT_CHANCE 64

/* One cell of ROOT, carrying water DOWN through itself - a conduit. */

/* Moves only, gravity-ward. */

/* Sides count as sources. Drawing from soil allows full column drainage. */

bool
step_one_conducting_cell(sand_t* s, int x, int y, int w, int h, const reaction_t* r) {
    const int down = ring_of(s->last_load_dx, s->last_load_dy);
    (void)r;

    int src_at = -1, src_x = 0, src_y = 0, src_m = 0;
    int dst_at = -1, dst_x = 0, dst_y = 0, dst_m = 0;

    for (int k = 0; k < 8; k++) {
        const int* nd = ring_dir(down + k);
        const int nx = x + nd[0], ny = y + nd[1];
        if ((unsigned)nx >= (unsigned)w || (unsigned)ny >= (unsigned)h) {
            continue;
        }
        const size_t nat = (size_t)ny * (size_t)w + (size_t)nx;
        const cell_t c = s->cells[nat];
        if (CELL_IS_EMPTY(c)) {
            continue;
        }
        const reaction_t* cr = reaction_of(c);
        if (cr->soil == 0) {
            continue; /* not soil: root, wood, stone, air - and, since D1,
                        * gunpowder: a fuse is not ground a root conducts
                        * water through. */
        }
        const int m = moisture_of(c, cr);
        if (k == 0 || k == 1 || k == 7) {
            /* Keeps codec soil below moist_max. Ensures !cell_is_burning(c)
             * for accurate moisture readings. */
            if (m < cr->moist_max && !cell_is_burning(c) && (dst_at < 0 || m < dst_m)) {
                dst_m = m;
                dst_at = (int)nat;
                dst_x = nx;
                dst_y = ny;
            }
        } else if (m > src_m) {
            /* Beside or above: a source, if it holds anything. */
            src_m = m;
            src_at = (int)nat;
            src_x = nx;
            src_y = ny;
        }
    }
    if (src_at < 0 || dst_at < 0) {
        return false; /* nothing to carry, or nowhere to carry it */
    }
    if ((int)(rng_next(&s->rng) & 0xFF) >= ROOT_CONDUCT_CHANCE) {
        return true;
    }
    const cell_t src = s->cells[src_at], dst = s->cells[dst_at];
    s->cells[src_at] = soil_set_moisture(src, (uint8_t)(src_m - 1), (uint8_t)(dst_m + 1));
    s->cells[dst_at] = with_moisture(dst, (uint8_t)(dst_m + 1), reaction_of(dst));
    mark_rows(s, src_y, src_y);
    mark_rows(s, dst_y, dst_y);
    wake_block_and_neighbors(s, src_x, src_y);
    wake_block_and_neighbors(s, dst_x, dst_y);
    return true;
}

/* ROOT cell consumes soil moisture with chance, docs/sand/Sand-Simulation.md */

/* Root does not need stem's machinery. Uses existing resource bound. */

/* Prevents system becoming a block. Uses standard roll discipline. */
bool
step_one_rooting_cell(sand_t* s, int x, int y, int w, int h, const reaction_t* r) {
    /* Cheapest question first, rejects thick columns without neighbour scan
     * touching RNG. */
    int root_neighbors = 0;
    for (int d = 0; d < 8; d++) {
        const int* nd = ring_dir(d);
        const int nx = x + nd[0], ny = y + nd[1];
        if ((unsigned)nx >= (unsigned)w || (unsigned)ny >= (unsigned)h) {
            continue;
        }
        if (s->cells[(size_t)ny * (size_t)w + (size_t)nx] == (cell_t)r->roots_to) {
            root_neighbors++;
        }
    }
    if (root_neighbors > ROOT_SURFACE_MAX) {
        return false; /* buried inside its own kind; nothing to do here */
    }

    /* Qualitative difference, not rounding error. Uses ring_dir() instead of
     * four-neighbour scan. */

    /* WEIGHTED PICK, not uniform. Moisture spreads sideways, favouring sides
     * over depth. */

    /* GRAVITY-WARD biases depth: root seeks water beneath, not beside. */

    /* TRUNK IS PARENT. First root has no neighbors, zero away-vector, gravity
     * dominant, critical for heading. */

    int away_x = 0, away_y = 0;
    for (int d = 0; d < 8; d++) {
        const int* nd = ring_dir(d);
        const int nx = x + nd[0], ny = y + nd[1];
        if ((unsigned)nx >= (unsigned)w || (unsigned)ny >= (unsigned)h) {
            continue;
        }
        const cell_t c = s->cells[(size_t)ny * (size_t)w + (size_t)nx];
        if (c == (cell_t)r->roots_to || (!CELL_IS_EMPTY(c) && CELL_MATERIAL(c) == r->clings_to)) {
            away_x -= nd[0];
            away_y -= nd[1];
        }
    }
    const int gx = s->last_load_dx, gy = s->last_load_dy;

    int cand_at[8], cand_x[8], cand_y[8], cand_w[8];
    int n_cand = 0, total_w = 0;
    for (int d = 0; d < 8; d++) {
        const int* nd = ring_dir(d);
        const int nx = x + nd[0], ny = y + nd[1];
        if ((unsigned)nx >= (unsigned)w || (unsigned)ny >= (unsigned)h) {
            continue;
        }
        const size_t nat = (size_t)ny * (size_t)w + (size_t)nx;
        const cell_t n = s->cells[nat];
        if (CELL_IS_EMPTY(n) || reaction_of(n)->soil == 0 || moisture_of(n, reaction_of(n)) == 0) {
            continue;
        }
        int wgt = 1;
        if (nd[0] * away_x + nd[1] * away_y > 0) {
            wgt += ROOT_WEIGHT_AWAY; /* carries on away from its parent */
        }
        if (nd[0] * gx + nd[1] * gy > 0) {
            wgt += ROOT_WEIGHT_DOWN; /* reaches down */
        }
        cand_at[n_cand] = (int)nat;
        cand_x[n_cand] = nx;
        cand_y[n_cand] = ny;
        cand_w[n_cand] = wgt;
        total_w += wgt;
        n_cand++;
    }
    if (n_cand == 0) {
        return false; /* nothing moist beside it right now */
    }
    if ((int)(rng_next(&s->rng) & 0xFF) >= r->roots) {
        return true; /* a candidate exists; just not this roll */
    }
    int pick = rng_below(&s->rng, total_w);
    int k = 0;
    while (pick >= cand_w[k]) {
        pick -= cand_w[k];
        k++;
    }
    const int eat_at = cand_at[k], ex = cand_x[k], ey = cand_y[k];

    place_reacted(s, ex, ey, (size_t)eat_at, r->roots_to);
    return true;
}

/* DRINKS adds moisture. `KIND_STATIC` blocks water via foliage. Water moves
 * from stem to roots. */
bool
step_one_drinking_cell(sand_t* s, int x, int y, int w, int h, const reaction_t* r, cell_t self) {
    int lx = -1, ly = -1;
    for (int d = 0; d < 4; d++) {
        const int nx = x + reaction_dirs[d][0];
        const int ny = y + reaction_dirs[d][1];
        if ((unsigned)nx >= (unsigned)w || (unsigned)ny >= (unsigned)h) {
            continue;
        }
        const cell_t n = s->cells[(size_t)ny * (size_t)w + (size_t)nx];
        if (!CELL_IS_EMPTY(n) && material_of(n)->kind == KIND_LIQUID && reaction_of(n)->wets != 0) {
            lx = nx;
            ly = ny;
            break;
        }
    }
    if (lx < 0) {
        return false; /* nothing to drink */
    }

    int lift = 0, contact_at = -1, root_depth = 0; /* drinking never spends
                                                     * soil moisture, so
                                                     * nothing here roots -
                                                     * scratch values */
    const int soil_at = find_water(s, x, y, w, h, r, self, &lift, &contact_at, &root_depth, true);
    if (soil_at < 0) {
        return true; /* thirsty, but nowhere to put it */
    }
    if ((int)(rng_next(&s->rng) & 0xFF) >= r->drinks) {
        return true;
    }

    pay_quench_cost(s, lx, ly, w);

    const cell_t soil = s->cells[soil_at];
    const reaction_t* sr = reaction_of(soil);
    s->cells[soil_at] = with_moisture(soil, (uint8_t)(moisture_of(soil, sr) + 1), sr);
    mark_rows(s, soil_at / w, soil_at / w);
    wake_block_and_neighbors(s, soil_at % w, soil_at / w);
    return true;
}

/* SPROUTS: Consume soil, close loop, grow into wood, end trees, leave posts.
 * Trunks live. */
bool
step_one_sprouting_cell(sand_t* s, int x, int y, int w, int h, const reaction_t* r) {
    int soil_at = -1, empty_at = -1, ex = 0, ey = 0;

    for (int d = 0; d < 4; d++) {
        const int nx = x + reaction_dirs[d][0];
        const int ny = y + reaction_dirs[d][1];
        if ((unsigned)nx >= (unsigned)w || (unsigned)ny >= (unsigned)h) {
            continue;
        }
        const size_t nat = (size_t)ny * (size_t)w + (size_t)nx;
        const cell_t n = s->cells[nat];
        if (CELL_IS_EMPTY(n)) {
            if (empty_at < 0) {
                empty_at = (int)nat;
                ex = nx;
                ey = ny;
            }
            continue;
        }
        if (soil_at < 0 && reaction_of(n)->soil != 0 && moisture_of(n, reaction_of(n)) != 0) {
            soil_at = (int)nat;
        }
    }
    if (soil_at < 0 || empty_at < 0) {
        return soil_at >= 0;
    }
    if ((int)(rng_next(&s->rng) & 0xFF) >= r->sprouts) {
        return true;
    }

    place_reacted(s, ex, ey, (size_t)empty_at, r->sprouts_to);

    /* SPENDS, NEVER SEEDS -1. 0 reports collar, disconnected roots. Sprouting
     * pays for leaf. */
    spend_soil_moisture(s, w, r, soil_at, 1, -1, 0);
    return true;
}

bool
step_one_budding_cell(sand_t* s, int x, int y, int w, int h, const reaction_t* r) {
    const cell_t self = s->cells[(size_t)y * (size_t)w + (size_t)x];

    bool crowned = false;
    for (int d = 0; d < 8 && !crowned; d++) {
        const int* nd = ring_dir(d);
        const int nx = x + nd[0], ny = y + nd[1];
        if ((unsigned)nx >= (unsigned)w || (unsigned)ny >= (unsigned)h) {
            continue;
        }
        crowned = (s->cells[(size_t)ny * (size_t)w + (size_t)nx] == (cell_t)r->sprouts_to);
    }
    if (!crowned) {
        return false;
    }
    {
        const int ax = x - s->last_load_dx, ay = y - s->last_load_dy;
        if ((unsigned)ax < (unsigned)w && (unsigned)ay < (unsigned)h
            && s->cells[(size_t)ay * (size_t)w + (size_t)ax] == self) {
            return false;
        }
    }

    /* Somewhere to put it, up and away from gravity. */
    const int up_i = ring_of(-s->last_load_dx, -s->last_load_dy);
    static const int out[5] = {7, 0, 1, 2, 6};
    int at = -1, bx = 0, by = 0;
    for (int d = 0; d < 5; d++) {
        const int* nd = ring_dir(up_i + out[d]);
        const int nx = x + nd[0], ny = y + nd[1];
        if ((unsigned)nx >= (unsigned)w || (unsigned)ny >= (unsigned)h) {
            continue;
        }
        const size_t nat = (size_t)ny * (size_t)w + (size_t)nx;
        if (CELL_IS_EMPTY(s->cells[nat])) {
            at = (int)nat;
            bx = nx;
            by = ny;
            break;
        }
    }
    if (at < 0) {
        return true; /* crowned, but boxed in */
    }

    int lift = 0, contact_at = -1, root_depth = 0;
    const int soil_at = find_water(s, x, y, w, h, r, self, &lift, &contact_at, &root_depth, false);
    if (soil_at < 0) {
        return true; /* nothing to drink */
    }
    const cell_t soil = s->cells[soil_at];
    if (moisture_of(soil, reaction_of(soil)) < BUD_COST) {
        return true;
    }
    if ((int)(rng_next(&s->rng) & 0xFF) >= r->buds) {
        return true;
    }

    place_reacted(s, bx, by, (size_t)at, r->buds_to);

    spend_soil_moisture(s, w, r, soil_at, BUD_COST, contact_at, root_depth);
    return true;
}

/* Stems grow along DITHERED gravity, alternating steps. Prevents rigid
 * angles, makes trunk wander. */
static bool
stem_next(sand_t* s, int x, int y, int ux, int uy, int w, int h, cell_t self, int* ox, int* oy) {
    const int up = ring_of(ux, uy);
    for (int i = 0; i < 3; i++) {
        const int* d = ring_dir(up + (i == 0 ? 0 : i == 1 ? 1 : 7));
        const int nx = x + d[0], ny = y + d[1];
        if ((unsigned)nx >= (unsigned)w || (unsigned)ny >= (unsigned)h) {
            continue;
        }
        if (s->cells[(size_t)ny * (size_t)w + (size_t)nx] == self) {
            *ox = nx;
            *oy = ny;
            return true;
        }
    }
    return false;
}

/* Bound so plant under half board does not walk it */
#define PUSH_REACH 8

/* Shift cells (dx, dy) from (gx, gy), halting at STATICs. Checks if (gx, gy)
 * is free. */
static bool
shove_aside(sand_t* s, int gx, int gy, int dx, int dy, int w, int h) {
    int ex = gx, ey = gy;
    int run = 0;

    while (run < PUSH_REACH) {
        if ((unsigned)ex >= (unsigned)w || (unsigned)ey >= (unsigned)h) {
            return false; /* shoved into the wall */
        }
        const cell_t c = s->cells[(size_t)ey * (size_t)w + (size_t)ex];
        if (CELL_IS_EMPTY(c)) {
            break; /* somewhere to put it all */
        }
        if (material_of(c)->kind == KIND_STATIC) {
            return false; /* will not budge */
        }
        ex += dx;
        ey += dy;
        run++;
    }
    if (run == 0) {
        return true; /* was empty to begin with */
    }
    if (run >= PUSH_REACH) {
        return false; /* too much of it to lift */
    }

    /* Back to front, so nothing is overwritten before it has moved. */
    for (int i = 0; i < run; i++) {
        const int tx = ex, ty = ey;
        ex -= dx;
        ey -= dy;
        s->cells[(size_t)ty * (size_t)w + (size_t)tx] = s->cells[(size_t)ey * (size_t)w + (size_t)ex];
        mark_rows(s, ty, ty);
        wake_block_and_neighbors(s, tx, ty);
    }
    s->cells[(size_t)gy * (size_t)w + (size_t)gx] = SAND_EMPTY;
    mark_rows(s, gy, gy);
    wake_block_and_neighbors(s, gx, gy);
    return true;
}

bool
step_one_growing_cell(sand_t* s, int x, int y, int w, int h, const reaction_t* r) {
    const cell_t self = s->cells[(size_t)y * (size_t)w + (size_t)x];

    /* Prevent rigid stems using dithered sweep. */
    const int ux = -s->last_step_dx;
    const int uy = -s->last_step_dy;
    if (ux == 0 && uy == 0) {
        return true; /* free fall: no up to grow towards */
    }

    /* Reach aids, BURIED stagnate, growth peaks early, surface optimal, dense
     * skip scans. */
    int packed = 0;
    for (int d = 0; d < 8; d++) {
        const int* nd = ring_dir(d);
        const int nx = x + nd[0], ny = y + nd[1];
        if ((unsigned)nx >= (unsigned)w || (unsigned)ny >= (unsigned)h) {
            packed++;
            continue;
        }
        if (is_kin(s->cells[(size_t)ny * (size_t)w + (size_t)nx], self, r)) {
            packed++;
        }
    }
    if (packed >= 5) {
        return true; /* inside the crowd, not at its edge */
    }

    int lift = 0, contact_at = -1, root_depth = 0;
    const int soil_at = find_water(s, x, y, w, h, r, self, &lift, &contact_at, &root_depth, false);
    if (soil_at < 0) {
        return true; /* nothing to drink */
    }
    if (lift >= TREE_LIFT) {
        return true; /* too high up to be fed */
    }
    if ((int)(rng_next(&s->rng) & 0xFF) >= r->grows) {
        return true;
    }

    int run = 1, tx = x, ty = y;
    for (int i = 0; i < GROW_REACH; i++) {
        int nx, ny;
        if (!stem_next(s, tx, ty, ux, uy, w, h, self, &nx, &ny)) {
            break;
        }
        tx = nx;
        ty = ny;
        run++;
    }

    /* Round RING, not up-plus-perpendicular: one step round is adjacent. */
    const int up = ring_of(ux, uy);
    const int side = rng_below(&s->rng, 2) ? 1 : 7; /* +1 or -1 round */

    int site, dx, dy;
    bool thicken = false;
    const int what = rng_below(&s->rng, 8);
    if (what < 4 || run < 3) {
        site = run - 1; /* HEIGHT: straight on from the tip */
        dx = 0;
        dy = 0; /* along the run - filled in below */
    } else if (what < 6) {
        site = run - 1; /* LEAN: the tip, one step round */
        dx = side;
        dy = 0; /* one step round from the run */
    } else if (what < 7) {
        site = rng_below(&s->rng, run - 1); /* BRANCH: out and up */
        dx = side;
        dy = 0;
    } else {
        /* WIDTH simulates tree growth by thickening trunk. */
        site = rng_below(&s->rng, (run + 1) / 2);
        dx = side * 2; /* square on to the run */
        dy = 0;
        thicken = true;
    }
    /* Back up the stem to the chosen site, the same way. */
    int sx = x, sy = y;
    for (int i = 0; i < site; i++) {
        int nx, ny;
        if (!stem_next(s, sx, sy, ux, uy, w, h, self, &nx, &ny)) {
            break;
        }
        sx = nx;
        sy = ny;
    }

    /* Gravity triggers `run < 3`, `holds_line` restores old behavior. */
    int head = up;
    if (r->holds_line != 0 && (int)(rng_next(&s->rng) & 0xFF) < r->holds_line) {
        /* Longer baseline increases horizontal drift from 24 to 38. Shorter
         * baseline simpler. */
        int px, py;
        if (stem_next(s, sx, sy, -ux, -uy, w, h, self, &px, &py)) {
            head = ring_of(sx - px, sy - py);
        }
    }
    {
        const int* hd = ring_dir(head + dx);
        dx = hd[0];
        dy = hd[1];
    }

    if (thicken) {
        /* TAPERED: allowance shrinks with height, fat at foot, single cell by
         * branches. Uniform grows a pillar, not a tree. */
        const int allowed = TRUNK_WIDTH - (lift + site) / 3;
        if (allowed < 2) {
            return true; /* too high up to be thickening */
        }
        int wide = 0;
        for (int i = 1; i < allowed; i++) {
            const int wx = sx + dx * i;
            const int wy = sy + dy * i;
            if ((unsigned)wx >= (unsigned)w || (unsigned)wy >= (unsigned)h) {
                break;
            }
            const cell_t c = s->cells[(size_t)wy * (size_t)w + (size_t)wx];
            if (c != self && !(r->clings_to != 0 && CELL_MATERIAL(c) == r->clings_to)) {
                break;
            }
            wide++;
        }
        if (wide >= allowed - 1) {
            return true; /* thick enough already */
        }
    }

    const int gx = sx + dx, gy = sy + dy;
    if ((unsigned)gx >= (unsigned)w || (unsigned)gy >= (unsigned)h) {
        return true;
    }
    /* Growth limited to tip; produces trees mostly underground. */
    const bool shoot = (site == run - 1) && !thicken;

    const size_t gat = (size_t)gy * (size_t)w + (size_t)gx;
    if (!CELL_IS_EMPTY(s->cells[gat]) && !(shoot && shove_aside(s, gx, gy, dx, dy, w, h))) {
        return true; /* in the way, and will not move */
    }

    /* Grow, and spend the water. */
    s->cells[gat] = self;
    latch_content_flags(s, self);
    mark_rows(s, gy, gy);
    wake_block_and_neighbors(s, gx, gy);

    spend_soil_moisture(s, w, r, soil_at, 1, contact_at, root_depth);

    /* HARDENING. Counted from the bottom; run measured once regardless of
     * growth. */
    if (r->hardens_to == 0 || r->harden_run == 0) {
        return true;
    }
    int cx = x, cy = y;
    for (int i = 0; i < GROW_REACH; i++) {
        int nx, ny;
        if (!stem_next(s, cx, cy, -ux, -uy, w, h, self, &nx, &ny)) {
            break;
        }
        cx = nx;
        cy = ny;
    }
    const int fx = cx, fy = cy;

    int trunk = 1;
    while (trunk < GROW_REACH) {
        int nx, ny;
        if (!stem_next(s, cx, cy, ux, uy, w, h, self, &nx, &ny)) {
            break;
        }
        cx = nx;
        cy = ny;
        trunk++;
    }
    if (trunk < r->harden_run) {
        return true;
    }

    /* Runs harden before long, wood does not grow. */
    if (__builtin_expect((int)(rng_next(&s->rng) & 0xFF) >= r->harden_chance, 1)) {
        return true;
    }

    /* SHAPING PASS: Hardens, converts to wood, thickens trunk, adds canopy.
     * Last cell green. Growth ends. */
    const int up_i = ring_of(ux, uy);

    /* Growth now from crowned wood (reaction_t.buds). */
    const int hard = trunk;

    int topx[CANOPY_SPAN], topy[CANOPY_SPAN];
    int ntop = 0;

    cx = fx;
    cy = fy;
    for (int i = 0; i < hard; i++) {
        int nx = 0, ny = 0;
        const bool more = stem_next(s, cx, cy, ux, uy, w, h, self, &nx, &ny);
        place_cell(s, cx, cy, (size_t)cy * (size_t)w + (size_t)cx, CELL_MAKE(r->hardens_to, 0));

        /* Taper linear by LENGTH. Step is taper. Twelve hardenings merge
         * trees; 6-7 preferred. */
        const int span = (hard > 1) ? hard - 1 : 1;
        const int extra = (int)r->trunk_girth * (span - i) / span;
        for (int g = 1; g <= extra; g++) {
            const int sidei = (g & 1) ? 2 : 6; /* square on, both ways */
            const int* gd = ring_dir(up_i + sidei);
            const int gx = cx + gd[0] * ((g + 1) / 2);
            const int gy = cy + gd[1] * ((g + 1) / 2);
            if ((unsigned)gx >= (unsigned)w || (unsigned)gy >= (unsigned)h) {
                continue;
            }
            const size_t gat = (size_t)gy * (size_t)w + (size_t)gx;
            if (!CELL_IS_EMPTY(s->cells[gat])) {
                continue;
            }
            place_cell(s, gx, gy, gat, CELL_MAKE(r->hardens_to, 0));
        }

        /* Hang crown once cells below are wood, avoid foliage. stem_next()
         * still walking. */
        if (ntop < CANOPY_SPAN) {
            topx[ntop] = cx;
            topy[ntop] = cy;
            ntop++;
        } else {
            for (int k = 1; k < CANOPY_SPAN; k++) {
                topx[k - 1] = topx[k];
                topy[k - 1] = topy[k];
            }
            topx[CANOPY_SPAN - 1] = cx;
            topy[CANOPY_SPAN - 1] = cy;
        }

        if (!more) {
            break;
        }
        cx = nx;
        cy = ny;
    }

    if (r->canopy != 0 && r->canopy_to != 0) {
        static const int crown[4] = {7, 1, 2, 6};
        for (int t = 0; t < ntop; t++) {
            for (int c = 0; c < 4; c++) {
                const int* cd = ring_dir(up_i + crown[c]);
                const int lx = topx[t] + cd[0], ly = topy[t] + cd[1];
                if ((unsigned)lx >= (unsigned)w || (unsigned)ly >= (unsigned)h) {
                    continue;
                }
                const size_t lat = (size_t)ly * (size_t)w + (size_t)lx;
                if (!CELL_IS_EMPTY(s->cells[lat])) {
                    continue;
                }
                if ((int)(rng_next(&s->rng) & 0xFF) >= r->canopy) {
                    continue;
                }
                place_reacted(s, lx, ly, lat, r->canopy_to);
            }
        }
    }
    return true;
}
