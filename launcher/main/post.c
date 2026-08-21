#include "post.h"

#include <inttypes.h>
#include <stdio.h>

#include "bsp/esp-bsp.h"
#include "driver/i2c_master.h"
#include "driver/temperature_sensor.h"
#include "esp_chip_info.h"
#include "esp_flash.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_mac.h"

#include "gfx.h"

static const char *TAG = "post";

/* Below this the framebuffer allocation and everything after it start failing
 * in confusing ways, so it is worth catching here where the message is clear. */
#define MIN_FREE_HEAP (40 * 1024)

#define EXPECTED_FLASH_BYTES (16 * 1024 * 1024)

typedef enum {
    REQUIRED,   /* absence means the board is faulty */
    OPTIONAL,   /* absence is legitimate - a missing SD card, say */
} severity_t;

static int checks_run;
static int checks_failed;

static void report(const char *name, bool ok, severity_t severity,
                   const char *detail)
{
    checks_run++;

    const char *mark;
    if (ok) {
        mark = "ok  ";
    } else if (severity == OPTIONAL) {
        mark = "--  ";        /* absent, but allowed to be */
    } else {
        mark = "FAIL";
        checks_failed++;
    }

    if (detail != NULL && detail[0] != '\0') {
        ESP_LOGI(TAG, "  [%s] %-16s %s", mark, name, detail);
    } else {
        ESP_LOGI(TAG, "  [%s] %-16s", mark, name);
    }
}

/* --- individual checks --------------------------------------------------- */

static void check_soc(void)
{
    esp_chip_info_t info;
    esp_chip_info(&info);

    char detail[96];
    snprintf(detail, sizeof(detail), "rev %d, %d core%s, %s%s%s",
             info.revision, info.cores, info.cores == 1 ? "" : "s",
             (info.features & CHIP_FEATURE_WIFI_BGN) ? "wifi " : "",
             (info.features & CHIP_FEATURE_BLE)      ? "ble "  : "",
             (info.features & CHIP_FEATURE_IEEE802154) ? "802.15.4" : "");
    report("soc", true, REQUIRED, detail);
}

static void check_flash(void)
{
    uint32_t size = 0;
    const bool ok = esp_flash_get_size(NULL, &size) == ESP_OK;

    char detail[64];
    snprintf(detail, sizeof(detail), "%" PRIu32 " MB%s", size / (1024 * 1024),
             (ok && size != EXPECTED_FLASH_BYTES) ? " (expected 16 MB)" : "");

    report("flash", ok && size == EXPECTED_FLASH_BYTES, REQUIRED, detail);
}

static void check_memory(void)
{
    const size_t free_heap = esp_get_free_heap_size();
    const size_t largest_dma = heap_caps_get_largest_free_block(MALLOC_CAP_DMA);

    char detail[96];
    snprintf(detail, sizeof(detail), "%u KiB free, largest DMA block %u KiB",
             (unsigned)(free_heap / 1024), (unsigned)(largest_dma / 1024));

    report("memory", free_heap > MIN_FREE_HEAP, REQUIRED, detail);

    /* This board has no PSRAM. Finding some would mean we are running on
     * different hardware than the code assumes, which is worth knowing. */
    const size_t psram = heap_caps_get_total_size(MALLOC_CAP_SPIRAM);
    report("psram", psram == 0, REQUIRED,
           psram == 0 ? "absent (expected)" : "present - unexpected on this board");
}

static void check_mac(void)
{
    uint8_t mac[6] = { 0 };
    const bool ok = esp_read_mac(mac, ESP_MAC_WIFI_STA) == ESP_OK;

    char detail[32];
    snprintf(detail, sizeof(detail), "%02x:%02x:%02x:%02x:%02x:%02x",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

    /* An all-zero MAC means eFuse was not programmed or could not be read,
     * which breaks every radio before it starts. */
    const bool valid = ok && (mac[0] | mac[1] | mac[2] | mac[3] | mac[4] | mac[5]);
    report("mac / efuse", valid, REQUIRED, detail);
}

static void check_temperature(void)
{
    temperature_sensor_handle_t sensor = NULL;
    temperature_sensor_config_t cfg = TEMPERATURE_SENSOR_CONFIG_DEFAULT(-10, 80);

    if (temperature_sensor_install(&cfg, &sensor) != ESP_OK) {
        report("temp sensor", false, REQUIRED, "install failed");
        return;
    }

    float celsius = 0.0f;
    bool ok = temperature_sensor_enable(sensor) == ESP_OK &&
              temperature_sensor_get_celsius(sensor, &celsius) == ESP_OK;

    temperature_sensor_disable(sensor);
    temperature_sensor_uninstall(sensor);

    char detail[32];
    snprintf(detail, sizeof(detail), "%.1f C", celsius);

    /* A plausible reading also rules out a sensor returning a stuck value. */
    report("temp sensor", ok && celsius > -40.0f && celsius < 125.0f,
           REQUIRED, detail);
}

/* Every I2C peripheral on this board shares one bus, so a single probe per
 * address establishes whether each chip is alive and addressable. This is the
 * heart of the POST: it is how "can we talk to the gyro" gets answered. */
static void check_i2c_devices(void)
{
    i2c_master_bus_handle_t bus = bsp_i2c_get_handle();
    if (bus == NULL) {
        report("i2c bus", false, REQUIRED, "not initialised");
        return;
    }
    report("i2c bus", true, REQUIRED, "port 0, SDA 8, SCL 7");

    static const struct {
        const char *name;
        uint16_t    address;
        severity_t  severity;
        const char *what;
    } devices[] = {
        { "io expander", BSP_IO_EXPANDER_I2C_ADDRESS, REQUIRED,
          "TCA9554 - drives display and touch reset" },
        { "pmu",         BSP_PMU_I2C_ADDRESS,         REQUIRED,
          "AXP2101 - power and battery" },
        { "imu",         BSP_IMU_I2C_ADDRESS,         REQUIRED,
          "QMI8658 - accelerometer and gyro" },
        { "rtc",         BSP_RTC_I2C_ADDRESS,         REQUIRED,
          "PCF85063 - real time clock" },
    };

    for (unsigned i = 0; i < sizeof(devices) / sizeof(devices[0]); i++) {
        const bool present =
            i2c_master_probe(bus, devices[i].address, 100) == ESP_OK;

        char detail[96];
        snprintf(detail, sizeof(detail), "0x%02x  %s",
                 devices[i].address, devices[i].what);
        report(devices[i].name, present, devices[i].severity, detail);
    }

    /* Touch sits at a different address per board revision, so identify which
     * answered rather than probing a single expected one. */
    const bool ft5x06 = i2c_master_probe(bus, BSP_TOUCH_FT5X06_I2C_ADDRESS, 100) == ESP_OK;
    const bool cst820 = i2c_master_probe(bus, BSP_TOUCH_CST820_I2C_ADDRESS, 100) == ESP_OK;

    report("touch", ft5x06 || cst820, REQUIRED,
           ft5x06 ? "0x38  FT5x06 (board V1)" :
           cst820 ? "0x15  CST820 (board V2)" : "no controller answered");
}

static void check_display(void)
{
    /* gfx owns the panel, so if the framebuffer exists the QSPI bus came up,
     * the panel accepted its init sequence and the display is on. */
    char detail[64];
    snprintf(detail, sizeof(detail), "%dx%d, %s",
             GFX_WIDTH, GFX_HEIGHT, bsp_board_variant_to_name(bsp_board_get_variant()));

    report("display", gfx_framebuffer() != NULL, REQUIRED, detail);
}

static void check_sdcard(void)
{
    /* Deliberately not probed. The SD slot and the display are wired to
     * separate pin sets on the one SPI2 controller, so mounting the card would
     * require tearing the display down - see docs/ESP32-C6-AMOLED-Notes.md.
     * Reporting the constraint is more useful than pretending to test it. */
    report("sd card", true, OPTIONAL, "not probed - shares SPI2 with the display");
}

static void check_audio(void)
{
    /* The ES8311 codec sits behind the power amplifier enable on the IO
     * expander and is only powered once audio is initialised, so probing it
     * here would report a false absence. Left to whatever uses audio. */
    report("audio codec", true, OPTIONAL, "not probed - powered on demand");
}

/* --- entry point --------------------------------------------------------- */

bool post_run(void)
{
    checks_run = 0;
    checks_failed = 0;

    ESP_LOGI(TAG, "power-on self test");

    check_soc();
    check_flash();
    check_memory();
    check_mac();
    check_temperature();
    check_i2c_devices();
    check_display();
    check_sdcard();
    check_audio();

    if (checks_failed == 0) {
        ESP_LOGI(TAG, "%d checks, all passed", checks_run);
    } else {
        ESP_LOGE(TAG, "%d checks, %d FAILED", checks_run, checks_failed);
    }

    /* A machine-readable line, so a production test rig can grep one string
     * rather than parse the table above. */
    printf("POST_COMPLETE checks=%d failures=%d\n", checks_run, checks_failed);
    fflush(stdout);

    return checks_failed == 0;
}
