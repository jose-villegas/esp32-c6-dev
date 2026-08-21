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
 * Suites are portable unless marked otherwise. A portable suite must not
 * include any ESP-IDF or hardware header, so it can link on a host.
 *===========================================================================*/
#pragma once

/* Portable: pure logic, runs anywhere. */
void run_touch_fsm_suite(void);
void run_gesture_suite(void);

/* Device only: needs real framebuffer memory, DMA and I2C. Not compiled into
 * the host runner. */
#ifdef DEVICE_BUILD
void run_gfx_suite(void);
#endif
