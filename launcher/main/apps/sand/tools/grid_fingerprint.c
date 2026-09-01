/*
 * Prints a reproducible fingerprint of what the simulation actually DOES:
 * for each reference scene, a hash over the whole grid after a fixed number
 * of steps, plus the per-material cell histogram behind it.
 *
 * This exists for the optimisation loop. A perf change is supposed to make
 * the same simulation cheaper, not a different simulation - but the test
 * suite only samples that claim at the points its assertions happen to
 * look, and every budget row measures TIME, not behaviour. A change can
 * therefore go green, land a real speedup, and still have quietly moved a
 * grain. Hashing the grid closes that gap: identical hash means the change
 * was genuinely free of behavioural consequence, over every cell, at every
 * scene, rather than at the handful of cells some assertion inspects.
 *
 * The histogram is printed BESIDE the hash on purpose, because "the hash
 * changed" on its own is not actionable. Material counts are invariant
 * under reordering - if two grains swap which one moves first, positions
 * differ but the counts cannot - so a changed hash with an IDENTICAL
 * histogram is the signature of a reordering, the class of change this
 * project has repeatedly found to be semantically fine but observably
 * different (merging the burning cell's three neighbour walks was priced
 * and deferred for exactly this reason). A changed histogram means
 * material was created or destroyed, which is a bug until proven
 * otherwise. That distinction is what makes an overnight run triageable
 * in the morning instead of a pile of unexplained diffs.
 *
 * Determinism is the whole product here: fixed seeds, fixed gravity, fixed
 * step counts, no clock read anywhere. Two runs of the same binary on the
 * same source must print byte-identical output, or this tool is worse than
 * useless - it would manufacture exactly the false alarms it is meant to
 * rule out.
 *
 * Usage:
 *   main/apps/sand/tools/report_fingerprint.sh            # print
 *   main/apps/sand/tools/report_fingerprint.sh --check    # diff vs baseline
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "sand.h"
#include "material.h"

/* Small enough that all four scenes run in well under a second on a laptop
 * - this gets called once per candidate in a loop that may try dozens
 * overnight - and large enough that grains actually interact rather than
 * each falling down its own private column. */
#define FP_W     64
#define FP_H     64
#define FP_STEPS 300

/* FNV-1a. Chosen for being short enough to read and verify by eye in this
 * file; nothing here needs cryptographic strength, only that two different
 * grids reliably produce two different numbers. */
static uint64_t fnv1a(const uint8_t *data, size_t n)
{
    uint64_t h = 1469598103934665603ULL;
    for (size_t i = 0; i < n; i++) {
        h ^= data[i];
        h *= 1099511628211ULL;
    }
    return h;
}

/* Buckets by the cell's HIGH nibble, which is the material id - including
 * id 15, the extended-range escape hatch (material.h), which is counted as
 * its own bucket rather than decoded. A change that moves cells into or
 * out of the extended range is exactly as interesting as one that moves
 * them between ordinary materials, and decoding here would hide it. */
static void histogram(const uint8_t *cells, int n, int counts[16])
{
    memset(counts, 0, sizeof(int) * 16);
    for (int i = 0; i < n; i++) {
        counts[(cells[i] >> 4) & 0x0F]++;
    }
}

typedef void (*scene_fn)(sand_t *s);

/* Scene 1: dry grains over a floor. The main sweep and nothing else - no
 * liquid, no reactions, no gas. This is the control: a change that alters
 * THIS hash altered the core movement rule, whatever else it claimed to
 * touch. */
static void scene_dry_fall(sand_t *s)
{
    for (int x = 0; x < FP_W; x++) {
        sand_set(s, x, FP_H - 1, CELL_MAKE(MAT_STONE, MASS_MAX));
    }
    for (int y = 4; y < 28; y++) {
        for (int x = 3; x < FP_W - 3; x++) {
            if (((x * 7 + y * 13) & 3) == 0) {
                sand_set(s, x, y, CELL_MAKE(MAT_SAND, MASS_MAX));
            }
        }
    }
}

/* Scene 2: a water column against a wall, so the liquid pass and its
 * cross-flow both run. Sand on top so the two movement models have to
 * interleave rather than each getting the grid to itself. */
static void scene_water_pool(sand_t *s)
{
    for (int x = 0; x < FP_W; x++) {
        sand_set(s, x, FP_H - 1, CELL_MAKE(MAT_STONE, MASS_MAX));
    }
    for (int y = FP_H - 20; y < FP_H - 1; y++) {
        sand_set(s, 8, y, CELL_MAKE(MAT_STONE, MASS_MAX));
        for (int x = 9; x < 40; x++) {
            sand_set(s, x, y, CELL_MAKE(MAT_WATER, MASS_MAX));
        }
    }
    for (int x = 12; x < 30; x++) {
        sand_set(s, x, 6, CELL_MAKE(MAT_SAND, MASS_MAX));
    }
}

/* Scene 3: lava meeting water over stone - quench, steam and the heat
 * conduction that drives them. The reactions pass, which is where most of
 * this app's per-cell cost now lives. */
static void scene_lava_quench(sand_t *s)
{
    for (int x = 0; x < FP_W; x++) {
        sand_set(s, x, FP_H - 1, CELL_MAKE(MAT_STONE, MASS_MAX));
    }
    for (int y = FP_H - 12; y < FP_H - 1; y++) {
        for (int x = 2; x < 30; x++) {
            sand_set(s, x, y, CELL_MAKE(MAT_WATER, MASS_MAX));
        }
    }
    for (int y = 8; y < 16; y++) {
        for (int x = 6; x < 24; x++) {
            sand_set(s, x, y, CELL_MAKE(MAT_LAVA, MASS_MAX));
        }
    }
    for (int x = 34; x < 60; x++) {
        sand_set(s, x, FP_H - 2, CELL_MAKE(MAT_SAND, MASS_MAX));
        sand_set(s, x, FP_H - 3, CELL_MAKE(MAT_DIRT, MASS_MAX));
    }
}

/* Scene 4: burning wood under a gas pocket. The gas pass, ignition and
 * smoke - the passes the other three scenes leave almost entirely idle. */
static void scene_fire_gas(sand_t *s)
{
    for (int x = 0; x < FP_W; x++) {
        sand_set(s, x, FP_H - 1, CELL_MAKE(MAT_STONE, MASS_MAX));
    }
    for (int y = FP_H - 10; y < FP_H - 1; y++) {
        for (int x = 4; x < 44; x++) {
            sand_set(s, x, y, CELL_MAKE(MAT_WOOD, MASS_MAX));
        }
    }
    for (int x = 10; x < 20; x++) {
        sand_set(s, x, FP_H - 11, CELL_MAKE(MAT_FIRE, MASS_MAX));
    }
    for (int y = 18; y < 30; y++) {
        for (int x = 20; x < 50; x++) {
            sand_set(s, x, y, CELL_MAKE(MAT_GAS, MASS_MAX));
        }
    }
    for (int y = 30; y < 36; y++) {
        for (int x = 44; x < 58; x++) {
            sand_set(s, x, y, CELL_MAKE(MAT_OIL, MASS_MAX));
        }
    }
}

/* Scene 5: lava sealed under a thick stone lid, with vent_chance forced
 * so the vent path fires every step rather than rarely.
 *
 * Added after this tool FAILED to notice a deliberately broken vent scan
 * (vent_column()'s reach widened 3x) - the first four scenes never build
 * sealed lava, so the whole vent mechanism was outside what the
 * fingerprint could see, and the gate passed a real behavioural
 * regression. That is the standing lesson for anyone extending this file:
 * a fingerprint only covers the mechanisms its scenes actually reach, and
 * the way to find out which those are is to break a mechanism on purpose
 * and check this tool goes red. */
static void scene_sealed_lava(sand_t *s)
{
    sand_set_vent_chance(s, 255);

    for (int x = 0; x < FP_W; x++) {
        sand_set(s, x, FP_H - 1, CELL_MAKE(MAT_STONE, MASS_MAX));
    }

    /* The lid must be DEEPER than SAND_VENT_REACH, or this scene proves
     * nothing: a vent that can already see open air through the lid
     * behaves identically whether its reach is 30 or 90, and the first
     * version of this scene - a 12-cell lid against a reach of 30 - was
     * exactly that mistake. It ran, it hashed, and it stayed green when
     * the vent scan was deliberately broken.
     *
     * So: lava on the floor, stone from just above it up past the reach,
     * open air only beyond. A correct vent stays capped inside the lid; a
     * widened one reaches the air and empties the pocket. */
    for (int k = 0; k < 3; k++) {
        const int cx = 12 + k * 20;
        const int lava_y = FP_H - 2;
        const int lid_top = lava_y - (SAND_VENT_REACH + 8);

        for (int y = lid_top; y < lava_y; y++) {
            for (int dx = -4; dx <= 4; dx++) {
                sand_set(s, cx + dx, y, CELL_MAKE(MAT_STONE, MASS_MAX));
            }
        }
        for (int dx = -4; dx <= 4; dx++) {
            if (dx != 0) {
                sand_set(s, cx + dx, lava_y, CELL_MAKE(MAT_STONE, MASS_MAX));
            }
        }
        sand_set(s, cx, lava_y, CELL_MAKE(MAT_LAVA, MASS_MAX));
    }
}

static const struct {
    const char *name;
    scene_fn    build;
    uint32_t    seed;
} SCENES[] = {
    { "dry_fall",    scene_dry_fall,    7u  },
    { "water_pool",  scene_water_pool,  11u },
    { "lava_quench", scene_lava_quench, 23u },
    { "fire_gas",    scene_fire_gas,    31u },
    { "sealed_lava", scene_sealed_lava, 41u },
};

int main(void)
{
    const int cell_count = FP_W * FP_H;

    printf("# grid fingerprint: %dx%d, %d steps per scene\n",
           FP_W, FP_H, FP_STEPS);
    printf("# scene hash mat0..mat15\n");

    for (size_t i = 0; i < sizeof SCENES / sizeof SCENES[0]; i++) {
        uint8_t   *cells    = calloc((size_t)cell_count, 1);
        impulse_t *impulses = calloc((size_t)cell_count, sizeof *impulses);
        if (!cells || !impulses) {
            fprintf(stderr, "grid_fingerprint: out of memory\n");
            free(cells);
            free(impulses);
            return 1;
        }

        sand_t s;
        sand_init(&s, cells, FP_W, FP_H, SCENES[i].seed);
        /* Impulses on, because the throw paths (vents, splashes) are part
         * of the behaviour being fingerprinted - a loop is allowed to
         * optimise them, so a change there must show up here. */
        sand_enable_impulses(&s, impulses, cell_count);
        SCENES[i].build(&s);

        /* Gravity held straight down and jostle fixed: this tool answers
         * "did the same input produce the same output", so every input
         * including the environment has to be pinned. */
        for (int step = 0; step < FP_STEPS; step++) {
            sand_step(&s, 0, 1000, 0);
        }

        int counts[16];
        histogram(cells, cell_count, counts);

        printf("%-12s %016llx",
               SCENES[i].name,
               (unsigned long long)fnv1a(cells, (size_t)cell_count));
        for (int m = 0; m < 16; m++) {
            printf(" %d", counts[m]);
        }
        printf("\n");

        free(cells);
        free(impulses);
    }

    return 0;
}
