/*=============================================================================
 * icons - drawing half of the module; see icons.h for the geometry half and
 * why they are split.
 *===========================================================================*/
#include "gfx/icons.h"

#include "gfx/gfx.h"

void icon_check(int x, int y, int w, int h, gfx_color_t color)
{
    icon_rect_t blocks[ICON_CHECK_MAX_BLOCKS];
    const int n = icon_check_blocks(w, h, blocks, ICON_CHECK_MAX_BLOCKS);

    for (int i = 0; i < n; i++) {
        gfx_fill_rect(x + blocks[i].x, y + blocks[i].y,
                     blocks[i].w, blocks[i].h, color);
    }
}
