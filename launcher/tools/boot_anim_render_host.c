/*=============================================================================
 * boot_anim_render_host - render one frame of the boot animation with the
 * REAL firmware code (boot_anim.c + gfx.c, unmodified drawing logic) on a
 * host build, and write it out as a BMP.
 *
 *     boot_anim_render_host <now_ms>   > frame.bmp
 *
 * Not built by idf.py, not part of test/run_tests.sh - a standalone binary
 * compiled straight from these three translation units:
 *
 *     boot_anim_render_host.c   (this file)
 *     main/gfx/gfx.c            (host-portable - see its own ESP_PLATFORM
 *                                comment)
 *     main/boot/boot_anim.c     (host-portable for the same reason)
 *
 * with `main` on the include path (and, for tools/boot_anim_editor_server.py's
 * live-editing use, a scratch directory containing a DRAFT
 * boot_anim_timeline.h placed on the include path AHEAD of `main`, so it
 * shadows the real, committed one without ever touching it - see that
 * script's own top comment).
 *
 * No file writing, no device, no serial: gfx_init() mallocs a plain
 * framebuffer, boot_anim_draw_frame() draws into it exactly as it would on
 * the real panel, and this reads gfx_framebuffer() straight back out. The
 * BMP encoding itself is main/util/screenshot.h's - already pure, already
 * host-portable, already tested (test/suites/suite_screenshot.c) - built
 * for exactly this shape of problem (device pixels -> a BMP a host script
 * reads) even though its one caller today streams over serial instead of
 * writing to stdout.
 *===========================================================================*/

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "boot/boot_anim.h"
#include "gfx/gfx.h"
#include "gfx/gfx_color.h"
#include "util/screenshot.h"

#if defined(_WIN32)
#include <fcntl.h>
#include <io.h>
#endif

int main(int argc, char **argv)
{
    if (argc != 2) {
        fprintf(stderr, "usage: %s <now_ms>\n", argv[0]);
        return 1;
    }
    const uint32_t now_ms = (uint32_t)strtoul(argv[1], NULL, 10);

#if defined(_WIN32)
    /* stdout is text mode by default on Windows, which would rewrite every
     * 0x0A pixel byte into a 0x0D 0x0A pair - silently corrupting the image
     * rather than failing loudly. */
    _setmode(_fileno(stdout), _O_BINARY);
#endif

    if (!gfx_init()) {
        fprintf(stderr, "gfx_init failed\n");
        return 1;
    }

    boot_anim_draw_frame(now_ms);

    const gfx_color_t *fb = gfx_framebuffer();
    const int32_t stride = screenshot_bmp_row_stride(GFX_WIDTH);

    uint8_t header[SCREENSHOT_BMP_HEADER_SIZE];
    screenshot_bmp_header(header, GFX_WIDTH, GFX_HEIGHT);
    fwrite(header, 1, sizeof(header), stdout);

    uint8_t *row = calloc(1, (size_t)stride);
    if (row == NULL) {
        fprintf(stderr, "out of memory\n");
        return 1;
    }

    /* Bottom-up, per screenshot_bmp_header()'s own contract (positive
     * biHeight). */
    for (int y = GFX_HEIGHT - 1; y >= 0; y--) {
        for (int x = 0; x < GFX_WIDTH; x++) {
            const uint32_t rgb = gfx_color_rgb888(fb[(size_t)y * GFX_WIDTH + x]);
            row[x * 3 + 0] = (uint8_t)(rgb);          /* B */
            row[x * 3 + 1] = (uint8_t)(rgb >> 8);     /* G */
            row[x * 3 + 2] = (uint8_t)(rgb >> 16);    /* R */
        }
        fwrite(row, 1, (size_t)stride, stdout);
    }

    free(row);
    return 0;
}
