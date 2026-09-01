/*=============================================================================
 * Portable suite: device_state_format_json - the pure formatting half of
 * util/device_state.h. device_state_read() (device_state.c) actually
 * touches hardware and is not tested here - only that a fully-populated
 * (or deliberately not-ready/not-ok) device_state_t formats into the exact
 * JSON shape screenshot.py's host side expects.
 *===========================================================================*/

#include <string.h>

#include "unity.h"
#include "suites.h"

#include "util/device_state.h"

static void test_full_state_formats_exactly(void)
{
    const device_state_t state = {
        .uptime_us           = 1234567,
        .heap_free_bytes     = 200000,
        .heap_min_free_bytes = 150000,
        .cpu_freq_mhz        = 160,
        .temp_ok             = true,
        .temp_c              = 34.5f,
        .quarter             = 1,
        .imu_ready           = true,
        .imu_read_ok         = true,
        .imu = { .ax = 100, .ay = -200, .az = 4000, .gx = 1, .gy = -1, .gz = 0 },
    };

    input_t input = { 0 };
    input.down       = true;
    input.x          = 50;
    input.y          = 60;
    input.boot.down  = true;
    input.power.held = false;

    char json[DEVICE_STATE_JSON_MAX];
    device_state_format_json(&state, &input, json);

    const char *expected =
        "{\"uptime_us\":1234567,\"heap_free_bytes\":200000,"
        "\"heap_min_free_bytes\":150000,\"cpu_freq_mhz\":160,"
        "\"temp_c\":34.5,\"orientation_quarter\":1,"
        "\"imu\":{\"ready\":true,\"ax\":100,\"ay\":-200,\"az\":4000,"
        "\"gx\":1,\"gy\":-1,\"gz\":0},"
        "\"touch\":{\"down\":true,\"x\":50,\"y\":60},"
        "\"buttons\":{\"boot_down\":true,\"power_held\":false}}";

    TEST_ASSERT_EQUAL_STRING(expected, json);
}

static void test_imu_not_ready_omits_axes(void)
{
    device_state_t state = { 0 };
    state.imu_ready = false;

    const input_t input = { 0 };
    char json[DEVICE_STATE_JSON_MAX];
    device_state_format_json(&state, &input, json);

    TEST_ASSERT_NOT_NULL(strstr(json, "\"imu\":{\"ready\":false}"));
}

static void test_imu_ready_but_read_failed(void)
{
    device_state_t state = { 0 };
    state.imu_ready   = true;
    state.imu_read_ok = false;

    const input_t input = { 0 };
    char json[DEVICE_STATE_JSON_MAX];
    device_state_format_json(&state, &input, json);

    TEST_ASSERT_NOT_NULL(strstr(json, "\"imu\":{\"ready\":true,\"read_failed\":true}"));
}

static void test_temp_not_ok_is_json_null_not_a_string(void)
{
    /* "null" (bare, no quotes) - a host json.loads() must see None, not the
     * four-character string "null", or a consumer checking `is None` would
     * silently see a string instead and never notice the sensor failed. */
    device_state_t state = { 0 };
    state.temp_ok = false;

    const input_t input = { 0 };
    char json[DEVICE_STATE_JSON_MAX];
    device_state_format_json(&state, &input, json);

    TEST_ASSERT_NOT_NULL(strstr(json, "\"temp_c\":null"));
    TEST_ASSERT_NULL(strstr(json, "\"temp_c\":\"null\""));
}

void suite_device_state(void)
{
    RUN_TEST(test_full_state_formats_exactly);
    RUN_TEST(test_imu_not_ready_omits_axes);
    RUN_TEST(test_imu_ready_but_read_failed);
    RUN_TEST(test_temp_not_ok_is_json_null_not_a_string);
}

SUITE_REGISTER(suite_device_state)
