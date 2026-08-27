/*=============================================================================
 * post_ui - drawing the POST report on the panel.
 *
 * Kept out of post.c so that file stays about hardware. This is presentation
 * only, shared by the two places the report is shown: the failure screen at
 * boot, and the Diagnostics app.
 *
 * Rendered at glyph scale 1 (8x8). At the UI's normal scale of 2 the panel is
 * only 23 characters wide, which is narrower than most of the detail strings.
 *===========================================================================*/

#include "post_ui.h"

#include <stdio.h>
#include <string.h>

#include "gfx/gfx.h"

#define BG_RGB        0x0A0C14
#define HEADING_RGB   0xE6EAF2
#define OK_RGB        0x3DDC97
#define FAIL_RGB      0xFF5C5C
#define ABSENT_RGB    0x6E778C
#define DETAIL_RGB    0x8A93A8

#define SCALE         1
#define GLYPH_W       (8 * SCALE)
#define LINE_H        10
#define MARGIN        10

/* Details go on their own line beneath the name rather than in a column beside
 * it. Sharing a line leaves about 23 characters for the detail, and nearly
 * every one is longer than that - so it read as a wall of truncated text. */
#define NAME_X        (MARGIN + 5 * GLYPH_W)
#define DETAIL_X      NAME_X
#define DETAIL_COLS   ((GFX_WIDTH - DETAIL_X - MARGIN) / GLYPH_W)

/* How much of `text` fits on one line of `columns` width, breaking at the
 * last space that still fits - or the whole line, if no such space exists.
 * That fallback is a hard break for a single token longer than the line, so
 * a pathological string still renders rather than looping forever. */
static int line_break_length(const char *text, int columns)
{
    const int len = (int)strlen(text);
    if (len <= columns) {
        return len;
    }

    int brk = columns;
    while (brk > 0 && text[brk] != ' ') {
        brk--;
    }
    return brk > 0 ? brk : columns;
}

/* Draws `text` across as many lines as it needs, breaking at spaces, and
 * returns the y below the last line. */
static int draw_wrapped(int x, int y, int columns, const char *text,
                        gfx_color_t colour)
{
    char line[64];
    if (columns > (int)sizeof(line) - 1) {
        columns = (int)sizeof(line) - 1;
    }

    while (*text != '\0') {
        while (*text == ' ') {
            text++;          /* skip the break we just consumed */
        }
        if (*text == '\0') {
            break;
        }

        const int take = line_break_length(text, columns);

        memcpy(line, text, (size_t)take);
        line[take] = '\0';
        gfx_text_scaled(x, y, line, colour, SCALE);

        text += take;
        y += LINE_H;
    }

    return y;
}

int post_ui_draw(int top, bool failures_only)
{
    const post_result_t *results = post_results();
    const int count = post_result_count();
    int y = top;

    for (int i = 0; i < count; i++) {
        const post_result_t *r = &results[i];

        /* An optional peripheral that is simply absent is not a failure, so it
         * is skipped when only failures were asked for. */
        const bool failed = !r->ok && r->severity == POST_REQUIRED;
        if (failures_only && !failed) {
            continue;
        }

        const char *mark;
        gfx_color_t mark_colour;
        if (r->ok) {
            mark = "[ok]";
            mark_colour = gfx_rgb(OK_RGB);
        } else if (r->severity == POST_OPTIONAL) {
            mark = "[--]";
            mark_colour = gfx_rgb(ABSENT_RGB);
        } else {
            mark = "[!!]";
            mark_colour = gfx_rgb(FAIL_RGB);
        }

        gfx_text_scaled(MARGIN, y, mark, mark_colour, SCALE);
        gfx_text_scaled(NAME_X, y, r->name, gfx_rgb(HEADING_RGB), SCALE);
        y += LINE_H;

        if (r->detail[0] != '\0') {
            y = draw_wrapped(DETAIL_X, y, DETAIL_COLS, r->detail,
                             failed ? gfx_rgb(FAIL_RGB) : gfx_rgb(DETAIL_RGB));
        }

        y += 3;   /* a little air between entries */
    }

    return y;
}

void post_ui_draw_report(const char *title)
{
    gfx_clear(gfx_rgb(BG_RGB));

    gfx_text(MARGIN, MARGIN, title, gfx_rgb(HEADING_RGB));

    int y = MARGIN + gfx_text_height() + 6;

    char summary[48];
    const int failures = post_failure_count();
    snprintf(summary, sizeof(summary), "%d checks, %d failed",
             post_result_count(), failures);
    gfx_text_scaled(MARGIN, y, summary,
                    failures ? gfx_rgb(FAIL_RGB) : gfx_rgb(OK_RGB), SCALE);

    y += LINE_H + 6;
    post_ui_draw(y, false);
}
