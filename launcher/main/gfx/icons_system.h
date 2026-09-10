/*=============================================================================
 * GENERATED FILE - do not edit.
 *
 *     python tools/gen_icons.py ../design/icons/system.png ../design/icons/system.json > main/gfx/icons_system.h
 *
 * Baked from ../design/icons/system.png (16x16 cells) - see gfx/icon.h for
 * icon_t's own fields and tools/gen_icons.py for the PNG decode, validation
 * and packing this table was produced by.
 *===========================================================================*/
#pragma once

#include <stdint.h>

#include "gfx/icon.h"

typedef enum {
    ICON_SYSTEM_CHECK,
    ICON_SYSTEM_COUNT
} icon_system_id_t;

static const uint8_t icon_system_rows[32] = {
    0x00, 0x00, 0x00, 0x04, 0x00, 0x0C, 0x00, 0x18, 0x00, 0x18, 0x00, 0x30, 0x00, 0x70, 0x30, 0x60, 0x18, 0xC0, 0x1D, 0xC0,
    0x0F, 0x80, 0x07, 0x80, 0x07, 0x00, 0x03, 0x00, 0x00, 0x00, 0x00, 0x00,
};

static const icon_t icon_system_table[ICON_SYSTEM_COUNT] = {
    [ICON_SYSTEM_CHECK] = { .offset = 0, .w = 16, .h = 16, .stride = 2, .blocks = 16 },
};
