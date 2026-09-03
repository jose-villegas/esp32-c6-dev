/*=============================================================================
 * screenshot.c - the device-only half: listens on the console for a
 * one-word trigger, and on request walks the live framebuffer through
 * gfx_color_rgb888() and screenshot_base64_encode(), printing the result
 * between marker lines a host script (tools/screenshot.py) reads back out
 * of the same stream idf_monitor/ESP_LOG already use.
 *
 * ALSO OWNS RUNSUITE, A SECOND, UNRELATED COMMAND
 *
 * CONFIG_LAUNCHER_SELFTEST only: "RUNSUITE <name>" runs exactly one
 * registered suite (suites_run_one() in test/suites.c) instead of
 * suites_run_all()'s everything-at-boot run - the targeted way to see one
 * suite's own report (a perf suite's especially) without waiting through
 * whatever else registered ahead of it alphabetically first. It lives
 * here, in a file otherwise about screenshots, rather than in its own
 * listener, because the console can only have one blocking reader:
 * usb_serial_jtag_vfs_use_driver() below hands this task exclusive,
 * interrupt-driven ownership of stdin, and a second task calling fgetc()
 * on the same stream would race it for every incoming byte. One line
 * listener, two commands - not a generic, registrable dispatch table,
 * since two is what this project actually has today (see CLAUDE.md on
 * designing for hypothetical future requirements).
 *

 * WHY USB-SERIAL-JTAG, NOT UART
 *
 * This board's one USB-C port is the ESP32-C6's own native USB-Serial/JTAG
 * peripheral, not an external bridge chip wired to UART0 - see
 * sdkconfig.defaults' own comment for the full story (Waveshare's docs say
 * so directly, and CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG=y there is what makes
 * it the primary console channel - the one that is actually read as well as
 * written). Everything below targets that peripheral for exactly the same
 * reason the first version of this file targeted UART0: match whatever the
 * console's own primary channel is, so this listener sees the same bytes
 * idf_monitor does.
 *
 * WHY A TASK, NOT AN ISR OR A POLL IN main.c's LOOP
 *
 * The default console reader (usb_serial_jtag_vfs.c's non-blocking path) is
 * non-blocking by construction - it returns "no data" instantly rather than
 * waiting, which is fine for code that also has other work to do but would
 * mean main.c's render loop busy-polling every frame just to ask "did
 * anyone type SCREENSHOT yet", 40-plus thousand times a minute at this
 * shell's frame rate. screenshot_start() below switches the console fd onto
 * the driver's interrupt-driven reader instead (usb_serial_jtag_vfs_use_driver
 * - the same call ESP-IDF's own esp_console REPL makes internally for this
 * peripheral, for the exact same reason), which is what makes a genuinely
 * blocking read possible, and gives that blocking read its own small task
 * rather than stalling anything else.
 *
 * WHY THE RESULT COMES BACK THROUGH A FLAG, NOT A DIRECT CALL
 *
 * screenshot_task() below could call screenshot_dump() itself the instant
 * the trigger line arrives, but the framebuffer it would be reading is
 * whatever the render loop happens to have half-drawn at that exact
 * instant - there is no lock between the two tasks. Going through
 * screenshot_take_request()/main.c's loop instead means the capture always
 * happens at a clean frame boundary, after step_app() has finished drawing
 * and before gfx_present() sends it - the same reasoning main.c already
 * applies to display_update() (see its own comment on why that runs ahead
 * of step_app() rather than whenever the IMU happens to be read).
 *===========================================================================*/
#include "util/screenshot.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "driver/usb_serial_jtag.h"
#include "driver/usb_serial_jtag_vfs.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "gfx/gfx.h"
#include "util/device_state.h"

#if CONFIG_LAUNCHER_SELFTEST
#include "suites.h"
#endif

static const char *TAG = "screenshot";

#define SCREENSHOT_TRIGGER "SCREENSHOT"

#if CONFIG_LAUNCHER_SELFTEST
#define RUNSUITE_TRIGGER "RUNSUITE "
#endif

static volatile bool s_request_pending;

/* Reads lines from stdin forever, setting s_request_pending on an exact
 * match. Runs at a low priority (below touch/buttons - see input/touch.c,
 * input/buttons.c for their own 6/5) since it spends essentially all its
 * time blocked waiting on bytes nobody is usually sending; when a line does
 * arrive there is nothing time-critical about noticing it a frame or two
 * later.
 *
 * A line over SCREENSHOT_LINE_MAX is never going to match either trigger -
 * dropped by resetting `len`, not by growing the buffer, so a host
 * accidentally in the wrong mode (pasting binary, say) cannot run this off
 * the end of a fixed buffer. Sized for RUNSUITE_TRIGGER plus the longest
 * suite name today (run_boot_anim_perf_suite, 25 chars) with real headroom
 * for names not yet written - bump this rather than trim a name to fit it,
 * the same "headroom, not a tight fit" reasoning SUITE_MAX already states. */
#define SCREENSHOT_LINE_MAX 48

static void screenshot_task(void *arg)
{
    (void)arg;
    char line[SCREENSHOT_LINE_MAX];
    int len = 0;

    while (1) {
        const int c = fgetc(stdin);
        if (c == EOF) {
            /* Should not happen once the driver is installed (fgetc blocks
             * until a byte arrives) - guarded anyway so a console detached
             * mid-run degrades to a slow poll instead of a spin loop. */
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }

        if (c == '\n' || c == '\r') {
            /* Either terminator ends a line - tools/screenshot.py sends a
             * bare '\n', but treating '\r' the same way means a line typed
             * by hand into monitor.sh (whose Enter key may send either,
             * depending on platform) still reaches the strcmp() below,
             * which is what makes "type SCREENSHOT into monitor.sh" a valid
             * way to test this listener in isolation from the host script. */
            if (len > 0) {
                line[len] = '\0';
                if (strcmp(line, SCREENSHOT_TRIGGER) == 0) {
                    ESP_LOGI(TAG, "trigger received");
                    s_request_pending = true;
#if CONFIG_LAUNCHER_SELFTEST
                } else if (strncmp(line, RUNSUITE_TRIGGER,
                                   strlen(RUNSUITE_TRIGGER)) == 0) {
                    const char *name = line + strlen(RUNSUITE_TRIGGER);
                    ESP_LOGI(TAG, "RUNSUITE %s", name);
                    if (!suites_run_one(name)) {
                        ESP_LOGE(TAG, "no suite named '%s' is registered",
                                 name);
                    }
#endif
                } else {
                    ESP_LOGI(TAG, "ignoring line: '%s'", line);
                }
                len = 0;
            }
            continue;
        }

        if (len < SCREENSHOT_LINE_MAX - 1) {
            line[len++] = (char)c;
        } else {
            len = 0;
        }
    }
}

void screenshot_start(void)
{
    usb_serial_jtag_driver_config_t cfg = USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();
    const esp_err_t err = usb_serial_jtag_driver_install(&cfg);
    if (err != ESP_OK) {
        /* The one most worth calling out by name: this is what happens if
         * something else already installed this driver before
         * screenshot_start() ran (ESP_ERR_INVALID_STATE) - silently
         * leaving the console on its default non-blocking reader, which
         * looks from the host exactly like a request that vanished into
         * nothing rather than a boot-time failure. */
        ESP_LOGE(TAG, "usb_serial_jtag_driver_install failed: %s - listener not started",
                 esp_err_to_name(err));
        return;
    }

    usb_serial_jtag_vfs_use_driver();

    /* Either terminator accepted on the way in - see screenshot_task()'s own
     * comment on why '\r' is treated the same as '\n' there. Left at LF here
     * (no translation) rather than switched to CR/CRLF: translating would
     * only rewrite '\r' into '\n' before screenshot_task() ever sees it,
     * which the task already does itself, and leaving translation off means
     * a stray '\r' from either source arrives unchanged instead of being
     * silently turned into two line endings for one keypress. */
    usb_serial_jtag_vfs_set_rx_line_endings(ESP_LINE_ENDINGS_LF);

    const BaseType_t created =
        xTaskCreate(screenshot_task, "screenshot", 3072, NULL, 4, NULL);
    if (created != pdPASS) {
        ESP_LOGE(TAG, "xTaskCreate failed (out of memory?) - listener not started");
        return;
    }

#if CONFIG_LAUNCHER_SELFTEST
    ESP_LOGI(TAG, "listening for '%s' and '%s<name>' on the console",
             SCREENSHOT_TRIGGER, RUNSUITE_TRIGGER);
#else
    ESP_LOGI(TAG, "listening for '%s' on the console", SCREENSHOT_TRIGGER);
#endif
}

bool screenshot_take_request(void)
{
    if (!s_request_pending) {
        return false;
    }
    s_request_pending = false;
    return true;
}

/* Not stack-local: screenshot_dump() runs on main.c's shell task, which has
 * a 3584-byte stack (CONFIG_ESP_MAIN_TASK_STACK_SIZE) shared with everything
 * else that task does. A 1104-byte row plus its 1472-byte base64 encoding
 * would be most of that budget on top of whatever printf and ESP_LOG
 * already use internally - see gfx.h's own top comment on why this board's
 * RAM is counted this closely everywhere else in the shell.
 *
 * Not permanently static either, as they were before: malloc'd in
 * screenshot_dump() and freed before it returns, so these 2,577 bytes are
 * only ever reserved for the duration of an actual capture instead of for
 * the whole life of the process. A CONFIG_LAUNCHER_DEVELOPMENT build (this
 * file is compiled into nothing else) carries them at every boot whether or
 * not a screenshot is ever taken - static here competed directly with
 * app_sand.c's grid for the single largest contiguous heap block it needs
 * (see that allocation's own comment), which is exactly the failure a --dev
 * build hit opening the sand app. */
static uint8_t *row;
static char    *row_b64;   /* +1: NUL, for printf("%s") */

/* How much room an app's diagnostic_json() fragment is given - see app_t's
 * own comment in app.h for what it may contain. Generous relative to what
 * either existing implementation (app_sand.c's) actually uses, on the same
 * reasoning DEVICE_STATE_JSON_MAX budgets headroom rather than a tight fit -
 * this is a diagnostic path, not one worth re-deriving an exact bound for. */
#define APP_DIAGNOSTIC_JSON_MAX 256

/* Prints one SCREENSHOT_STATE: line of plain-text JSON (no base64 - it is
 * already printable ASCII, and small enough next to the image that the
 * base64 encoding's whole reason to exist - staying inside a UART-safe
 * byte range - is not worth the extra decode step on the host for this
 * one line) describing device state at this same frame. The reading and
 * the formatting both live in util/device_state.h/.c - see that module's
 * own comment for the field list and why it is split out rather than
 * living here.
 *
 * `current_app`'s OPTIONAL diagnostic_json() (app_t, app.h) is spliced in
 * as an "app" key AFTER device_state_format_json() has already produced a
 * complete, valid JSON object - by overwriting that object's closing `}`
 * with `,"app":<fragment>}` rather than teaching device_state.h anything
 * about apps at all. device_state.h stays exactly what its own top comment
 * says it is: board state, nothing else. */
static void dump_state(const input_t *input, const app_t *current_app)
{
    device_state_t state;
    device_state_read(&state);

    char json[DEVICE_STATE_JSON_MAX];
    device_state_format_json(&state, input, json);

    if (current_app != NULL && current_app->diagnostic_json != NULL) {
        char app_json[APP_DIAGNOSTIC_JSON_MAX];
        current_app->diagnostic_json(app_json, sizeof app_json);

        const size_t len = strlen(json);
        /* json[len-1] is device_state_format_json()'s own closing `}` -
         * always present, since that function always emits a complete
         * object. Only splice if there is genuinely room for the fragment
         * plus the `,"app":` wrapper plus the new closing `}` - a
         * truncated app fragment would rather be dropped than emitted as
         * broken JSON the host script's json.loads() then rejects
         * outright, losing the WHOLE line (device state included, not
         * just the app part) rather than only the addition. */
        if (len > 0 && json[len - 1] == '}' &&
            len - 1 + strlen(",\"app\":") + strlen(app_json) + 1
                < sizeof json) {
            snprintf(json + len - 1, sizeof(json) - (len - 1),
                     ",\"app\":%s}", app_json);
        }
    }

    printf("SCREENSHOT_STATE:%s\n", json);
}

void screenshot_dump(const input_t *input, const app_t *current_app)
{
    const int32_t  stride      = screenshot_bmp_row_stride(GFX_WIDTH);
    const uint32_t pixel_bytes = (uint32_t)(stride * GFX_HEIGHT);
    const uint32_t total_bytes = SCREENSHOT_BMP_HEADER_SIZE + pixel_bytes;

    /* sizes named rather than re-derived from sizeof(row)/sizeof(row_b64)
     * below: row/row_b64 are pointers now (see their own declaration
     * comment), so sizeof on them would give the pointer's own size, not
     * the buffer's. */
    const size_t row_bytes     = (size_t)GFX_WIDTH * 3;
    const size_t row_b64_bytes = (size_t)GFX_WIDTH * 4 + 1;

    row = malloc(row_bytes);
    row_b64 = malloc(row_b64_bytes);
    if (row == NULL || row_b64 == NULL) {
        ESP_LOGE(TAG, "could not allocate %u+%u-byte row buffers - "
                      "screenshot skipped; largest free block is %u",
                 (unsigned)row_bytes, (unsigned)row_b64_bytes,
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
        free(row);
        free(row_b64);
        row = NULL;
        row_b64 = NULL;
        return;
    }

    ESP_LOGI(TAG, "streaming %lu bytes to the console", (unsigned long)total_bytes);

    /* The marker and data lines are plain printf(), not ESP_LOGx: a log
     * line carries a "I (12345) TAG: " prefix (see boot/post.c's report()
     * for an ordinary use of that prefix) that tools/screenshot.py would
     * otherwise have to strip back off before the fixed-prefix match it
     * does on every line - simpler for both ends to keep the protocol's
     * own lines free of it from the start. */
    printf("SCREENSHOT_BEGIN size=%lu\n", (unsigned long)total_bytes);

    uint8_t header[SCREENSHOT_BMP_HEADER_SIZE];
    screenshot_bmp_header(header, GFX_WIDTH, GFX_HEIGHT);
    /* 54 bytes -> 72 base64 chars (SCREENSHOT_BMP_HEADER_SIZE is a multiple
     * of 3, so this is exact with no padding - see screenshot_base64_encode()'s
     * own comment on why that property matters here). */
    char header_b64[72 + 1];
    screenshot_base64_encode(header, sizeof header, header_b64);
    header_b64[sizeof(header_b64) - 1] = '\0';
    printf("SCREENSHOT_DATA:%s\n", header_b64);

    const gfx_color_t *fb = gfx_framebuffer();

    /* Bottom-to-top, matching the bottom-up rows screenshot_bmp_header()
     * declares (positive biHeight) - see that function's own comment. */
    for (int32_t y = GFX_HEIGHT - 1; y >= 0; y--) {
        const gfx_color_t *src_row = fb + (size_t)y * GFX_WIDTH;
        for (int32_t x = 0; x < GFX_WIDTH; x++) {
            /* gfx_color_rgb888() is the panel-format-to-0xRRGGBB conversion
             * gfx_color.h already carries and tests (suite_gfx_color.c) -
             * reused rather than re-deriving the byte swap and channel
             * widths here. BMP's own pixel order is B, G, R. */
            const uint32_t rgb = gfx_color_rgb888(src_row[x]);
            row[x * 3 + 0] = (uint8_t)(rgb);
            row[x * 3 + 1] = (uint8_t)(rgb >> 8);
            row[x * 3 + 2] = (uint8_t)(rgb >> 16);
        }
        screenshot_base64_encode(row, row_bytes, row_b64);
        row_b64[row_b64_bytes - 1] = '\0';
        printf("SCREENSHOT_DATA:%s\n", row_b64);
    }

    dump_state(input, current_app);

    printf("SCREENSHOT_END\n");
    fflush(stdout);

    free(row);
    free(row_b64);
    row = NULL;
    row_b64 = NULL;
}
