/* Scratch tool: enumerate (material row, reaction_t field) pairs whose
 * gating condition is satisfied (byte != 0), the way dump_reactions.c's
 * build_rows() walks reactions[]/extended_reactions[]. Excludes TARGET
 * fields (ignites_to etc - say WHERE, not WHETHER) and pure-encoding
 * fields (lit_from, tones, moist_max). Prints each slot plus total M. */

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "material.h"

typedef enum { KIND_GATE, KIND_TARGET, KIND_ENCODING } slot_kind_t;

typedef struct {
    size_t offset;
    const char* name;
    slot_kind_t kind;
} field_row_t;

#define G(f) {offsetof(reaction_t, f), #f, KIND_GATE}
#define T(f) {offsetof(reaction_t, f), #f, KIND_TARGET}
#define E(f) {offsetof(reaction_t, f), #f, KIND_ENCODING}

static const field_row_t fields[] = {
    G(flammability), T(ignites_to),    G(needs_air),     G(burns),       G(burn_decay),    E(lit_from), G(residue),
    T(quench_to),    G(flare),         G(explodes),      G(dissolves),   G(dissolvable),   G(fizz),     G(evaporates),
    G(condenses),    T(condenses_to),  T(heats_to),      G(heat_chance), G(melts),         T(flaw_to),  G(flaw_chance),
    T(spoils_to),    G(spoils_chance), G(heat_ramp),     G(cools),       G(chills),        G(conducts), G(boils),
    T(boils_to),     G(warms),         G(thaws),         G(wets),        G(soaks),         T(soaks_to), G(dries),
    G(soil),         E(tones),         E(moist_max),     T(soaked_to),   G(soaked_chance), G(grows),    G(falls),
    T(hardens_to),   G(harden_run),    G(harden_chance), T(clings_to),   G(roots),         T(roots_to), G(canopy),
    T(canopy_to),    G(trunk_girth),   G(holds_line),    G(sprouts),     T(sprouts_to),    G(buds),     T(buds_to),
    G(drinks),       T(shatters_to),
};
#define NFIELDS (sizeof(fields) / sizeof(fields[0]))

/* Byte-for-byte the same as dump_reactions.c's field_docs[] gate: this array
 * must claim every offset in reaction_t exactly once. */
static void
check_complete(void) {
    static int seen[512];
    for (size_t i = 0; i < NFIELDS; i++) {
        if (fields[i].offset >= sizeof(reaction_t)) {
            fprintf(stderr, "offset out of range: %s\n", fields[i].name);
            exit(1);
        }
        if (seen[fields[i].offset]) {
            fprintf(stderr, "duplicate offset: %s\n", fields[i].name);
            exit(1);
        }
        seen[fields[i].offset] = 1;
    }
    for (size_t off = 0; off < sizeof(reaction_t); off++) {
        if (!seen[off]) {
            fprintf(stderr, "reaction_t offset %zu not covered by fields[]\n", off);
            exit(1);
        }
    }
    if (NFIELDS != sizeof(reaction_t)) {
        fprintf(stderr, "NFIELDS=%zu != sizeof(reaction_t)=%zu\n", NFIELDS, sizeof(reaction_t));
        exit(1);
    }
}

typedef struct {
    const char* name;
    const reaction_t* r;
} mrow_t;

static mrow_t all_rows[(MAT_COUNT - 1) + MATERIAL_EXTENDED_COUNT + 1];
static size_t all_rows_count;

static void
build_rows(void) {
    all_rows_count = 0;
    for (uint8_t m = MAT_SAND; m < MAT_COUNT; m++) {
        all_rows[all_rows_count].name = material_by_id((material_id_t)m)->name;
        all_rows[all_rows_count].r = &reactions[m];
        all_rows_count++;
    }
    for (uint8_t k = 0; k < MATERIAL_EXTENDED_COUNT; k++) {
        const char* nm = material_name(MATX(k));
        if (nm[0] == '?' && nm[1] == '\0') {
            continue;
        }
        all_rows[all_rows_count].name = nm;
        all_rows[all_rows_count].r = &extended_reactions[k];
        all_rows_count++;
    }
    all_rows[all_rows_count].name = "Gunpowder";
    all_rows[all_rows_count].r = &extended_reactions[8];
    all_rows_count++;
}

int
main(void) {
    check_complete();
    build_rows();

    size_t m = 0;
    for (size_t ri = 0; ri < all_rows_count; ri++) {
        const unsigned char* bytes = (const unsigned char*)all_rows[ri].r;
        for (size_t fi = 0; fi < NFIELDS; fi++) {
            if (fields[fi].kind != KIND_GATE) {
                continue;
            }
            if (bytes[fields[fi].offset] != 0) {
                printf("%s.%s\n", all_rows[ri].name, fields[fi].name);
                m++;
            }
        }
    }
    fprintf(stderr, "M = %zu\n", m);
    return 0;
}
