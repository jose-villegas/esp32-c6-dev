"""Request a screenshot from the device and save it as a .bmp.

Invoked by screenshot.sh. Kept in Python because pyserial ships inside
ESP-IDF's environment and behaves the same on every platform, which a shell
script reading a serial port directly does not - the same reasoning
test/collect_device_results.py's own top comment gives for the identical
split there.

Sends the trigger word over the console UART (see util/screenshot.c) and
reads the response back out of the same stream idf_monitor would otherwise
be showing as logs: a SCREENSHOT_BEGIN line announcing the byte count, one
SCREENSHOT_DATA: line per base64-encoded chunk, and a SCREENSHOT_END line.
Anything else on the wire - ordinary ESP_LOG output, in particular - is
ignored rather than treated as an error, since the device keeps logging
normally while it streams.
"""

import argparse
import base64
import re
import sys
import time

import serial

BEGIN_RE = re.compile(r"^SCREENSHOT_BEGIN size=(\d+)$")
DATA_PREFIX = "SCREENSHOT_DATA:"
END_LINE = "SCREENSHOT_END"

TRIGGER = b"SCREENSHOT\n"


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", required=True)
    ap.add_argument("--baud", type=int, default=115200)
    ap.add_argument("--out", required=True)
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

    with port:
        port.reset_input_buffer()
        port.write(TRIGGER)
        port.flush()

        while time.monotonic() < deadline:
            chunk = port.read(4096)
            if chunk:
                buffer += chunk.decode("utf-8", errors="replace")

            while "\n" in buffer:
                line, buffer = buffer.split("\n", 1)
                line = line.rstrip("\r")

                if total_size is None:
                    m = BEGIN_RE.match(line)
                    if m:
                        total_size = int(m.group(1))
                    continue

                if line == END_LINE:
                    data = base64.b64decode("".join(chunks))
                    if len(data) != total_size:
                        print(f"warning: decoded {len(data)} bytes but the "
                              f"device announced {total_size}", file=sys.stderr)
                    with open(args.out, "wb") as f:
                        f.write(data)
                    print(f"wrote {args.out} ({len(data)} bytes)")
                    return 0

                if line.startswith(DATA_PREFIX):
                    chunks.append(line[len(DATA_PREFIX):])
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
