/*=============================================================================
 * dump_reactions - compile material.c's reaction tables into markdown.
 *
 * See docs/Sand/Reaction-Doc-Generator-Plan.md for the design this follows.
 * This file started as that plan's phase 1 (raw ladder output, no by-feel
 * tuning) and now carries phase 2's by-feel pass as well: the rate ladder's
 * silent middle, the chance ladders' 0/255 endpoints, the one checked
 * ADVERB_EXCEPTIONS override, contrast-legible colour via
 * LEGIBILITY_OVERRIDES, and colour/typography reaching the default
 * per-material section rather than only the anatomy examples. Run through
 * report_reactions.sh, which builds this, captures its stdout, and splices
 * it into the BEGIN/END GENERATED region of docs/Sand/Reaction-Table.md
 * (see this file's own main(), and report_reactions.sh's top comment, for
 * why it is a splice and not a whole-file overwrite: some real mechanics -
 * lava's cool-off chaining, the covered-lava burst, water/acid's faster
 * drain on stone and glass - live entirely at a read site in
 * sand_reactions.c with no reaction_t field to walk, so a human documents
 * them by hand outside the markers, and a whole-file overwrite would
 * silently delete that documentation on every regenerate - see bd
 * esp32c6-3mu).
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
 * WHAT STILL DELIBERATELY DOES NOT HAPPEN
 *
 * No hand-written PER-REACTION prose or guessed triggers: every rate/
 * frequency word is still the ladder's computed bucket (bar the one
 * checked exception), and every field whose real trigger is a condition
 * living at a read site in sand_reactions.c (not in the table) prints
 * exactly what that file's own REACTION_DOC(field, "why") annotation says
 * at the point that decides it - see reaction_doc.h and this file's
 * parse_reaction_docs()/cause_at() - never a clause guessed from the field
 * name alone. A field whose trigger lives at a read site but has not yet
 * been annotated that way is still future work, tracked in the plan's own
 * "Phasing" section, not something this pass fakes.
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
#include <errno.h>
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
    GRP_CONDENSE,     /* a 2x2 block collapsing into one cell: condenses,
                       * condenses_to */
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

/* The two SCALE_CHANCE vocabularies. Both index off the same four slots
 * (chance_bucket_for(), in the Decoding section below) - only the WORDS
 * differ, because a one-shot chance/256 can answer two different
 * questions depending on what it is conditioned on.
 *
 * Four slots, not five: 0 and 255 are handled directly in adverb() as the
 * categorical absolutes they are (see adverb_for()'s own top comment for
 * why 0/255 are not extremes of a scale) - "never" for 0, unconditionally,
 * on both vocabularies; the OTHER absolute (255, "costs no draw at all")
 * DOES still take a vocabulary word here, because unlike the rate ladder
 * there is no shared word that reads right for both "how often" and "how
 * well" ("always" fits a frequency, "outright" fits an ease/resistance
 * question, and neither fits the other) - so slot 0 of each array below is
 * that word, and slots 1-3 are the three ordinary buckets, high to low.
 *
 * Silence is NOT an option here, unlike the rate ladder's middle band -
 * see adverb_for()'s own comment on why RATE can stay quiet for the
 * common case but CHANCE cannot: a reader who sees no word at all for
 * `residue` would read that as "always leaves smoke", which is false at
 * (say) 90/256. So every nonzero, non-255 value here always prints one of
 * the three in-between words - never nothing.
 *
 * frequency_words[] is "how often does this happen" - the right question
 * for residue, fizz, harden_chance, canopy and holds_line, each a fresh
 * roll at its own moment ("mostly leaves smoke when it burns out").
 *
 * ease_words[] is "how well does this go, given it is already happening" -
 * the right question for `dissolvable` alone. Its own comment in
 * material.h is explicit that it is "the chance an ATTEMPT to dissolve
 * succeeds", i.e. conditional on an attempt already under way, not a
 * frequency in its own right - "Mostly gives way to acid" answers a
 * question nobody asked, and fronts the adverb besides. Same buckets,
 * different vocabulary - see field_doc_t.chance_vocab. Deliberately no
 * duration word anywhere in this array (a prior version used "almost
 * instantly" / "slowly" here, smuggling a time-to-wait word into a
 * chance-of-success question - the same category error the rate/frequency
 * split exists to prevent, just missed on this one field). */
static const char *const frequency_words[] = {
    "always", "mostly", "occasionally", "seldom",
};
static const char *const ease_words[] = {
    "outright", "handily", "adequately", "poorly",
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

    /* GRP_CONDENSE - the inverse of evaporation: a 2x2 block of one
     * material collapsing into a single cell of another. Genuine
     * per-step rate, gated on a 2x2 neighbourhood match rather than a
     * partner or a prior roll - the same shape as `dissolves`, not a
     * one-shot FCHANCE. */
    FRATE(condenses,    GRP_CONDENSE, "condenses"),
    F(condenses_to,     GRP_CONDENSE, FK_TARGET, NULL),

    /* GRP_TRANSFORM. `flaw_chance` is a one-shot chance conditioned on
     * heat_chance's roll already having succeeded, the same shape as
     * `dissolvable` - not a rate of its own, so FCHANCE rather than
     * FRATE. `spoils_to`/`spoils_chance` fire from a condition
     * (heat_chance succeeding on a WET cell) that lives entirely at the
     * read site in sand_reactions.c, not in this table - see this file's
     * REACTION_DOC/cause_at() machinery, and sand_reactions.c's own
     * REACTION_DOC(spoils_to, ...) call, for where emit_spoils() gets its
     * clause from rather than guessing it. */
    F(heats_to,     GRP_TRANSFORM, FK_TARGET, NULL),
    FRATE(heat_chance, GRP_TRANSFORM, "melts"),
    /* `melts` shares heats_to with heat_chance but answers to LAVA
     * alone - direct contact with a burning liquid, never a flame and
     * never heat through a conductor. See reaction_t.melts for why a
     * material with no variant has to draw that line at the source. */
    FRATE(melts,       GRP_TRANSFORM, "melts under lava"),
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
    FRATE(boils,    GRP_TEMPERATURE, "boils"),
    F(boils_to,     GRP_TEMPERATURE, FK_TARGET, NULL),
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

    /* `roots` is not rolled independently every step the way `sprouts`
     * and `buds` above are - it only rolls once GROWING, BUDDING or
     * SPROUTING has already spent a level of soil moisture this same
     * step (spend_soil_moisture(), sand_reactions.c), so it is a
     * one-shot chance conditioned on that spend, the same shape as
     * `residue` or `flaw_chance` - FCHANCE, not FRATE. */
    FCHANCE(roots,  GRP_REGROW, "roots into the soil it drinks from"),
    F(roots_to,     GRP_REGROW, FK_TARGET, NULL),

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
 * REACTION_DOC parsing - the cause clauses this file cannot derive from
 * material.c's tables at all, because the condition that gates them lives
 * entirely at a read site in sand_reactions.c (see shatters_to's two
 * SAND_SHOCK_HEAT/SAND_SHOCK_COLD thresholds, and spoils_to's "only while
 * the cell is still wet" gate - neither appears anywhere in reactions[]/
 * extended_reactions[]).
 *
 * sand_reactions.c is read as TEXT here, never linked - see reaction_doc.h's
 * own top comment for why that is not a reversal of "compile the tables, do
 * not parse them" (that rule is about constant EXPRESSIONS needing the
 * preprocessor; a string literal has nothing to evaluate, only to read back
 * verbatim), and report_reactions.sh for how its path reaches this program.
 *
 * is_known_field() below deliberately duplicates field_doc()'s own lookup
 * rather than calling it, because a bad field name here is not fatal in the
 * way field_doc() assumes - it just refers to a REACTION_DOC() invocation
 * (reported by number, `errno`/exit(1) same as the rest of this file) rather
 * than a `%s` this program already trusts elsewhere.
 *---------------------------------------------------------------------*/

/* Comfortably more than the number of REACTION_DOC() calls sand_reactions.c
 * carries today (three, as of this writing) - raise it if a future one
 * trips the check in parse_reaction_docs(). */
#define CAUSE_MAX        32
#define CAUSE_FIELD_LEN  32
#define CAUSE_TEXT_LEN   160

typedef struct {
    char field[CAUSE_FIELD_LEN];
    char text[CAUSE_TEXT_LEN];
} cause_t;

static cause_t causes[CAUSE_MAX];
static size_t causes_count;

/* Every field name this file actually pulls a cause_at() clause out of -
 * NOT every field with a trigger at a read site (hardens_to's own
 * REACTION_DOC() in sand_reactions.c documents one, at its lignify branch
 * in step_one_withering_cell(), but nothing in this file prints it, so it
 * is deliberately absent here; adding a printed clause for it later means
 * adding it to this list too). A trailing `has_cause` column on field_docs[]
 * rows would say the same thing but forces every F()/FRATE()/FCHANCE() row
 * in the whole table to grow a new argument for the sake of the two fields
 * that need one - this small separate list costs far less churn for the
 * same guarantee, checked by causes_are_complete() below exactly like a
 * column would be. */
static const char *const causes_expected[] = {
    "shatters_to",
    "spoils_to",
};

static bool is_known_field(const char *name)
{
    for (size_t i = 0; i < ARRAY_LEN(field_docs); i++) {
        if (strcmp(field_docs[i].name, name) == 0) {
            return true;
        }
    }
    return false;
}

static char *read_whole_file(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        fprintf(stderr, "dump_reactions: cannot open %s: %s\n", path,
                strerror(errno));
        exit(1);
    }
    if (fseek(f, 0, SEEK_END) != 0) {
        fprintf(stderr, "dump_reactions: cannot seek %s\n", path);
        exit(1);
    }
    const long size = ftell(f);
    if (size < 0 || fseek(f, 0, SEEK_SET) != 0) {
        fprintf(stderr, "dump_reactions: cannot size %s\n", path);
        exit(1);
    }
    char *buf = malloc((size_t)size + 1);
    if (buf == NULL) {
        fprintf(stderr, "dump_reactions: out of memory reading %s (%ld "
                "bytes)\n", path, size);
        exit(1);
    }
    const size_t got = fread(buf, 1, (size_t)size, f);
    fclose(f);
    buf[got] = '\0';
    return buf;
}

/* Scans `src` (sand_reactions.c's own text, from read_whole_file()) for
 * every `REACTION_DOC(field, "literal")` invocation and records it in
 * causes[]. Deliberately strict rather than forgiving - a REACTION_DOC()
 * call this cannot parse the way reaction_doc.h documents it should fail
 * the doc build loudly, not silently drop the clause it was meant to
 * supply. */
static void parse_reaction_docs(const char *path, const char *src)
{
    const char *p = src;
    int invocation_no = 0;
    while ((p = strstr(p, "REACTION_DOC(")) != NULL) {
        invocation_no++;
        p += strlen("REACTION_DOC(");
        while (isspace((unsigned char)*p)) p++;
        const char *field_start = p;
        while (isalnum((unsigned char)*p) || *p == '_') p++;
        const size_t field_len = (size_t)(p - field_start);
        if (field_len == 0 || field_len >= CAUSE_FIELD_LEN) {
            fprintf(stderr, "%s: REACTION_DOC #%d has no plain field name\n",
                    path, invocation_no);
            exit(1);
        }
        char field[CAUSE_FIELD_LEN];
        memcpy(field, field_start, field_len);
        field[field_len] = '\0';
        while (isspace((unsigned char)*p)) p++;
        if (*p != ',') {
            fprintf(stderr, "%s: REACTION_DOC(%s, ...) #%d - expected ',' "
                    "after the field name\n", path, field, invocation_no);
            exit(1);
        }
        p++;
        while (isspace((unsigned char)*p)) p++;
        if (*p != '"') {
            fprintf(stderr, "%s: REACTION_DOC(%s, ...) #%d - the second "
                    "argument must be a plain string literal, not an "
                    "expression\n", path, field, invocation_no);
            exit(1);
        }
        p++;
        char text[CAUSE_TEXT_LEN];
        size_t tlen = 0;
        while (*p != '"') {
            if (*p == '\0' || *p == '\n') {
                fprintf(stderr, "%s: REACTION_DOC(%s, ...) #%d - "
                        "unterminated string literal\n", path, field,
                        invocation_no);
                exit(1);
            }
            if (*p == '\\' && p[1] != '\0') p++;
            if (tlen + 1 >= CAUSE_TEXT_LEN) {
                fprintf(stderr, "%s: REACTION_DOC(%s, ...) #%d - clause "
                        "text longer than %d bytes\n", path, field,
                        invocation_no, CAUSE_TEXT_LEN - 1);
                exit(1);
            }
            text[tlen++] = *p++;
        }
        p++;
        text[tlen] = '\0';
        while (isspace((unsigned char)*p)) p++;
        if (*p != ')') {
            fprintf(stderr, "%s: REACTION_DOC(%s, ...) #%d - expected ')' "
                    "right after the string literal (adjacent-literal "
                    "concatenation is not supported here - write the "
                    "clause as one literal)\n", path, field, invocation_no);
            exit(1);
        }
        if (!is_known_field(field)) {
            fprintf(stderr, "%s: REACTION_DOC(%s, ...) #%d - \"%s\" is not "
                    "a field in field_docs[] (dump_reactions.c) - a typo, "
                    "or field_docs[] needs a row for it\n", path, field,
                    invocation_no, field);
            exit(1);
        }
        if (causes_count >= CAUSE_MAX) {
            fprintf(stderr, "%s: more than %d REACTION_DOC() invocations - "
                    "raise CAUSE_MAX in dump_reactions.c\n", path,
                    CAUSE_MAX);
            exit(1);
        }
        snprintf(causes[causes_count].field, CAUSE_FIELD_LEN, "%s", field);
        snprintf(causes[causes_count].text, CAUSE_TEXT_LEN, "%s", text);
        causes_count++;
    }
}

static size_t cause_count(const char *field)
{
    size_t n = 0;
    for (size_t i = 0; i < causes_count; i++) {
        if (strcmp(causes[i].field, field) == 0) n++;
    }
    return n;
}

/* The `index`-th REACTION_DOC() clause for `field`, in the order those
 * calls appear in sand_reactions.c - callers rely on that source order to
 * tell two clauses for the same field apart (see emit_shatter()'s and
 * emit_pairwise_table()'s own comments on which index means which of
 * shatters_to's two thresholds). */
static const char *cause_at(const char *field, size_t index)
{
    size_t seen = 0;
    for (size_t i = 0; i < causes_count; i++) {
        if (strcmp(causes[i].field, field) != 0) continue;
        if (seen == index) return causes[i].text;
        seen++;
    }
    fprintf(stderr, "dump_reactions: cause_at(\"%s\", %zu) - fewer than "
            "%zu clause(s) were found for this field\n", field, index,
            index + 1);
    exit(1);
}

static void causes_are_complete(void)
{
    bool ok = true;
    for (size_t i = 0; i < ARRAY_LEN(causes_expected); i++) {
        if (cause_count(causes_expected[i]) == 0) {
            fprintf(stderr, "dump_reactions: field \"%s\" is expected to "
                    "have a REACTION_DOC(...) in sand_reactions.c, but none "
                    "was found\n", causes_expected[i]);
            ok = false;
        }
    }
    if (!ok) exit(1);
}

/*-----------------------------------------------------------------------
 * Decoding.
 *---------------------------------------------------------------------*/

/* The rate ladder - for FK_RATE fields with scale == SCALE_RATE: a chance
 * rolled every step against a steady partner, which has a genuine expected
 * TIME to wait (SIM_HZ 60 - see app_sand.c - so one step is ~16.7ms, and
 * the expected wait against one steady partner is 256/value steps).
 *
 * 0 AND 255 ARE NOT ENDS OF A SCALE, THEY ARE A DIFFERENT CATEGORY
 *
 * 0 means the reaction cannot happen - never, full stop, not "extremely
 * slowly". 255 means it happens on contact and costs NO random draw at
 * all: see flammability's own comment in material.h - a material at 255
 * leaves the RNG stream exactly as it was before the field existed,
 * because try_ignite() (sand_reactions.c) short-circuits before rolling.
 * Both are therefore handled as flat, unconditional words below, not as
 * the top/bottom rungs of the ladder that follows.
 *
 * THE MIDDLE IS SILENT ON PURPOSE
 *
 * Everything from 1 to 254 used to print a word (a six-band ladder:
 * instantly/swiftly/fast/readily/steadily/slowly). Measured against this
 * table's actual values, one band - "fast" and up - covered the vast
 * majority of them: "quickly"-class words printed on the clear majority of
 * every adverb this file emitted, an ordinary-case word that told a reader
 * nothing they could not already assume. The fix is to stop printing a
 * word for the ordinary case and let SILENCE mean "nothing unusual here" -
 * readable because a RATE lives in a sentence ("catches fire from fire or
 * lava") where dropping the adverb entirely still reads as a complete,
 * true claim. Only the genuinely slow band still speaks up:
 *
 *   v == 0            never    (cannot happen - see above)
 *   v in 1..5         slowly   (>= ~853ms against one steady partner)
 *   v in 6..254       (nothing - ordinary speed, said by staying quiet)
 *   v == 255          instantly (no draw at all - see above)
 *
 * The cutoff sits at 5, verified against this table's own data rather than
 * assumed: wood's `flammability` is 6, one step above the cutoff, and
 * correctly falls silent - a flame front touches several of a log's faces
 * at once, so the single-steady-partner model this ladder is built on
 * already understates wood's real ignition speed, and silence (ordinary)
 * reads closer to true than a printed "slowly" would. Glass's `cools` (5)
 * and dirt's `dries` (2, moved down from an earlier 5 - see MAT_DIRT's own
 * comment - so both sit at or under the cutoff either way) both land in
 * "slowly", correctly: draining heat back to ambient and drying out are
 * both meant to read as slow, ongoing processes, not something that just
 * happens.
 *
 * ONE MEASURED EXCEPTION: sand's `heat_chance` (16) computes to SILENT
 * under this cutoff, but MAT_SAND's own comment in material.c measures the
 * real behaviour as "deliberately slow... something you set up and wait
 * for" - a bed of eleven cells under a held flame takes 137 steps (~2.3s)
 * to fully convert, because the model above assumes one steady partner and
 * heat_chance is actually rolled per adjacent heat source, so an interior
 * cell with no direct exposure waits on its neighbours first. That gap
 * between the model and the measured comment is real, so it is handled as
 * a checked ADVERB_EXCEPTIONS entry below, not by moving this cutoff to
 * paper over it. */
#define RATE_SLOW_CUTOFF 5

static const char *adverb_for(uint8_t v)
{
    if (v == 0)   return "never";
    if (v == 255) return "instantly";
    if (v <= RATE_SLOW_CUTOFF) return "slowly";
    return ""; /* silent middle - see this function's own top comment */
}

/* The chance bucket - for FK_RATE fields with scale == SCALE_CHANCE: a
 * ONE-SHOT roll at a single moment (a burn winking out, an acid bite
 * landing, a hardening run deciding whether it takes) rather than a rate
 * against a steady partner. There is no time axis to convert to a speed
 * word for these - see field_scale_t's own comment - so this reads the
 * same raw value as a plain percentage of 256 instead.
 *
 * Never called with v == 0 or v == 255 - adverb() below handles both
 * directly (0 as the universal "never", 255 through the vocabulary's own
 * slot 0 - see frequency_words[]/ease_words[]'s own comment for why 255
 * needs a vocabulary word here where the rate ladder just above did not).
 * So this only ever has to split 1..254 into three ordinary bands: */
static int chance_bucket_for(uint8_t v)
{
    if (v >= 150) return 1;
    if (v >= 50)  return 2;
    return 3; /* 1..49 */
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
        /* 0 is the same flat "never" on every vocabulary - see this
         * section's own top comment on why 0/255 are handled once, here,
         * rather than duplicated into every vocabulary array. */
        if (v == 0) return "never";
        const char *const *vocab = (fd->chance_vocab != NULL)
                                        ? fd->chance_vocab : frequency_words;
        return (v == 255) ? vocab[0] : vocab[chance_bucket_for(v)];
    }
    return adverb_for(v);
}

/*-----------------------------------------------------------------------
 * Adverb exceptions - a by-feel override for the one case where this
 * ladder's single-steady-partner model provably disagrees with a measured
 * comment in material.c (see adverb_for()'s own top comment on sand's
 * heat_chance). A tiny table with a startup soundness check, the same
 * idiom field_docs_offsets_are_sound() already uses: wrong data here is
 * worse than no override at all, so it is checked, not just declared.
 *---------------------------------------------------------------------*/

typedef struct {
    uint8_t     cell;   /* the row this override applies to - a plain
                        * material id, or a MATX() spec for an extended
                        * one; matched against mrow_t.color_id */
    const char *field;  /* must name a real field_docs[] row */
    const char *adverb; /* the word to print instead of the ladder's own */
    const char *why;    /* the measurement/evidence for overriding the
                        * ladder - required, never empty, and expected to
                        * quote or paraphrase the material.c comment that
                        * justifies it */
} adverb_exception_t;

static const adverb_exception_t ADVERB_EXCEPTIONS[] = {
    { MAT_SAND, "heat_chance", "slowly",
      "MAT_SAND.heat_chance = 16 falls silent under the ladder above (it "
      "is above RATE_SLOW_CUTOFF), but the field's own comment in "
      "material.c measures a bed of eleven sand cells under a held flame "
      "taking 137 steps (~2.3s) to fully convert, and calls that "
      "\"deliberately slow - glass should be something you set up and "
      "wait for, not something that happens whenever a spark lands on a "
      "dune\". The gap is the model, not the data: heat_chance rolls once "
      "PER ADJACENT HEAT SOURCE, so an interior cell with only one hot "
      "neighbour waits on the cells ahead of it first - the single-"
      "steady-partner ladder cannot see that queuing effect." },
};

/* Looks up an override for (field_name, cell) - the only two things an
 * emit_*() clause has on hand at the point it would otherwise call
 * adverb() directly. Returns NULL when no override applies, which is the
 * overwhelming common case (one entry in the table above, as of this
 * writing). */
static const char *adverb_exception_for(const char *field_name, uint8_t cell)
{
    for (size_t i = 0; i < ARRAY_LEN(ADVERB_EXCEPTIONS); i++) {
        if (ADVERB_EXCEPTIONS[i].cell == cell &&
            strcmp(ADVERB_EXCEPTIONS[i].field, field_name) == 0) {
            return ADVERB_EXCEPTIONS[i].adverb;
        }
    }
    return NULL;
}

/* adverb(), but for a clause that also knows which material row it is
 * printing - checks ADVERB_EXCEPTIONS first, falls back to the plain
 * ladder otherwise. Every emit_*() clause capable of carrying an override
 * should call this instead of adverb() directly; today that is only
 * emit_transform()'s heat_chance clause. */
static const char *adverb_cell(const char *field_name, uint8_t v, uint8_t cell)
{
    const char *ov = adverb_exception_for(field_name, cell);
    return (ov != NULL) ? ov : adverb(field_name, v);
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

/* Wraps a cause_at() clause in the same ***bold italic*** typography
 * emit_anatomy() gives it via MARK_CAUSE (see mark_t's own comment) -
 * applied directly here for every DEFAULT-section clause that prints one
 * (emit_spoils(), emit_shatter()), since neither of those goes through the
 * seg_t/MARK_CAUSE machinery emit_anatomy()'s own examples use. Returns a
 * pointer into a static buffer, same convention as prose_name() just above -
 * safe under the same "never called twice in one statement" rule (grep
 * cause_marked( to confirm before adding a call that would). */
static const char *cause_marked(const char *field, size_t index)
{
    static char buf[CAUSE_TEXT_LEN + 8];
    snprintf(buf, sizeof(buf), "***%s***", cause_at(field, index));
    return buf;
}

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

/* "#rrggbb" plus the NUL, the fixed width every material_hex() output (and
 * every buffer meant to hold one) needs. */
#define COLOR_LEN 8

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

    /* The plain case: a shade, centred on the band's own middle. Sand
     * stops short of the top four shades (SAND_CULLET_BASE in material.h)
     * so a painted dune can never read as cullet; a drying material stops
     * short of its own wet range the same way (SOIL_DRY_TONES) so a
     * painted cell can never read as moisture - MATERIAL_SHADE_SPAN()
     * already encodes both for every material this table has, and a
     * freshly poured cell of a drying material IS just a dry tone, banded
     * exactly like a shade (material.h's own comment on soil's state
     * split; random_cell() in sand.c is where the real jitter this only
     * centres on happens), so there is no separate case left to write
     * here the way the old fixed-bit-split encoding needed. */
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

/*-----------------------------------------------------------------------
 * Legibility overrides - material_hex() above returns the device's exact
 * palette value, which is what the anatomy section below still shows (see
 * its own top comment: raw values are the documentation of the actual
 * palette). The DEFAULT per-material section and Legend read colour
 * through legible_hex() instead: several of those exact values fail a 3:1
 * WCAG contrast floor against GitHub's light (#FFFFFF) and dark (#0D1117)
 * page backgrounds - snow and steam wash out on light, oil and lava wash
 * out on dark, and so on - so a name coloured with the raw value can be
 * unreadable in whichever theme the reader is in.
 *
 * Same idiom as ADVERB_EXCEPTIONS above: a small table of RESULTS, not a
 * live HSL bisection in C (that was done once, offline, in Python, to get
 * exact numbers - see the table's own comment below), with a startup
 * check that recomputes the raw value LIVE through material_hex() and
 * exits(1) on any mismatch, so a future palette change cannot silently
 * leave a stale override in place the way it could with no check at all.
 * Only materials that actually fail get a row - passing materials read
 * straight through material_hex(), unmodified, forever. */
typedef struct {
    uint8_t     cell;    /* matched against mrow_t.color_id, same as
                          * ADVERB_EXCEPTIONS.cell */
    const char *raw;     /* material_hex()'s output for `cell` at the time
                          * this row was computed - the soundness check
                          * recomputes it live and compares */
    const char *legible; /* same hue/saturation, lightness bisected against
                          * both #FFFFFF and #0D1117 until each clears
                          * 3:1, moving the least distance needed (lift if
                          * too dark, darken if too pale) */
} legibility_override_t;

/* Computed 2026-09-05 against this branch's own material.c (see this
 * file's own git history if these ever need recomputing: hex_to_rgb ->
 * sRGB-linearise -> WCAG relative luminance -> contrast ratio against
 * both backgrounds -> bisect HLS lightness, hue/saturation held fixed,
 * until both ratios clear 3.0 with a small margin to survive hex
 * rounding). Every material NOT listed here already clears 3:1 on both
 * backgrounds at its raw value - see legibility_overrides_are_sound()
 * below and this section's own Legend note for the passing list. */
static const legibility_override_t LEGIBILITY_OVERRIDES[] = {
    { MAT_SAND,             "#D6A663", "#C58834" },
    { MAT_WATER,            "#10416B", "#1863A3" },
    { MAT_GAS,              "#CEEBBD", "#5CA532" },
    { MAT_FIRE,             "#FFE363", "#B19100" },
    { MAT_WOOD,             "#5A3D21", "#825830" },
    { MAT_STEAM,            "#F7FBFF", "#2D96FF" },
    { MAT_OIL,              "#101008", "#636331" },
    { MAT_LAVA,             "#8C1400", "#BF1B00" },
    { MAT_ACID,             "#296908", "#2B6F08" },
    { MAT_SNOW,             "#E6EFF7", "#6099CC" },
    { MATX(MATX_ICE),       "#B5E7F7", "#16A0CC" },
    { MATX(MATX_PLANT),     "#526529", "#54682A" },
    { MATX(MATX_LEAF),      "#6BB23A", "#63A435" },
    { MATX(MATX_ROOT),      "#BDA68C", "#AC8F6F" },
};

/* legible_hex()'s search below - it is a linear scan of a table with
 * fifteen rows, not a lookup this program runs often enough to warrant
 * anything smarter. */
static const legibility_override_t *legibility_override_for(uint8_t cell)
{
    for (size_t i = 0; i < ARRAY_LEN(LEGIBILITY_OVERRIDES); i++) {
        if (LEGIBILITY_OVERRIDES[i].cell == cell) {
            return &LEGIBILITY_OVERRIDES[i];
        }
    }
    return NULL;
}

/* material_hex(), but reading through LEGIBILITY_OVERRIDES first - this is
 * what every DEFAULT per-material clause and the Legend colour through;
 * emit_anatomy() calls material_hex() directly instead, on purpose (see
 * this section's own top comment). */
static void legible_hex(uint8_t v, char *buf, size_t cap)
{
    const legibility_override_t *ov = legibility_override_for(v);
    if (ov != NULL) {
        snprintf(buf, cap, "%s", ov->legible);
        return;
    }
    material_hex(v, buf, cap);
}

/* Startup check, run once after build_rows() - mirrors
 * adverb_exceptions_are_sound()'s shape exactly: every `cell` must be a
 * real row, and the stored `raw` value must still match what
 * material_hex() computes for it live, today. A mismatch means the
 * palette moved since this table was computed and the `legible` column
 * needs recomputing against the new raw value, not silently kept. */
static void legibility_overrides_are_sound(void)
{
    for (size_t i = 0; i < ARRAY_LEN(LEGIBILITY_OVERRIDES); i++) {
        const legibility_override_t *ov = &LEGIBILITY_OVERRIDES[i];
        const mrow_t *row = NULL;
        for (size_t j = 0; j < all_rows_count; j++) {
            if (all_rows[j].color_id == ov->cell) {
                row = &all_rows[j];
                break;
            }
        }
        if (row == NULL) {
            fprintf(stderr,
                    "dump_reactions: LEGIBILITY_OVERRIDES[%zu] names cell "
                    "%u, which is not any row build_rows() produced\n",
                    i, (unsigned)ov->cell);
            exit(1);
        }
        char live[COLOR_LEN];
        material_hex(ov->cell, live, sizeof(live));
        if (strcmp(live, ov->raw) != 0) {
            fprintf(stderr,
                    "dump_reactions: LEGIBILITY_OVERRIDES[%zu] (%s) was "
                    "computed against raw value %s, but material_hex() "
                    "gives %s today - the palette moved; recompute this "
                    "row's `legible` column against the new raw value\n",
                    i, row->name, ov->raw, live);
            exit(1);
        }
    }
}

/* One material-name segment, coloured through legible_hex() and rendered
 * as the same single-dollar LaTeX span emit_anatomy() uses (see that
 * section's own top comment on why: inline, upright, one colour per
 * instance). Returns a pointer into a small ring of static buffers rather
 * than one shared buffer - contrast prose_name(), which gets away with
 * exactly one because nothing there ever needs two results alive at once;
 * several of the sentences below name two or more materials in a single
 * printf() argument list (a heat_sources list beside a target material, a
 * hardens_to beside a clings_to), and a single shared buffer would let the
 * later call silently overwrite the earlier one before printf() ever runs
 * (argument evaluation order is unspecified, so this is not a hypothetical
 * - see material_hex()'s own comment on the exact same hazard, solved the
 * same way there with caller-owned buffers instead of a ring; a ring is
 * enough here because every caller copies the text out via printf() before
 * this ring could wrap all the way around within one statement). */
#define MAT_SPAN_RING 8
#define MAT_SPAN_LEN  96
static char mat_span_bufs[MAT_SPAN_RING][MAT_SPAN_LEN];
static int mat_span_next = 0;

static const char *mat_span(uint8_t v, const char *name)
{
    char hex[COLOR_LEN];
    legible_hex(v, hex, sizeof(hex));
    char *buf = mat_span_bufs[mat_span_next];
    mat_span_next = (mat_span_next + 1) % MAT_SPAN_RING;
    snprintf(buf, MAT_SPAN_LEN, "$\\textcolor{%s}{\\text{%s}}$", hex, name);
    return buf;
}

/* mat_span(), naming the material through the same to_name()/prose_name()
 * pair every plain-text clause already used to print it - the common case
 * where the displayed name is exactly what a TARGET field's raw byte
 * decodes to, lowercased for prose. */
static const char *mat_span_v(uint8_t v)
{
    return mat_span(v, prose_name(to_name(v)));
}

/* The RATE/frequency segment for a default per-material clause - **word**,
 * bold, with its own leading space, or the empty string when the rate
 * ladder falls silent (adverb_for()'s middle band - see its own top
 * comment). Never returns an empty "****": a silent adverb returns "",
 * full stop, so a format string that places this right before a literal
 * word boundary (a comma, a "from", a period) collapses cleanly to a
 * single space there instead of a double space or a pair of empty
 * asterisks - every call site below relies on that and does NOT also put
 * a literal space in front of this call's slot. Ring-buffered for the
 * same reason mat_span() is - emit_acid() alone needs two rate words
 * (dissolves and fizz) live in one sentence. */
#define RATE_GAP_RING 4
#define RATE_GAP_LEN  40
static char rate_gap_bufs[RATE_GAP_RING][RATE_GAP_LEN];
static int rate_gap_next = 0;

static const char *rate_gap(const char *word)
{
    if (word[0] == '\0') {
        return "";
    }
    char *buf = rate_gap_bufs[rate_gap_next];
    rate_gap_next = (rate_gap_next + 1) % RATE_GAP_RING;
    snprintf(buf, RATE_GAP_LEN, " **%s**", word);
    return buf;
}

/* Builds a colour-marked, prose-cased join of every all_rows[] row `pred`
 * selects - the DEFAULT-section equivalent of join_names() (used for the
 * plain pairwise-table columns) and of collect_material_list() (used for
 * emit_anatomy()'s raw-coloured examples): every member gets its own
 * legible colour, never a single flat span around the whole joined
 * string, because a heat_sources list can legitimately name two materials
 * (Fire, Lava) that must not share a colour. */
static void build_colored_list(bool (*pred)(const mrow_t *), const char *sep,
                                char *out, size_t cap)
{
    out[0] = '\0';
    bool first = true;
    for (size_t i = 0; i < all_rows_count; i++) {
        if (!pred(&all_rows[i])) continue;
        const char *span =
            mat_span(all_rows[i].color_id, prose_name(all_rows[i].name));
        size_t len = strlen(out);
        snprintf(out + len, cap - len, "%s%s", first ? "" : sep, span);
        first = false;
    }
    if (out[0] == '\0') {
        snprintf(out, cap, "(none)");
    }
}

/* Startup check: every ADVERB_EXCEPTIONS entry must name a real field, a
 * real material row, carry a non-empty `why`, and actually DIFFER from
 * what the ladder would print unassisted - an override that now agrees
 * with the default is stale (the ladder moved out from under it, or the
 * source value changed), and silently keeping it would hide that instead
 * of surfacing it. Run once, at startup, after build_rows() - see this
 * file's own field_docs_offsets_are_sound() for the same idiom applied to
 * a different table. */
static void adverb_exceptions_are_sound(void)
{
    for (size_t i = 0; i < ARRAY_LEN(ADVERB_EXCEPTIONS); i++) {
        const adverb_exception_t *e = &ADVERB_EXCEPTIONS[i];
        const field_doc_t *fd = field_doc(e->field); /* exits(1) itself if
                                    * the field is not a real field_docs[]
                                    * row */
        if (e->why == NULL || e->why[0] == '\0') {
            fprintf(stderr,
                    "dump_reactions: ADVERB_EXCEPTIONS[%zu] (%s) has no "
                    "`why` - every override needs the measurement that "
                    "justifies it\n", i, e->field);
            exit(1);
        }
        const mrow_t *row = NULL;
        for (size_t j = 0; j < all_rows_count; j++) {
            if (all_rows[j].color_id == e->cell) {
                row = &all_rows[j];
                break;
            }
        }
        if (row == NULL) {
            fprintf(stderr,
                    "dump_reactions: ADVERB_EXCEPTIONS[%zu] (%s) names "
                    "cell %u, which is not any row build_rows() produced\n",
                    i, e->field, (unsigned)e->cell);
            exit(1);
        }
        const uint8_t raw =
            *(const uint8_t *)((const unsigned char *)row->r + fd->offset);
        const char *would_be = adverb(e->field, raw);
        if (strcmp(would_be, e->adverb) == 0) {
            fprintf(stderr,
                    "dump_reactions: ADVERB_EXCEPTIONS[%zu] (%s on %s) is "
                    "stale - the ladder already computes \"%s\" for this "
                    "value on its own, so the override does nothing; "
                    "remove it\n",
                    i, e->field, row->name, would_be);
            exit(1);
        }
    }
}


/* The liquid(s) that actually wet things - KIND_LIQUID with wets != 0 -
 * derived from the data rather than hardcoded, so `soaks`/`drinks` keep
 * naming the right liquid(s) if a second wetting liquid is ever added.
 * Water is the only one today (see material.c, one `.wets = 1` hit), but
 * nothing below assumes that.
 *
 * Populated once in main(), right after build_rows() fills all_rows[] - via
 * build_colored_list(), not join_names(): every DEFAULT per-material clause
 * that reads this wants the coloured, legible-hex form (see build_colored_
 * list()'s own comment), so this string already carries each member's own
 * $\textcolor{}{}$ span - it is not a plain name list any more. See
 * reaction_t.wets's own comment in material.h for why "any liquid" is the
 * wrong claim for `soaks`/`drinks`: a bank of sand under oil or lava turned
 * entirely into saturated soil, which is the bug `wets` exists to prevent.
 * `thaws` (GRP_THAW) is the one field that is genuinely any liquid, and
 * keeps saying so untouched - see emit_thaw(). Sized for a handful of
 * coloured spans (~40 bytes each), comfortably more than today's one
 * member needs. */
static char wetting_liquids[256];

/* The material(s) that actually radiate heat - `burns != 0` - derived the
 * same way as wetting_liquids just above, so emit_transform()/emit_ignite()
 * keep naming the right source(s) if a third heat source is ever added.
 * Fire and Lava are the only two today (see pred_burns, already used by
 * emit_pairwise_table() to join the same predicate into "Fire / Lava"), but
 * nothing below assumes there are exactly two. Coloured, like
 * wetting_liquids just above - see that variable's own comment. */
static char heat_sources[256];

/* The liquid(s) that quench a fire - KIND_LIQUID, neither fuel nor a heat
 * source itself - mirroring neighbor_quenches() (sand_reactions.c)
 * exactly, the same predicate emit_pairwise_table() already uses
 * (is_quenching_liquid()) for the pairwise column. Water and Acid both
 * qualify today (Oil is fuel, Lava is a heat source), but nothing below
 * assumes there are exactly two - replaces the old hardcoded "a quenching
 * liquid" phrase in emit_burn() with the real, derived list, coloured like
 * wetting_liquids above. */
static char quenching_liquids[256];

/* The material(s) `dries != 0` selects - see pred_dries()'s own comment
 * for why that is the right predicate. Dirt alone today. Coloured, like
 * the lists above - replaces the hardcoded word "soil" wherever it
 * appeared as prose glue rather than a real field value. */
static char soil_names[64];

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
        printf("- *Catches* %s%s from %s%s.\n", mat_span_v(MAT_FIRE),
               rate_gap(adv), heat_sources, air);
    } else if (r->ignites_to == self_id) {
        /* "burns in place" - shorter than the old "burns where it stands,
         * rather than flaring away", and the same claim: burning is a
         * STATE of this material (see the comment above), so it stays put
         * rather than becoming a flame that floats off on its own. */
        printf("- *Catches* %s%s from %s%s, and burns in place.\n",
               mat_span_v(MAT_FIRE), rate_gap(adv), heat_sources, air);
    } else {
        printf("- *Catches* %s%s from %s%s, *charring to* %s.\n",
               mat_span_v(MAT_FIRE), rate_gap(adv), heat_sources, air,
               mat_span_v(r->ignites_to));
    }
}

static void emit_burn(const reaction_t *r)
{
    if (r->burns == 0 && r->burn_decay == 0) return;

    if (r->burns != 0) {
        printf("- Is a heat source in its own right");
    } else {
        printf("- Once alight, *burns down*%s",
               rate_gap(adverb("burn_decay", r->burn_decay)));
    }
    if (r->flare != 0) {
        printf(", and *licks flame into an empty neighbour*%s",
               rate_gap(adverb("flare", r->flare)));
    }
    if (r->residue != 0) {
        /* `residue` is a one-shot chance at the moment a burn finishes,
         * not a per-step rate - adverb() always returns a frequency word
         * for it (chance-scale fields never fall silent - see
         * frequency_words[]'s own comment), so rate_gap() never collapses
         * this one to nothing in practice; it is still routed through the
         * same helper as every other rate/frequency mention for one
         * consistent bold treatment. */
        printf(";%s *leaves* %s when it burns out",
               rate_gap(adverb("residue", r->residue)),
               mat_span_v(MAT_SMOKE));
    }

    /* Quenching. step_one_burning_cell() (sand_reactions.c) reads
     * quench_to only on the `burns` path - a burn_decay material's cell
     * is reset to unlit (variant 0) on contact with a quenching liquid
     * WITHOUT ever consulting quench_to, so printing quench_to's value
     * for that case would describe a field the code provably never reads.
     * That is a code fact pulled from the read site, not a guess, and it
     * is the kind of thing this generator is allowed to know - it is not
     * a hidden threshold the way SAND_SHOCK_HEAT is.
     *
     * Named quenching liquids, not the old generic "a quenching liquid" -
     * quenching_liquids mirrors neighbor_quenches() (sand_reactions.c)
     * exactly (see pred_dries's neighbour, is_quenching_liquid(), and this
     * file's own top comment on quenching_liquids). */
    if (r->burns != 0) {
        const char *quenched =
            (r->quench_to != 0) ? mat_span_v(r->quench_to) : "nothing";
        printf(". Touched by %s, *becomes* %s", quenching_liquids, quenched);
    } else {
        printf(". Touched by %s, simply goes out", quenching_liquids);
    }
    printf(".\n");
}

static void emit_transform(const reaction_t *r, uint8_t cell)
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
        printf("- Under long heat from %s, *melts* to %s.\n", heat_sources,
               mat_span_v(r->heats_to));
    } else if (r->heat_chance == 0) {
        /* `melts` alone: heats_to reachable ONLY through direct contact
         * with lava - not a flame, not an ember, not heat through a wall.
         * The only row on this path today is Root, and the whole reason
         * it exists is that a root must shrug off fire yet give way to
         * molten rock; see reaction_t.melts. Named as "lava" rather than
         * through heat_sources on purpose - heat_sources is every burning
         * material, and naming them all here would state the exact
         * opposite of what this field means. */
        if (r->melts == 0) return;
        printf("- Touching %s - and %s only, never a flame - "
               "*becomes* %s%s.\n",
               mat_span_v(MAT_LAVA), mat_span_v(MAT_LAVA),
               mat_span_v(r->heats_to),
               rate_gap(adverb_cell("melts", r->melts, cell)));
    } else {
        printf("- Beside %s, *melts* to %s%s.\n", heat_sources,
               mat_span_v(r->heats_to),
               rate_gap(adverb_cell("heat_chance", r->heat_chance, cell)));
        if (r->melts != 0) {
            printf("  Touching %s directly, *melts*%s instead.\n",
                   mat_span_v(MAT_LAVA),
                   rate_gap(adverb_cell("melts", r->melts, cell)));
        }
        if (r->flaw_to != 0) {
            /* Same trigger as the clause just printed - the SAME roll,
             * not a second one - so this reads as a qualifier on it
             * rather than a separate reaction. */
            printf("  *Comes out as* %s%s instead, in clumped runs rather "
                   "than an even speckle.\n",
                   mat_span_v(r->flaw_to),
                   rate_gap(adverb("flaw_chance", r->flaw_chance)));
        }
    }
}

static void emit_spoils(const reaction_t *r)
{
    if (r->spoils_to == 0) return;
    printf("- *Spoils* into %s %s.\n", mat_span_v(r->spoils_to),
           cause_marked("spoils_to", 0));
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
        printf("- *Holds heat*%s", rate_gap(adverb("heat_ramp", r->heat_ramp)));
        if (r->conducts != 0) {
            printf(" and *passes it on*%s",
                   rate_gap(adverb("conducts", r->conducts)));
        }
        if (r->cools != 0) {
            printf(", *draining back to ambient*%s once nothing is "
                   "heating it", rate_gap(adverb("cools", r->cools)));
        }
    } else {
        printf("- *Passes heat on*%s, without banking any of it itself",
               rate_gap(adverb("conducts", r->conducts)));
    }
    printf(".\n");
}

/* Its own function rather than folded into emit_temperature() just above:
 * that one is gated on heat_ramp/conducts, banking or passing heat along,
 * and water (boils's whole reason for existing) does neither - it just
 * boils where it stands, so emit_temperature()'s own gate would skip it
 * entirely if this clause lived there instead. */
static void emit_boils(const reaction_t *r)
{
    if (r->boils == 0) return;
    const uint8_t boils_to = r->boils_to ? r->boils_to : MAT_STEAM;
    printf("- Once conducted heat reaches it, *boils* into %s%s.\n",
           mat_span_v(boils_to), rate_gap(adverb("boils", r->boils)));
}

static void emit_cold(const reaction_t *r)
{
    if (r->chills == 0) return;
    printf("- *Chills whatever it touches*%s.\n",
           rate_gap(adverb("chills", r->chills)));
}

static void emit_warmth(const reaction_t *r)
{
    if (r->warms == 0) return;
    printf("- *Warms whatever it touches*%s, without igniting or "
           "quenching anything.\n", rate_gap(adverb("warms", r->warms)));
}

static void emit_thaw(const reaction_t *r)
{
    if (r->thaws == 0) return;
    /* `thaws` shares its product with GRP_TRANSFORM's `heats_to` - the
     * liquid-contact roll in sand_reactions.c writes the same target
     * field the heat-contact roll does, so naming it here is the field's
     * actual behaviour, not redundant prose borrowed from another group. */
    if (r->heats_to != 0) {
        printf("- *Melts in any liquid it touches*%s, becoming %s.\n",
               rate_gap(adverb("thaws", r->thaws)), mat_span_v(r->heats_to));
    } else {
        printf("- *Melts in any liquid it touches*%s.\n",
               rate_gap(adverb("thaws", r->thaws)));
    }
}

static void emit_wet(const reaction_t *r)
{
    if (r->wets != 0) {
        printf("- *Wets* whatever it touches: things that soak will draw "
               "it in.\n");
    }
    if (r->soaks != 0) {
        /* `soaks` only ever fires beside a wetting liquid - see
         * sand_reactions.c's own soaking loop, which skips a neighbour
         * outright when reaction_of(n)->wets == 0 - so this names
         * wetting_liquids, not "any liquid" (that claim is `thaws`'s
         * alone; see this file's comment on wetting_liquids). */
        if (r->soaks_to != 0) {
            printf("- *Soaks up* any %s it touches%s, becoming %s "
                   "once it takes a unit in.\n",
                   wetting_liquids, rate_gap(adverb("soaks", r->soaks)),
                   mat_span_v(r->soaks_to));
        } else {
            printf("- *Soaks up* any %s it touches%s, growing wetter "
                   "(its own moisture rises) rather than changing into "
                   "anything.\n", wetting_liquids,
                   rate_gap(adverb("soaks", r->soaks)));
        }
    }
    if (r->dries != 0) {
        printf("- *Dries back out*%s, on its own, with no partner "
               "needed.\n", rate_gap(adverb("dries", r->dries)));
    }
}

static void emit_acid(const reaction_t *r)
{
    if (r->dissolves != 0) {
        printf("- *Dissolves an adjacent cell*%s",
               rate_gap(adverb("dissolves", r->dissolves)));
        if (r->fizz != 0) {
            /* `fizz` is a one-shot chance on the cell just eaten, not a
             * rate - adverb() always returns a frequency word for it
             * (chance-scale fields never fall silent), and "leaving smoke
             * behind" stays unmarked glue rather than a second verb mark
             * in the same clause. */
            printf(",%s leaving %s behind",
                   rate_gap(adverb("fizz", r->fizz)),
                   mat_span_v(MAT_SMOKE));
        }
        printf(".\n");
    }
    if (r->dissolvable != 0) {
        /* `dissolvable` is the chance a single ATTEMPT to dissolve THIS
         * material succeeds (see its own comment in material.h) - an EASE
         * question, not a frequency one, so adverb() reads it through
         * ease_words[] rather than frequency_words[] (see field_docs[]'s
         * FCHANCE_VOCAB row for this field) and the material stays the
         * subject: "Dissolves in acid handily", not the fronted-adverb
         * "Mostly gives way to acid". */
        printf("- *Dissolves in* %s%s.\n", mat_span_v(MAT_ACID),
               rate_gap(adverb("dissolvable", r->dissolvable)));
    }
}

static void emit_evaporates(const reaction_t *r)
{
    if (r->evaporates == 0) return;
    printf("- *Spontaneously evaporates into* %s%s - unconditional, no "
           "heat or neighbour required.\n",
           mat_span_v(MAT_GAS), rate_gap(adverb("evaporates", r->evaporates)));
}

static void emit_condense(const reaction_t *r)
{
    if (r->condenses == 0 || r->condenses_to == 0) return;
    printf("- A 2x2 block of it%s *condenses* into a single cell of %s.\n",
           rate_gap(adverb("condenses", r->condenses)),
           mat_span_v(r->condenses_to));
}

static void emit_grow(const reaction_t *r)
{
    if (r->grows != 0) {
        printf("- *Grows into wet* %s%s, against gravity, spending a "
               "level of that %s's moisture per cell.\n",
               soil_names, rate_gap(adverb("grows", r->grows)), soil_names);
    }
    if (r->falls != 0) {
        printf("- *Falls under gravity*%s when there is empty space "
               "beneath it.\n", rate_gap(adverb("falls", r->falls)));
    }
    if (r->withers != 0) {
        if (r->sheltered_by != 0) {
            printf("- *Withers away*%s if it cannot reach water through "
                   "its own roots and is not touching %s.\n",
                   rate_gap(adverb("withers", r->withers)),
                   mat_span_v(r->sheltered_by));
        } else {
            printf("- *Withers away*%s if it cannot reach water through "
                   "its own roots.\n",
                   rate_gap(adverb("withers", r->withers)));
        }
    }
}

static void emit_harden(const reaction_t *r)
{
    if (r->hardens_to == 0) return;
    /* `harden_chance`, `holds_line` and `canopy` are each a one-shot
     * decision made once, at the moment a run hardens - not a per-step
     * rate against a partner - so adverb() gives each a frequency word
     * ("mostly", "occasionally", ...) here, and each is phrased before its
     * verb ("mostly hardens") rather than after ("hardens mostly"), which
     * is the reading a frequency word wants. */
    printf("- A straight run of %u cells%s *hardens* into %s",
           (unsigned)r->harden_run,
           rate_gap(adverb("harden_chance", r->harden_chance)),
           mat_span_v(r->hardens_to));
    if (r->trunk_girth != 0) {
        printf(", up to %u cells wider at the foot than at the tip",
               (unsigned)r->trunk_girth);
    }
    if (r->holds_line != 0) {
        printf(", and%s a limb holds its own direction (rather than "
               "bending back toward gravity)",
               rate_gap(adverb("holds_line", r->holds_line)));
    }
    if (r->clings_to != 0) {
        printf("; the hardened body counts as part of %s",
               mat_span_v(r->clings_to));
    }
    printf(".\n");
    if (r->canopy != 0 && r->canopy_to != 0) {
        printf("- The moment it hardens, it also%s *leafs its crown* with "
               "%s, one candidate space at a time.\n",
               rate_gap(adverb("canopy", r->canopy)),
               mat_span_v(r->canopy_to));
    }
}

static void emit_regrow(const reaction_t *r)
{
    if (r->sprouts != 0 && r->sprouts_to != 0) {
        printf("- Standing in wet %s, *sprouts* %s beside itself%s.\n",
               soil_names, mat_span_v(r->sprouts_to),
               rate_gap(adverb("sprouts", r->sprouts)));
    }
    if (r->buds != 0 && r->buds_to != 0) {
        printf("- Once already in leaf and able to reach water, *buds* "
               "new %s beside itself%s.\n",
               mat_span_v(r->buds_to), rate_gap(adverb("buds", r->buds)));
    }
    if (r->drinks != 0) {
        /* Same wetting-liquid gate as `soaks` - step_one_drinking_cell()
         * (sand_reactions.c) requires reaction_of(n)->wets != 0 on the
         * neighbour it drinks from - so this names wetting_liquids rather
         * than the generic "a liquid" the old wording implied.
         *
         * Simplified from an earlier version that said the water "comes
         * out as a level of moisture in the soil at its root, not in
         * itself" - that named an invisible internal gate ("room for
         * more") a reader cannot observe, and denied the water changing
         * the drinker itself without saying why the sentence was even
         * under this material. This says where the water actually goes
         * instead. */
        printf("- Touching %s, *passes it down to* %s at its roots%s.\n",
               wetting_liquids, soil_names,
               rate_gap(adverb("drinks", r->drinks)));
    }
    if (r->roots != 0 && r->roots_to != 0) {
        printf("- Spending its own %s moisture to grow, bud or sprout,%s "
               "*roots into* %s it drinks from, welding that cell into "
               "%s.\n",
               soil_names, rate_gap(adverb("roots", r->roots)), soil_names,
               mat_span_v(r->roots_to));
    }
}

static void emit_shatter(const reaction_t *r)
{
    if (r->shatters_to == 0) return;
    /* Index 0: sand_reactions.c's first shatters_to REACTION_DOC(), at the
     * hot-onto-cold branch of try_heat_transform_given() - "if warmed while
     * badly chilled". Index 1 (the OTHER threshold, "if chilled while hot")
     * belongs to emit_pairwise_table()'s chills/shatters_to row instead;
     * see that function's own comment. */
    printf("- *Shatters* into %s %s.\n", mat_span_v(r->shatters_to),
           cause_marked("shatters_to", 0));
}

static void emit_material_section(const char *name, const reaction_t *r,
                                  uint8_t self_id, uint8_t color_id)
{
    if (row_is_empty(r)) return;
    printf("\n### %s\n\n", name);
    emit_ignite(r, self_id);
    emit_burn(r);
    emit_transform(r, color_id);
    emit_spoils(r);
    emit_temperature(r);
    emit_boils(r);
    emit_cold(r);
    emit_warmth(r);
    emit_thaw(r);
    emit_wet(r);
    emit_acid(r);
    emit_evaporates(r);
    emit_condense(r);
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

/* `dries != 0` is already the canonical "this variant can mean moisture"
 * marker - sand_reactions.c's own comment on the smelting wet-earth branch
 * says so explicitly, and the MOISTURE SPREADS block in that same file
 * relies on the identical test. Dirt is the only row this selects today
 * (Root has no `dries` of its own), so the derived list below reads
 * "dirt" wherever the per-material prose used to hardcode the word
 * "soil" - see the plan's own note on why a hardcoded word cannot track a
 * second moisture-bearing material if one is ever added. */
static bool pred_dries(const mrow_t *row) { return row->r->dries != 0; }

/* The two halves of the Legend's legibility note - which materials print
 * at a lightness-adjusted colour (LEGIBILITY_OVERRIDES has a row for their
 * cell) versus their exact, unadjusted device value (no row - they already
 * clear 3:1 against both backgrounds on their own). Derived rather than
 * typed out, so the note cannot drift from LEGIBILITY_OVERRIDES the way a
 * hardcoded sentence could. */
static bool pred_needs_legibility_override(const mrow_t *row)
{
    return legibility_override_for(row->color_id) != NULL;
}
static bool pred_exact_color(const mrow_t *row)
{
    return legibility_override_for(row->color_id) == NULL;
}

/* The Legend, printed once, before ## Per-material - the key to every
 * coloured name that follows, and the explanation for why colour and
 * typography mean what they mean on this page. Unlike emit_anatomy()'s own
 * material list (raw device values, deliberately - see that section's own
 * top comment), this one reads through legible_hex(): it is the actual key
 * a reader needs for the section that follows it. */
static void emit_legend(void)
{
    printf("\n## Legend\n\n");
    printf("Every material name below is coloured - this list doubles as "
           "the key: whatever colour a name gets here is the colour that "
           "same name gets in every bullet that follows.\n\n");

    for (size_t i = 0; i < all_rows_count; i++) {
        printf("%s%s", mat_span_v(all_rows[i].color_id),
               (i + 1 < all_rows_count) ? ", " : "\n\n");
    }

    char adjusted[256];
    char exact[256];
    join_names(pred_needs_legibility_override, ", ", adjusted,
               sizeof(adjusted));
    join_names(pred_exact_color, ", ", exact, sizeof(exact));
    printf("These colours are lightness-adjusted from the device's exact "
           "palette wherever the raw value fails a 3:1 WCAG contrast floor "
           "against GitHub's light and dark page backgrounds (hue and "
           "saturation are left alone - only lightness moves, and only as "
           "far as it has to): %s. Everything else - %s - already clears "
           "3:1 on its own and prints its exact, unadjusted device value; "
           "see LEGIBILITY_OVERRIDES in this file's own source for the "
           "raw/adjusted pair behind each one. `emit_anatomy()`'s own "
           "material list further down uses the raw values throughout, on "
           "purpose - it is documenting the actual palette, not standing "
           "in as this page's key.\n\n", adjusted, exact);

    printf("The pairwise table further down stays plain text, with no "
           "colour at all, deliberately: every coloured name on this page "
           "is inline LaTeX (`$\\textcolor{}{}$`), and that has only ever "
           "been confirmed to render inside running prose on GitHub, never "
           "inside a markdown TABLE CELL - so this generator does not "
           "colour table cells on an unverified assumption.\n\n");

    printf("Typography carries the other three roles colour does not: "
           "*italic* is the verb driving a clause, **bold** is a rate or "
           "frequency word (silent, rather than printed, for the common "
           "case - see this file's own adverb_for()), and ***bold "
           "italic*** is a trigger clause that lives at a read site in "
           "sand_reactions.c rather than in the table itself, recovered "
           "from that file's own REACTION_DOC() annotation at the point "
           "that decides it rather than guessed at.\n");
}

static void print_join_row(const char *a, const char *b, const char *becomes,
                           const char *rate, const char *note)
{
    printf("| %s | %s | %s | %s | %s |\n", a, b, becomes, rate, note);
}

/* The pairwise table's own rate/frequency cell. Absence means something
 * different in a table cell than it does in a sentence: silence in prose
 * reads as "ordinary speed" (see adverb_for()'s own top comment on the
 * rate ladder's silent middle), which is a real, true claim a sentence can
 * make by simply not mentioning a rate. A blank TABLE CELL cannot make
 * that claim - it reads as "nobody filled this in" - so every rate/
 * frequency cell in this table is routed through this helper, which
 * substitutes an explicit em dash for an empty adverb rather than leaving
 * the cell blank. */
static const char *table_rate(const char *word)
{
    return (word[0] == '\0') ? "—" : word;
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
            if (all_rows[i].r->fizz != 0) {
                /* `fizz` is FCHANCE - never silent (see frequency_words[]'s
                 * own comment on why chance-scale fields always print a
                 * word) - so adverb() always has something real to say
                 * here. */
                snprintf(becomes, sizeof(becomes), "nothing (%s smoke)",
                         adverb("fizz", all_rows[i].r->fizz));
            } else {
                snprintf(becomes, sizeof(becomes), "nothing");
            }
            char rate[64];
            snprintf(rate, sizeof(rate), "%s / %s",
                     table_rate(adverb("dissolves", all_rows[i].r->dissolves)),
                     table_rate(adverb("dissolvable",
                                        all_rows[j].r->dissolvable)));
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
                       table_rate(adverb("flammability",
                                          all_rows[i].r->flammability)),
                       all_rows[i].r->needs_air ? "only where it touches air"
                                                : "");
    }

    /* heats_to x burns (memoryless and ramped both go through the same
     * try_heat_transform() trigger - contact with a burning cell, or
     * through a conductor) */
    for (size_t i = 0; i < all_rows_count; i++) {
        if (all_rows[i].r->heats_to == 0) continue;
        char rate[64];
        /* `melts` is its own row against LAVA alone, never folded into
         * the burners row: it is direct contact with a burning liquid,
         * not "fire or lava, or through a conductor", and a row that has
         * ONLY melts (Root) would otherwise be printed as reachable by
         * every burner at a rate of "never" - true and useless. See
         * reaction_t.melts. */
        if (all_rows[i].r->melts != 0) {
            print_join_row(all_rows[i].name, "Lava", to_name(all_rows[i].r->heats_to),
                           table_rate(adverb("melts", all_rows[i].r->melts)),
                           "direct contact only - not fire, not through a conductor");
        }
        if (all_rows[i].r->heat_ramp == 0 && all_rows[i].r->heat_chance == 0) {
            continue; /* melts-only: the burners row would be a lie */
        }
        if (all_rows[i].r->heat_ramp != 0) {
            snprintf(rate, sizeof(rate), "under long heat (banked)");
        } else {
            snprintf(rate, sizeof(rate), "%s",
                     table_rate(adverb("heat_chance",
                                        all_rows[i].r->heat_chance)));
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
                           table_rate(adverb("chills", all_rows[i].r->chills)),
                           "");
            if (all_rows[j].r->shatters_to != 0) {
                /* Index 1: sand_reactions.c's SECOND shatters_to
                 * REACTION_DOC(), at step_one_cold_cell()'s SAND_SHOCK_HEAT
                 * check - "if chilled while hot", the direction this row
                 * itself is walking (A chills B). Index 0 belongs to
                 * emit_shatter() instead; see that function's own comment. */
                print_join_row(all_rows[i].name, all_rows[j].name,
                               to_name(all_rows[j].r->shatters_to),
                               cause_at("shatters_to", 1),
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
                               table_rate(adverb("soaks", all_rows[j].r->soaks)),
                               "the liquid pays a unit of its own mass");
            } else {
                char becomes[64];
                snprintf(becomes, sizeof(becomes), "%s, +1 moisture",
                         all_rows[j].name);
                print_join_row(all_rows[i].name, all_rows[j].name, becomes,
                               table_rate(adverb("soaks", all_rows[j].r->soaks)),
                               "no material change - soaks_to is 0");
            }
        }
        /* drinks: a THIRD cell changes (dirt at the root), not the
         * subject and not the liquid - see reaction_t.drinks. "dirt", not
         * the old hardcoded "soil" - see pred_dries()'s own comment. */
        for (size_t j = 0; j < all_rows_count; j++) {
            if (all_rows[j].r->drinks == 0) continue;
            print_join_row(all_rows[i].name, all_rows[j].name,
                           "the dirt at B's root, +1 moisture",
                           table_rate(adverb("drinks", all_rows[j].r->drinks)),
                           "B itself is unchanged - a third cell changes");
        }
    }
    /* dries: self-driven, no partner at all. */
    for (size_t i = 0; i < all_rows_count; i++) {
        if (all_rows[i].r->dries == 0) continue;
        char becomes[64];
        snprintf(becomes, sizeof(becomes), "%s, -1 moisture", all_rows[i].name);
        print_join_row(all_rows[i].name, "(none - self-driven)", becomes,
                       table_rate(adverb("dries", all_rows[i].r->dries)), "");
    }

    /* thaws x any KIND_LIQUID */
    for (size_t i = 0; i < all_rows_count; i++) {
        if (all_rows[i].r->thaws == 0) continue;
        print_join_row(all_rows[i].name, liquids, to_name(all_rows[i].r->heats_to),
                       table_rate(adverb("thaws", all_rows[i].r->thaws)),
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
 * **bold** (every rate word scannable in one pass - which is exactly what
 * made the by-feel adverb tuning pass legible once it landed), and
 * MARK_CAUSE as ***bold italic*** (a clause rather than a single word, and
 * the rarest thing on this page - every instance today is a clause
 * recovered from a REACTION_DOC() call in sand_reactions.c, via cause_at()
 * above). Glue stays plain and unmarked, same as always. This
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

/* A MARK_RATE segment that can legitimately be empty - the rate ladder's
 * silent middle (adverb_for()'s own top comment) applies here exactly as
 * it does in the default per-material clauses, and several of this
 * section's own examples land a live value in that silent band (Metal's
 * `conducts`, Ice's `chills`, and others - the ladder was tuned against
 * this table's real data, not against which examples happen to be picked
 * here). Appends its own leading-space glue AND the mark together, and
 * appends NOTHING at all when the word is empty, so the caller's own
 * surrounding glue never ends up with a stray double space or an empty
 * "****" - the same rule rate_gap() enforces for the plain-text default
 * clauses, just expressed as two seg_t entries instead of one pre-joined
 * string. Every call site below therefore drops the "seg_glue(\" \")"
 * a fixed-present rate word used to need immediately before it. */
static void seg_rate_gap(seg_t *segs, size_t *n, const char *word)
{
    if (word[0] == '\0') {
        return;
    }
    seg_glue(segs, n, " ");
    seg_mark(segs, n, MARK_RATE, word);
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
           "clean prose, unedited (see docs/Sand/Reaction-Doc-Generator-"
           "Plan.md: it is the deliverable this whole file exists to "
           "produce). This section is generated separately, from the same "
           "decode helpers "
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
           "string per field for exactly this reason, but this file never "
           "actually reads that column back out (grep `->verb` in this "
           "file - nothing matches): today the words are typed directly "
           "into the matching `emit_*()` printf() call, kept in sync with "
           "`field_docs[]` by hand instead of by the compiler.\n");
    printf("- Rate / frequency (**bold**) - the ladder bucket: "
           "`adverb_for()` for a genuine per-step rate (SCALE_RATE, silent "
           "for the ordinary middle band - see that function's own top "
           "comment for why), or `chance_bucket_for()` through "
           "frequency_words[]/ease_words[] for a one-shot chance "
           "(SCALE_CHANCE, which never falls silent). Two different "
           "ladders share this one channel because both answer the same "
           "kind of question for the same kind of raw byte; only the "
           "vocabulary differs, and it is DIFFERENT for a reason (see "
           "ease_words[]'s own comment above). Bold makes every rate word "
           "scannable in one pass. A silent rate prints no word at all "
           "here, same as in the default section - see seg_rate_gap().\n");
    printf("- Cause (***bold italic***) - a trigger that lives at a read "
           "site in sand_reactions.c, not in this table - recovered from "
           "that file's own REACTION_DOC() annotation at the point that "
           "decides it, rather than guessed at. Bold italic marks it as a "
           "clause rather than a single word, which is also the rarest "
           "thing on this page.\n");
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
        seg_rate_gap(segs, &n, adverb("flammability", row->r->flammability));
        seg_glue(segs, &n, " from ");
        seg_list(segs, &n, heat, heat_n, " or ");
        seg_glue(segs, &n, ", and burns in place.");
        print_example("Ignite - GRP_IGNITE: flammability, ignites_to, "
                       "heat_sources (emit_ignite)", segs, n);
    }

    /* GRP_BURN - emit_burn(), the `burns` branch: residue, quench_to,
     * quenching_liquids (derived, not a single field - see emit_burn()'s
     * own comment on why "a quenching liquid" was replaced with the real,
     * derived list). */
    {
        const mrow_t *row = find_row("Fire");
        char subject[COLOR_LEN];
        material_hex(row->color_id, subject, sizeof(subject));
        char quench_name[32];
        char quench_color[COLOR_LEN];
        snprintf(quench_name, sizeof(quench_name), "%s",
                 prose_name(to_name(row->r->quench_to)));
        material_hex(row->r->quench_to, quench_color, sizeof(quench_color));
        list_item_t quench[LIST_ITEM_MAX];
        const size_t quench_n = collect_material_list(is_quenching_liquid,
                                                       quench, LIST_ITEM_MAX);
        char smoke_color[COLOR_LEN];
        material_hex(MAT_SMOKE, smoke_color, sizeof(smoke_color));

        seg_t segs[SEG_MAX];
        size_t n = 0;
        seg_material(segs, &n, subject, "Fire");
        seg_glue(segs, &n, ": ");
        seg_glue(segs, &n, "Is a heat source in its own right; ");
        seg_mark(segs, &n, MARK_RATE, adverb("residue", row->r->residue));
        seg_glue(segs, &n, " ");
        seg_mark(segs, &n, MARK_VERB, "leaves");
        seg_glue(segs, &n, " ");
        seg_material(segs, &n, smoke_color, "smoke");
        seg_glue(segs, &n, " when it burns out. Touched by ");
        seg_list(segs, &n, quench, quench_n, " or ");
        seg_glue(segs, &n, ", becomes ");
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
        /* adverb_cell(), not adverb() - Sand's heat_chance is exactly the
         * row ADVERB_EXCEPTIONS overrides (see that table's own comment),
         * so this example shows the same "slowly" a reader sees in the
         * default Sand section below, not the silent middle the plain
         * ladder alone would give it. */
        seg_rate_gap(segs, &n,
                     adverb_cell("heat_chance", row->r->heat_chance,
                                 row->color_id));
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
        seg_rate_gap(segs, &n, adverb("conducts", row->r->conducts));
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
        seg_rate_gap(segs, &n, adverb("chills", row->r->chills));
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
        seg_rate_gap(segs, &n, adverb("warms", row->r->warms));
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
        seg_glue(segs, &n, " it touches");
        seg_rate_gap(segs, &n, adverb("soaks", row->r->soaks));
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
        char smoke_color[COLOR_LEN];
        material_hex(MAT_SMOKE, smoke_color, sizeof(smoke_color));

        seg_t segs[SEG_MAX];
        size_t n = 0;
        seg_material(segs, &n, subject, "Acid");
        seg_glue(segs, &n, ": ");
        seg_mark(segs, &n, MARK_VERB, "Dissolves an adjacent cell");
        seg_rate_gap(segs, &n, adverb("dissolves", row->r->dissolves));
        seg_glue(segs, &n, ", ");
        /* `fizz` is FCHANCE - never silent, unlike `dissolves` just above -
         * so this one keeps the unconditional seg_mark() the way every
         * chance-scale field in this section does. */
        seg_mark(segs, &n, MARK_RATE, adverb("fizz", row->r->fizz));
        seg_glue(segs, &n, " leaving ");
        seg_material(segs, &n, smoke_color, "smoke");
        seg_glue(segs, &n, " behind.");
        print_example("Acid - GRP_ACID: dissolves, fizz (emit_acid)", segs,
                       n);
    }

    /* GRP_GROW - emit_grow(), the grows branch. "Wet DIRT", not the old
     * hardcoded "wet soil" - see pred_dries()'s own comment and this
     * file's emit_grow(). Dirt is itself a material name now, so it gets
     * its own MARK_MATERIAL segment rather than folding into the verb
     * phrase the way "wet soil" once did. */
    {
        const mrow_t *row = find_row("Plant");
        const mrow_t *dirt = find_row("Dirt");
        char subject[COLOR_LEN];
        char dirt_color[COLOR_LEN];
        material_hex(row->color_id, subject, sizeof(subject));
        material_hex(dirt->color_id, dirt_color, sizeof(dirt_color));

        seg_t segs[SEG_MAX];
        size_t n = 0;
        seg_material(segs, &n, subject, "Plant");
        seg_glue(segs, &n, ": ");
        seg_mark(segs, &n, MARK_VERB, "Grows into wet");
        seg_glue(segs, &n, " ");
        seg_material(segs, &n, dirt_color, "dirt");
        seg_rate_gap(segs, &n, adverb("grows", row->r->grows));
        seg_glue(segs, &n, ", against gravity, spending a level of that ");
        seg_material(segs, &n, dirt_color, "dirt");
        seg_glue(segs, &n, "'s moisture per cell.");
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

    /* GRP_REGROW - emit_regrow(), the sprouts branch: sprouts, sprouts_to.
     * "Wet DIRT", not the old hardcoded "wet soil" - see the GRP_GROW
     * example just above for the same fix, and pred_dries()'s comment. */
    {
        const mrow_t *row = find_row("Wood");
        const mrow_t *dirt = find_row("Dirt");
        char subject[COLOR_LEN];
        char dirt_color[COLOR_LEN];
        material_hex(row->color_id, subject, sizeof(subject));
        material_hex(dirt->color_id, dirt_color, sizeof(dirt_color));
        char sprouts_name[32];
        char sprouts_color[COLOR_LEN];
        snprintf(sprouts_name, sizeof(sprouts_name), "%s",
                 prose_name(to_name(row->r->sprouts_to)));
        material_hex(row->r->sprouts_to, sprouts_color, sizeof(sprouts_color));

        seg_t segs[SEG_MAX];
        size_t n = 0;
        seg_material(segs, &n, subject, "Wood");
        seg_glue(segs, &n, ": Standing in wet ");
        seg_material(segs, &n, dirt_color, "dirt");
        seg_glue(segs, &n, ", ");
        seg_mark(segs, &n, MARK_VERB, "sprouts");
        seg_glue(segs, &n, " ");
        seg_material(segs, &n, sprouts_color, sprouts_name);
        seg_glue(segs, &n, " beside itself");
        seg_rate_gap(segs, &n, adverb("sprouts", row->r->sprouts));
        seg_glue(segs, &n, ".");
        print_example("Regrow - GRP_REGROW: sprouts, sprouts_to "
                       "(emit_regrow)", segs, n);
    }

    /* GRP_SHATTER - emit_shatter(): shatters_to, and MARK_CAUSE - one of
     * two per-material clauses that ever print it (emit_spoils() is the
     * other). Index 0, matching emit_shatter()'s own call - see that
     * function's comment. */
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
        seg_mark(segs, &n, MARK_CAUSE, cause_at("shatters_to", 0));
        seg_glue(segs, &n, ".");
        print_example("Shatter - GRP_SHATTER: shatters_to (emit_shatter)",
                       segs, n);
    }
}

/*-----------------------------------------------------------------------
 * main
 *---------------------------------------------------------------------*/

int main(int argc, char **argv)
{
    if (argc != 2) {
        fprintf(stderr, "usage: %s <path/to/sand_reactions.c>\n",
                (argc > 0) ? argv[0] : "dump_reactions");
        fprintf(stderr, "  sand_reactions.c is read as TEXT, never linked - "
                "see reaction_doc.h and this file's own "
                "parse_reaction_docs() for why.\n");
        return 1;
    }
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
    adverb_exceptions_are_sound();
    legibility_overrides_are_sound();
    /* sand_reactions.c's own REACTION_DOC() calls, read as text - must run
     * before any emit_*() call below, since emit_shatter(), emit_spoils()
     * and emit_pairwise_table() all pull their cause clauses out of this. */
    {
        char *sand_reactions_src = read_whole_file(argv[1]);
        parse_reaction_docs(argv[1], sand_reactions_src);
        free(sand_reactions_src);
    }
    causes_are_complete();
    /* Each of these four is only ever read in DEFAULT-section prose
     * (emit_wet(), emit_regrow(), emit_transform(), emit_ignite(),
     * emit_burn(), emit_grow()) - never a table cell or heading - so
     * build_colored_list() already lowercases and colours each member once
     * here, rather than at every call site. pred_burns/pred_wets_liquid are
     * shared with emit_pairwise_table()'s plain, uncoloured columns; see
     * this section's own top comment on why the pairwise table never gets
     * this treatment. */
    build_colored_list(pred_wets_liquid, " or ", wetting_liquids,
                        sizeof(wetting_liquids));
    build_colored_list(pred_burns, " or ", heat_sources, sizeof(heat_sources));
    build_colored_list(is_quenching_liquid, " or ", quenching_liquids,
                        sizeof(quenching_liquids));
    build_colored_list(pred_dries, " or ", soil_names, sizeof(soil_names));

    /* The title ("# Reaction Table") and everything before this point in
     * the checked-in doc are hand-owned structural furniture, not this
     * file's output - see report_reactions.sh's own top comment for why
     * that split exists (it is what lets a hand-written top note and a
     * hand-added appendix survive a regenerate). Everything THIS file
     * prints, start to finish, is wrapped in one BEGIN/END GENERATED pair
     * so report_reactions.sh can replace exactly that span and leave
     * everything outside it alone; the two marker lines are therefore
     * literal, load-bearing text, not decoration - report_reactions.sh
     * matches them by exact string equality, so if this spelling ever
     * changes here it must change there too. */
    printf("<!-- BEGIN GENERATED -->\n");
    printf("<!-- GENERATED by launcher/main/apps/sand/tools/dump_reactions.c "
           "via launcher/main/apps/sand/tools/report_reactions.sh - do not "
           "edit by hand. Edit the generator, or the tables in material.c "
           "it reads, and regenerate: everything from the BEGIN GENERATED "
           "marker above to the matching END GENERATED marker below is "
           "replaced whole on every run; hand-written material outside "
           "those two markers is left untouched - see this doc's own top "
           "note. -->\n\n");
    printf("See docs/Sand/Reaction-Doc-Generator-Plan.md for the design "
           "this follows. Every rate/frequency word below is the ladder's "
           "computed bucket (see this file's own adverb_for()/"
           "chance_bucket_for()), with one checked, by-feel exception - see "
           "ADVERB_EXCEPTIONS in the source. Every ***bold italic*** clause "
           "marks a trigger that lives at a read site in sand_reactions.c "
           "rather than in the table itself, recovered from that file's "
           "own REACTION_DOC() annotations rather than guessed at.\n");

    emit_legend();

    printf("\n## Per-material\n");
    for (size_t i = 0; i < all_rows_count; i++) {
        emit_material_section(all_rows[i].name, all_rows[i].r,
                              all_rows[i].self_id, all_rows[i].color_id);
    }

    emit_pairwise_table();
    emit_anatomy();

    printf("\n<!-- END GENERATED -->\n");

    return 0;
}
