#pragma once

#include "app.h"
#include "ui/launcher_layout_generated.h"

void ui_launcher_init(void);

/* Builds and draws the home screen for this frame.
 * Returns the index of the app the user picked, or -1 if none. */
int ui_launcher_frame(const input_t* input);

/* Host previews pass authored geometry directly; firmware calls
 * ui_launcher_frame() and keeps using the baked table. */
int ui_launcher_frame_layout(const input_t* input, const launcher_layout_t* layout);
