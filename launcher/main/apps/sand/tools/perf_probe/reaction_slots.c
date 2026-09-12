/* Scratch instrumentation - see reaction_slots.h. */
#include "reaction_slots.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

static const char* const slot_field_names[F_COUNT] = {
    "flammability", "needs_air", "burns",       "burn_decay",    "residue",     "flare", "explodes",    "dissolves",
    "dissolvable",  "fizz",      "evaporates",  "condenses",     "heat_chance", "melts", "flaw_chance", "spoils_chance",
    "heat_ramp",    "cools",     "chills",      "conducts",      "boils",       "warms", "thaws",       "wets",
    "soaks",        "dries",     "soil",        "soaked_chance", "grows",       "falls", "harden_run",  "harden_chance",
    "roots",        "canopy",    "trunk_girth", "holds_line",    "sprouts",     "buds",  "drinks",
};

#define SLOT_ROWS ((MAT_COUNT - 1) + MATERIAL_EXTENDED_COUNT + 1)

static const char* slot_row_names[SLOT_ROWS];
static int slot_row_count;
static bool slot_hit[SLOT_ROWS][F_COUNT];

/* Row order matches dump_reactions.c's build_rows(): plain materials
 * MAT_SAND..MAT_COUNT-1, then named extended slots, then one Gunpowder
 * row - see that function's own comment for why gunpowder is one row. */
void
slot_rows_init(void) {
    slot_row_count = 0;
    for (uint8_t m = MAT_SAND; m < MAT_COUNT; m++) {
        slot_row_names[slot_row_count++] = material_by_id((material_id_t)m)->name;
    }
    for (uint8_t k = 0; k < MATERIAL_EXTENDED_COUNT; k++) {
        const char* nm = material_name(MATX(k));
        if (nm[0] == '?' && nm[1] == '\0') {
            continue;
        }
        slot_row_names[slot_row_count++] = nm;
    }
    slot_row_names[slot_row_count++] = "Gunpowder";
    memset(slot_hit, 0, sizeof(slot_hit));
}

/* Maps a raw cell byte to a row index the same way build_rows() would
 * classify its owning row: plain materials by id offset, extended statics
 * by name lookup, gunpowder to the one shared row. */
int
slot_row_of(cell_t c) {
    const uint8_t m = CELL_MATERIAL(c);
    if (m != MAT_EXTENDED) {
        if (m < MAT_SAND || m >= MAT_COUNT) {
            return -1;
        }
        return (int)(m - MAT_SAND);
    }
    if (cell_is_gunpowder(c)) {
        return slot_row_count - 1;
    }
    const char* nm = material_name(c);
    for (int i = 0; i < slot_row_count; i++) {
        if (strcmp(slot_row_names[i], nm) == 0) {
            return i;
        }
    }
    return -1;
}

void
slot_note(cell_t which, slot_field_t f) {
    const int row = slot_row_of(which);
    if (row < 0) {
        return;
    }
    slot_hit[row][f] = true;
}

/* Most call sites only have a `const reaction_t *` (r/nr/rx/br/cr) in
 * scope, not a cell_t - resolved by pointer identity against reactions[]/
 * extended_reactions[] instead of threading a cell_t through every
 * helper this file's instrumentation touches. */
static int
slot_row_of_reaction(const reaction_t* r) {
    for (uint8_t m = MAT_SAND; m < MAT_COUNT; m++) {
        if (&reactions[m] == r) {
            return (int)(m - MAT_SAND);
        }
    }
    for (uint8_t k = 0; k < MATERIAL_EXTENDED_COUNT; k++) {
        if (&extended_reactions[k] == r) {
            return slot_row_of(MATX(k));
        }
    }
    for (int k = MATERIAL_EXTENDED_COUNT; k < MATERIAL_EXTENDED_CODES; k++) {
        if (&extended_reactions[k] == r) {
            return slot_row_count - 1;
        }
    }
    return -1;
}

void
slot_note_r(const reaction_t* r, slot_field_t f) {
    const int row = slot_row_of_reaction(r);
    if (row < 0) {
        return;
    }
    slot_hit[row][f] = true;
}

void
slot_dump(FILE* out) {
    int n = 0;
    for (int row = 0; row < slot_row_count; row++) {
        for (int f = 0; f < F_COUNT; f++) {
            if (slot_hit[row][f]) {
                fprintf(out, "%s.%s\n", slot_row_names[row], slot_field_names[f]);
                n++;
            }
        }
    }
    fprintf(out, "TOTAL %d\n", n);
}
