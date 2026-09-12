/*
 * model_scene_main - the scene driver instruction_model.py compiles with
 * gcov, to learn how many times each source line of a step runs.
 *
 * Separate from counters_scene_main.c next door, which counts hand-placed
 * events and must stay byte-identical across the refs its bisect reaches.
 * This one counts nothing itself: gcov does the counting, so the file's
 * whole job is to build a scene and run a fixed number of steps with the
 * profile counters zeroed either side of the setup - a scene builder writes
 * tens of thousands of cells through sand_set(), and leaving that in the
 * profile would triple sand_set()'s line counts and put it at the top of a
 * report about the step.
 *
 * The scenes mirror suite_sand_perf.c's own builders, held as literals here
 * for the same reason counters_scene_main.c holds REAL_W/REAL_H as literals:
 * this file links against nothing but sand.h's public API, so it compiles
 * against any ref without dragging the suite's Unity scaffolding along.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "material.h"
#include "sand.h"

void __gcov_reset(void);
void __gcov_dump(void);

#define REAL_W          184
#define REAL_H          224
#define REAL_BLOCK_COLS ((REAL_W + SAND_BLOCK_W - 1) / SAND_BLOCK_W)
#define REAL_BLOCK_ROWS ((REAL_H + SAND_BLOCK_H - 1) / SAND_BLOCK_H)

/* Half a screen of water dropped in as an uneven slab - the same scene
 * test_a_screen_of_water_fits_in_the_frame_budget builds, seed and all. */
static void
build_water(sand_t* real, uint8_t* big, uint8_t* blocks) {
    sand_init(real, big, REAL_W, REAL_H, 11u);
    sand_enable_sleeping(real, blocks);
    for (int y = 0; y < REAL_H / 2; y++) {
        for (int x = REAL_W / 4; x < (REAL_W * 3) / 4; x++) {
            sand_set(real, x, y, CELL_MAKE(MAT_WATER, MASS_MAX));
        }
    }
}

static void
build_sand(sand_t* real, uint8_t* big, uint8_t* blocks) {
    sand_init(real, big, REAL_W, REAL_H, 7u);
    sand_enable_sleeping(real, blocks);
    for (int y = 0; y < REAL_H; y++) {
        for (int x = 0; x < REAL_W; x++) {
            sand_set(real, x, y, CELL_MAKE(MAT_SAND, MASS_MAX));
        }
    }
}

int
main(int argc, char** argv) {
    const char* scene = (argc > 1) ? argv[1] : "water";
    const int steps = (argc > 2) ? atoi(argv[2]) : 20;

    uint8_t* big = malloc((size_t)REAL_W * (size_t)REAL_H);
    uint8_t* blocks = malloc((size_t)REAL_BLOCK_COLS * (size_t)REAL_BLOCK_ROWS);
    if (big == NULL || blocks == NULL) {
        fprintf(stderr, "model_scene_main: grid allocation failed\n");
        return 1;
    }

    sand_t real;
    if (strcmp(scene, "sand") == 0) {
        build_sand(&real, big, blocks);
    } else if (strcmp(scene, "water") == 0) {
        build_water(&real, big, blocks);
    } else {
        fprintf(stderr, "model_scene_main: unknown scene '%s'\n", scene);
        return 1;
    }

    __gcov_reset();
    for (int i = 0; i < steps; i++) {
        sand_step(&real, 0, 1000, 0);
    }
    __gcov_dump();

    printf("scene %s, %d steps\n", scene, steps);
    free(big);
    free(blocks);
    return 0;
}
