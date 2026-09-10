/*=============================================================================
 * GENERATED FILE - do not edit.
 *
 *     python tools/gen_icons.py main/apps/sand/icons/sand.png main/apps/sand/icons/sand.json > main/apps/sand/icons_sand.h
 *
 * Baked from main/apps/sand/icons/sand.png (16x16 cells) - see gfx/icon.h for
 * icon_t's own fields and tools/gen_icons.py for the PNG/SVG decode,
 * validation and packing this table was produced by.
 *===========================================================================*/
#pragma once

#include <stdint.h>

#include "gfx/icon.h"

typedef enum {
    ICON_SAND_POUR,
    ICON_SAND_ERASE,
    ICON_SAND_BOOM,
    ICON_SAND_INFO,
    ICON_SAND_COUNT
} icon_sand_id_t;

static const uint8_t icon_sand_rows[128] = {
    0x7F, 0xFE, 0x3F, 0xFC, 0x1F, 0xF8, 0x0F, 0xF0, 0x07, 0xE0, 0x03, 0xC0, 0x01, 0x80, 0x01, 0x80, 0x01, 0x80, 0x01, 0x80,
    0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x80, 0x00, 0x00, 0x01, 0x00, 0xC0, 0x03, 0x60, 0x06, 0x30, 0x0C, 0x18, 0x18,
    0x0C, 0x30, 0x06, 0x60, 0x03, 0xC0, 0x03, 0xC0, 0x03, 0xC0, 0x03, 0xC0, 0x06, 0x60, 0x0C, 0x30, 0x18, 0x18, 0x30, 0x0C,
    0x60, 0x06, 0xC0, 0x03, 0x01, 0x80, 0x01, 0x80, 0x01, 0x80, 0x03, 0xC0, 0x07, 0xE0, 0x0F, 0xF0, 0x3F, 0xFC, 0xFF, 0xFF,
    0xFF, 0xFF, 0x3F, 0xFC, 0x0F, 0xF0, 0x07, 0xE0, 0x03, 0xC0, 0x01, 0x80, 0x01, 0x80, 0x01, 0x80, 0x00, 0x00, 0x00, 0x00,
    0x03, 0xC0, 0x03, 0xC0, 0x03, 0xC0, 0x00, 0x00, 0x03, 0xC0, 0x03, 0xC0, 0x03, 0xC0, 0x03, 0xC0, 0x03, 0xC0, 0x03, 0xC0,
    0x03, 0xC0, 0x03, 0xC0, 0x03, 0xC0, 0x00, 0x00,
};

static const icon_t icon_sand_table[ICON_SAND_COUNT] = {
    [ICON_SAND_POUR] = { .offset = 0, .w = 16, .h = 16, .stride = 2, .blocks = 13 },
    [ICON_SAND_ERASE] = { .offset = 32, .w = 16, .h = 16, .stride = 2, .blocks = 28 },
    [ICON_SAND_BOOM] = { .offset = 64, .w = 16, .h = 16, .stride = 2, .blocks = 16 },
    [ICON_SAND_INFO] = { .offset = 96, .w = 16, .h = 16, .stride = 2, .blocks = 12 },
};

/* Rects every icon here emits if all are drawn once - a
 * command-list cost, not just a count. */
#define ICON_SAND_TOTAL_BLOCKS 69

/* Pins this table against its own blob, so a bad offset or
 * stride is a compile error where the header is included rather
 * than a wrong glyph at draw time. */
_Static_assert(sizeof icon_sand_rows == 128,
               "icon_sand_rows was rebaked without its offsets");
_Static_assert(0 + 16 * 2 <= (int)sizeof icon_sand_rows,
               "icon pour runs past the end of icon_sand_rows");
_Static_assert(2 == (16 + 7) / 8,
               "icon pour stride does not match its width");
_Static_assert(32 + 16 * 2 <= (int)sizeof icon_sand_rows,
               "icon erase runs past the end of icon_sand_rows");
_Static_assert(2 == (16 + 7) / 8,
               "icon erase stride does not match its width");
_Static_assert(64 + 16 * 2 <= (int)sizeof icon_sand_rows,
               "icon boom runs past the end of icon_sand_rows");
_Static_assert(2 == (16 + 7) / 8,
               "icon boom stride does not match its width");
_Static_assert(96 + 16 * 2 <= (int)sizeof icon_sand_rows,
               "icon info runs past the end of icon_sand_rows");
_Static_assert(2 == (16 + 7) / 8,
               "icon info stride does not match its width");
