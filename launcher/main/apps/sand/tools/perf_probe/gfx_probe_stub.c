/*=============================================================================
 * gfx_probe_stub - link-only stand-ins for the handful of gfx/ and app_sand.c
 * symbols suite_sand.c's DEVICE_BUILD blocks reference outside the frame-
 * budget scenes this probe actually runs (the present-cost section, and the
 * alloc-selfcheck test) - the whole translation unit still needs every
 * symbol it references to resolve at link time, even ones whose call site
 * never executes at runtime (their address is taken by the file's own
 * RUN_TEST table, which is enough to need a definition). Nothing here is
 * exercised by any scene probe_main.c can select.
 *
 * Re-verify this against suite_sand.c (grep for gfx_/sand_app_ calls across
 * the whole file, not just the SAND_HOST_PROBE scenes) whenever a new
 * DEVICE_BUILD symbol shows up unresolved at link time - don't assume this
 * file is still complete just because it built last round.
 *===========================================================================*/
#include <stdbool.h>
#include <stddef.h>

#include "gfx/gfx.h"

void
gfx_mark_dirty(int x, int y, int w, int h) {
    (void)x;
    (void)y;
    (void)w;
    (void)h;
}

void
gfx_present(void) {}

void
gfx_reset_strip_send_counts(void) {}

void
gfx_get_strip_send_counts(int* full_bands, int* gathered, int* partial_bands) {
    if (full_bands) {
        *full_bands = 0;
    }
    if (gathered) {
        *gathered = 0;
    }
    if (partial_bands) {
        *partial_bands = 0;
    }
}

/* Declared inline in suite_sand.c's DEVICE_BUILD block (normally defined in
 * app_sand.c, the hardware-facing entry point this probe does not compile).
 * This probe never runs the one test that calls it - link-only stub. */
bool
sand_app_alloc_selfcheck(size_t* out_largest_free, bool* out_impulses_ok) {
    if (out_largest_free) {
        *out_largest_free = 0;
    }
    if (out_impulses_ok) {
        *out_impulses_ok = false;
    }
    return false;
}
