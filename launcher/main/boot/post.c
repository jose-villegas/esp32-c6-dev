#include "boot/post.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "bsp/esp-bsp.h"
#include "driver/i2c_master.h"
#include "driver/temperature_sensor.h"
#include "esp_chip_info.h"
#include "esp_flash.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "gfx/gfx.h"

static const char *TAG = "post";

/* Below this the framebuffer allocation and everything after it start failing
 * in confusing ways, so it is worth catching here where the message is clear. */
#define MIN_FREE_HEAP (40 * 1024)

#define EXPECTED_FLASH_BYTES (16 * 1024 * 1024)

/* The ES8311's datasheet address is 0x30 in 8-bit form; I2C wants the 7-bit
 * value. Getting this wrong reports a working codec as missing. */
#define ES8311_I2C_7BIT_ADDR 0x18

/* Results are retained rather than only logged. A board in the field may have
 * nothing attached to its serial port, so they have to be showable on screen. */
static post_result_t results[POST_MAX_CHECKS];
static int checks_run;
static int checks_failed;

static void report(const char *name, bool ok, post_severity_t severity,
                   const char *detail)
{
    const char *mark;
    if (ok) {
        mark = "ok  ";
    } else if (severity == POST_OPTIONAL) {
        mark = "--  ";        /* absent, but allowed to be */
    } else {
        mark = "FAIL";
        checks_failed++;
    }

    if (checks_run < POST_MAX_CHECKS) {
        post_result_t *r = &results[checks_run];
        r->name     = name;
        r->ok       = ok;
        r->severity = severity;
        snprintf(r->detail, sizeof(r->detail), "%s", detail ? detail : "");
    }
    checks_run++;

    ESP_LOGI(TAG, "  [%s] %-16s %s", mark, name, detail ? detail : "");
}

const post_result_t *post_results(void) { return results; }
int post_result_count(void) { return checks_run < POST_MAX_CHECKS ? checks_run : POST_MAX_CHECKS; }
int post_failure_count(void) { return checks_failed; }

/* --- phase one: before the display claims SPI2 --------------------------- */

void post_run_before_display(void)
{
    checks_run = 0;
    checks_failed = 0;

    ESP_LOGI(TAG, "power-on self test (storage)");

    /* The SD slot and the display are wired to different pins on the one SPI2
     * controller, so only one can hold the bus. Testing the card here - before
     * gfx_init() takes SPI2 - means genuinely mounting it, with no teardown and
     * nothing to restore afterwards. Once the display is up this is impossible
     * without tearing it down again. */
    const esp_err_t err = bsp_sdcard_mount();

    if (err == ESP_OK && bsp_sdcard != NULL) {
        const uint64_t bytes =
            (uint64_t)bsp_sdcard->csd.capacity * bsp_sdcard->csd.sector_size;

        char detail[72];
        snprintf(detail, sizeof(detail), "%s, %llu MB, mounted and released",
                 bsp_sdcard->cid.name, (unsigned long long)(bytes >> 20));
        report("sd card", true, POST_OPTIONAL, detail);

        /* Release it again so the display can have the bus back. */
        bsp_sdcard_unmount();
    } else {
        /* No card is a normal state, not a fault - hence OPTIONAL. */
        report("sd card", false, POST_OPTIONAL,
               err == ESP_ERR_NOT_FOUND ? "no card inserted"
                                        : "no card / not mountable");
    }
}

/* Re-tests the card while the shell is running, by borrowing SPI2 from the
 * display and giving it straight back.
 *
 * This is the experiment the board notes described but had not measured: the
 * claim was that resuming without re-sending the panel's init sequence costs
 * single-digit milliseconds. The timings are logged so the note can be
 * replaced with a fact. */
static void check_sdcard_live(void)
{
    const int64_t t0 = esp_timer_get_time();
    gfx_suspend();
    const int64_t t_suspended = esp_timer_get_time();

    const esp_err_t err = bsp_sdcard_mount();
    char card[40] = "no card";
    if (err == ESP_OK && bsp_sdcard != NULL) {
        const uint64_t bytes =
            (uint64_t)bsp_sdcard->csd.capacity * bsp_sdcard->csd.sector_size;
        snprintf(card, sizeof(card), "%s, %llu MB",
                 bsp_sdcard->cid.name, (unsigned long long)(bytes >> 20));
        bsp_sdcard_unmount();
    }
    const int64_t t_card = esp_timer_get_time();

    /* No full init: the panel never lost power, so only the ESP32 side needs
     * rebuilding. This is the part worth measuring. */
    const bool back = gfx_resume(false);
    const int64_t t_resumed = esp_timer_get_time();

    ESP_LOGI(TAG, "sd round trip: suspend %lld us, card %lld us, resume %lld us",
             (long long)(t_suspended - t0),
             (long long)(t_card - t_suspended),
             (long long)(t_resumed - t_card));

    /* Wider than post_result_t::detail on purpose so the round-trip figure
     * cannot be the thing that gets cut; report() trims to fit. */
    char detail[sizeof(card) + 40];
    snprintf(detail, sizeof(detail), "%s (live, %lld ms round trip)",
             card, (long long)((t_resumed - t0) / 1000));

    report("sd card", err == ESP_OK, POST_OPTIONAL, detail);

    if (!back) {
        report("display resume", false, POST_REQUIRED, "panel did not come back");
    }
}

/* --- phase two: once the display is up ----------------------------------- */

static void check_soc(void)
{
    esp_chip_info_t info;
    esp_chip_info(&info);

    char detail[72];
    snprintf(detail, sizeof(detail), "rev %d, %d core, %s%s%s",
             info.revision, info.cores,
             (info.features & CHIP_FEATURE_WIFI_BGN) ? "wifi " : "",
             (info.features & CHIP_FEATURE_BLE)      ? "ble "  : "",
             (info.features & CHIP_FEATURE_IEEE802154) ? "802.15.4" : "");
    report("soc", true, POST_REQUIRED, detail);
}

static void check_flash(void)
{
    uint32_t size = 0;
    const bool ok = esp_flash_get_size(NULL, &size) == ESP_OK;

    char detail[72];
    snprintf(detail, sizeof(detail), "%" PRIu32 " MB%s", size / (1024 * 1024),
             (ok && size != EXPECTED_FLASH_BYTES) ? " (expected 16)" : "");

    report("flash", ok && size == EXPECTED_FLASH_BYTES, POST_REQUIRED, detail);
}

static void check_memory(void)
{
    const size_t free_heap = esp_get_free_heap_size();
    const size_t largest_dma = heap_caps_get_largest_free_block(MALLOC_CAP_DMA);

    char detail[72];
    snprintf(detail, sizeof(detail), "%u KiB free, DMA block %u KiB",
             (unsigned)(free_heap / 1024), (unsigned)(largest_dma / 1024));
    report("memory", free_heap > MIN_FREE_HEAP, POST_REQUIRED, detail);

    /* This board has no PSRAM. Finding some would mean we are running on
     * different hardware than the code assumes, which is worth knowing. */
    const size_t psram = heap_caps_get_total_size(MALLOC_CAP_SPIRAM);
    report("psram", psram == 0, POST_REQUIRED,
           psram == 0 ? "absent (expected)" : "present - unexpected");
}

static void check_mac(void)
{
    uint8_t mac[6] = { 0 };
    const bool ok = esp_read_mac(mac, ESP_MAC_WIFI_STA) == ESP_OK;

    char detail[72];
    snprintf(detail, sizeof(detail), "%02x:%02x:%02x:%02x:%02x:%02x",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

    /* An all-zero MAC means eFuse was never programmed or cannot be read,
     * which breaks every radio before it starts. */
    const bool valid = ok && (mac[0] | mac[1] | mac[2] | mac[3] | mac[4] | mac[5]);
    report("mac / efuse", valid, POST_REQUIRED, detail);
}

static void check_temperature(void)
{
    temperature_sensor_handle_t sensor = NULL;
    temperature_sensor_config_t cfg = TEMPERATURE_SENSOR_CONFIG_DEFAULT(-10, 80);

    if (temperature_sensor_install(&cfg, &sensor) != ESP_OK) {
        report("temp sensor", false, POST_REQUIRED, "install failed");
        return;
    }

    float celsius = 0.0f;
    const bool ok = temperature_sensor_enable(sensor) == ESP_OK &&
                    temperature_sensor_get_celsius(sensor, &celsius) == ESP_OK;

    temperature_sensor_disable(sensor);
    temperature_sensor_uninstall(sensor);

    char detail[72];
    snprintf(detail, sizeof(detail), "%.1f C", celsius);

    /* A plausible reading also rules out a sensor stuck at a fixed value. */
    report("temp sensor", ok && celsius > -40.0f && celsius < 125.0f,
           POST_REQUIRED, detail);
}

/* Every I2C peripheral shares one bus, so a single probe per address
 * establishes whether each chip is alive and addressable. This is the heart of
 * the POST - it is how "can we talk to the gyro" gets answered. */
static void check_i2c_devices(void)
{
    i2c_master_bus_handle_t bus = bsp_i2c_get_handle();
    if (bus == NULL) {
        report("i2c bus", false, POST_REQUIRED, "not initialised");
        return;
    }
    report("i2c bus", true, POST_REQUIRED, "port 0, SDA 8, SCL 7");

    static const struct {
        const char *name;
        uint16_t    address;
        const char *what;
    } devices[] = {
        { "io expander", BSP_IO_EXPANDER_I2C_ADDRESS, "TCA9554 reset lines" },
        { "pmu",         BSP_PMU_I2C_ADDRESS,         "AXP2101 power" },
        { "imu",         BSP_IMU_I2C_ADDRESS,         "QMI8658 accel+gyro" },
        { "rtc",         BSP_RTC_I2C_ADDRESS,         "PCF85063 clock" },
    };

    for (unsigned i = 0; i < sizeof(devices) / sizeof(devices[0]); i++) {
        const bool present = i2c_master_probe(bus, devices[i].address, 100) == ESP_OK;

        char detail[72];
        snprintf(detail, sizeof(detail), "0x%02x  %s",
                 devices[i].address, devices[i].what);
        report(devices[i].name, present, POST_REQUIRED, detail);
    }

    /* Touch sits at a different address per board revision, so report which
     * one answered rather than probing a single expected address. */
    const bool ft5x06 = i2c_master_probe(bus, BSP_TOUCH_FT5X06_I2C_ADDRESS, 100) == ESP_OK;
    const bool cst820 = i2c_master_probe(bus, BSP_TOUCH_CST820_I2C_ADDRESS, 100) == ESP_OK;

    report("touch", ft5x06 || cst820, POST_REQUIRED,
           ft5x06 ? "0x38  FT5x06 (V1)" :
           cst820 ? "0x15  CST820 (V2)" : "no controller answered");
}

static void check_audio_codec(void)
{
    i2c_master_bus_handle_t bus = bsp_i2c_get_handle();
    if (bus == NULL) {
        report("audio codec", false, POST_REQUIRED, "no i2c bus");
        return;
    }

    /* The codec sits behind the power amplifier enable on the IO expander, so
     * bring that up before probing or an alive codec reports as missing. This
     * only powers the rail - it makes no sound and configures nothing. */
    const bool powered = bsp_audio_poweramp_enable(true) == ESP_OK;
    vTaskDelay(pdMS_TO_TICKS(10));   /* let the rail settle before probing */

    const bool present =
        i2c_master_probe(bus, ES8311_I2C_7BIT_ADDR, 100) == ESP_OK;

    /* Leave it off again: POST must not change the state the shell inherits. */
    bsp_audio_poweramp_enable(false);

    char detail[72];
    snprintf(detail, sizeof(detail), "0x%02x  ES8311%s",
             ES8311_I2C_7BIT_ADDR, powered ? "" : " (amp enable failed)");
    report("audio codec", present, POST_REQUIRED, detail);
}

static void check_display(void)
{
    char detail[72];
    snprintf(detail, sizeof(detail), "%dx%d %s", GFX_WIDTH, GFX_HEIGHT,
             bsp_board_variant_to_name(bsp_board_get_variant()));
    report("display", gfx_framebuffer() != NULL, POST_REQUIRED, detail);
}

void post_rerun(void)
{
    checks_run = 0;
    checks_failed = 0;

    ESP_LOGI(TAG, "re-running self test");

    /* Now that gfx owns the panel it can be released, so the card really can
     * be re-tested live: suspend the display, take SPI2, mount, hand the bus
     * back. The panel goes on showing its last frame throughout, because it
     * refreshes from its own GRAM without the MCU. */
    check_sdcard_live();

    post_run_after_display();
}

bool post_run_after_display(void)
{
    check_soc();
    check_flash();
    check_memory();
    check_mac();
    check_temperature();
    check_i2c_devices();
    check_audio_codec();
    check_display();

    if (checks_failed == 0) {
        ESP_LOGI(TAG, "%d checks, all passed", checks_run);
    } else {
        ESP_LOGE(TAG, "%d checks, %d FAILED", checks_run, checks_failed);
    }

    /* A machine-readable line, so a production rig can grep one string rather
     * than parse the table above. */
    printf("POST_COMPLETE checks=%d failures=%d\n", checks_run, checks_failed);
    fflush(stdout);

    return checks_failed == 0;
}
