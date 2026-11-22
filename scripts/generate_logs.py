#!/usr/bin/env python3
"""Generate reproducible input files for netlog."""

import argparse
import random
from pathlib import Path


EVENTS = (
    ("INFO", "REQ_OK", 50, 400),
    ("WARN", "SLOW", 401, 899),
    ("ERROR", "TIMEOUT", 900, 2000),
    ("ERROR", "CRC_ERROR", 50, 300),
)
EVENT_WEIGHTS = (80, 10, 7, 3)


def non_negative_int(text):
    value = int(text)
    if value < 0:
        raise argparse.ArgumentTypeError("must be non-negative")
    return value


def generate_logs(output, lines, devices, seed, bad_every):
    rng = random.Random(seed)
    with output.open("x", encoding="utf-8", newline="\n") as log:
        for line_number in range(1, lines + 1):
            if bad_every and line_number % bad_every == 0:
                log.write("broken_line\n")
                continue

            device = rng.randrange(1, devices + 1)
            level, event, low_us, high_us = rng.choices(EVENTS, EVENT_WEIGHTS)[0]
            duration_us = rng.randint(low_us, high_us)
            timestamp = 1_700_000_000 + line_number - 1
            log.write(f"{timestamp} dev{device:04d} {level} {event} {duration_us}\n")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("output", type=Path, help="new log file to create")
    parser.add_argument("--lines", type=non_negative_int, default=100_000,
                        help="total lines including malformed lines (default: 100000)")
    parser.add_argument("--devices", type=non_negative_int, default=100,
                        help="number of possible device IDs (default: 100)")
    parser.add_argument("--seed", type=int, default=42,
                        help="random seed (default: 42)")
    parser.add_argument("--bad-every", type=non_negative_int, default=0,
                        help="make every Nth line malformed (default: 0, disabled)")
    args = parser.parse_args()
    if args.devices == 0:
        parser.error("--devices must be at least 1")
    if args.lines > 2**63 - 1 - 1_700_000_000 + 1:
        parser.error("--lines would exceed the timestamp range")

    try:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        generate_logs(args.output, args.lines, args.devices, args.seed, args.bad_every)
    except OSError as error:
        parser.exit(1, f"{error}\n")


if __name__ == "__main__":
    main()
