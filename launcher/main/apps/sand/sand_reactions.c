/*=============================================================================
 * sand_reactions - fire chemistry: ignites fuel, spreads, is extinguished,
 * burns out.
 *
 * Two materials burn: fire itself, and the ember a log of wood chars into,
 * both dispatched by reaction_t.burns (material.h) rather than a
 * CELL_MATERIAL(c) == MAT_FIRE check, which would silently ignore ember.
 *
 * WHY WOOD CHARS INTO AN EMBER RATHER THAN IGNITING STRAIGHT TO FIRE: fire
 * is KIND_GAS, so a wood cell that became fire would float away on the
 * very next sand_step_gas() pass, leaving a hole where the log was - the
 * burn would stall or race depending on nothing the player can see.
 * MAT_EMBER splits the job instead: the ember (KIND_STATIC) stays exactly
 * where the log was, igniting neighbours and decaying in place, while an
 * ordinary, separate MAT_FIRE (reaction_t.flare) licks up off it purely
 * for looks and to reach fuel stacked above, rising through its own
 * unrelated sand_step_gas() pass.
 *
 * Nothing here ever relocates a cell - every mutation is in place - so
 * this pass needs none of sand_step_gas()'s reversed-sweep machinery,
 * just a fixed scan order. That scan reads row[x] fresh at every index,
 * so a cell ignited earlier in THIS SAME pass (ahead of the scan pointer)
 * gets its own turn to spread further within the same step, while one
 * ignited behind the pointer waits for the next sand_step() - a
 * deliberate choice (explosion-like spread through a connected pocket of
 * fuel, not a slow creep), not an oversight, and true only for pockets
 * laid out ahead of the scan's own fixed row-major direction.
 *
 * Ember, at density 150, is essentially never smothered - smothered()
 * needs all four neighbours STRICTLY denser, and only stone (200)
 * qualifies, so burying a log in sand will not put it out. That is an
 * accepted limitation, not a bug to chase: only decay or water ends an
 * ember.
 *
 * Quenching (water touching a burning cell) produces MAT_STEAM at a cost
 * to the quenching liquid's own mass (pay_quench_cost()); simply running
 * out of life produces MAT_SMOKE instead. They were one material at
 * first: physically almost identical, but wrong on the SCREEN - a fire
 * burning out in mid-air, nowhere near water, puffing bright kettle-steam
 * reads as a bug to anyone who can see there was nothing there to boil.
 * The split is mostly a palette difference (cool/bright for steam,
 * warm/dim for smoke).
 *
 * THE BOILER: fire never crosses stone directly - conduct_heat() conducts
 * heat through it instead, boiling a liquid or igniting fuel on the far
 * side, never creating fire in empty space, which is what keeps a sealed
 * box sealed. The alternative (a can_enter() special case letting fire
 * pass through) was rejected: it would leak fire through every sealed
 * stone container. Boiling happens at the heat source; the steam bubbles
 * out on its own through try_bubble() (sand_gas.c) - an earlier version
 * had to walk the pool upward and boil the LAST cell instead, back when
 * steam had no way to rise past the liquid above it, but that workaround
 * came back out once try_bubble() lifted the limitation it was dodging.
 * conduct_heat()'s own reach has to attenuate with thickness rather than
 * stop at one conductor cell: the pour brush cannot draw a wall one cell
 * thick, so a reach-of-one boiler was unbuildable on the device despite
 * reading as a clean rule in isolation. CONDUCT_REACH bounds the walk's
 * cost but must stay generous enough that attenuation, not the cap, is
 * what limits depth in any scene the brush can actually draw.
 *===========================================================================*/

#include "sand_priv.h"
#include "reaction_doc.h"

static const int reaction_dirs[4][2] = {
    {0, -1},
    {0, 1},
    {-1, 0},
    {1, 0},
};

/* PAIR_BITS - classifies neighbour probes, replacing s->heat_mask/s->wet_mask
 * and adding three more. */


/* HONESTY: All but PAIR_DENSER use `theirs`. See top comment and
 * docs/Sand/Reaction-Table.md. */


/* Consistent lookup shape for every consumer. PAIR_DENSER is genuinely
 * pairwise. */

/* See pair_theirs_bits() for `mine`-less calls. */
#define PAIR_HEAT_RESPONSIVE (1u << 0) /* theirs could pass try_heat_transform()'s first two gates - was heat_mask */
#define PAIR_WETS            (1u << 1) /* theirs is a liquid whose reaction row wets - was wet_mask */
#define PAIR_IGNITABLE       (1u << 2) /* theirs has a nonzero flammability - try_ignite()'s own first reject */
#define PAIR_QUENCHES        (1u << 3) /* theirs is a liquid that is neither fuel nor a heat source - neighbor_quenches() */
#define PAIR_DISSOLVABLE     (1u << 4) /* theirs has a nonzero dissolvable - step_one_dissolver_cell()'s own reject */
static uint8_t pair_bits[MATERIAL_MAX][MATERIAL_MAX];

/* Reads theirs-only bits. Used by try_heat_transform(), step_one_cold_cell(),
 * conduct_heat(). MAT_EMPTY stores theirs-only bits. Avoid PAIR_DENSER. */
static inline uint8_t
pair_theirs_bits(uint8_t theirs) {
    return pair_bits[MAT_EMPTY][theirs];
}

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

static inline bool
neighbor_quenches(const sand_t* s, int nx, int ny, int w, int h) {
    if ((unsigned)nx >= (unsigned)w || (unsigned)ny >= (unsigned)h) {
        return false;
    }
    const cell_t n = s->cells[(size_t)ny * (size_t)w + (size_t)nx];
    if (CELL_IS_EMPTY(n)) {
        return false;
    }
    return (pair_theirs_bits(CELL_MATERIAL(n)) & PAIR_QUENCHES) != 0;
}


/* Checks burial; returns false on failure. No cover_mask()/covered_at().
 * Burial skips rotation, side. */
static inline bool
smothered(const sand_t* s, int x, int y, int w, int h, uint8_t density) {
    for (int d = 0; d < 4; d++) {
        if (!neighbor_smothers(s, x + reaction_dirs[d][0], y + reaction_dirs[d][1], w, h, density)) {
            return false;
        }
    }
    return true;
}

static inline bool
touches_air(const sand_t* s, int x, int y, int w, int h) {
    for (int d = 0; d < 4; d++) {
        const int nx = x + reaction_dirs[d][0];
        const int ny = y + reaction_dirs[d][1];
        if ((unsigned)nx >= (unsigned)w || (unsigned)ny >= (unsigned)h) {
            continue;
        }
        const cell_t n = s->cells[(size_t)ny * (size_t)w + (size_t)nx];
        if (CELL_IS_EMPTY(n) || material_of(n)->kind == KIND_GAS) {
            return true;
        }
    }
    return false;
}

static inline void pay_quench_cost(sand_t* s, int nx, int ny, int w);

static void crack_run(sand_t* s, int x, int y, int w, int h, material_id_t from, material_id_t into);

static inline bool emit_into_empty_neighbor(sand_t* s, int x, int y, int w, int h, uint8_t spec);

static inline __attribute__((always_inline)) bool try_heat_transform_given(sand_t* s, int nx, int ny, int w, int h,
                                                                           size_t at, cell_t n);

static inline cell_t soil_set_moisture(cell_t c, uint8_t new_moisture, uint8_t nearby_moisture);

#define HEAT_FLAW_CLUMP 5

/* FORCED INLINE, and that is a performance fix rather than a
 * preference. */


/* Flaw/spoils branches grow function; unmeasured. Check cost if ratio
 * changes. */




/* Inline costs more than call on this chip. Forces four call sites. Capture
 * pending. */

/* Revert if host disagrees */

/* Turns (nx, ny) into heats_to if in bounds and roll succeeds. Heat without
 * burning. */

/* Sand into glass is the only use. Kept separate from try_ignite_given(). */



/* Wrapper checks before calling core; returns change status. */
static inline __attribute__((always_inline)) bool
try_heat_transform(sand_t* s, int nx, int ny, int w, int h) {
    if ((unsigned)nx >= (unsigned)w || (unsigned)ny >= (unsigned)h) {
        return false;
    }
    const size_t at = (size_t)ny * (size_t)w + (size_t)nx;
    const cell_t n = s->cells[at];
    if (CELL_IS_EMPTY(n)) {
        return false;
    }
    if ((pair_theirs_bits(CELL_MATERIAL(n)) & PAIR_HEAT_RESPONSIVE) == 0) {
        return false;
    }
    return try_heat_transform_given(s, nx, ny, w, h, at, n);
}

/* FORCED INLINE for 2 sites; wrapper perf. Shared walk confirms
 * step_one_burning_cell() shrink. */
static inline __attribute__((always_inline)) bool
try_heat_transform_given(sand_t* s, int nx, int ny, int w, int h, size_t at, cell_t n) {
    const reaction_t* r = reaction_of(n);

    /* Material climbs, transforms at top. Triggered by burning cell or
     * conductor. */
    if (r->heat_ramp != 0) {
        /* SHOCK: hot-to-cold, cracks. Mirror step_one_cold_cell(). Check
         * before ramp. */
        REACTION_DOC(shatters_to, "if warmed while badly chilled");
        if (r->shatters_to != 0 && CELL_VARIANT(n) <= SAND_SHOCK_COLD) {
            crack_run(s, nx, ny, w, h, (material_id_t)CELL_MATERIAL(n), (material_id_t)r->shatters_to);
            return true;
        }
        if ((int)(rng_next(&s->rng) & 0xFF) >= r->heat_ramp) {
            return false;
        }
        const uint8_t heat = CELL_VARIANT(n);
        if (heat + 1 >= MATERIAL_VARIANTS) {
            if (r->heats_to == 0) {
                return false; /* banks heat but melts into nothing */
            }
            place_reacted(s, nx, ny, at, (material_id_t)r->heats_to);
            return true;
        }
        s->cells[at] = CELL_MAKE(CELL_MATERIAL(n), heat + 1);
        s->may_have_temperature = true;
        mark_rows(s, ny, ny);
        wake_block_and_neighbors(s, nx, ny);
        return true;
    }

    if (r->heats_to == 0 || r->heat_chance == 0) {
        return false;
    }
    /* A LIT CELL `heats_to` GUNPOWDER_LIT_CELL. `explodes != 0` avoids extra
     * compare. */
    if (r->explodes != 0 && cell_code(n) >= r->lit_from) {
        return false;
    }
    if ((int)(rng_next(&s->rng) & 0xFF) >= r->heat_chance) {
        return false;
    }

    /* WET EARTH FIRST. `dries != 0` checks. CELL_MOISTURE() confirms. Sand,
     * glass untouched. Two reads on success. Saturated dirt needs
     * SOIL_MOISTURE_MAX + 1 for metal with steam. */
    if (r->dries != 0 && moisture_of(n, r) != 0) {
        /* See reaction_t.spoils_to (material.h). Spoil pre-empts moisture
         * reduction. */


        /* Dirt dries ambiently, so cell may be below SOIL_MOISTURE_MAX before
         * HEAT. */



        /* Wet ore cracking on first contact, not after warning. */
        REACTION_DOC(spoils_to, "if wet when heat reaches it");
        if (r->spoils_to != 0 &&
            (int)(rng_next(&s->rng) & 0xFF) < r->spoils_chance) {
            place_reacted(s, nx, ny, at, (material_id_t)r->spoils_to);
            return true;
        }
        /* No neighbour to bias from - see soil_set_moisture() comment. */
        s->cells[at] = soil_set_moisture(n, (uint8_t)(moisture_of(n, r) - 1), 0);
        mark_rows(s, ny, ny);
        wake_block_and_neighbors(s, nx, ny);
        emit_into_empty_neighbor(s, nx, ny, w, h, MAT_STEAM);
        return true;
    }

    material_id_t yield = (material_id_t)r->heats_to;

    if (r->flaw_to != 0) {
        if (s->heat_flaw_seq % HEAT_FLAW_CLUMP == 0) {
            s->heat_flaw_is_flawed =
                (int)(rng_next(&s->rng) & 0xFF) < r->flaw_chance;
        }
        s->heat_flaw_seq++;
        if (s->heat_flaw_is_flawed) {
            yield = (material_id_t)r->flaw_to;
        }
    }

    place_reacted(s, nx, ny, at, yield);
    return true;
}

#define CRACK_MAX 256

/* Cracks convert up to CRACK_MAX cells, leaving CULLET sand. */
static inline void
place_cracked(sand_t* s, int x, int y, size_t at, material_id_t into) {
    if (into == MAT_SAND) {
        place_cell(s, x, y, at, CELL_MAKE(into, (uint8_t)(SAND_CULLET_BASE + rng_below(&s->rng, SAND_CULLET_SHADES))));
        return;
    }
    place_reacted(s, x, y, at, into);
}

static void
crack_run(sand_t* s, int x, int y, int w, int h, material_id_t from, material_id_t into) {
    uint16_t frontier[CRACK_MAX];
    int top = 0, done = 0;

    const size_t first = (size_t)y * (size_t)w + (size_t)x;
    place_cracked(s, x, y, first, into);
    frontier[top++] = (uint16_t)first;

    while (top > 0 && done < CRACK_MAX) {
        const uint16_t at = frontier[--top];
        const int cx = (int)(at % (unsigned)w);
        const int cy = (int)(at / (unsigned)w);
        done++;

        for (int d = 0; d < 4; d++) {
            const int nx = cx + reaction_dirs[d][0];
            const int ny = cy + reaction_dirs[d][1];
            if ((unsigned)nx >= (unsigned)w || (unsigned)ny >= (unsigned)h) {
                continue;
            }
            const size_t nat = (size_t)ny * (size_t)w + (size_t)nx;
            if (CELL_MATERIAL(s->cells[nat]) != from) {
                continue;
            }
            place_cracked(s, nx, ny, nat, into);
            if (top < CRACK_MAX) {
                frontier[top++] = (uint16_t)nat;
            }
        }
    }
}

/* Walks lava-cooling chain, freezes neighbours holding same liquid. */



/* See KIND_LIQUID gate */


/* Iterative, not recursive. Stack limit on 368 KB RAM. */

static void
cool_off_chain(sand_t* s, int x, int y, int w, int h, uint8_t product, int chance) {
    int cx = x, cy = y;
    for (int link = 0; link < SAND_LAVA_COOLOFF_MAX_CHAIN; link++) {
        if (chance == 0 || (int)(rng_next(&s->rng) & 0xFF) >= chance) {
            return;
        }
        /* Collect eligible neighbours first for uniform pick. */
        int cand_x[4], cand_y[4], n_cand = 0;
        for (int d = 0; d < 4; d++) {
            const int nx = cx + reaction_dirs[d][0];
            const int ny = cy + reaction_dirs[d][1];
            if ((unsigned)nx >= (unsigned)w || (unsigned)ny >= (unsigned)h) {
                continue;
            }
            const cell_t n = s->cells[(size_t)ny * (size_t)w + (size_t)nx];
            if (CELL_IS_EMPTY(n)) {
                continue;
            }
            if (material_of(n)->kind != KIND_LIQUID || reaction_of(n)->quench_to != product) {
                continue;
            }
            cand_x[n_cand] = nx;
            cand_y[n_cand] = ny;
            n_cand++;
        }
        if (n_cand == 0) {
            return;
        }
        const int pick = rng_below(&s->rng, n_cand);
        cx = cand_x[pick];
        cy = cand_y[pick];
        place_reacted(s, cx, cy, (size_t)cy * (size_t)w + (size_t)cx, product);
    }
}

/* `#define` used for materials with `dries != 0` */
#define SOIL_PERCOLATE_CHANCE 15

/* Dry front marks zero moisture, new tone, skips flat soil. See
 * CELL_WITH_MOISTURE(). */

/* nearby_moisture: moisture level at last watering hand-off or root sink */

/* Pass 0 for no neighbour; cell dries to same look. */

/* SCALED THROUGH `dry_tone_from_moisture()` - `nearby_moisture` equals tone
 * if ranges match. */

static inline cell_t soil_dry_out(cell_t c, uint8_t nearby_moisture)
{
    /* Handles any material; avoids MAT_EXTENDED index error. Byte-identical
     * to CELL_SOIL() for dirt. */
    const reaction_t* r = reaction_of(c);
    return soil_cell(c, dry_tone_from_moisture(nearby_moisture, r), 0, r);
}

_Static_assert(SOIL_DRY_TONES - 1 == SOIL_MOISTURE_MAX,
               "dry_tone_from_moisture() is identity for dirt only because "
               "these two ranges are the same width - the fingerprint gate "
               "is what actually proves soil_dry_out() stayed byte-"
               "identical for dirt after this stopped being a direct read");

/* All sites changing soil moisture call this instead of CELL_WITH_MOISTURE().
 * Calls soil_dry_out() at zero moisture, adjusted by `nearby_moisture`. */
static inline cell_t soil_set_moisture(cell_t c, uint8_t new_moisture, uint8_t nearby_moisture)
{
    return new_moisture != 0
               ? with_moisture(c, new_moisture, reaction_of(c))
               : soil_dry_out(c, nearby_moisture);
}

/* Splits cell for input/output. Soaks UNIT, transforms or increases variant.
 * Drying decreases variant. Returns true if wet/near liquid. Prevents
 * `may_have_moisture`. Activated by SOAKING side. */
static bool
step_one_soaking_cell(sand_t* s, uint8_t* row, int x, int y, int w, int h, const reaction_t* r) {
    const cell_t c = row[x];
    const uint8_t held = moisture_of(c, r);

    /* `>=` used, not `==`. Short-circuits on `soaked_to != 0`. */
    REACTION_DOC(soaked_to, "once fully saturated, at a per-step chance");
    if (r->soaked_to != 0 && held >= r->moist_max && (int)(rng_next(&s->rng) & 0xFF) < r->soaked_chance) {
        const size_t at = (size_t)y * (size_t)w + (size_t)x;
        place_reacted(s, x, y, at, r->soaked_to);
        return true;
    }

    bool beside_liquid = false;

    const int soaks = (s->soak >= 0) ? s->soak : r->soaks;

    if (soaks != 0 && r->soaks != 0 && s->may_have_liquid) {
        for (int d = 0; d < 4; d++) {
            const int nx = x + reaction_dirs[d][0];
            const int ny = y + reaction_dirs[d][1];
            if ((unsigned)nx >= (unsigned)w || (unsigned)ny >= (unsigned)h) {
                continue;
            }
            const size_t nat = (size_t)ny * (size_t)w + (size_t)nx;
            const cell_t n = s->cells[nat];
            /* PAIR_WETS merges wets test into shift-and-test, covering
             * CELL_IS_EMPTY() without materials[] or reaction_of(n). */
            if ((pair_theirs_bits(CELL_MATERIAL(n)) & PAIR_WETS) == 0) {
                continue;
            }
            beside_liquid = true;

            if ((int)(rng_next(&s->rng) & 0xFF) >= soaks) {
                continue;
            }
            /* The liquid pays for what was taken out of it. */
            pay_quench_cost(s, nx, ny, w);

            if (r->soaks_to != 0) {
                s->cells[(size_t)y * (size_t)w + (size_t)x] =
                    soil_cell(CELL_MAKE(r->soaks_to, 0), 0, 1,
                             &reactions[r->soaks_to]);
                latch_content_flags(s, s->cells[(size_t)y * (size_t)w + (size_t)x]);
                mark_rows(s, y, y);
                wake_block_and_neighbors(s, x, y);
                return true;
            }
            if (held < r->moist_max) {
                row[x] = with_moisture(c, (uint8_t)(held + 1), r);
                mark_rows(s, y, y);
                wake_block_and_neighbors(s, x, y);
            }
            return true;
        }
    }

    const int spread = soaks;

    /* `dries` marks wet; `spread` checked for sinking. */
    if (r->dries != 0 && held >= 2 && spread != 0 && (int)(rng_next(&s->rng) & 0xFF) < spread) {
        for (int d = 0; d < 4; d++) {
            const int nx = x + reaction_dirs[d][0];
            const int ny = y + reaction_dirs[d][1];
            if ((unsigned)nx >= (unsigned)w || (unsigned)ny >= (unsigned)h) {
                continue;
            }
            const size_t nat = (size_t)ny * (size_t)w + (size_t)nx;
            const cell_t n = s->cells[nat];
            if (CELL_IS_EMPTY(n)) {
                continue;
            }
            const reaction_t* nr = reaction_of(n);
            if (nr->soaks == 0) {
                continue; /* not something that drinks */
            }

            int give, cost, recv_m;
            if (nr->soaks_to != 0) {
                give = held / 2;
                if (give == 0) {
                    continue; /* not enough to bind a grain */
                }
                cost = give;
                recv_m = give;
                s->cells[nat] = soil_cell(CELL_MAKE(nr->soaks_to, 0), 0,
                                         (uint8_t)give, &reactions[nr->soaks_to]);
                latch_content_flags(s, s->cells[nat]);
            } else if (same_species(n, c) && !cell_is_burning(n)) {
                /* Moisture_of() reads lit fuse as 0. Gap calc overwrites lit
                 * byte. */
                give = (held - moisture_of(n, nr)) / 2;
                if (give == 0) {
                    continue; /* already even with this one */
                }
                recv_m = moisture_of(n, nr) + give;
                s->cells[nat] = with_moisture(n, (uint8_t)recv_m, nr);
                cost = give;
            } else {
                continue;
            }

            row[x] = soil_set_moisture(c, (uint8_t)(held - cost), (uint8_t)recv_m);
            mark_rows(s, y, y);
            mark_rows(s, ny, ny);
            wake_block_and_neighbors(s, x, y);
            wake_block_and_neighbors(s, nx, ny);
            return true;
        }
    }

    /* Water percolates downhill, not just diffuses. */




    /* A fixed direction would advance a flat, evenly-damped sheet; a
     * wandering direction driving a real share down through the soil
     * instead produces fingers that split when a wet cell sends half one
     * way and half the other, and merge where two meet - closer to what
     * water actually does in sand. */

    /* Random number advances RNG for wet cells, shifts decisions. Trap
     * try_ignite() docs, once triggered. */
    if (r->dries != 0 && held != 0) {
        const int down = ring_of(s->last_load_dx, s->last_load_dy);
        int open[3], n_open = 0;
        for (int i = 0; i < 3; i++) {
            const int* fd = ring_dir(down + (i == 0 ? 0 : i == 1 ? 1 : 7));
            const int nx = x + fd[0], ny = y + fd[1];
            if ((unsigned)nx >= (unsigned)w || (unsigned)ny >= (unsigned)h) {
                continue;
            }
            const cell_t below = s->cells[(size_t)ny * (size_t)w + (size_t)nx];
            if (CELL_IS_EMPTY(below)) {
                continue;
            }
            const reaction_t* br = reaction_of(below);
            if (br->soaks == 0) {
                continue;
            }
            /* Lit fuse carve-out: prevent dousing */
            if (!cell_is_burning(below) &&
                (br->soaks_to != 0 || (br->dries != 0 && moisture_of(below, br) < br->moist_max))) {
                open[n_open++] = i;
            }
        }
        if (n_open != 0 && (int)(rng_next(&s->rng) & 0xFF) < SOIL_PERCOLATE_CHANCE) {
            const int pick = open[rng_below(&s->rng, n_open)];
            const int* fd = ring_dir(down + (pick == 0 ? 0 : pick == 1 ? 1 : 7));
            const int nx = x + fd[0], ny = y + fd[1];
            const size_t nat = (size_t)ny * (size_t)w + (size_t)nx;
            const cell_t below = s->cells[nat];
            const reaction_t* br = reaction_of(below);

            int give = (held + 1) / 2;
            int cost = give;
            int recv_m;
            if (br->soaks_to != 0) {
                give = held / 2;
                if (give == 0) {
                    return true; /* too little to bind a grain */
                }
                cost = give;
                recv_m = give;
                /* Arrives WET, so no tone of its own - the soaking
                 * branch above's own comment covers why. */
                s->cells[nat] = soil_cell(CELL_MAKE(br->soaks_to, 0), 0,
                                          (uint8_t)give, &reactions[br->soaks_to]);
                latch_content_flags(s, s->cells[nat]);
            } else {
                const int room = (int)br->moist_max - moisture_of(below, br);
                if (give > room) {
                    give = room;
                }
                recv_m = moisture_of(below, br) + give;
                s->cells[nat] = with_moisture(below, (uint8_t)recv_m, br);
                cost = give;
            }
            row[x] = soil_set_moisture(c, (uint8_t)(held - cost), (uint8_t)recv_m);
            mark_rows(s, y, y);
            mark_rows(s, ny, ny);
            wake_block_and_neighbors(s, x, y);
            wake_block_and_neighbors(s, nx, ny);
            return true;
        }
    }

    if (r->dries != 0 && held != 0 && (int)(rng_next(&s->rng) & 0xFF) < r->dries) {
        row[x] = soil_set_moisture(c, (uint8_t)(held - 1), 0);
        mark_rows(s, y, y);
        wake_block_and_neighbors(s, x, y);
        return held - 1 != 0;
    }

    /* r->dries halves wetness, prevents false "still wet." Without,
     * may_have_moisture could latch. */
    return (r->dries != 0 && held != 0) || beside_liquid;
}

/* may_have_heat_holder checks grid heat_ramp; skips heat_ramp == 0 cells. */
static void
step_one_warming_cell(sand_t* s, int x, int y, int w, int h, const reaction_t* r) {
    for (int d = 0; d < 4; d++) {
        const int nx = x + reaction_dirs[d][0];
        const int ny = y + reaction_dirs[d][1];
        if ((unsigned)nx >= (unsigned)w || (unsigned)ny >= (unsigned)h) {
            continue;
        }
        const size_t nat = (size_t)ny * (size_t)w + (size_t)nx;
        const cell_t n = s->cells[nat];
        if (CELL_IS_EMPTY(n)) {
            continue;
        }
        const reaction_t* nr = reaction_of(n);

        /* Something that BANKS heat climbs one level. */
        if (nr->heat_ramp != 0) {
            const uint8_t t = CELL_VARIANT(n);
            if (t + 1 >= MATERIAL_VARIANTS) {
                continue; /* melting is the ramp's job, not convection's */
            }
            if ((int)(rng_next(&s->rng) & 0xFF) >= r->warms) {
                continue;
            }
            s->cells[nat] = CELL_MAKE(CELL_MATERIAL(n), (uint8_t)(t + 1));
            s->may_have_temperature = true;
            mark_rows(s, ny, ny);
            wake_block_and_neighbors(s, nx, ny);
            continue;
        }

        /* And something that CANNOT bank it melts outright. */





        /* Both gates. Gas must warm and target melt. Convection melts slower
         * than flame. */

        /* Warmer air costs snow its life. One gate would make every boiler a
         * snow-eater. */
        if (nr->chills == 0 || nr->heats_to == 0 || nr->heat_chance == 0) {
            continue;
        }
        if ((int)(rng_next(&s->rng) & 0xFF) >= r->warms) {
            continue;
        }
        /* Snow vital for shock. 1.5s smoke clears snow, risking boiler.
         * Quarter rate minimally impacts ice, doubles snow life. */
        if ((int)(rng_next(&s->rng) & 0xFF) >= (nr->heat_chance >> 2)) {
            continue;
        }
        place_reacted(s, nx, ny, nat, (material_id_t)nr->heats_to);
    }
}

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

static bool
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
                    if (!CELL_IS_EMPTY(under)
                        && (reaction_of(under)->soil != 0 || under == (cell_t)r->roots_to)) {
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
                if (!cell_is_burning(c) &&
                    (wants_room ? moisture_of(c, cr) < cr->moist_max
                                : moisture_of(c, cr) != 0)) {
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
    s->cells[soil_at] = soil_set_moisture(
        soil, (uint8_t)(moisture_of(soil, reaction_of(soil)) - amount), 0);
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
#define ROOT_SURFACE_MAX 2

/* DOWN/AWAY=2. Trunk=parent. Roots stop at dry soil. Weights guide moist cell
 * selection. */
#define ROOT_WEIGHT_AWAY 2
#define ROOT_WEIGHT_DOWN 2

#define ROOT_CONDUCT_CHANCE 64

/* One cell of ROOT, carrying water DOWN through itself - a conduit. */



/* Moves only, gravity-ward. */


/* Sides count as sources. Drawing from soil allows full column drainage. */

static bool
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

/* ROOT cell consumes soil moisture with chance, docs/Sand/Sand-Simulation.md */



/* Root does not need stem's machinery. Uses existing resource bound. */


/* Prevents system becoming a block. Uses standard roll discipline. */
static bool
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
        if (CELL_IS_EMPTY(n) || reaction_of(n)->soil == 0 ||
            moisture_of(n, reaction_of(n)) == 0) {
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
static bool
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
static bool
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
        if (soil_at < 0 && reaction_of(n)->soil != 0 &&
            moisture_of(n, reaction_of(n)) != 0) {
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

static bool
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

/* Cells WITHER without drink or trunk. Touch wood first (8 reads). Dried soil
 * keeps leaves. */
static bool
step_one_withering_cell(sand_t* s, int x, int y, int w, int h, const reaction_t* r) {
    const size_t at = (size_t)y * (size_t)w + (size_t)x;
    const cell_t self = s->cells[at];

    if (r->sheltered_by != 0) {
        for (int d = 0; d < 8; d++) {
            const int* nd = ring_dir(d);
            const int nx = x + nd[0], ny = y + nd[1];
            if ((unsigned)nx >= (unsigned)w || (unsigned)ny >= (unsigned)h) {
                continue;
            }
            const cell_t n = s->cells[(size_t)ny * (size_t)w + (size_t)nx];
            if (!CELL_IS_EMPTY(n) && CELL_MATERIAL(n) == r->sheltered_by) {
                return false; /* under its tree; it stays */
            }
            /* ROOT counts as shelter; `roots_to` matches `find_water()`.
             * Roots touch wood; else, column rots from bottom. */
            if (r->roots_to != 0 && n == (cell_t)r->roots_to) {
                return false;
            }
        }
    }

    /* See lignifying branch. Leaf, no hardens_to, skips reads. */
    bool attached = false;
    if (r->hardens_to != 0 && r->clings_to != 0) {
        for (int d = 0; d < 8; d++) {
            const int* nd = ring_dir(d);
            const int nx = x + nd[0], ny = y + nd[1];
            if ((unsigned)nx >= (unsigned)w || (unsigned)ny >= (unsigned)h) {
                continue;
            }
            const cell_t n = s->cells[(size_t)ny * (size_t)w + (size_t)nx];
            if (!CELL_IS_EMPTY(n) && CELL_MATERIAL(n) == r->clings_to) {
                attached = true;
                break;
            }
        }
    }

    int lift = 0, contact_at = -1, root_depth = 0; /* just a reachability
                                                     * check - nothing here
                                                     * spends, so nothing
                                                     * roots */
    if (find_water(s, x, y, w, h, r, self, &lift, &contact_at, &root_depth, false) >= 0) {
        return false; /* it can still drink */
    }
    if ((int)(rng_next(&s->rng) & 0xFF) >= r->withers) {
        return false;
    }

    /* Withering prevents shoot from becoming permanent woody speck. CELL_MAKE
     * used for wood burn progress. */
    REACTION_DOC(hardens_to, "if withering but still touching its own hardened trunk");
    if (attached) {
        place_cell(s, x, y, at, CELL_MAKE(r->hardens_to, 0));
        return true;
    }

    s->cells[at] = SAND_EMPTY;
    mark_rows(s, y, y);
    wake_block_and_neighbors(s, x, y);
    return true;
}

static bool
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

/* COLD melts, pulls temp, cracks if hot. Chilling, melting snow. Warm liquid
 * aids survival. */
static bool
step_one_cold_cell(sand_t* s, int x, int y, int w, int h, const reaction_t* r) {
    for (int d = 0; d < 4; d++) {
        const int nx = x + reaction_dirs[d][0];
        const int ny = y + reaction_dirs[d][1];
        if ((unsigned)nx >= (unsigned)w || (unsigned)ny >= (unsigned)h) {
            continue;
        }
        const size_t nat = (size_t)ny * (size_t)w + (size_t)nx;
        const cell_t n = s->cells[nat];
        if (CELL_IS_EMPTY(n)) {
            continue;
        }
        const reaction_t* nr = reaction_of(n);

        /* MELTING, from any liquid - see reaction_t.thaws. */
        if (r->thaws != 0 && r->heats_to != 0 && material_of(n)->kind == KIND_LIQUID
            && (int)(rng_next(&s->rng) & 0xFF) < r->thaws) {
            place_reacted(s, x, y, (size_t)y * (size_t)w + (size_t)x, (material_id_t)r->heats_to);
            return false;
        }

        if (r->chills == 0 || nr->heat_ramp == 0) {
            continue;
        }
        const uint8_t temp = CELL_VARIANT(n);

        REACTION_DOC(shatters_to, "if chilled while hot");
        if (temp >= SAND_SHOCK_HEAT && nr->shatters_to != 0) {
            crack_run(s, nx, ny, w, h, (material_id_t)CELL_MATERIAL(n), (material_id_t)nr->shatters_to);
            if (try_heat_transform(s, x, y, w, h)) {
                return false; /* and this cell melted paying for it */
            }
            continue;
        }

        if (temp == 0) {
            continue; /* already as cold as this scale goes */
        }
        if ((int)(rng_next(&s->rng) & 0xFF) >= r->chills) {
            continue;
        }

        s->cells[nat] = CELL_MAKE(CELL_MATERIAL(n), (uint8_t)(temp - 1));
        s->may_have_temperature = true;
        mark_rows(s, ny, ny);
        wake_block_and_neighbors(s, nx, ny);

        if (temp > SAND_AMBIENT_HEAT && try_heat_transform(s, x, y, w, h)) {
            return false;
        }
    }
    return true;
}

#define SPREAD_SHIFT 1

static bool
step_one_tempered_cell(sand_t* s, uint8_t* row, int x, int y, int w, int h, const reaction_t* r) {
    const cell_t c = row[x];
    const uint8_t temp = CELL_VARIANT(c);


    /* Spreading makes frost patch visible. */

    /* PUSHED to avoid ambient cell noticing frosted neighbour. Similar to
     * chilling bug. */



    /* Push question to where it is already cheap */
    bool wet = false;
    for (int d = 0; d < 4; d++) {
        const int nx = x + reaction_dirs[d][0];
        const int ny = y + reaction_dirs[d][1];
        if ((unsigned)nx >= (unsigned)w || (unsigned)ny >= (unsigned)h) {
            continue;
        }
        const size_t nat = (size_t)ny * (size_t)w + (size_t)nx;
        const cell_t n = s->cells[nat];
        if (CELL_IS_EMPTY(n)) {
            continue;
        }
        /* PAIR_QUENCHES avoids reaction_of(n) load. */
        if ((pair_theirs_bits(CELL_MATERIAL(n)) & PAIR_QUENCHES) != 0) {
            wet = true;
        }
        if (r->conducts == 0 || reaction_of(n)->heat_ramp == 0) {
            continue;
        }
        const uint8_t nt = CELL_VARIANT(n);
        const int gap = (int)temp - (int)nt;
        if (gap > -2 && gap < 2) {
            continue;
        }
        if ((int)(rng_next(&s->rng) & 0xFF) >= (r->conducts >> SPREAD_SHIFT)) {
            continue;
        }
        s->cells[nat] = CELL_MAKE(CELL_MATERIAL(n), (uint8_t)(gap > 0 ? nt + 1 : nt - 1));
        s->may_have_temperature = true;
        mark_rows(s, ny, ny);
        wake_block_and_neighbors(s, nx, ny);
    }





    /* MULTIPLIES DRAIN BY SAND_WET_COOLING_FACTOR (sand.h) */

    /* GATED TO temp > SAND_AMBIENT_HEAT - branch below untouched by `wet`. */

    /* Water cannot fall below ambient, preventing SAND_SHOCK_COLD. Snow
     * retains that role. */
    unsigned drain = r->cools;
    if (temp > SAND_AMBIENT_HEAT) {
        drain *= (unsigned)(temp - SAND_AMBIENT_HEAT);
        if (wet) {
            drain *= SAND_WET_COOLING_FACTOR;
        }
        if (drain > 255u) {
            drain = 255u;
        }
    }
    if (drain == 0 || (unsigned)(rng_next(&s->rng) & 0xFF) >= drain) {
        return temp != SAND_AMBIENT_HEAT;
    }

    const uint8_t next = (uint8_t)(temp > SAND_AMBIENT_HEAT ? temp - 1 : temp + 1);
    row[x] = CELL_MAKE(CELL_MATERIAL(c), next);
    mark_rows(s, y, y);
    wake_block_and_neighbors(s, x, y);
    return next != SAND_AMBIENT_HEAT;
}

/* Fixed blast radius. Cascade ignition simulates lid giving way. Tune on
 * device. */
#define SAND_GAS_IGNITE_BLAST_RADIUS 8

/* Shift for simplicity and reliability. */
#define SAND_DAMP_IGNITION_SHIFT     2

/* Checks GAS confinement by KIND_STATIC neighbours. Off-grid not considered a
 * wall. Avoids flood fill. */
static inline bool
gas_ignite_confined(const sand_t* s, int x, int y, int w, int h) {
    for (int d = 0; d < 4; d++) {
        const int nx = x + reaction_dirs[d][0];
        const int ny = y + reaction_dirs[d][1];
        if ((unsigned)nx >= (unsigned)w || (unsigned)ny >= (unsigned)h) {
            continue;
        }
        const cell_t n = s->cells[(size_t)ny * (size_t)w + (size_t)nx];
        if (!CELL_IS_EMPTY(n) && material_of(n)->kind == KIND_STATIC) {
            return true;
        }
    }
    return false;
}

static inline bool
try_ignite_given(sand_t* s, int nx, int ny, int w, int h, size_t at, cell_t n) {
    const reaction_t* r = reaction_of(n);
    if (r->flammability == 0) {
        return false;
    }
    /* place_reacted() resets burn, prevents log extinguishing.
     * cell_is_burning() checks GUNPOWDER_LIT (7). */
    if (cell_is_burning(n)) {
        return false;
    }
    if (r->needs_air && !touches_air(s, nx, ny, w, h)) {
        return false; /* buried in more of itself - a pool of fuel burns
                         * at its surface, not through its volume */
    }
    /* s->flammability mirrors s->decay's override: negative uses material's
     * table, other values override; 255 is checked before rolling for
     * reproducibility. */
    int f = (s->flammability >= 0) ? s->flammability : r->flammability;
    /* Gunpowder dampens re-ignition odds. */
    REACTION_DOC(flammability, "damped further per moisture level, on any material with a moisture codec");
    const uint8_t m = (r->dries != 0) ? moisture_of(n, r) : 0;
    if (m) {
        f >>= SAND_DAMP_IGNITION_SHIFT * m;
    }
    if (f <= 0) {
        return false; /* before any RNG draw - a fully damped roll must not
                          shift the RNG stream for scenes that never reach
                          this material's moisture range */
    }
    if (f < 255 && (int)(rng_next(&s->rng) & 0xFF) >= f) {
        return false;
    }
    if (s->impulse_buf != NULL && material_of(n)->kind == KIND_GAS &&
        gas_ignite_confined(s, nx, ny, w, h)) {
        sand_explode(s, nx, ny, SAND_GAS_IGNITE_BLAST_RADIUS);
        return true;
    }
    const material_id_t becomes = r->ignites_to ? r->ignites_to : MAT_FIRE;
    place_reacted(s, nx, ny, at, becomes);
    return true;
}

/* Bidirectional. Confirms placement. Checks for duplicates. */
static inline bool
emit_into_empty_neighbor(sand_t* s, int x, int y, int w, int h, uint8_t spec) {
    for (int d = 0; d < 4; d++) {
        const int fx = x + reaction_dirs[d][0];
        const int fy = y + reaction_dirs[d][1];
        if ((unsigned)fx >= (unsigned)w || (unsigned)fy >= (unsigned)h) {
            continue;
        }
        const size_t at = (size_t)fy * (size_t)w + (size_t)fx;
        if (CELL_IS_EMPTY(s->cells[at])) {
            place_reacted(s, fx, fy, at, spec);
            return true;
        }
    }
    return false;
}



/* Lava LANDS as separate grains, spends steps in open air. */


/* MAT_FIRE cells burn, react, roll smoke; suspect in lava pours. */

/* SUPPORTED, matches sweep direction. Off-grid reads as STONE. */

/* Falling grain skips roll if cell moves next step. */

/* KIND_STATIC exempt; never moves, thus unaffected by gravity. */

static inline bool
try_flare(sand_t* s, int x, int y, int w, int h, const material_t* mat,
         uint8_t flare) {
    if (flare == 0) {
        return false;
    }
    if (mat->kind != KIND_STATIC) {
        const cell_t below = sand_at(s, x + s->last_step_dx,
                                     y + s->last_step_dy);
        if (CELL_IS_EMPTY(below)) {
            return false;
        }
    }
    if ((int)(rng_next(&s->rng) & 0xFF) >= flare) {
        return false;
    }
    return emit_into_empty_neighbor(s, x, y, w, h, MAT_FIRE);
}

/* Cell pays 1 mass. Stops fire from draining. Preserves slow quench. Follows
 * give_mass(). */
static inline void
pay_quench_cost(sand_t* s, int nx, int ny, int w) {
    const size_t at = (size_t)ny * (size_t)w + (size_t)nx;
    const cell_t n = s->cells[at];
    const int mass = CELL_VARIANT(n) - 1;
    s->cells[at] = (mass > 0) ? CELL_MAKE(CELL_MATERIAL(n), mass) : CELL_EMPTY;
    mark_rows(s, ny, ny);
    wake_block_and_neighbors(s, nx, ny);
}

/* Bounds conduct_heat() walk - see "THE BOILER" for rationale. Caps cold
 * pass. */
#define CONDUCT_REACH 32


/* Attempts direct connection, fails. Rolls `conducts` to CONDUCT_REACH. Stops
 * on failure, off-grid, or empty. Liquid boils, fuel ignites, neighbors warm.
 * Returns status. */
static inline bool
conduct_heat(sand_t* s, int x, int y, int w, int h) {
    bool acted = false;

    for (int d = 0; d < 4; d++) {
        const int dx = reaction_dirs[d][0];
        const int dy = reaction_dirs[d][1];
        int rx = x + dx;
        int ry = y + dy;

        if ((unsigned)rx >= (unsigned)w || (unsigned)ry >= (unsigned)h) {
            continue;
        }
        if (CELL_IS_EMPTY(s->cells[(size_t)ry * (size_t)w + (size_t)rx])) {
            continue;
        }
        if (reaction_of(s->cells[(size_t)ry * (size_t)w + (size_t)rx])->conducts == 0) {
            continue; /* Early-out - most neighbours not conductors */
        }

        bool got_through = false;
        for (int depth = 0; depth < CONDUCT_REACH; depth++) {
            const cell_t here = s->cells[(size_t)ry * (size_t)w + (size_t)rx];
            const int c = (s->conduction >= 0) ? s->conduction : reaction_of(here)->conducts;
            if ((int)(rng_next(&s->rng) & 0xFF) >= c) {
                break; /* heat stops inside this cell of the run */
            }

            const int nx = rx + dx;
            const int ny = ry + dy;
            if ((unsigned)nx >= (unsigned)w || (unsigned)ny >= (unsigned)h) {
                break;
            }
            const cell_t next = s->cells[(size_t)ny * (size_t)w + (size_t)nx];
            if (CELL_IS_EMPTY(next)) {
                break; /* never creates fire in empty space */
            }
            rx = nx;
            ry = ny;
            if (reaction_of(next)->conducts == 0) {
                got_through = true; /* the far side - not a conductor */
                break;
            }
            /* still inside the conductor run - loop again and roll for
             * THIS cell's own conducts figure */
        }

        if (!got_through) {
            continue;
        }

        const size_t bat = (size_t)ry * (size_t)w + (size_t)rx;
        const cell_t bc = s->cells[bat];
        const material_t* bm = material_of(bc);





        if (bm->kind == KIND_LIQUID && reaction_of(bc)->burns == 0 && reaction_of(bc)->flammability == 0) {
            /* Boils cells near conductor, replacing old method. Eliminates
             * deadlock via `boils` in reaction_t; retries on failure. */
            const int boils = (s->boils >= 0) ? s->boils : reaction_of(bc)->boils;
            if (boils != 0 && (int)(rng_next(&s->rng) & 0xFF) < boils) {
                /* place_reacted() avoids shortened steam life. */
                const uint8_t boils_to = reaction_of(bc)->boils_to ? reaction_of(bc)->boils_to : MAT_STEAM;
                place_reacted(s, rx, ry, bat, boils_to);
                acted = true;
            }
        } else {
            const reaction_t* br = reaction_of(bc);
            if (br->heat_ramp != 0 || (br->heats_to != 0 && br->heat_chance != 0)) {
                if (try_heat_transform(s, rx, ry, w, h)) {
                    acted = true;
                }
            }
            if (br->flammability != 0 && (!br->needs_air || touches_air(s, rx, ry, w, h))) {
                const uint8_t becomes = br->ignites_to ? br->ignites_to : MAT_FIRE;
                place_reacted(s, rx, ry, bat, becomes);
                acted = true;
            }
        }
    }

    return acted;
}

/* ACID BUBBLES - CONTINUOUS, AMBIENT look, not event-specific. */




/* LIVES HERE, NOT IN move_liquid_grain() - skips settled blocks. */


/* NOT gated by block-sleeping - needed for dissolving, cooling, and bubbling. */


/* NO dx,dy PARAMETER - uses s->last_step_dx/last_step_dy for "which way is
 * down" (sand.h). */


/* UPWARD, STRAIGHT, MINIMAL SPREAD - NOT splash_displace()'s full ring. */
static void
acid_bubble(sand_t* s, int x, int y) {
    const int dx = s->last_step_dx, dy = s->last_step_dy;
    const int ux = x - dx, uy = y - dy; /* one step AGAINST gravity */
    if (!CELL_IS_EMPTY(sand_at(s, ux, uy))) {
        return; /* not exposed - nothing above to pop into */
    }
    if ((rng_next(&s->rng) & 0xFF) >= SAND_ACID_BUBBLE_CHANCE) {
        return;
    }
    const int i_up = (ring_of(dx, dy) + 4) & 7;
    const int spread = (int)(rng_next(&s->rng) % 3) - 1; /* -1, 0 or 1 */
    sand_impulse(s, x, y, (i_up + spread + 8) & 7, SAND_ACID_BUBBLE_SPEED);
}


/* A separate, much higher evaporate roll (below) can consume the whole
 * acid cell in one bite, so acid has a real chance to run out rather than
 * dissolving without limit while remaining one cell forever - the mistake
 * oil-soaked ash made before soaking became a real transfer. */

/* Acid eats one neighbour at a time. Returns whether it dissolved anything. */
static bool
step_one_dissolver_cell(sand_t* s, uint8_t* row, int x, int y, int w, int h, const reaction_t* r) {
    const bool per_material = s->evaporates < 0;
    const int evaporates = per_material ? r->evaporates : s->evaporates;
    if (evaporates != 0 && (int)(rng_next(&s->rng) & 0xFF) < evaporates
        && (!per_material || (rng_next(&s->rng) % 60) == 0)) {
        const size_t at = (size_t)y * (size_t)w + (size_t)x;
        place_reacted(s, x, y, at, MAT_GAS);
        return true;
    }

    if ((int)(rng_next(&s->rng) & 0xFF) >= r->dissolves) {
        return false;
    }

    for (int d = 0; d < 4; d++) {
        const int nx = x + reaction_dirs[d][0];
        const int ny = y + reaction_dirs[d][1];
        if ((unsigned)nx >= (unsigned)w || (unsigned)ny >= (unsigned)h) {
            continue;
        }
        const size_t at = (size_t)ny * (size_t)w + (size_t)nx;
        const cell_t n = s->cells[at];
        if (CELL_IS_EMPTY(n)) {
            continue;
        }
        /* PAIR_DISSOLVABLE: rejects neighbours with dissolvable figure 0 -
         * genuine acid cells only. */
        if ((pair_theirs_bits(CELL_MATERIAL(n)) & PAIR_DISSOLVABLE) == 0) {
            continue;
        }
        const uint8_t give = reaction_of(n)->dissolvable;
        if (give == 0 || (int)(rng_next(&s->rng) & 0xFF) >= give) {
            continue;
        }

        /* DILUTION occurs. SAND_ACID_DILUTE_TO_WATER_CHANCE dictates. Cells
         * evolve symmetrically. Winner vaporizes, loser converts.
         * CELL_VARIANT persists. Transformation costs. */
        if (CELL_MATERIAL(n) == MAT_WATER) {
            /* Measuring acid alone was asymmetrical: deep acid vs. adjacent
             * water. */
            int acid_backing = 0, water_backing = 0;
            for (int bd = 0; bd < 4; bd++) {
                const int abx = x + reaction_dirs[bd][0];
                const int aby = y + reaction_dirs[bd][1];
                if ((unsigned)abx < (unsigned)w && (unsigned)aby < (unsigned)h
                    && CELL_MATERIAL(s->cells[(size_t)aby * (size_t)w + (size_t)abx]) == MAT_ACID) {
                    acid_backing++;
                }

                const int wbx = nx + reaction_dirs[bd][0];
                const int wby = ny + reaction_dirs[bd][1];
                if ((unsigned)wbx < (unsigned)w && (unsigned)wby < (unsigned)h
                    && CELL_MATERIAL(s->cells[(size_t)wby * (size_t)w + (size_t)wbx]) == MAT_WATER) {
                    water_backing++;
                }
            }
            const int mass_bias = (s->acid_dilute_mass_bias >= 0)
                                       ? s->acid_dilute_mass_bias : SAND_ACID_DILUTE_MASS_BIAS;
            const int water_wins_chance = SAND_ACID_DILUTE_TO_WATER_CHANCE
                                           + (water_backing - acid_backing) * mass_bias;

            const int roll = (int)(rng_next(&s->rng) & 0xFF);
            const size_t self_at = (size_t)y * (size_t)w + (size_t)x;
            if (roll < SAND_ACID_DILUTE_EVAPORATE_CHANCE) {
                place_reacted(s, x, y, self_at, MAT_GAS);
            } else if (roll < SAND_ACID_DILUTE_EVAPORATE_CHANCE + water_wins_chance) {
                /* Acid converts to water, mass carries over. Uses
                 * place_cell()/place_reacted() for content flags. */
                place_cell(s, x, y, self_at, CELL_MAKE(MAT_WATER, CELL_VARIANT(row[x])));
                place_reacted(s, nx, ny, at, MAT_STEAM);
            } else {
                place_reacted(s, x, y, self_at, MAT_GAS);
                place_cell(s, nx, ny, at, CELL_MAKE(MAT_ACID, CELL_VARIANT(n)));
            }
            return true;
        }

        /* Oil turns to gas. Acid dies or pays quench cost. Rolls independent. */
        if (CELL_MATERIAL(n) == MAT_OIL) {
            const uint8_t oil_residue = ((int)(rng_next(&s->rng) & 0xFF) < SAND_ACID_OIL_TO_GAS_CHANCE)
                                         ? MAT_GAS : MAT_ACID;
            place_reacted(s, nx, ny, at, oil_residue);

            if ((int)(rng_next(&s->rng) & 0xFF) < SAND_ACID_OIL_DEATH_CHANCE) {
                const size_t self_at = (size_t)y * (size_t)w + (size_t)x;
                s->cells[self_at] = CELL_EMPTY;
                mark_rows(s, y, y);
                wake_block_and_neighbors(s, x, y);
            } else {
                pay_quench_cost(s, x, y, w);
            }
            return true;
        }

        /* Smoke lighter, try_bubble() moves it. Smoke or gas, 50% chance,
         * acid breathes gas. */
        if (r->fizz != 0 && (int)(rng_next(&s->rng) & 0xFF) < r->fizz) {
            const uint8_t residue = (rng_next(&s->rng) & 1) ? MAT_GAS : MAT_SMOKE;
            place_reacted(s, nx, ny, at, residue);
        } else {
            s->cells[at] = CELL_EMPTY;
            mark_rows(s, ny, ny);
            wake_block_and_neighbors(s, nx, ny);
        }

        if ((int)(rng_next(&s->rng) & 0xFF) < SAND_ACID_EAT_DEATH_CHANCE) {
            row[x] = CELL_EMPTY;
            mark_rows(s, y, y);
            wake_block_and_neighbors(s, x, y);
        } else {
            pay_quench_cost(s, x, y, w);
        }
        return true;
    }
    return false;
}

/* See reaction_t.explodes, material.h */

/* 2x2, NOT 3x3. Burn-out rolls independent per cell. */




/* gunpowder shares high nibble with extended statics; avoid counting ice as
 * fuse */

/* cell_is_burning(n): re-derive unnecessary */
static inline bool
lit_here(const sand_t* s, int nx, int ny, int w, int h, cell_t grain, const reaction_t* r) {
    if ((unsigned)nx >= (unsigned)w || (unsigned)ny >= (unsigned)h) {
        return false;
    }
    const cell_t n = s->cells[(size_t)ny * (size_t)w + (size_t)nx];
    return same_species(n, grain) && cell_code(n) >= r->lit_from;
}

static inline bool
find_lit_two_by_two(const sand_t* s, int x, int y, int w, int h, cell_t grain, const reaction_t* r,
                     int* out_dx, int* out_dy) {
    for (int dy = -1; dy <= 1; dy += 2) {
        if (!lit_here(s, x, y + dy, w, h, grain, r)) {
            continue;
        }
        for (int dx = -1; dx <= 1; dx += 2) {
            if (lit_here(s, x + dx, y, w, h, grain, r) && lit_here(s, x + dx, y + dy, w, h, grain, r)) {
                *out_dx = dx;
                *out_dy = dy;
                return true;
            }
        }
    }
    return false;
}

static inline void
spend_lit_two_by_two(sand_t* s, int x, int y, int w, int dx, int dy) {
    const int px[3] = {x + dx, x, x + dx};
    const int py[3] = {y, y + dy, y + dy};
    for (int i = 0; i < 3; i++) {
        const size_t at = (size_t)py[i] * (size_t)w + (size_t)px[i];
        place_reacted(s, px[i], py[i], at, MAT_FIRE);
    }
}

/* BOUNDS BURST COST PER FRAME; CADENCE OF DETONATIONS. BOARD-WIDE. */
#define SAND_GUNPOWDER_BLAST_COOLDOWN 8

static bool
step_one_burning_cell(sand_t* s, uint8_t* row, int x, int y, int w, int h) {
    cell_t grain = row[x];
    const material_t* mat = material_of(grain);
    const uint8_t mat_id = CELL_MATERIAL(grain);
    const size_t at = (size_t)y * (size_t)w + (size_t)x;

    /* Material burns own rate, decay stays 0, not transient. */
    const reaction_t* rx = reaction_of(grain);
    const bool lit_state = rx->burn_decay != 0;
    const int burn_rate = (s->decay >= 0) ? s->decay : rx->burn_decay;

    if (lit_state ? !tick_decay_at(s, row, x, y, &grain, rx, burn_rate)
                  : !tick_decay(s, row, x, y, &grain, mat, mat_id)) {
        if (rx->explodes != 0) {
            REACTION_DOC(explodes, "at burn-out, if it is one corner of a 2x2 that is all lit and the board's blast cooldown has run out");
            int dx = 0, dy = 0;
            if (s->impulse_buf != NULL && s->fuse_blast_wait == 0 &&
                find_lit_two_by_two(s, x, y, w, h, grain, rx, &dx, &dy)) {
                s->fuse_blast_wait = (uint8_t)((s->fuse_cooldown >= 0) ? s->fuse_cooldown
                                                                       : SAND_GUNPOWDER_BLAST_COOLDOWN);
                spend_lit_two_by_two(s, x, y, w, dx, dy);
                sand_explode(s, x, y, rx->explodes);
            } else {
                place_reacted(s, x, y, at, MAT_FIRE);
            }
            return true;
        }
        /* MAT_SMOKE, not MAT_STEAM. Avoids confusion. Use hardcoded for
         * uniform residue; use `smokes_to` if needed. */
        const uint8_t residue = rx->residue;
        if (residue != 0 && (int)(rng_next(&s->rng) & 0xFF) < residue) {
            place_reacted(s, x, y, at, MAT_SMOKE);
        }
        return true;
    }

    if (s->may_have_liquid) {
        for (int d = 0; d < 4; d++) {
            const int nx = x + reaction_dirs[d][0];
            const int ny = y + reaction_dirs[d][1];
            if (neighbor_quenches(s, nx, ny, w, h)) {
                const uint8_t quench_to = rx->quench_to;
                if (lit_state) {
                    /* Ember needs distinct state. Use `with_moisture()` in
                     * `place_cell()`. */
                    if (rx->tones != 0) {
                        place_cell(s, x, y, at, with_moisture(grain, rx->moist_max, rx));
                    } else {
                        row[x] = CELL_MAKE(mat_id, 0);
                        mark_rows(s, y, y);
                        wake_block_and_neighbors(s, x, y);
                    }
                } else if (quench_to != 0) {
                    uint8_t product      = quench_to;
                    bool leaves_residue  = true;
                    if (mat_id == MAT_FIRE) {
                        const size_t nat = (size_t)ny * (size_t)w + (size_t)nx;
                        const uint8_t liquid_boils_to = reaction_of(s->cells[nat])->boils_to;
                        if (liquid_boils_to == MAT_GAS) {
                            leaves_residue = (int)(rng_next(&s->rng) & 0xFF)
                                             < SAND_ACID_QUENCH_RESIDUE_CHANCE;
                            product = ((int)(rng_next(&s->rng) & 0xFF)
                                       < SAND_ACID_QUENCH_SMOKE_CHANCE)
                                          ? MAT_SMOKE
                                          : MAT_GAS;
                        } else {
                            product = liquid_boils_to ? liquid_boils_to : MAT_STEAM;
                        }
                    }
                    if (leaves_residue) {
                        place_reacted(s, x, y, at, product);
                        /* Gated on KIND_LIQUID. Only lava quenches AS liquid. */
                        if (mat->kind == KIND_LIQUID) {
                            const int lava_cooloff = (s->lava_cooloff >= 0)
                                                          ? s->lava_cooloff
                                                          : SAND_LAVA_COOLOFF_CHANCE;
                            cool_off_chain(s, x, y, w, h, product, lava_cooloff);
                        }
                    } else {
                        row[x] = CELL_EMPTY;
                        mark_rows(s, y, y);
                        wake_block_and_neighbors(s, x, y);
                    }
                } else {
                    row[x] = CELL_EMPTY;
                    mark_rows(s, y, y);
                    wake_block_and_neighbors(s, x, y);
                }
                pay_quench_cost(s, nx, ny, w);
                return true;
            }
        }
    }





    /* Covered_at checks lid with cover_mask. */

    if (mat->kind != KIND_LIQUID && rx->explodes == 0 && smothered(s, x, y, w, h, mat->density)) {
        row[x] = lit_state ? cell_with_code(grain, 0) : CELL_EMPTY;
        mark_rows(s, y, y);
        wake_block_and_neighbors(s, x, y);
        return true;
    }

    bool acted = false;



    /* WHOLE-CELL EVENT, NOT PER-NEIGHBOUR PROBE */



    /* NOT smothered()'s all-4 test - fixes cover_count(), SCREEN cardinal
     * count, and pool neighbor limits. */


    /* covered_at()'s lid with diagonal crust cells fixes
     * test_a_wide_pool_under_a_crust_bursts. */


    /* NOT GATED ON s->impulse_buf. No fallback needed: sand_explode() is a
     * no-op with impulses off. */

    /* See test_buried_lava_still_becomes_stone_with_impulses_off,
     * suite_sand_lava_burial.c */
    const bool is_lava = mat->kind == KIND_LIQUID && rx->quench_to != 0;
    /* Test at 255 means 'fire on every cell'; 1-in-N complicates testing. */
    const bool burst_natural = s->lava_burst < 0;
    const int burst_chance = burst_natural ? SAND_LAVA_BURST_CHANCE : s->lava_burst;
    if (is_lava && burst_chance != 0 &&
        (int)(rng_next(&s->rng) & 0xFF) < burst_chance &&
        (!burst_natural || (rng_next(&s->rng) % SAND_LAVA_BURST_GATE) == 0) &&
        covered_at(s, x, y, w, h, mat->density)) {
        place_reacted(s, x, y, at, rx->quench_to);
        sand_explode(s, x, y, SAND_LAVA_BURST_RADIUS);
        return true;
    }




    /* DO NOT merge with quench or conduct_heat walks - see top comment for
     * RNG draw reasons. */

    /* HOISTED OUT OF THE LOOP: mat_id fixed, pair_bits[mat_id] reused. */



    /* Host numbers mispredict device; capture settles it. */

    /* TRIGGER A of cool_off_chain(): 0 short-circuits check before loop
     * starts. */
    const int lava_cooloff = (mat->kind == KIND_LIQUID && rx->quench_to != 0)
                                  ? ((s->lava_cooloff >= 0) ? s->lava_cooloff
                                                             : SAND_LAVA_COOLOFF_CHANCE)
                                  : 0;
    const uint8_t* my_pair_row = pair_bits[mat_id];
    for (int d = 0; d < 4; d++) {
        const int nx = x + reaction_dirs[d][0];
        const int ny = y + reaction_dirs[d][1];
        if ((unsigned)nx >= (unsigned)w || (unsigned)ny >= (unsigned)h) {
            continue;
        }
        const size_t nat = (size_t)ny * (size_t)w + (size_t)nx;
        const cell_t n = s->cells[nat];
        if (CELL_IS_EMPTY(n)) {
            continue;
        }
        const uint8_t pair = my_pair_row[CELL_MATERIAL(n)];
        if ((pair & PAIR_IGNITABLE) != 0 && try_ignite_given(s, nx, ny, w, h, nat, n)) {
            acted = true;
        }
        if ((pair & PAIR_HEAT_RESPONSIVE) != 0) {
            /* Heat-ramping materials store heat. Only CELL_MATERIAL change or
             * thermal shock triggers `cool_off_chain()`. */
            const uint8_t before_mat = CELL_MATERIAL(n);
            bool changed = try_heat_transform_given(s, nx, ny, w, h, nat, n);
            /* MELTING - LIQUID door opens. See reaction_t.melts. Fire and
             * lava merge. Check source, neighbors, RNG. Flame near root pays
             * three checks, avoids stream. */
            if (!changed && mat->kind == KIND_LIQUID) {
                const reaction_t* nr = reaction_of(n);
                if (nr->melts != 0 && nr->heats_to != 0 && (int)(rng_next(&s->rng) & 0xFF) < nr->melts) {
                    place_reacted(s, nx, ny, nat, nr->heats_to);
                    changed = true;
                }
            }
            if (changed) {
                acted = true;
                if (lava_cooloff != 0 && CELL_MATERIAL(s->cells[nat]) != before_mat &&
                    (int)(rng_next(&s->rng) & 0xFF) < lava_cooloff) {
                    place_reacted(s, x, y, at, rx->quench_to);
                    cool_off_chain(s, x, y, w, h, rx->quench_to, lava_cooloff);
                    return true;
                }
            }
        }
    }

    if (conduct_heat(s, x, y, w, h)) {
        acted = true;
    }

    if (try_flare(s, x, y, w, h, mat, reaction_of(grain)->flare)) {
        acted = true;
    }

    return acted;
}

/* Tiny chance for cells to merge. Called per cell, L-to-R, T-to-B. */
static inline bool
step_one_condensing_cell(sand_t* s, int x, int y, int w, int h, const reaction_t* r) {
    if (x + 1 >= w || y + 1 >= h) {
        return false;
    }
    const size_t at    = (size_t)y * (size_t)w + (size_t)x;
    const size_t at_r  = at + 1;
    const size_t at_d  = at + (size_t)w;
    const size_t at_dr = at_d + 1;
    const uint8_t mat_id = CELL_MATERIAL(s->cells[at]);

    if (CELL_IS_EMPTY(s->cells[at_r]) || CELL_MATERIAL(s->cells[at_r]) != mat_id
        || CELL_IS_EMPTY(s->cells[at_d]) || CELL_MATERIAL(s->cells[at_d]) != mat_id
        || CELL_IS_EMPTY(s->cells[at_dr]) || CELL_MATERIAL(s->cells[at_dr]) != mat_id) {
        return false;
    }

    const int condenses = (s->condenses >= 0) ? s->condenses : r->condenses;
    if (condenses == 0 || (int)(rng_next(&s->rng) & 0xFF) >= condenses) {
        return false;
    }

    place_reacted(s, x, y, at, r->condenses_to);
    place_cell(s, x + 1, y, at_r, CELL_EMPTY);
    place_cell(s, x, y + 1, at_d, CELL_EMPTY);
    place_cell(s, x + 1, y + 1, at_dr, CELL_EMPTY);
    return true;
}

/* ACID RAIN fake collapse, MAT_GAS/STEAM, 2-2 splits. Survivor 50/50
 * Acid/Water. Caller: FOUND_DISSOLVER, FOUND_MOISTURE, FOUND_CONDENSING. */
static inline bool
step_one_acid_rain_cell(sand_t* s, int x, int y, int w, int h) {
    if (x + 1 >= w || y + 1 >= h) {
        return false;
    }
    const size_t at    = (size_t)y * (size_t)w + (size_t)x;
    const size_t at_r  = at + 1;
    const size_t at_d  = at + (size_t)w;
    const size_t at_dr = at_d + 1;
    const uint8_t m0 = CELL_MATERIAL(s->cells[at]);
    const uint8_t m1 = CELL_MATERIAL(s->cells[at_r]);
    const uint8_t m2 = CELL_MATERIAL(s->cells[at_d]);
    const uint8_t m3 = CELL_MATERIAL(s->cells[at_dr]);

    if ((m0 != MAT_STEAM && m0 != MAT_GAS) || (m1 != MAT_STEAM && m1 != MAT_GAS)
        || (m2 != MAT_STEAM && m2 != MAT_GAS) || (m3 != MAT_STEAM && m3 != MAT_GAS)) {
        return false;
    }
    const int steam_count = (m0 == MAT_STEAM) + (m1 == MAT_STEAM)
                             + (m2 == MAT_STEAM) + (m3 == MAT_STEAM);
    if (steam_count != 2) {
        return false;
    }

    const int acid_rain = (s->acid_rain >= 0) ? s->acid_rain : SAND_ACID_RAIN_CHANCE;
    if (acid_rain == 0 || (int)(rng_next(&s->rng) & 0xFF) >= acid_rain) {
        return false;
    }

    /* Coin flip, see header for acid probability. */
    const uint8_t residue = (rng_next(&s->rng) & 1) ? MAT_ACID : MAT_WATER;
    place_reacted(s, x, y, at, residue);
    place_cell(s, x + 1, y, at_r, CELL_EMPTY);
    place_cell(s, x, y + 1, at_d, CELL_EMPTY);
    place_cell(s, x + 1, y + 1, at_dr, CELL_EMPTY);
    return true;
}

/* Avoids treating stone as heat source */

/* PRESENCE, NOT ACTIVITY: may_have_burning latches as soon as a cell is
 * identified as burning, before step_one_burning_cell() runs - not only
 * when it reacts or decays this step. Latching on activity instead would
 * let a quiet burning cell (no fuel/liquid touching it, roll not hit) fall
 * out of the bookkeeping while still on the grid. */

/* Separate flags avoid false positives. */
#define FOUND_BURNING     1u
#define FOUND_DISSOLVER   2u
#define FOUND_TEMPERATURE 4u
#define FOUND_MOISTURE    8u
#define FOUND_FALLER      16u
#define FOUND_WITHERING   32u
#define FOUND_CONDENSING  64u

/* REACTION-STAGE DISPATCH TABLE skips PREFIX rows. Water, oil, metal traverse
 * all fields. */


/* Two tables: key NOT material nibble - 16 different rows for each. */

static uint8_t material_first_stage[MATERIAL_MAX];
static uint8_t extended_first_stage[MATERIAL_EXTENDED_CODES];

static unsigned
step_one_reacting_row(sand_t* s, int y, int w, int h) {
    uint8_t* row = s->cells + (size_t)y * (size_t)w;

    static void* const stage_labels[RSTAGE_COUNT] = {
        &&stage_burn_any,  &&stage_burn_always, &&stage_burn_check, &&stage_dissolve, &&stage_acid_rain,
        &&stage_condense,  &&stage_heat_ramp,   &&stage_chill,      &&stage_warm,     &&stage_soak_dry,
        &&stage_fall,      &&stage_wither,      &&stage_drink,      &&stage_root,     &&stage_grow,
        &&stage_sprout,    &&stage_bud,         &&stage_end,
    };

    unsigned found = 0;
    for (int x = 0; x < w; x++) {
        const cell_t c = row[x];
        if (CELL_IS_EMPTY(c)) {
            continue;
        }
        const reaction_t* r;
        uint8_t stage;
        if (CELL_MATERIAL(c) == MAT_EXTENDED) {
            const uint8_t variant = CELL_VARIANT(c);
            r = &extended_reactions[variant];
            stage = extended_first_stage[variant];
        } else {
            const uint8_t mat = CELL_MATERIAL(c);
            r = &reactions[mat];
            stage = material_first_stage[mat];
        }
        goto* stage_labels[stage];

    stage_burn_always:
        found |= FOUND_BURNING;
        step_one_burning_cell(s, row, x, y, w, h);
        continue;

    stage_burn_check:
        if (cell_code(c) >= r->lit_from) {
            found |= FOUND_BURNING;
            step_one_burning_cell(s, row, x, y, w, h);
            continue;
        }
        goto stage_dissolve;

    stage_burn_any:
        if (cell_is_burning(c)) {
            found |= FOUND_BURNING;
            step_one_burning_cell(s, row, x, y, w, h);
            continue;
        }

    stage_dissolve:
        if (r->dissolves) {
            found |= FOUND_DISSOLVER;
            /* MAT_ACID specific - see acid_bubble()'s comment. Future
             * dissolvers may not bubble. */
            if (CELL_MATERIAL(c) == MAT_ACID) {
                acid_bubble(s, x, y);
            }
            step_one_dissolver_cell(s, row, x, y, w, h, r);
            continue;
        }
        /* See SAND_ACID_RAIN_CHANCE's comment (sand.h). */


        /* found |= ... survivor writes at (x, y), walk never revisits, no
         * other branch reports FOUND_DISSOLVER/FOUND_MOISTURE, steam can't
         * report FOUND_CONDENSING. */


    stage_acid_rain:
        if (CELL_MATERIAL(c) == MAT_GAS || CELL_MATERIAL(c) == MAT_STEAM) {
            if (step_one_acid_rain_cell(s, x, y, w, h)) {
                found |= FOUND_DISSOLVER | FOUND_MOISTURE | FOUND_CONDENSING;
                continue;
            }
        }
        /* May_have_temperature not re-armed for boards with no heat-holder. */
    stage_condense:
        if (r->condenses != 0) {
            found |= FOUND_CONDENSING;
            if (step_one_condensing_cell(s, x, y, w, h, r)) {
                continue;
            }
        }
    stage_heat_ramp:
        if (r->heat_ramp != 0) {
            if (CELL_VARIANT(c) != SAND_AMBIENT_HEAT && step_one_tempered_cell(s, row, x, y, w, h, r)) {
                found |= FOUND_TEMPERATURE;
            }
            continue;
        }
        /* Drift on dry ground persists, found later when liquid reaches it. */
    stage_chill:
        if (r->chills != 0) {
            if (step_one_cold_cell(s, x, y, w, h, r)) {
                found |= FOUND_TEMPERATURE;
            }
            continue;
        }
        /* Gated on `may_have_heat_holder` to avoid unnecessary neighbour
         * scans. */
    stage_warm:
        if (r->warms != 0 && s->may_have_temperature && s->may_have_heat_holder) {
            step_one_warming_cell(s, x, y, w, h, r);
            found |= FOUND_TEMPERATURE;
            continue;
        }
        /* Cheap tests first: field, may_have_liquid, then neighbour scan. */
    stage_soak_dry:
        if ((r->soaks != 0 || r->dries != 0) && step_one_soaking_cell(s, row, x, y, w, h, r)) {
            found |= FOUND_MOISTURE;
            continue;
        }
    stage_fall:
        if (r->falls != 0) {
            /* Armed by EXISTING. Landed seed clears flag, stops pass.
             * Dissolve ground, plants hang. Bug. */
            found |= FOUND_FALLER;
            if (step_one_falling_cell(s, x, y, w, h, r)) {
                continue;
            }
        }
        /* Not gated on may_have_moisture: cells are far from water, boards
         * may have none. */
    stage_wither:
        if (r->withers != 0) {
            found |= FOUND_WITHERING;
            if (step_one_withering_cell(s, x, y, w, h, r)) {
                continue;
            }
        }
    stage_drink:
        if (r->drinks != 0 && s->may_have_liquid) {
            if (step_one_drinking_cell(s, x, y, w, h, r, c)) {
                found |= FOUND_MOISTURE;
            }
        }
    stage_root:
        if (r->roots != 0 && c == (cell_t)r->roots_to && s->may_have_moisture) {
            /* Conduct first, tip growth level constraint. */
            if (step_one_conducting_cell(s, x, y, w, h, r)) {
                found |= FOUND_MOISTURE;
            }
            if (step_one_rooting_cell(s, x, y, w, h, r)) {
                found |= FOUND_MOISTURE;
            }
            continue;
        }
    stage_grow:
        if (r->grows != 0 && s->may_have_moisture) {
            step_one_growing_cell(s, x, y, w, h, r);
            found |= FOUND_MOISTURE;
            continue;
        }
        /* Budding. Same gate as growing, and reached by unlit wood, which
         * falls through every branch above it. */
    stage_sprout:
        if (r->sprouts != 0 && s->may_have_moisture) {
            if (step_one_sprouting_cell(s, x, y, w, h, r)) {
                found |= FOUND_MOISTURE;
            }
        }
        /* Budding, on the same gate. Reached by wood, which falls through
         * every branch above it. */
    stage_bud:
        if (r->buds != 0 && s->may_have_moisture) {
            if (step_one_budding_cell(s, x, y, w, h, r)) {
                found |= FOUND_MOISTURE;
            }
        }
    stage_end:;
    }
    return found;
}

void
sand_step_reactions(sand_t* s) {
    if (s->fuse_blast_wait != 0) {
        s->fuse_blast_wait--;
    }
    /* Dissolving, not fire. Heat, condensation independent. */
    if (!s->may_have_burning && !s->may_have_dissolver && !s->may_have_temperature && !s->may_have_moisture
        && !s->may_have_faller && !s->may_have_withering && !s->may_have_condenser) {
        return;
    }





    uint8_t theirs_bits[MATERIAL_MAX] = {0};
    for (int m = 1; m < MAT_COUNT; m++) {
        const reaction_t* r = &reactions[m];
        if (r->heat_ramp != 0 || (r->heats_to != 0 && (r->heat_chance != 0 || r->melts != 0))) {
            theirs_bits[m] |= PAIR_HEAT_RESPONSIVE;
        }
        if (material_by_id((material_id_t)m)->kind == KIND_LIQUID) {
            if (r->wets != 0) {
                theirs_bits[m] |= PAIR_WETS;
            }
            if (r->flammability == 0 && r->burns == 0) {
                theirs_bits[m] |= PAIR_QUENCHES;
            }
        }
        if (r->flammability != 0) {
            theirs_bits[m] |= PAIR_IGNITABLE;
        }
        if (r->dissolvable != 0) {
            theirs_bits[m] |= PAIR_DISSOLVABLE;
        }
    }
    /* MATERIAL_EXTENDED_CODES (16), not _COUNT (8): a probe only knows a
     * cell is MAT_EXTENDED, never which of the sixteen codes, so
     * theirs_bits[MAT_EXTENDED] ORs every extended material's bits into one
     * shared slot. Including gunpowder's half is safe since OR only sets
     * bits, never clears one a static already set. */
    for (int k = 0; k < MATERIAL_EXTENDED_CODES; k++) {
        const reaction_t* r = &extended_reactions[k];
        if (r->heat_ramp != 0 || (r->heats_to != 0 && (r->heat_chance != 0 || r->melts != 0))) {
            theirs_bits[MAT_EXTENDED] |= PAIR_HEAT_RESPONSIVE;
        }
        if (r->flammability != 0) {
            theirs_bits[MAT_EXTENDED] |= PAIR_IGNITABLE;
        }
        if (r->dissolvable != 0) {
            theirs_bits[MAT_EXTENDED] |= PAIR_DISSOLVABLE;
        }
    }
    for (int mine = 0; mine < MATERIAL_MAX; mine++) {
        for (int theirs = 0; theirs < MATERIAL_MAX; theirs++) {
            pair_bits[mine][theirs] = theirs_bits[theirs];
        }
    }

    for (int m = 0; m < MAT_COUNT; m++) {
        const bool is_acid_rain_material = (m == MAT_GAS || m == MAT_STEAM);
        material_first_stage[m] = reaction_first_stage(&reactions[m], is_acid_rain_material);
    }
    for (int k = 0; k < MATERIAL_EXTENDED_CODES; k++) {
        extended_first_stage[k] = reaction_first_stage(&extended_reactions[k], false);
    }

    const int w = s->w;
    const int h = s->h;

    unsigned found = 0;
    for (int y = 0; y < h; y++) {
        found |= step_one_reacting_row(s, y, w, h);
    }

    if (!(found & FOUND_BURNING)) {
        s->may_have_burning = false;
    }
    if (!(found & FOUND_DISSOLVER)) {
        s->may_have_dissolver = false;
    }
    if (!(found & FOUND_TEMPERATURE)) {
        s->may_have_temperature = false;
    }
    if (!(found & FOUND_MOISTURE)) {
        s->may_have_moisture = false;
    }
    if (!(found & FOUND_FALLER)) {
        s->may_have_faller = false;
    }
    if (!(found & FOUND_WITHERING)) {
        s->may_have_withering = false;
    }
    if (!(found & FOUND_CONDENSING)) {
        s->may_have_condenser = false;
    }

    /* may_have_heat_holder NOT cleared; clearing at end is wrong. */

    /* may_have_heat_holder: cell with heat_ramp can be created during this
     * pass. */

    /* Clearing wipes flag; convection dead. */

    /* Measured: arm-only version byte-identical to HEAD on eight scenes. */


    /* Nobody pays per-step upkeep once armed. */
}
