/*=============================================================================
 * suite_sand_split.h - the single-step decomposition harness.
 *
 * PERMANENT, unlike the gates it drives. Pass gates are scaffolding that
 * scripts/strip-pass-gates.py removes at the end of a round; this file is not
 * in that script's list, so the harness survives and the next round writes
 * only its gates and its rows. That is the whole reason it lives here rather
 * than in suite_sand_perf.c, where the strip would take it with everything
 * else.
 *
 * Empty when CONFIG_LAUNCHER_SAND_PASS_GATES is off, so a build with no gates
 * carries no unused functions.
 *
 * See docs/sand/Perf-Instruments.md for when to reach for this and for the
 * three rules it enforces - identical boards per configuration, min over
 * repeats, and a scene being its setup plus its builder plus its pours.
 *===========================================================================*/
#pragma once

/*---------------------------------------------------------------------------
 * The single-step decomposition harness.
 *
 * Every gated round needs the same five steps: rebuild the scene, warm it with
 * everything on, flip ONE gate, time exactly one step, take the min. Doing it
 * by hand per round is how two techniques drift into measuring different
 * scenes, which is the bug water_scene_single_step_us()'s own comment was
 * written to prevent for one scene. This does it for any of them.
 *
 * WHY REBUILD PER REPEAT rather than stepping on: a gate that is off changes
 * the board, so twenty steps with a pass disabled measure a DIFFERENT
 * simulation. Rebuilding means every configuration sees a byte-identical
 * board at the moment it is timed, which is what makes the phases add up to
 * the whole rather than merely being upper bounds.
 *
 * MIN, NEVER MEAN: the interesting number is the cost with nothing else
 * intruding, and an RTOS tick landing inside a timed step only ever adds.
 *
 * A NULL gate times the same step with nothing flipped - the baseline every
 * phase is subtracted from. See docs/sand/Perf-Instruments.md.
 *-------------------------------------------------------------------------*/
typedef struct {
    const char *name;
    /* EVERYTHING THE BUDGET ROW DOES, not just its builder. The first use of
     * this harness measured a soak stage at 1 us because the row it copied
     * calls sand_set_soak() BEFORE the builder and the harness did not - so
     * nothing soaked, and the gate had nothing to skip. A scene is its setup
     * plus its builder plus whatever it does mid-settle; leave any of that
     * out and the number is of a different scene. */
    void      (*setup)(sand_t *s);   /* NULL when the scene needs none */
    void      (*build)(sand_t *s);   /* suite_sand_scenes.h shape */
    void      (*during)(sand_t *s, int step);  /* pours, rain - NULL if none */
    uint32_t    seed;
    int         sgx, sgy;            /* gravity while settling */
    int         gx, gy;              /* gravity for the timed step - differs
                                        from the above for a flip scene, whose
                                        whole cost is the turn */
    int         settle;              /* steps with everything on first */
} split_scene_t;

static int64_t split_single_step_us(const split_scene_t *sc,
                                    volatile bool *gate, int repeats)
{
    uint8_t *big    = malloc(REAL_W * REAL_H);
    uint8_t *blocks = malloc(REAL_BLOCK_COLS * REAL_BLOCK_ROWS);
    if (big == NULL || blocks == NULL) {
        free(big);
        free(blocks);
        TEST_FAIL_MESSAGE("split harness needs a grid and a block map");
    }

    int64_t best = INT64_MAX;
    for (int r = 0; r < repeats; r++) {
        sand_t real;
        sand_init(&real, big, REAL_W, REAL_H, sc->seed);
        sand_enable_sleeping(&real, blocks);
        if (sc->setup != NULL) {
            sc->setup(&real);
        }
        sc->build(&real);

        for (int i = 0; i < sc->settle; i++) {
            if (sc->during != NULL) {
                sc->during(&real, i);
            }
            sand_step(&real, sc->sgx, sc->sgy, 0);
        }

        if (gate != NULL) {
            *gate = false;
        }
        const int64_t t0 = esp_timer_get_time();
        sand_step(&real, sc->gx, sc->gy, 0);
        const int64_t dt = esp_timer_get_time() - t0;
        if (gate != NULL) {
            *gate = true;   /* restored before anything else runs */
        }

        if (dt < best) {
            best = dt;
        }
    }

    free(big);
    free(blocks);
    return best;
}

/* Baseline, then one row per gate, then the residual. Prints the shape a
 * round actually reads: what each phase costs and what share of the step it
 * is. The caller owns the gate list because WHICH gates exist is the round's
 * entire content. */
static void split_report(const split_scene_t *sc, const char *const *names,
                         volatile bool *const *gates, int n, int repeats)
{
    const int64_t whole = split_single_step_us(sc, NULL, repeats);
    ESP_LOGI("device_tests", "split %s: whole step %lld us (min of %d)",
             sc->name, (long long)whole, repeats);

    int64_t accounted = 0;
    for (int i = 0; i < n; i++) {
        const int64_t off = split_single_step_us(sc, gates[i], repeats);
        const int64_t phase = whole - off;
        accounted += phase;
        ESP_LOGI("device_tests", "split %s: %-16s %lld us  %d%%",
                 sc->name, names[i], (long long)phase,
                 whole > 0 ? (int)((phase * 100) / whole) : 0);
    }

    /* Prints, never asserts. A residual near zero means the gates PARTITION
     * the step; a large one means they do not, and that is information about
     * the gates rather than a failure.
     *
     * A NEGATIVE PHASE IS THE SAME WARNING, LOUDER: switching work OFF made
     * the step cost MORE, so something else absorbed it. Measured on this
     * harness's first run - gating the fall inside move_liquid_grain left its
     * mass to the slides, which then did more, and it read -18%. A gate
     * partitions only when the work it disables cannot be picked up elsewhere
     * in the same step: true between passes, false inside one function
     * sharing a budget across its branches. */
    ESP_LOGI("device_tests", "split %s: residual %lld us of %lld",
             sc->name, (long long)(whole - accounted), (long long)whole);
}

/* The settled-pool board, matching
 * test_turning_a_settled_pool_to_landscape_fits_in_the_frame_budget above so
 * the decomposition and the budget row cannot drift apart. The harness does
 * the settling; this only fills the pool. */
static void build_settled_pool_for_split(sand_t *s)
{
    for (int y = (REAL_H * 3) / 5; y < REAL_H; y++) {
        for (int x = 0; x < REAL_W; x++) {
            sand_set(s, x, y, CELL_MAKE(MAT_WATER, MASS_MAX));
        }
    }
}

#if CONFIG_LAUNCHER_SAND_PASS_GATES
/* WHERE THE SETTLED POOL'S MOVE WORK GOES - bd esp32c6-2u7, which asks what
 * step_one_grain() spends its time on and how much is dispatch rather than
 * the moves themselves.
 *
 * Host counters already said 94% of move_liquid_grain() calls on this board
 * move nothing (1,297,446 of 1,380,945). What they cannot say is what that
 * costs, so these four gates split the call from its three phases: reaching
 * move_liquid_grain at all, the density swap, the fall, and the two slides.
 *
 * Prints; asserts nothing. A decomposition is a measurement, and pinning a
 * budget to one would make every future change to the split a test failure. */
static void test_the_settled_pool_move_work_splits_into_its_phases(void)
{
    static const split_scene_t pool = {
        .name = "settled pool", .build = build_settled_pool_for_split,
        .seed = 17u,   /* no setup and no pours - see its budget row */
        .sgx = 0, .sgy = 1000,      /* settle upright */
        .gx = 1000, .gy = 0,        /* then turn it on its side */
        .settle = 300,
    };
    static const char *const names[] = { "sweep_move", "liq_sink",
                                         "liq_down", "liq_slides" };
    static volatile bool *const gates[] = { &sand_step_gate_sweep_move,
                                            &sand_step_gate_liq_sink,
                                            &sand_step_gate_liq_down,
                                            &sand_step_gate_liq_slides };
    split_report(&pool, names, gates, 4, 3);
}
#endif

#endif
