"""Instrument the sand reaction firing sites with slot_note_r() calls.

Takes any number of .c files, or a directory whose *.c files are all
candidates. Each edit is placed in whichever file actually contains its
anchor, so a source split (sand_reactions.c -> sand_plants.c, and
whatever comes next) needs no anchor rewriting - just pass the directory.

Exits NON-ZERO if any edit finds no home. That matters more than it
looks: an unplaced edit means those reactions are never counted, and the
coverage report then shows them as "never fires" rather than as "not
measured". The 2026-09 sand_plants.c split would have silently zeroed
16 plant slots exactly that way.
"""
import sys, os, glob

args = sys.argv[1:]
paths = []
for a in args:
    if os.path.isdir(a):
        paths.extend(sorted(glob.glob(os.path.join(a, "*.c"))))
    else:
        paths.append(a)
if not paths:
    sys.exit("usage: apply_edits.py <file.c | dir> ...")

texts = {}
for p in paths:
    with open(p, "r", encoding="utf-8", errors="ignore") as f:
        texts[p] = f.read()

edits = []

def add(old, new, label):
    edits.append((old, new, label))

add(
'#include "sand_priv.h"\n#include "reaction_doc.h"',
'#include "sand_priv.h"\n#include "reaction_doc.h"\n#include "reaction_slots.h"',
"include")

add(
"""        if ((int)(rng_next(&s->rng) & 0xFF) >= r->heat_ramp) {
            return false;
        }
        const uint8_t heat = CELL_VARIANT(n);""",
"""        if ((int)(rng_next(&s->rng) & 0xFF) >= r->heat_ramp) {
            return false;
        }
        slot_note_r(r, F_heat_ramp);
        const uint8_t heat = CELL_VARIANT(n);""",
"heat_ramp")

add(
"""    if ((int)(rng_next(&s->rng) & 0xFF) >= r->heat_chance) {
        return false;
    }

    /* WET EARTH FIRST.""",
"""    if ((int)(rng_next(&s->rng) & 0xFF) >= r->heat_chance) {
        return false;
    }
    slot_note_r(r, F_heat_chance);

    /* WET EARTH FIRST.""",
"heat_chance")

add(
"""        if (r->spoils_to != 0 && (int)(rng_next(&s->rng) & 0xFF) < r->spoils_chance) {
            place_reacted(s, nx, ny, at, (material_id_t)r->spoils_to);
            return true;
        }""",
"""        if (r->spoils_to != 0 && (int)(rng_next(&s->rng) & 0xFF) < r->spoils_chance) {
            slot_note_r(r, F_spoils_chance);
            place_reacted(s, nx, ny, at, (material_id_t)r->spoils_to);
            return true;
        }""",
"spoils_chance")

add(
"""        s->heat_flaw_seq++;
        if (s->heat_flaw_is_flawed) {
            yield = (material_id_t)r->flaw_to;
        }""",
"""        s->heat_flaw_seq++;
        if (s->heat_flaw_is_flawed) {
            slot_note_r(r, F_flaw_chance);
            yield = (material_id_t)r->flaw_to;
        }""",
"flaw_chance")

add(
"""        if (r->thaws != 0 && r->heats_to != 0 && material_of(n)->kind == KIND_LIQUID
            && (int)(rng_next(&s->rng) & 0xFF) < r->thaws) {
            place_reacted(s, x, y, (size_t)y * (size_t)w + (size_t)x, (material_id_t)r->heats_to);
            return false;
        }""",
"""        if (r->thaws != 0 && r->heats_to != 0 && material_of(n)->kind == KIND_LIQUID
            && (int)(rng_next(&s->rng) & 0xFF) < r->thaws) {
            slot_note_r(r, F_thaws);
            place_reacted(s, x, y, (size_t)y * (size_t)w + (size_t)x, (material_id_t)r->heats_to);
            return false;
        }""",
"thaws")

add(
"""        if ((int)(rng_next(&s->rng) & 0xFF) >= r->chills) {
            continue;
        }

        s->cells[nat] = CELL_MAKE(CELL_MATERIAL(n), (uint8_t)(temp - 1));""",
"""        if ((int)(rng_next(&s->rng) & 0xFF) >= r->chills) {
            continue;
        }
        slot_note_r(r, F_chills);

        s->cells[nat] = CELL_MAKE(CELL_MATERIAL(n), (uint8_t)(temp - 1));""",
"chills")

add(
"""        if ((int)(rng_next(&s->rng) & 0xFF) >= (r->conducts >> SPREAD_SHIFT)) {
            continue;
        }
        s->cells[nat] = CELL_MAKE(CELL_MATERIAL(n), (uint8_t)(gap > 0 ? nt + 1 : nt - 1));""",
"""        if ((int)(rng_next(&s->rng) & 0xFF) >= (r->conducts >> SPREAD_SHIFT)) {
            continue;
        }
        slot_note_r(r, F_conducts);
        s->cells[nat] = CELL_MAKE(CELL_MATERIAL(n), (uint8_t)(gap > 0 ? nt + 1 : nt - 1));""",
"conducts_tempered")

add(
"""    if (drain == 0 || (unsigned)(rng_next(&s->rng) & 0xFF) >= drain) {
        return temp != SAND_AMBIENT_HEAT;
    }

    const uint8_t next = (uint8_t)(temp > SAND_AMBIENT_HEAT ? temp - 1 : temp + 1);""",
"""    if (drain == 0 || (unsigned)(rng_next(&s->rng) & 0xFF) >= drain) {
        return temp != SAND_AMBIENT_HEAT;
    }
    slot_note_r(r, F_cools);

    const uint8_t next = (uint8_t)(temp > SAND_AMBIENT_HEAT ? temp - 1 : temp + 1);""",
"cools")

add(
"""    if (f < 255 && (int)(rng_next(&s->rng) & 0xFF) >= f) {
        return false;
    }
    if (s->impulse_buf != NULL && material_of(n)->kind == KIND_GAS &&""",
"""    if (f < 255 && (int)(rng_next(&s->rng) & 0xFF) >= f) {
        return false;
    }
    slot_note_r(r, F_flammability);
    if (r->needs_air) {
        slot_note_r(r, F_needs_air);
    }
    if (s->impulse_buf != NULL && material_of(n)->kind == KIND_GAS &&""",
"flammability_ignite")

add(
"""    if (try_flare(s, x, y, w, h, mat, reaction_of(grain)->flare)) {
        acted = true;
    }

    return acted;""",
"""    if (try_flare(s, x, y, w, h, mat, reaction_of(grain)->flare)) {
        acted = true;
        slot_note_r(rx, F_flare);
    }

    return acted;""",
"flare")

add(
"""            const int boils = (s->boils >= 0) ? s->boils : reaction_of(bc)->boils;
            if (boils != 0 && (int)(rng_next(&s->rng) & 0xFF) < boils) {
                /* place_reacted() avoids shortened steam life. */""",
"""            const int boils = (s->boils >= 0) ? s->boils : reaction_of(bc)->boils;
            if (boils != 0 && (int)(rng_next(&s->rng) & 0xFF) < boils) {
                slot_note_r(reaction_of(bc), F_boils);
                /* place_reacted() avoids shortened steam life. */""",
"boils")

add(
"""            const int c = (s->conduction >= 0) ? s->conduction : reaction_of(here)->conducts;
            if ((int)(rng_next(&s->rng) & 0xFF) >= c) {
                break; /* heat stops inside this cell of the run */
            }""",
"""            const int c = (s->conduction >= 0) ? s->conduction : reaction_of(here)->conducts;
            if ((int)(rng_next(&s->rng) & 0xFF) >= c) {
                break; /* heat stops inside this cell of the run */
            }
            slot_note_r(reaction_of(here), F_conducts);""",
"conducts_chain")

add(
"""            if (br->flammability != 0 && (!br->needs_air || touches_air(s, rx, ry, w, h))) {
                const uint8_t becomes = br->ignites_to ? br->ignites_to : MAT_FIRE;""",
"""            if (br->flammability != 0 && (!br->needs_air || touches_air(s, rx, ry, w, h))) {
                slot_note_r(br, F_flammability);
                if (br->needs_air) {
                    slot_note_r(br, F_needs_air);
                }
                const uint8_t becomes = br->ignites_to ? br->ignites_to : MAT_FIRE;""",
"flammability_conduct")

add(
"""    if (evaporates != 0 && (int)(rng_next(&s->rng) & 0xFF) < evaporates
        && (!per_material || (rng_next(&s->rng) % 60) == 0)) {
        const size_t at = (size_t)y * (size_t)w + (size_t)x;
        place_reacted(s, x, y, at, MAT_GAS);
        return true;
    }

    if ((int)(rng_next(&s->rng) & 0xFF) >= r->dissolves) {
        return false;
    }

    for (int d = 0; d < 4; d++) {""",
"""    if (evaporates != 0 && (int)(rng_next(&s->rng) & 0xFF) < evaporates
        && (!per_material || (rng_next(&s->rng) % 60) == 0)) {
        slot_note_r(r, F_evaporates);
        const size_t at = (size_t)y * (size_t)w + (size_t)x;
        place_reacted(s, x, y, at, MAT_GAS);
        return true;
    }

    if ((int)(rng_next(&s->rng) & 0xFF) >= r->dissolves) {
        return false;
    }
    slot_note_r(r, F_dissolves);

    for (int d = 0; d < 4; d++) {""",
"evaporates_dissolves")

add(
"""        const uint8_t give = reaction_of(n)->dissolvable;
        if (give == 0 || (int)(rng_next(&s->rng) & 0xFF) >= give) {
            continue;
        }""",
"""        const uint8_t give = reaction_of(n)->dissolvable;
        if (give == 0 || (int)(rng_next(&s->rng) & 0xFF) >= give) {
            continue;
        }
        slot_note_r(reaction_of(n), F_dissolvable);""",
"dissolvable")

add(
"""        if (r->fizz != 0 && (int)(rng_next(&s->rng) & 0xFF) < r->fizz) {
            const uint8_t residue = (rng_next(&s->rng) & 1) ? MAT_GAS : MAT_SMOKE;
            place_reacted(s, nx, ny, at, residue);""",
"""        if (r->fizz != 0 && (int)(rng_next(&s->rng) & 0xFF) < r->fizz) {
            slot_note_r(r, F_fizz);
            const uint8_t residue = (rng_next(&s->rng) & 1) ? MAT_GAS : MAT_SMOKE;
            place_reacted(s, nx, ny, at, residue);""",
"fizz")

add(
"""    const reaction_t* rx = reaction_of(grain);
    const bool lit_state = rx->burn_decay != 0;
    const int burn_rate = (s->decay >= 0) ? s->decay : rx->burn_decay;""",
"""    const reaction_t* rx = reaction_of(grain);
    const bool lit_state = rx->burn_decay != 0;
    const int burn_rate = (s->decay >= 0) ? s->decay : rx->burn_decay;
    if (lit_state) {
        slot_note_r(rx, F_burn_decay);
    } else if (rx->burns != 0) {
        slot_note_r(rx, F_burns);
    }""",
"burns_dispatch")

add(
"""        const uint8_t residue = rx->residue;
        if (residue != 0 && (int)(rng_next(&s->rng) & 0xFF) < residue) {
            place_reacted(s, x, y, at, MAT_SMOKE);
        }
        return true;""",
"""        const uint8_t residue = rx->residue;
        if (residue != 0 && (int)(rng_next(&s->rng) & 0xFF) < residue) {
            slot_note_r(rx, F_residue);
            place_reacted(s, x, y, at, MAT_SMOKE);
        }
        return true;""",
"residue")

add(
"""            if (s->impulse_buf != NULL && s->fuse_blast_wait == 0
                && find_lit_two_by_two(s, x, y, w, h, grain, rx, &dx, &dy)) {
                s->fuse_blast_wait =""",
"""            if (s->impulse_buf != NULL && s->fuse_blast_wait == 0
                && find_lit_two_by_two(s, x, y, w, h, grain, rx, &dx, &dy)) {
                slot_note_r(rx, F_explodes);
                s->fuse_blast_wait =""",
"explodes")

add(
"""                if (nr->melts != 0 && nr->heats_to != 0 && (int)(rng_next(&s->rng) & 0xFF) < nr->melts) {
                    place_reacted(s, nx, ny, nat, nr->heats_to);
                    changed = true;
                }""",
"""                if (nr->melts != 0 && nr->heats_to != 0 && (int)(rng_next(&s->rng) & 0xFF) < nr->melts) {
                    slot_note_r(nr, F_melts);
                    place_reacted(s, nx, ny, nat, nr->heats_to);
                    changed = true;
                }""",
"melts")

add(
"""    const int condenses = (s->condenses >= 0) ? s->condenses : r->condenses;
    if (condenses == 0 || (int)(rng_next(&s->rng) & 0xFF) >= condenses) {
        return false;
    }

    place_reacted(s, x, y, at, r->condenses_to);""",
"""    const int condenses = (s->condenses >= 0) ? s->condenses : r->condenses;
    if (condenses == 0 || (int)(rng_next(&s->rng) & 0xFF) >= condenses) {
        return false;
    }
    slot_note_r(r, F_condenses);

    place_reacted(s, x, y, at, r->condenses_to);""",
"condenses")

add(
"""    if (r->soaked_to != 0 && held >= r->moist_max && (int)(rng_next(&s->rng) & 0xFF) < r->soaked_chance) {
        const size_t at = (size_t)y * (size_t)w + (size_t)x;
        place_reacted(s, x, y, at, r->soaked_to);
        return true;
    }""",
"""    if (r->soaked_to != 0 && held >= r->moist_max && (int)(rng_next(&s->rng) & 0xFF) < r->soaked_chance) {
        slot_note_r(r, F_soaked_chance);
        const size_t at = (size_t)y * (size_t)w + (size_t)x;
        place_reacted(s, x, y, at, r->soaked_to);
        return true;
    }""",
"soaked_chance")

add(
"""            beside_liquid = true;

            if ((int)(rng_next(&s->rng) & 0xFF) >= soaks) {
                continue;
            }
            /* The liquid pays for what was taken out of it. */
            pay_quench_cost(s, nx, ny, w);""",
"""            beside_liquid = true;
            slot_note_r(reaction_of(n), F_wets);

            if ((int)(rng_next(&s->rng) & 0xFF) >= soaks) {
                continue;
            }
            slot_note_r(r, F_soaks);
            /* The liquid pays for what was taken out of it. */
            pay_quench_cost(s, nx, ny, w);""",
"wets_soaks")

add(
"""    if (r->dries != 0 && held != 0 && (int)(rng_next(&s->rng) & 0xFF) < r->dries) {
        row[x] = soil_set_moisture(c, (uint8_t)(held - 1), 0);
        mark_rows(s, y, y);
        wake_block_and_neighbors(s, x, y);
        return held - 1 != 0;
    }""",
"""    if (r->dries != 0 && held != 0 && (int)(rng_next(&s->rng) & 0xFF) < r->dries) {
        slot_note_r(r, F_dries);
        row[x] = soil_set_moisture(c, (uint8_t)(held - 1), 0);
        mark_rows(s, y, y);
        wake_block_and_neighbors(s, x, y);
        return held - 1 != 0;
    }""",
"dries")

add(
"""    if ((int)(rng_next(&s->rng) & 0xFF) >= r->falls) {
        return true; /* still falling, just not now */
    }

    s->cells[nat] = s->cells[at];""",
"""    if ((int)(rng_next(&s->rng) & 0xFF) >= r->falls) {
        return true; /* still falling, just not now */
    }
    slot_note_r(r, F_falls);

    s->cells[nat] = s->cells[at];""",
"falls")

add(
"""    if ((int)(rng_next(&s->rng) & 0xFF) >= r->withers) {
        return false;
    }

    /* Withering prevents shoot from becoming permanent woody speck. CELL_MAKE""",
"""    if ((int)(rng_next(&s->rng) & 0xFF) >= r->withers) {
        return false;
    }
    slot_note_r(r, F_withers);

    /* Withering prevents shoot from becoming permanent woody speck. CELL_MAKE""",
"withers")

add(
"""            if (reaction_of(c)->soil != 0) {
                nx = tx;
                ny = ty;
                on_soil = true;
                break; /* ground: stop looking for stem */
            }""",
"""            if (reaction_of(c)->soil != 0) {
                slot_note_r(reaction_of(c), F_soil);
                nx = tx;
                ny = ty;
                on_soil = true;
                break; /* ground: stop looking for stem */
            }""",
"soil")

add(
"""    if ((int)(rng_next(&s->rng) & 0xFF) >= r->grows) {
        return true;
    }

    int run = 1, tx = x, ty = y;""",
"""    if ((int)(rng_next(&s->rng) & 0xFF) >= r->grows) {
        return true;
    }
    slot_note_r(r, F_grows);

    int run = 1, tx = x, ty = y;""",
"grows")

add(
"""    if (r->holds_line != 0 && (int)(rng_next(&s->rng) & 0xFF) < r->holds_line) {
        /* Longer baseline increases horizontal drift from 24 to 38. Shorter""",
"""    if (r->holds_line != 0 && (int)(rng_next(&s->rng) & 0xFF) < r->holds_line) {
        slot_note_r(r, F_holds_line);
        /* Longer baseline increases horizontal drift from 24 to 38. Shorter""",
"holds_line")

add(
"""    if (trunk < r->harden_run) {
        return true;
    }
""",
"""    if (trunk < r->harden_run) {
        return true;
    }
    slot_note_r(r, F_harden_run);
""",
"harden_run")

add(
"""    if (__builtin_expect((int)(rng_next(&s->rng) & 0xFF) >= r->harden_chance, 1)) {
        return true;
    }

    /* SHAPING PASS: Hardens, converts to wood, thickens trunk, adds canopy.""",
"""    if (__builtin_expect((int)(rng_next(&s->rng) & 0xFF) >= r->harden_chance, 1)) {
        return true;
    }
    slot_note_r(r, F_harden_chance);

    /* SHAPING PASS: Hardens, converts to wood, thickens trunk, adds canopy.""",
"harden_chance")

add(
"""        const int extra = (int)r->trunk_girth * (span - i) / span;
        for (int g = 1; g <= extra; g++) {""",
"""        const int extra = (int)r->trunk_girth * (span - i) / span;
        if (extra > 0) {
            slot_note_r(r, F_trunk_girth);
        }
        for (int g = 1; g <= extra; g++) {""",
"trunk_girth")

add(
"""                if ((int)(rng_next(&s->rng) & 0xFF) >= r->canopy) {
                    continue;
                }
                place_reacted(s, lx, ly, lat, r->canopy_to);""",
"""                if ((int)(rng_next(&s->rng) & 0xFF) >= r->canopy) {
                    continue;
                }
                slot_note_r(r, F_canopy);
                place_reacted(s, lx, ly, lat, r->canopy_to);""",
"canopy")

add(
"""    if ((int)(rng_next(&s->rng) & 0xFF) >= r->roots) {
        return true; /* a candidate exists; just not this roll */
    }
    int pick = rng_below(&s->rng, total_w);""",
"""    if ((int)(rng_next(&s->rng) & 0xFF) >= r->roots) {
        return true; /* a candidate exists; just not this roll */
    }
    slot_note_r(r, F_roots);
    int pick = rng_below(&s->rng, total_w);""",
"roots_root_row")

add(
"""    if ((int)(rng_next(&s->rng) & 0xFF) >= r->roots) {
        return;
    }
    place_reacted(s, contact_at % w, contact_at / w, (size_t)contact_at, r->roots_to);""",
"""    if ((int)(rng_next(&s->rng) & 0xFF) >= r->roots) {
        return;
    }
    slot_note_r(r, F_roots);
    place_reacted(s, contact_at % w, contact_at / w, (size_t)contact_at, r->roots_to);""",
"roots_grower_row")

add(
"""    if ((int)(rng_next(&s->rng) & 0xFF) >= r->drinks) {
        return true;
    }

    pay_quench_cost(s, lx, ly, w);""",
"""    if ((int)(rng_next(&s->rng) & 0xFF) >= r->drinks) {
        return true;
    }
    slot_note_r(r, F_drinks);

    pay_quench_cost(s, lx, ly, w);""",
"drinks")

add(
"""    if ((int)(rng_next(&s->rng) & 0xFF) >= r->sprouts) {
        return true;
    }

    place_reacted(s, ex, ey, (size_t)empty_at, r->sprouts_to);""",
"""    if ((int)(rng_next(&s->rng) & 0xFF) >= r->sprouts) {
        return true;
    }
    slot_note_r(r, F_sprouts);

    place_reacted(s, ex, ey, (size_t)empty_at, r->sprouts_to);""",
"sprouts")

add(
"""    if ((int)(rng_next(&s->rng) & 0xFF) >= r->buds) {
        return true;
    }

    place_reacted(s, bx, by, (size_t)at, r->buds_to);""",
"""    if ((int)(rng_next(&s->rng) & 0xFF) >= r->buds) {
        return true;
    }
    slot_note_r(r, F_buds);

    place_reacted(s, bx, by, (size_t)at, r->buds_to);""",
"buds")

n_ok = 0
unplaced = []
touched = set()
for old, new, label in edits:
    if label == "include":
        continue  # handled below, per instrumented file
    homes = [p for p in paths if texts[p].count(old) == 1]
    if len(homes) != 1:
        total = sum(texts[p].count(old) for p in paths)
        where = ", ".join(os.path.basename(p) for p in homes) or "nowhere"
        print(f"FAIL [{label}]: {total} occurrence(s) across {len(paths)} "
              f"file(s), matched in {where} (need exactly one file)")
        unplaced.append(label)
        continue
    texts[homes[0]] = texts[homes[0]].replace(old, new, 1)
    touched.add(homes[0])
    n_ok += 1

# Every file that received a slot call needs the header, and which files
# those are is now discovered rather than assumed - the split's real
# lesson.
INC = '#include "reaction_slots.h"'
for p in sorted(touched):
    if INC in texts[p]:
        continue
    last = texts[p].rfind("#include")
    if last < 0:
        print(f"FAIL [include]: {os.path.basename(p)} has no #include to anchor to")
        unplaced.append(f"include:{os.path.basename(p)}")
        continue
    eol = texts[p].find("\n", last)
    texts[p] = texts[p][:eol + 1] + INC + "\n" + texts[p][eol + 1:]

print(f"{n_ok}/{len(edits) - 1} slot edits applied across "
      f"{len(touched)} of {len(paths)} file(s): "
      + ", ".join(sorted(os.path.basename(p) for p in touched)))

for p in sorted(touched):
    with open(p, "w", encoding="utf-8", newline="") as f:
        f.write(texts[p])

if unplaced:
    print("UNPLACED (these reactions would report as never-firing): "
          + ", ".join(unplaced))
    sys.exit(1)
