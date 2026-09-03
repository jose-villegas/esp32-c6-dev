#!/usr/bin/env python3
"""Resets the device, waits for the shell to come up, sends "RUNSUITE
<name>" on the console (see main/util/screenshot.c), and captures output
for a fixed window afterward.

The capture half of using RUNSUITE from a host script - the same role
tools/sweeps/capture_selftest.py plays for a full SELFTEST_COMPLETE run,
but scoped to one suite instead of waiting through every suite alphabetically
ahead of it. No single completion marker is generic across every suite
(unlike SELFTEST_COMPLETE for the full run), so this captures for a fixed
--timeout after sending the command rather than trying to detect the
suite's own end - pick a timeout generous enough for whichever suite this
is pointed at; a short one just truncates the tail of the capture, it does
not error.

CONFIG_LAUNCHER_SELFTEST_AUTORUN must be OFF for this to be worth using -
with it on, the device runs every suite automatically before the shell
(and so the RUNSUITE listener) ever comes up, defeating the whole point of
running just one.
"""
import argparse
import sys
import time

import serial


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("suite_name", help="Exact registered suite name, e.g. run_boot_anim_perf_suite")
    parser.add_argument("out_path", help="File to write captured serial output to")
    parser.add_argument("--port", default="COM3", help="Serial port (default: COM3)")
    parser.add_argument("--baud", type=int, default=115200, help="Baud rate (default: 115200)")
    parser.add_argument(
        "--timeout", type=float, default=60.0,
        help="How long to capture for after sending RUNSUITE (default: 60)",
    )
    args = parser.parse_args()

    ser = serial.Serial(args.port, args.baud, timeout=1)
    try:
        # Hard reset via RTS pulse - see capture_selftest.py's own comment
        # on why this, not esptool, and why DTR stays deasserted.
        ser.setDTR(False)
        ser.setRTS(True)
        time.sleep(0.1)
        ser.setRTS(False)

        sent = False
        start = time.time()
        with open(args.out_path, "wb") as f:
            while time.time() - start < args.timeout:
                line = ser.readline()
                if line:
                    f.write(line)
                    f.flush()
                    if not sent and b"shell: Ready" in line:
                        # A beat before sending: the shell logs "Ready" the
                        # instant its own init returns, not once the console
                        # listener's own task has actually reached its
                        # blocking read - see screenshot.c's own xTaskCreate
                        # call, started moments earlier in the same
                        # function. Sending immediately risks the command
                        # arriving before anything is listening for it.
                        time.sleep(0.3)
                        ser.write(("RUNSUITE %s\n" % args.suite_name).encode("utf-8"))
                        sent = True
        if not sent:
            print("WARNING: never saw 'shell: Ready' - RUNSUITE was not sent", file=sys.stderr)
    finally:
        ser.close()

    print("DONE")
    return 0


if __name__ == "__main__":
    sys.exit(main())
