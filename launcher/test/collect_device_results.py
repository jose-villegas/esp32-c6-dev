"""Read a device self-test run from the serial console and report the outcome.

Invoked by run_device_tests.sh once the diagnostics firmware is flashed. Kept
in Python because pyserial ships inside ESP-IDF's environment and behaves the
same on every platform, which a shell script reading a serial port does not.

Exits 0 only if the run completed and every test passed. A run that never
finishes - a hang, a crash loop, a deadlocked DMA wait - is a failure, which is
the correct outcome and is exactly how the counting-semaphore deadlock would
surface if it ever returned.
"""

import argparse
import re
import sys
import time

import serial

# Printed by selftest.c once the whole run is over. Its presence is what
# distinguishes "finished" from "went quiet".
SENTINEL = re.compile(r"SELFTEST_COMPLETE failures=(\d+) elapsed_ms=(\d+)")

# Unity's own per-test lines, so failures can be echoed with their messages.
RESULT = re.compile(r":(PASS|FAIL|IGNORE)")


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", required=True)
    ap.add_argument("--baud", type=int, default=115200)
    ap.add_argument("--timeout", type=float, default=60.0,
                    help="seconds to wait for the run to finish")
    ap.add_argument("--quiet", action="store_true",
                    help="print only failures and the summary")
    args = ap.parse_args()

    try:
        port = serial.Serial(args.port, args.baud, timeout=0.2)
    except serial.SerialException as exc:
        print(f"could not open {args.port}: {exc}", file=sys.stderr)
        return 2

    # Pulse reset so the run is captured from boot. Without this we would
    # attach midway and miss the beginning, or wait forever on a board that
    # already finished.
    port.dtr = False
    port.rts = True
    time.sleep(0.12)
    port.rts = False

    deadline = time.monotonic() + args.timeout
    buffer = ""
    failures = []

    with port:
        while time.monotonic() < deadline:
            chunk = port.read(4096)
            if chunk:
                buffer += chunk.decode("utf-8", errors="replace")

            while "\n" in buffer:
                line, buffer = buffer.split("\n", 1)
                line = line.rstrip("\r")

                if ":FAIL" in line:
                    failures.append(line)

                if not args.quiet and RESULT.search(line):
                    print(line)

                done = SENTINEL.search(line)
                if done:
                    count, elapsed_ms = int(done.group(1)), int(done.group(2))
                    print()
                    if count == 0:
                        print(f"device tests PASSED in {elapsed_ms} ms")
                        return 0
                    print(f"device tests FAILED: {count} failure(s)",
                          file=sys.stderr)
                    for f in failures:
                        print(f"  {f}", file=sys.stderr)
                    return 1

    print(f"\ntimed out after {args.timeout:g}s without a completion marker.",
          file=sys.stderr)
    print("the board hung, crashed, or never started the run.", file=sys.stderr)
    if failures:
        print("failures seen before the timeout:", file=sys.stderr)
        for f in failures:
            print(f"  {f}", file=sys.stderr)
    return 1


if __name__ == "__main__":
    sys.exit(main())
