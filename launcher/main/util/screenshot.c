/*=============================================================================
 * screenshot.c - the device-only half: listens on the console for a
 * one-word trigger, and on request walks the live framebuffer through
 * gfx_color_rgb888() and screenshot_base64_encode(), printing the result
 * between marker lines a host script (tools/screenshot.py) reads back out
 * of the same stream idf_monitor/ESP_LOG already use.
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
#include <string.h>

#include "driver/usb_serial_jtag.h"
#include "driver/usb_serial_jtag_vfs.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "gfx/gfx.h"

static const char *TAG = "screenshot";

#define SCREENSHOT_TRIGGER "SCREENSHOT"

static volatile bool s_request_pending;

/* Reads lines from stdin forever, setting s_request_pending on an exact
 * match. Runs at a low priority (below touch/buttons - see input/touch.c,
 * input/buttons.c for their own 6/5) since it spends essentially all its
 * time blocked waiting on bytes nobody is usually sending; when a line does
 * arrive there is nothing time-critical about noticing it a frame or two
 * later.
 *
 * A line over SCREENSHOT_LINE_MAX is never going to match the trigger -
 * dropped by resetting `len`, not by growing the buffer, so a host
 * accidentally in the wrong mode (pasting binary, say) cannot run this off
 * the end of a fixed buffer. */
#define SCREENSHOT_LINE_MAX 32

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

    ESP_LOGI(TAG, "listening for '%s' on the console", SCREENSHOT_TRIGGER);
}

bool screenshot_take_request(void)
{
    if (!s_request_pending) {
        return false;
    }
    s_request_pending = false;
    return true;
}

/* Static, not stack-local: screenshot_dump() runs on main.c's shell task,
 * which has a 3584-byte stack (CONFIG_ESP_MAIN_TASK_STACK_SIZE) shared with
 * everything else that task does. A 1104-byte row plus its 1472-byte
 * base64 encoding would be most of that budget on top of whatever printf
 * and ESP_LOG already use internally - see gfx.h's own top comment on why
 * this board's RAM is counted this closely everywhere else in the shell. */
static uint8_t row[GFX_WIDTH * 3];
static char    row_b64[GFX_WIDTH * 4 + 1];   /* +1: NUL, for printf("%s") */

void screenshot_dump(void)
{
    const int32_t  stride      = screenshot_bmp_row_stride(GFX_WIDTH);
    const uint32_t pixel_bytes = (uint32_t)(stride * GFX_HEIGHT);
    const uint32_t total_bytes = SCREENSHOT_BMP_HEADER_SIZE + pixel_bytes;

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
        screenshot_base64_encode(row, sizeof row, row_b64);
        row_b64[sizeof(row_b64) - 1] = '\0';
        printf("SCREENSHOT_DATA:%s\n", row_b64);
    }

    printf("SCREENSHOT_END\n");
    fflush(stdout);
}
