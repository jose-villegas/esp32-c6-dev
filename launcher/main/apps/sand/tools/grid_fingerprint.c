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

/* Small enough that every scene runs in well under a second on a laptop
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
 * this" idiom: a cell's low nibble means whatever its own material says it
 * means. MASS_MAX is mass for water, lava and oil only; for stone and glass
 * it is the top of the heat ramp, for wood full burn progress, for sand and
 * dirt full saturation, and for the gases the right value purely by the
 * coincidence of MATERIAL_VARIANTS - 1.
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
#define FP_ACID   CELL_MAKE(MAT_ACID,  MASS_MAX)
#define FP_STEAM  CELL_MAKE(MAT_STEAM, MATERIAL_VARIANTS - 1)
#define FP_GLASS  CELL_MAKE(MAT_GLASS, SAND_AMBIENT_HEAT)  /* heat_ramp, like stone */
#define FP_CULLET CELL_MAKE(MAT_SAND,  SAND_CULLET_BASE)   /* glass milled to grains */
#define FP_METAL  MATX(MATX_METAL)
#define FP_SNOW   CELL_MAKE(MAT_SNOW,  0)             /* dry, as sand and dirt are */
#define FP_SMOKE  CELL_MAKE(MAT_SMOKE, MATERIAL_VARIANTS - 1)
#define FP_WET_DIRT CELL_MAKE(MAT_DIRT, MASS_MAX)      /* saturated, not damp */
#define FP_HOT_GLASS CELL_MAKE(MAT_GLASS, SAND_SHOCK_HEAT - 1)
#define FP_ICE    MATX(MATX_ICE)
#define FP_POWDER GUNPOWDER_CELL(0)                    /* dry, unlit */
#define FP_PLANT  MATX(MATX_PLANT)
#define FP_LEAF   MATX(MATX_LEAF)
#define FP_ROOT   MATX(MATX_ROOT)


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

/* Scene 5: lava sealed under a stone lid, lava-burst chance (bd
 * esp32c6-mqt) forced to 255 so every covered cell converts within this
 * scene's step budget. Standing lesson for anyone extending this file: a
 * fingerprint only covers the mechanisms its scenes actually reach - to
 * find out which, break a mechanism on purpose and check this tool goes
 * red. main() calls sand_enable_impulses() unconditionally, so a burst
 * here converts the cell to stone AND throws debris, same as any other
 * scene. */
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

/* Scene 6: the moisture codec (soaking up, percolating, drying) sits
 * entirely outside every scene above - none of them calls sand_set_soak(),
 * so step_one_soaking_cell() could regress with this whole file green. Dry
 * dirt shelved against a stone floor and watered from above exercises both
 * the soaking-up transition and the percolation that spreads it downward,
 * over the whole budget rather than a first splash. */
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

/* A planted bed, because nothing else here grows: setting GROW_REACH to 1 -
 * a value that cripples growth outright - moved not one of the eight scenes
 * above, so the whole plant and root system had no behavioural cover.
 *
 * Dry dirt over sand, seeded, with rain above: dirt already at full moisture
 * cannot soak the rain up, so it pools and drowns the seeds. */
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

/* ACID, which appeared nowhere in this file - every scene above ran without
 * one acid cell, so any change to the dissolver or acid rain got "identical
 * to baseline" from a gate that never executed it. The cullet fix
 * (esp32c6-75e) shipped on exactly that; deleting it again takes this
 * scene's MAT_SAND from 20 to 0 and moves no other row.
 *
 * One band per outcome, because what the dissolver does interestingly is
 * REFUSE: sand goes fast (200), stone slowly (60), metal barely (1), glass
 * and cullet never. */
static void scene_acid_bath(sand_t *s)
{
    /* 1 in 256 in production would not fire inside 300 steps - forced for
     * the reason scene_sealed_lava forces the burst. */
    sand_set_acid_rain(s, 255);

    for (int x = 0; x < FP_W; x++) {
        sand_set(s, x, FP_H - 1, FP_STONE);
    }

    /* Glass is immune for having no dissolvable at all; cullet is immune
     * because it IS glass, and needs its own reject to say so - it shares
     * MAT_SAND's row. A band that starts going moves the histogram. */
    for (int y = FP_H - 3; y < FP_H - 1; y++) {
        for (int x = 0; x < FP_W; x++) {
            cell_t target;
            if (x < 10) {
                target = FP_SAND;
            } else if (x < 20) {
                target = FP_CULLET;
            } else if (x < 30) {
                target = FP_METAL;
            } else if (x < 40) {
                target = FP_GLASS;
            } else if (x < 50) {
                target = FP_STONE;
            } else {
                target = FP_WATER;   /* the dilution path */
            }
            sand_set(s, x, y, target);
        }
    }

    for (int y = FP_H - 12; y < FP_H - 3; y++) {
        for (int x = 2; x < FP_W - 2; x++) {
            sand_set(s, x, y, FP_ACID);
        }
    }

    /* Alternating rows put exactly two steam in every 2x2, which is what
     * step_one_acid_rain_cell() demands - so the 4x4 scan bd esp32c6-va6
     * wants to optimise actually runs here. */
    for (int y = 6; y < 18; y++) {
        for (int x = 4; x < 44; x++) {
            sand_set(s, x, y, (y & 1) ? FP_STEAM : FP_GAS);
        }
    }
}

/* SNOW, which no scene placed and none produced either - unlike smoke, which
 * fire at least makes in scene_fire_gas. Three issues are queued against it
 * (esp32c6-bl4, -tov, -l4s), all changing melting, cold reach or conversion,
 * and today each would get "identical to baseline" and mean nothing by it.
 *
 * Four bays under one smoke band, walled off in stone so they cannot pour
 * into each other. Each pending change targets a bay that is INERT today -
 * which is what lets this scene fail when they land. */
static void scene_snow_thaw(sand_t *s)
{
    sand_set_soak(s, SAND_SOAK_PER_MATERIAL);

    for (int x = 0; x < FP_W; x++) {
        sand_set(s, x, FP_H - 1, FP_STONE);
    }
    for (int x = 15; x < FP_W; x += 16) {
        for (int y = 40; y < FP_H - 1; y++) {
            sand_set(s, x, y, FP_STONE);
        }
    }

    for (int y = FP_H - 8; y < FP_H - 1; y++) {
        for (int x = 0; x < 15; x++) {
            sand_set(s, x, y, FP_WATER);      /* thaws=4 - melts today */
        }
        for (int x = 16; x < 31; x++) {
            sand_set(s, x, y, FP_WET_DIRT);   /* inert today; bd esp32c6-bl4 */
        }
    }

    /* Heat held FIVE cells from the snow through a conductor, because today
     * chilling only ever reaches an immediate neighbour - bd esp32c6-tov
     * would let the cold travel, and that shows up here as a different row.
     * Lava rather than a hot pane alone so the source is still hot at 300. */
    for (int x = 32; x < 47; x++) {
        sand_set(s, x, FP_H - 2, FP_LAVA);
        sand_set(s, x, FP_H - 3, FP_HOT_GLASS);
        for (int y = FP_H - 7; y < FP_H - 3; y++) {
            sand_set(s, x, y, FP_STONE);
        }
    }

    /* Settled snow resting ON something convertible: inert today, and what
     * bd esp32c6-l4s would turn to ice. Ice below it covers MATX_ICE's own
     * chills and thaws, which nothing else here reaches. */
    for (int x = 48; x < FP_W; x++) {
        for (int y = FP_H - 4; y < FP_H - 1; y++) {
            sand_set(s, x, y, FP_STONE);
        }
        for (int y = FP_H - 7; y < FP_H - 4; y++) {
            sand_set(s, x, y, FP_ICE);
        }
    }

    for (int y = FP_H - 16; y < FP_H - 8; y++) {
        for (int x = 0; x < FP_W; x++) {
            if (x % 16 != 15) {
                sand_set(s, x, y, FP_SNOW);
            }
        }
    }

    /* Smoke warms (28) whatever it touches, and it starts on the snow. */
    for (int y = FP_H - 20; y < FP_H - 16; y++) {
        for (int x = 0; x < FP_W; x++) {
            sand_set(s, x, y, FP_SMOKE);
        }
    }
}

/* A QUIET snow bank - the only shape that can show the crust rule.
 *
 * scene_snow_thaw cannot: it melts and drifts all window, so its blocks never
 * rest and a rest-gated rule correctly never fires. Tried, and its hash came
 * back identical with sleeping on and the roll forced.
 *
 * Snow on stone, nothing else. Roll forced for scene_sealed_lava's reason: at
 * 1 in 65536 a bank crusts over minutes, and 300 steps would show nothing. */
static void scene_snow_crust(sand_t *s)
{
    sand_set_crust(s, 4);

    for (int x = 0; x < FP_W; x++) {
        sand_set(s, x, FP_H - 1, FP_STONE);
    }
    for (int y = FP_H - 9; y < FP_H - 1; y++) {
        for (int x = 4; x < FP_W - 4; x++) {
            sand_set(s, x, y, FP_SNOW);
        }
    }
}


/* A KEG STANDING IN WATER: exercises every soak/dry/convert rule
 * gunpowder owns, including its explosive exit - the coverage this tool
 * otherwise misses for gunpowder entirely. Walled, because an unwalled
 * powder slides off an open ledge and loose water spreads away faster
 * than a keg soaks, which would leave nothing to observe. */
static void scene_powder_keg(sand_t *s)
{
    /* RATES FORCED, and deliberately not the shipped ones. A keg soaks at 2 in
     * 256 and converts once in 256 steps, so at 300 steps a scene left on the
     * shipped tuning would not even saturate, let alone reach oil - it would
     * hash the soak path and silently cover nothing else. These make the
     * MECHANISM reachable inside the window; the rate itself is tuning and is
     * pinned by unit tests, where re-pegging it does not move eleven other
     * rows. */
    sand_set_soak(s, 60);
    sand_set_soak_convert(s, 1);

    for (int x = 0; x < FP_W; x++) {
        sand_set(s, x, FP_H - 1, FP_STONE);
    }
    for (int y = 40; y < FP_H - 1; y++) {
        sand_set(s, 10, y, FP_STONE);
        sand_set(s, 30, y, FP_STONE);
    }
    for (int y = FP_H - 10; y < FP_H - 1; y++) {
        for (int x = 11; x < 30; x++) {
            sand_set(s, x, y, FP_POWDER);
        }
    }
    for (int y = FP_H - 16; y < FP_H - 10; y++) {
        for (int x = 11; x < 30; x++) {
            sand_set(s, x, y, FP_WATER);
        }
    }

    /* A dry half too, so the row also pins that dry powder does NOT convert -
     * the other half of what soaked_to promises. */
    for (int y = FP_H - 10; y < FP_H - 1; y++) {
        for (int x = 40; x < 58; x++) {
            sand_set(s, x, y, FP_POWDER);
        }
    }
}

/* Snow resting on DRY SAND and DRY DIRT, which nothing else here presents it
 * with: snow_thaw stands its bank on water, saturated dirt, stone and ice,
 * and snow_crust stands it on stone alone. A dry powder is a foreign face
 * like any other, so seeding rolls against it, and chills reaches it - and
 * the two are measured to be snow's dearest partners on a 380-pairing
 * arena. */
static void scene_snow_earth(sand_t *s)
{
    sand_set_crust(s, 4);

    for (int x = 0; x < FP_W; x++) {
        sand_set(s, x, FP_H - 1, FP_STONE);
    }
    for (int y = FP_H - 14; y < FP_H - 1; y++) {
        for (int x = 0; x < FP_W; x++) {
            sand_set(s, x, y, ((x / 8) & 1) ? FP_DIRT : FP_SAND);
        }
    }
    for (int y = FP_H - 24; y < FP_H - 14; y++) {
        for (int x = 4; x < FP_W - 4; x++) {
            sand_set(s, x, y, FP_SNOW);
        }
    }
}

/* Acid and lava on GREENERY, which no row here has ever put them near, and
 * Root <- Acid, Plant <- Lava and Leaf <- Lava are three of the six dearest
 * interactions on the same arena.
 *
 * Painted rather than grown: a fingerprint scene has one board and no way to
 * pour at a later step, so a grown bed would still be sprouting when the
 * acid was long spent. The wall keeps the two from quenching each other. */
static void scene_plant_ruin(sand_t *s)
{
    sand_set_soak(s, SAND_SOAK_PER_MATERIAL);

    for (int x = 0; x < FP_W; x++) {
        sand_set(s, x, FP_H - 1, FP_STONE);
    }
    for (int y = FP_H - 16; y < FP_H - 1; y++) {
        for (int x = 0; x < FP_W; x++) {
            sand_set(s, x, y, FP_WET_DIRT);
        }
    }
    for (int x = 4; x < FP_W; x += 8) {
        for (int y = FP_H - 14; y < FP_H - 4; y++) {
            sand_set(s, x, y, FP_ROOT);
        }
        for (int y = FP_H - 22; y < FP_H - 16; y++) {
            sand_set(s, x, y, FP_PLANT);
            sand_set(s, x - 1, y, FP_LEAF);
            sand_set(s, x + 1, y, FP_LEAF);
        }
    }

    for (int y = 0; y < FP_H; y++) {
        for (int x = FP_W / 2 - 1; x <= FP_W / 2 + 1; x++) {
            sand_set(s, x, y, FP_STONE);
        }
    }

    for (int y = FP_H - 30; y < FP_H - 24; y++) {
        for (int x = 0; x < FP_W / 2 - 1; x++) {
            sand_set(s, x, y, FP_ACID);
        }
        for (int x = FP_W / 2 + 2; x < FP_W; x++) {
            sand_set(s, x, y, FP_LAVA);
        }
    }
}

/* GRAVITY IS PER SCENE, and the six original rows keep the straight-down
 * vector they were baselined with - their hashes must not move.
 *
 * A vertical vector is the ONE case that leaves the perpendicular
 * horizontal: perp is ring_dir(i_stable + 2), so py == 0 only at (0, +-1).
 * Landscape and every diagonal give py != 0, where equalise_gas() takes a
 * different path and the row skip has a branch that runs nowhere else -
 * corrupting which left --check reporting "identical". */
static const struct {
    const char *name;
    scene_fn    build;
    uint32_t    seed;
    int         gx;
    int         gy;
    int         sleeping;   /* 0 for every original row - see the two at the end */
} SCENES[] = {
    { "dry_fall",    scene_dry_fall,    7u,  0,    1000, 0 },
    { "water_pool",  scene_water_pool,  11u, 0,    1000, 0 },
    { "lava_quench", scene_lava_quench, 23u, 0,    1000, 0 },
    { "fire_gas",    scene_fire_gas,    31u, 0,    1000, 0 },
    { "sealed_lava", scene_sealed_lava, 41u, 0,    1000, 0 },
    { "wet_earth",   scene_wet_earth,   53u, 0,    1000, 0 },

    /* Same builders, held sideways and cornerwise. */
    { "gas_land",    scene_fire_gas,    31u, 1000, 0, 0 },
    { "water_diag",  scene_water_pool,  11u, 1000, 1000, 0 },

    /* The only row here in which anything grows - see the builder. */
    { "plant_bed",   scene_plant_bed,   11u, 0,    1000, 0 },

    /* The only row here that puts acid on the board at all. */
    { "acid_bath",   scene_acid_bath,   67u, 0,    1000, 0 },

    /* Likewise snow, ice and deliberately-placed smoke. */
    { "snow_thaw",   scene_snow_thaw,   71u, 0,    1000, 0 },

    /* SLEEPING ON, which every row above leaves OFF though the app runs with it
     * ON. cell_settled() answers false when block_state is NULL, so no
     * settled-gated rule could fire anywhere here (bd esp32c6-1x9).
     *
     * EXTRA rows, not switched originals, so no existing hash moves - and the
     * pair is the point: block sleep claims to change nothing, and two rows
     * differing only in sleeping are what check that. They come back
     * byte-identical to their awake twins, which is that claim measured. */
    { "snow_asleep", scene_snow_thaw,   71u, 0,    1000, 1 },
    { "pool_asleep", scene_water_pool,  11u, 0,    1000, 1 },

    /* The one combination neither half above reaches: held sideways WITH
     * the blocks awake to it. Block shape is asymmetric - 32 across by 64
     * along - so which cells a block calls settled depends on the gravity
     * axis, and every sleeping row here pointed the same way. A change to
     * that shape lands here and nowhere else in this table. */
    { "pool_land",   scene_water_pool,  11u, 1000, 0,    1 },

    /* And the one row where a SETTLED-gated rule actually fires - see the
     * builder for why the rate is forced. */
    { "snow_crust",  scene_snow_crust,  71u, 0,    1000, 1 },

    /* The gunpowder row - see the builder for what was invisible without it. */
    { "powder_keg",  scene_powder_keg,  29u, 0,    1000, 0 },

    /* Two pairings the rows above never put in contact - see each builder.
     * Sleeping ON for the snow row, since seeding is settled-gated. */
    { "snow_earth",  scene_snow_earth,  71u, 0,    1000, 1 },
    { "plant_ruin",  scene_plant_ruin,  67u, 0,    1000, 0 },
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
        uint8_t *blocks = NULL;
        if (SCENES[i].sleeping) {
            blocks = calloc((size_t)s.block_cols * (size_t)s.block_rows, 1);
            if (blocks == NULL) {
                fprintf(stderr, "grid_fingerprint: out of memory for blocks\n");
                free(cells);
                free(impulses);
                return 1;
            }
            sand_enable_sleeping(&s, blocks);
        }
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
        free(blocks);
    }

    return 0;
}
