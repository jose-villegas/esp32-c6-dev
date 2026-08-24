#!/usr/bin/env python3
"""Resets the device via an RTS pulse and captures its serial output to a
file until SELFTEST_COMPLETE appears (or a timeout elapses).

Used by the sweep scripts in this directory between each build+flash and
the moment they parse the self-test's log lines for real numbers. Kept as
a standalone script, not inlined into each .ps1, because pyserial is
easiest to drive from Python and this needs to run many times per sweep.

Resetting via RTS/DTR directly (rather than shelling out to esptool for a
reset) matters: esptool needs to open and close the port itself, and the
gap between esptool releasing the port and this script opening it again
was enough, in practice, to miss the first several seconds of boot output
- including the self-test's early lines. Holding the port open across the
whole reset avoids that gap entirely.
"""
import argparse
import sys
import time

import serial


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("out_path", help="File to write captured serial output to")
    parser.add_argument("--port", default="COM3", help="Serial port (default: COM3)")
    parser.add_argument("--baud", type=int, default=115200, help="Baud rate (default: 115200)")
    parser.add_argument(
        "--timeout", type=float, default=90.0,
        help="Give up after this many seconds even if SELFTEST_COMPLETE never appears (default: 90)",
    )
    args = parser.parse_args()

    ser = serial.Serial(args.port, args.baud, timeout=1)
    try:
        # Hard reset via RTS pulse (EN line, through the board's auto-reset
        # circuit) - keep DTR deasserted so GPIO0 stays high (normal boot,
        # not the bootloader's download mode).
        ser.setDTR(False)
        ser.setRTS(True)
        time.sleep(0.1)
        ser.setRTS(False)

        start = time.time()
        with open(args.out_path, "wb") as f:
            while time.time() - start < args.timeout:
                line = ser.readline()
                if line:
                    f.write(line)
                    f.flush()
                    if b"SELFTEST_COMPLETE" in line:
                        break
    finally:
        ser.close()

    print("DONE")
    return 0


if __name__ == "__main__":
    sys.exit(main())
