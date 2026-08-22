/*=============================================================================
 * gfx_color - what a pixel is, separately from how the panel works.
 *
 * Split out of gfx.h because this part is pure arithmetic and nothing else:
 * no BSP, no drivers, no hardware headers. That lets code which only needs to
 * describe colours - a material table, say - be compiled and tested on a host,
 * while gfx.h keeps everything that genuinely needs the board.
 *
 * The macros matter for more than tidiness. A colour table built from them is
 * a compile-time constant, so it lands in .rodata and is memory-mapped from
 * flash at zero cost in RAM - which on this board is the resource that actually
 * runs out. Computing the same table at startup would cost real bytes of the
 * scarcest thing there is.
 *===========================================================================*/
#pragma once

#include <stdint.h>

/* A packed, panel-ready pixel. Produced by GFX_RGB or gfx_rgb(), stored in the
 * framebuffer, never inspected by callers. */
typedef uint16_t gfx_color_t;

/* 0xRRGGBB to RGB565. */
#define GFX_RGB565(rgb)                        \
    ((((uint32_t)(rgb) >> 8) & 0xF800u) |      \
     (((uint32_t)(rgb) >> 5) & 0x07E0u) |      \
     (((uint32_t)(rgb) >> 3) & 0x001Fu))

/* 0xRRGGBB to the panel's format: RGB565 with the bytes swapped, which is what
 * this QSPI controller expects - the opposite order to the chip's native
 * layout. Usable in a constant expression. */
#define GFX_RGB(rgb)                           \
    ((gfx_color_t)((GFX_RGB565(rgb) >> 8) |    \
                   (GFX_RGB565(rgb) << 8)))
