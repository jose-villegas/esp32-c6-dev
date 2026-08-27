/*=============================================================================
 * app - the contract between the shell and the things it launches.
 *
 * "Apps" here are not processes. There is one binary, one address space and
 * one core; an app is a set of callbacks the shell drives. That keeps
 * switching instant and costs no flash partitions, at the price of apps not
 * being isolated from each other - a misbehaving app can corrupt the shell.
 *
 * An app never owns the screen or the frame loop. It draws into the shared
 * framebuffer when asked and returns; the shell decides when to present, and
 * paints its own chrome on top afterwards.
 *===========================================================================*/
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "input/buttons.h"

/* Touch state for the current frame.
 *
 * `pressed` and `released` are edges (true only on the frame the transition
 * happened); `down` is the level. Edges are what UI code almost always wants -
 * using the level for a button would re-trigger it every frame it is held. */
typedef struct {
    bool down;
    bool pressed;
    bool released;
    int  x, y;              /* current position, or the last one seen */
    int  press_x, press_y;  /* where the current touch began */

    /* The two physical buttons, delivered the same way touch is so an app
     * never has to poll anything itself. See buttons.h - PWR is an event from
     * the power-management chip, so only its `pressed` edge is meaningful. */
    button_t boot;
    button_t power;
} input_t;

typedef struct {
    const char *name;
    const char *summary;    /* one line, shown in the launcher list */

    /* Called once as the app starts. Use it to reset state; there is no
     * guarantee the app has not run before. */
    void (*enter)(void);

    /* Called once per frame. Draw into the shared framebuffer via gfx.
     * `dt_ms` is the time since the previous frame, for animation that should
     * not depend on framerate. */
    void (*frame)(uint32_t dt_ms, const input_t *input);

    /* Called once as the app stops. Release anything enter() acquired. */
    void (*exit)(void);
} app_t;

/*---------------------------------------------------------------------------
 * The registry
 *
 * Apps register themselves. There is no central list to edit, which is the
 * point: an app is entirely contained in main/apps/<name>/, and deleting that
 * folder removes it - source, logic and tests - without touching another file.
 * The build globs the folder, so even CMakeLists.txt stays untouched.
 *
 * APP_REGISTER() places a constructor in .init_array, which the ESP-IDF startup
 * runs before app_main(). The registry is a fixed array filled in at that
 * point, so no allocation happens and registration cannot fail at an awkward
 * time.
 *
 * Link order decides .init_array order, which is not something to rely on, so
 * the shell sorts by name before showing the list.
 *-------------------------------------------------------------------------*/

#define APP_MAX 16

/* Called by APP_REGISTER before main(). Ignores anything past APP_MAX, having
 * complained about it. */
void app_register(const app_t *app);

#define APP_REGISTER(symbol)                                        \
    __attribute__((constructor))                                    \
    static void symbol##_register(void) { app_register(&symbol); }

/* Registered apps, sorted by name. Valid from the first line of app_main(). */
const app_t *const *app_list(void);
int app_list_count(void);
