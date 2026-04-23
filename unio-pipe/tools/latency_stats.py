#!/usr/bin/env python3
"""Parse a UNIO_PIPE_LATENCY_CSV and print p50 / p95 / max /
mean for capture→decode, decode→present, and capture→present,
skipping a warmup prefix.

Usage:
    ./latency_stats.py <csv-path> [warmup-rows]

Default warmup = 60 rows. CSV is emitted by the sink-side helper
when UNIO_PIPE_LATENCY_CSV=/path is set in its environment. Each
row: frame_id, capture_ns, decode_done_ns, present_done_ns,
width, height, capture_to_decode_us, decode_to_present_us,
capture_to_present_us. Rows with any field >= 1e9 µs (usually
an overflow from a cross-machine clock skew) are dropped as
garbage before computing stats.
"""
import csv
import statistics
import sys

GARBAGE_THRESHOLD_US = 1_000_000_000  # 1000 seconds — anything
                                       # bigger is a clock-skew
                                       # overflow artefact.

COLUMNS = [
    "capture_to_decode_us",
    "decode_to_present_us",
    "capture_to_present_us",
]


def main(csv_path: str, warmup: int) -> int:
    rows = list(csv.DictReader(open(csv_path)))
    print(f"rows (raw): {len(rows)}")
    print(f"warmup:     {warmup}")
    for key in COLUMNS:
        vals = sorted(
            int(r[key]) for r in rows if int(r[key]) < GARBAGE_THRESHOLD_US
        )
        warm = vals[warmup:] if len(vals) > warmup else vals
        if len(warm) < 10:
            print(f"  {key:30s} n={len(warm):4d} (too few samples)")
            continue
        n = len(warm)
        p50 = warm[n // 2]
        p95 = warm[int(n * 0.95)]
        mx = warm[-1]
        mean = statistics.mean(warm)
        print(
            f"  {key:30s} n={n:5d}  "
            f"p50={p50/1000:7.2f} ms  "
            f"p95={p95/1000:7.2f} ms  "
            f"max={mx/1000:7.2f} ms  "
            f"mean={mean/1000:7.2f} ms"
        )
    return 0


if __name__ == "__main__":
    if len(sys.argv) < 2 or len(sys.argv) > 3:
        print(__doc__, file=sys.stderr)
        sys.exit(2)
    path = sys.argv[1]
    warm = int(sys.argv[2]) if len(sys.argv) == 3 else 60
    sys.exit(main(path, warm))
