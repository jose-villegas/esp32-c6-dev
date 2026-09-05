/*=============================================================================
 * reaction_doc - REACTION_DOC(field, why): says WHERE a reaction_t field's
 * trigger actually lives, at the read site that decides it.
 *
 * See docs/Sand/Reaction-Doc-Generator-Plan.md ("The cause clause belongs
 * at the read site") for the problem this solves. `shatters_to` fires
 * under a threshold - SAND_SHOCK_COLD, SAND_SHOCK_HEAT - that appears
 * nowhere in reactions[]/extended_reactions[], only in the code that reads
 * the field. No script over material.c recovers that; it can only be
 * declared where the condition actually lives, which is here, immediately
 * above the `if` it describes.
 *
 * WHY THIS SHAPE
 *
 *   - `field` is compiler-checked. `sizeof(((const reaction_t *)0)->field)`
 *     fails to build if `field` is misspelled or renamed out from under
 *     this call - the same protection field_docs[]'s own _Static_assert
 *     gives the table side (dump_reactions.c), now extended to the
 *     read-site side. A stale field name here is a build error, not a
 *     silently wrong doc.
 *   - It expands to nothing at runtime and puts no string in the firmware
 *     image - not behind a guard, because there is nothing to guard:
 *     `why` is never referenced by the expansion at all, so it costs the
 *     device build exactly as much as a comment would.
 *   - dump_reactions.c never links this header's callers. It reads
 *     sand_reactions.c as TEXT (see report_reactions.sh, which passes that
 *     file's path as an argument) and extracts every
 *     `REACTION_DOC(field, "literal")` call by scanning for it, not by
 *     compiling it. That is not a reversal of the plan's "compile the
 *     tables, do not parse them" rule - that rule is about constant
 *     EXPRESSIONS (`MATX(MATX_LEAF)`, `SAND_SHOCK_HEAT` itself) that need
 *     the preprocessor to resolve. A string literal has no such semantics;
 *     there is nothing to evaluate, only to read back verbatim. Linking
 *     sand_reactions.c into the tool instead would drag in sand.c, the RNG
 *     and the rest of the simulation into the doc build's link graph, to
 *     resolve on the order of a handful of strings - a bad trade, and
 *     recorded as a decision taken in the plan's own "Decisions taken"
 *     section.
 *
 * `why` must be a plain string literal - dump_reactions.c errors out,
 * rather than guessing, if it is anything else (an identifier, a macro, a
 * concatenation), or if `field` does not name one of field_docs[]'s own
 * rows. Write it as a sentence FRAGMENT that completes the generated
 * sentence: lower case, no trailing period - see dump_reactions.c's
 * field_docs[]/emit_*() functions for the shape it drops into. "if chilled
 * while hot", not "when CELL_VARIANT(n) <= SAND_SHOCK_COLD".
 *===========================================================================*/
#pragma once

#include "material.h"

#define REACTION_DOC(field, why) \
    ((void)sizeof(((const reaction_t *)0)->field))
