/*=============================================================================
 * On-device self test.
 *
 * Runs at boot, inside the shipped firmware, before the launcher starts. There
 * is no separate test build: what gets verified is exactly the binary that
 * ships, compiled by the same toolchain with the same options.
 *
 * It runs EVERY suite, not just the hardware ones. The portable suites already
 * pass on a host, but passing there only proves the logic is right on a laptop
 * - running them here proves the same code behaves identically built by the
 * RISC-V toolchain and executed on this chip.
 *
 * Cost is roughly half a second, most of it the DMA tests waiting on real
 * frames, which is cheap enough to pay on every boot for the guarantee that a
 * booting device is a verified device.
 *===========================================================================*/

#include "selftest.h"

#include <stdio.h>

#include "unity.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "../test/suites.h"

static const char *TAG = "selftest";

/* Unity requires these once per binary. The suites manage their own fixtures,
 * since they all share this program. */
void setUp(void) { }
void tearDown(void) { }

int selftest_run(void)
{
    const int64_t started = esp_timer_get_time();

    ESP_LOGI(TAG, "running self test");

    UNITY_BEGIN();

    /* Every registered suite, portable and hardware alike. Which ones exist
     * is decided at compile time by what was built in - see suites.h. */
    suites_run_all();

    const int failures = UNITY_END();
    const int64_t elapsed_ms = (esp_timer_get_time() - started) / 1000;

    /* A sentinel on its own line, so an automated harness can tell a finished
     * run from a board that went quiet mid-test. */
    printf("\nSELFTEST_COMPLETE failures=%d elapsed_ms=%lld\n",
           failures, (long long)elapsed_ms);
    fflush(stdout);

    if (failures > 0) {
        ESP_LOGE(TAG, "%d test(s) FAILED", failures);
    } else {
        ESP_LOGI(TAG, "all tests passed in %lld ms", (long long)elapsed_ms);
    }
    return failures;
}
