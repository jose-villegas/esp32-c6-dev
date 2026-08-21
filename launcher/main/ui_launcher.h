#pragma once

#include "app.h"

void ui_launcher_init(void);

/* Builds and draws the home screen for this frame.
 * Returns the index of the app the user picked, or -1 if none. */
int ui_launcher_frame(const input_t *input);
