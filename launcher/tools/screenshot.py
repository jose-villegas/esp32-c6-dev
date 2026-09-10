"""Request a screenshot from the device and save it as a lossless .png.

Invoked by screenshot.sh. Kept in Python because pyserial ships inside
ESP-IDF's environment and behaves the same on every platform, which a shell
script reading a serial port directly does not - the same reasoning
test/collect_device_results.py's own top comment gives for the identical
split there.

Sends the trigger word over the console UART (see util/screenshot.c) and
reads the response back out of the same stream idf_monitor would otherwise
be showing as logs: a SCREENSHOT_BEGIN line announcing the byte count, one
SCREENSHOT_DATA: line per base64-encoded chunk, one SCREENSHOT_STATE: line
of plain-text JSON (device state at that same frame - sensors, memory,
clock; see screenshot_dump()'s own comment in screenshot.c for the field
list), and a SCREENSHOT_END line. Anything else on the wire - ordinary
ESP_LOG output, in particular - is ignored rather than treated as an
error, since the device keeps logging normally while it streams.

The device streams its frame as a 24bpp BMP (see screenshot_bmp_header() in
util/screenshot.h) - the simplest thing to emit from a microcontroller with
no image library on it - but nothing here ever writes that BMP to disk:
bmp_bytes_to_png() below converts it to PNG entirely in memory, and `--out`
gets only the PNG. This is genuinely lossless, not just smaller - PNG's
compression is DEFLATE, the same as zlib/gzip, so every pixel round-trips
exactly; this is not JPEG. Standard library only (zlib + struct), no
Pillow - Pillow is not installed in the ESP-IDF python env this script
actually runs under, so depending on it used to mean silently getting no
image at all. Also writes a same-named .json beside the .png if a
SCREENSHOT_STATE: line arrived.
"""

import argparse
import base64
import json
import os
import re
import struct
import sys
import time
import zlib

import serial

BEGIN_RE = re.compile(r"^SCREENSHOT_BEGIN size=(\d+)$")
DATA_PREFIX = "SCREENSHOT_DATA:"
STATE_PREFIX = "SCREENSHOT_STATE:"
END_LINE = "SCREENSHOT_END"

TRIGGER = b"SCREENSHOT\n"


def _png_chunk(tag: bytes, data: bytes) -> bytes:
    """One PNG chunk: 4-byte big-endian length, the 4-byte type, the data,
    then a big-endian CRC32 over type+data - the whole file is just these
    end to end after the fixed 8-byte signature."""
    return (struct.pack(">I", len(data)) + tag + data +
            struct.pack(">I", zlib.crc32(tag + data) & 0xffffffff))


def bmp_bytes_to_png(bmp: bytes) -> bytes:
    """Converts an in-memory 24bpp BMP - the exact bytes screenshot_dump()
    streams, see screenshot_bmp_header()'s own comment for the byte layout
    - into an in-memory PNG. No Pillow, no temp file: a minimal PNG is just
    the 8-byte signature, an IHDR chunk, one IDAT chunk holding
    zlib.compress() of the raw scanlines (each prefixed with a filter byte;
    0 = "None" is correct and simplest here), and an empty IEND chunk.

    Two orderings BMP and PNG disagree on, both handled below: BMP stores
    rows bottom-up (screenshot_bmp_header() always writes a positive
    biHeight - see its own comment) while PNG wants top-down, and BMP's
    pixel order is B,G,R while PNG wants R,G,B. Getting either backwards
    produces an image that LOOKS like a real screenshot - upside-down, or
    blue-tinted - which is worse than no image at all.

    Width/height/row-stride are read out of the BMP header rather than
    assumed, so this keeps working unchanged if the panel resolution ever
    does - the same "trust what the device announced" reasoning the
    SCREENSHOT_BEGIN size check in main() already applies.
    """
    if bmp[0:2] != b"BM":
        raise ValueError("not a BMP: missing the 'BM' signature")

    pixel_offset, = struct.unpack_from("<I", bmp, 10)
    width, height = struct.unpack_from("<ii", bmp, 18)
    bits_per_pixel, = struct.unpack_from("<H", bmp, 28)
    if bits_per_pixel != 24:
        raise ValueError(f"expected a 24bpp BMP, got {bits_per_pixel}bpp")
    if height <= 0:
        # screenshot_bmp_header() only ever writes a positive (bottom-up)
        # height - a value <= 0 here means this isn't this tool's BMP.
        raise ValueError(f"expected a positive (bottom-up) height, got {height}")

    stride = ((width * 3 + 3) // 4) * 4     # BMP pads every row to 4 bytes

    scanlines = []
    for image_row in range(height):
        # BMP's first stored row is the BOTTOM of the image; PNG's first
        # written row is the TOP - so PNG row N reads BMP's stored row
        # (height - 1 - N).
        bmp_row = height - 1 - image_row
        row_start = pixel_offset + bmp_row * stride
        bgr = bmp[row_start:row_start + width * 3]

        rgb = bytearray(len(bgr))
        rgb[0::3], rgb[1::3], rgb[2::3] = bgr[2::3], bgr[1::3], bgr[0::3]
        scanlines.append(b"\x00" + bytes(rgb))    # filter byte 0 = None

    raw = b"".join(scanlines)

    png = b"\x89PNG\r\n\x1a\n"
    png += _png_chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0))
    png += _png_chunk(b"IDAT", zlib.compress(raw, 6))
    png += _png_chunk(b"IEND", b"")
    return png


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", required=True)
    ap.add_argument("--baud", type=int, default=115200)
    ap.add_argument("--out", required=True,
                    help="output path; any extension given is replaced "
                         "with .png (the .json state snapshot lands beside "
                         "it under the same stem)")
    ap.add_argument("--timeout", type=float, default=90.0,
                    help="seconds to wait for the whole capture to arrive - "
                         "a full 368x448 frame is roughly 650 KB of base64 "
                         "over a 115200-baud link, which takes the better "
                         "part of a minute on its own")
    args = ap.parse_args()

    # Two-step open, NOT serial.Serial(port, baud) directly: this board's
    # USB-serial bridge treats a DTR/RTS transition as a reset pulse, the
    # same auto-reset circuit esptool.py's flashing sequence relies on -
    # and resetting the board is exactly what this tool must NOT do, since
    # the whole point is to capture whatever app is ALREADY on screen, not
    # whatever the boot animation happens to be drawing a moment later.
    # Setting dtr/rts before open() is what keeps them from toggling during
    # it, the standard fix for attaching to a running board without
    # restarting it.
    port = serial.Serial()
    port.port = args.port
    port.baudrate = args.baud
    port.timeout = 0.2
    port.dtr = False
    port.rts = False

    try:
        port.open()
    except serial.SerialException as exc:
        print(f"could not open {args.port}: {exc}", file=sys.stderr)
        print("is the firmware running, and no other program "
              "(idf_monitor, another screenshot.sh, ...) already "
              "holding the port?", file=sys.stderr)
        return 2

    deadline = time.monotonic() + args.timeout
    buffer = ""
    total_size = None
    chunks = []
    received_b64_chars = 0
    state_json = None

    # Printed immediately, before anything blocks: without this, a slow but
    # perfectly healthy capture (see --timeout's own help text above) prints
    # nothing at all until it either finishes or times out, and looks
    # identical to a genuine hang for the whole minute in between.
    print(f"sent trigger, waiting for the device "
          f"(up to {args.timeout:g}s for a full frame)...",
          file=sys.stderr, flush=True)
    last_progress = time.monotonic()

    with port:
        port.reset_input_buffer()
        port.write(TRIGGER)
        port.flush()
        last_trigger_sent = time.monotonic()

        while time.monotonic() < deadline:
            chunk = port.read(4096)
            if chunk:
                buffer += chunk.decode("utf-8", errors="replace")

            now = time.monotonic()

            # Resend the trigger periodically until SCREENSHOT_BEGIN shows
            # up. A single lost byte is easy to hit right after flashing:
            # flashing resets the board, and if this runs before the
            # firmware has gotten through POST and the boot animation to
            # install its UART listener, the one-shot trigger arrives
            # before anything is reading for it and is gone for good -
            # otherwise indistinguishable from a genuine hang, since the
            # firmware comes up moments later in a perfectly normal,
            # listening state that will happily answer the NEXT one.
            if total_size is None and now - last_trigger_sent >= 5.0:
                print("  ... no response yet, resending trigger (the device "
                      "may still be booting)", file=sys.stderr, flush=True)
                port.write(TRIGGER)
                port.flush()
                last_trigger_sent = now

            if now - last_progress >= 3.0:
                if total_size is None:
                    print("  ... still waiting for SCREENSHOT_BEGIN "
                          "(nothing recognizable seen yet)",
                          file=sys.stderr, flush=True)
                else:
                    received_bytes = received_b64_chars * 3 // 4
                    pct = min(100, received_bytes * 100 // max(total_size, 1))
                    print(f"  ... {pct}% ({received_bytes}/{total_size} bytes)",
                          file=sys.stderr, flush=True)
                last_progress = now

            while "\n" in buffer:
                line, buffer = buffer.split("\n", 1)
                line = line.rstrip("\r")

                if total_size is None:
                    m = BEGIN_RE.match(line)
                    if m:
                        total_size = int(m.group(1))
                        print(f"  capturing {total_size} bytes...",
                              file=sys.stderr, flush=True)
                    continue

                if line == END_LINE:
                    data = base64.b64decode("".join(chunks))
                    if len(data) != total_size:
                        print(f"warning: decoded {len(data)} bytes but the "
                              f"device announced {total_size}", file=sys.stderr)

                    png = bmp_bytes_to_png(data)
                    png_path = os.path.splitext(args.out)[0] + ".png"
                    with open(png_path, "wb") as f:
                        f.write(png)
                    print(f"wrote {png_path} ({len(png)} bytes, converted "
                          f"losslessly from a {len(data)}-byte BMP capture)")

                    if state_json is not None:
                        state_path = os.path.splitext(args.out)[0] + ".json"
                        try:
                            # Round-tripped through json.loads/dump rather
                            # than written raw: this both validates the
                            # device's own formatting (screenshot.c's
                            # snprintf() is hand-rolled, not a JSON library -
                            # see suite_device_state.c for what IS verified,
                            # on a host, ahead of ever reaching real
                            # hardware) and pretty-prints it for a human
                            # reading the file afterward.
                            parsed = json.loads(state_json)
                            with open(state_path, "w") as f:
                                json.dump(parsed, f, indent=2)
                                f.write("\n")
                            print(f"wrote {state_path}")
                        except json.JSONDecodeError as exc:
                            print(f"warning: SCREENSHOT_STATE line was not "
                                  f"valid JSON ({exc}); writing it raw to "
                                  f"{state_path}", file=sys.stderr)
                            with open(state_path, "w") as f:
                                f.write(state_json + "\n")
                            print(f"wrote {state_path} (raw, unparsed)")
                    else:
                        print("no SCREENSHOT_STATE line arrived - device "
                              "state was not captured", file=sys.stderr)

                    return 0

                if line.startswith(STATE_PREFIX):
                    state_json = line[len(STATE_PREFIX):]
                    continue

                if line.startswith(DATA_PREFIX):
                    encoded = line[len(DATA_PREFIX):]
                    chunks.append(encoded)
                    received_b64_chars += len(encoded)
                # anything else on the wire is ordinary log output - ignored

    print(f"\ntimed out after {args.timeout:g}s without a complete capture.",
          file=sys.stderr)
    if total_size is None:
        print("never saw a SCREENSHOT_BEGIN line - is the firmware built "
              "with the screenshot listener (util/screenshot.c), and is it "
              "actually running (not stuck in the boot animation or a "
              "crash loop)?", file=sys.stderr)
    else:
        print(f"saw SCREENSHOT_BEGIN size={total_size} but never "
              f"SCREENSHOT_END - the transfer started but did not finish.",
              file=sys.stderr)
    return 1


if __name__ == "__main__":
    sys.exit(main())
