/*=============================================================================
 * screenshot - streaming the current framebuffer to a host script over the
 * console's own serial connection, as an uncompressed 24-bit BMP.
 *
 * There is no other channel off this board: no SD-card-as-USB-drive, no
 * second data port - only the one UART that already carries boot logs and
 * idf_monitor's output (see boot/post.c's SD-card notes for the OTHER
 * shared-bus story on this board; this one shares a wire, not a bus). So a
 * screenshot triggered from a host script has to travel down that same
 * serial link, threaded between whatever else is being logged - see
 * screenshot.c's own top comment for how.
 *
 * Split the way gfx_color.h/gfx.c and ui_style.h/ui.c are: the byte-exact BMP
 * header and the base64 encoding below are pure arithmetic - no BSP, no
 * UART, no framebuffer - so they can be built and checked on a host (see
 * test/suites/suite_screenshot.c). Actually listening on the console and
 * walking the live framebuffer needs the device; that part is screenshot.c,
 * never compiled for a host build.
 *===========================================================================*/
#pragma once

#include <stdbool.h>
#include <stdint.h>

/*-----------------------------------------------------------------------------
 * BMP encoding - see screenshot.c's write loop for how these two are used
 * together to build one row at a time.
 *---------------------------------------------------------------------------*/

/* BITMAPFILEHEADER (14 bytes) + BITMAPINFOHEADER (40 bytes), with no pixel
 * data - see screenshot_bmp_header() below. */
#define SCREENSHOT_BMP_HEADER_SIZE 54

/* Bytes per row once padded to BMP's 4-byte row boundary: width * 3 (24bpp,
 * no alpha), rounded up to the next multiple of 4. */
static inline int32_t screenshot_bmp_row_stride(int32_t width)
{
    return ((width * 3 + 3) / 4) * 4;
}

/* Fills `out[SCREENSHOT_BMP_HEADER_SIZE]` with a BITMAPFILEHEADER followed by
 * a BITMAPINFOHEADER describing an uncompressed, bottom-up (positive
 * biHeight), 24-bit-per-pixel BMP of `width` x `height` pixels - the pixel
 * data itself, screenshot_bmp_row_stride(width) * height bytes of it, is
 * expected to follow immediately after in the stream.
 *
 * Written byte-by-byte in explicit little-endian order rather than through a
 * packed struct: a compiler is free to pad a struct's members for alignment,
 * and BMP's own layout has no padding at all between fields - the two would
 * only agree by accident on any particular compiler/ABI. See
 * BITMAPFILEHEADER / BITMAPINFOHEADER in any BMP format reference for the
 * field order this follows.
 *
 * `width`/`height` are taken as given, not clipped or validated: the one
 * caller (screenshot_dump() in screenshot.c) always passes
 * GFX_WIDTH/GFX_HEIGHT, both small positive constants, so there is nothing
 * here worth guarding against. */
static inline void screenshot_bmp_header(uint8_t out[SCREENSHOT_BMP_HEADER_SIZE],
                                         int32_t width, int32_t height)
{
    const int32_t  stride      = screenshot_bmp_row_stride(width);
    const uint32_t pixel_bytes = (uint32_t)(stride * height);
    const uint32_t file_size   = SCREENSHOT_BMP_HEADER_SIZE + pixel_bytes;

    /* BITMAPFILEHEADER, offsets 0..13 */
    out[0] = 'B'; out[1] = 'M';
    out[2] = (uint8_t)(file_size);
    out[3] = (uint8_t)(file_size >> 8);
    out[4] = (uint8_t)(file_size >> 16);
    out[5] = (uint8_t)(file_size >> 24);
    out[6] = out[7] = out[8] = out[9] = 0;      /* reserved1, reserved2 */
    out[10] = SCREENSHOT_BMP_HEADER_SIZE;       /* bfOffBits: pixels start here */
    out[11] = out[12] = out[13] = 0;

    /* BITMAPINFOHEADER, offsets 14..53 */
    out[14] = 40; out[15] = out[16] = out[17] = 0;   /* biSize */
    out[18] = (uint8_t)(width);
    out[19] = (uint8_t)(width >> 8);
    out[20] = (uint8_t)(width >> 16);
    out[21] = (uint8_t)(width >> 24);
    out[22] = (uint8_t)(height);                     /* positive: bottom-up rows */
    out[23] = (uint8_t)(height >> 8);
    out[24] = (uint8_t)(height >> 16);
    out[25] = (uint8_t)(height >> 24);
    out[26] = 1; out[27] = 0;                        /* biPlanes = 1 */
    out[28] = 24; out[29] = 0;                       /* biBitCount = 24 */
    out[30] = out[31] = out[32] = out[33] = 0;       /* biCompression = BI_RGB */
    out[34] = (uint8_t)(pixel_bytes);
    out[35] = (uint8_t)(pixel_bytes >> 8);
    out[36] = (uint8_t)(pixel_bytes >> 16);
    out[37] = (uint8_t)(pixel_bytes >> 24);
    out[38] = out[39] = out[40] = out[41] = 0;       /* biXPelsPerMeter */
    out[42] = out[43] = out[44] = out[45] = 0;       /* biYPelsPerMeter */
    out[46] = out[47] = out[48] = out[49] = 0;       /* biClrUsed */
    out[50] = out[51] = out[52] = out[53] = 0;       /* biClrImportant */
}

/*-----------------------------------------------------------------------------
 * Base64 - the console UART carries text (ESP_LOG lines, the REPL a human
 * might be typing into), so the framebuffer's raw bytes cannot go down it
 * unescaped: a stray 0x0A in pixel data would look like a line break, and
 * plenty of byte values are not valid UTF-8 on their own, which is how
 * idf_monitor's own decoding is configured. Base64 is the standard fix -
 * every byte that comes out is printable ASCII - and at 4 output bytes per 3
 * input bytes it costs a third more over the wire than a hex dump would cost
 * two thirds more, which matters at 115200 baud for a 322 KiB frame.
 *
 * RFC 4648, no line breaks of its own (screenshot.c adds those, one encoded
 * chunk per printed line) and '=' padding for a trailing partial group.
 *---------------------------------------------------------------------------*/

/* How many bytes screenshot_base64_encode() writes for `len` input bytes -
 * NOT including a NUL terminator, which callers wanting a C string must
 * budget for separately. */
static inline int32_t screenshot_base64_encoded_len(int32_t len)
{
    return ((len + 2) / 3) * 4;
}

/* Encodes `len` bytes at `in` into `out`, which must hold at least
 * screenshot_base64_encoded_len(len) bytes. Does not NUL-terminate.
 *
 * Pure - no I/O, no chunking state carried between calls - which is what
 * lets screenshot.c call it once per BMP row independently rather than
 * threading a leftover-bytes accumulator through the whole frame: every row
 * screenshot_dump() builds is a multiple of 3 bytes (GFX_WIDTH * 3, and
 * SCREENSHOT_BMP_HEADER_SIZE for the header before it), so each call here
 * ends on a clean group boundary and produces output identical to encoding
 * the whole stream at once - the '=' padding a partial trailing group would
 * need never actually occurs in practice for this caller. Tested for a
 * width where it DOES occur regardless (suite_screenshot.c), since a pure
 * function's contract should not depend on how its one real caller happens
 * to use it. */
static inline void screenshot_base64_encode(const uint8_t *in, int32_t len, char *out)
{
    static const char table[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

    int32_t i = 0, o = 0;
    for (; i + 3 <= len; i += 3) {
        const uint32_t v = ((uint32_t)in[i] << 16) | ((uint32_t)in[i + 1] << 8) | in[i + 2];
        out[o++] = table[(v >> 18) & 0x3F];
        out[o++] = table[(v >> 12) & 0x3F];
        out[o++] = table[(v >> 6)  & 0x3F];
        out[o++] = table[v & 0x3F];
    }

    const int32_t rem = len - i;
    if (rem == 1) {
        const uint32_t v = (uint32_t)in[i] << 16;
        out[o++] = table[(v >> 18) & 0x3F];
        out[o++] = table[(v >> 12) & 0x3F];
        out[o++] = '=';
        out[o++] = '=';
    } else if (rem == 2) {
        const uint32_t v = ((uint32_t)in[i] << 16) | ((uint32_t)in[i + 1] << 8);
        out[o++] = table[(v >> 18) & 0x3F];
        out[o++] = table[(v >> 12) & 0x3F];
        out[o++] = table[(v >> 6)  & 0x3F];
        out[o++] = '=';
    }
}

/*-----------------------------------------------------------------------------
 * The device-only half - see screenshot.c
 *---------------------------------------------------------------------------*/

/* Starts the background task that listens on the console's UART for a
 * capture request. Call once, from app_main() - the same place and the same
 * pattern as touch_start()/buttons_start(): a small dedicated task the shell
 * never talks to directly, its result read back out through the accessor
 * below instead. */
void screenshot_start(void);

/* Reads and clears whether the listener task has seen a request since the
 * last call - the same "read and consume once per frame" contract
 * buttons_read() already uses (see input/buttons.h), and for the same
 * reason: main.c's loop is the only place that should act on a request, and
 * only once per request, however many frames it takes main.c to get back
 * around to checking. */
bool screenshot_take_request(void);

/* Streams the current framebuffer to stdout as base64 BMP data, framed
 * between SCREENSHOT_BEGIN/SCREENSHOT_END marker lines a host script greps
 * for - see tools/screenshot.py. Meant to be called from main.c's frame
 * loop right after a frame is drawn and before it is presented, so what is
 * captured is exactly what is about to appear on screen (see gfx.h's own
 * dirty-tracking comment: gfx_present() only sends what changed, but the
 * full framebuffer this reads from is always complete regardless). */
void screenshot_dump(void);
