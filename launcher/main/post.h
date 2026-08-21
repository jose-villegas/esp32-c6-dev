/*=============================================================================
 * post - power-on self test.
 *
 * A health check of the board's hardware, run on every boot in EVERY build,
 * release included. It answers "is this board working right now?" - a different
 * question from "is this code correct?", which the test suites answer.
 *
 * That distinction is why this is separate from selftest.c:
 *
 *   POST                              test suites
 *   ------------------------------    ------------------------------
 *   ships in release                  diagnostics builds only
 *   checks hardware presence/health   checks software behaviour
 *   read-only, no side effects        draws to the panel, mutates state
 *   fast (~95 ms)                     ~580 ms
 *   fails => this board is faulty     fails => this code is wrong
 *
 * Everything here must stay non-destructive: probing, reading identity
 * registers, reporting. Nothing that changes device state or costs real time,
 * because it runs before the user sees anything.
 *===========================================================================*/
#pragma once

#include <stdbool.h>

#define POST_MAX_CHECKS 24

typedef enum {
    POST_REQUIRED,   /* absence means the board is faulty */
    POST_OPTIONAL,   /* absence is legitimate - a missing SD card, say */
} post_severity_t;

typedef struct {
    const char     *name;
    bool            ok;
    post_severity_t severity;
    char            detail[72];
} post_result_t;

/* POST runs in two phases, because the SD card and the display are wired to
 * different pins on the one SPI2 controller and cannot both hold it.
 *
 * Call order matters:
 *
 *   post_run_before_display();   // SD card - needs SPI2 free
 *   gfx_init();                  // display takes SPI2
 *   post_run_after_display();    // everything else
 *
 * Testing the card before the display claims the bus means genuinely mounting
 * it, with no teardown and nothing to restore. Doing it later would mean
 * dismantling a running display.
 */
void post_run_before_display(void);

/* Returns true if all REQUIRED checks passed across both phases. Optional
 * peripherals - an absent SD card, say - are reported but never fail. */
bool post_run_after_display(void);

/* The results of the last run, retained so they can be shown on screen as well
 * as logged - a board in the field may have no serial cable attached. */
const post_result_t *post_results(void);
int post_result_count(void);
int post_failure_count(void);
