#!/usr/bin/env python3
"""Phase-coverage simulation for the MFS_1 passive scan cadence.

Replaces the withdrawn closed-form rule (d x N >= I - W) in
docs/superpowers/specs/2026-09-14-scan-reliability-amendment.md section 2. That
rule was unsound: off by one, and invalid whenever the drift d exceeds the
scan window W (its own derivation assumes d <= W). This script instead
brute-forces the actual question - does at least one of N consecutive scans
catch the advertiser - over a dense sample of start phases, for a given scan
period and a list of counterpart advertising intervals.

Model
-----
The advertiser is idealised as an instant: it transmits at times
phi + j*I for integer j >= 0, where phi in [0, I) is an unknown start phase
and I is its advertising interval. A command needs N consecutive scans to be
heard in total (30 s command / period, about 5 at these periods), and the
device is deemed to hear it if ANY of those N scans catches ANY repetition of
the advert.

Scan k (k = 0 .. N-1) opens a window [k*P, k*P + W], where P is the scan
period and W the scan window. That window catches an advert at phase
phi + j*I exactly when

    (phi - k*P) mod I <= W

(the advert falls within W of the window's start, modulo the advertiser's own
interval, since j ranges over all integers). The command is missed only if
this fails for every k in 0..N-1. Sweeping phi uniformly over one full
interval [0, I) and counting the fraction of phase samples for which every
one of the N scans misses gives the miss fraction for that (P, W, N, I) - the
probability a command starting at a uniformly random moment is never heard.

Limits of this model (deliberate simplifications)
--------------------------------------------------
- Adverts are instants with zero air time and no advDelay jitter (the random
  0-10 ms BLE stack adds before each advertising event). Real adverts have
  nonzero duration, which can only help catch a command relative to this
  model - so this model is pessimistic, not optimistic.
- The advertiser's interval I is taken as exactly steady. Measured intervals
  drift by a few ms in practice (see the amendment section 1); that drift
  helps coverage rather than hurting it, again making this model pessimistic.
- A command is "heard" the instant one advert lands in one scan window - it
  does not model the AES-CCM decode step, RF margin or collisions.
- Intervals at or beyond roughly 5 x W cannot be fully covered by any scan
  period at this N: a single 100 ms window can only ever intersect one
  advertiser cycle per scan when I is that large, so N=5 scans sweep at most
  5 x W of phase out of an I - W gap the window does not already cover. Long
  intervals (order 500 ms and up) should be treated as fundamentally
  unreliable at this cadence, not tuned around - see docs/v1-scope.md and the
  amendment for why foreground fast advertising in the app is the actual
  guarantee for those cases, not the scan period.

Usage
-----
    python3 tools/scan_phase_check.py [interval_ms ...]
        [--period-units UNITS] [--window-ms MS] [--scans N] [--steps N]
    python3 tools/scan_phase_check.py --sweep MIN_UNITS MAX_UNITS
        [--max-interval-ms MS] [other flags as above]

With no positional intervals, a default list covering the measured and
plausible counterpart intervals is used.
"""
import argparse
import sys

DEFAULT_PERIOD_UNITS = 9401
BLE_UNIT_MS = 0.625
DEFAULT_WINDOW_MS = 100.0
DEFAULT_SCANS = 5
DEFAULT_STEPS = 2000
DEFAULT_INTERVALS_MS = (35, 100, 152.5, 187.5, 211.25, 318.75, 417.5, 546.25, 760, 852.5, 1022.5, 1285)
SWEEP_TOP_N = 20


def miss_fraction(period_ms, window_ms, scans, interval_ms, steps):
    """Fraction of start phases for which every one of `scans` consecutive
    scans misses an advertiser at `interval_ms`, or None if the interval is
    exempt (I <= W, so a single window always contains an advert).
    """
    if interval_ms <= window_ms:
        return None

    missed_count = 0
    for step in range(steps):
        phase = interval_ms * step / steps
        caught = False
        for scan_index in range(scans):
            offset = (phase - scan_index * period_ms) % interval_ms
            if offset <= window_ms:
                caught = True
                break
        if not caught:
            missed_count += 1

    return missed_count / steps


def verdict_for(fraction):
    if fraction is None:
        return "exempt"
    if fraction == 0:
        return "PASS"
    return f"FAIL {fraction * 100:.0f}%"


def run_check(period_ms, window_ms, scans, steps, intervals):
    print(f"Period {period_ms:g} ms, window {window_ms:g} ms, N {scans}, {steps} phase steps.")
    for interval_ms in intervals:
        fraction = miss_fraction(period_ms, window_ms, scans, interval_ms, steps)
        fraction_text = "-" if fraction is None else f"{fraction:.4f}"
        print(f"  I={interval_ms:>9g} ms  miss={fraction_text:>7}  {verdict_for(fraction)}")


def run_sweep(min_units, max_units, window_ms, scans, steps, intervals, max_interval_ms):
    candidates = [i for i in intervals if max_interval_ms is None or i <= max_interval_ms]
    non_exempt = [i for i in candidates if i > window_ms]

    print(f"Sweeping period units {min_units}..{max_units} "
          f"({min_units * BLE_UNIT_MS:g}..{max_units * BLE_UNIT_MS:g} ms),")
    print(f"ranked by worst-case miss fraction over: {', '.join(f'{i:g}' for i in non_exempt)} ms.")

    results = []
    for units in range(min_units, max_units + 1):
        period_ms = units * BLE_UNIT_MS
        worst = 0.0
        worst_interval = None
        for interval_ms in non_exempt:
            fraction = miss_fraction(period_ms, window_ms, scans, interval_ms, steps)
            if fraction is not None and fraction > worst:
                worst = fraction
                worst_interval = interval_ms
        results.append((worst, units, period_ms, worst_interval))

    results.sort(key=lambda entry: (entry[0], entry[1]))

    print(f"Best {min(SWEEP_TOP_N, len(results))} periods (lowest worst-case miss first):")
    for worst, units, period_ms, worst_interval in results[:SWEEP_TOP_N]:
        worst_interval_text = "-" if worst_interval is None else f"{worst_interval:g} ms"
        print(f"  {units} units = {period_ms:.3f} ms  worst-miss {worst * 100:5.1f}%  (at I={worst_interval_text})")


def build_parser():
    parser = argparse.ArgumentParser(description="Phase-coverage simulation for the MFS_1 scan cadence.")
    parser.add_argument("intervals", nargs="*", type=float, default=list(DEFAULT_INTERVALS_MS),
                         help="Counterpart advertising intervals in ms to check (default: measured/plausible set).")
    parser.add_argument("--period-units", type=int, default=DEFAULT_PERIOD_UNITS,
                         help=f"Scan period in 0.625 ms BLE units (default {DEFAULT_PERIOD_UNITS}).")
    parser.add_argument("--window-ms", type=float, default=DEFAULT_WINDOW_MS,
                         help=f"Scan window in ms (default {DEFAULT_WINDOW_MS:g}).")
    parser.add_argument("--scans", type=int, default=DEFAULT_SCANS,
                         help=f"Number of consecutive scans in one command window, N (default {DEFAULT_SCANS}).")
    parser.add_argument("--steps", type=int, default=DEFAULT_STEPS,
                         help=f"Number of phase samples per interval (default {DEFAULT_STEPS}).")
    parser.add_argument("--sweep", nargs=2, type=int, metavar=("MIN_UNITS", "MAX_UNITS"),
                         help="Rank periods (in BLE units) by worst-case miss fraction over the given intervals.")
    parser.add_argument("--max-interval-ms", type=float, default=None,
                         help="With --sweep, only rank against intervals at or below this value.")
    return parser


def main():
    parser = build_parser()
    args = parser.parse_args()

    if args.sweep:
        min_units, max_units = args.sweep
        run_sweep(min_units, max_units, args.window_ms, args.scans, args.steps,
                  args.intervals, args.max_interval_ms)
        return 0

    period_ms = args.period_units * BLE_UNIT_MS
    run_check(period_ms, args.window_ms, args.scans, args.steps, args.intervals)
    return 0


if __name__ == "__main__":
    sys.exit(main())
