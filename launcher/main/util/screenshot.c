/*=============================================================================
 * screenshot.c - the device-only half: listens on the console's UART for a
 * one-word trigger, and on request walks the live framebuffer through
 * gfx_color_rgb888() and screenshot_base64_encode(), printing the result
 * between marker lines a host script (tools/screenshot.py) reads back out
 * of the same stream idf_monitor/ESP_LOG already use.
 *
 * WHY A TASK, NOT AN ISR OR A POLL IN main.c's LOOP
 *
 * The default console UART reader (uart_vfs.c's ROM-polling path) is
 * non-blocking by construction - it returns "no data" instantly rather than
 * waiting, which is fine for a REPL that also has other work to do but
 * would mean main.c's render loop busy-polling every frame just to ask "did
 * anyone type SCREENSHOT yet", 40-plus thousand times a minute at this
 * shell's frame rate. screenshot_start() below switches the console fd onto
 * the UART driver's interrupt-driven reader instead (uart_vfs_dev_use_driver
 * - the exact call ESP-IDF's own esp_console REPL makes internally, for the
 * exact same reason), which is what makes a genuinely blocking read
 * possible, and gives that blocking read its own small task rather than
 * stalling anything else.
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

#include "driver/uart.h"
#include "driver/uart_vfs.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "gfx/gfx.h"

static const char *TAG = "screenshot";

#define SCREENSHOT_UART    UART_NUM_0
#define SCREENSHOT_TRIGGER "SCREENSHOT"

static volatile bool s_request_pending;

/* Reads lines from stdin forever, setting s_request_pending on an exact
 * match. Runs at a low priority (below touch/buttons - see input/touch.c,
 * input/buttons.c for their own 6/5) since it spends essentially all its
 * time blocked waiting on UART bytes nobody is usually sending; when a line
 * does arrive there is nothing time-critical about noticing it a frame or
 * two later.
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

        if (c == '\n') {
            if (len > 0) {
                line[len] = '\0';
                if (strcmp(line, SCREENSHOT_TRIGGER) == 0) {
                    s_request_pending = true;
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
    /* Matches ESP-IDF's own esp_console_repl_chip.c UART setup exactly
     * (rx_buffer_size 256, no tx ring buffer, no event queue) - that
     * component solves the identical problem (a blocking read on the
     * console UART without disturbing normal stdout logging) and this
     * reuses its proven call sequence rather than a hand-guessed one. */
    const uart_config_t cfg = {
        .baud_rate  = CONFIG_ESP_CONSOLE_UART_BAUDRATE,
        .data_bits  = UART_DATA_8_BITS,
        .parity     = UART_PARITY_DISABLE,
        .stop_bits  = UART_STOP_BITS_1,
        .flow_ctrl  = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    uart_param_config(SCREENSHOT_UART, &cfg);
    uart_set_pin(SCREENSHOT_UART, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE,
                 UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    uart_driver_install(SCREENSHOT_UART, 256, 0, 0, NULL, 0);
    uart_vfs_dev_use_driver(SCREENSHOT_UART);

    /* No CR/LF translation on the way in: tools/screenshot.py sends a bare
     * '\n' after the trigger word, and this is what makes that arrive at
     * screenshot_task() unchanged instead of being remapped as if a
     * terminal's Enter key (CR) had produced it. */
    uart_vfs_dev_port_set_rx_line_endings(SCREENSHOT_UART, ESP_LINE_ENDINGS_LF);

    xTaskCreate(screenshot_task, "screenshot", 3072, NULL, 4, NULL);
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
