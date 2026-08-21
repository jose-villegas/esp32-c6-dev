#pragma once

/* Runs every test suite on the device and reports to the console.
 * Returns the number of failures; zero means everything passed.
 *
 * Called at boot, after the display is up (the graphics suite needs a live
 * framebuffer) and before the launcher takes over. */
int selftest_run(void);
