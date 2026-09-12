/* GENERATED FILE - do not edit.
 * python tools/gen_control_center_layout.py main/ui/control_center_layout.json
 *     main/ui/control_center_layout_generated.h
 */
#pragma once

#include <stdint.h>

typedef enum {
    CONTROL_CENTER_ELEMENT_GRABBER = 0,
    CONTROL_CENTER_ELEMENT_HEADER = 1,
    CONTROL_CENTER_ELEMENT_WIFI = 2,
    CONTROL_CENTER_ELEMENT_BLUETOOTH = 3,
    CONTROL_CENTER_ELEMENT_LINK = 4,
    CONTROL_CENTER_ELEMENT_VOLUME = 5,
    CONTROL_CENTER_ELEMENT_BRIGHTNESS = 6,
    CONTROL_CENTER_ELEMENT_NOTIFICATIONS_HEADER = 7,
    CONTROL_CENTER_ELEMENT_LIBRARY_NOTIFICATION = 8,
    CONTROL_CENTER_ELEMENT_CONTROLLER_NOTIFICATION = 9,
    CONTROL_CENTER_ELEMENT_COUNT = 10
} control_center_element_id_t;

typedef struct { int16_t x, y, width, height; } control_center_layout_rect_t;
typedef struct {
    int16_t canvas_width, canvas_height;
    control_center_layout_rect_t rects[CONTROL_CENTER_ELEMENT_COUNT];
} control_center_layout_t;

static const control_center_layout_t control_center_layout_portrait = {
    .canvas_width = 368, .canvas_height = 448,
    .rects = {
        [CONTROL_CENTER_ELEMENT_GRABBER] = {150, 10, 68, 4},
        [CONTROL_CENTER_ELEMENT_HEADER] = {16, 26, 336, 20},
        [CONTROL_CENTER_ELEMENT_WIFI] = {16, 54, 160, 78},
        [CONTROL_CENTER_ELEMENT_BLUETOOTH] = {192, 54, 160, 78},
        [CONTROL_CENTER_ELEMENT_LINK] = {16, 144, 336, 58},
        [CONTROL_CENTER_ELEMENT_VOLUME] = {16, 214, 160, 52},
        [CONTROL_CENTER_ELEMENT_BRIGHTNESS] = {192, 214, 160, 52},
        [CONTROL_CENTER_ELEMENT_NOTIFICATIONS_HEADER] = {16, 280, 336, 18},
        [CONTROL_CENTER_ELEMENT_LIBRARY_NOTIFICATION] = {16, 308, 336, 58},
        [CONTROL_CENTER_ELEMENT_CONTROLLER_NOTIFICATION] = {16, 378, 336, 58},
    },
};

static const control_center_layout_t control_center_layout_landscape = {
    .canvas_width = 448, .canvas_height = 368,
    .rects = {
        [CONTROL_CENTER_ELEMENT_GRABBER] = {190, 10, 68, 4},
        [CONTROL_CENTER_ELEMENT_HEADER] = {16, 26, 416, 20},
        [CONTROL_CENTER_ELEMENT_WIFI] = {16, 54, 130, 84},
        [CONTROL_CENTER_ELEMENT_BLUETOOTH] = {159, 54, 130, 84},
        [CONTROL_CENTER_ELEMENT_LINK] = {302, 54, 130, 84},
        [CONTROL_CENTER_ELEMENT_VOLUME] = {16, 150, 203, 52},
        [CONTROL_CENTER_ELEMENT_BRIGHTNESS] = {229, 150, 203, 52},
        [CONTROL_CENTER_ELEMENT_NOTIFICATIONS_HEADER] = {16, 218, 416, 18},
        [CONTROL_CENTER_ELEMENT_LIBRARY_NOTIFICATION] = {16, 246, 416, 50},
        [CONTROL_CENTER_ELEMENT_CONTROLLER_NOTIFICATION] = {16, 306, 416, 50},
    },
};
