/*=============================================================================
 * Portable suite: screenshot_bmp_header/screenshot_bmp_row_stride - the pure
 * byte layout of the BMP screenshot_capture() (screenshot.c, device-only)
 * writes.
 *
 * Every expected byte below is spelled out by hand against the
 * BITMAPFILEHEADER/BITMAPINFOHEADER field order rather than derived from the
 * same arithmetic screenshot_bmp_header() uses - a test that reused the
 * implementation's own shifts could carry the same bug and still pass.
 *===========================================================================*/

#include "unity.h"
#include "suites.h"

#include "util/screenshot.h"

static void test_row_stride_already_aligned_needs_no_padding(void)
{
    /* This panel's own width: 368 * 3 = 1104, already a multiple of 4. */
    TEST_ASSERT_EQUAL_INT32(1104, screenshot_bmp_row_stride(368));
}

static void test_row_stride_pads_up_to_next_multiple_of_four(void)
{
    /* width=1 -> 3 bytes of pixel data, padded up to 4. */
    TEST_ASSERT_EQUAL_INT32(4, screenshot_bmp_row_stride(1));
    /* width=2 -> 6 bytes, padded up to 8. */
    TEST_ASSERT_EQUAL_INT32(8, screenshot_bmp_row_stride(2));
    /* width=4 -> 12 bytes, already aligned. */
    TEST_ASSERT_EQUAL_INT32(12, screenshot_bmp_row_stride(4));
}

static void test_header_starts_with_bm_magic(void)
{
    uint8_t header[SCREENSHOT_BMP_HEADER_SIZE];
    screenshot_bmp_header(header, 4, 4);

    TEST_ASSERT_EQUAL_UINT8('B', header[0]);
    TEST_ASSERT_EQUAL_UINT8('M', header[1]);
}

static void test_header_file_size_covers_header_plus_padded_pixels(void)
{
    /* width=1, height=2: two rows, each padded to 4 bytes (see the stride
     * test above) -> 8 bytes of pixel data on top of the 54-byte header. */
    uint8_t header[SCREENSHOT_BMP_HEADER_SIZE];
    screenshot_bmp_header(header, 1, 2);

    const uint32_t expected_file_size = SCREENSHOT_BMP_HEADER_SIZE + 8;
    const uint32_t file_size =
        (uint32_t)header[2]        | ((uint32_t)header[3]  << 8) |
        ((uint32_t)header[4] << 16) | ((uint32_t)header[5] << 24);
    TEST_ASSERT_EQUAL_UINT32(expected_file_size, file_size);

    /* bfOffBits: pixel data always starts right after the fixed-size
     * header, whatever width/height/file size are. */
    const uint32_t off_bits =
        (uint32_t)header[10]        | ((uint32_t)header[11] << 8) |
        ((uint32_t)header[12] << 16) | ((uint32_t)header[13] << 24);
    TEST_ASSERT_EQUAL_UINT32(SCREENSHOT_BMP_HEADER_SIZE, off_bits);
}

static void test_header_dimensions_and_bit_depth(void)
{
    uint8_t header[SCREENSHOT_BMP_HEADER_SIZE];
    screenshot_bmp_header(header, 368, 448);

    const uint32_t width =
        (uint32_t)header[18]        | ((uint32_t)header[19] << 8) |
        ((uint32_t)header[20] << 16) | ((uint32_t)header[21] << 24);
    TEST_ASSERT_EQUAL_UINT32(368, width);

    /* biHeight is POSITIVE - a negative value in a real BMP would mean
     * top-down rows, which is not what screenshot_capture()'s write loop
     * produces (it walks y from GFX_HEIGHT-1 down to 0, i.e. bottom-up). */
    const uint32_t height =
        (uint32_t)header[22]        | ((uint32_t)header[23] << 8) |
        ((uint32_t)header[24] << 16) | ((uint32_t)header[25] << 24);
    TEST_ASSERT_EQUAL_UINT32(448, height);

    const uint16_t planes = (uint16_t)(header[26] | (header[27] << 8));
    TEST_ASSERT_EQUAL_UINT16(1, planes);

    const uint16_t bpp = (uint16_t)(header[28] | (header[29] << 8));
    TEST_ASSERT_EQUAL_UINT16(24, bpp);

    /* biCompression must be BI_RGB (0) - this is an UNCOMPRESSED image,
     * the entire point of choosing BMP over a codec in the first place. */
    const uint32_t compression =
        (uint32_t)header[30]        | ((uint32_t)header[31] << 8) |
        ((uint32_t)header[32] << 16) | ((uint32_t)header[33] << 24);
    TEST_ASSERT_EQUAL_UINT32(0, compression);
}

static void test_header_is_exactly_54_bytes_with_no_trailing_garbage(void)
{
    /* biClrUsed and biClrImportant (the header's last 8 bytes) are always
     * zero for a 24bpp image with no palette - a stray nonzero byte here
     * would be the kind of off-by-one that only shows up as a viewer
     * misreading the image, not a crash. */
    uint8_t header[SCREENSHOT_BMP_HEADER_SIZE];
    screenshot_bmp_header(header, 4, 4);

    const uint8_t expected_tail[8] = { 0, 0, 0, 0, 0, 0, 0, 0 };
    TEST_ASSERT_EQUAL_UINT8_ARRAY(expected_tail, &header[46], 8);
}

/*-----------------------------------------------------------------------------
 * screenshot_base64_encoded_len / screenshot_base64_encode
 *
 * The encode tests use RFC 4648's own worked example ("Man" and its
 * prefixes) plus the standard "f"/"fo"/"foo" padding vectors, rather than
 * anything derived from this file's own arithmetic - the same reasoning
 * suite_gfx_color.c's top comment gives for checking gfx_color_mix()
 * against GFX_RGB(...) constants instead of its own round-trip.
 *---------------------------------------------------------------------------*/

static void test_encoded_len_matches_rfc4648_examples(void)
{
    TEST_ASSERT_EQUAL_INT32(0, screenshot_base64_encoded_len(0));
    TEST_ASSERT_EQUAL_INT32(4, screenshot_base64_encoded_len(1));   /* "f"   -> "Zg==" */
    TEST_ASSERT_EQUAL_INT32(4, screenshot_base64_encoded_len(2));   /* "fo"  -> "Zm8=" */
    TEST_ASSERT_EQUAL_INT32(4, screenshot_base64_encoded_len(3));   /* "foo" -> "Zm9v" */
}

static void test_encode_empty_input_produces_no_bytes(void)
{
    char out[1] = { 'x' };
    screenshot_base64_encode((const uint8_t *)"", 0, out);
    /* Nothing written - out[0] must still hold whatever the caller put
     * there, i.e. this must not have touched it. */
    TEST_ASSERT_EQUAL_CHAR('x', out[0]);
}

static void test_encode_one_byte_pads_with_two_equals(void)
{
    char out[5] = { 0 };
    screenshot_base64_encode((const uint8_t *)"f", 1, out);
    out[4] = '\0';
    TEST_ASSERT_EQUAL_STRING("Zg==", out);
}

static void test_encode_two_bytes_pads_with_one_equals(void)
{
    char out[5] = { 0 };
    screenshot_base64_encode((const uint8_t *)"fo", 2, out);
    out[4] = '\0';
    TEST_ASSERT_EQUAL_STRING("Zm8=", out);
}

static void test_encode_three_bytes_needs_no_padding(void)
{
    char out[5] = { 0 };
    screenshot_base64_encode((const uint8_t *)"foo", 3, out);
    out[4] = '\0';
    TEST_ASSERT_EQUAL_STRING("Zm9v", out);
}

static void test_encode_a_multi_group_string(void)
{
    /* RFC 4648 section 10's own worked example. */
    char out[8] = { 0 };
    screenshot_base64_encode((const uint8_t *)"Man", 3, out);
    out[4] = '\0';
    TEST_ASSERT_EQUAL_STRING("TWFu", out);
}

static void test_encode_two_clean_groups_matches_encoding_them_together(void)
{
    /* The property screenshot_dump() actually leans on: encoding "foobar"
     * as two independent 3-byte calls ("foo" then "bar") must produce the
     * same bytes, in order, as encoding it as one 6-byte call - see
     * screenshot_base64_encode()'s own comment on why a BMP row boundary
     * (always a multiple of 3 here) is safe to split calls on. */
    char whole[9] = { 0 };
    screenshot_base64_encode((const uint8_t *)"foobar", 6, whole);
    whole[8] = '\0';

    char split[9] = { 0 };
    screenshot_base64_encode((const uint8_t *)"foo", 3, split);
    screenshot_base64_encode((const uint8_t *)"bar", 3, split + 4);
    split[8] = '\0';

    TEST_ASSERT_EQUAL_STRING(whole, split);
}

void suite_screenshot(void)
{
    RUN_TEST(test_row_stride_already_aligned_needs_no_padding);
    RUN_TEST(test_row_stride_pads_up_to_next_multiple_of_four);
    RUN_TEST(test_header_starts_with_bm_magic);
    RUN_TEST(test_header_file_size_covers_header_plus_padded_pixels);
    RUN_TEST(test_header_dimensions_and_bit_depth);
    RUN_TEST(test_header_is_exactly_54_bytes_with_no_trailing_garbage);

    RUN_TEST(test_encoded_len_matches_rfc4648_examples);
    RUN_TEST(test_encode_empty_input_produces_no_bytes);
    RUN_TEST(test_encode_one_byte_pads_with_two_equals);
    RUN_TEST(test_encode_two_bytes_pads_with_one_equals);
    RUN_TEST(test_encode_three_bytes_needs_no_padding);
    RUN_TEST(test_encode_a_multi_group_string);
    RUN_TEST(test_encode_two_clean_groups_matches_encoding_them_together);
}

SUITE_REGISTER(suite_screenshot)
