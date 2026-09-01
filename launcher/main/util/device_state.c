/*=============================================================================
 * device_state.c - the device-only half: everything device_state_read()
 * needs actual hardware for. See device_state.h for the pure formatting
 * side and the module's own reason to exist.
 *===========================================================================*/
#include "util/device_state.h"

#include "driver/temperature_sensor.h"
#include "esp_system.h"
#include "esp_timer.h"

#include "display/display.h"

/* One-shot install/enable/read/disable/uninstall, the same pattern
 * boot/post.c's check_temperature() already uses and already proved safe
 * on this chip - duplicated rather than shared because post.c's version is
 * `static` and tangled up with its own POST report() call, and a five-line
 * sensor read is cheaper to keep independent than to detangle (the same
 * reasoning main.c and app_sand.c each keep their own small copy of the
 * IMU's gravity-axis mapping instead of sharing one). Not something to call
 * every frame - fine for the occasional, deliberately-triggered snapshot
 * device_state_read() is for. */
static bool read_die_temperature(float *out_celsius)
{
    temperature_sensor_handle_t sensor = NULL;
    temperature_sensor_config_t cfg = TEMPERATURE_SENSOR_CONFIG_DEFAULT(-10, 80);
    if (temperature_sensor_install(&cfg, &sensor) != ESP_OK) {
        return false;
    }

    const bool ok = temperature_sensor_enable(sensor) == ESP_OK &&
                    temperature_sensor_get_celsius(sensor, out_celsius) == ESP_OK;

    temperature_sensor_disable(sensor);
    temperature_sensor_uninstall(sensor);
    return ok;
}

void device_state_read(device_state_t *out)
{
    out->uptime_us           = esp_timer_get_time();
    out->heap_free_bytes     = (uint32_t)esp_get_free_heap_size();
    out->heap_min_free_bytes = (uint32_t)esp_get_minimum_free_heap_size();

    /* Not a runtime query: this project does not enable dynamic frequency
     * scaling (no CONFIG_PM_ENABLE), so the Kconfig value already IS the
     * running frequency, and reading it back at runtime would need
     * esp_clk_cpu_freq() from esp_hw_support's private
     * esp_private/esp_clk.h - not a header this module should reach into
     * for a value that cannot actually change on this board. */
    out->cpu_freq_mhz = CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ;

    out->temp_ok = read_die_temperature(&out->temp_c);

    out->quarter = display_shell_quarter();

    out->imu_ready = imu_ready();
    out->imu_read_ok = out->imu_ready && imu_read(&out->imu);
}
