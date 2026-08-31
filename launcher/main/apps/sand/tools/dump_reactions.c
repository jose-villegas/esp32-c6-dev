/*=============================================================================
 * dump_reactions - compile material.c's reaction tables into markdown.
 *
 * See docs/Sand/Reaction-Doc-Generator-Plan.md for the design this follows;
 * this file is that plan's phase 1. Run through report_reactions.sh, which
 * builds this, captures its stdout, and writes docs/Sand/Reaction-Table.md.
 *
 * WHY A PROGRAM AND NOT A SCRIPT OVER THE TEXT
 *
 * material.c's tables use constant expressions - MATX(MATX_LEAF), MATX()'s
 * bit-shift, SAND_SHOCK_HEAT (itself SAND_AMBIENT_HEAT + 2) - and regexing
 * those back into values means reimplementing the preprocessor. Linking
 * material.c and reading the resulting reactions[]/extended_reactions[]
 * arrays at runtime resolves every one of them for free, the same way the
 * simulation itself does.
 *
 * WHAT PHASE 1 DELIBERATELY DOES NOT DO
 *
 * No hand-written prose. Every adverb below is the RATE LADDER's computed
 * bucket - even the ones the plan already flags as reading wrong by feel
 * (sand -> glass, the ignition family) - and every field whose real trigger
 * is a condition living at a read site in sand_reactions.c (not in the
 * table) renders as a literal "[TODO: trigger]" rather than a guessed
 * clause. That is intentional: this is raw output to look at and tune
 * against, not the tuned copy. See the plan's own "Phasing" section.
 *
 * THE ONE PLACE THIS DEVIATES FROM THE PLAN'S OWN WORDING, ON PURPOSE
 *
 * The plan's "Kind cannot be inferred" section names canopy_to, sprouts_to
 * and shatters_to as cell specs and every other `_to` as a material id.
 * That is not what the data says: MAT_GLASS.shatters_to holds MAT_SAND, an
 * ordinary material id, and MAT_DIRT.heats_to holds MATX(MATX_METAL), an
 * extended cell spec - so the true split is not by FIELD NAME, it is by
 * VALUE, exactly the way place_reacted() (sand_reactions.c) itself decides:
 * a byte >= (MAT_EXTENDED << 4) is a whole cell spec, anything below that is
 * a plain material id. to_name() below implements that check instead of a
 * fixed per-field list, because a fixed list would get shatters_to's own
 * current value wrong - it would print "empty" for glass shattering into
 * sand, which is exactly the "confidently wrong name, silently" failure the
 * plan's own paragraph warns about, one paragraph before naming the field
 * that trips it. */

#include <ctype.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

#include "material.h"

/* Not defined anywhere in this codebase today (checked) - every other array
 * here is either sized by a named constant or walked with sizeof/pointer
 * arithmetic. field_docs[] is neither: its length IS the claim being
 * checked below, so it needs its own name rather than a magic number. */
#define ARRAY_LEN(a) (sizeof(a) / sizeof((a)[0]))

/*-----------------------------------------------------------------------
 * The field table.
 *
 * One row per reaction_t field. `group` says which clause it contributes
 * to (see the emit_* functions below, one per group); `kind` says how a
 * TARGET field's value decodes. `verb`/`adverb_override` exist because the
 * plan asks for them explicitly - an irregular verb form, or a rate whose
 * ladder bucket reads wrong once partner count or persistence is accounted
 * for (sand -> glass, the ignition family - see the plan's "The ladder
 * lies in two directions" section). Phase 1 sets neither: every adverb
 * below takes the computed bucket, unedited, on purpose - that by-feel
 * pass is explicitly phase 2's job, not this file's.
 *---------------------------------------------------------------------*/

typedef enum {
    GRP_IGNITE = 0,   /* catching fire: flammability, ignites_to, needs_air */
    GRP_BURN,         /* being alight: burns, burn_decay, residue, quench_to, flare */
    GRP_TRANSFORM,    /* heat alone, no flame involved: heats_to, heat_chance */
    GRP_TEMPERATURE,  /* banking/passing heat: heat_ramp, cools, conducts */
    GRP_COLD,         /* drawing heat out of a neighbour: chills */
    GRP_WARMTH,       /* convection into a neighbour: warms */
    GRP_THAW,         /* melting in any liquid: thaws */
    GRP_WET,          /* the wetting family: wets, soaks, soaks_to, dries */
    GRP_ACID,         /* dissolving / being dissolved: dissolves, dissolvable, fizz */
    GRP_GROW,         /* extending into wet soil, or dying without it */
    GRP_HARDEN,       /* becoming wood, and what that leaves behind */
    GRP_REGROW,       /* new growth and foliage from a finished trunk, and drinking */
    GRP_SHATTER,      /* thermal shock: shatters_to */
    GRP_COUNT
} group_id_t;

typedef enum {
    FK_RATE,      /* a chance/256 per step (or per adjacent partner per
                   * step) - the rate ladder applies directly */
    FK_TARGET,    /* a uint8_t naming a material id or a whole extended
                   * cell spec - see to_name() */
    FK_FLAG,      /* boolean-ish: zero/nonzero, no rate to speak of */
    FK_COUNT_MAG, /* a plain magnitude (a cell count, a width) - NOT a
                   * chance/256, so the rate ladder must not touch it */
} field_kind_t;

/* Only meaningful when kind == FK_RATE - splits FK_RATE into the two
 * different questions a chance/256 can answer, because "how fast" is only
 * a real question for one of them.
 *
 * A field rolled every step against an adjacent partner (or every step,
 * full stop) has a genuine expected TIME to wait, so the rate ladder
 * (adverb_for(), a speed word) applies. A field that is instead a ONE-SHOT
 * roll at a single moment - a burn winking out, an acid bite landing, a
 * hardening run deciding whether it takes this time - has no time axis at
 * all: it either happens this once or it does not, and asking "how fast"
 * of it is exactly the malformed question this enum exists to stop
 * emit_* from asking. Those get the chance bucket (chance_bucket_for()) and
 * a vocabulary word (frequency_words[] or ease_words[], by field) instead.
 * See adverb() below, and the two ladders' own comments for the
 * boundaries. */
typedef enum {
    SCALE_NA = 0, /* kind != FK_RATE - no ladder of either kind applies */
    SCALE_RATE,   /* rolled every step against a partner - speed ladder */
    SCALE_CHANCE, /* a one-shot roll at a single moment - frequency ladder */
} field_scale_t;

typedef struct {
    size_t offset;
    const char *name;
    group_id_t group;
    field_kind_t kind;
    field_scale_t scale;          /* meaningful only when kind == FK_RATE -
                                    * see field_scale_t's own comment */
    const char *verb;             /* NULL where the group's own template
                                    * carries the wording instead */
    const char *adverb_override;  /* NULL = use the computed rate-ladder
                                    * bucket; unset by every field in
                                    * phase 1, see this file's top comment */
    const char *const *chance_vocab; /* meaningful only when scale ==
                                       * SCALE_CHANCE - which VOCABULARY the
                                       * bucket index (chance_bucket_for())
                                       * renders through. NULL = frequency_
                                       * words[], the default "how often"
                                       * reading every FCHANCE() row gets.
                                       * `dissolvable` is the one field that
                                       * asks for ease_words[] instead - see
                                       * those arrays' own comment for why. */
} field_doc_t;

#define F(field, grp, knd, vb) \
    { offsetof(reaction_t, field), #field, (grp), (knd), SCALE_NA, (vb), \
      NULL, NULL }
/* A genuine per-step rate - see field_scale_t's own comment. */
#define FRATE(field, grp, vb) \
    { offsetof(reaction_t, field), #field, (grp), FK_RATE, SCALE_RATE, \
      (vb), NULL, NULL }
/* A one-shot chance at a single moment, not a rate - see field_scale_t's
 * own comment for why this needs a different ladder from FRATE. Renders
 * through frequency_words[] (the default "how often" vocabulary) unless a
 * row overrides chance_vocab directly - see FCHANCE_VOCAB below. */
#define FCHANCE(field, grp, vb) \
    { offsetof(reaction_t, field), #field, (grp), FK_RATE, SCALE_CHANCE, \
      (vb), NULL, NULL }
/* Same as FCHANCE, but names an explicit chance_vocab instead of taking
 * the frequency_words[] default - see field_doc_t.chance_vocab. */
#define FCHANCE_VOCAB(field, grp, vb, voc) \
    { offsetof(reaction_t, field), #field, (grp), FK_RATE, SCALE_CHANCE, \
      (vb), NULL, (voc) }

/* The two SCALE_CHANCE vocabularies. Both index off the same five buckets
 * (chance_bucket_for(), in the Decoding section below) - only the WORDS
 * differ, because a one-shot chance/256 can answer two different
 * questions depending on what it is conditioned on.
 *
 * frequency_words[] is "how often does this happen" - the right question
 * for residue, fizz, harden_chance, canopy and holds_line, each a fresh
 * roll at its own moment ("often leaves smoke when it burns out").
 *
 * ease_words[] is "how well does this go, given it is already happening" -
 * the right question for `dissolvable` alone. Its own comment in
 * material.h is explicit that it is "the chance an ATTEMPT to dissolve
 * succeeds", i.e. conditional on an attempt already under way, not a
 * frequency in its own right - "Usually gives way to acid" answers a
 * question nobody asked, and fronts the adverb besides. Same buckets,
 * different vocabulary - see field_doc_t.chance_vocab. */
static const char *const frequency_words[] = {
    "almost always", "usually", "often", "sometimes", "rarely",
};
/* Same five buckets as frequency_words[] above, but an EASE/RESISTANCE
 * scale rather than a time-to-wait one - `dissolvable` has no clock in it
 * (see this array's own field_docs row and dissolvable's comment in
 * material.h: it is the chance a single ATTEMPT succeeds, not a rate to
 * convert into a duration), so none of these five words may imply "how
 * long". A prior version of this table used "almost instantly" / "slowly"
 * here, which are duration words smuggled into a chance-of-success
 * question - the same category error the rate/frequency split above
 * exists to prevent, just missed on this one field. */
static const char *const ease_words[] = {
    "very easily", "easily", "readily", "reluctantly", "barely",
};

static const field_doc_t field_docs[] = {
    /* GRP_IGNITE */
    FRATE(flammability, GRP_IGNITE, "catches"),
    F(ignites_to,   GRP_IGNITE, FK_TARGET, NULL),
    F(needs_air,    GRP_IGNITE, FK_FLAG,   NULL),

    /* GRP_BURN. `residue` is a one-shot chance at the moment a burn-down
     * finishes, not a per-step rate against a partner - see field_scale_t
     * and emit_burn(). */
    F(burns,        GRP_BURN, FK_FLAG,   NULL),
    FRATE(burn_decay, GRP_BURN, "burns down"),
    FCHANCE(residue,  GRP_BURN, "leaves smoke"),
    F(quench_to,    GRP_BURN, FK_TARGET, NULL),
    FRATE(flare,     GRP_BURN, "licks flame into the air"),
    FRATE(vent_chance, GRP_BURN, "vents through whatever covers it from above"),

    /* GRP_ACID (dissolves/dissolvable/fizz all belong to the acid pair,
     * whichever side of it this row is on - see emit_acid()). `dissolves`
     * is rolled every step per adjacent target, a genuine rate; `fizz` and
     * `dissolvable` are each a one-shot chance at the moment a single
     * dissolve happens - `dissolvable`'s own comment in material.h is
     * explicit that it is "the chance an ATTEMPT to dissolve succeeds",
     * i.e. conditional on an attempt, not a rate in its own right. */
    FRATE(dissolves,    GRP_ACID, "dissolves an adjacent cell"),
    /* `dissolvable` asks "how WELL does acid do here", not "how often" -
     * see its own comment just above emit_acid() - so it renders through
     * ease_words[] rather than the frequency_words[] every other FCHANCE()
     * row gets. */
    FCHANCE_VOCAB(dissolvable, GRP_ACID, "dissolves in acid", ease_words),
    FCHANCE(fizz,        GRP_ACID, "leaves smoke"),
    /* Pre-existing gap, unrelated to whatever else changed in this table
     * recently: `evaporates` never had a field_docs row at all, so this
     * file has not compiled since the field was added - caught trying to
     * regenerate the doc for an unrelated change, fixed here rather than
     * left for the next person. Genuine per-step rate, no partner or
     * condition required - the same shape as `dries` elsewhere in this
     * file (FRATE(dries, GRP_WET, ...)), not a one-shot FCHANCE. */
    FRATE(evaporates,   GRP_ACID, "spontaneously evaporates into gas"),

    /* GRP_TRANSFORM. `flaw_chance` is a one-shot chance conditioned on
     * heat_chance's roll already having succeeded, the same shape as
     * `dissolvable` - not a rate of its own, so FCHANCE rather than
     * FRATE. `spoils_to`/`spoils_chance` fire from a condition
     * (heat_chance succeeding on a WET cell) that lives entirely at the
     * read site in sand_reactions.c, not in this table - see this file's
     * own top comment on shatters_to for why that gets the [TODO: trigger]
     * treatment (emit_spoils()) rather than guessed prose. */
    F(heats_to,     GRP_TRANSFORM, FK_TARGET, NULL),
    FRATE(heat_chance, GRP_TRANSFORM, "melts"),
    F(flaw_to,       GRP_TRANSFORM, FK_TARGET, NULL),
    FCHANCE(flaw_chance, GRP_TRANSFORM, "comes out flawed"),
    F(spoils_to,     GRP_TRANSFORM, FK_TARGET, NULL),
    FCHANCE(spoils_chance, GRP_TRANSFORM, "spoils"),

    /* GRP_TEMPERATURE */
    FRATE(heat_ramp, GRP_TEMPERATURE, "holds heat"),
    FRATE(cools,      GRP_TEMPERATURE, "drains back to ambient"),

    /* GRP_COLD / GRP_WARMTH / GRP_THAW - each one field */
    FRATE(chills,   GRP_COLD,   "chills whatever it touches"),
    FRATE(conducts, GRP_TEMPERATURE, "passes heat on"),
    FRATE(warms,    GRP_WARMTH, "warms whatever it touches"),
    FRATE(thaws,    GRP_THAW,   "melts in any liquid it touches"),

    /* GRP_WET - the wetting family, see the plan's own section on it */
    F(wets,         GRP_WET, FK_FLAG, NULL),
    FRATE(soaks,    GRP_WET, "soaks up a wetting liquid it touches"),
    F(soaks_to,     GRP_WET, FK_TARGET, NULL),
    FRATE(dries,    GRP_WET, "dries back out"),

    /* GRP_GROW */
    FRATE(grows,    GRP_GROW, "grows into wet soil"),
    FRATE(falls,    GRP_GROW, "falls under gravity"),
    FRATE(withers,  GRP_GROW, "withers away"),

    /* GRP_HARDEN - becoming wood, and what that moment leaves behind.
     * `harden_chance`, `canopy` and `holds_line` are each a one-shot
     * decision made once, at the moment a run hardens - not a per-step
     * rate against a partner - see field_scale_t and emit_harden(). */
    F(hardens_to,     GRP_HARDEN, FK_TARGET,   NULL),
    F(harden_run,     GRP_HARDEN, FK_COUNT_MAG, NULL),
    FCHANCE(harden_chance, GRP_HARDEN, "hardens"),
    F(clings_to,      GRP_HARDEN, FK_TARGET,    NULL),
    F(sheltered_by,   GRP_GROW,   FK_TARGET,    NULL), /* modifies withers,
                                                        * not hardening -
                                                        * see emit_grow() */
    FCHANCE(canopy,   GRP_HARDEN, "leafs its crown"),
    F(canopy_to,      GRP_HARDEN, FK_TARGET,    NULL),
    F(trunk_girth,    GRP_HARDEN, FK_COUNT_MAG, NULL),
    FCHANCE(holds_line, GRP_HARDEN, "holds its own line"),

    /* GRP_REGROW - new growth from a finished trunk, and drinking */
    FRATE(sprouts,  GRP_REGROW, "sprouts foliage"),
    F(sprouts_to,   GRP_REGROW, FK_TARGET, NULL),
    FRATE(buds,     GRP_REGROW, "buds new growth"),
    F(buds_to,      GRP_REGROW, FK_TARGET, NULL),
    FRATE(drinks,   GRP_REGROW, "drinks through its roots"),

    /* GRP_SHATTER */
    F(shatters_to,  GRP_SHATTER, FK_TARGET, NULL),
};

#undef F
#undef FRATE
#undef FCHANCE
#undef FCHANCE_VOCAB

/* Every field in reaction_t is a uint8_t and the struct has no other
 * members, so there is no padding: the byte SIZE of the struct is also
 * its FIELD COUNT. That is what lets this be a single number rather than
 * a walk - see field_docs_offsets_are_sound() below for the check this
 * count alone cannot do (catching a duplicated or skipped offsetof).
 *
 * This is deliberately the gate the plan asks for: add a field to
 * reaction_t and this stops compiling until field_docs[] carries a row
 * for it - a group, a kind, and (if the field is ambiguous or its trigger
 * lives at a read site rather than in the table) the things this file
 * cannot derive on its own. */
_Static_assert(ARRAY_LEN(field_docs) == sizeof(reaction_t),
               "every reaction_t field needs exactly one row in "
               "field_docs[] - add one (group, kind, and a verb if the "
               "group template needs it) for whatever field just changed "
               "the struct's size");

/* The count above passes even if two rows name the same offset and a third
 * field is never mentioned at all - sizeof(reaction_t) would still equal
 * ARRAY_LEN(field_docs), just wrong in a way the assert cannot see. This
 * walks the offsets themselves and insists they are exactly 0..N-1 with no
 * repeats, which is the only way both "every field is covered" and "no
 * field is covered twice" can be verified from here. */
static void field_docs_offsets_are_sound(void)
{
    bool seen[sizeof(reaction_t)] = { false };
    for (size_t i = 0; i < ARRAY_LEN(field_docs); i++) {
        const size_t off = field_docs[i].offset;
        if (off >= sizeof(reaction_t)) {
            fprintf(stderr,
                    "dump_reactions: field_docs[%zu] (%s) has offset %zu, "
                    "outside reaction_t (size %zu)\n",
                    i, field_docs[i].name, off, sizeof(reaction_t));
            exit(1);
        }
        if (seen[off]) {
            fprintf(stderr,
                    "dump_reactions: field_docs[%zu] (%s) repeats offset "
                    "%zu - some other row already claimed it, and "
                    "whichever field owns it for real has no row at all\n",
                    i, field_docs[i].name, off);
            exit(1);
        }
        seen[off] = true;
    }
    for (size_t off = 0; off < sizeof(reaction_t); off++) {
        if (!seen[off]) {
            fprintf(stderr,
                    "dump_reactions: no field_docs[] row claims offset "
                    "%zu - reaction_t has a field the table never heard "
                    "of\n", off);
            exit(1);
        }
    }
}

static const field_doc_t *field_doc(const char *name)
{
    for (size_t i = 0; i < ARRAY_LEN(field_docs); i++) {
        if (strcmp(field_docs[i].name, name) == 0) {
            return &field_docs[i];
        }
    }
    fprintf(stderr, "dump_reactions: field_doc(\"%s\") - no such field\n",
            name);
    exit(1);
}

/*-----------------------------------------------------------------------
 * Decoding.
 *---------------------------------------------------------------------*/

/* The rate ladder - for FK_RATE fields with scale == SCALE_RATE: a chance
 * rolled every step against a steady partner, which has a genuine expected
 * TIME to wait (SIM_HZ 60 - see app_sand.c - so one step is ~16.7ms, and
 * the expected wait against one steady partner is 256/value steps).
 *
 * Six buckets, not five: the original five left a single "fast" band
 * covering v in [21, 84], and two pairs of materials the source
 * deliberately gives DIFFERENT numbers landed on the SAME word because of
 * it - steam's `warms` 48 against smoke's 28 (materials[MAT_STEAM]/
 * [MAT_SMOKE] call them "the hotter carrier" and "cooler than steam and
 * far longer lived"), and ice's `chills` 60 against snow's 40 (ice's own
 * comment: "Chills harder than snow, 60 against 40"). Splitting that band
 * at 45 keeps both pairs apart without disturbing anything below it:
 *
 *   v >= 85   (<=  50ms,  ~<=3 steps)   instantly
 *   v >= 45   (<=  94ms,  ~<=6 steps)   swiftly    <- new: was "fast" too
 *   v >= 21   (<= 203ms,  ~<=12 steps)  fast
 *   v >= 5    (<= 853ms,  ~<=51 steps)  readily
 *   v >= 2    (<=2133ms, ~<=128 steps)  steadily
 *   v == 1    (~4267ms,   256 steps)    slowly
 *   v == 0                              never (defensive; callers gate on
 *                                       v != 0 first)
 *
 * Wood's `flammability` 6 falls in the >= 5 "readily" band, same as
 * before this split - still read as easy/ready, not "barely", "rarely" or
 * "slowly". Oil's 50 now lands in the new "swiftly" band, a full tier
 * above wood's "readily", so oil still reads faster than wood. Gas's 255
 * stays in "instantly". */
static const char *adverb_for(uint8_t v)
{
    if (v >= 85) return "instantly";
    if (v >= 45) return "swiftly";
    if (v >= 21) return "fast";
    if (v >= 5)  return "readily";
    if (v >= 2)  return "steadily";
    if (v >= 1)  return "slowly";
    return "never";  /* defensive: callers gate on v != 0 first */
}

/* The chance bucket - for FK_RATE fields with scale == SCALE_CHANCE: a
 * ONE-SHOT roll at a single moment (a burn winking out, an acid bite
 * landing, a hardening run deciding whether it takes) rather than a rate
 * against a steady partner. There is no time axis to convert to a speed
 * word for these - see field_scale_t's own comment - so this reads the
 * same raw value as a plain percentage of 256 instead, five buckets wide:
 *
 *   v >= 231  (>= ~90%)    bucket 0
 *   v >= 154  (>= ~60%)    bucket 1
 *   v >=  77  (>= ~30%)    bucket 2
 *   v >=  26  (>= ~10%)    bucket 3
 *   v >=   1  (>  0%)      bucket 4
 *   v ==   0                          -1 (defensive; callers gate on
 *                                      v != 0 first)
 *
 * A bucket INDEX, not a word - unlike adverb_for() above, this scale only
 * ever answers one question ("which of five bands"), and two different
 * fields ask two different things of that answer. Which vocabulary turns
 * the index into a word is the caller's call (adverb() below), by way of
 * field_doc_t.chance_vocab - see frequency_words[]/ease_words[]'s own
 * comment for why. */
static int chance_bucket_for(uint8_t v)
{
    if (v >= 231) return 0;
    if (v >= 154) return 1;
    if (v >= 77)  return 2;
    if (v >= 26)  return 3;
    if (v >= 1)   return 4;
    return -1;  /* defensive: callers gate on v != 0 first */
}

static const char *adverb(const char *field_name, uint8_t v)
{
    const field_doc_t *fd = field_doc(field_name);
    if (fd->adverb_override != NULL) {
        return fd->adverb_override;
    }
    /* scale is only meaningful for FK_RATE fields, which is every field
     * that ever reaches adverb() - see field_scale_t's own comment. */
    if (fd->scale == SCALE_CHANCE) {
        const char *const *vocab = (fd->chance_vocab != NULL)
                                        ? fd->chance_vocab : frequency_words;
        const int b = chance_bucket_for(v);
        return (b >= 0) ? vocab[b] : "never";
    }
    return adverb_for(v);
}

/* Decode a TARGET field's raw byte the way place_reacted() (sand_reactions.c)
 * itself does at the moment it writes the cell: >= (MAT_EXTENDED << 4) is a
 * whole extended cell spec (MATX(k)'s high nibble IS MAT_EXTENDED), anything
 * below that is a plain material id. Value-based, not field-name-based - see
 * this file's top comment for why a fixed per-field list gets shatters_to
 * wrong today. */
static const char *to_name(uint8_t v)
{
    if (v >= (uint8_t)(MAT_EXTENDED << 4)) {
        return material_name((cell_t)v);
    }
    if (v < MATERIAL_MAX) {
        return materials[v].name;
    }
    /* Cannot happen for any value this table actually stores - every
     * target/spec field is either an ordinary id (< MATERIAL_MAX) or a
     * MATX() spec (>= MAT_EXTENDED << 4) by construction. Kept as a
     * named fallback rather than an assert so a future stray value prints
     * something legible instead of aborting a doc build. */
    return "?";
}

/* Lowercase a NUL-terminated string in place. The one helper both prose
 * name-lowercasing paths below share - see prose_name() (a single material
 * name, from to_name()) and main()'s own use on wetting_liquids (already a
 * mutable buffer it owns outright, so no copy is needed there).
 *
 * materials[].name is Title Case because it doubles as a brush label (the
 * palette/UI need a proper label to show), but every material this game
 * has is an ordinary common noun - "sand", "water", "lava", never a proper
 * noun - so unconditionally lowercasing it for prose is always correct
 * here; a name that named a person or place would need a smarter rule, but
 * nothing in materials[] ever does. */
static void str_lower(char *s)
{
    for (; *s != '\0'; s++) {
        *s = (char)tolower((unsigned char)*s);
    }
}

/* Lowercase a material name for use INSIDE a prose sentence - one of the
 * per-material bullets, never a table cell or a ### heading (those want
 * materials[].name's own Title Case untouched - see to_name()'s callers in
 * emit_pairwise_table(), none of which route through this). Returns a
 * pointer into a static buffer, like to_name() effectively does via
 * materials[]/extended_names[] (just mutable here instead of literal) -
 * safe because, as of this file, no single printf() call ever calls
 * prose_name() twice in one statement (grep to_name( to confirm before
 * adding one that would). */
static const char *prose_name(const char *name)
{
    static char buf[32];
    size_t i = 0;
    for (; name[i] != '\0' && i + 1 < sizeof(buf); i++) {
        buf[i] = name[i];
    }
    buf[i] = '\0';
    str_lower(buf);
    return buf;
}

#define CAUSE "[TODO: trigger]"

/*-----------------------------------------------------------------------
 * One row's worth of rows (materials[] name + reactions[]/extended_
 * reactions[] row + movement kind), built once and reused for both the
 * per-material section and the pairwise join table below.
 *---------------------------------------------------------------------*/

typedef struct {
    const char *name;
    const reaction_t *r;
    material_kind_t kind;
    uint8_t self_id;   /* the plain material id this row's own material is
                        * (MAT_EXTENDED for every extended material, since
                        * they share that one id and have no plain id of
                        * their own) - see emit_ignite()'s self check,
                        * which needs this to tell "ignites into itself"
                        * (wood) apart from "ignites into a third thing" */
    uint8_t color_id;  /* the raw byte a TARGET field would hold if it
                        * named this exact material - a plain id for an
                        * ordinary material, MATX(k) for an extended one.
                        * Unlike self_id above (MAT_EXTENDED for every
                        * extended material) this keeps k, because
                        * material_hex() needs the real swatch cell to
                        * look a colour up, not just which physics row the
                        * material shares - see material_hex()'s own
                        * comment. */
} mrow_t;

static mrow_t all_rows[(MAT_COUNT - 1) + MATERIAL_EXTENDED_COUNT];
static size_t all_rows_count;

/* The liquid(s) that actually wet things - KIND_LIQUID with wets != 0 -
 * derived from the data rather than hardcoded, so `soaks`/`drinks` keep
 * naming the right liquid(s) if a second wetting liquid is ever added.
 * Water is the only one today (see material.c, one `.wets = 1` hit), but
 * nothing below assumes that.
 *
 * Populated once in main(), right after build_rows() fills all_rows[] -
 * every emit_* function that reads it runs after that. See reaction_t.
 * wets's own comment in material.h for why "any liquid" is the wrong claim
 * for `soaks`/`drinks`: a bank of sand under oil or lava turned entirely
 * into saturated soil, which is the bug `wets` exists to prevent. `thaws`
 * (GRP_THAW) is the one field that is genuinely any liquid, and keeps
 * saying so untouched - see emit_thaw(). */
static char wetting_liquids[64];

/* The material(s) that actually radiate heat - `burns != 0` - derived the
 * same way as wetting_liquids just above, so emit_transform()/emit_ignite()
 * keep naming the right source(s) if a third heat source is ever added.
 * Fire and Lava are the only two today (see pred_burns, already used by
 * emit_pairwise_table() to join the same predicate into "Fire / Lava"), but
 * nothing below assumes there are exactly two.
 *
 * Populated once in main(), right after wetting_liquids - same reasoning:
 * every emit_* function that reads it runs after that point. */
static char heat_sources[64];

static void build_rows(void)
{
    all_rows_count = 0;
    for (uint8_t m = MAT_SAND; m < MAT_COUNT; m++) {
        all_rows[all_rows_count].name = materials[m].name;
        all_rows[all_rows_count].r    = &reactions[m];
        all_rows[all_rows_count].kind = (material_kind_t)materials[m].kind;
        all_rows[all_rows_count].self_id = m;
        all_rows[all_rows_count].color_id = m;
        all_rows_count++;
    }
    for (uint8_t k = 0; k < MATERIAL_EXTENDED_COUNT; k++) {
        const char *nm = material_name(MATX(k));
        if (nm[0] == '?' && nm[1] == '\0') {
            continue;  /* unnamed extended slot - material_name()'s own
                        * fallback for a row nobody has claimed yet */
        }
        all_rows[all_rows_count].name = nm;
        all_rows[all_rows_count].r    = &extended_reactions[k];
        /* Every extended material shares MAT_EXTENDED's one physics row -
         * see material.h's own comment on why. */
        all_rows[all_rows_count].kind = (material_kind_t)materials[MAT_EXTENDED].kind;
        all_rows[all_rows_count].self_id = MAT_EXTENDED;
        all_rows[all_rows_count].color_id = MATX(k);
        all_rows_count++;
    }
}

static bool row_is_empty(const reaction_t *r)
{
    const unsigned char *bytes = (const unsigned char *)r;
    for (size_t i = 0; i < sizeof(*r); i++) {
        if (bytes[i] != 0) return false;
    }
    return true;
}

/*-----------------------------------------------------------------------
 * Per-material clause emitters, one per group, called in group sort-key
 * order for every material row. Each one is a no-op unless its group's
 * driver field is nonzero, so a material with (say) no plant fields at
 * all costs nothing but a skipped comparison.
 *---------------------------------------------------------------------*/

static void emit_ignite(const reaction_t *r, uint8_t self_id)
{
    if (r->flammability == 0) return;

    const char *adv = adverb("flammability", r->flammability);
    const char *air = r->needs_air ? ", only where it touches air" : "";

    /* heat_sources names the same `burns != 0` materials
     * emit_pairwise_table() already joins into "Fire / Lava" for this exact
     * relationship (see pred_burns) - derived from the data, not typed out,
     * so a third heat source would show up here too. It is not the
     * complete list of things that can set a neighbour alight (a burning
     * log spreads to the wood beside it too, via burn_decay - see
     * step_one_burning_cell() in sand_reactions.c), only the materials that
     * ARE a heat source in their own right, which is what "melts to X" and
     * "catches fire" are naming. */

    /* "0 (MAT_EMPTY) is read as MAT_FIRE" - reaction_t.ignites_to's own
     * comment in material.h, and try_ignite() (sand_reactions.c) does
     * exactly that. Not an inference: it is the field's documented and
     * coded default. That comment also names the other two shapes
     * `ignites_to` takes, and each gets its own sentence rather than one
     * template forcing all three through "becoming %s":
     *
     *   resolves to MAT_FIRE (explicit, or the 0 default) - the fuel is
     *   simply gone, replaced by flame. "becoming Fire" would be true but
     *   redundant, so it is dropped rather than said.
     *
     *   resolves to itself (wood) - burning is a STATE of this material,
     *   not a transformation into a different one. The same comment says
     *   why: it "stays put and keeps burning rather than turning into a
     *   flame that immediately floats away". "becoming Wood" says the
     *   opposite of that - it reads as a change - so the sentence says
     *   what actually happens instead.
     *
     *   resolves to a third material - the fuel chars into something else
     *   entirely. Nothing hits this today, but the shape is real: a
     *   slower fuel could leave behind ash or coal instead of relighting
     *   as itself. */
    if (r->ignites_to == 0 || r->ignites_to == MAT_FIRE) {
        printf("- Catches fire %s from %s%s.\n", adv, heat_sources, air);
    } else if (r->ignites_to == self_id) {
        printf("- Catches fire %s from %s%s, and burns where it stands, "
               "rather than flaring away.\n", adv, heat_sources, air);
    } else {
        printf("- Catches fire %s from %s%s, charring to %s.\n", adv,
               heat_sources, air, prose_name(to_name(r->ignites_to)));
    }
}

static void emit_burn(const reaction_t *r)
{
    if (r->burns == 0 && r->burn_decay == 0) return;

    if (r->burns != 0) {
        printf("- Is a heat source in its own right");
    } else {
        printf("- Once alight, burns down %s",
               adverb("burn_decay", r->burn_decay));
    }
    if (r->flare != 0) {
        printf(", and licks flame into an empty neighbour %s",
               adverb("flare", r->flare));
    }
    if (r->residue != 0) {
        /* `residue` is a one-shot chance at the moment a burn finishes,
         * not a per-step rate - adverb() already returns a frequency word
         * ("often", "rarely", ...) for it, so no "sometimes (%s)" wrapper
         * is needed around it any more. */
        printf("; %s leaves smoke when it burns out",
               adverb("residue", r->residue));
    }

    /* Quenching. step_one_burning_cell() (sand_reactions.c) reads
     * quench_to only on the `burns` path - a burn_decay material's cell
     * is reset to unlit (variant 0) on contact with a quenching liquid
     * WITHOUT ever consulting quench_to, so printing quench_to's value
     * for that case would describe a field the code provably never reads.
     * That is a code fact pulled from the read site, not a guess, and it
     * is the kind of thing this generator is allowed to know - it is not
     * a hidden threshold the way SAND_SHOCK_HEAT is. */
    if (r->burns != 0) {
        const char *quenched = (r->quench_to != 0)
                                    ? prose_name(to_name(r->quench_to))
                                    : "nothing";
        printf(". Touched by a quenching liquid, becomes %s", quenched);
    } else {
        printf(". Touched by a quenching liquid, simply goes out");
    }
    printf(".\n");

    if (r->vent_chance != 0) {
        printf("- If covered from above - anything directly above it, "
               "up-left, or up-right, gravity-relative - vents through "
               "the lid %s, throwing up to SAND_VENT_REACH cells along "
               "each covered direction independently.\n",
               adverb("vent_chance", r->vent_chance));
    }
}

static void emit_transform(const reaction_t *r)
{
    if (r->heats_to == 0) return;
    if (r->heat_ramp != 0) {
        /* Banked, not rolled - see GRP_TEMPERATURE for the ramp/drain
         * pair that decides how long "long" is.
         *
         * The cause folds into "long heat" itself ("under long heat from
         * %s") rather than a leading "Beside %s," clause (the no-ramp
         * branch below). Glass is the one row on this path today, and its
         * own heats_to is lava - one of heat_sources's two members - so a
         * leading "Beside fire or lava, melts to lava" would read as a
         * tautology (naming lava as both cause and product back to back).
         * Naming the source through what heats it, instead of what stands
         * beside it, says the same true thing without that collision -
         * and reads fine whether or not a future banked reaction's product
         * happens to be a heat source too, so it is not a glass-specific
         * branch. */
        printf("- Under long heat from %s, melts to %s.\n", heat_sources,
               prose_name(to_name(r->heats_to)));
    } else {
        printf("- Beside %s, melts to %s %s.\n", heat_sources,
               prose_name(to_name(r->heats_to)),
               adverb("heat_chance", r->heat_chance));
        if (r->flaw_to != 0) {
            /* Same trigger as the clause just printed - the SAME roll,
             * not a second one - so this reads as a qualifier on it
             * rather than a separate reaction. */
            printf("  %s comes out as %s instead, in clumped runs "
                   "rather than an even speckle.\n",
                   adverb("flaw_chance", r->flaw_chance),
                   prose_name(to_name(r->flaw_to)));
        }
    }
}

static void emit_spoils(const reaction_t *r)
{
    if (r->spoils_to == 0) return;
    printf("- Spoils into %s %s.\n", prose_name(to_name(r->spoils_to)),
           CAUSE);
}

static void emit_temperature(const reaction_t *r)
{
    if (r->heat_ramp == 0 && r->conducts == 0) return;
    /* Metal conducts (see MATX_METAL's own row) with no heat_ramp at all -
     * deliberately, per its own comment: no variant to bank heat in, so it
     * SURVIVES heat rather than holding any. "Holds heat never" would say
     * the field applies and just came out at the bottom of the ladder,
     * which is a different claim from "this material has no ramp" - so the
     * phrase is dropped rather than printed with adverb_for(0). */
    if (r->heat_ramp != 0) {
        printf("- Holds heat %s", adverb("heat_ramp", r->heat_ramp));
        if (r->conducts != 0) {
            printf(" and passes it on %s", adverb("conducts", r->conducts));
        }
        if (r->cools != 0) {
            printf(", draining back to ambient %s once nothing is heating "
                   "it", adverb("cools", r->cools));
        }
    } else {
        printf("- Passes heat on %s, without banking any of it itself",
               adverb("conducts", r->conducts));
    }
    printf(".\n");
}

static void emit_cold(const reaction_t *r)
{
    if (r->chills == 0) return;
    printf("- Chills whatever it touches %s.\n",
           adverb("chills", r->chills));
}

static void emit_warmth(const reaction_t *r)
{
    if (r->warms == 0) return;
    printf("- Warms whatever it touches %s, without igniting or "
           "quenching anything.\n", adverb("warms", r->warms));
}

static void emit_thaw(const reaction_t *r)
{
    if (r->thaws == 0) return;
    /* `thaws` shares its product with GRP_TRANSFORM's `heats_to` - the
     * liquid-contact roll in sand_reactions.c writes the same target
     * field the heat-contact roll does, so naming it here is the field's
     * actual behaviour, not redundant prose borrowed from another group. */
    if (r->heats_to != 0) {
        printf("- Melts in any liquid it touches %s, becoming %s.\n",
               adverb("thaws", r->thaws), prose_name(to_name(r->heats_to)));
    } else {
        printf("- Melts in any liquid it touches %s.\n",
               adverb("thaws", r->thaws));
    }
}

static void emit_wet(const reaction_t *r)
{
    if (r->wets != 0) {
        printf("- Wets whatever it touches: things that soak will draw "
               "it in.\n");
    }
    if (r->soaks != 0) {
        /* `soaks` only ever fires beside a wetting liquid - see
         * sand_reactions.c's own soaking loop, which skips a neighbour
         * outright when reaction_of(n)->wets == 0 - so this names
         * wetting_liquids, not "any liquid" (that claim is `thaws`'s
         * alone; see this file's comment on wetting_liquids). */
        if (r->soaks_to != 0) {
            printf("- Soaks up any %s it touches %s, becoming %s "
                   "once it takes a unit in.\n",
                   wetting_liquids, adverb("soaks", r->soaks),
                   prose_name(to_name(r->soaks_to)));
        } else {
            printf("- Soaks up any %s it touches %s, growing wetter "
                   "(its own moisture rises) rather than changing into "
                   "anything.\n", wetting_liquids, adverb("soaks", r->soaks));
        }
    }
    if (r->dries != 0) {
        printf("- Dries back out %s, on its own, with no partner "
               "needed.\n", adverb("dries", r->dries));
    }
}

static void emit_acid(const reaction_t *r)
{
    if (r->dissolves != 0) {
        printf("- Dissolves an adjacent cell %s", adverb("dissolves", r->dissolves));
        if (r->fizz != 0) {
            /* `fizz` is a one-shot chance on the cell just eaten, not a
             * rate - adverb() gives a frequency word here, so it drops
             * straight in without the old "sometimes (%s)" wrapper. */
            printf(", %s leaving smoke behind", adverb("fizz", r->fizz));
        }
        printf(".\n");
    }
    if (r->dissolvable != 0) {
        /* `dissolvable` is the chance a single ATTEMPT to dissolve THIS
         * material succeeds (see its own comment in material.h) - an EASE
         * question, not a frequency one, so adverb() reads it through
         * ease_words[] rather than frequency_words[] (see field_docs[]'s
         * FCHANCE_VOCAB row for this field) and the material stays the
         * subject: "Dissolves in acid easily", not the fronted-adverb
         * "Usually gives way to acid". */
        printf("- Dissolves in acid %s.\n",
               adverb("dissolvable", r->dissolvable));
    }
}

static void emit_evaporates(const reaction_t *r)
{
    if (r->evaporates == 0) return;
    printf("- Spontaneously evaporates into gas %s - unconditional, no "
           "heat or neighbour required.\n",
           adverb("evaporates", r->evaporates));
}

static void emit_grow(const reaction_t *r)
{
    if (r->grows != 0) {
        printf("- Grows into wet soil %s, against gravity, spending a "
               "level of that soil's moisture per cell.\n",
               adverb("grows", r->grows));
    }
    if (r->falls != 0) {
        printf("- Falls under gravity %s when there is empty space "
               "beneath it.\n", adverb("falls", r->falls));
    }
    if (r->withers != 0) {
        if (r->sheltered_by != 0) {
            printf("- Withers away %s if it cannot reach water through "
                   "its own roots and is not touching %s.\n",
                   adverb("withers", r->withers),
                   prose_name(to_name(r->sheltered_by)));
        } else {
            printf("- Withers away %s if it cannot reach water through "
                   "its own roots.\n", adverb("withers", r->withers));
        }
    }
}

static void emit_harden(const reaction_t *r)
{
    if (r->hardens_to == 0) return;
    /* `harden_chance`, `holds_line` and `canopy` are each a one-shot
     * decision made once, at the moment a run hardens - not a per-step
     * rate against a partner - so adverb() gives each a frequency word
     * ("usually", "sometimes", ...) here, and each is phrased before its
     * verb ("usually hardens") rather than after ("hardens usually"),
     * which is the reading a frequency word wants. */
    printf("- A straight run of %u cells %s hardens into %s",
           (unsigned)r->harden_run, adverb("harden_chance", r->harden_chance),
           prose_name(to_name(r->hardens_to)));
    if (r->trunk_girth != 0) {
        printf(", up to %u cells wider at the foot than at the tip",
               (unsigned)r->trunk_girth);
    }
    if (r->holds_line != 0) {
        printf(", and %s a limb holds its own direction (rather than "
               "bending back toward gravity)",
               adverb("holds_line", r->holds_line));
    }
    if (r->clings_to != 0) {
        printf("; the hardened body counts as part of %s",
               prose_name(to_name(r->clings_to)));
    }
    printf(".\n");
    if (r->canopy != 0 && r->canopy_to != 0) {
        printf("- The moment it hardens, it also %s leafs its crown with "
               "%s, one candidate space at a time.\n",
               adverb("canopy", r->canopy), prose_name(to_name(r->canopy_to)));
    }
}

static void emit_regrow(const reaction_t *r)
{
    if (r->sprouts != 0 && r->sprouts_to != 0) {
        printf("- Standing in wet soil, sprouts %s beside itself %s.\n",
               prose_name(to_name(r->sprouts_to)), adverb("sprouts", r->sprouts));
    }
    if (r->buds != 0 && r->buds_to != 0) {
        printf("- Once already in leaf and able to reach water, buds new "
               "%s beside itself %s.\n",
               prose_name(to_name(r->buds_to)), adverb("buds", r->buds));
    }
    if (r->drinks != 0) {
        /* Same wetting-liquid gate as `soaks` - step_one_drinking_cell()
         * (sand_reactions.c) requires reaction_of(n)->wets != 0 on the
         * neighbour it drinks from - so this names wetting_liquids rather
         * than the generic "a liquid" the old wording implied. */
        printf("- Touching %s and rooted in soil with room for "
               "more, drinks %s - the water comes out as a level of "
               "moisture in the soil at its root, not in itself.\n",
               wetting_liquids, adverb("drinks", r->drinks));
    }
}

static void emit_shatter(const reaction_t *r)
{
    if (r->shatters_to == 0) return;
    printf("- Shatters into %s %s.\n", prose_name(to_name(r->shatters_to)),
           CAUSE);
}

static void emit_material_section(const char *name, const reaction_t *r,
                                  uint8_t self_id)
{
    if (row_is_empty(r)) return;
    printf("\n### %s\n\n", name);
    emit_ignite(r, self_id);
    emit_burn(r);
    emit_transform(r);
    emit_spoils(r);
    emit_temperature(r);
    emit_cold(r);
    emit_warmth(r);
    emit_thaw(r);
    emit_wet(r);
    emit_acid(r);
    emit_evaporates(r);
    emit_grow(r);
    emit_harden(r);
    emit_regrow(r);
    emit_shatter(r);
}

/*-----------------------------------------------------------------------
 * The pairwise A + B -> C table.
 *
 * Keyed on the RATE field and branching on the target - never the other
 * way around. Walking `*_to` fields and asking what triggers them would
 * miss every reaction whose target IS its own row (dirt getting wetter)
 * or a third cell entirely (a plant drinking) - see the plan's "The
 * pairwise join is phase 1, not phase 2" section, which is the reason
 * this table exists at all rather than being deferred.
 *---------------------------------------------------------------------*/

static bool is_burning_material(const reaction_t *r)
{
    return r->burns != 0;
}

/* Mirrors neighbor_quenches() (sand_reactions.c) exactly: a liquid quenches
 * iff it is neither fuel nor itself a heat source. Not a magic threshold -
 * both fields it reads are already in the table this program links. */
static bool is_quenching_liquid(const mrow_t *row)
{
    return row->kind == KIND_LIQUID && row->r->flammability == 0 &&
           row->r->burns == 0;
}

static void join_names(bool (*pred)(const mrow_t *), const char *sep,
                       char *out, size_t cap)
{
    out[0] = '\0';
    bool first = true;
    for (size_t i = 0; i < all_rows_count; i++) {
        if (!pred(&all_rows[i])) continue;
        size_t len = strlen(out);
        int n = snprintf(out + len, cap - len, "%s%s", first ? "" : sep,
                          all_rows[i].name);
        (void)n;
        first = false;
    }
    if (out[0] == '\0') {
        snprintf(out, cap, "(none)");
    }
}

static bool pred_burns(const mrow_t *row) { return row->r->burns != 0; }
static bool pred_kind_liquid(const mrow_t *row) { return row->kind == KIND_LIQUID; }
static bool pred_wets_liquid(const mrow_t *row)
{
    return row->kind == KIND_LIQUID && row->r->wets != 0;
}

static void print_join_row(const char *a, const char *b, const char *becomes,
                           const char *rate, const char *note)
{
    printf("| %s | %s | %s | %s | %s |\n", a, b, becomes, rate, note);
}

static void emit_pairwise_table(void)
{
    printf("\n## Pairwise reactions\n\n");
    printf("Generated by walking the RATE field that drives each reaction "
           "and branching on its target, never by walking `*_to` fields - "
           "see this file's own top comment on the pairwise join for why "
           "(`soaks_to == 0` is a real reaction with no target, and "
           "iterating `*_to` alone would silently skip it).\n\n");
    printf("| A | B | becomes | rate | note |\n");
    printf("|---|---|---|---|---|\n");

    char burners[256];
    join_names(pred_burns, " / ", burners, sizeof(burners));
    char liquids[256];
    join_names(pred_kind_liquid, " / ", liquids, sizeof(liquids));

    /* dissolves x dissolvable. `fizz` is read from the DISSOLVER's own row
     * (all_rows[i], not the cell being eaten) - see step_one_dissolver_
     * cell() in sand_reactions.c, which rolls r->fizz on the acid cell at
     * the moment it eats a neighbour. */
    for (size_t i = 0; i < all_rows_count; i++) {
        if (all_rows[i].r->dissolves == 0) continue;
        for (size_t j = 0; j < all_rows_count; j++) {
            if (all_rows[j].r->dissolvable == 0) continue;
            char becomes[64];
            snprintf(becomes, sizeof(becomes), "nothing%s",
                     all_rows[i].r->fizz != 0 ? " (sometimes smoke)" : "");
            char rate[64];
            snprintf(rate, sizeof(rate), "%s / %s",
                     adverb("dissolves", all_rows[i].r->dissolves),
                     adverb("dissolvable", all_rows[j].r->dissolvable));
            print_join_row(all_rows[i].name, all_rows[j].name, becomes,
                           rate, "both rolls must pass");
        }
    }

    /* flammability/ignites_to x burns. Same three-way split as
     * emit_ignite() (see that function's own comment on ignites_to's
     * three shapes): 0 or MAT_FIRE reads as Fire, a third material reads
     * as that material, and igniting into SELF reads as "<name>, alight" -
     * never the bare material name, which would read as a no-op ("Wood ->
     * becomes: Wood") when what actually happens is a state change, not a
     * material change. */
    for (size_t i = 0; i < all_rows_count; i++) {
        if (all_rows[i].r->flammability == 0) continue;
        char becomes[64];
        if (all_rows[i].r->ignites_to == 0 ||
            all_rows[i].r->ignites_to == MAT_FIRE) {
            snprintf(becomes, sizeof(becomes), "Fire");
        } else if (all_rows[i].r->ignites_to == all_rows[i].self_id) {
            snprintf(becomes, sizeof(becomes), "%s, alight", all_rows[i].name);
        } else {
            snprintf(becomes, sizeof(becomes), "%s",
                     to_name(all_rows[i].r->ignites_to));
        }
        print_join_row(all_rows[i].name, burners, becomes,
                       adverb("flammability", all_rows[i].r->flammability),
                       all_rows[i].r->needs_air ? "only where it touches air"
                                                : "");
    }

    /* heats_to x burns (memoryless and ramped both go through the same
     * try_heat_transform() trigger - contact with a burning cell, or
     * through a conductor) */
    for (size_t i = 0; i < all_rows_count; i++) {
        if (all_rows[i].r->heats_to == 0) continue;
        char rate[64];
        if (all_rows[i].r->heat_ramp != 0) {
            snprintf(rate, sizeof(rate), "under long heat (banked)");
        } else {
            snprintf(rate, sizeof(rate), "%s",
                     adverb("heat_chance", all_rows[i].r->heat_chance));
        }
        print_join_row(all_rows[i].name, burners, to_name(all_rows[i].r->heats_to),
                       rate, "or through a conductor");
    }

    /* quench_to x quenching liquids */
    for (size_t i = 0; i < all_rows_count; i++) {
        if (!is_burning_material(all_rows[i].r) &&
            all_rows[i].r->burn_decay == 0) {
            continue;
        }
        char qbuf[256];
        join_names(is_quenching_liquid, " / ", qbuf, sizeof(qbuf));
        if (all_rows[i].r->burn_decay != 0) {
            /* burn_decay path: quench_to is never read - see emit_burn()'s
             * own comment on step_one_burning_cell(). */
            print_join_row(all_rows[i].name, qbuf, "itself, unlit",
                           "on contact",
                           "quench_to is not read on this path");
        } else {
            const char *becomes = (all_rows[i].r->quench_to != 0)
                                       ? to_name(all_rows[i].r->quench_to)
                                       : "nothing";
            print_join_row(all_rows[i].name, qbuf, becomes, "on contact", "");
        }
    }

    /* chills x heat_ramp/shatters_to */
    for (size_t i = 0; i < all_rows_count; i++) {
        if (all_rows[i].r->chills == 0) continue;
        for (size_t j = 0; j < all_rows_count; j++) {
            if (all_rows[j].r->heat_ramp == 0) continue;
            char becomes[64];
            snprintf(becomes, sizeof(becomes), "%s, one heat level cooler",
                     all_rows[j].name);
            print_join_row(all_rows[i].name, all_rows[j].name, becomes,
                           adverb("chills", all_rows[i].r->chills), "");
            if (all_rows[j].r->shatters_to != 0) {
                print_join_row(all_rows[i].name, all_rows[j].name,
                               to_name(all_rows[j].r->shatters_to), CAUSE,
                               "only if B is hot enough when A touches it");
            }
        }
    }

    /* wets x soaks - the wetting family. Four reactions, not a loop over
     * *_to: see this file's top comment and the plan's own section on it. */
    for (size_t i = 0; i < all_rows_count; i++) {
        if (all_rows[i].r->wets == 0) continue;
        for (size_t j = 0; j < all_rows_count; j++) {
            if (all_rows[j].r->soaks == 0) continue;
            if (all_rows[j].r->soaks_to != 0) {
                print_join_row(all_rows[i].name, all_rows[j].name,
                               to_name(all_rows[j].r->soaks_to),
                               adverb("soaks", all_rows[j].r->soaks),
                               "the liquid pays a unit of its own mass");
            } else {
                char becomes[64];
                snprintf(becomes, sizeof(becomes), "%s, +1 moisture",
                         all_rows[j].name);
                print_join_row(all_rows[i].name, all_rows[j].name, becomes,
                               adverb("soaks", all_rows[j].r->soaks),
                               "no material change - soaks_to is 0");
            }
        }
        /* drinks: a THIRD cell changes (the soil at the root), not the
         * subject and not the liquid - see reaction_t.drinks. */
        for (size_t j = 0; j < all_rows_count; j++) {
            if (all_rows[j].r->drinks == 0) continue;
            print_join_row(all_rows[i].name, all_rows[j].name,
                           "the soil at B's root, +1 moisture",
                           adverb("drinks", all_rows[j].r->drinks),
                           "B itself is unchanged - a third cell changes");
        }
    }
    /* dries: self-driven, no partner at all. */
    for (size_t i = 0; i < all_rows_count; i++) {
        if (all_rows[i].r->dries == 0) continue;
        char becomes[64];
        snprintf(becomes, sizeof(becomes), "%s, -1 moisture", all_rows[i].name);
        print_join_row(all_rows[i].name, "(none - self-driven)", becomes,
                       adverb("dries", all_rows[i].r->dries), "");
    }

    /* thaws x any KIND_LIQUID */
    for (size_t i = 0; i < all_rows_count; i++) {
        if (all_rows[i].r->thaws == 0) continue;
        print_join_row(all_rows[i].name, liquids, to_name(all_rows[i].r->heats_to),
                       adverb("thaws", all_rows[i].r->thaws),
                       "any liquid counts, not water alone");
    }
}

/*-----------------------------------------------------------------------
 * "How these sentences are built."
 *
 * Not the per-material table itself - that stays clean, unmarked prose,
 * on purpose: it is the deliverable a future brush-description feature
 * will read from, not a place for this file's own internals to leak into.
 * This is a SEPARATE section, appended after the pairwise table, showing
 * one representative sentence per group (see group_id_t) with its slots
 * marked, so a reader can see how much of each sentence is a generated
 * slot and how much is hand-written glue inside an emit_*() function.
 *
 * Every marked RATE and OBJECT word below is pulled through the exact
 * same adverb()/to_name()/prose_name() calls (and the exact same live
 * reaction_t rows) the real per-material clauses use above - so those
 * words track material.c and cannot go stale the way a typed-out example
 * would. The GLUE text cannot be sourced the same way: it is prose typed
 * directly into the matching emit_*() function's printf() calls, so it is
 * transcribed by hand from that function here, and needs a matching edit
 * if that function's own wording ever changes - each example below names
 * the emit_*() function and fields it mirrors, to make that edit findable.
 *
 * The material slots are marked with inline LaTeX colour spans -
 * $\textcolor{#RRGGBB}{\text{...}}$, SINGLE dollars on each side. An
 * earlier version used $${\color{...}...}$$ per slot and failed three ways
 * at once: $$...$$ is DISPLAY math (a block element), so every marked span
 * broke onto its own centred line and shredded the sentence; VSCode's math
 * extension reads a bare $ as its own inline delimiter, so it saw $$ as two
 * of those and threw a KaTeX parse error; and even rendered correctly,
 * KaTeX sets the words in an italic serif math font that clashes with the
 * surrounding sans prose. A version after that switched to plain markdown
 * emphasis - **bold**, `code`, *italic* - which sidesteps all three
 * failures but loses colour: three visual weights cannot carry five
 * distinct slots, so Subject had to share **bold** with Verb and Cause had
 * to fold into unmarked glue. Inline single-dollar math keeps colour and
 * drops the block-math and font problems: $\textcolor{...}{\text{...}}$
 * flows inline, renders upright (`\text{}` switches back out of math
 * italics), and every material name below gets its own colour rather than
 * sharing a channel - see COLOUR MEANS MATERIAL, NOTHING ELSE below for
 * why grammar roles do NOT also compete for that same channel any more,
 * which is what lets plain markdown emphasis back in for them without
 * repeating that earlier failure.
 *
 * WHICH COLOUR A MATERIAL GETS
 *
 * Subject and Object used to be two fixed, unrelated colours (a flat red
 * for whichever row the sentence is about, a flat green for whichever
 * material a field's value names) - which meant the colour told you SLOT,
 * not SUBSTANCE: Wood and Glass rendered in the identical red, Steam and
 * Sand in the identical green, even though the whole point of colouring a
 * material anywhere else in this game is that no two materials share a
 * colour. Every material NAME below - subject or object, and the literal
 * word "fire" inside "Catches fire" (emit_ignite()'s own wording for
 * ignites_to's 0/MAT_FIRE default, not a to_name() call, but naming
 * MAT_FIRE all the same) - now renders in THAT material's own colour
 * instead: material_hex() below reads it straight out of
 * material_palette(), the exact array the panel itself renders from, at
 * the same representative swatch a freshly painted cell of that material
 * actually takes - see representative_variant()'s own comment for why that
 * is not simply app_sand.c's brush_color() variant-13 shortcut, and
 * material_hex()'s for the extended-cell case, which still is.
 *
 * COLOUR MEANS MATERIAL, NOTHING ELSE
 *
 * Verb, Rate/frequency and Cause used to keep one fixed colour each, the
 * same way Subject/Object once did before the fix just above - which left
 * colour answering two unrelated questions at once (WHICH material, and
 * WHICH grammar role), and the palette only ever had an opinion on the
 * first one: nothing in material_palette() says what colour `melts`
 * should be, so those three colours were arbitrary picks this file made
 * up out of nowhere, not derived from anything the game itself knows.
 * Colour now means exactly one thing on this whole page - "this word is a
 * material" - and grammar role moved to typography instead: MARK_VERB
 * prints as *italic* (a light touch for the action), MARK_RATE as
 * **bold** (every rate word scannable in one pass - exactly what the
 * by-feel adverb tuning pass still ahead of us, phase 2, needs), and
 * MARK_CAUSE as ***bold italic*** (a clause rather than a single word, and
 * the rarest thing on this page - every instance today is the CAUSE
 * placeholder below). Glue stays plain and unmarked, same as always. This
 * is not a return to the all-markdown attempt two paragraphs up, which
 * failed because it had to carry FIVE distinct slots (Subject, Object,
 * Verb, Rate, Cause) through three visual weights: with material identity
 * now handled entirely by its own per-instance LaTeX colour span, the
 * typography channel only has to tell apart THREE grammar roles plus
 * unmarked glue - one weight per role, nothing sharing, nothing left
 * over. See print_marked() below for where each mark_t turns into its
 * treatment, and the Legend this function prints for the reader-facing
 * version of this same rationale. */

typedef enum {
    MARK_NONE,     /* glue (see the Legend below) - printed as ordinary
                    * markdown text, in the reader's own theme colour. */
    MARK_MATERIAL, /* a material's own name - colour is per-INSTANCE, not
                    * per-slot: two MARK_MATERIAL segments in the same
                    * sentence (a subject and a cause used to be able to
                    * share a word; now Wood and Fire never share a colour)
                    * each carry their own hex in seg_t.color, computed by
                    * material_hex() below. Colour means exactly one thing
                    * on this page - "this word is a material" - so this is
                    * the only mark that still carries one; see COLOUR
                    * MEANS MATERIAL, NOTHING ELSE above. */
    MARK_VERB,     /* the action - *italic*, see print_marked() below and
                    * the Legend this file prints. */
    MARK_RATE,     /* rate / frequency - **bold**. */
    MARK_CAUSE,    /* a trigger clause - ***bold italic***. */
} mark_t;

/* "#rrggbb" plus the NUL, the fixed width every material_hex() output (and
 * every buffer meant to hold one) needs. */
#define COLOR_LEN 8

typedef struct {
    mark_t mark;
    const char *color; /* only meaningful when mark == MARK_MATERIAL - the
                        * only mark that carries a colour at all (see
                        * mark_t's own comment). NULL for every other mark:
                        * MARK_VERB/MARK_RATE/MARK_CAUSE render as markdown
                        * emphasis instead (see print_marked() below), and
                        * MARK_NONE is unmarked glue. */
    const char *text;
} seg_t;

static const mrow_t *find_row(const char *name)
{
    for (size_t i = 0; i < all_rows_count; i++) {
        if (strcmp(all_rows[i].name, name) == 0) {
            return &all_rows[i];
        }
    }
    fprintf(stderr, "dump_reactions: find_row(\"%s\") - no such material\n",
            name);
    exit(1);
}

/* The variant a FRESHLY PAINTED cell of `material` actually gets, mirrored
 * from random_cell() in sand.c - deliberately, not independently reasoned
 * about: that function is the one place that decides what a fresh cell of
 * each material looks like, and this doc's swatches are supposed to be
 * showing the reader that exact cell rather than a second, possibly
 * disagreeing, guess at it. If random_cell() ever grows a branch, changes
 * a constant, or reorders its checks, this needs the same change or the
 * two silently drift apart again - check both when you touch either.
 *
 * The one deliberate difference: random_cell() draws one random number
 * where its variant is a shade (the plain shade-band case, and the tone
 * half of a drying material's variant); this picks the CENTRE of that same
 * range instead, every time, because a swatch needs no RNG and a fixed
 * doc needs a fixed answer.
 *
 * This used to be a fixed "+13" on every ordinary material - copied from
 * app_sand.c's brush_color(), which still does that (see this file's own
 * top comment on why that file cannot be touched here). +13 happened to
 * read as a plausible shade for sand, which is the ONE material it was
 * ever measured against, and was wrong for everything whose variant means
 * something other than a shade: it read glass and stone as hot (they bank
 * TEMPERATURE in their variant, and 13 is far above SAND_AMBIENT_HEAT),
 * wood as mid-burn (its variant is LIFE LEFT TO BURN, and 13 is one ember
 * short of fully consumed), and sand itself as cullet (13 sits inside the
 * top four shades sand.c reserves for glass that has been broken back down
 * to sand - see SAND_CULLET_BASE in material.h - not in the dune band an
 * ordinary painted grain actually takes). A variant does not mean the same
 * thing for every material, so one fixed number cannot be a representative
 * swatch for all of them; only asking each material's own fields, the way
 * random_cell() does, can. */
static uint8_t representative_variant(material_id_t material)
{
    /* A fresh liquid cell is a full one - see random_cell()'s own comment. */
    if (materials[material].kind == KIND_LIQUID) {
        return MASS_MAX;
    }
    /* A transient material starts at full life. */
    if (materials[material].decay != 0) {
        return MATERIAL_VARIANTS - 1;
    }
    /* A heat-ramping material starts at room temperature. */
    if (reactions[material].heat_ramp != 0) {
        return SAND_AMBIENT_HEAT;
    }
    /* A material that burns only while lit starts unlit. */
    if (reactions[material].burn_decay != 0) {
        return 0;
    }

    /* Everything past this point is banded by `band` in random_cell() - the
     * point in the shade cycle (or, for a drying material, the tone cycle)
     * the caller's pour has drifted to - and then jittered by one random
     * step either side of it. There is no pour here and nothing to centre
     * on, so this picks the middle of the whole range instead: not a
     * point `band` could ever equal (band drifts with pour_phase, a
     * runtime counter this doc does not have), just the representative
     * centre a jittered draw would scatter around. */
    const int span = MATERIAL_SHADE_SPAN(material);
    const int mid  = span / 2;

    /* A drying material's variant is moisture in the low bits and a
     * carried tone in the top one - see material.h's own comment on
     * SOIL_MOISTURE_BITS. A fresh cell is bone dry (moisture 0, exactly,
     * not centred - random_cell() never randomises this half); the tone
     * half IS randomised there, so it gets the same centring as the plain
     * shade band below, folded into SOIL_TONES's own range. SOIL_TONES is
     * 2, so "middle" is not a value rng_below() could draw as a middle
     * either - it is simply mid's own parity, which is what an even-valued
     * band (as `mid` always is on this table's spans) would hand
     * random_cell() on 7 of every 8 calls, before the eighth-chance flip
     * to the other tone that makes this the ordinary case, not a rare one. */
    if (reactions[material].dries != 0) {
        const unsigned tone = (unsigned)mid % SOIL_TONES;
        return (uint8_t)(tone << SOIL_MOISTURE_BITS);
    }

    /* The plain case: a shade, centred on the band's own middle. Sand
     * stops short of the top four shades (SAND_CULLET_BASE in material.h)
     * so a painted dune can never read as cullet; MATERIAL_SHADE_SPAN()
     * already encodes that for every material, sand included. */
    return (uint8_t)mid;
}

/* Writes the colour material value v resolves to, as "#rrggbb", into buf -
 * v decodes exactly the way to_name() decodes a TARGET field (a plain
 * material id, or a whole MATX() cell spec), so any raw field value or an
 * all_rows[] row's own mrow_t.color_id can be passed straight through.
 *
 * Reads material_palette() at the representative swatch a freshly painted
 * cell of this material actually takes - representative_variant() above
 * for an ordinary material (see its own comment for why that is not the
 * "+13" app_sand.c's brush_color() still uses), the whole cell unchanged
 * for an extended one, exactly as random_cell() itself leaves it (an
 * extended cell's low nibble names WHICH extended material this is, not a
 * shade, so there is no variant for random_cell() - or this - to pick; see
 * try_spawn_one()'s own comment in sand.c). This doc's colours are
 * therefore the colours a freshly poured cell of each material actually
 * shows on the panel, not a second independent guess at them - and, for
 * the four materials whose variant means something other than a plain
 * shade, no longer the same guess brush_color() makes either.
 *
 * gfx_color_t is RGB565 with the two bytes swapped (GFX_RGB(), gfx_color.h
 * - the QSPI panel wants the opposite byte order to the chip's native
 * layout), so recovering 0xRRGGBB is GFX_RGB() run backwards: swap the
 * bytes back, split into 5/6/5 bit fields, then scale each field up to
 * 8 bits with round-to-nearest ((n * 255 + half_max) / max) rather than a
 * naive shift, so 0x1F (5-bit max) recovers as 0xFF (8-bit max) and not
 * 0xF8.
 *
 * Takes a caller-owned buffer rather than a shared static one (contrast
 * prose_name(), which gets away with exactly one shared buffer because
 * nothing here ever calls it twice before printing) - an anatomy example
 * can need two or more colours live at once inside a single seg_t[] build
 * (GRP_HARDEN's hardens_to and clings_to, or a heat_sources list with two
 * members), and one shared buffer would silently turn every earlier colour
 * into the last one computed. buf must be at least COLOR_LEN bytes. */
static void material_hex(uint8_t v, char *buf, size_t cap)
{
    const cell_t base = (v >= (uint8_t)(MAT_EXTENDED << 4))
                             ? (cell_t)v : CELL_MAKE(v, 0);
    const cell_t swatch =
        cell_is_extended(base)
            ? base
            : CELL_MAKE(CELL_MATERIAL(base),
                        representative_variant(
                            (material_id_t)CELL_MATERIAL(base)));
    const gfx_color_t packed = material_palette()[swatch];
    const uint16_t rgb565 = (uint16_t)((packed >> 8) | (packed << 8));
    const uint8_t r5 = (rgb565 >> 11) & 0x1Fu;
    const uint8_t g6 = (rgb565 >> 5)  & 0x3Fu;
    const uint8_t b5 = rgb565         & 0x1Fu;
    const uint8_t r8 = (uint8_t)((r5 * 255 + 15) / 31);
    const uint8_t g8 = (uint8_t)((g6 * 255 + 31) / 63);
    const uint8_t b8 = (uint8_t)((b5 * 255 + 15) / 31);
    /* Uppercase, for one consistent hex-digit case across every material
     * colour this doc prints - MARK_VERB/MARK_RATE/MARK_CAUSE carry no
     * colour of their own any more (see mark_t's own comment), so this no
     * longer has a second source of #RRGGBB literals to stay consistent
     * with, just itself across every call site. */
    snprintf(buf, cap, "#%02X%02X%02X", r8, g8, b8);
}

/* One member of a derived material list (heat_sources, wetting_liquids)
 * with its own name and colour copied into stable, per-item storage -
 * never a pointer into prose_name()'s or material_hex()'s shared/reused
 * buffers, which a multi-member list would alias across members (see
 * material_hex()'s own comment on why one shared buffer is not safe here). */
typedef struct {
    char name[32];
    char color[COLOR_LEN];
} list_item_t;

/* Comfortably more than heat_sources's or wetting_liquids's current
 * membership (2 and 1) - see those variables' own top comments: nothing
 * here assumes a fixed count, this is just a generous static bound so
 * collect_material_list() needs no allocation. */
#define LIST_ITEM_MAX 8

/* Fills items[] (capacity cap) with one entry per all_rows[] row pred
 * selects, in table order - the same walk join_names() does to build
 * heat_sources/wetting_liquids as one flat string, except every member
 * keeps its own name AND its own colour instead of collapsing into one
 * shared span (see this section's top comment on why a derived list is no
 * exception to "every material name gets its own colour"). Returns the
 * count filled. */
static size_t collect_material_list(bool (*pred)(const mrow_t *),
                                     list_item_t *items, size_t cap)
{
    size_t count = 0;
    for (size_t i = 0; i < all_rows_count && count < cap; i++) {
        if (!pred(&all_rows[i])) continue;
        snprintf(items[count].name, sizeof(items[count].name), "%s",
                 prose_name(all_rows[i].name));
        material_hex(all_rows[i].color_id, items[count].color,
                     sizeof(items[count].color));
        count++;
    }
    return count;
}

/* Generous headroom over the longest example built below (GRP_HARDEN,
 * eleven segments) so every seg_t[] here is a plain fixed-size local
 * array rather than anything allocated. */
#define SEG_MAX 32

static void seg_glue(seg_t *segs, size_t *n, const char *text)
{
    segs[(*n)++] = (seg_t){ MARK_NONE, NULL, text };
}

static void seg_mark(seg_t *segs, size_t *n, mark_t mark, const char *text)
{
    segs[(*n)++] = (seg_t){ mark, NULL, text };
}

/* A single material-name segment, coloured by its own already-computed hex
 * (see material_hex()) rather than a shared slot colour. */
static void seg_material(seg_t *segs, size_t *n, const char *color,
                          const char *text)
{
    segs[(*n)++] = (seg_t){ MARK_MATERIAL, color, text };
}

/* A whole collect_material_list() result, each member its own coloured
 * segment, sep printed as unmarked glue between members and never after
 * the last one - the same shape join_names() joins into one string, kept
 * apart here instead of flattened (see collect_material_list()'s own
 * comment). */
static void seg_list(seg_t *segs, size_t *n, const list_item_t *items,
                      size_t count, const char *sep)
{
    for (size_t i = 0; i < count; i++) {
        if (i > 0) {
            seg_glue(segs, n, sep);
        }
        seg_material(segs, n, items[i].color, items[i].name);
    }
}

static void print_plain(const seg_t *segs, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        printf("%s", segs[i].text);
    }
    printf("\n");
}

/* Each marked slot gets exactly one treatment applied directly around its
 * own text - never nested, and never doubled. MARK_MATERIAL keeps the
 * single-dollar LaTeX colour span (see this section's own top comment for
 * why $$ display math is not an option); MARK_VERB/MARK_RATE/MARK_CAUSE
 * are plain markdown emphasis instead - a completely separate mechanism
 * from the LaTeX span, so a material segment and a grammar-role segment
 * are never nested inside one another, only ever sequential (see COLOUR
 * MEANS MATERIAL, NOTHING ELSE above for why the two channels no longer
 * collide the way an all-markdown attempt once did).
 *
 * Two markdown-marked segments landing back to back with no glue between
 * them would be a real hazard - e.g. an italic verb run straight into a
 * bold rate word would emit "*word***word2**", which most parsers do not
 * read as italic-then-bold - but every seg_t[] this file builds keeps at
 * least a glue character between two adjacent marked segments (a space, a
 * comma-space, ": "), so a run like "**swiftly**, **often**" lands its
 * markers against that glue, not against another marker, and parses as
 * emphasis rather than literal asterisks. */
static void print_marked(const seg_t *segs, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        switch (segs[i].mark) {
        case MARK_NONE:
            printf("%s", segs[i].text);
            break;
        case MARK_MATERIAL:
            printf("$\\textcolor{%s}{\\text{%s}}$", segs[i].color,
                   segs[i].text);
            break;
        case MARK_VERB:
            printf("*%s*", segs[i].text);
            break;
        case MARK_RATE:
            printf("**%s**", segs[i].text);
            break;
        case MARK_CAUSE:
            printf("***%s***", segs[i].text);
            break;
        }
    }
    printf("\n");
}

/* Plain sentence first, then the marked form immediately after - so the
 * section stays legible without a renderer (a raw diff, an editor, CI)
 * and not only on github.com. */
static void print_example(const char *heading, const seg_t *segs, size_t n)
{
    printf("\n**%s**\n\n", heading);
    print_plain(segs, n);
    printf("\n");
    print_marked(segs, n);
    printf("\n");
}

static void emit_anatomy(void)
{
    printf("\n## How these sentences are built\n\n");
    printf("Not a markup pass on the table above - that table stays as "
           "clean prose, unedited (see the plan's own phasing: it is the "
           "deliverable this whole file exists to produce). This section "
           "is generated separately, from the same decode helpers "
           "(`adverb()`, `to_name()`, `prose_name()`) and the same live "
           "`reactions[]`/`extended_reactions[]` rows, picking one "
           "representative sentence per group and marking which part of "
           "it came from where.\n");

    printf("\n### Legend\n\n");
    printf("Colour means exactly one thing on this page: this word is a "
           "material. Every material name below - subject or object, "
           "wherever it appears, whether it names a single TARGET field "
           "through `to_name()` or is one member of a derived list like "
           "`wetting_liquids`/`heat_sources` walked out by "
           "`collect_material_list()` (each member keeps its own colour "
           "there too, never collapsed into one shared span) - renders in "
           "that material's OWN colour, read straight out of "
           "`material_palette()` at the representative swatch a freshly "
           "painted cell of that material actually takes - mirroring "
           "`random_cell()` in sand.c rather than app_sand.c's brush "
           "indicator, which still shortcuts every ordinary material to "
           "variant 13 (a plausible shade for sand alone, and the wrong "
           "colour for any material whose variant is not a shade) - so "
           "this list "
           "doubles as the key: whatever colour a name gets below is the "
           "colour that same name gets in every example after it.\n\n");

    printf("**Materials**\n\n");
    for (size_t i = 0; i < all_rows_count; i++) {
        char hex[COLOR_LEN];
        material_hex(all_rows[i].color_id, hex, sizeof(hex));
        printf("$\\textcolor{%s}{\\text{%s}}$%s", hex, all_rows[i].name,
               (i + 1 < all_rows_count) ? ", " : "\n\n");
    }

    printf("These are the device's exact palette values, not colours "
           "chosen for legibility - deliberately, so the doc and the panel "
           "never disagree. That means a few names above sit near the "
           "ends of the range on purpose: the palest materials (snow, "
           "sand) wash out against a light background, and the darkest "
           "(stone) washes out against a dark one. If a name anywhere in "
           "this section is hard to read, that is the palette speaking, "
           "not a rendering bug.\n\n");

    printf("Nothing else on this page is coloured. The palette answers "
           "what colour a material is; it has never answered what colour "
           "a verb should be, so the three grammar roles below are marked "
           "with typography instead, one weight per role:\n\n");

    printf("- Verb (*italic*) - the wording tied to the field driving the "
           "clause, set off with a light touch since it is naming an "
           "action, not a substance. `field_docs[]` carries a `verb` "
           "string per field for exactly this reason, but phase 1 never "
           "actually reads that column back out (grep `->verb` in this "
           "file - nothing matches): today the words are typed directly "
           "into the matching `emit_*()` printf() call, kept in sync with "
           "`field_docs[]` by hand instead of by the compiler.\n");
    printf("- Rate / frequency (**bold**) - the ladder bucket: "
           "`adverb_for()` for a genuine per-step rate (SCALE_RATE), or "
           "`chance_bucket_for()` through frequency_words[]/ease_words[] "
           "for a one-shot chance (SCALE_CHANCE). Two different ladders "
           "share this one channel because both answer the same question "
           "- \"which of five bands\" - for the same kind of raw byte; "
           "only the vocabulary differs, and it is DIFFERENT for a reason "
           "(see ease_words[]'s own comment above). Bold makes every rate "
           "word scannable in one pass, which is exactly what the by-feel "
           "adverb tuning pass still ahead of us (phase 2) needs.\n");
    printf("- Cause (***bold italic***) - a trigger that lives at a read "
           "site in sand_reactions.c, not in this table - rendered as the "
           "literal `%s` placeholder rather than guessed at. Bold italic "
           "marks it as a clause rather than a single word, which is also "
           "the rarest thing on this page: every instance today is that "
           "same placeholder.\n", CAUSE);
    printf("- Glue (plain, unmarked) - prose typed by hand inside the "
           "`emit_*()` function itself: connective words, punctuation, "
           "the parts no field drives.\n");

    printf("\n### Examples\n\n");
    printf("One representative sentence per group (see group_id_t), the "
           "subject written out explicitly, in its own colour, and "
           "prefixed to the real per-material clause. Plain text first, "
           "then the same sentence with its slots marked.\n");

    /* GRP_IGNITE - emit_ignite(), self-ignition branch: flammability,
     * ignites_to == self_id, heat_sources. The literal word "fire" is
     * emit_ignite()'s own wording for ignites_to's 0/MAT_FIRE default (see
     * that function's own comment) - not a to_name() call, but it names
     * MAT_FIRE all the same, so it gets MAT_FIRE's colour too, split out
     * of the surrounding glue rather than left plain. */
    {
        const mrow_t *row = find_row("Wood");
        char subject[COLOR_LEN];
        char fire[COLOR_LEN];
        material_hex(row->color_id, subject, sizeof(subject));
        material_hex(MAT_FIRE, fire, sizeof(fire));
        list_item_t heat[LIST_ITEM_MAX];
        const size_t heat_n = collect_material_list(pred_burns, heat,
                                                     LIST_ITEM_MAX);

        seg_t segs[SEG_MAX];
        size_t n = 0;
        seg_material(segs, &n, subject, "Wood");
        seg_glue(segs, &n, ": ");
        seg_mark(segs, &n, MARK_VERB, "Catches");
        seg_glue(segs, &n, " ");
        seg_material(segs, &n, fire, "fire");
        seg_glue(segs, &n, " ");
        seg_mark(segs, &n, MARK_RATE,
                 adverb("flammability", row->r->flammability));
        seg_glue(segs, &n, " from ");
        seg_list(segs, &n, heat, heat_n, " or ");
        seg_glue(segs, &n, ", and burns where it stands, rather than "
                 "flaring away.");
        print_example("Ignite - GRP_IGNITE: flammability, ignites_to, "
                       "heat_sources (emit_ignite)", segs, n);
    }

    /* GRP_BURN - emit_burn(), the `burns` branch: residue, quench_to. */
    {
        const mrow_t *row = find_row("Fire");
        char subject[COLOR_LEN];
        material_hex(row->color_id, subject, sizeof(subject));
        char quench_name[32];
        char quench_color[COLOR_LEN];
        snprintf(quench_name, sizeof(quench_name), "%s",
                 prose_name(to_name(row->r->quench_to)));
        material_hex(row->r->quench_to, quench_color, sizeof(quench_color));

        seg_t segs[SEG_MAX];
        size_t n = 0;
        seg_material(segs, &n, subject, "Fire");
        seg_glue(segs, &n, ": ");
        seg_glue(segs, &n, "Is a heat source in its own right; ");
        seg_mark(segs, &n, MARK_RATE, adverb("residue", row->r->residue));
        seg_glue(segs, &n, " ");
        seg_mark(segs, &n, MARK_VERB, "leaves smoke");
        seg_glue(segs, &n, " when it burns out. Touched by a quenching "
                 "liquid, becomes ");
        seg_material(segs, &n, quench_color, quench_name);
        seg_glue(segs, &n, ".");
        print_example("Burn - GRP_BURN: burns, residue, quench_to "
                       "(emit_burn)", segs, n);
    }

    /* GRP_TRANSFORM - emit_transform(), the rolled (not banked) branch:
     * heats_to, heat_chance, heat_sources. */
    {
        const mrow_t *row = find_row("Sand");
        char subject[COLOR_LEN];
        material_hex(row->color_id, subject, sizeof(subject));
        list_item_t heat[LIST_ITEM_MAX];
        const size_t heat_n = collect_material_list(pred_burns, heat,
                                                     LIST_ITEM_MAX);
        char heats_name[32];
        char heats_color[COLOR_LEN];
        snprintf(heats_name, sizeof(heats_name), "%s",
                 prose_name(to_name(row->r->heats_to)));
        material_hex(row->r->heats_to, heats_color, sizeof(heats_color));

        seg_t segs[SEG_MAX];
        size_t n = 0;
        seg_material(segs, &n, subject, "Sand");
        seg_glue(segs, &n, ": Beside ");
        seg_list(segs, &n, heat, heat_n, " or ");
        seg_glue(segs, &n, ", ");
        seg_mark(segs, &n, MARK_VERB, "melts");
        seg_glue(segs, &n, " to ");
        seg_material(segs, &n, heats_color, heats_name);
        seg_glue(segs, &n, " ");
        seg_mark(segs, &n, MARK_RATE,
                 adverb("heat_chance", row->r->heat_chance));
        seg_glue(segs, &n, ".");
        print_example("Transform - GRP_TRANSFORM: heats_to, heat_chance, "
                       "heat_sources (emit_transform)", segs, n);
    }

    /* GRP_TEMPERATURE - emit_temperature(), the no-ramp branch: conducts
     * alone. */
    {
        const mrow_t *row = find_row("Metal");
        char subject[COLOR_LEN];
        material_hex(row->color_id, subject, sizeof(subject));

        seg_t segs[SEG_MAX];
        size_t n = 0;
        seg_material(segs, &n, subject, "Metal");
        seg_glue(segs, &n, ": ");
        seg_mark(segs, &n, MARK_VERB, "Passes heat on");
        seg_glue(segs, &n, " ");
        seg_mark(segs, &n, MARK_RATE, adverb("conducts", row->r->conducts));
        seg_glue(segs, &n, ", without banking any of it itself.");
        print_example("Temperature - GRP_TEMPERATURE: conducts "
                       "(emit_temperature)", segs, n);
    }

    /* GRP_COLD - emit_cold(): chills. */
    {
        const mrow_t *row = find_row("Ice");
        char subject[COLOR_LEN];
        material_hex(row->color_id, subject, sizeof(subject));

        seg_t segs[SEG_MAX];
        size_t n = 0;
        seg_material(segs, &n, subject, "Ice");
        seg_glue(segs, &n, ": ");
        seg_mark(segs, &n, MARK_VERB, "Chills whatever it touches");
        seg_glue(segs, &n, " ");
        seg_mark(segs, &n, MARK_RATE, adverb("chills", row->r->chills));
        seg_glue(segs, &n, ".");
        print_example("Cold - GRP_COLD: chills (emit_cold)", segs, n);
    }

    /* GRP_WARMTH - emit_warmth(): warms. */
    {
        const mrow_t *row = find_row("Steam");
        char subject[COLOR_LEN];
        material_hex(row->color_id, subject, sizeof(subject));

        seg_t segs[SEG_MAX];
        size_t n = 0;
        seg_material(segs, &n, subject, "Steam");
        seg_glue(segs, &n, ": ");
        seg_mark(segs, &n, MARK_VERB, "Warms whatever it touches");
        seg_glue(segs, &n, " ");
        seg_mark(segs, &n, MARK_RATE, adverb("warms", row->r->warms));
        seg_glue(segs, &n, ", without igniting or quenching anything.");
        print_example("Warmth - GRP_WARMTH: warms (emit_warmth)", segs, n);
    }

    /* GRP_THAW - emit_thaw(), the heats_to != 0 branch: thaws, heats_to. */
    {
        const mrow_t *row = find_row("Snow");
        char subject[COLOR_LEN];
        material_hex(row->color_id, subject, sizeof(subject));
        char heats_name[32];
        char heats_color[COLOR_LEN];
        snprintf(heats_name, sizeof(heats_name), "%s",
                 prose_name(to_name(row->r->heats_to)));
        material_hex(row->r->heats_to, heats_color, sizeof(heats_color));

        seg_t segs[SEG_MAX];
        size_t n = 0;
        seg_material(segs, &n, subject, "Snow");
        seg_glue(segs, &n, ": ");
        seg_mark(segs, &n, MARK_VERB, "Melts in any liquid it touches");
        seg_glue(segs, &n, " ");
        seg_mark(segs, &n, MARK_RATE, adverb("thaws", row->r->thaws));
        seg_glue(segs, &n, ", becoming ");
        seg_material(segs, &n, heats_color, heats_name);
        seg_glue(segs, &n, ".");
        print_example("Thaw - GRP_THAW: thaws, heats_to (emit_thaw)", segs,
                       n);
    }

    /* GRP_WET - emit_wet(), the soaks_to != 0 branch: soaks, soaks_to,
     * and wetting_liquids (derived, not a single field). */
    {
        const mrow_t *row = find_row("Sand");
        char subject[COLOR_LEN];
        material_hex(row->color_id, subject, sizeof(subject));
        list_item_t wet[LIST_ITEM_MAX];
        const size_t wet_n = collect_material_list(pred_wets_liquid, wet,
                                                    LIST_ITEM_MAX);
        char soaks_name[32];
        char soaks_color[COLOR_LEN];
        snprintf(soaks_name, sizeof(soaks_name), "%s",
                 prose_name(to_name(row->r->soaks_to)));
        material_hex(row->r->soaks_to, soaks_color, sizeof(soaks_color));

        seg_t segs[SEG_MAX];
        size_t n = 0;
        seg_material(segs, &n, subject, "Sand");
        seg_glue(segs, &n, ": ");
        seg_mark(segs, &n, MARK_VERB, "Soaks up");
        seg_glue(segs, &n, " any ");
        seg_list(segs, &n, wet, wet_n, " or ");
        seg_glue(segs, &n, " it touches ");
        seg_mark(segs, &n, MARK_RATE, adverb("soaks", row->r->soaks));
        seg_glue(segs, &n, ", becoming ");
        seg_material(segs, &n, soaks_color, soaks_name);
        seg_glue(segs, &n, " once it takes a unit in.");
        print_example("Wet - GRP_WET: soaks, soaks_to, wetting_liquids "
                       "(emit_wet)", segs, n);
    }

    /* GRP_ACID - emit_acid(), the dissolves branch: dissolves (a genuine
     * rate) and fizz (a one-shot chance) side by side - two different
     * ladders, one slot marker. Neither field names a material, so this
     * example has no MARK_MATERIAL segment beyond the subject. */
    {
        const mrow_t *row = find_row("Acid");
        char subject[COLOR_LEN];
        material_hex(row->color_id, subject, sizeof(subject));

        seg_t segs[SEG_MAX];
        size_t n = 0;
        seg_material(segs, &n, subject, "Acid");
        seg_glue(segs, &n, ": ");
        seg_mark(segs, &n, MARK_VERB, "Dissolves an adjacent cell");
        seg_glue(segs, &n, " ");
        seg_mark(segs, &n, MARK_RATE, adverb("dissolves", row->r->dissolves));
        seg_glue(segs, &n, ", ");
        seg_mark(segs, &n, MARK_RATE, adverb("fizz", row->r->fizz));
        seg_glue(segs, &n, " leaving smoke behind.");
        print_example("Acid - GRP_ACID: dissolves, fizz (emit_acid)", segs,
                       n);
    }

    /* GRP_GROW - emit_grow(), the grows branch. */
    {
        const mrow_t *row = find_row("Plant");
        char subject[COLOR_LEN];
        material_hex(row->color_id, subject, sizeof(subject));

        seg_t segs[SEG_MAX];
        size_t n = 0;
        seg_material(segs, &n, subject, "Plant");
        seg_glue(segs, &n, ": ");
        seg_mark(segs, &n, MARK_VERB, "Grows into wet soil");
        seg_glue(segs, &n, " ");
        seg_mark(segs, &n, MARK_RATE, adverb("grows", row->r->grows));
        seg_glue(segs, &n, ", against gravity, spending a level of that "
                 "soil's moisture per cell.");
        print_example("Grow - GRP_GROW: grows (emit_grow)", segs, n);
    }

    /* GRP_HARDEN - emit_harden(): harden_chance (a one-shot chance),
     * hardens_to, holds_line (another one-shot chance), clings_to.
     *
     * Two separate FK_TARGET fields in one sentence, so prose_name()'s
     * single static buffer cannot hold both at once - each result is
     * copied into its own local buffer immediately, before the second
     * call would overwrite the first (Plant's hardens_to and clings_to
     * happen to both be Wood today, which would have hidden exactly that
     * bug had this just called prose_name() twice inline). material_hex()
     * needs the same discipline, for the same reason - see its own
     * comment. */
    {
        const mrow_t *row = find_row("Plant");
        char subject[COLOR_LEN];
        material_hex(row->color_id, subject, sizeof(subject));
        char hardens_to[32];
        char hardens_color[COLOR_LEN];
        char clings_to[32];
        char clings_color[COLOR_LEN];
        snprintf(hardens_to, sizeof(hardens_to), "%s",
                 prose_name(to_name(row->r->hardens_to)));
        material_hex(row->r->hardens_to, hardens_color, sizeof(hardens_color));
        snprintf(clings_to, sizeof(clings_to), "%s",
                 prose_name(to_name(row->r->clings_to)));
        material_hex(row->r->clings_to, clings_color, sizeof(clings_color));

        seg_t segs[SEG_MAX];
        size_t n = 0;
        seg_material(segs, &n, subject, "Plant");
        seg_glue(segs, &n, ": A straight run of 6 cells ");
        seg_mark(segs, &n, MARK_RATE,
                 adverb("harden_chance", row->r->harden_chance));
        seg_glue(segs, &n, " ");
        seg_mark(segs, &n, MARK_VERB, "hardens");
        seg_glue(segs, &n, " into ");
        seg_material(segs, &n, hardens_color, hardens_to);
        seg_glue(segs, &n, ", up to 2 cells wider at the foot than at the "
                 "tip, and ");
        seg_mark(segs, &n, MARK_RATE,
                 adverb("holds_line", row->r->holds_line));
        seg_glue(segs, &n, " a limb holds its own direction (rather than "
                 "bending back toward gravity); the hardened body counts "
                 "as part of ");
        seg_material(segs, &n, clings_color, clings_to);
        seg_glue(segs, &n, ".");
        print_example("Harden - GRP_HARDEN: harden_chance, hardens_to, "
                       "holds_line, clings_to (emit_harden)", segs, n);
    }

    /* GRP_REGROW - emit_regrow(), the sprouts branch: sprouts, sprouts_to. */
    {
        const mrow_t *row = find_row("Wood");
        char subject[COLOR_LEN];
        material_hex(row->color_id, subject, sizeof(subject));
        char sprouts_name[32];
        char sprouts_color[COLOR_LEN];
        snprintf(sprouts_name, sizeof(sprouts_name), "%s",
                 prose_name(to_name(row->r->sprouts_to)));
        material_hex(row->r->sprouts_to, sprouts_color, sizeof(sprouts_color));

        seg_t segs[SEG_MAX];
        size_t n = 0;
        seg_material(segs, &n, subject, "Wood");
        seg_glue(segs, &n, ": Standing in wet soil, ");
        seg_mark(segs, &n, MARK_VERB, "sprouts");
        seg_glue(segs, &n, " ");
        seg_material(segs, &n, sprouts_color, sprouts_name);
        seg_glue(segs, &n, " beside itself ");
        seg_mark(segs, &n, MARK_RATE, adverb("sprouts", row->r->sprouts));
        seg_glue(segs, &n, ".");
        print_example("Regrow - GRP_REGROW: sprouts, sprouts_to "
                       "(emit_regrow)", segs, n);
    }

    /* GRP_SHATTER - emit_shatter(): shatters_to, and CAUSE - the one
     * per-material clause that ever prints it. */
    {
        const mrow_t *row = find_row("Glass");
        char subject[COLOR_LEN];
        material_hex(row->color_id, subject, sizeof(subject));
        char shatters_name[32];
        char shatters_color[COLOR_LEN];
        snprintf(shatters_name, sizeof(shatters_name), "%s",
                 prose_name(to_name(row->r->shatters_to)));
        material_hex(row->r->shatters_to, shatters_color,
                     sizeof(shatters_color));

        seg_t segs[SEG_MAX];
        size_t n = 0;
        seg_material(segs, &n, subject, "Glass");
        seg_glue(segs, &n, ": ");
        seg_mark(segs, &n, MARK_VERB, "Shatters");
        seg_glue(segs, &n, " into ");
        seg_material(segs, &n, shatters_color, shatters_name);
        seg_glue(segs, &n, " ");
        seg_mark(segs, &n, MARK_CAUSE, CAUSE);
        seg_glue(segs, &n, ".");
        print_example("Shatter - GRP_SHATTER: shatters_to (emit_shatter)",
                       segs, n);
    }
}

/*-----------------------------------------------------------------------
 * main
 *---------------------------------------------------------------------*/

int main(void)
{
#ifdef _WIN32
    /* MinGW's CRT defaults stdout to text mode, which rewrites every '\n'
     * this file prints into "\r\n" - invisible on Windows, but it makes
     * report_reactions.sh --check compare a CRLF TMP_MD against docs/Sand/
     * Reaction-Table.md's LF (.gitattributes forces every .md to eol=lf -
     * see that file's own comment on why - so the committed doc is LF
     * regardless of which platform generated it). Binary mode turns off
     * the rewrite, so this prints the same bytes on every platform the
     * same way find_cc.sh already picks a compiler on every platform. */
    _setmode(_fileno(stdout), _O_BINARY);
#endif
    field_docs_offsets_are_sound();
    build_rows();
    join_names(pred_wets_liquid, " or ", wetting_liquids,
              sizeof(wetting_liquids));
    /* wetting_liquids is only ever read in prose (emit_wet(), emit_regrow())
     * - never a table cell or heading - so, unlike to_name()'s results, it
     * can be lowercased once, here, rather than at each of its three call
     * sites. */
    str_lower(wetting_liquids);
    /* Same deal for heat_sources - only ever read in prose (emit_transform(),
     * emit_ignite()), so it is lowercased once here rather than at each call
     * site. pred_burns is already defined above (emit_pairwise_table() uses
     * it too, for the Title-Case "Fire / Lava" pairwise column). */
    join_names(pred_burns, " or ", heat_sources, sizeof(heat_sources));
    str_lower(heat_sources);

    printf("# Reaction Table\n\n");
    printf("<!-- GENERATED by launcher/main/apps/sand/tools/dump_reactions.c "
           "via launcher/main/apps/sand/tools/report_reactions.sh - do not "
           "edit by hand. Edit the generator, or the tables in material.c "
           "it reads, and regenerate. -->\n\n");
    printf("Phase 1 output (see docs/Sand/Reaction-Doc-Generator-Plan.md): "
           "every adverb below is the rate ladder's computed bucket, "
           "unedited - some are already known to read wrong by feel "
           "(sand -> glass, the ignition family - see the plan's own "
           "\"The ladder lies in two directions\" section) and are queued "
           "for a by-feel pass in phase 2, not fixed here. Every "
           "`" CAUSE "` marks a trigger that lives at a read site in "
           "sand_reactions.c rather than in the table itself, and is not "
           "guessed at.\n");

    printf("\n## Per-material\n");
    for (size_t i = 0; i < all_rows_count; i++) {
        emit_material_section(all_rows[i].name, all_rows[i].r,
                              all_rows[i].self_id);
    }

    emit_pairwise_table();
    emit_anatomy();

    return 0;
}
