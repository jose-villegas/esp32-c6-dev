/*
 * Shared fixtures and assertion helpers - see suite_sand_common.h. No
 * SUITE_REGISTER here: this file registers no suite of its own, it just
 * gives every other suite_sand_*.c file the same fixture grid and the same
 * handful of assertion helpers to build on.
 */
#include <stdlib.h>
#include <string.h>

#include "suites.h"
#include "unity.h"

#include "sand.h"
#include "suite_sand_common.h"

sand_t s;
uint8_t cells[W * H];
sand_test_fx_t fx;
uint8_t dirty[H];
uint8_t sleep_blocks[BLOCK_COLS * BLOCK_ROWS];
uint8_t* wide_cells;
sand_t wide;

void
fixture(void) {
    sand_init(&s, cells, W, H, 12345u);
}

/* Load a picture of a grid. Rows are given top to bottom, so the text reads
 * the way the screen looks. */
void
load(const char* rows[], int count) {
    sand_clear(&s);
    for (int y = 0; y < count; y++) {
        for (int x = 0; rows[y][x] != '\0'; x++) {
            if (rows[y][x] == 'o') {
                sand_set(&s, x, y, SAND_FIRST_SHADE);
            }
        }
    }
}

void
dirty_fixture(void) {
    fixture();
    sand_track_dirty_rows(&s, dirty);
    memset(dirty, 0, sizeof(dirty));
}

void
settle_with_sleeping(const char* rows[], int count, int steps, int gx, int gy) {
    fixture();
    sand_enable_sleeping(&s, sleep_blocks);
    load(rows, count);

    for (int i = 0; i < steps; i++) {
        sand_step(&s, gx, gy, 0);
    }
}

void
assert_nothing_left_to_do(int gx, int gy) {
    uint8_t settled[W * H];
    memcpy(settled, cells, sizeof(settled));

    /* Same grid, same rules, but every row examined every step. */
    sand_t awake;
    sand_init(&awake, cells, W, H, 999u);
    memcpy(cells, settled, sizeof(settled));

    for (int i = 0; i < 60; i++) {
        sand_step(&awake, gx, gy, 0);
    }

    TEST_ASSERT_EQUAL_MEMORY_MESSAGE(settled, cells, sizeof(settled),
                                     "a fully awake simulation found something to move that the sleeping "
                                     "one had left alone - which means sleeping froze sand that should "
                                     "still have been falling");
}

/* Total AMOUNT of a material, not the number of cells holding it.
 *
 * For a liquid these are different questions, and only this one has a
 * conserved answer: two half-full cells can merge into one full cell without
 * a drop being lost. Counting cells and calling it conservation would report
 * a leak every time water settled. */
long
mass_of(const sand_t* g, int w, int h, material_id_t m) {
    long total = 0;
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            const cell_t c = sand_at(g, x, y);
            if (!CELL_IS_EMPTY(c) && CELL_MATERIAL(c) == m) {
                total += CELL_VARIANT(c);
            }
        }
    }
    return total;
}

int
count_of(material_id_t m) {
    int n = 0;
    for (int y = 0; y < H; y++) {
        for (int x = 0; x < W; x++) {
            if (CELL_MATERIAL(sand_at(&s, x, y)) == m) {
                n++;
            }
        }
    }
    return n;
}

/* Cells, not mass: a powder's variant is a shade, so summing it would be
 * meaningless for sand. */
int
count_cells_of(uint8_t id) {
    int n = 0;
    for (int y = 0; y < H; y++) {
        for (int x = 0; x < W; x++) {
            if (CELL_MATERIAL(sand_at(&s, x, y)) == id) {
                n++;
            }
        }
    }
    return n;
}

long
mass_held_by(uint8_t id) {
    long m = 0;
    for (int y = 0; y < H; y++) {
        for (int x = 0; x < W; x++) {
            const cell_t c = sand_at(&s, x, y);
            if (!CELL_IS_EMPTY(c) && CELL_MATERIAL(c) == id) {
                m += CELL_VARIANT(c);
            }
        }
    }
    return m;
}

/* Total mass of one liquid on the board - fill levels, not cell count.
 * A liquid that spreads occupies more cells holding less each, so cells
 * are the wrong unit for asking whether any of it was destroyed. */
int
liquid_mass_of(uint8_t id) {
    int m = 0;
    for (int y = 0; y < H; y++) {
        for (int x = 0; x < W; x++) {
            const cell_t c = sand_at(&s, x, y);
            if (CELL_MATERIAL(c) == id) {
                m += CELL_VARIANT(c);
            }
        }
    }
    return m;
}

int
first_row_holding(uint8_t id) {
    for (int y = 0; y < H; y++) {
        for (int x = 0; x < W; x++) {
            if (CELL_MATERIAL(sand_at(&s, x, y)) == id) {
                return y;
            }
        }
    }
    return -1;
}

/* The caller passes the exact span it is about to fill (x0..x1 inclusive) -
 * no slack, no spare cells, by construction. A room with even one spare
 * empty cell left room for a gas or fire cell to drift sideways into it
 * during the same step it was placed, before any reaction ever ran. */
void
fire_room(int x0, int x1) {
    fixture();
    for (int x = 0; x < W; x++) {
        sand_set(&s, x, 2, STONE);
        sand_set(&s, x, 4, STONE);
    }
    sand_set(&s, x0 - 1, 3, STONE);
    sand_set(&s, x1 + 1, 3, STONE);
}

/* A sealed stone box: stone floor and stone walls, so the water can neither
 * drain nor spread and a gas cell placed inside it has nowhere to go
 * except up through the water. */
void
water_column(void) {
    fixture();
    sand_set_decay(&s, 0); /* immortal: these tests measure movement,
                              * and a decaying cell that vanished
                              * mid-rise would read as "never escaped" */
    for (int y = 1; y <= 7; y++) {
        sand_set(&s, 1, y, STONE);
        sand_set(&s, 6, y, STONE);
    }
    for (int x = 1; x <= 6; x++) {
        sand_set(&s, x, 7, STONE);
    }
    for (int y = 2; y <= 6; y++) {
        for (int x = 2; x <= 5; x++) {
            sand_set(&s, x, y, CELL_MAKE(MAT_WATER, MASS_MAX));
        }
    }
}

/* A sealed GLASS tank with `sand_rows` of sand in the bottom and
 * `acid_rows` of acid above it. Returns the acid mass placed.
 *
 * Glass, not stone. Stone used to be immune to acid and was therefore the
 * only thing acid could be kept in; it dissolves like everything else now
 * and glass is the sole exception. */
long
acid_tank(int sand_rows, int acid_rows) {
    fixture();
    sand_set_mobility(&s, SAND_MOBILITY_PER_MATERIAL);
    for (int x = 1; x < W - 1; x++) {
        sand_set(&s, x, H - 1, GLASS);
    }
    for (int y = 1; y < H; y++) {
        sand_set(&s, 1, y, GLASS);
        sand_set(&s, W - 2, y, GLASS);
    }
    for (int y = H - 1 - sand_rows; y < H - 1; y++) {
        for (int x = 2; x < W - 2; x++) {
            sand_set(&s, x, y, CELL_MAKE(MAT_SAND, 8));
        }
    }
    for (int y = 1; y <= acid_rows; y++) {
        for (int x = 2; x < W - 2; x++) {
            sand_set(&s, x, y, CELL_MAKE(MAT_ACID, MASS_MAX));
        }
    }
    return mass_held_by(MAT_ACID);
}

/* Rec.601 luminance of a panel colour, 0-255.
 *
 * The palette stores GFX_RGB, which is RGB565 with the bytes swapped for
 * this QSPI controller - so getting a brightness back out means undoing
 * both. Written out here rather than guessed at, because a test that
 * unpacks the colour wrongly will still compare two numbers and still
 * pass or fail for reasons of its own. */
int
panel_luminance(gfx_color_t c) {
    const unsigned v = (unsigned)((c >> 8) | ((c & 0xFFu) << 8));
    const unsigned r = ((v >> 11) & 0x1Fu) * 255u / 31u;
    const unsigned g = ((v >> 5) & 0x3Fu) * 255u / 63u;
    const unsigned b = (v & 0x1Fu) * 255u / 31u;
    return (int)((299u * r + 587u * g + 114u * b) / 1000u);
}
