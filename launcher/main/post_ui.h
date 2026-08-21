#pragma once

#include <stdbool.h>

#include "post.h"

/* Draws the retained POST results as rows starting at `top`, returning the y
 * below the last row. With `failures_only`, absent optional peripherals and
 * passing checks are skipped - which is what the boot failure screen wants. */
int post_ui_draw(int top, bool failures_only);

/* A full report: title, summary line, then every check. Clears the screen
 * first. Does not present - the caller decides when to push the frame. */
void post_ui_draw_report(const char *title);
