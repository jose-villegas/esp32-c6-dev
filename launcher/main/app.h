/*
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
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
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

    /* Opt-in, not opt-out: false unless an app sets it. main.c only
     * tracks the edge-swipe-home gesture and draws its hint strip while
     * an app with this true is running - an app that leaves it unset
     * gets neither, and is responsible for its own way back to the
     * launcher. It exists for an app whose own input is a touch drag
     * near a screen edge, which the swipe-home gesture cannot be told
     * apart from: such an app turns the generic one off and offers a
     * deliberate control instead. */
    bool home_gesture;

    /* Opt-in, like home_gesture above: NULL unless an app sets it. If
     * set, called only from screenshot_dump() (CONFIG_LAUNCHER_DEVELOPMENT
     * builds only) to let the running app attach its own state to a
     * screenshot capture - a JSON OBJECT fragment (starting with `{`,
     * ending with `}`, no trailing comma) written into `out` (at most
     * `len` bytes, NUL-terminated). Spliced into the capture's
     * device-state JSON as a new "app" key. Diagnostic only - nothing
     * about the app's own behaviour depends on this. */
    void (*diagnostic_json)(char *out, size_t len);
} app_t;

/*
 * Apps register themselves, so an app is entirely contained in
 * main/apps/<name>/ and deleting that folder removes it - source, logic and
 * tests - without touching another file, CMakeLists.txt included.
 *
 * APP_REGISTER() places a constructor in .init_array, which ESP-IDF runs
 * before app_main(), into a fixed array - no allocation, and registration
 * cannot fail at an awkward time. Link order decides .init_array order, so
 * the shell sorts by name before showing the list.
 */

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
