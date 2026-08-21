/*=============================================================================
 * post_ui - drawing the POST report on the panel.
 *
 * Kept out of post.c so that file stays about hardware. This is only
 * presentation, and it is shared by the two places the report is shown: the
 * failure screen at boot, and the Diagnostics app.
 *
 * Rendered at glyph scale 1 (8x8), which gives 46 columns across the panel.
 * At the UI's normal scale of 2 there are only 23, which is not enough for a
 * status, a name and a detail on one line.
 *===========================================================================*/

#include "post_ui.h"

#include <stdio.h>
#include <string.h>

#include "gfx.h"

#define BG_RGB        0x0A0C14
#define HEADING_RGB   0xE6EAF2
#define OK_RGB        0x3DDC97
#define FAIL_RGB      0xFF5C5C
#define ABSENT_RGB    0x6E778C
#define DETAIL_RGB    0x8A93A8

#define SCALE         1
#define GLYPH_W       (8 * SCALE)
#define ROW_H         11
#define MARGIN        10

/* Columns, in characters, laid out so the widest real detail string still
 * fits before the panel edge. */
#define COL_NAME_X    (MARGIN + 6 * GLYPH_W)
#define COL_DETAIL_X  (MARGIN + 20 * GLYPH_W)

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
        gfx_text_scaled(COL_NAME_X, y, r->name, gfx_rgb(HEADING_RGB), SCALE);
        gfx_text_scaled(COL_DETAIL_X, y, r->detail,
                        failed ? gfx_rgb(FAIL_RGB) : gfx_rgb(DETAIL_RGB), SCALE);

        y += ROW_H;
    }

    return y;
}

void post_ui_draw_report(const char *title)
{
    gfx_clear(gfx_rgb(BG_RGB));

    gfx_text(MARGIN, MARGIN, title, gfx_rgb(HEADING_RGB));

    int y = MARGIN + gfx_text_height() + 8;

    char summary[48];
    const int failures = post_failure_count();
    snprintf(summary, sizeof(summary), "%d checks, %d failed",
             post_result_count(), failures);
    gfx_text_scaled(MARGIN, y,
                    summary,
                    failures ? gfx_rgb(FAIL_RGB) : gfx_rgb(OK_RGB), SCALE);

    y += ROW_H + 6;
    post_ui_draw(y, false);
}
