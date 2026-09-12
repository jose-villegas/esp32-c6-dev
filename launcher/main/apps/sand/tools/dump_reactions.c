/*
 * dump_reactions - compile material.c's reaction tables into markdown.
 *
 * Run through report_reactions.sh, which builds this, captures its stdout and
 * splices it into the BEGIN/END GENERATED region of
 * docs/sand/Reaction-Table.md. A splice and not a whole-file overwrite because
 * some real mechanics - lava's cool-off chaining, the covered-lava burst,
 * water and acid draining faster on stone and glass - live entirely at a read
 * site in sand_reactions.c with no reaction_t field to walk, so a human
 * documents those by hand outside the markers and an overwrite would delete
 * them on every regenerate (bd esp32c6-3mu). See
 * docs/plans/Reaction-Doc-Generator-Plan.md for the design.
 *
 * A program and not a script over the text because material.c's tables are
 * constant expressions - MATX(MATX_LEAF), MATX()'s bit-shift, SAND_SHOCK_HEAT
 * - and regexing those back into values means reimplementing the preprocessor.
 * Linking material.c and reading reactions[] at runtime resolves every one of
 * them for free, the way the simulation itself does.
 *
 * A `_to` field's kind is decided by its VALUE, not by its name: a byte
 * >= (MAT_EXTENDED << 4) is a whole cell spec, anything below is a plain
 * material id, which is what place_reacted() itself does. to_name() checks the
 * value rather than consulting a per-field list, since MAT_GLASS.shatters_to
 * holds an ordinary material id while MAT_DIRT.heats_to holds an extended
 * spec, and a fixed list would confidently print "empty" for glass shattering
 * into sand.
 *
 * Every rate and frequency word is the ladder's computed bucket, and a field
 * whose real trigger lives at a read site prints that site's own
 * REACTION_DOC(field, "why") annotation rather than a clause guessed from the
 * field name.
 */

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
#include "material_palette.h"
/* Declarations and #defines only - no sand.c body comes with it, so this
 * does not reopen reaction_doc.h's "do not link the simulation" decision. */
#include "sand.h"

/* Not defined anywhere in this codebase today (checked) - every other array
 * here is either sized by a named constant or walked with sizeof/pointer
 * arithmetic. field_docs[] is neither: its length IS the claim being
 * checked below, so it needs its own name rather than a magic number. */
#define ARRAY_LEN(a) (sizeof(a) / sizeof((a)[0]))

/*
 * One row per reaction_t field. `group` says which clause it contributes
 * to (one emit_* function per group below); `kind` says how a TARGET
 * field's value decodes. `verb`/`adverb_override` cover an irregular verb
 * form, or a rate whose ladder bucket reads wrong once partner count or
 * persistence is accounted for; nothing here sets either yet, so every
 * adverb takes the computed bucket unedited.
 */

typedef enum {
    GRP_IGNITE = 0,  /* catching fire: flammability, ignites_to, needs_air */
    GRP_BURN,        /* being alight: burns, burn_decay, residue, quench_to, flare */
    GRP_TRANSFORM,   /* heat alone, no flame involved: heats_to, heat_chance */
    GRP_TEMPERATURE, /* banking/passing heat: heat_ramp, cools, conducts */
    GRP_COLD,        /* drawing heat out of a neighbour: chills */
    GRP_WARMTH,      /* convection into a neighbour: warms */
    GRP_THAW,        /* melting in any liquid: thaws */
    GRP_WET,         /* the wetting family: wets, soaks, soaks_to, dries */
    GRP_ACID,        /* dissolving / being dissolved: dissolves, dissolvable, fizz */
    GRP_GROW,        /* extending into wet soil, or dying without it */
    GRP_HARDEN,      /* becoming wood, and what that leaves behind */
    GRP_REGROW,      /* new growth and foliage from a finished trunk, and drinking */
    GRP_SHATTER,     /* thermal shock: shatters_to */
    GRP_PADDING,     /* not chemistry at all: the bytes that round the row
                       * to a 64-byte stride - see reaction_t's stride_pad0 */
    GRP_CONDENSE,    /* a 2x2 block collapsing into one cell: condenses,
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
    FK_PAD,       /* not a field: a byte that exists only to round the row's
                   * size. Never read, never emitted - it has a row here
                   * solely so the every-byte-is-documented walk below stays
                   * a full check rather than being told to skip offsets */
} field_kind_t;

/* Only meaningful when kind == FK_RATE - splits FK_RATE into two questions.
 * adverb_for() applies to fields with a time axis. chance_bucket_for()
 * applies to one-shot rolls. See adverb(). */
typedef enum {
    SCALE_NA = 0, /* kind != FK_RATE - no ladder of either kind applies */
    SCALE_RATE,   /* rolled every step against a partner - speed ladder */
    SCALE_CHANCE, /* a one-shot roll at a single moment - frequency ladder */
} field_scale_t;

typedef struct {
    size_t offset;
    const char* name;
    group_id_t group;
    field_kind_t kind;
    field_scale_t scale;             /* meaningful only when kind == FK_RATE -
                                    * see field_scale_t's own comment */
    const char* verb;                /* NULL where the group's own template
                                    * carries the wording instead */
    const char* adverb_override;     /* NULL = use the computed rate-ladder
                                    * bucket; unset by every field in
                                    * phase 1, see this file's top comment */
    const char* const* chance_vocab; /* NULL = frequency_ words[],
                                      * `dissolvable` asks for ease_words[] -
                                      * see their comments. */
} field_doc_t;

#define F(field, grp, knd, vb)  {offsetof(reaction_t, field), #field, (grp), (knd), SCALE_NA, (vb), NULL, NULL}
/* A genuine per-step rate - see field_scale_t's own comment. */
#define FRATE(field, grp, vb)   {offsetof(reaction_t, field), #field, (grp), FK_RATE, SCALE_RATE, (vb), NULL, NULL}
/* A one-shot chance at a single moment, not a rate - see field_scale_t's
 * own comment for why this needs a different ladder from FRATE. Renders
 * through frequency_words[] (the default "how often" vocabulary) unless a
 * row overrides chance_vocab directly - see FCHANCE_VOCAB below. */
#define FCHANCE(field, grp, vb) {offsetof(reaction_t, field), #field, (grp), FK_RATE, SCALE_CHANCE, (vb), NULL, NULL}
/* Same as FCHANCE, but names an explicit chance_vocab instead of taking
 * the frequency_words[] default - see field_doc_t.chance_vocab. */
#define FCHANCE_VOCAB(field, grp, vb, voc)                                                                             \
    {offsetof(reaction_t, field), #field, (grp), FK_RATE, SCALE_CHANCE, (vb), NULL, (voc)}

/* Two SCALE_CHANCE vocabularies share chance_bucket_for()'s four slots -
 * only the words differ. 255 still needs a word here, unlike the rate
 * ladder: no shared word fits both "how often" and "how well". Silence
 * isn't an option either: a missing word for `residue` reads as "always",
 * which can be false. frequency_words[] = "how often"; ease_words[] =
 * "how well, given it's already happening" (dissolvable only). */
static const char* const frequency_words[] = {
    "always",
    "mostly",
    "occasionally",
    "seldom",
};
static const char* const ease_words[] = {
    "outright",
    "handily",
    "adequately",
    "poorly",
};

static const char* const frequency_words_child[] = {
    "always",
    "usually",
    "sometimes",
    "hardly ever",
};
static const char* const ease_words_child[] = {
    "completely",
    "a lot",
    "a little",
    "hardly at all",
};

static const field_doc_t field_docs[] = {
    /* GRP_IGNITE */
    FRATE(flammability, GRP_IGNITE, "catches"),
    F(ignites_to, GRP_IGNITE, FK_TARGET, NULL),
    F(needs_air, GRP_IGNITE, FK_FLAG, NULL),

    /* GRP_BURN. `residue` is a one-shot chance at the moment a burn-down
     * finishes, not a per-step rate against a partner - see field_scale_t
     * and emit_burn(). */
    F(burns, GRP_BURN, FK_FLAG, NULL),
    FRATE(burn_decay, GRP_BURN, "burns down"),
    F(lit_from, GRP_BURN, FK_COUNT_MAG, NULL),
    FCHANCE(residue, GRP_BURN, "leaves smoke"),
    F(quench_to, GRP_BURN, FK_TARGET, NULL),
    FRATE(flare, GRP_BURN, "sets fire to the empty spot next to it"),
    /* Nonzero is a BLAST RADIUS IN CELLS, read only at the final burn step
     * for gunpowder - see reaction_t.explodes in material.h. Silent for every
     * material but gunpowder. */
    F(explodes, GRP_BURN, FK_COUNT_MAG, "detonates"),

    FRATE(dissolves, GRP_ACID, "eats through whatever is next to it"),
    /* `dissolvable` asks "how WELL does acid do here", not "how often" -
     * see its own comment just above emit_acid() - so it renders through
     * ease_words[] rather than the frequency_words[] every other FCHANCE()
     * row gets. */
    FCHANCE_VOCAB(dissolvable, GRP_ACID, "gives in to acid", ease_words),
    FCHANCE(fizz, GRP_ACID, "leaves smoke"),
    FRATE(evaporates, GRP_ACID, "turns into gas all by itself"),

    /* GRP_CONDENSE - the inverse of evaporation: a 2x2 block of one
     * material collapsing into a single cell of another. Genuine
     * per-step rate, gated on a 2x2 neighbourhood match rather than a
     * partner or a prior roll - the same shape as `dissolves`, not a
     * one-shot FCHANCE. */
    FRATE(condenses, GRP_CONDENSE, "turns into"),
    F(condenses_to, GRP_CONDENSE, FK_TARGET, NULL),

    F(heats_to, GRP_TRANSFORM, FK_TARGET, NULL),
    /* Took two of reaction_t's five padding bytes rather than growing the
     * row: the 64-byte stride is load-bearing (see stride_pad's own note). */
    F(crusts_to, GRP_TRANSFORM, FK_TARGET, NULL),
    FRATE(crusts, GRP_TRANSFORM, "crusts over once settled"),
    FRATE(heat_chance, GRP_TRANSFORM, "melts"),
    /* `melts` shares heats_to with heat_chance but answers to LAVA
     * alone - direct contact with a burning liquid, never a flame and
     * never heat through a conductor. See reaction_t.melts for why a
     * material with no variant has to draw that line at the source. */
    FRATE(melts, GRP_TRANSFORM, "melts under lava"),
    F(flaw_to, GRP_TRANSFORM, FK_TARGET, NULL),
    FCHANCE(flaw_chance, GRP_TRANSFORM, "comes out flawed"),
    F(spoils_to, GRP_TRANSFORM, FK_TARGET, NULL),
    FCHANCE(spoils_chance, GRP_TRANSFORM, "spoils"),

    /* GRP_TEMPERATURE */
    FRATE(heat_ramp, GRP_TEMPERATURE, "holds heat"),
    FRATE(cools, GRP_TEMPERATURE, "cools back down"),

    /* GRP_COLD / GRP_WARMTH / GRP_THAW - each one field */
    FRATE(chills, GRP_COLD, "chills whatever it touches"),
    FRATE(conducts, GRP_TEMPERATURE, "passes heat along"),
    FRATE(boils, GRP_TEMPERATURE, "boils"),
    F(boils_to, GRP_TEMPERATURE, FK_TARGET, NULL),
    FRATE(warms, GRP_WARMTH, "warms whatever it touches"),
    FRATE(thaws, GRP_THAW, "melts when anything wet touches it"),

    /* GRP_WET - the wetting family, see the plan's own section on it */
    F(wets, GRP_WET, FK_FLAG, NULL),
    FRATE(soaks, GRP_WET, "soaks up anything wet that touches it"),
    F(soaks_to, GRP_WET, FK_TARGET, NULL),
    FRATE(dries, GRP_WET, "dries out all by itself"),
    /* soil defines materials treated as ground; not same as `dries`.
     * pred_soil() reads it for soil_names. Row needed for byte-count gate. */
    F(soil, GRP_WET, FK_FLAG, NULL),
    F(tones, GRP_WET, FK_COUNT_MAG, NULL),
    F(moist_max, GRP_WET, FK_COUNT_MAG, NULL),
    F(soaked_to, GRP_WET, FK_TARGET, NULL),
    FRATE(soaked_chance, GRP_WET, "turns into"),

    /* GRP_GROW */
    FRATE(grows, GRP_GROW, "grows up into wet soil"),
    FRATE(falls, GRP_GROW, "falls down"),

    /* GRP_HARDEN - becoming wood, and what that moment leaves behind.
     * `harden_chance`, `canopy` and `holds_line` are each a one-shot
     * decision made once, at the moment a run hardens - not a per-step
     * rate against a partner - see field_scale_t and emit_harden(). */
    F(hardens_to, GRP_HARDEN, FK_TARGET, NULL),
    F(harden_run, GRP_HARDEN, FK_COUNT_MAG, NULL),
    FCHANCE(harden_chance, GRP_HARDEN, "hardens"),
    F(clings_to, GRP_HARDEN, FK_TARGET, NULL),
    FCHANCE(canopy, GRP_HARDEN, "grows leaves on top"),
    F(canopy_to, GRP_HARDEN, FK_TARGET, NULL),
    F(trunk_girth, GRP_HARDEN, FK_COUNT_MAG, NULL),
    FCHANCE(holds_line, GRP_HARDEN, "keeps growing the same way it started"),

    /* GRP_REGROW - new growth from a finished trunk, and drinking */
    FRATE(sprouts, GRP_REGROW, "sprouts foliage"),
    F(sprouts_to, GRP_REGROW, FK_TARGET, NULL),
    FRATE(buds, GRP_REGROW, "buds new growth"),
    F(buds_to, GRP_REGROW, FK_TARGET, NULL),
    FRATE(drinks, GRP_REGROW, "sends water down through its roots"),

    FCHANCE(roots, GRP_REGROW, "turns the spot into root"),
    F(roots_to, GRP_REGROW, FK_TARGET, NULL),

    /* GRP_SHATTER */
    F(shatters_to, GRP_SHATTER, FK_TARGET, NULL),

    /* NOT A REACTION, queue_flying_grain() reads dislodge_density for
     * KIND_STATIC. Grp_shatter placeholder for assert. */
    F(dislodge_density, GRP_SHATTER, FK_COUNT_MAG, NULL),

    /* Padding, not chemistry - see reaction_t. Listed so every byte of the
     * struct is still claimed by exactly one row; nothing emits these,
     * since output is driven by the fields a material actually sets. */
    F(stride_pad2, GRP_PADDING, FK_PAD, NULL),
    F(stride_pad3, GRP_PADDING, FK_PAD, NULL),
    F(stride_pad4, GRP_PADDING, FK_PAD, NULL),
};

#undef F
#undef FRATE
#undef FCHANCE
#undef FCHANCE_VOCAB

_Static_assert(ARRAY_LEN(field_docs) == sizeof(reaction_t), "every reaction_t field needs exactly one row in "
                                                            "field_docs[] - add one (group, kind, and a verb if the "
                                                            "group template needs it) for whatever field just changed "
                                                            "the struct's size");

static void
field_docs_offsets_are_sound(void) {
    bool seen[sizeof(reaction_t)] = {false};
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
                    "of\n",
                    off);
            exit(1);
        }
    }
}

static const field_doc_t*
field_doc(const char* name) {
    for (size_t i = 0; i < ARRAY_LEN(field_docs); i++) {
        if (strcmp(field_docs[i].name, name) == 0) {
            return &field_docs[i];
        }
    }
    fprintf(stderr, "dump_reactions: field_doc(\"%s\") - no such field\n", name);
    exit(1);
}

/*
 * REACTION_DOC parsing - the cause clauses no table can yield, because the
 * condition gating them lives at a read site in sand_reactions.c and
 * appears nowhere in reactions[]/extended_reactions[].
 *
 * sand_reactions.c is read as TEXT here, never linked; reaction_doc.h says
 * why that does not reverse "compile the tables, do not parse them".
 * is_known_field() below duplicates field_doc()'s lookup rather than
 * calling it, because a bad field name here is not fatal the way
 * field_doc() assumes.
 */

/* Comfortably more than the number of REACTION_DOC() calls sand_reactions.c
 * carries today (three, as of this writing) - raise it if a future one
 * trips the check in parse_reaction_docs(). */
#define CAUSE_MAX       32
#define CAUSE_FIELD_LEN 32
#define CAUSE_TEXT_LEN  160

typedef struct {
    char field[CAUSE_FIELD_LEN];
    char text[CAUSE_TEXT_LEN];
} cause_t;

static cause_t causes[CAUSE_MAX];
static size_t causes_count;

static const char* const causes_expected[] = {
    "shatters_to",
    "soaks_to",
    "spoils_to",
};

static bool
is_known_field(const char* name) {
    for (size_t i = 0; i < ARRAY_LEN(field_docs); i++) {
        if (strcmp(field_docs[i].name, name) == 0) {
            return true;
        }
    }
    return false;
}

static char*
read_whole_file(const char* path) {
    FILE* f = fopen(path, "rb");
    if (f == NULL) {
        fprintf(stderr, "dump_reactions: cannot open %s: %s\n", path, strerror(errno));
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
    char* buf = malloc((size_t)size + 1);
    if (buf == NULL) {
        fprintf(stderr,
                "dump_reactions: out of memory reading %s (%ld "
                "bytes)\n",
                path, size);
        exit(1);
    }
    const size_t got = fread(buf, 1, (size_t)size, f);
    fclose(f);
    buf[got] = '\0';
    return buf;
}

static void
parse_reaction_docs(const char* path, const char* src) {
    const char* p = src;
    int invocation_no = 0;
    while ((p = strstr(p, "REACTION_DOC(")) != NULL) {
        invocation_no++;
        p += strlen("REACTION_DOC(");
        while (isspace((unsigned char)*p)) {
            p++;
        }
        const char* field_start = p;
        while (isalnum((unsigned char)*p) || *p == '_') {
            p++;
        }
        const size_t field_len = (size_t)(p - field_start);
        if (field_len == 0 || field_len >= CAUSE_FIELD_LEN) {
            fprintf(stderr, "%s: REACTION_DOC #%d has no plain field name\n", path, invocation_no);
            exit(1);
        }
        char field[CAUSE_FIELD_LEN];
        memcpy(field, field_start, field_len);
        field[field_len] = '\0';
        while (isspace((unsigned char)*p)) {
            p++;
        }
        if (*p != ',') {
            fprintf(stderr,
                    "%s: REACTION_DOC(%s, ...) #%d - expected ',' "
                    "after the field name\n",
                    path, field, invocation_no);
            exit(1);
        }
        p++;
        while (isspace((unsigned char)*p)) {
            p++;
        }
        if (*p != '"') {
            fprintf(stderr,
                    "%s: REACTION_DOC(%s, ...) #%d - the second "
                    "argument must be a plain string literal, not an "
                    "expression\n",
                    path, field, invocation_no);
            exit(1);
        }
        p++;
        char text[CAUSE_TEXT_LEN];
        size_t tlen = 0;
        while (*p != '"') {
            if (*p == '\0' || *p == '\n') {
                fprintf(stderr,
                        "%s: REACTION_DOC(%s, ...) #%d - "
                        "unterminated string literal\n",
                        path, field, invocation_no);
                exit(1);
            }
            if (*p == '\\' && p[1] != '\0') {
                p++;
            }
            if (tlen + 1 >= CAUSE_TEXT_LEN) {
                fprintf(stderr,
                        "%s: REACTION_DOC(%s, ...) #%d - clause "
                        "text longer than %d bytes\n",
                        path, field, invocation_no, CAUSE_TEXT_LEN - 1);
                exit(1);
            }
            text[tlen++] = *p++;
        }
        p++;
        text[tlen] = '\0';
        while (isspace((unsigned char)*p)) {
            p++;
        }
        if (*p != ')') {
            fprintf(stderr,
                    "%s: REACTION_DOC(%s, ...) #%d - expected ')' "
                    "right after the string literal (adjacent-literal "
                    "concatenation is not supported here - write the "
                    "clause as one literal)\n",
                    path, field, invocation_no);
            exit(1);
        }
        if (!is_known_field(field)) {
            fprintf(stderr,
                    "%s: REACTION_DOC(%s, ...) #%d - \"%s\" is not "
                    "a field in field_docs[] (dump_reactions.c) - a typo, "
                    "or field_docs[] needs a row for it\n",
                    path, field, invocation_no, field);
            exit(1);
        }
        if (causes_count >= CAUSE_MAX) {
            fprintf(stderr,
                    "%s: more than %d REACTION_DOC() invocations - "
                    "raise CAUSE_MAX in dump_reactions.c\n",
                    path, CAUSE_MAX);
            exit(1);
        }
        snprintf(causes[causes_count].field, CAUSE_FIELD_LEN, "%s", field);
        snprintf(causes[causes_count].text, CAUSE_TEXT_LEN, "%s", text);
        causes_count++;
    }
}

static size_t
cause_count(const char* field) {
    size_t n = 0;
    for (size_t i = 0; i < causes_count; i++) {
        if (strcmp(causes[i].field, field) == 0) {
            n++;
        }
    }
    return n;
}

/* Callers rely on source order of REACTION_DOC() clauses for `field` (see
 * emit_shatter() and emit_pairwise_table()). */
static const char*
cause_at(const char* field, size_t index) {
    size_t seen = 0;
    for (size_t i = 0; i < causes_count; i++) {
        if (strcmp(causes[i].field, field) != 0) {
            continue;
        }
        if (seen == index) {
            return causes[i].text;
        }
        seen++;
    }
    fprintf(stderr,
            "dump_reactions: cause_at(\"%s\", %zu) - fewer than "
            "%zu clause(s) were found for this field\n",
            field, index, index + 1);
    exit(1);
}

static void
causes_are_complete(void) {
    bool ok = true;
    for (size_t i = 0; i < ARRAY_LEN(causes_expected); i++) {
        if (cause_count(causes_expected[i]) == 0) {
            fprintf(stderr,
                    "dump_reactions: field \"%s\" is expected to "
                    "have a REACTION_DOC(...) in sand_reactions.c, but none "
                    "was found\n",
                    causes_expected[i]);
            ok = false;
        }
    }
    if (!ok) {
        exit(1);
    }
}

/* Decoding. */

/* Rate ladder (FK_RATE/SCALE_RATE): one chance per step against a steady
 * partner (expected wait = 256/value steps). 255 = instant, no RNG draw
 * (try_ignite() short-circuits before rolling; see flammability in
 * material.h). 6..254 stays silent, reading true unqualified. Exception:
 * sand's heat_chance (16) is silent by this rule but rolls per adjacent
 * heat source, so a held-flame bed converts far slower in practice
 * (~137 steps) - a checked ADVERB_EXCEPTIONS entry below, not a moved
 * cutoff. */
#define RATE_SLOW_CUTOFF 5

static const char*
adverb_for(uint8_t v) {
    if (v == 0) {
        return "never";
    }
    if (v == 255) {
        return "instantly";
    }
    if (v <= RATE_SLOW_CUTOFF) {
        return "slowly";
    }
    return ""; /* silent middle - see this function's own top comment */
}

static int
chance_bucket_for(uint8_t v) {
    if (v >= 150) {
        return 1;
    }
    if (v >= 50) {
        return 2;
    }
    return 3; /* 1..49 */
}

static const char*
adverb(const char* field_name, uint8_t v) {
    const field_doc_t* fd = field_doc(field_name);
    if (fd->adverb_override != NULL) {
        return fd->adverb_override;
    }
    /* scale is only meaningful for FK_RATE fields, which is every field
     * that ever reaches adverb() - see field_scale_t's own comment. */
    if (fd->scale == SCALE_CHANCE) {
        /* 0 is the same flat "never" on every vocabulary - see this
         * section's own top comment on why 0/255 are handled once, here,
         * rather than duplicated into every vocabulary array. */
        if (v == 0) {
            return "never";
        }
        const char* const* vocab = (fd->chance_vocab != NULL) ? fd->chance_vocab : frequency_words;
        return (v == 255) ? vocab[0] : vocab[chance_bucket_for(v)];
    }
    return adverb_for(v);
}

/*
 * Adverb exceptions - a by-feel override for the one case where this
 * ladder's single-steady-partner model provably disagrees with a measured
 * comment in material.c (see adverb_for()'s own top comment on sand's
 * heat_chance). A tiny table with a startup soundness check, the same
 * idiom field_docs_offsets_are_sound() already uses: wrong data here is
 * worse than no override at all, so it is checked, not just declared.
 */

typedef struct {
    uint8_t cell;       /* the row this override applies to - a plain
                        * material id, or a MATX() spec for an extended
                        * one; matched against mrow_t.color_id */
    const char* field;  /* must name a real field_docs[] row */
    const char* adverb; /* the word to print instead of the ladder's own */
    const char* why;    /* the measurement/evidence for overriding the
                        * ladder - required, never empty, and expected to
                        * quote or paraphrase the material.c comment that
                        * justifies it */
} adverb_exception_t;

static const adverb_exception_t ADVERB_EXCEPTIONS[] = {
    {MAT_SAND, "heat_chance", "slowly",
     "MAT_SAND.heat_chance = 16 falls silent under the ladder above (it "
     "is above RATE_SLOW_CUTOFF), but the field's own comment in "
     "material.c measures a bed of eleven sand cells under a held flame "
     "taking 137 steps (~2.3s) to fully convert, and calls that "
     "\"deliberately slow - glass should be something you set up and "
     "wait for, not something that happens whenever a spark lands on a "
     "dune\". The gap is the model, not the data: heat_chance rolls once "
     "PER ADJACENT HEAT SOURCE, so an interior cell with only one hot "
     "neighbour waits on the cells ahead of it first - the single-"
     "steady-partner ladder cannot see that queuing effect."},
    {GUNPOWDER_CELL(2), "soaked_chance", "slowly",
     "GUNPOWDER_REACTION.soaked_chance = 8 (material.c) falls silent "
     "under the ladder above (it is above RATE_SLOW_CUTOFF), but the "
     "row's own comment calls it \"uncommon, rolled only once already "
     "fully soaked - it lingers wet a good while first\", and the commit "
     "that set it (\"soaked gunpowder turns to oil at 8 in 256, not 3 - "
     "too rare to see\") measured the earlier value of 3 as needing a "
     "raise just to be OBSERVABLE at all - the opposite of ordinary "
     "speed."},
};

/* Looks up an override for (field_name, cell) - the only two things an
 * emit_*() clause has on hand at the point it would otherwise call
 * adverb() directly. Returns NULL when no override applies, which is the
 * overwhelming common case (one entry in the table above, as of this
 * writing). */
static const char*
adverb_exception_for(const char* field_name, uint8_t cell) {
    for (size_t i = 0; i < ARRAY_LEN(ADVERB_EXCEPTIONS); i++) {
        if (ADVERB_EXCEPTIONS[i].cell == cell && strcmp(ADVERB_EXCEPTIONS[i].field, field_name) == 0) {
            return ADVERB_EXCEPTIONS[i].adverb;
        }
    }
    return NULL;
}

static const char*
adverb_cell(const char* field_name, uint8_t v, uint8_t cell) {
    const char* ov = adverb_exception_for(field_name, cell);
    return (ov != NULL) ? ov : adverb(field_name, v);
}

static const char*
adverb_child(const char* field_name, uint8_t v) {
    const field_doc_t* fd = field_doc(field_name);
    if (fd->adverb_override != NULL) {
        return fd->adverb_override;
    }
    if (fd->scale == SCALE_CHANCE) {
        if (v == 0) {
            return "never";
        }
        const char* const* vocab = (fd->chance_vocab != NULL) ? ease_words_child : frequency_words_child;
        return (v == 255) ? vocab[0] : vocab[chance_bucket_for(v)];
    }
    return adverb_for(v);
}

static const char*
adverb_cell_child(const char* field_name, uint8_t v, uint8_t cell) {
    const char* ov = adverb_exception_for(field_name, cell);
    return (ov != NULL) ? ov : adverb_child(field_name, v);
}

/* Decode TARGET field like place_reacted(). >= (MAT_EXTENDED << 4) is
 * MATX(k), < is material id. Value-based, not field-name-based - see top
 * comment for fixed list issues. */
static const char*
to_name(uint8_t v) {
    /* GUNPOWDER_LIT_CELL holds fuse ignition state, not plain "Gunpowder".
     * Named separately for extended-cell-spec branch. */
    if (v == GUNPOWDER_LIT_CELL) {
        return "Lit Gunpowder";
    }
    if (v >= (uint8_t)(MAT_EXTENDED << 4)) {
        return material_name((cell_t)v);
    }
    if (v < MATERIAL_MAX) {
        return material_by_id((material_id_t)v)->name;
    }
    /* Kept as a named fallback rather than an assert for legible future
     * errors. */
    return "?";
}

/* materials[].name is Title Case for UI labels, but always a common noun
 * here, so unconditionally lowercasing is correct; see prose_name(), main()'s
 * wetting_liquids use. */
static void
str_lower(char* s) {
    for (; *s != '\0'; s++) {
        *s = (char)tolower((unsigned char)*s);
    }
}

/* Returns a pointer into a static buffer, safe because no single printf()
 * call ever calls prose_name() twice in one statement. */
static const char*
prose_name(const char* name) {
    static char buf[32];
    size_t i = 0;
    for (; name[i] != '\0' && i + 1 < sizeof(buf); i++) {
        buf[i] = name[i];
    }
    buf[i] = '\0';
    str_lower(buf);
    return buf;
}

/* Wraps cause_at() in MARK_CAUSE typography for DEFAULT-section clauses.
 * Returns static buffer pointer, safe under "never called twice in one
 * statement" rule. */
static const char*
cause_marked(const char* field, size_t index) {
    static char buf[CAUSE_TEXT_LEN + 8];
    snprintf(buf, sizeof(buf), "***%s***", cause_at(field, index));
    return buf;
}

/*
 * One row's worth of rows (materials[] name + reactions[]/extended_
 * reactions[] row + movement kind), built once and reused for both the
 * per-material section and the pairwise join table below.
 */

typedef struct {
    const char* name;
    const reaction_t* r;
    material_kind_t kind;
    uint8_t self_id;  /* the plain material id this row's own material is
                        * (MAT_EXTENDED for every extended material, since
                        * they share that one id and have no plain id of
                        * their own) - see emit_ignite()'s self check,
                        * which needs this to tell "ignites into itself"
                        * (wood) apart from "ignites into a third thing" */
    uint8_t color_id; /* the raw byte a TARGET field would hold if it
                        * named this exact material - a plain id for an
                        * ordinary material, MATX(k) for an extended one.
                        * Unlike self_id above (MAT_EXTENDED for every
                        * extended material) this keeps k, because
                        * material_hex() needs the real swatch cell to
                        * look a colour up, not just which physics row the
                        * material shares - see material_hex()'s own
                        * comment. */
} mrow_t;

/* +1 for gunpowder: one row for all eight low-nibble codes 0xF8-0xFF, not
 * one per code - see build_rows()'s own comment on why. */
static mrow_t all_rows[(MAT_COUNT - 1) + MATERIAL_EXTENDED_COUNT + 1];
static size_t all_rows_count;

/* "#rrggbb" plus the NUL, the fixed width every material_hex() output (and
 * every buffer meant to hold one) needs. */
#define COLOR_LEN 8

static const mrow_t*
find_row(const char* name) {
    for (size_t i = 0; i < all_rows_count; i++) {
        if (strcmp(all_rows[i].name, name) == 0) {
            return &all_rows[i];
        }
    }
    fprintf(stderr, "dump_reactions: find_row(\"%s\") - no such material\n", name);
    exit(1);
}

/* The variant a freshly painted cell of `material` gets, mirrored from
 * random_cell() in sand.c - not independently reasoned about; check both
 * when touching either. random_cell() draws a random shade; this picks
 * the CENTRE of that range, since a fixed doc needs a fixed answer. A
 * variant means something different per material (stone/glass bank
 * TEMPERATURE, wood LIFE LEFT TO BURN, sand can mean cullet), so only
 * asking each material's own fields, like random_cell(), gives a
 * representative swatch. */
static uint8_t
representative_variant(material_id_t material) {
    /* A fresh liquid cell is a full one - see random_cell()'s own comment. */
    if (material_by_id(material)->kind == KIND_LIQUID) {
        return MASS_MAX;
    }
    /* A transient material starts at full life. */
    if (material_by_id(material)->decay != 0) {
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

    /* Picks the middle of the whole range instead of a point `band` could
     * equal. */
    const int span = MATERIAL_SHADE_SPAN(material);
    const int mid = span / 2;

    return (uint8_t)mid;
}

/* Writes v's colour as "#rrggbb" into buf. Reads material_palette() at the
 * representative swatch a fresh cell of this material actually takes
 * (representative_variant() above), not brush_color()'s "+13" -
 * deliberately different for materials whose variant isn't a plain shade.
 * Caller-owned buffer, not shared static: an anatomy example can need two
 * or more colours live at once (hardens_to and clings_to), and a shared
 * buffer would overwrite the earlier one. buf must be at least COLOR_LEN
 * bytes. */
static void
material_hex(uint8_t v, char* buf, size_t cap) {
    const cell_t base = (v >= (uint8_t)(MAT_EXTENDED << 4)) ? (cell_t)v : CELL_MAKE(v, 0);
    /* base >= (MAT_EXTENDED << 4), NOT cell_is_extended(base) - statics only.
     * Gunpowder byte already a resolved swatch. Testing fell through to
     * "ordinary material", incorrect colour. Confirmed by direct
     * instrumentation. */
    const cell_t swatch =
        (base >= (cell_t)(MAT_EXTENDED << 4))
            ? base
            : CELL_MAKE(CELL_MATERIAL(base), representative_variant((material_id_t)CELL_MATERIAL(base)));
    const gfx_color_t packed = material_palette()[swatch];
    /* gfx_color_t is RGB565 byte-swapped (GFX_RGB(), gfx_color.h); this
     * reverses that, then round-to-nearest (not a naive shift) so 0x1F
     * recovers as 0xFF, not 0xF8. */
    const uint16_t rgb565 = (uint16_t)((packed >> 8) | (packed << 8));
    const uint8_t r5 = (rgb565 >> 11) & 0x1Fu;
    const uint8_t g6 = (rgb565 >> 5) & 0x3Fu;
    const uint8_t b5 = rgb565 & 0x1Fu;
    const uint8_t r8 = (uint8_t)((r5 * 255 + 15) / 31);
    const uint8_t g8 = (uint8_t)((g6 * 255 + 31) / 63);
    const uint8_t b8 = (uint8_t)((b5 * 255 + 15) / 31);
    snprintf(buf, cap, "#%02X%02X%02X", r8, g8, b8);
}

/*
 * Legibility overrides. Several exact palette values fail a 3:1 WCAG
 * contrast floor against GitHub's light (#FFFFFF) and dark (#0D1117) page
 * backgrounds, so a name in the raw value can be unreadable in whichever
 * theme the reader is in. A startup check recomputes each raw value through
 * material_hex() and exit(1)s on a mismatch, so a palette change cannot
 * leave a stale override behind.
 */
typedef struct {
    uint8_t cell;        /* matched against mrow_t.color_id, same as
                          * ADVERB_EXCEPTIONS.cell */
    const char* raw;     /* material_hex()'s output for `cell` at the time
                          * this row was computed - the soundness check
                          * recomputes it live and compares */
    const char* legible; /* same hue/saturation, lightness bisected against
                          * both #FFFFFF and #0D1117 until each clears
                          * 3:1, moving the least distance needed (lift if
                          * too dark, darken if too pale) */
} legibility_override_t;

static const legibility_override_t LEGIBILITY_OVERRIDES[] = {
    {MAT_SAND, "#D6A663", "#C58834"},          {MAT_WATER, "#10416B", "#1863A3"},
    {MAT_GAS, "#CEEBBD", "#5CA532"},           {MAT_FIRE, "#FFE363", "#B19100"},
    {MAT_WOOD, "#5A3D21", "#825830"},          {MAT_STEAM, "#F7FBFF", "#2D96FF"},
    {MAT_OIL, "#101008", "#636331"},           {MAT_LAVA, "#8C1400", "#BF1B00"},
    {MAT_ACID, "#296908", "#2B6F08"},          {MAT_SNOW, "#E6EFF7", "#6099CC"},
    {MATX(MATX_ICE), "#B5E7F7", "#16A0CC"},    {MATX(MATX_PLANT), "#526529", "#54682A"},
    {MATX(MATX_LEAF), "#6BB23A", "#63A435"},   {MATX(MATX_ROOT), "#BDA68C", "#AC8F6F"},
    {GUNPOWDER_CELL(2), "#421408", "#B03515"},
};

/* legible_hex()'s search below - it is a linear scan of a table with
 * fifteen rows, not a lookup this program runs often enough to warrant
 * anything smarter. */
static const legibility_override_t*
legibility_override_for(uint8_t cell) {
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
static void
legible_hex(uint8_t v, char* buf, size_t cap) {
    const legibility_override_t* ov = legibility_override_for(v);
    if (ov != NULL) {
        snprintf(buf, cap, "%s", ov->legible);
        return;
    }
    material_hex(v, buf, cap);
}

static void
legibility_overrides_are_sound(void) {
    for (size_t i = 0; i < ARRAY_LEN(LEGIBILITY_OVERRIDES); i++) {
        const legibility_override_t* ov = &LEGIBILITY_OVERRIDES[i];
        const mrow_t* row = NULL;
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

#define MAT_SPAN_RING 8
#define MAT_SPAN_LEN  96
static char mat_span_bufs[MAT_SPAN_RING][MAT_SPAN_LEN];
static int mat_span_next = 0;

static const char*
mat_span(uint8_t v, const char* name) {
    char hex[COLOR_LEN];
    legible_hex(v, hex, sizeof(hex));
    char* buf = mat_span_bufs[mat_span_next];
    mat_span_next = (mat_span_next + 1) % MAT_SPAN_RING;
    snprintf(buf, MAT_SPAN_LEN, "$\\textcolor{%s}{\\text{%s}}$", hex, name);
    return buf;
}

/* mat_span(), naming the material through the same to_name()/prose_name()
 * pair every plain-text clause already used to print it - the common case
 * where the displayed name is exactly what a TARGET field's raw byte
 * decodes to, lowercased for prose. */
static const char*
mat_span_v(uint8_t v) {
    return mat_span(v, prose_name(to_name(v)));
}

/* Never returns an empty "****": a silent adverb returns "", full stop.
 * Ring-buffered for the same reason mat_span() is - emit_acid() alone needs
 * two rate words (dissolves and fizz) live in one sentence. */
#define RATE_GAP_RING 4
#define RATE_GAP_LEN  40
static char rate_gap_bufs[RATE_GAP_RING][RATE_GAP_LEN];
static int rate_gap_next = 0;

static const char*
rate_gap(const char* word) {
    if (word[0] == '\0') {
        return "";
    }
    char* buf = rate_gap_bufs[rate_gap_next];
    rate_gap_next = (rate_gap_next + 1) % RATE_GAP_RING;
    snprintf(buf, RATE_GAP_LEN, " **%s**", word);
    return buf;
}

static void
build_colored_list(bool (*pred)(const mrow_t*), const char* sep, char* out, size_t cap) {
    out[0] = '\0';
    bool first = true;
    for (size_t i = 0; i < all_rows_count; i++) {
        if (!pred(&all_rows[i])) {
            continue;
        }
        const char* span = mat_span(all_rows[i].color_id, prose_name(all_rows[i].name));
        size_t len = strlen(out);
        snprintf(out + len, cap - len, "%s%s", first ? "" : sep, span);
        first = false;
    }
    if (out[0] == '\0') {
        snprintf(out, cap, "(none)");
    }
}

static void
adverb_exceptions_are_sound(void) {
    for (size_t i = 0; i < ARRAY_LEN(ADVERB_EXCEPTIONS); i++) {
        const adverb_exception_t* e = &ADVERB_EXCEPTIONS[i];
        const field_doc_t* fd = field_doc(e->field); /* exits(1) itself if
                                    * the field is not a real field_docs[]
                                    * row */
        if (e->why == NULL || e->why[0] == '\0') {
            fprintf(stderr,
                    "dump_reactions: ADVERB_EXCEPTIONS[%zu] (%s) has no "
                    "`why` - every override needs the measurement that "
                    "justifies it\n",
                    i, e->field);
            exit(1);
        }
        const mrow_t* row = NULL;
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
        const uint8_t raw = *(const uint8_t*)((const unsigned char*)row->r + fd->offset);
        const char* would_be = adverb(e->field, raw);
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

static char wetting_liquids[256];

static char heat_sources[256];

/* Mirrors neighbor_quenches() in sand_reactions.c. Uses is_quenching_liquid()
 * for pairwise column. Water and Acid qualify; Oil is fuel, Lava is heat
 * source. Replaces hardcoded "a quenching liquid" phrase in emit_burn() with
 * derived list. */
static char quenching_liquids[256];

static char soil_names[256];

static void
build_rows(void) {
    all_rows_count = 0;
    for (uint8_t m = MAT_SAND; m < MAT_COUNT; m++) {
        all_rows[all_rows_count].name = material_by_id((material_id_t)m)->name;
        all_rows[all_rows_count].r = &reactions[m];
        all_rows[all_rows_count].kind = (material_kind_t)material_by_id((material_id_t)m)->kind;
        all_rows[all_rows_count].self_id = m;
        all_rows[all_rows_count].color_id = m;
        all_rows_count++;
    }
    for (uint8_t k = 0; k < MATERIAL_EXTENDED_COUNT; k++) {
        const char* nm = material_name(MATX(k));
        if (nm[0] == '?' && nm[1] == '\0') {
            continue; /* unnamed extended slot - material_name()'s own
                        * fallback for a row nobody has claimed yet */
        }
        all_rows[all_rows_count].name = nm;
        all_rows[all_rows_count].r = &extended_reactions[k];
        /* Every extended material shares MAT_EXTENDED's one physics row -
         * see material.h's own comment on why. */
        all_rows[all_rows_count].kind = (material_kind_t)material_by_id(MAT_EXTENDED)->kind;
        all_rows[all_rows_count].self_id = MAT_EXTENDED;
        all_rows[all_rows_count].color_id = MATX(k);
        all_rows_count++;
    }

    all_rows[all_rows_count].name = "Gunpowder";
    all_rows[all_rows_count].r = &extended_reactions[8];
    all_rows[all_rows_count].kind = (material_kind_t)material_of(GUNPOWDER_BASE)->kind;
    all_rows[all_rows_count].self_id = MAT_EXTENDED;
    all_rows[all_rows_count].color_id = GUNPOWDER_CELL(2);
    all_rows_count++;
}

static bool
row_is_empty(const reaction_t* r) {
    const unsigned char* bytes = (const unsigned char*)r;
    for (size_t i = 0; i < sizeof(*r); i++) {
        if (bytes[i] != 0) {
            return false;
        }
    }
    return true;
}

/*
 * Per-material clause emitters, one per group, called in group sort-key
 * order for every material row. Each one is a no-op unless its group's
 * driver field is nonzero, so a material with (say) no plant fields at
 * all costs nothing but a skipped comparison.
 */

static void
emit_ignite(const reaction_t* r, uint8_t self_id) {
    if (r->flammability == 0) {
        return;
    }

    const char* adv = adverb_child("flammability", r->flammability);

    /* ignites_to has three shapes (material.h's own comment; try_ignite()
     * matches). Each gets its own sentence, not one template: MAT_FIRE/0
     * default drops "becoming Fire" as redundant. Self (wood) - burning
     * is a STATE not a transformation, so the sentence avoids "becoming
     * Wood". A third material - unused today but a real shape (ash/coal).
     * Wording targets an early-elementary reader (child-reader persona);
     * each extra fact gets its own sentence, not a trailing "and", so
     * reading does not blur by the end. */
    if (r->ignites_to == 0 || r->ignites_to == MAT_FIRE) {
        printf("- *Catches* %s%s from %s.\n", mat_span_v(MAT_FIRE), rate_gap(adv), heat_sources);
    } else if (r->ignites_to == self_id) {
        printf("- *Catches* %s%s from %s.\n", mat_span_v(MAT_FIRE), rate_gap(adv), heat_sources);
        printf("- It keeps burning right where it is.\n");
    } else {
        printf("- *Catches* %s%s from %s.\n", mat_span_v(MAT_FIRE), rate_gap(adv), heat_sources);
        printf("- It *turns into* %s instead.\n", mat_span_v(r->ignites_to));
    }
    if (r->needs_air != 0) {
        printf("- But only where it can touch air.\n");
    }
}

static void
emit_burn(const reaction_t* r) {
    if (r->burns == 0 && r->burn_decay == 0) {
        return;
    }

    if (r->burns != 0) {
        printf("- Makes its own heat, all the time.\n");
    } else {
        printf("- Once it is on fire, it *burns down*%s.\n", rate_gap(adverb_child("burn_decay", r->burn_decay)));
    }
    if (r->explodes != 0) {
        /* Read only at burn-out, not at ignition. This handles the fuse
         * model, not just lighting. */
        printf("- Burning out, it *detonates* instead of simply going "
               "out - blasting a %u-cell radius, but only if it is one "
               "corner of a 2x2 that is still alight.\n",
               (unsigned)r->explodes);
    }
    if (r->flare != 0) {
        printf("- It *sets fire to* the empty spot next to it%s.\n", rate_gap(adverb_child("flare", r->flare)));
    }
    if (r->residue != 0) {
        printf("- It%s *leaves* %s when it burns out.\n", rate_gap(adverb_child("residue", r->residue)),
               mat_span_v(MAT_SMOKE));
    }

    /* Named quenching liquids, not generic "a quenching liquid".
     * quenching_liquids mirrors neighbor_quenches() (sand_reactions.c). */
    if (r->burns != 0) {
        const char* quenched = (r->quench_to != 0) ? mat_span_v(r->quench_to) : "nothing";
        printf("- If %s touches it, it *turns into* %s.\n", quenching_liquids, quenched);
    } else if (r->explodes != 0) {
        printf("- If %s touches it, it goes out - but stays soaked.\n", quenching_liquids);
    } else {
        printf("- If %s touches it, the fire just goes out.\n", quenching_liquids);
    }
}

static void
emit_transform(const reaction_t* r, uint8_t cell) {
    if (r->heats_to == 0) {
        return;
    }
    if (r->heat_ramp != 0) {
        /* Glass melts to lava; naming cause avoids tautology. */
        printf("- If %s stays next to it a long time, it *melts* into "
               "%s.\n",
               heat_sources, mat_span_v(r->heats_to));
    } else if (r->heat_chance == 0) {
        /* heats_to only via direct lava contact; root needed for fire
         * resistance yet lava vulnerability; see reaction_t.melts. Named
         * "lava" for specificity, not heat_sources. */
        if (r->melts == 0) {
            return;
        }
        printf("- If %s touches it, it *turns into* %s%s.\n", mat_span_v(MAT_LAVA), mat_span_v(r->heats_to),
               rate_gap(adverb_cell_child("melts", r->melts, cell)));
        /* "flame", not "fire" - keeps distinction clean */
        printf("- A flame alone will not do this. It has to be %s.\n", mat_span_v(MAT_LAVA));
    } else if (r->flaw_to != 0) {
        printf("- Next to %s, it%s *turns into* clumps of %s instead of "
               "%s.\n",
               heat_sources, rate_gap(adverb_child("flaw_chance", r->flaw_chance)), mat_span_v(r->flaw_to),
               mat_span_v(r->heats_to));
    } else if (r->heats_to == GUNPOWDER_LIT_CELL) {
        /* Heat alone lighting a fuse reads as catching light, the same verb
         * emit_ignite() already uses for the flame path - "melts into Lit
         * Gunpowder" described a substance change that never happens. */
        printf("- Next to %s, it%s *catches light* instead.\n", heat_sources,
               rate_gap(adverb_cell_child("heat_chance", r->heat_chance, cell)));
    } else {
        printf("- Next to %s, it%s *melts* into %s.\n", heat_sources,
               rate_gap(adverb_cell_child("heat_chance", r->heat_chance, cell)), mat_span_v(r->heats_to));
        if (r->melts != 0) {
            printf("- If %s touches it directly, it%s *melts* right away "
                   "instead.\n",
                   mat_span_v(MAT_LAVA), rate_gap(adverb_cell_child("melts", r->melts, cell)));
        }
    }
}

static void
emit_spoils(const reaction_t* r) {
    if (r->spoils_to == 0) {
        return;
    }
    printf("- It *turns into* %s %s.\n", mat_span_v(r->spoils_to), cause_marked("spoils_to", 0));
}

static void
emit_temperature(const reaction_t* r) {
    if (r->heat_ramp == 0 && r->conducts == 0) {
        return;
    }
    if (r->heat_ramp != 0) {
        printf("- It *holds heat*%s.\n", rate_gap(adverb_child("heat_ramp", r->heat_ramp)));
        if (r->conducts != 0) {
            printf("- It also *passes heat along*%s.\n", rate_gap(adverb_child("conducts", r->conducts)));
        }
        if (r->cools != 0) {
            printf("- Once nothing is heating it, it *cools back down*%s "
                   "again.\n",
                   rate_gap(adverb_child("cools", r->cools)));
        }
    } else {
        printf("- It *passes heat along*%s, but it never gets hot "
               "itself.\n",
               rate_gap(adverb_child("conducts", r->conducts)));
    }
}

/* Its own function: emit_temperature() is gated on heat_ramp/conducts and
 * water does neither, so it would be skipped. */
static void
emit_boils(const reaction_t* r) {
    if (r->boils == 0) {
        return;
    }
    const uint8_t boils_to = r->boils_to ? r->boils_to : MAT_STEAM;
    /* "Conducted heat reaches it" meant "it gets hot enough", whether that
     * heat arrived straight from a flame or through a conductor a few
     * cells thick - "gets hot enough" says the same thing without naming
     * the mechanism a five-year-old does not need. */
    printf("- Once it gets hot enough, it *boils* into %s%s.\n", mat_span_v(boils_to),
           rate_gap(adverb_child("boils", r->boils)));
}

static void
emit_cold(const reaction_t* r) {
    if (r->chills == 0) {
        return;
    }
    printf("- *Chills whatever it touches*%s.\n", rate_gap(adverb_child("chills", r->chills)));
}

static void
emit_warmth(const reaction_t* r) {
    if (r->warms == 0) {
        return;
    }
    printf("- *Warms whatever it touches*%s.\n", rate_gap(adverb_child("warms", r->warms)));
}

static void
emit_thaw(const reaction_t* r) {
    if (r->thaws == 0) {
        return;
    }
    if (r->heats_to != 0) {
        printf("- If anything wet touches it, it *melts*%s, turning into "
               "%s. Soaked ground counts, more slowly the drier it is.\n",
               rate_gap(adverb_child("thaws", r->thaws)), mat_span_v(r->heats_to));
    } else {
        printf("- If anything wet touches it, it *melts*%s.\n", rate_gap(adverb_child("thaws", r->thaws)));
    }
}

static void
emit_wet(const reaction_t* r, uint8_t cell) {
    if (r->wets != 0) {
        printf("- It *makes things wet*. Thirsty things will soak it "
               "up.\n");
    }
    if (r->soaks != 0) {
        /* `soaks` only ever fires beside a wetting liquid - see
         * sand_reactions.c's own soaking loop, which skips a neighbour
         * outright when reaction_of(n)->wets == 0 - so this names
         * wetting_liquids, not "any liquid" (that claim is `thaws`'s
         * alone; see this file's comment on wetting_liquids). */
        if (r->soaks_to != 0) {
            /* "once it takes a unit in" -> "once it has soaked up
             * enough" - `unit` is an internal accounting word with
             * nothing for a reader to picture; "enough" says the same
             * true thing (there is a threshold) without it. */
            printf("- It *soaks up* any %s it touches%s, and turns into "
                   "%s once it has soaked up enough - %s.\n",
                   wetting_liquids, rate_gap(adverb_child("soaks", r->soaks)), mat_span_v(r->soaks_to),
                   cause_marked("soaks_to", 0));
        } else {
            printf("- It *soaks up* any %s it touches%s, and gets "
                   "wetter.\n",
                   wetting_liquids, rate_gap(adverb_child("soaks", r->soaks)));
        }
    }
    if (r->dries != 0) {
        printf("- It *dries out*%s, all by itself.\n", rate_gap(adverb_child("dries", r->dries)));
    }
    if (r->soaked_to != 0) {
        printf("- Once it is fully soaked, it *turns into* %s%s.\n", mat_span_v(r->soaked_to),
               rate_gap(adverb_cell_child("soaked_chance", r->soaked_chance, cell)));
    }
}

static void
emit_acid(const reaction_t* r) {
    if (r->dissolves != 0) {
        printf("- It%s *eats through* whatever is next to it.\n", rate_gap(adverb_child("dissolves", r->dissolves)));
        if (r->fizz != 0) {
            printf("- It%s *leaves* %s behind when it does.\n", rate_gap(adverb_child("fizz", r->fizz)),
                   mat_span_v(MAT_SMOKE));
        }
    }
    if (r->dissolvable != 0) {
        printf("- It *gives in to* %s%s.\n", mat_span_v(MAT_ACID),
               rate_gap(adverb_child("dissolvable", r->dissolvable)));
    }
}

static void
emit_evaporates(const reaction_t* r) {
    if (r->evaporates == 0) {
        return;
    }
    printf("- It *turns into* %s%s all by itself.\n", mat_span_v(MAT_GAS),
           rate_gap(adverb_child("evaporates", r->evaporates)));
}

static void
emit_condense(const reaction_t* r) {
    if (r->condenses == 0 || r->condenses_to == 0) {
        return;
    }
    printf("- When enough of it comes together in one spot, it%s *turns "
           "into* %s.\n",
           rate_gap(adverb_child("condenses", r->condenses)), mat_span_v(r->condenses_to));
}

static void
emit_grow(const reaction_t* r) {
    if (r->grows != 0) {
        printf("- It *grows up* into wet %s%s.\n", soil_names, rate_gap(adverb_child("grows", r->grows)));
        printf("- Growing uses up a bit of the %s's water.\n", soil_names);
    }
    if (r->falls != 0) {
        printf("- It *falls down*%s when there is empty space below it.\n", rate_gap(adverb_child("falls", r->falls)));
    }
}

static void
emit_harden(const reaction_t* r) {
    if (r->hardens_to == 0) {
        return;
    }
    printf("- If it grows straight for %u spots in a row, it%s *turns "
           "into* %s.\n",
           (unsigned)r->harden_run, rate_gap(adverb_child("harden_chance", r->harden_chance)),
           mat_span_v(r->hardens_to));
    if (r->trunk_girth != 0) {
        printf("- The bottom can be up to %u spots wider than the top.\n", (unsigned)r->trunk_girth);
    }
    if (r->holds_line != 0) {
        printf("- Its branches%s keep growing the same way they started, "
               "instead of curving back down.\n",
               rate_gap(adverb_child("holds_line", r->holds_line)));
    }
    if (r->clings_to != 0) {
        printf("- Once it turns hard, it *becomes part of* %s too.\n", mat_span_v(r->clings_to));
    }
    if (r->canopy != 0 && r->canopy_to != 0) {
        printf("- As soon as it turns hard, it also%s *grows* %s on "
               "top.\n",
               rate_gap(adverb_child("canopy", r->canopy)), mat_span_v(r->canopy_to));
    }
}

static void
emit_regrow(const reaction_t* r, uint8_t cell) {
    if (r->sprouts != 0 && r->sprouts_to != 0) {
        printf("- If it is standing in wet %s, it%s *sprouts* %s next to "
               "itself.\n",
               soil_names, rate_gap(adverb_child("sprouts", r->sprouts)), mat_span_v(r->sprouts_to));
    }
    if (r->buds != 0 && r->buds_to != 0) {
        printf("- Once it has leaves and can reach water, it%s *buds* "
               "new %s next to itself.\n",
               rate_gap(adverb_child("buds", r->buds)), mat_span_v(r->buds_to));
    }
    if (r->drinks != 0) {
        /* Same wetting-liquids gate as `soaks` - requires
         * reaction_of(n)->wets != 0 on neighbour. Specifies wetting_liquids,
         * not generic "a liquid". Simplified from earlier version naming
         * unobservable internal gate. */
        printf("- If %s touches it, it%s *sends the water down* to %s at "
               "its roots.\n",
               wetting_liquids, rate_gap(adverb_child("drinks", r->drinks)), soil_names);
    }
    if (r->roots != 0 && r->roots_to != 0) {
        /* roots differs by row type; see field_docs[] and reaction_t.roots.
         * GROWER spends soil moisture to grow/bud/sprout. ROOT spreads into
         * moist soil. Old template incorrectly applied GROWER logic to ROOT.
         * Now uses cell id to differentiate. */
        if (cell == MATX(MATX_ROOT)) {
            printf("- If it touches wet %s, it%s *turns* that spot into "
                   "more %s.\n",
                   soil_names, rate_gap(adverb_child("roots", r->roots)), mat_span_v(r->roots_to));
        } else {
            printf("- Growing new parts uses up some of the %s's water.\n", soil_names);
            printf("- When that happens, it%s *turns* the spot under it "
                   "into %s.\n",
                   rate_gap(adverb_child("roots", r->roots)), mat_span_v(r->roots_to));
        }
    }
}

static void
emit_shatter(const reaction_t* r) {
    if (r->shatters_to == 0) {
        return;
    }
    printf("- It *shatters* into %s %s.\n", mat_span_v(r->shatters_to), cause_marked("shatters_to", 0));
}

/* CRUST_ROLL_MAX, not the usual 256, and not a literal either - the
 * denominator has already moved once, and printing a stale one states the
 * wrong probability rather than merely reading oddly. */
static void
emit_crust(const reaction_t* r) {
    if (r->crusts == 0 || r->crusts_to == 0) {
        return;
    }
    printf("- Once it has *settled*, it slowly crusts into %s "
           "(%u in %u a step, and only while at rest).\n",
           mat_span_v(r->crusts_to), (unsigned)r->crusts, (unsigned)CRUST_ROLL_MAX);
}

static void
emit_material_section(const char* name, const reaction_t* r, uint8_t self_id, uint8_t color_id) {
    if (row_is_empty(r)) {
        return;
    }
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
    emit_crust(r);
    emit_wet(r, color_id);
    emit_acid(r);
    emit_evaporates(r);
    emit_condense(r);
    emit_grow(r);
    emit_harden(r);
    emit_regrow(r, color_id);
    emit_shatter(r);
}

/*
 * The pairwise A + B -> C table.
 *
 * Keyed on the RATE field and branching on the target - never the other
 * way around. Walking `*_to` fields and asking what triggers them would
 * miss every reaction whose target IS its own row (dirt getting wetter)
 * or a third cell entirely (a plant drinking) - see the plan's "The
 * pairwise join is phase 1, not phase 2" section, which is the reason
 * this table exists at all rather than being deferred.
 */

static bool
is_burning_material(const reaction_t* r) {
    return r->burns != 0;
}

/* Mirrors neighbor_quenches() (sand_reactions.c) exactly: a liquid quenches
 * iff it is neither fuel nor itself a heat source. Not a magic threshold -
 * both fields it reads are already in the table this program links. */
static bool
is_quenching_liquid(const mrow_t* row) {
    return row->kind == KIND_LIQUID && row->r->flammability == 0 && row->r->burns == 0;
}

static void
join_names(bool (*pred)(const mrow_t*), const char* sep, char* out, size_t cap) {
    out[0] = '\0';
    bool first = true;
    for (size_t i = 0; i < all_rows_count; i++) {
        if (!pred(&all_rows[i])) {
            continue;
        }
        size_t len = strlen(out);
        int n = snprintf(out + len, cap - len, "%s%s", first ? "" : sep, all_rows[i].name);
        (void)n;
        first = false;
    }
    if (out[0] == '\0') {
        snprintf(out, cap, "(none)");
    }
}

static bool
pred_burns(const mrow_t* row) {
    return row->r->burns != 0;
}

static bool
pred_kind_liquid(const mrow_t* row) {
    return row->kind == KIND_LIQUID;
}

static bool
pred_wets_liquid(const mrow_t* row) {
    return row->kind == KIND_LIQUID && row->r->wets != 0;
}

/* `soil != 0` (reaction_t.soil, material.h) - GUNPOWDER IS NOT SOIL (D1,
 * GUNPOWDER_FIXES.md sec 7): `dries` meant "plantable ground". Now gunpowder
 * sets `dries`, so `soil` checks for plant/root sites. `soil` is only set by
 * dirt. */
static bool
pred_soil(const mrow_t* row) {
    return row->r->soil != 0;
}

static bool
pred_needs_legibility_override(const mrow_t* row) {
    return legibility_override_for(row->color_id) != NULL;
}

static bool
pred_exact_color(const mrow_t* row) {
    return legibility_override_for(row->color_id) == NULL;
}

/* Key to coloured names using legible_hex(), not raw device values. */
static void
emit_legend(void) {
    printf("\n## Legend\n\n");
    printf("Every material name below is coloured - this list doubles as "
           "the key: whatever colour a name gets here is the colour that "
           "same name gets in every bullet that follows.\n\n");

    for (size_t i = 0; i < all_rows_count; i++) {
        printf("%s%s", mat_span_v(all_rows[i].color_id), (i + 1 < all_rows_count) ? ", " : "\n\n");
    }

    char adjusted[256];
    char exact[256];
    join_names(pred_needs_legibility_override, ", ", adjusted, sizeof(adjusted));
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
           "in as this page's key.\n\n",
           adjusted, exact);

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

static void
print_join_row(const char* a, const char* b, const char* becomes, const char* rate, const char* note) {
    printf("| %s | %s | %s | %s | %s |\n", a, b, becomes, rate, note);
}

/* Blank TABLE CELL cannot mean "ordinary speed" - it reads as "nobody filled
 * this in". Use em dash instead. */
static const char*
table_rate(const char* word) {
    return (word[0] == '\0') ? "—" : word;
}

static void
emit_pairwise_table(void) {
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

    for (size_t i = 0; i < all_rows_count; i++) {
        if (all_rows[i].r->dissolves == 0) {
            continue;
        }
        for (size_t j = 0; j < all_rows_count; j++) {
            if (all_rows[j].r->dissolvable == 0) {
                continue;
            }
            if (all_rows[j].self_id == MAT_WATER || all_rows[j].self_id == MAT_OIL) {
                continue;
            }
            char becomes[64];
            if (all_rows[i].r->fizz != 0) {
                /* `fizz` is FCHANCE - never silent (see frequency_words[]'s
                 * own comment on why chance-scale fields always print a
                 * word) - so adverb() always has something real to say
                 * here. */
                snprintf(becomes, sizeof(becomes), "nothing (%s smoke)", adverb("fizz", all_rows[i].r->fizz));
            } else {
                snprintf(becomes, sizeof(becomes), "nothing");
            }
            char rate[64];
            snprintf(rate, sizeof(rate), "%s / %s", table_rate(adverb("dissolves", all_rows[i].r->dissolves)),
                     table_rate(adverb("dissolvable", all_rows[j].r->dissolvable)));
            print_join_row(all_rows[i].name, all_rows[j].name, becomes, rate, "both rolls must pass");
        }
    }

    /* Acid|Water and Acid|Oil (bd esp32c6-c3r): neither is a dissolve past
     * the shared gate above, so hand-written here rather than walked - but
     * every number comes from sand.h's own #defines, not typed twice. */
    {
        const mrow_t* acid = find_row("Acid");
        const mrow_t* water = find_row("Water");
        char rate[64];
        snprintf(rate, sizeof(rate), "%s / %s", table_rate(adverb("dissolves", acid->r->dissolves)),
                 table_rate(adverb("dissolvable", water->r->dissolvable)));

        /* step_one_dissolver_cell()'s MAT_WATER branch: one roll, three
         * outcomes. water_wins/acid_wins is the unbiased baseline -
         * SAND_ACID_DILUTE_MASS_BIAS shifts it by local backing at runtime. */
        const int evaporate = SAND_ACID_DILUTE_EVAPORATE_CHANCE;
        const int water_wins = SAND_ACID_DILUTE_TO_WATER_CHANCE;
        const int acid_wins = 256 - evaporate - water_wins;
        char note[256];
        snprintf(note, sizeof(note),
                 "one roll: %d/256 Acid alone boils to Gas; else "
                 "%d/256 baseline Acid->Water & Water->Steam, or %d/256 "
                 "baseline Acid->Gas & Water->Acid - shifted by local "
                 "backing (sand_set_acid_dilute_mass_bias())",
                 evaporate, water_wins, acid_wins);
        print_join_row("Acid", "Water", "Gas, or swaps identity with Water", rate, note);

        /* MAT_OIL branch: two INDEPENDENT rolls, not the paired
         * dissolves/fizz roll the generic join above prints - Oil's own
         * fate and Acid's own fate are decided separately. */
        const mrow_t* oil = find_row("Oil");
        snprintf(rate, sizeof(rate), "%s / %s", table_rate(adverb("dissolves", acid->r->dissolves)),
                 table_rate(adverb("dissolvable", oil->r->dissolvable)));
        const int oil_to_gas = SAND_ACID_OIL_TO_GAS_CHANCE;
        const int acid_dies = SAND_ACID_OIL_DEATH_CHANCE;
        snprintf(note, sizeof(note),
                 "two independent rolls: %d/256 Oil becomes Gas (else "
                 "Acid); separately %d/256 the Acid cell dies outright "
                 "(else it survives and pays a quench cost)",
                 oil_to_gas, acid_dies);
        print_join_row("Acid", "Oil", "Oil becomes Gas or Acid; Acid may die too", rate, note);
    }

    for (size_t i = 0; i < all_rows_count; i++) {
        if (all_rows[i].r->flammability == 0) {
            continue;
        }
        char becomes[64];
        if (all_rows[i].r->ignites_to == 0 || all_rows[i].r->ignites_to == MAT_FIRE) {
            snprintf(becomes, sizeof(becomes), "Fire");
        } else if (all_rows[i].r->ignites_to == all_rows[i].self_id) {
            snprintf(becomes, sizeof(becomes), "%s, alight", all_rows[i].name);
        } else {
            snprintf(becomes, sizeof(becomes), "%s", to_name(all_rows[i].r->ignites_to));
        }
        print_join_row(all_rows[i].name, burners, becomes,
                       table_rate(adverb("flammability", all_rows[i].r->flammability)),
                       all_rows[i].r->needs_air ? "only where it touches air" : "");
    }

    /* heats_to x burns (memoryless and ramped both go through the same
     * try_heat_transform() trigger - contact with a burning cell, or
     * through a conductor) */
    for (size_t i = 0; i < all_rows_count; i++) {
        if (all_rows[i].r->heats_to == 0) {
            continue;
        }
        char rate[64];
        /* melts is direct contact with LAVA; see reaction_t.melts. */
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
            snprintf(rate, sizeof(rate), "%s", table_rate(adverb("heat_chance", all_rows[i].r->heat_chance)));
        }
        print_join_row(all_rows[i].name, burners, to_name(all_rows[i].r->heats_to), rate, "or through a conductor");
    }

    /* quench_to x quenching liquids */
    for (size_t i = 0; i < all_rows_count; i++) {
        if (!is_burning_material(all_rows[i].r) && all_rows[i].r->burn_decay == 0) {
            continue;
        }
        char qbuf[256];
        join_names(is_quenching_liquid, " / ", qbuf, sizeof(qbuf));
        if (all_rows[i].r->burn_decay != 0) {
            const char* becomes = (all_rows[i].r->explodes != 0) ? "itself, soaked" : "itself, unlit";
            print_join_row(all_rows[i].name, qbuf, becomes, "on contact", "quench_to is not read on this path");
        } else {
            const char* becomes = (all_rows[i].r->quench_to != 0) ? to_name(all_rows[i].r->quench_to) : "nothing";
            print_join_row(all_rows[i].name, qbuf, becomes, "on contact", "");
        }
    }

    /* chills x heat_ramp/shatters_to */
    for (size_t i = 0; i < all_rows_count; i++) {
        if (all_rows[i].r->chills == 0) {
            continue;
        }
        for (size_t j = 0; j < all_rows_count; j++) {
            if (all_rows[j].r->heat_ramp == 0) {
                continue;
            }
            char becomes[64];
            snprintf(becomes, sizeof(becomes), "%s, one heat level cooler", all_rows[j].name);
            print_join_row(all_rows[i].name, all_rows[j].name, becomes,
                           table_rate(adverb("chills", all_rows[i].r->chills)), "");
            if (all_rows[j].r->shatters_to != 0) {
                /* Index 1: sand_reactions.c's SECOND shatters_to
                 * REACTION_DOC(), at step_one_cold_cell()'s SAND_SHOCK_HEAT
                 * check - "if chilled while hot", the direction this row
                 * itself is walking (A chills B). Index 0 belongs to
                 * emit_shatter() instead; see that function's own comment. */
                print_join_row(all_rows[i].name, all_rows[j].name, to_name(all_rows[j].r->shatters_to),
                               cause_at("shatters_to", 1), "only if B is hot enough when A touches it");
            }
        }
    }

    /* wets x soaks - the wetting family. Four reactions, not a loop over
     * *_to: see this file's top comment and the plan's own section on it. */
    for (size_t i = 0; i < all_rows_count; i++) {
        if (all_rows[i].r->wets == 0) {
            continue;
        }
        for (size_t j = 0; j < all_rows_count; j++) {
            if (all_rows[j].r->soaks == 0) {
                continue;
            }
            if (all_rows[j].r->soaks_to != 0) {
                print_join_row(all_rows[i].name, all_rows[j].name, to_name(all_rows[j].r->soaks_to),
                               table_rate(adverb("soaks", all_rows[j].r->soaks)),
                               "the liquid pays a unit of its own mass");
            } else {
                char becomes[64];
                snprintf(becomes, sizeof(becomes), "%s, +1 moisture", all_rows[j].name);
                print_join_row(all_rows[i].name, all_rows[j].name, becomes,
                               table_rate(adverb("soaks", all_rows[j].r->soaks)), "no material change - soaks_to is 0");
            }
        }
        /* drinks: a THIRD cell changes (dirt at the root), not the
         * subject and not the liquid - see reaction_t.drinks. "dirt", not
         * the old hardcoded "soil" - see pred_soil()'s own comment. */
        for (size_t j = 0; j < all_rows_count; j++) {
            if (all_rows[j].r->drinks == 0) {
                continue;
            }
            print_join_row(all_rows[i].name, all_rows[j].name, "the dirt at B's root, +1 moisture",
                           table_rate(adverb("drinks", all_rows[j].r->drinks)),
                           "B itself is unchanged - a third cell changes");
        }
    }
    /* dries: self-driven, no partner at all. */
    for (size_t i = 0; i < all_rows_count; i++) {
        if (all_rows[i].r->dries == 0) {
            continue;
        }
        char becomes[64];
        snprintf(becomes, sizeof(becomes), "%s, -1 moisture", all_rows[i].name);
        print_join_row(all_rows[i].name, "(none - self-driven)", becomes,
                       table_rate(adverb("dries", all_rows[i].r->dries)), "");
    }

    /* thaws x any KIND_LIQUID */
    for (size_t i = 0; i < all_rows_count; i++) {
        if (all_rows[i].r->thaws == 0) {
            continue;
        }
        print_join_row(all_rows[i].name, liquids, to_name(all_rows[i].r->heats_to),
                       table_rate(adverb("thaws", all_rows[i].r->thaws)), "any liquid counts, not water alone");
    }
}

/*
 * "How these sentences are built" - one marked example per group_id_t. The
 * pairwise table above stays unmarked prose, being the deliverable a
 * brush-description feature reads.
 *
 * GLUE is the one slot that can go stale: it is typed into an emit_*()
 * printf and transcribed here by hand, so each example names the function
 * it mirrors. Colour spans use single-dollar inline math - display math
 * ($$) is a block element and shreds the sentence.
 */

typedef enum {
    MARK_NONE,     /* glue (see the Legend below) - printed as ordinary
                    * markdown text, in the reader's own theme colour. */
    MARK_MATERIAL, /* a material's own name - colour is per-INSTANCE, not
                    * per-slot: each segment carries its own hex in
                    * seg_t.color, computed by material_hex() below, so two
                    * materials in one sentence never share a colour.
                    * Colour means exactly one thing on this page - "this
                    * word is a material" - so this is the only mark that
                    * still carries one; see COLOUR MEANS MATERIAL,
                    * NOTHING ELSE above. */
    MARK_VERB,     /* the action - *italic*, see print_marked() below and
                    * the Legend this file prints. */
    MARK_RATE,     /* rate / frequency - **bold**. */
    MARK_CAUSE,    /* a trigger clause - ***bold italic***. */
} mark_t;

typedef struct {
    mark_t mark;
    const char* color; /* only meaningful when mark == MARK_MATERIAL - the
                        * only mark that carries a colour at all (see
                        * mark_t's own comment). NULL for every other mark:
                        * MARK_VERB/MARK_RATE/MARK_CAUSE render as markdown
                        * emphasis instead (see print_marked() below), and
                        * MARK_NONE is unmarked glue. */
    const char* text;
} seg_t;

/* Never a pointer into prose_name() or material_hex() buffers; one shared
 * buffer is unsafe. */
typedef struct {
    char name[32];
    char color[COLOR_LEN];
} list_item_t;

/* Comfortably more than heat_sources's or wetting_liquids's current
 * membership (2 and 1) - see those variables' own top comments: nothing
 * here assumes a fixed count, this is just a generous static bound so
 * collect_material_list() needs no allocation. */
#define LIST_ITEM_MAX 8

/* Fills items[] with selected all_rows[] entries, keeping names and colours.
 * Returns count. */
static size_t
collect_material_list(bool (*pred)(const mrow_t*), list_item_t* items, size_t cap) {
    size_t count = 0;
    for (size_t i = 0; i < all_rows_count && count < cap; i++) {
        if (!pred(&all_rows[i])) {
            continue;
        }
        snprintf(items[count].name, sizeof(items[count].name), "%s", prose_name(all_rows[i].name));
        material_hex(all_rows[i].color_id, items[count].color, sizeof(items[count].color));
        count++;
    }
    return count;
}

/* Generous headroom over the longest example built below (GRP_HARDEN,
 * eleven segments) so every seg_t[] here is a plain fixed-size local
 * array rather than anything allocated. */
#define SEG_MAX 32

static void
seg_glue(seg_t* segs, size_t* n, const char* text) {
    segs[(*n)++] = (seg_t){MARK_NONE, NULL, text};
}

static void
seg_mark(seg_t* segs, size_t* n, mark_t mark, const char* text) {
    segs[(*n)++] = (seg_t){mark, NULL, text};
}

static void
seg_rate_gap(seg_t* segs, size_t* n, const char* word) {
    if (word[0] == '\0') {
        return;
    }
    seg_glue(segs, n, " ");
    seg_mark(segs, n, MARK_RATE, word);
}

/* A single material-name segment, coloured by its own already-computed hex
 * (see material_hex()) rather than a shared slot colour. */
static void
seg_material(seg_t* segs, size_t* n, const char* color, const char* text) {
    segs[(*n)++] = (seg_t){MARK_MATERIAL, color, text};
}

/* A whole collect_material_list() result, each member its own coloured
 * segment, sep printed as unmarked glue between members and never after
 * the last one - the same shape join_names() joins into one string, kept
 * apart here instead of flattened (see collect_material_list()'s own
 * comment). */
static void
seg_list(seg_t* segs, size_t* n, const list_item_t* items, size_t count, const char* sep) {
    for (size_t i = 0; i < count; i++) {
        if (i > 0) {
            seg_glue(segs, n, sep);
        }
        seg_material(segs, n, items[i].color, items[i].name);
    }
}

static void
print_plain(const seg_t* segs, size_t n) {
    for (size_t i = 0; i < n; i++) {
        printf("%s", segs[i].text);
    }
    printf("\n");
}

/* Every seg_t[] built in this file must keep a glue character (a space, a
 * comma-space, ...) between two adjacent marked segments - two markers
 * with nothing between them (e.g. *word***word2**) do not parse as
 * separate spans in most markdown renderers. */
static void
print_marked(const seg_t* segs, size_t n) {
    for (size_t i = 0; i < n; i++) {
        switch (segs[i].mark) {
            case MARK_NONE: printf("%s", segs[i].text); break;
            case MARK_MATERIAL: printf("$\\textcolor{%s}{\\text{%s}}$", segs[i].color, segs[i].text); break;
            case MARK_VERB: printf("*%s*", segs[i].text); break;
            case MARK_RATE: printf("**%s**", segs[i].text); break;
            case MARK_CAUSE: printf("***%s***", segs[i].text); break;
        }
    }
    printf("\n");
}

/* Plain sentence first, then the marked form immediately after - so the
 * section stays legible without a renderer (a raw diff, an editor, CI)
 * and not only on github.com. */
static void
print_example(const char* heading, const seg_t* segs, size_t n) {
    printf("\n**%s**\n\n", heading);
    print_plain(segs, n);
    printf("\n");
    print_marked(segs, n);
    printf("\n");
}

static void
emit_anatomy(void) {
    printf("\n## How these sentences are built\n\n");
    printf("Not a markup pass on the table above - that table stays as "
           "clean prose, unedited (see docs/plans/Reaction-Doc-Generator-"
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
        printf("$\\textcolor{%s}{\\text{%s}}$%s", hex, all_rows[i].name, (i + 1 < all_rows_count) ? ", " : "\n\n");
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

    /* GRP_IGNITE - emit_ignite(): flammability, ignites_to == self_id,
     * heat_sources. See emit_ignite() comment for "fire" and MAT_FIRE colour. */
    {
        const mrow_t* row = find_row("Wood");
        char subject[COLOR_LEN];
        char fire[COLOR_LEN];
        material_hex(row->color_id, subject, sizeof(subject));
        material_hex(MAT_FIRE, fire, sizeof(fire));
        list_item_t heat[LIST_ITEM_MAX];
        const size_t heat_n = collect_material_list(pred_burns, heat, LIST_ITEM_MAX);

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
                      "heat_sources (emit_ignite)",
                      segs, n);
    }

    /* GRP_BURN - emit_burn(), the `burns` branch: residue, quench_to,
     * quenching_liquids (derived, not a single field - see emit_burn()'s
     * own comment on why "a quenching liquid" was replaced with the real,
     * derived list). */
    {
        const mrow_t* row = find_row("Fire");
        char subject[COLOR_LEN];
        material_hex(row->color_id, subject, sizeof(subject));
        char quench_name[32];
        char quench_color[COLOR_LEN];
        snprintf(quench_name, sizeof(quench_name), "%s", prose_name(to_name(row->r->quench_to)));
        material_hex(row->r->quench_to, quench_color, sizeof(quench_color));
        list_item_t quench[LIST_ITEM_MAX];
        const size_t quench_n = collect_material_list(is_quenching_liquid, quench, LIST_ITEM_MAX);
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
                      "(emit_burn)",
                      segs, n);
    }

    /* GRP_TRANSFORM - emit_transform(), the rolled (not banked) branch:
     * heats_to, heat_chance, heat_sources. */
    {
        const mrow_t* row = find_row("Sand");
        char subject[COLOR_LEN];
        material_hex(row->color_id, subject, sizeof(subject));
        list_item_t heat[LIST_ITEM_MAX];
        const size_t heat_n = collect_material_list(pred_burns, heat, LIST_ITEM_MAX);
        char heats_name[32];
        char heats_color[COLOR_LEN];
        snprintf(heats_name, sizeof(heats_name), "%s", prose_name(to_name(row->r->heats_to)));
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
        seg_rate_gap(segs, &n, adverb_cell("heat_chance", row->r->heat_chance, row->color_id));
        seg_glue(segs, &n, ".");
        print_example("Transform - GRP_TRANSFORM: heats_to, heat_chance, "
                      "heat_sources (emit_transform)",
                      segs, n);
    }

    /* GRP_TEMPERATURE - emit_temperature(), the no-ramp branch: conducts
     * alone. */
    {
        const mrow_t* row = find_row("Metal");
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
                      "(emit_temperature)",
                      segs, n);
    }

    /* GRP_COLD - emit_cold(): chills. */
    {
        const mrow_t* row = find_row("Ice");
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
        const mrow_t* row = find_row("Steam");
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
        const mrow_t* row = find_row("Snow");
        char subject[COLOR_LEN];
        material_hex(row->color_id, subject, sizeof(subject));
        char heats_name[32];
        char heats_color[COLOR_LEN];
        snprintf(heats_name, sizeof(heats_name), "%s", prose_name(to_name(row->r->heats_to)));
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
        print_example("Thaw - GRP_THAW: thaws, heats_to (emit_thaw)", segs, n);
    }

    /* GRP_WET - emit_wet(), the soaks_to != 0 branch: soaks, soaks_to,
     * and wetting_liquids (derived, not a single field). */
    {
        const mrow_t* row = find_row("Sand");
        char subject[COLOR_LEN];
        material_hex(row->color_id, subject, sizeof(subject));
        list_item_t wet[LIST_ITEM_MAX];
        const size_t wet_n = collect_material_list(pred_wets_liquid, wet, LIST_ITEM_MAX);
        char soaks_name[32];
        char soaks_color[COLOR_LEN];
        snprintf(soaks_name, sizeof(soaks_name), "%s", prose_name(to_name(row->r->soaks_to)));
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
        seg_glue(segs, &n, " once it takes a unit in - ");
        seg_mark(segs, &n, MARK_CAUSE, cause_at("soaks_to", 0));
        seg_glue(segs, &n, ".");
        print_example("Wet - GRP_WET: soaks, soaks_to, wetting_liquids "
                      "(emit_wet)",
                      segs, n);
    }

    /* GRP_ACID - emit_acid(), the dissolves branch: dissolves (a genuine
     * rate) and fizz (a one-shot chance) side by side - two different
     * ladders, one slot marker. Neither field names a material, so this
     * example has no MARK_MATERIAL segment beyond the subject. */
    {
        const mrow_t* row = find_row("Acid");
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
        print_example("Acid - GRP_ACID: dissolves, fizz (emit_acid)", segs, n);
    }

    /* GRP_GROW - emit_grow(), the grows branch. "Wet DIRT", not the old
     * hardcoded "wet soil" - see pred_soil()'s own comment and this
     * file's emit_grow(). Dirt is itself a material name now, so it gets
     * its own MARK_MATERIAL segment rather than folding into the verb
     * phrase the way "wet soil" once did. */
    {
        const mrow_t* row = find_row("Plant");
        const mrow_t* dirt = find_row("Dirt");
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

    {
        const mrow_t* row = find_row("Plant");
        char subject[COLOR_LEN];
        material_hex(row->color_id, subject, sizeof(subject));
        char hardens_to[32];
        char hardens_color[COLOR_LEN];
        char clings_to[32];
        char clings_color[COLOR_LEN];
        snprintf(hardens_to, sizeof(hardens_to), "%s", prose_name(to_name(row->r->hardens_to)));
        material_hex(row->r->hardens_to, hardens_color, sizeof(hardens_color));
        snprintf(clings_to, sizeof(clings_to), "%s", prose_name(to_name(row->r->clings_to)));
        material_hex(row->r->clings_to, clings_color, sizeof(clings_color));

        seg_t segs[SEG_MAX];
        size_t n = 0;
        seg_material(segs, &n, subject, "Plant");
        seg_glue(segs, &n, ": A straight run of 6 cells ");
        seg_mark(segs, &n, MARK_RATE, adverb("harden_chance", row->r->harden_chance));
        seg_glue(segs, &n, " ");
        seg_mark(segs, &n, MARK_VERB, "hardens");
        seg_glue(segs, &n, " into ");
        seg_material(segs, &n, hardens_color, hardens_to);
        seg_glue(segs, &n,
                 ", up to 2 cells wider at the foot than at the "
                 "tip, and ");
        seg_mark(segs, &n, MARK_RATE, adverb("holds_line", row->r->holds_line));
        seg_glue(segs, &n,
                 " a limb holds its own direction (rather than "
                 "bending back toward gravity); the hardened body counts "
                 "as part of ");
        seg_material(segs, &n, clings_color, clings_to);
        seg_glue(segs, &n, ".");
        print_example("Harden - GRP_HARDEN: harden_chance, hardens_to, "
                      "holds_line, clings_to (emit_harden)",
                      segs, n);
    }

    /* GRP_REGROW - emit_regrow(), the sprouts branch: sprouts, sprouts_to.
     * "Wet DIRT", not the old hardcoded "wet soil" - see the GRP_GROW
     * example just above for the same fix, and pred_soil()'s comment. */
    {
        const mrow_t* row = find_row("Wood");
        const mrow_t* dirt = find_row("Dirt");
        char subject[COLOR_LEN];
        char dirt_color[COLOR_LEN];
        material_hex(row->color_id, subject, sizeof(subject));
        material_hex(dirt->color_id, dirt_color, sizeof(dirt_color));
        char sprouts_name[32];
        char sprouts_color[COLOR_LEN];
        snprintf(sprouts_name, sizeof(sprouts_name), "%s", prose_name(to_name(row->r->sprouts_to)));
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
                      "(emit_regrow)",
                      segs, n);
    }

    /* GRP_SHATTER - emit_shatter(): shatters_to, and MARK_CAUSE - one of
     * two per-material clauses that ever print it (emit_spoils() is the
     * other). Index 0, matching emit_shatter()'s own call - see that
     * function's comment. */
    {
        const mrow_t* row = find_row("Glass");
        char subject[COLOR_LEN];
        material_hex(row->color_id, subject, sizeof(subject));
        char shatters_name[32];
        char shatters_color[COLOR_LEN];
        snprintf(shatters_name, sizeof(shatters_name), "%s", prose_name(to_name(row->r->shatters_to)));
        material_hex(row->r->shatters_to, shatters_color, sizeof(shatters_color));

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
        print_example("Shatter - GRP_SHATTER: shatters_to (emit_shatter)", segs, n);
    }
}

/* main */

int
main(int argc, char** argv) {
    if (argc < 2) {
        fprintf(stderr,
                "usage: %s <path/to/sand_reactions.c> "
                "[<path/to/sand_plants.c> ...]\n",
                (argc > 0) ? argv[0] : "dump_reactions");
        fprintf(stderr, "  Each argument is read as TEXT, never linked - see "
                        "reaction_doc.h and this file's own parse_reaction_docs() "
                        "for why. sand_reactions.c and sand_plants.c both carry "
                        "REACTION_DOC() calls; pass both.\n");
        return 1;
    }
#ifdef _WIN32
    /* Binary mode for stdout to match LF in committed docs. */
    _setmode(_fileno(stdout), _O_BINARY);
#endif
    field_docs_offsets_are_sound();
    build_rows();
    adverb_exceptions_are_sound();
    legibility_overrides_are_sound();
    /* Every REACTION_DOC() call across the given files, read as text - must
     * run before any emit_*() call below, since emit_shatter(), emit_spoils()
     * and emit_pairwise_table() all pull their cause clauses out of this. */
    for (int i = 1; i < argc; i++) {
        char* src = read_whole_file(argv[i]);
        parse_reaction_docs(argv[i], src);
        free(src);
    }
    causes_are_complete();
    build_colored_list(pred_wets_liquid, " or ", wetting_liquids, sizeof(wetting_liquids));
    build_colored_list(pred_burns, " or ", heat_sources, sizeof(heat_sources));
    build_colored_list(is_quenching_liquid, " or ", quenching_liquids, sizeof(quenching_liquids));
    build_colored_list(pred_soil, " or ", soil_names, sizeof(soil_names));

    printf("<!-- BEGIN GENERATED -->\n");
    printf("<!-- GENERATED by launcher/main/apps/sand/tools/dump_reactions.c "
           "via launcher/main/apps/sand/tools/report_reactions.sh - do not "
           "edit by hand. Edit the generator, or the tables in material.c "
           "it reads, and regenerate: everything from the BEGIN GENERATED "
           "marker above to the matching END GENERATED marker below is "
           "replaced whole on every run; hand-written material outside "
           "those two markers is left untouched - see this doc's own top "
           "note. -->\n\n");
    printf("See docs/plans/Reaction-Doc-Generator-Plan.md for the design "
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
        emit_material_section(all_rows[i].name, all_rows[i].r, all_rows[i].self_id, all_rows[i].color_id);
    }

    emit_pairwise_table();
    emit_anatomy();

    printf("\n<!-- END GENERATED -->\n");

    return 0;
}
