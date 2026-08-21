/*=============================================================================
 * post - power-on self test.
 *
 * A health check of the board's hardware, run on every boot in EVERY build,
 * release included. It answers "is this board working right now?" - which is a
 * different question from "is this code correct?", the one the test suites
 * answer.
 *
 * That distinction is why this is separate from selftest.c:
 *
 *   POST                              test suites
 *   ------------------------------    ------------------------------
 *   ships in release                  diagnostics builds only
 *   checks hardware presence/health   checks software behaviour
 *   read-only, no side effects        draws to the panel, mutates state
 *   fast (a few ms)                   ~580 ms
 *   fails => this board is faulty     fails => this code is wrong
 *
 * Everything here must stay non-destructive: probing, reading identity
 * registers, reporting. Nothing that changes device state or costs real time,
 * because it runs before the user sees anything.
 *===========================================================================*/
#pragma once

#include <stdbool.h>

/* Runs every check and logs a report.
 * Returns true if all REQUIRED checks passed. Optional peripherals (an absent
 * SD card, say) are reported but do not fail the board. */
bool post_run(void);
