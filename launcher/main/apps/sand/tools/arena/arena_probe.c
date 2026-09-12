/* Material-vs-material ratio arena for bd esp32c6-l5z.
 *
 * Pours material A to a target share of the board, lets it settle, then pours
 * material B over it and times every step OF THE POUR - the transient, which
 * is what feels slow, not the settled end state.
 *
 * Sweeping both shares maps a cost surface, and each suspect predicts a
 * different shape: scaling in A alone means per-cell work that B merely arms
 * board-wide; in B alone means B's own passes; in the product means genuine
 * interaction; in the total means the main sweep's walk.
 *
 * Poured, never drawn - a drawn block has no voids and a flat surface, and
 * the contact area between the two materials is what several suspects scale
 * with.
 *
 * Board is 184x224, the device perf scenes' size, so numbers relate to them.
 *
 * Usage: ratio_probe <matA> <pctA> <matB> <pctB>
 *        ratio_probe Sand 65 Water 10
 */
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "apps/sand/material.h"
#include "apps/sand/sand.h"

#define GW           184
#define GH           224
#define CELLS        (GW * GH)
#define SETTLE_STEPS 400
#define POUR_WINDOW  300
#define POUR_GUARD   20000
#define POUR_STALL   250

typedef struct {
    double mean_us;
    double worst_us;
    double settled_cells;
    double moving_cells;
} timing_t;

#define CENSUS_EVERY    10
#define PER_STEP_STAMPS 2

static double
now_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1e6 + (double)ts.tv_nsec / 1e3;
}

static int
count_of(const sand_t* g, cell_t want) {
    int n = 0;
    for (int y = 0; y < GH; y++) {
        for (int x = 0; x < GW; x++) {
            const cell_t c = sand_at(g, x, y);
            if (!CELL_IS_EMPTY(c) && material_name(c) == material_name(want)) {
                n++;
            }
        }
    }
    return n;
}

/* The variant nibble means different things per kind: mass for a liquid,
 * temperature for a static solid. */
static cell_t
spawn_variant_for(material_id_t id) {
    if (material_by_id(id)->kind == KIND_STATIC) {
        return CELL_MAKE(id, SAND_AMBIENT_HEAT);
    }
    return CELL_MAKE(id, 8);
}

/* Every material the grid can hold, including the extended nibble's own -
 * ice, plant, leaf, metal, root and gunpowder are materials to the player and
 * to the reaction table, and only an accident of encoding keeps them out of
 * materials[]. material_name() is the one identity that spans both halves. */
static int
all_spawn_cells(cell_t* out, int max) {
    int n = 0;
    for (int m = 1; m < MAT_COUNT && n < max; m++) {
        if (m == MAT_EXTENDED) {
            continue;
        }
        out[n++] = spawn_variant_for((material_id_t)m);
    }
    for (int k = 0; k < 8 && n < max; k++) {
        out[n++] = MATX(k);
    }
    if (n < max) {
        out[n++] = GUNPOWDER_CELL(0);
    }
    return n;
}

static cell_t
resolve_material(const char* name) {
    cell_t cells[MAT_COUNT + 16];
    const int n = all_spawn_cells(cells, (int)(sizeof cells / sizeof cells[0]));
    for (int i = 0; i < n; i++) {
        const char* nm = material_name(cells[i]);
        if (nm != NULL && _stricmp(nm, name) == 0) {
            return cells[i];
        }
    }
    return 0;
}

static void
list_materials(void) {
    cell_t cells[MAT_COUNT + 16];
    const int n = all_spawn_cells(cells, (int)(sizeof cells / sizeof cells[0]));
    for (int i = 0; i < n; i++) {
        const char* nm = material_name(cells[i]);
        printf("%s%s", (nm != NULL) ? nm : "?", (i + 1 < n) ? " " : "");
    }
    putchar(10);
}

/* Where a material has to ENTER the arena for the scene to be its own
 * physics rather than an accident of the spawn row. A powder or liquid is
 * poured from the top and falls; a gas released at the top is already at its
 * destination and disperses off the ceiling, so it enters at the floor and
 * rises. A static never moves at all - see place_static(). */
static int
entry_row_for(cell_t cell) {
    const material_t* m = material_of(cell);
    if (m->kind == KIND_GAS) {
        return GH - 2;
    }
    return 1;
}

static bool
is_static(cell_t cell) {
    return material_of(cell)->kind == KIND_STATIC;
}

/* THE POUR IS A DRAGGED BRUSH, not a scatter across the width. The app's own
 * pour is POUR_RADIUS_PX 10 at 2 px per cell, stamped through sand_spawn_cell
 * - the same call the brush makes - and a player drags it left to right and
 * back. Scattering single cells across the full width instead produced a
 * rain, which piles and wets quite differently from a moving column. */
#define POUR_RADIUS  5
#define SWEEP_MARGIN (POUR_RADIUS + 2)

static int
sweep_x(int step) {
    const int span = GW - 2 * SWEEP_MARGIN;
    const int cycle = step % (2 * span);
    return SWEEP_MARGIN + ((cycle < span) ? cycle : (2 * span - cycle - 1));
}

static int
stamp_column(sand_t* g, cell_t cell, int x, int from, int dir) {
    for (int probe = 0; probe < GH / 2; probe += POUR_RADIUS) {
        const int y = from + dir * probe;
        if (y < 1 || y > GH - 2) {
            return 0;
        }
        const int placed = sand_spawn_cell(g, x, y, POUR_RADIUS, cell);
        if (placed > 0) {
            return placed;
        }
    }
    return 0;
}

/* A player pours into whatever space there is: nobody holds the brush against
 * a packed ceiling and waits. A fixed entry row made a gas-filled arena
 * refuse every grain and report a pour of zero, which is a property of the
 * scene rather than of the game.
 *
 * The search is both ways because a player moves both ways - inward from the
 * entry row for headroom, and sideways when the column under the brush is
 * packed. Only a genuinely full arena places nothing. */
static int
pour_stamp(sand_t* g, cell_t cell, int step) {
    const int x0 = sweep_x(step);
    const int from = entry_row_for(cell);
    const int dir = (from < GH / 2) ? 1 : -1;

    for (int off = 0; off < GW / 2; off += POUR_RADIUS) {
        for (int side = 0; side < 2; side++) {
            const int x = (side == 0) ? x0 + off : x0 - off;
            if (x < SWEEP_MARGIN || x > GW - 1 - SWEEP_MARGIN) {
                continue;
            }
            const int placed = stamp_column(g, cell, x, from, dir);
            if (placed > 0) {
                return placed;
            }
            if (off == 0) {
                break;
            }
        }
    }
    return 0;
}

/* A static cannot be poured to a share, so it is BUILT to one: filled from
 * the floor up, which is the shape stone and glass actually take in play.
 * Pouring them was the first tournament's bug - Stone and Glass won the
 * fastest bracket at 0.3 us because only 176 cells of 16,486 ever existed. */
static int
place_static(sand_t* g, cell_t cell, int target) {
    int placed = 0;
    for (int y = GH - 2; y >= 1 && placed < target; y--) {
        for (int x = 0; x < GW && placed < target; x++) {
            if (CELL_IS_EMPTY(sand_at(g, x, y))) {
                sand_set(g, x, y, cell);
                placed++;
            }
        }
    }
    return placed;
}

/* The sim's own notion of rest, not a guess from velocity: a cell counts as
 * settled if its block does. That is the same test every settled-gated rule
 * in the simulation makes, so the ratio reported here is the one the skips
 * actually see. */
static void
census_mass(const sand_t* g, double* settled, double* moving) {
    for (int y = 0; y < GH; y++) {
        for (int x = 0; x < GW; x++) {
            if (CELL_IS_EMPTY(sand_at(g, x, y))) {
                continue;
            }
            if (sand_block_settled(g, x / SAND_BLOCK_W, y / SAND_BLOCK_H)) {
                *settled += 1.0;
            } else {
                *moving += 1.0;
            }
        }
    }
}

static void
settle(sand_t* g, int steps) {
    for (int i = 0; i < steps; i++) {
        sand_step(g, 0, 1000, 0);
    }
}

/* Returns what it actually achieved: a gas rises and disperses, fire burns
 * out, and neither will ever reach a settled share. The stall guard is what
 * makes those materials answerable rather than an infinite loop, and the
 * shortfall is itself a result worth printing. */
static int
pour_until_share(sand_t* g, cell_t cell, int target) {
    int phase = 4;
    int best = 0;
    int stalled = 0;

    if (is_static(cell)) {
        return place_static(g, cell, target);
    }
    for (int guard = 0; guard < POUR_GUARD; guard++) {
        const int have = count_of(g, cell);
        if (have >= target) {
            return have;
        }
        if (have > best) {
            best = have;
            stalled = 0;
        } else if (++stalled > POUR_STALL) {
            return have;
        }
        pour_stamp(g, cell, phase);
        phase++;
        sand_step(g, 0, 1000, 0);
    }
    return count_of(g, cell);
}

static timing_t
pour_timed(sand_t* g, cell_t cell, int target) {
    const int per_step = (target + POUR_WINDOW - 1) / POUR_WINDOW;
    timing_t t = {0.0, 0.0, 0.0, 0.0};
    int spawned = 0;
    int samples = 0;

    for (int i = 0; i < POUR_WINDOW; i++) {
        const int want = (target - spawned < per_step) ? target - spawned : per_step;
        const int n = (want > 0) ? want : 0;
        if (is_static(cell)) {
            spawned += place_static(g, cell, n);
        } else {
            int placed = 0;
            for (int s = 0; s < PER_STEP_STAMPS && placed < n; s++) {
                placed += pour_stamp(g, cell, i * PER_STEP_STAMPS + s);
            }
            spawned += placed;
        }

        const double t0 = now_us();
        sand_step(g, 0, 1000, 0);
        const double dt = now_us() - t0;
        t.mean_us += dt;
        if (dt > t.worst_us) {
            t.worst_us = dt;
        }
        if (i % CENSUS_EVERY == 0) {
            census_mass(g, &t.settled_cells, &t.moving_cells);
            samples++;
        }
    }
    t.mean_us /= POUR_WINDOW;
    if (samples > 0) {
        t.settled_cells /= samples;
        t.moving_cells /= samples;
    }
    return t;
}

int
main(int argc, char** argv) {
    if (argc == 2 && strcmp(argv[1], "--list") == 0) {
        list_materials();
        return 0;
    }
    if (argc < 5) {
        fprintf(stderr, "usage: ratio_probe <matA> <pctA> <matB> <pctB>\n");
        return 2;
    }
    const cell_t a_cell = resolve_material(argv[1]);
    const cell_t b_cell = resolve_material(argv[3]);
    const int a_pct = atoi(argv[2]);
    const int b_pct = atoi(argv[4]);
    if (a_cell == 0 || b_cell == 0) {
        fprintf(stderr, "unknown material: %s / %s\n", argv[1], argv[3]);
        return 2;
    }

    uint8_t* cells = calloc(CELLS, 1);
    uint8_t* blocks = calloc(((GW + SAND_BLOCK_W - 1) / SAND_BLOCK_W) * ((GH + SAND_BLOCK_H - 1) / SAND_BLOCK_H), 1);
    sand_t g;
    memset(&g, 0, sizeof g);
    sand_init(&g, cells, GW, GH, 12345u);
    sand_enable_sleeping(&g, blocks);

    const int a_target = CELLS * a_pct / 100;
    pour_until_share(&g, a_cell, a_target);
    settle(&g, SETTLE_STEPS);
    const int a_n = count_of(&g, a_cell);

    const timing_t t = pour_timed(&g, b_cell, CELLS * b_pct / 100);

    printf("a=%s a_pct=%d b=%s b_pct=%d a_n=%d a_want=%d b_n=%d "
           "settled=%.0f moving=%.0f "
           "mean_us=%.1f worst_us=%.1f\n",
           argv[1], a_pct, argv[3], b_pct, a_n, a_target, count_of(&g, b_cell), t.settled_cells, t.moving_cells,
           t.mean_us, t.worst_us);

    free(cells);
    free(blocks);
    return 0;
}
