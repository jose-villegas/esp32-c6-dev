/*
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
 * out of life produces MAT_SMOKE instead. Kept as two materials because a
 * fire burning out in mid-air, nowhere near water, puffing bright
 * kettle-steam reads as a bug to anyone who can see there was nothing there
 * to boil. The split is mostly a palette difference (cool/bright for steam,
 * warm/dim for smoke).
 *
 * THE BOILER: fire never crosses stone directly - conduct_heat() conducts
 * heat through it instead, boiling a liquid or igniting fuel on the far
 * side, never creating fire in empty space, which is what keeps a sealed
 * box sealed. Not a can_enter() special case letting fire pass through:
 * that would leak fire through every sealed stone container. Boiling
 * happens at the heat source; the steam bubbles out on its own through
 * try_bubble() (sand_gas.c). conduct_heat()'s own reach has to attenuate
 * with thickness rather than stop at one conductor cell: the pour brush
 * cannot draw a wall one cell thick, so a reach-of-one boiler is
 * unbuildable on the device despite reading as a clean rule in isolation.
 * CONDUCT_REACH bounds the walk's cost but must stay generous enough that
 * attenuation, not the cap, is what limits depth in any scene the brush can
 * actually draw.
 */

#include "reaction_doc.h"
#include "sand_priv.h"

/* PAIR_BITS - classifies neighbour probes, replacing s->heat_mask/s->wet_mask
 * and adding three more. */

/* HONESTY: All but PAIR_DENSER use `theirs`. See top comment and
 * docs/sand/Reaction-Table.md. */

/* Consistent lookup shape for every consumer. PAIR_DENSER is genuinely
 * pairwise. */

/* See pair_theirs_bits() for `mine`-less calls. */
#define PAIR_HEAT_RESPONSIVE (1u << 0) /* theirs could pass try_heat_transform()'s first two gates - was heat_mask */
#define PAIR_WETS            (1u << 1) /* theirs is a liquid whose reaction row wets - was wet_mask */
#define PAIR_IGNITABLE       (1u << 2) /* theirs has a nonzero flammability - try_ignite()'s own first reject */
#define PAIR_QUENCHES        (1u << 3) /* theirs is a liquid that is neither fuel nor a heat source - neighbor_quenches() */
#define PAIR_DISSOLVABLE     (1u << 4) /* theirs has a nonzero dissolvable - step_one_dissolver_cell()'s own reject */
#define PAIR_CONDUCTS        (1u << 5) /* theirs has a nonzero conducts - conduct_heat()'s own reject */

/* Every pair bit any material now on the board can offer a neighbour: the OR
 * of theirs_bits over s->may_have_materials, recomputed once a step in
 * sand_step_reactions(). Every "is there anything here I could act ON" reject
 * reads this instead of walking to find out.
 *
 * Starts all-ones so nothing is skipped before the first pass has looked.
 *
 * seen_materials is the same mask being rebuilt from what THIS pass actually
 * walks, so the board can narrow as well as widen - a latch alone only ever
 * grows. */
static uint8_t  present_pair_bits = 0xFFu;
static uint16_t seen_materials;

/* Can an acid-rain quad exist at all? It needs all four cells to be steam or
 * gas with exactly two steam - so two of each - and a board missing either
 * material can never form one, wherever the cells happen to sit.
 *
 * True until the first pass has looked, since it gates work being skipped. */
static bool     acid_rain_possible = true;

/* The densest non-liquid anywhere on the board, from the same mask. A cell at
 * or above it has no possible smotherer, because neighbor_smothers() asks only
 * whether the NEIGHBOUR is a denser non-liquid - so the answer is a property of
 * the board, not of the cell asking, exactly as the pair bits are.
 *
 * 255 until the first pass has looked: it gates work being skipped. */
static uint8_t  max_smothering_density = 255u;

/* A 17th material would fall out of the mask silently, and a material missing
 * from it reads as absent - which SKIPS work rather than adding it. Wrong
 * output, no crash, so nothing else would catch it. */
_Static_assert(MATERIAL_MAX <= 16, "may_have_materials is a uint16_t bit per material");

static uint8_t pair_bits[MATERIAL_MAX][MATERIAL_MAX];

/* Reads theirs-only bits. Used by try_heat_transform(), step_one_cold_cell(),
 * conduct_heat(). MAT_EMPTY stores theirs-only bits. Avoid PAIR_DENSER. */
static inline uint8_t
pair_theirs_bits(uint8_t theirs) {
    return pair_bits[MAT_EMPTY][theirs];
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

static void crack_run(sand_t* s, int x, int y, int w, int h, material_id_t from, material_id_t into);

static inline bool emit_into_empty_neighbor(sand_t* s, int x, int y, int w, int h, uint8_t spec);

static inline __attribute__((always_inline)) bool try_heat_transform_given(sand_t* s, int nx, int ny, int w, int h,
                                                                           size_t at, cell_t n);

/* HEAT LEVELS DO NOT WAKE: a write that only moves a cell's heat nibble one
 * step marks its row for drawing and stops there.
 *
 * Waking buys another chance to MOVE, and only STONE and GLASS hold a
 * heat_ramp - both KIND_STATIC. What does change how a cell moves changes its
 * MATERIAL, through place_cell(), which still wakes.
 *
 * Waking shook a solid ice block out of its column, and put snow's crust rate
 * under COLD_REWARM_PERIOD: at a period of 1 the balance ceiling moved 9x
 * (bd esp32c6-8ce). */

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
        mark_rows(s, ny, ny);   /* drawn, not woken - see HEAT LEVELS DO NOT WAKE */
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
        if (r->spoils_to != 0 && (int)(rng_next(&s->rng) & 0xFF) < r->spoils_chance) {
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
            s->heat_flaw_is_flawed = (int)(rng_next(&s->rng) & 0xFF) < r->flaw_chance;
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
        place_cell(s, x, y, at, cullet_cell(s));
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

/* A saturated cell only rolls its conversion one step in this many.
 *
 * soaked_chance FLOORS AT 1 IN 256 - the roll is rng_next() & 0xFF - so it
 * cannot reach "magnitudes slower" alone. Spacing the roll can, and costs no
 * draw on the steps it skips. First oil went from 18 steps to ~400.
 *
 * A POWER OF TWO, the gate being a mask, so it moves only in factors of two.
 * Finer changes go on soaked_chance, which is gunpowder's alone - no other row
 * declares soaked_to, whatever an earlier note here claimed. */
#define SOAKED_CONVERT_PERIOD 64

/* Splits cell for input/output. Soaks UNIT, transforms or increases variant.
 * Drying decreases variant. Returns true if wet/near liquid. Prevents
 * `may_have_moisture`. Activated by SOAKING side. */
static bool
step_one_soaking_cell(sand_t* s, uint8_t* row, int x, int y, int w, int h, const reaction_t* r) {
    const cell_t c = row[x];
    const uint8_t held = moisture_of(c, r);

    /* `>=` used, not `==`. Short-circuits on `soaked_to != 0`. */
    REACTION_DOC(soaked_to, "once fully saturated, at a per-step chance");
    const unsigned convert_period = (s->soak_convert > 0)
        ? (unsigned)s->soak_convert : SOAKED_CONVERT_PERIOD;
    if (r->soaked_to != 0 && held >= r->moist_max
        && (((unsigned)s->step_phase + (unsigned)x * 5u + (unsigned)y * 33u)
            & (convert_period - 1u)) == 0u
        && (int)(rng_next(&s->rng) & 0xFF) < r->soaked_chance) {
        const size_t at = (size_t)y * (size_t)w + (size_t)x;
        place_reacted(s, x, y, at, r->soaked_to);
        return true;
    }

    bool beside_liquid = false;

    /* Cullet is glass milled back to grains, with none of a dune's pore
     * space left, so it neither drinks nor binds into soil. Forced to zero
     * rather than skipped at the conversion itself so a shard standing in
     * water does not spend the water for nothing. */
    const int soaks = cell_is_cullet(c) ? 0 : ((s->soak >= 0) ? s->soak : r->soaks);

    /* LOCAL, NOT BOARD-WIDE. This walk looks only at the four orthogonal
     * neighbours, all one cell away, so any water it could find is in this
     * block or one touching it - exactly what BLOCK_LIQUID_NEAR covers. A
     * cell this rejects provably had nothing to find.
     *
     * RNG-NEUTRAL: the roll inside the walk is drawn only after a PAIR_WETS
     * neighbour is found, so a cell with no liquid neighbour draws nothing
     * and the stream is untouched. */
    if (soaks != 0 && r->soaks != 0 && liquid_near(s, x, y)) {
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

            REACTION_DOC(soaks_to, "unless the grain is cullet, which is glass and holds no water");
            if (r->soaks_to != 0) {
                s->cells[(size_t)y * (size_t)w + (size_t)x] =
                    soil_cell(CELL_MAKE(r->soaks_to, 0), 0, 1, &reactions[r->soaks_to]);
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
            if (nr->soaks == 0 || cell_is_cullet(n)) {
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
                s->cells[nat] = soil_cell(CELL_MAKE(nr->soaks_to, 0), 0, (uint8_t)give, &reactions[nr->soaks_to]);
                latch_content_flags(s, s->cells[nat]);
            } else if (same_species(n, c) && !cell_is_burning(n)) {
                /* Moisture_of() reads lit fuse as 0. Gap calc overwrites lit
                 * byte. */
                /* SIGNED ON PURPOSE, unlike the three halvings above: a
                 * WETTER neighbour makes this negative and the lines below
                 * depend on it, moving moisture the other way. Casting it
                 * unsigned turns a small negative into a huge positive
                 * (bd esp32c6-pz7). */
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
            if (br->soaks == 0 || cell_is_cullet(below)) {
                continue;
            }
            /* Lit fuse carve-out: prevent dousing */
            if (!cell_is_burning(below)
                && (br->soaks_to != 0 || (br->dries != 0 && moisture_of(below, br) < br->moist_max))) {
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
                s->cells[nat] = soil_cell(CELL_MAKE(br->soaks_to, 0), 0, (uint8_t)give, &reactions[br->soaks_to]);
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

/* COLD melts, pulls temp, cracks if hot. Chilling, melting snow. Warm liquid
 * aids survival. */
/* Bounds both conduction walks - conduct_heat() out of a burning cell and the
 * cold walk in step_one_cold_cell(). See "THE BOILER" for the rationale, and
 * note the two share it deliberately: a medium carries cold as far as it
 * carries heat. */
#define CONDUCT_REACH 32

/* How far COLD carries, which is no longer the same as heat.
 *
 * They shared CONDUCT_REACH on the argument that a medium carries cold as far
 * as it carries heat, and that stopped being true once cold got its own
 * attenuation run and, now, the diagonals. Thirty-two cells of cold read as
 * unrealistic in play - a third of it does not. */
#define COLD_REACH (CONDUCT_REACH / 3)

/* Cells the cold crosses per attenuation roll - THE ONE PLACE COLD BEATS HEAT,
 * which is what bd esp32c6-tov asked for. The heat walk rolls at every cell,
 * so at glass's conducts of 220 it clears CONDUCT_REACH about once in a
 * hundred; once per run of four makes that nearer one in three.
 *
 * Measured, 40x50 glass slab under snow at equilibrium: cells below ambient
 * 24% -> 64%, cells cold enough to shatter when warmed 12% -> 47%. */
#define COLD_CARRY_RUN 4

/* One carry attempt per cold cell every this many steps. The face a chiller
 * touches still cools every step; reaching deeper is rate-limited, so a slab
 * frosts over rather than reading as frozen the moment snow lands.
 *
 * A PERIOD, NOT A CHANCE: a 3-in-256 roll cost a draw on every cell every step
 * to say "no", and moved the shared RNG stream under every other rule on the
 * board. The x and y multipliers only spread the phase, so a drift narrower
 * than the period does not cool in one burst. Power of two. */
#define COLD_CARRY_PERIOD 512

/* A cell below ambient drifts back one level in this many steps.
 *
 * BOTH KNOBS ARE NEEDED: how deep the cold gets is the RATIO of cooling to
 * rewarming, not a race against time. Slowing the carry alone does not make a
 * slab take longer to freeze - it makes it never freeze.
 *
 * A period, not a divisor on the drain: cools is 5, so dividing lands on
 * 5, 2, 1, 0 and nothing between, and the only setting slower than a level a
 * step was never rewarming at all, which latches a slab cold forever. */
#define COLD_REWARM_PERIOD 32

static bool
step_one_cold_cell(sand_t* s, int x, int y, int w, int h, const reaction_t* r) {
    /* ONE DECISION FOR THE CELL, not one per direction - the question is
     * whether this cell sends cold onward this step, and asking it four times
     * would make the rate four times what it reads as. */
    const bool carries = (present_pair_bits & PAIR_CONDUCTS) != 0
        && r->chills != 0
        && (((unsigned)s->step_phase + (unsigned)x * 5u + (unsigned)y * 33u)
            & (COLD_CARRY_PERIOD - 1u)) == 0u;
    bool spent_on_heat = false;

    /* THE CARRY RUNS ON ALL EIGHT, unlike the contact loop below it.
     *
     * Conduction is proximity, and a cell touching at a corner is as close as
     * one touching at a face - a cardinals-only walk sent cold down columns
     * and rows and left the diagonals of a slab untouched, which reads as a
     * grid rather than as cold spreading.
     *
     * Lifted out of that loop so it is walked ONCE for the cell rather than
     * once per cardinal neighbour, which also makes the source's bill below
     * one per step instead of up to four. */
    if (carries) {
        for (int d = 0; d < 8; d++) {
            const int *dir = ring_dir(d);
            int cx = x, cy = y;
            for (int depth = 1; depth < COLD_REACH; depth++) {
                cx += dir[0];
                cy += dir[1];
                if ((unsigned)cx >= (unsigned)w || (unsigned)cy >= (unsigned)h) {
                    break;
                }
                const size_t cat = (size_t)cy * (size_t)w + (size_t)cx;
                const cell_t cc = s->cells[cat];
                if (CELL_IS_EMPTY(cc)) {
                    break;
                }
                const reaction_t* cr = reaction_of(cc);
                if (cr->conducts == 0 || cr->heat_ramp == 0) {
                    break;   /* the medium ends here */
                }
                const uint8_t ct = CELL_VARIANT(cc);
                if (ct == 0) {
                    continue;   /* already as cold as the scale goes */
                }
                if ((depth % COLD_CARRY_RUN) == 0
                    && (int)(rng_next(&s->rng) & 0xFF) >= cr->conducts) {
                    break;   /* the cold did not carry this far this step */
                }
                /* Drawn, not woken - see HEAT LEVELS DO NOT WAKE. This walk
                 * is where that rule was first found and paid for. */
                s->cells[cat] = CELL_MAKE(CELL_MATERIAL(cc), (uint8_t)(ct - 1));
                mark_rows(s, cy, cy);
                if (ct > SAND_AMBIENT_HEAT) {
                    spent_on_heat = true;
                }
            }
        }
    }

    /* THE SOURCE PAYS FOR THE DEPTH TOO. Cooling something HOT has always cost
     * the chilling cell - that is what stops snow being a free and permanent
     * heat sink, and a test is named for it. Reaching deeper without paying
     * deeper would quietly void that. */
    if (spent_on_heat && try_heat_transform(s, x, y, w, h)) {
        return false;
    }

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

        /* AND FROM WET SOIL, which is water too, just bound in grains. The
         * test above reads the neighbour's KIND, and dirt carries its water
         * as a moisture nibble, so a soaked bank looked dry to it.
         *
         * Scaled by wetness and halved: bound water reaches the snow more
         * slowly than free, and soil under about half saturation melts
         * nothing. The rate is computed BEFORE the roll so dry ground draws
         * no random number and cannot move the stream. */
        if (r->thaws != 0 && r->heats_to != 0 && nr->dries != 0 && nr->moist_max != 0) {
            const uint8_t wet = moisture_of(n, nr);
            const int rate = (int)r->thaws * (int)wet / ((int)nr->moist_max * 2);
            if (rate > 0 && (int)(rng_next(&s->rng) & 0xFF) < rate) {
                /* The soil pays for it, or one damp cell melts a whole bank. */
                s->cells[nat] = soil_set_moisture(n, (uint8_t)(wet - 1), 0);
                mark_rows(s, ny, ny);
                place_reacted(s, x, y, (size_t)y * (size_t)w + (size_t)x, (material_id_t)r->heats_to);
                return false;
            }
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
            continue; /* the face is as cold as it goes - but the walk above
                       * has already carried cold past it */
        }
        if ((int)(rng_next(&s->rng) & 0xFF) >= r->chills) {
            continue;
        }

        s->cells[nat] = CELL_MAKE(CELL_MATERIAL(n), (uint8_t)(temp - 1));
        s->may_have_temperature = true;
        mark_rows(s, ny, ny);   /* drawn, not woken - see HEAT LEVELS DO NOT WAKE */

        /* AND ON THROUGH THE MEDIUM. Cold stopped where it touched: snow on
         * glass chilled three rows and sat there, the same at 250 steps as at
         * 1000.
         *
         * conduct_heat() has walked conductors all along but only out of a
         * BURNING cell, so no cold source could enter it. This is its mirror,
         * at the same reach, attenuating on the conductor's own `conducts` so
         * nothing new needs tuning. Free where nothing conducts. */

        if (temp > SAND_AMBIENT_HEAT && try_heat_transform(s, x, y, w, h)) {
            return false;
        }
    }

    /* THE SOURCE PAYS FOR THE DEPTH, ONCE PER STEP. Cooling something HOT has
     * always cost the chilling cell - that is what stops snow being a free and
     * permanent heat sink, and a test is named for it. The walk cools panes
     * several cells in, so billing only the face it touches would void that.
     *
     * Once per step, NOT per direction: per direction billed a block of ice up
     * to four melts a step and melted it out of the column it was put in. */
    if (spent_on_heat) {
        (void)try_heat_transform(s, x, y, w, h);
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
        mark_rows(s, ny, ny);   /* drawn, not woken - see HEAT LEVELS DO NOT WAKE */
    }

    /* MULTIPLIES DRAIN BY SAND_WET_COOLING_FACTOR (sand.h) */

    /* GATED TO temp > SAND_AMBIENT_HEAT - branch below untouched by `wet`. */

    /* Water cannot fall below ambient, preventing SAND_SHOCK_COLD. Snow
     * retains that role. */
    unsigned drain = r->cools;
    if (temp < SAND_AMBIENT_HEAT
        && (((unsigned)s->step_phase + (unsigned)x * 17u + (unsigned)y * 3u)
            & (COLD_REWARM_PERIOD - 1u)) != 0u) {
        drain = 0;   /* not this cell's step to warm back up */
    }
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
    mark_rows(s, y, y);   /* drawn, not woken - see HEAT LEVELS DO NOT WAKE */
    return next != SAND_AMBIENT_HEAT;
}

/* How this cell is exposed: on a foreign face, on its own crust, or neither.
 *
 * A crust is a SHELL that GROWS INWARD, counted apart because the two run at
 * very different rates. A foreign face is where ice starts; a face on ice
 * already formed is how the shell thickens, far slower or the front eats the
 * bank.
 *
 * Neither face is interior, and never crusts. AIR IS NOT A FACE: count it and
 * a drift rims its whole outline in ice. Off-grid, likewise. */
#define FACE_FOREIGN 1u
#define FACE_CRUST   2u

/* How much crust a cell must be backed by before it joins one, over all eight
 * neighbours.
 *
 * EIGHT, BECAUSE FOUR CANNOT EXPRESS IT. A snow cell on a fully iced row has
 * one orthogonal ice neighbour, so any threshold above one stalls a flat front
 * and the cover never finishes. Over eight it has three, so three is the least
 * that advances a flat front while refusing a cell brushing a corner. */
#define CRUST_WIDEN_MIN_ICE 3

static inline unsigned
crust_faces(const sand_t* s, int x, int y, int w, int h, uint8_t mine,
            uint8_t becomes) {
    unsigned faces = 0;
    unsigned crust_seen = 0;
    bool open = false;
    for (int d = 0; d < 8; d++) {
        const int *dir = ring_dir(d);
        const int nx = x + dir[0];
        const int ny = y + dir[1];
        if ((unsigned)nx >= (unsigned)w || (unsigned)ny >= (unsigned)h) {
            continue;
        }
        const cell_t n = s->cells[(size_t)ny * (size_t)w + (size_t)nx];
        if (CELL_IS_EMPTY(n)) {
            open = true;
            continue;
        }
        const uint8_t m = CELL_MATERIAL(n);
        if (m == becomes) {
            crust_seen++;
        } else if (m != mine && (dir[0] == 0 || dir[1] == 0)) {
            /* A FOREIGN face stays orthogonal: touching a wall at the corner
             * is not resting against it, and counting it ices the diagonal
             * staircase a poured pile leaves along any slope. */
            faces |= FACE_FOREIGN;
        }
    }
    /* THE RIM STAYS SNOW: open space anywhere around it makes this the drift's
     * own surface, and a surface does not thicken a crust forming underneath
     * it. Only widening is held to this - a cell pressed against a wall still
     * seeds, or a cover would never start. */
    if (!open && crust_seen >= CRUST_WIDEN_MIN_ICE) {
        faces |= FACE_CRUST;
    }
    return faces;
}

/* How often each of the two paths gets to roll. Periods, not divisors on the
 * chance: crusts is a small count, so dividing floors to zero.
 *
 * SEEDING NEEDS A CLOCK TOO. Ungated it rolls every step, so a whole contact
 * face turned within a second of settling while the shell behind it took
 * minutes - the crust appeared rather than formed. Still the faster of the
 * two, being what starts a shell, but no longer instant.
 *
 * The stagger multipliers differ per path so the two do not come due
 * together. */
#define CRUST_SEED_PERIOD  4
#define CRUST_WIDEN_PERIOD 8

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
    if (s->impulse_buf != NULL && material_of(n)->kind == KIND_GAS && gas_ignite_confined(s, nx, ny, w, h)) {
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

/* Tries the cell against gravity, then its two neighbours, never sideways
 * or down. Not emit_into_empty_neighbor(): its screen-space order can put
 * fire beside/beneath lava and breaks under tilt. Not straight-up-only:
 * measured, 6% of rolls land vs 14% for this spread, costing the
 * thermal-shock scene 59% of its fire (827/2000 cells); raising `flare`
 * can't substitute - it sets a rate, not a density.
 *
 * Uses last_step, not last_load, matching try_flare()'s "below" check one
 * line earlier. */
static inline bool
emit_against_gravity(sand_t* s, int x, int y, int w, int h, uint8_t spec) {
    const int dx = s->last_step_dx, dy = s->last_step_dy;
    if (dx == 0 && dy == 0) {
        return false;   /* no down yet, so no up to rise into */
    }
    const int up = ring_of(-dx, -dy);

    /* Straight on before either shoulder - the same ordering find_water()
     * uses when it walks gravity-ward. */
    for (int k = 0; k < 3; k++) {
        const int* d = ring_dir(up + (k == 0 ? 0 : k == 1 ? 1 : 7));
        const int ux = x + d[0], uy = y + d[1];
        if ((unsigned)ux >= (unsigned)w || (unsigned)uy >= (unsigned)h) {
            continue;
        }
        const size_t at = (size_t)uy * (size_t)w + (size_t)ux;
        if (!CELL_IS_EMPTY(s->cells[at])) {
            continue;
        }
        place_reacted(s, ux, uy, at, spec);
        return true;
    }
    return false;
}

static inline bool
try_flare(sand_t* s, int x, int y, int w, int h, const material_t* mat, uint8_t flare) {
    if (flare == 0) {
        return false;
    }
    if (mat->kind != KIND_STATIC) {
        const cell_t below = sand_at(s, x + s->last_step_dx, y + s->last_step_dy);
        if (CELL_IS_EMPTY(below)) {
            return false;
        }
    }
    if ((int)(rng_next(&s->rng) & 0xFF) >= flare) {
        return false;
    }
    return emit_against_gravity(s, x, y, w, h, MAT_FIRE);
}

/* Attempts direct connection, fails. Rolls `conducts` to CONDUCT_REACH. Stops
 * on failure, off-grid, or empty. Liquid boils, fuel ignites, neighbors warm.
 * Returns status. */
static inline bool
conduct_heat(sand_t* s, int x, int y, int w, int h) {
    bool acted = false;

    /* SKIPPED WHOLE when nothing on the board conducts. Host counters: on a
     * full screen of fire this walk is entered 41216 times a step and finds
     * nothing every single time, and the phase split prices it at 36725 us,
     * 18% of that scene.
     *
     * RNG-NEUTRAL: the conduction roll sits inside the depth loop, which is
     * only reached once a neighbour has passed the PAIR_CONDUCTS reject - so
     * a board with no conductor draws nothing and the stream is untouched. */
    if ((present_pair_bits & PAIR_CONDUCTS) == 0) {
        return false;
    }

    for (int d = 0; d < 4; d++) {
        const int dx = reaction_dirs[d][0];
        const int dy = reaction_dirs[d][1];
        int rx = x + dx;
        int ry = y + dy;

        if ((unsigned)rx >= (unsigned)w || (unsigned)ry >= (unsigned)h) {
            continue;
        }
        const cell_t first = s->cells[(size_t)ry * (size_t)w + (size_t)rx];
        if (CELL_IS_EMPTY(first)) {
            continue;
        }
        /* Early-out - most neighbours are not conductors. Reading the bit
         * table rather than reaction_of()->conducts keeps the reject in
         * SRAM: reactions[] is flash-resident DROM behind the i-cache and
         * its 61-byte stride costs a multiply, which measured as 18% of a
         * fire step for four rejects that do nothing. */
        if ((pair_theirs_bits(CELL_MATERIAL(first)) & PAIR_CONDUCTS) == 0) {
            continue;
        }

        bool got_through = false;
        for (int depth = 0; depth < CONDUCT_REACH; depth++) {
            const cell_t here = s->cells[(size_t)ry * (size_t)w + (size_t)rx];
            /* The bit table folds all sixteen extended variants into one
             * MAT_EXTENDED slot, so the reject above passes any extended
             * cell once metal sets the bit. Re-testing here keeps that
             * approximation from reaching the roll below, which would spend
             * an RNG draw the exact test never spent. Dead for depth > 0:
             * the loop only advances into cells it has already found to
             * conduct. */
            const int own = reaction_of(here)->conducts;
            if (own == 0) {
                break;
            }
            const int c = (s->conduction >= 0) ? s->conduction : own;
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
        && (!per_material || (rng_next(&s->rng) & 63u) == 0)) {
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
        /* Cullet is glass milled to grains, and MAT_GLASS has no
         * .dissolvable - acid cannot touch a pane whole. It shares
         * MAT_SAND's row, though, so the material table alone cannot say
         * cullet is different; reject it here explicitly. */
        if (cell_is_cullet(n)) {
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
            const int mass_bias =
                (s->acid_dilute_mass_bias >= 0) ? s->acid_dilute_mass_bias : SAND_ACID_DILUTE_MASS_BIAS;
            const int water_wins_chance = SAND_ACID_DILUTE_TO_WATER_CHANCE + (water_backing - acid_backing) * mass_bias;

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
            const uint8_t oil_residue =
                ((int)(rng_next(&s->rng) & 0xFF) < SAND_ACID_OIL_TO_GAS_CHANCE) ? MAT_GAS : MAT_ACID;
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
find_lit_two_by_two(const sand_t* s, int x, int y, int w, int h, cell_t grain, const reaction_t* r, int* out_dx,
                    int* out_dy) {
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
            REACTION_DOC(
                explodes,
                "at burn-out, if it is one corner of a 2x2 that is all lit and the board's blast cooldown has run out");
            int dx = 0, dy = 0;
            if (s->impulse_buf != NULL && s->fuse_blast_wait == 0
                && find_lit_two_by_two(s, x, y, w, h, grain, rx, &dx, &dy)) {
                s->fuse_blast_wait =
                    (uint8_t)((s->fuse_cooldown >= 0) ? s->fuse_cooldown : SAND_GUNPOWDER_BLAST_COOLDOWN);
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
                    uint8_t product = quench_to;
                    bool leaves_residue = true;
                    if (mat_id == MAT_FIRE) {
                        const size_t nat = (size_t)ny * (size_t)w + (size_t)nx;
                        const uint8_t liquid_boils_to = reaction_of(s->cells[nat])->boils_to;
                        if (liquid_boils_to == MAT_GAS) {
                            leaves_residue = (int)(rng_next(&s->rng) & 0xFF) < SAND_ACID_QUENCH_RESIDUE_CHANCE;
                            product =
                                ((int)(rng_next(&s->rng) & 0xFF) < SAND_ACID_QUENCH_SMOKE_CHANCE) ? MAT_SMOKE : MAT_GAS;
                        } else {
                            product = liquid_boils_to ? liquid_boils_to : MAT_STEAM;
                        }
                    }
                    if (leaves_residue) {
                        place_reacted(s, x, y, at, product);
                        /* Gated on KIND_LIQUID. Only lava quenches AS liquid. */
                        if (mat->kind == KIND_LIQUID) {
                            const int lava_cooloff =
                                (s->lava_cooloff >= 0) ? s->lava_cooloff : SAND_LAVA_COOLOFF_CHANCE;
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

    /* SKIPPED WHOLE when nothing on the board is a denser non-liquid: every
     * one of smothered()'s four probes would reject, and it draws no random
     * number, so the skip is RNG-neutral outright rather than by argument.
     *
     * Measured: on a full screen of fire this test is reached 41216 times a
     * step and has NEVER once smothered. */
    if (mat->kind != KIND_LIQUID && rx->explodes == 0
        && mat->density < max_smothering_density
        && smothered(s, x, y, w, h, mat->density)) {
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
    if (is_lava && burst_chance != 0 && (int)(rng_next(&s->rng) & 0xFF) < burst_chance
        && (!burst_natural || (rng_next(&s->rng) % SAND_LAVA_BURST_GATE) == 0)
        && covered_at(s, x, y, w, h, mat->density)) {
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
                                 ? ((s->lava_cooloff >= 0) ? s->lava_cooloff : SAND_LAVA_COOLOFF_CHANCE)
                                 : 0;
    /* SKIPPED WHOLE when nothing on the board can be paired WITH. On a full
     * screen of fire this is every cell, every step: measured 41216 walks and
     * 41216 of them finding nothing, four bounds-checked probes each.
     *
     * A NEIGHBOUR PROPERTY, which is what makes one flag enough:
     * pair_bits[mine][theirs] does not depend on mine - the table is sixteen
     * identical rows - so "can anything here be acted on" is the same
     * question for every cell doing the looking.
     *
     * RNG-NEUTRAL: both draws inside this walk sit behind a non-zero pair
     * byte, so a board with none draws nothing and the stream is untouched. */
    if ((present_pair_bits & (PAIR_IGNITABLE | PAIR_HEAT_RESPONSIVE)) == 0) {
        goto pair_done;
    }

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
                if (lava_cooloff != 0 && CELL_MATERIAL(s->cells[nat]) != before_mat
                    && (int)(rng_next(&s->rng) & 0xFF) < lava_cooloff) {
                    place_reacted(s, x, y, at, rx->quench_to);
                    cool_off_chain(s, x, y, w, h, rx->quench_to, lava_cooloff);
                    return true;
                }
            }
        }
    }

pair_done:
    if (conduct_heat(s, x, y, w, h)) {
        acted = true;
    }

    /* rx, not a second reaction_of(grain): decay only ever rewrites the code
     * nibble, so the row is the same one and re-deriving it costs a flash
     * dereference per burning cell for nothing. */
    if (try_flare(s, x, y, w, h, mat, rx->flare)) {
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
    const size_t at = (size_t)y * (size_t)w + (size_t)x;
    const size_t at_r = at + 1;
    const size_t at_d = at + (size_t)w;
    const size_t at_dr = at_d + 1;
    const uint8_t mat_id = CELL_MATERIAL(s->cells[at]);

    if (CELL_IS_EMPTY(s->cells[at_r]) || CELL_MATERIAL(s->cells[at_r]) != mat_id || CELL_IS_EMPTY(s->cells[at_d])
        || CELL_MATERIAL(s->cells[at_d]) != mat_id || CELL_IS_EMPTY(s->cells[at_dr])
        || CELL_MATERIAL(s->cells[at_dr]) != mat_id) {
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
    /* SKIPPED WHOLE when the board cannot hold a quad. Four cell loads and
     * four material decodes per gas or steam cell otherwise, every step, to
     * rediscover that the same thing is missing.
     *
     * RNG-NEUTRAL: the roll sits below the quad tests, so a board that cannot
     * form one draws nothing and the stream is untouched.
     *
     * Measured: a screen of smoke and steam reaches this 204247 times a run
     * and forms a quad NEVER - it holds no gas at all. */
    if (!acid_rain_possible) {
        return false;
    }
    if (x + 1 >= w || y + 1 >= h) {
        return false;
    }
    const size_t at = (size_t)y * (size_t)w + (size_t)x;
    const size_t at_r = at + 1;
    const size_t at_d = at + (size_t)w;
    const size_t at_dr = at_d + 1;
    const uint8_t m0 = CELL_MATERIAL(s->cells[at]);
    const uint8_t m1 = CELL_MATERIAL(s->cells[at_r]);
    const uint8_t m2 = CELL_MATERIAL(s->cells[at_d]);
    const uint8_t m3 = CELL_MATERIAL(s->cells[at_dr]);

    if ((m0 != MAT_STEAM && m0 != MAT_GAS) || (m1 != MAT_STEAM && m1 != MAT_GAS) || (m2 != MAT_STEAM && m2 != MAT_GAS)
        || (m3 != MAT_STEAM && m3 != MAT_GAS)) {
        return false;
    }
    const int steam_count = (m0 == MAT_STEAM) + (m1 == MAT_STEAM) + (m2 == MAT_STEAM) + (m3 == MAT_STEAM);
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
        &&stage_burn_any, &&stage_burn_always, &&stage_burn_check, &&stage_dissolve, &&stage_acid_rain,
        &&stage_condense, &&stage_heat_ramp,   &&stage_crust,      &&stage_chill,    &&stage_warm,
        &&stage_soak_dry,
        &&stage_fall,     &&stage_drink,       &&stage_root,       &&stage_grow,
        &&stage_sprout,   &&stage_bud,         &&stage_end,
    };

    unsigned found = 0;
    for (int x = 0; x < w; x++) {
        const cell_t c = row[x];
        seen_materials |= (uint16_t)(1u << CELL_MATERIAL(c));
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
    stage_crust:
        /* THE ROLL IS LAST on purpose: drawn before the settled test it would
         * shift the random stream for every scene whether or not snow is
         * present, moving every baselined hash for a rule that did nothing.
         *
         * Converts in place and deliberately does NOT wake. Waking would clear
         * BLOCK_SETTLED on the very bank whose stillness allowed this, so the
         * crust would form one cell and stall; and nothing needs waking,
         * because snow becoming ice only makes the board more solid. */
        const unsigned faces = (r->crusts != 0 && cell_settled(s, x, y))
            ? crust_faces(s, x, y, w, h, CELL_MATERIAL(c),
                          CELL_MATERIAL((cell_t)r->crusts_to))
            : 0u;
        const unsigned phase = (unsigned)s->step_phase;
        const bool seed_due = ((faces & FACE_FOREIGN) != 0)
            && ((phase + (unsigned)x * 11u + (unsigned)y * 7u)
                & (CRUST_SEED_PERIOD - 1u)) == 0u;
        const bool widen_due = ((faces & FACE_CRUST) != 0)
            && ((phase + (unsigned)x * 5u + (unsigned)y * 33u)
                & (CRUST_WIDEN_PERIOD - 1u)) == 0u;
        const bool may_crust = seed_due || widen_due;
        if (may_crust
            && (int)(rng_next(&s->rng) & (CRUST_ROLL_MAX - 1)) < ((s->crust >= 0) ? s->crust : r->crusts)) {
            REACTION_DOC(crusts_to, "what a settled cell slowly crusts into");
            row[x] = (cell_t)r->crusts_to;
            latch_content_flags(s, row[x]);
            mark_rows(s, y, y);
            continue;
        }
        /* Falls through: snow that did not crust this step still chills. */
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
        if ((r->soaks != 0 || r->dries != 0)
            && step_one_soaking_cell(s, row, x, y, w, h, r)) {
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

/* BUILT ONCE, NOT PER STEP. Every one of these is a pure function of
 * reactions[], extended_reactions[] and materials[] - all const, all
 * flash-resident, none of them reachable by anything at runtime - yet the
 * whole lot was rebuilt on every step that got past the seven-flag early
 * out. reaction_first_stage() alone is a seventeen-field ladder run
 * thirty-two times, and pair_bits is 256 stores.
 *
 * A step's own cost is unchanged by this on a busy board; what it removes is
 * a fixed toll on every step of every scene that has anything reacting at
 * all. */
static bool reaction_tables_ready;

static void build_reaction_tables(void)
{
    if (reaction_tables_ready) {
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
        if (r->conducts != 0) {
            theirs_bits[m] |= PAIR_CONDUCTS;
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
        if (r->conducts != 0) {
            theirs_bits[MAT_EXTENDED] |= PAIR_CONDUCTS;
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

    reaction_tables_ready = true;
}

void
sand_step_reactions(sand_t* s) {
    if (s->fuse_blast_wait != 0) {
        s->fuse_blast_wait--;
    }
    /* Dissolving, not fire. Heat, condensation independent. */
    if (!s->may_have_burning && !s->may_have_dissolver && !s->may_have_temperature && !s->may_have_moisture
        && !s->may_have_faller && !s->may_have_condenser) {
        return;
    }

    build_reaction_tables();

    /* Materials -> bits, once, here: this is the first point in a step where
     * the table is known built, and the passes below read the result per
     * cell. */
    present_pair_bits = 0;
    max_smothering_density = 0;
    acid_rain_possible = (s->may_have_materials & (1u << MAT_STEAM)) != 0
                      && (s->may_have_materials & (1u << MAT_GAS)) != 0;
    for (int m = 0; m < MATERIAL_MAX; m++) {
        if ((s->may_have_materials & (1u << m)) == 0) {
            continue;
        }
        present_pair_bits |= pair_theirs_bits((uint8_t)m);

        /* MAT_EXTENDED is one bit over several materials, so it contributes
         * the densest of them - the conservative direction for a skip. */
        if (m == MAT_EXTENDED) {
            for (int k = 0; k < MATERIAL_EXTENDED_CODES; k++) {
                const material_t* em = material_of(MATX(k));
                if (em->kind != KIND_LIQUID && em->density > max_smothering_density) {
                    max_smothering_density = em->density;
                }
            }
            continue;
        }
        const material_t* mm = material_of(CELL_MAKE((uint8_t)m, 0));
        if (mm->kind != KIND_LIQUID && mm->density > max_smothering_density) {
            max_smothering_density = mm->density;
        }
    }
    /* CLEARED HERE so a bit latch_content_flags() ORs in mid-pass survives the
     * write-back below. Assigning the walk's census there instead dropped any
     * cell this pass CREATED at its own coordinates - the walk logged the old
     * material and never returns (bd esp32c6-cxx).
     *
     * Only the mask can be cleared here. The six may_have_* bools are read
     * per cell as live gates by stage_warm and the plant stages, so they keep
     * the clear-at-the-end rule below. */
    s->may_have_materials = 0;
    seen_materials = 0;

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
    if (!(found & FOUND_CONDENSING)) {
        s->may_have_condenser = false;
    }
    /* Same shape as the flags above: OR, so a material created mid-pass by
     * place_cell() keeps the bit it just latched. */
    s->may_have_materials |= seen_materials;

    /* may_have_heat_holder NOT cleared; clearing at end is wrong. */

    /* may_have_heat_holder: cell with heat_ramp can be created during this
     * pass. */

    /* Clearing wipes flag; convection dead. */

    /* Measured: arm-only version byte-identical to HEAD on eight scenes. */

    /* Nobody pays per-step upkeep once armed. */
}
