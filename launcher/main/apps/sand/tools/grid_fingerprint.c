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
#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

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

/* ONE MACRO PER MATERIAL, because MASS_MAX is not a general "a full cell of
 * this" idiom and using it as one built four different kinds of wrong cell.
 * A cell's low nibble means whatever its own material says it means:
 *
 *     water, lava, oil    MASS - MASS_MAX is correct, and only here
 *     stone, glass        HEAT - MASS_MAX is the TOP of the ramp, so every
 *                         floor and wall in this file started white-hot
 *     wood                BURN PROGRESS - cell_is_burning() is variant != 0
 *                         for anything with burn_decay, so the wood wall in
 *                         scene_fire_gas started ALIGHT, which is why that
 *                         scene never actually tested ignition spreading
 *     sand, dirt          MOISTURE (plus a tone bit) - MASS_MAX is fully
 *                         saturated, so the "dry grains" control was wet
 *     gas, fire, smoke    REMAINING LIFE - MASS_MAX happens to be the right
 *                         VALUE here, but only by coincidence of both being
 *                         MATERIAL_VARIANTS - 1; spelled honestly instead
 *
 * Naming them mirrors suite_sand.c, which has always done this correctly and
 * is why the same bug never reached the test suite. Regenerating the baseline
 * alongside this is expected: the SCENES changed, not the simulation.
 */
#define FP_STONE  CELL_MAKE(MAT_STONE, SAND_AMBIENT_HEAT)
#define FP_SAND   CELL_MAKE(MAT_SAND,  0)   /* dry */
#define FP_DIRT   CELL_MAKE(MAT_DIRT,  0)   /* dry */
#define FP_WOOD   CELL_MAKE(MAT_WOOD,  0)   /* not alight */
#define FP_WATER  CELL_MAKE(MAT_WATER, MASS_MAX)
#define FP_LAVA   CELL_MAKE(MAT_LAVA,  MASS_MAX)
#define FP_OIL    CELL_MAKE(MAT_OIL,   MASS_MAX)
#define FP_GAS    CELL_MAKE(MAT_GAS,   MATERIAL_VARIANTS - 1)
#define FP_FIRE   CELL_MAKE(MAT_FIRE,  MATERIAL_VARIANTS - 1)


/* Scene 1: dry grains over a floor. The main sweep and nothing else - no
 * liquid, no reactions, no gas. This is the control: a change that alters
 * THIS hash altered the core movement rule, whatever else it claimed to
 * touch. */
static void scene_dry_fall(sand_t *s)
{
    for (int x = 0; x < FP_W; x++) {
        sand_set(s, x, FP_H - 1, FP_STONE);
    }
    for (int y = 4; y < 28; y++) {
        for (int x = 3; x < FP_W - 3; x++) {
            if (((x * 7 + y * 13) & 3) == 0) {
                sand_set(s, x, y, FP_SAND);
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
        sand_set(s, x, FP_H - 1, FP_STONE);
    }
    for (int y = FP_H - 20; y < FP_H - 1; y++) {
        sand_set(s, 8, y, FP_STONE);
        for (int x = 9; x < 40; x++) {
            sand_set(s, x, y, FP_WATER);
        }
    }
    for (int x = 12; x < 30; x++) {
        sand_set(s, x, 6, FP_SAND);
    }
}

/* Scene 3: lava meeting water over stone - quench, steam and the heat
 * conduction that drives them. The reactions pass, which is where most of
 * this app's per-cell cost now lives. */
static void scene_lava_quench(sand_t *s)
{
    for (int x = 0; x < FP_W; x++) {
        sand_set(s, x, FP_H - 1, FP_STONE);
    }
    for (int y = FP_H - 12; y < FP_H - 1; y++) {
        for (int x = 2; x < 30; x++) {
            sand_set(s, x, y, FP_WATER);
        }
    }
    for (int y = 8; y < 16; y++) {
        for (int x = 6; x < 24; x++) {
            sand_set(s, x, y, FP_LAVA);
        }
    }
    for (int x = 34; x < 60; x++) {
        sand_set(s, x, FP_H - 2, FP_SAND);
        sand_set(s, x, FP_H - 3, FP_DIRT);
    }
}

/* Scene 4: burning wood under a gas pocket. The gas pass, ignition and
 * smoke - the passes the other three scenes leave almost entirely idle. */
static void scene_fire_gas(sand_t *s)
{
    for (int x = 0; x < FP_W; x++) {
        sand_set(s, x, FP_H - 1, FP_STONE);
    }
    for (int y = FP_H - 10; y < FP_H - 1; y++) {
        for (int x = 4; x < 44; x++) {
            sand_set(s, x, y, FP_WOOD);
        }
    }
    for (int x = 10; x < 20; x++) {
        sand_set(s, x, FP_H - 11, FP_FIRE);
    }
    for (int y = 18; y < 30; y++) {
        for (int x = 20; x < 50; x++) {
            sand_set(s, x, y, FP_GAS);
        }
    }
    for (int y = 30; y < 36; y++) {
        for (int x = 44; x < 58; x++) {
            sand_set(s, x, y, FP_OIL);
        }
    }
}

/* Scene 5: lava sealed under a stone lid, with the lava-burst chance (bd
 * esp32c6-mqt) forced so every covered cell converts within this scene's
 * own step budget rather than waiting on its real, deliberately rare
 * production rate.
 *
 * Added after this tool FAILED to notice a deliberately broken vent scan
 * (the mechanism this scene originally exercised, removed by bd
 * esp32c6-0f2 once the burst replaced it) - the first four scenes never
 * build sealed lava, so the whole mechanism was outside what the
 * fingerprint could see, and the gate passed a real behavioural
 * regression. Rebuilt against the burst rather than dropped, so the
 * standing lesson for anyone extending this file still holds: a
 * fingerprint only covers the mechanisms its scenes actually reach, and
 * the way to find out which those are is to break a mechanism on purpose
 * and check this tool goes red.
 *
 * STALE CLAIM CORRECTED: this used to say no impulse buffer is enabled
 * anywhere in this file, making sand_explode() here a documented no-op.
 * main() (below) now calls sand_enable_impulses() unconditionally for
 * every scene, itself explicitly - "the throw paths (explosions, bursts,
 * splashes) are part of the behaviour being fingerprinted" - so a burst
 * here converts the covered cell to stone AND throws debris, same as any
 * other scene. That is still enough to exercise and hash the conversion
 * this scene exists to cover; it is simply no longer the ONLY thing this
 * scene exercises. */
static void scene_sealed_lava(sand_t *s)
{
    sand_set_lava_burst(s, 255);

    for (int x = 0; x < FP_W; x++) {
        sand_set(s, x, FP_H - 1, FP_STONE);
    }

    /* Three separate pockets, each a lava cell boxed on all 4 cardinal
     * AND all 4 diagonal neighbours by stone - unambiguously covered on
     * all three of covered_at()'s gravity-relative lid cells regardless
     * of which way is down, so the scene proves the conversion actually
     * fires rather than merely COULD fire. */
    for (int k = 0; k < 3; k++) {
        const int cx = 12 + k * 20;
        const int lava_y = FP_H - 3;

        for (int dy = -1; dy <= 1; dy++) {
            for (int dx = -1; dx <= 1; dx++) {
                if (dx == 0 && dy == 0) continue;
                sand_set(s, cx + dx, lava_y + dy, FP_STONE);
            }
        }
        sand_set(s, cx, lava_y, FP_LAVA);
    }
}

/* Scene 6: a dirt bank under standing water, soaking enabled - the
 * moisture codec (soaking up, percolating, drying) sits entirely OUTSIDE
 * every scene above: none of them ever call sand_set_soak(), so
 * step_one_soaking_cell() - the function moisture_of()/with_moisture()
 * (material.h) now route both dirt AND gunpowder through - could regress
 * freely with this whole file staying green. Dry dirt shelved against a
 * stone floor, watered from above, gives both the soaking-up transition
 * and the percolation that spreads it downward through the bank real,
 * sustained exercise over the whole budget - not just a first splash. */
static void scene_wet_earth(sand_t *s)
{
    sand_set_soak(s, SAND_SOAK_PER_MATERIAL);

    for (int x = 0; x < FP_W; x++) {
        sand_set(s, x, FP_H - 1, FP_STONE);
    }
    for (int y = FP_H - 20; y < FP_H - 1; y++) {
        for (int x = 4; x < 40; x++) {
            sand_set(s, x, y, FP_DIRT);
        }
    }
    for (int y = FP_H - 32; y < FP_H - 20; y++) {
        for (int x = 8; x < 36; x++) {
            sand_set(s, x, y, FP_WATER);
        }
    }
}

/* A PLANTED BED, because nothing else here grows.
 *
 * THE GAP THIS CLOSES, demonstrated rather than assumed: setting GROW_REACH
 * to 1 - a value that cripples plant growth outright - moved not one of the
 * eight scenes above. The whole plant and root system, anchored()'s support
 * search, find_water(), rooting, budding and sprouting, had no
 * behavioural cover at all, while a performance round was about to start
 * changing it.
 *
 * Dry dirt over sand, seeded, with rain above: dirt at full moisture cannot
 * soak the rain up, so it pools and drowns the seeds, and a submerged plant
 * has no room to bud into and never grows. */
static void scene_plant_bed(sand_t *s)
{
    sand_set_soak(s, SAND_SOAK_PER_MATERIAL);

    const int bed_top = FP_H - 20;

    for (int y = bed_top; y < FP_H; y++) {
        for (int x = 0; x < FP_W; x++) {
            sand_set(s, x, y, y < FP_H - 10 ? FP_DIRT : FP_SAND);
        }
    }
    for (int x = 4; x < FP_W; x += 8) {
        sand_set(s, x, bed_top - 1, MATX(MATX_PLANT));
    }
    for (int y = bed_top - 8; y < bed_top - 4; y++) {
        for (int x = 0; x < FP_W; x++) {
            sand_set(s, x, y, FP_WATER);
        }
    }
}

/* GRAVITY IS PER SCENE, and the six original rows keep the straight-down
 * vector they were baselined with - their hashes must not move.
 *
 * The two below exist because everything else here holds gravity vertical,
 * and a vertical vector is the ONE case that leaves the perpendicular
 * horizontal: perp is ring_dir(i_stable + 2), so py == 0 only when gravity is
 * (0, +-1). Landscape and every diagonal give py != 0, where equalise_gas()
 * takes a different path, gas_run_t's carry is disabled, and the row skip has
 * a branch that runs nowhere else. None of that was reachable by this tool -
 * corrupting a tilted-only branch left --check reporting "identical". */
static const struct {
    const char *name;
    scene_fn    build;
    uint32_t    seed;
    int         gx;
    int         gy;
} SCENES[] = {
    { "dry_fall",    scene_dry_fall,    7u,  0,    1000 },
    { "water_pool",  scene_water_pool,  11u, 0,    1000 },
    { "lava_quench", scene_lava_quench, 23u, 0,    1000 },
    { "fire_gas",    scene_fire_gas,    31u, 0,    1000 },
    { "sealed_lava", scene_sealed_lava, 41u, 0,    1000 },
    { "wet_earth",   scene_wet_earth,   53u, 0,    1000 },

    /* Same builders, held sideways and cornerwise. */
    { "gas_land",    scene_fire_gas,    31u, 1000, 0    },
    { "water_diag",  scene_water_pool,  11u, 1000, 1000 },

    /* The only row here in which anything grows - see the builder. */
    { "plant_bed",   scene_plant_bed,   11u, 0,    1000 },
};

int main(void)
{
    const int cell_count = FP_W * FP_H;

#ifdef _WIN32
    /* Byte-identical output (top comment) must hold ACROSS PLATFORMS: the
     * baseline is checked in and compared against a fresh run, routinely
     * from different machines. MinGW's CRT defaults stdout to text mode and
     * rewrites every '\n' into "\r\n", so a Windows run disagreed with the
     * LF baseline on every line whatever the simulation did - --check could
     * not pass here, --update wrote a baseline that passed nowhere else.
     * Same fix as dump_reactions.c beside this file. */
    _setmode(_fileno(stdout), _O_BINARY);
#endif

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
        /* Impulses on, because the throw paths (explosions, bursts,
         * splashes) are part of the behaviour being fingerprinted - a
         * loop is allowed to optimise them, so a change there must show
         * up here. */
        sand_enable_impulses(&s, impulses, cell_count);
        SCENES[i].build(&s);

        /* Gravity pinned PER SCENE and jostle fixed: this tool answers "did
         * the same input produce the same output", so every input including
         * the environment has to be pinned - pinned to one value, not to the
         * same value everywhere. */
        for (int step = 0; step < FP_STEPS; step++) {
            sand_step(&s, SCENES[i].gx, SCENES[i].gy, 0);
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
