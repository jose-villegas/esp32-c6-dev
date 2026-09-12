/* Scratch instrumentation, not part of the real tree: which (material row,
 * reaction_t field) slots actually fire at runtime. Field list mirrors
 * count_slots.c's own classification (target/encoding fields excluded).
 * Row order mirrors dump_reactions.c's build_rows(). Real state lives in
 * reaction_slots.c (external linkage) so every translation unit that
 * includes this header shares the same counters - see that file. */
#ifndef REACTION_SLOTS_H
#define REACTION_SLOTS_H

#include <stdio.h>

#include "material.h"

typedef enum {
    F_flammability,
    F_needs_air,
    F_burns,
    F_burn_decay,
    F_residue,
    F_flare,
    F_explodes,
    F_dissolves,
    F_dissolvable,
    F_fizz,
    F_evaporates,
    F_condenses,
    F_heat_chance,
    F_melts,
    F_flaw_chance,
    F_spoils_chance,
    F_heat_ramp,
    F_cools,
    F_chills,
    F_conducts,
    F_boils,
    F_warms,
    F_thaws,
    F_wets,
    F_soaks,
    F_dries,
    F_soil,
    F_soaked_chance,
    F_grows,
    F_falls,
    F_harden_run,
    F_harden_chance,
    F_roots,
    F_canopy,
    F_trunk_girth,
    F_holds_line,
    F_sprouts,
    F_buds,
    F_drinks,
    F_COUNT
} slot_field_t;

void slot_rows_init(void);
void slot_note(cell_t which, slot_field_t f);
void slot_note_r(const reaction_t* r, slot_field_t f);
void slot_dump(FILE* out);

#endif
