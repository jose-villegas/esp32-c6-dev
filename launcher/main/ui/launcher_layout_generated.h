/*=============================================================================
 * GENERATED FILE - do not edit.
 *
 *     python tools/gen_launcher_layout.py main/ui/launcher_layout.json \
 *         main/ui/launcher_layout_generated.h
 *
 * Authored in main/ui/launcher_layout.json. Firmware reads this fixed
 * geometry directly; no layout parser or solver is linked on the device.
 *===========================================================================*/
#pragma once

#include <stdint.h>

typedef enum {
    LAUNCHER_ELEMENT_STATUS_BAR = 0,
    LAUNCHER_ELEMENT_LAST_PLAYED = 1,
    LAUNCHER_ELEMENT_LIBRARY = 2,
    LAUNCHER_ELEMENT_RENDER_LAB = 3,
    LAUNCHER_ELEMENT_PAGE_INDICATOR = 4,
    LAUNCHER_ELEMENT_COUNT = 5
} launcher_element_id_t;

typedef struct {
    int16_t x;
    int16_t y;
    int16_t width;
    int16_t height;
} launcher_layout_rect_t;

typedef struct {
    int16_t canvas_width;
    int16_t canvas_height;
    launcher_layout_rect_t rects[LAUNCHER_ELEMENT_COUNT];
} launcher_layout_t;

static const launcher_layout_t launcher_layout_portrait = {
    .canvas_width = 368,
    .canvas_height = 448,
    .rects = {
        [LAUNCHER_ELEMENT_STATUS_BAR] = {8, 8, 352, 34},
        [LAUNCHER_ELEMENT_LAST_PLAYED] = {12, 86, 168, 150},
        [LAUNCHER_ELEMENT_LIBRARY] = {188, 86, 168, 150},
        [LAUNCHER_ELEMENT_RENDER_LAB] = {100, 250, 168, 150},
        [LAUNCHER_ELEMENT_PAGE_INDICATOR] = {160, 416, 48, 8},
    },
};

static const launcher_layout_t launcher_layout_landscape = {
    .canvas_width = 448,
    .canvas_height = 368,
    .rects = {
        [LAUNCHER_ELEMENT_STATUS_BAR] = {12, 12, 424, 34},
        [LAUNCHER_ELEMENT_LAST_PLAYED] = {16, 98, 122, 171},
        [LAUNCHER_ELEMENT_LIBRARY] = {153, 98, 125, 173},
        [LAUNCHER_ELEMENT_RENDER_LAB] = {289, 98, 125, 173},
        [LAUNCHER_ELEMENT_PAGE_INDICATOR] = {200, 279, 48, 8},
    },
};
