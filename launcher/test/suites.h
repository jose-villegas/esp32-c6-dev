/*=============================================================================
 * The test suites, shared by both runners.
 *
 * Every suite here is compiled into BOTH:
 *   - the host runner (test/host_main.c), for a sub-second TDD loop
 *   - the shipped firmware (main/selftest.c), which runs them at boot
 *
 * Running the same suites in both places is deliberate. The host loop is for
 * developing; the on-device run proves the code behaves identically when built
 * by the RISC-V toolchain and executed on the real chip, which is not something
 * a laptop can vouch for.
 *
 * Suites REGISTER THEMSELVES with SUITE_REGISTER, so there is no list to keep
 * in step. That matters most for app-owned suites: a suite living in
 * main/apps/<name>/ disappears with its app when the folder is deleted, and
 * nothing else needs editing. See main/app.h for the same pattern applied to
 * apps themselves.
 *
 * A portable suite must not include any ESP-IDF or hardware header, so it can
 * link on a host. Suites that need the chip are guarded with DEVICE_BUILD and
 * are simply not compiled into the host runner.
 *===========================================================================*/
#pragma once

/* Headroom, not a tight fit: the device selftest build alone (test/suites'
 * own dozen plus one suite per app) already sits close to whatever this is
 * set to, and a suite past the limit is dropped SILENTLY - suite_register()
 * only printf()s a warning to the boot console, so an overflow here reads as
 * a green run that quietly tested less than it claims. Bump this rather than
 * trim suites to fit it. */
#define SUITE_MAX 32

typedef void (*suite_fn)(void);

/* Called by SUITE_REGISTER before main(). */
void suite_register(const char *name, suite_fn fn);

#define SUITE_REGISTER(fn)                                          \
    __attribute__((constructor))                                    \
    static void fn##_register(void) { suite_register(#fn, fn); }

/* Runs every registered suite, in name order so the output is stable. */
void suites_run_all(void);
