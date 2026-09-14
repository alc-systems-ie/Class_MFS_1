#!/usr/bin/env python3
"""Phase-coverage simulation for the MFS_1 passive scan cadence.

Replaces the withdrawn closed-form rule (d x N >= I - W) in
docs/superpowers/specs/2026-09-14-scan-reliability-amendment.md section 2. That
rule was unsound: off by one, and invalid whenever the drift d exceeds the
scan window W (its own derivation assumes d <= W). This script instead
brute-forces the actual question - does at least one of N consecutive scans
catch the advertiser - for a given scan period and a list of counterpart
advertising intervals, in two models: a steady advertiser (exact phase sweep)
and a jittered one (Monte Carlo).

How many scans a command gets, N
--------------------------------
A command is advertised for a burst of B ms (30 000 ms, the app's Send window).
The burst starts at an arbitrary moment relative to the scan schedule, so the
first scan that opens inside it starts anywhere from 0 to P after the burst
starts. Scan k then occupies [s + kP, s + kP + W] with s in [0, P), and it lies
wholly inside the burst when s + kP + W <= B. In the worst case (s -> P) that
holds for every k with (k + 1)P + W <= B, so the number of complete windows
GUARANTEED inside any burst is

    N = floor((B - W) / P)

That is the N this script derives (override with --scans). N = 5 needs
5P + W <= B, i.e. P <= (B - W) / 5 = 5980 ms at B = 30 000 ms and W = 100 ms.
RAISING THE PERIOD PAST 5980 ms DROPS N TO 4, which the steady results below do
not survive. 5970 ms has only 10 ms of that margin, and a burst that starts
advertising late eats into it. (floor((B - W) / P) + 1 is the best case, a burst
that starts exactly as a window opens; it is not guaranteed and not used.)

Steady model (the PASS / FAIL verdicts)
---------------------------------------
The advertiser is idealised as an instant transmitting at phi + j*I for integer
j, where phi in [0, I) is an unknown start phase and I is its advertising
interval. Scan k opens a window [kP, kP + W], which catches an advert exactly
when

    (phi - k*P) mod I <= W

(the advert falls within W of the window's start, modulo the advertiser's own
interval, since j ranges over all integers). The command is missed only if this
fails for every k in 0..N-1. Sweeping phi uniformly over [0, I) and counting the
fraction of phase samples for which every one of the N scans misses gives the
miss fraction - the probability a command starting at a uniformly random moment
is never heard. An interval I <= W is exempt: a single window always contains an
advert.

A PASS HOLDS FOR A STEADY INTERVAL ONLY. Real advDelay jitter can add a small
miss rate (see below): a steady PASS is not a guarantee, and this model is not
pessimistic. A period that passes steadily does so by walking the scan phase
across the advertiser's cycle in exact steps; jitter perturbs those steps.

Jitter model (--jitter-ms J, Monte Carlo)
-----------------------------------------
Every BLE advertising event is delayed by a pseudo-random advDelay of 0-10 ms
(Core Specification Vol 6 Part B 4.4.2.2.1), so successive adverts are I + U(0, J)
apart rather than exactly I - a mean spacing of I + J/2. That reads I as the
NOMINAL advertising interval (Apple's documented 152.5, 211.25, 318.75 ms...).
--jitter-centred instead draws I - J/2 + U(0, J), a mean spacing of exactly I,
which reads I as a MEASURED mean spacing. The two differ sharply near a passing
island's edge: a J/2 = 5 ms shift of the mean moves 187.5 ms to 192.5 ms, and
5970 ms is within 2.5 ms of 31 x 192.5 ms. The Mac's 187.5 ms is a measured
spacing - the 6000 ms scan stayed locked to one phase of it across whole
commands, which a mean 5 ms longer could not do - so --jitter-centred is the
model supported for it; the uncentred model shows the sensitivity.

Each trial: the burst starts at t = 0 with the first advert; each later advert
follows the previous one by a fresh draw of the gap above; the first scan opens at s = U(0, P) and scans k = 0..N-1 open at s + kP, each
catching the command if any advert lies in [s + kP, s + kP + W]. The miss
fraction is the fraction of --trials trials in which no window catches an
advert (standard error about sqrt(m(1 - m)/trials)). The burst start is the
random start phase. With J = 0 this reproduces the steady model to within
sampling error. Periods in a --sweep share the same random advert trains (common
random numbers), so their ranking is not dominated by sampling noise.

Limits of both models (deliberate simplifications)
--------------------------------------------------
- Adverts are instants with zero air time. Real adverts last a few hundred
  microseconds, which can only help a window catch one.
- A command is "heard" the instant one advert lands in one scan window - neither
  model covers the AES-CCM decode step, RF margin, collisions or the controller
  closing a window early.
- The advertiser's mean interval is taken as exact. Measured intervals differ
  from nominal by a few ms (amendment section 1), and a steady PASS can become a
  FAIL for a nearby interval - re-run the sweep against the measured value.
- Intervals at or beyond roughly 5 x W cannot be fully covered by any scan
  period at this N: a single 100 ms window can only ever intersect one
  advertiser cycle per scan when I is that large, so N = 5 scans sweep at most
  5 x W of phase out of an I - W gap the window does not already cover. Long
  intervals (order 500 ms and up) should be treated as fundamentally unreliable
  at this cadence, not tuned around - the app's foreground-only sending, which
  keeps the counterpart advertising fast, is what bounds that case (amendment
  section 6).

Usage
-----
    python3 tools/scan_phase_check.py [interval_ms ...]
        [--period-units UNITS] [--window-ms MS] [--burst-ms MS] [--scans N]
        [--steps N] [--jitter-ms J] [--jitter-centred] [--trials T] [--seed S]
    python3 tools/scan_phase_check.py --sweep MIN_UNITS MAX_UNITS
        [--max-interval-ms MS] [--jitter-ms J] [other flags as above]

With no positional intervals, a default list covering the measured and
plausible counterpart intervals is used. With --jitter-ms, both the steady and
the jitter results are reported, and a sweep is ranked by the jitter result.
The jitter mode needs numpy; the steady mode needs only the standard library.
"""
import argparse
import math
import sys

DEFAULT_PERIOD_UNITS = 9552
BLE_UNIT_MS = 0.625
DEFAULT_WINDOW_MS = 100.0
DEFAULT_BURST_MS = 30000.0
DEFAULT_STEPS = 2000
DEFAULT_TRIALS = 20000
DEFAULT_SEED = 1
DEFAULT_INTERVALS_MS = (35, 100, 152.5, 187.5, 211.25, 318.75, 417.5, 546.25, 760, 852.5, 1022.5, 1285)
SWEEP_TOP_N = 20

# Row offset for the flattened advert matrix in the jitter model. Must exceed
# every advert time and window end in one trial.
ROW_OFFSET_MS = 1.0e6


def derived_scans(period_ms, window_ms, burst_ms):
    """Complete scan windows guaranteed inside any burst: floor((B - W) / P)."""
    return math.floor((burst_ms - window_ms) / period_ms)


def miss_fraction(period_ms, window_ms, scans, interval_ms, steps):
    """Steady model: fraction of start phases for which every one of `scans`
    consecutive scans misses an advertiser at `interval_ms`, or None if the
    interval is exempt (I <= W, so a single window always contains an advert).
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


class AdvertTrains:
    """Pre-drawn jittered advert times for one interval, shared by every period
    tested against it (common random numbers)."""

    def __init__(self, interval_ms, jitter_ms, centred, trials, burst_ms, rng):
        import numpy as np

        self.np = np
        self.trials = trials
        base_ms = interval_ms - jitter_ms / 2.0 if centred else interval_ms
        adverts = int(math.ceil(burst_ms / base_ms)) + 1
        gaps = base_ms + rng.uniform(0.0, jitter_ms, size=(trials, adverts - 1))
        times = np.zeros((trials, adverts))
        times[:, 1:] = np.cumsum(gaps, axis=1)
        self.row_offsets = np.arange(trials, dtype=float) * ROW_OFFSET_MS
        self.flat = (times + self.row_offsets[:, None]).ravel()
        self.start_fraction = rng.uniform(0.0, 1.0, size=trials)

    def miss_fraction(self, period_ms, window_ms, scans):
        np = self.np
        caught = np.zeros(self.trials, dtype=bool)
        first_open = self.start_fraction * period_ms

        for scan_index in range(scans):
            opens = first_open + scan_index * period_ms + self.row_offsets
            index = np.searchsorted(self.flat, opens, side="left")
            index = np.minimum(index, self.flat.size - 1)
            caught |= self.flat[index] <= opens + window_ms

        return 1.0 - caught.mean()


def jitter_exempt(interval_ms, jitter_ms, window_ms):
    # Conservative for the centred model, whose longest gap is I + J/2.
    return interval_ms + jitter_ms <= window_ms


def jitter_text(args):
    if args.jitter_centred:
        return f"I - {args.jitter_ms / 2:g} + U(0, {args.jitter_ms:g}) (centred, mean I)"
    return f"I + U(0, {args.jitter_ms:g}) (mean I + {args.jitter_ms / 2:g})"


def verdict_for(fraction):
    if fraction is None:
        return "exempt"
    if fraction == 0:
        return "PASS"
    return f"FAIL {fraction * 100:.0f}%"


def resolve_scans(args, period_ms):
    scans = args.scans if args.scans is not None else derived_scans(period_ms, args.window_ms, args.burst_ms)
    if scans < 1:
        sys.exit(f"Period {period_ms:g} ms leaves no complete scan window in a {args.burst_ms:g} ms burst.")
    return scans


def make_rng(seed):
    import numpy as np

    return np.random.default_rng(seed)


def run_check(args):
    period_ms = args.period_units * BLE_UNIT_MS
    scans = resolve_scans(args, period_ms)
    source = "given" if args.scans is not None else f"floor(({args.burst_ms:g} - {args.window_ms:g}) / {period_ms:g})"
    rng = make_rng(args.seed) if args.jitter_ms > 0 else None

    print(f"Period {period_ms:g} ms ({args.period_units} units), window {args.window_ms:g} ms, "
          f"N {scans} ({source}), {args.steps} phase steps.")
    if args.scans is None:
        print(f"  N stays {scans} up to P = {(args.burst_ms - args.window_ms) / scans:g} ms; "
              f"above that it drops to {scans - 1}.")
    if rng is not None:
        print(f"  Jitter: each advert interval {jitter_text(args)} ms, {args.trials} trials, seed {args.seed}.")

    for interval_ms in args.intervals:
        fraction = miss_fraction(period_ms, args.window_ms, scans, interval_ms, args.steps)
        fraction_text = "-" if fraction is None else f"{fraction:.4f}"
        line = f"  I={interval_ms:>9g} ms  steady miss={fraction_text:>7}  {verdict_for(fraction):<9}"
        if rng is not None:
            if jitter_exempt(interval_ms, args.jitter_ms, args.window_ms):
                line += "  jitter: exempt"
            else:
                trains = AdvertTrains(interval_ms, args.jitter_ms, args.jitter_centred, args.trials, args.burst_ms, rng)
                jitter = trains.miss_fraction(period_ms, args.window_ms, scans)
                error = math.sqrt(max(jitter * (1.0 - jitter), 0.0) / args.trials)
                line += f"  jitter miss={jitter * 100:6.2f}% (+/- {error * 100:.2f})"
        print(line)


def run_sweep(args):
    min_units, max_units = args.sweep
    candidates = [i for i in args.intervals if args.max_interval_ms is None or i <= args.max_interval_ms]
    non_exempt = [i for i in candidates if i > args.window_ms]
    use_jitter = args.jitter_ms > 0

    print(f"Sweeping period units {min_units}..{max_units} "
          f"({min_units * BLE_UNIT_MS:g}..{max_units * BLE_UNIT_MS:g} ms), window {args.window_ms:g} ms, "
          f"N {'given' if args.scans is not None else 'derived per period'},")
    print(f"ranked by worst-case {'JITTER' if use_jitter else 'steady'} miss fraction over: "
          f"{', '.join(f'{i:g}' for i in non_exempt)} ms.")

    trains = {}
    if use_jitter:
        rng = make_rng(args.seed)
        print(f"Jitter: each advert interval {jitter_text(args)} ms, {args.trials} trials, seed {args.seed}, "
              f"common random numbers across periods.")
        for interval_ms in non_exempt:
            if not jitter_exempt(interval_ms, args.jitter_ms, args.window_ms):
                trains[interval_ms] = AdvertTrains(interval_ms, args.jitter_ms, args.jitter_centred, args.trials, args.burst_ms, rng)

    results = []
    for units in range(min_units, max_units + 1):
        period_ms = units * BLE_UNIT_MS
        scans = resolve_scans(args, period_ms)
        steady_worst, steady_at = 0.0, None
        jitter_worst, jitter_at = 0.0, None
        for interval_ms in non_exempt:
            fraction = miss_fraction(period_ms, args.window_ms, scans, interval_ms, args.steps)
            if fraction is not None and fraction > steady_worst:
                steady_worst, steady_at = fraction, interval_ms
            if interval_ms in trains:
                jitter = trains[interval_ms].miss_fraction(period_ms, args.window_ms, scans)
                if jitter > jitter_worst:
                    jitter_worst, jitter_at = jitter, interval_ms
        rank = jitter_worst if use_jitter else steady_worst
        results.append((rank, units, period_ms, scans, steady_worst, steady_at, jitter_worst, jitter_at))

    results.sort(key=lambda entry: (entry[0], entry[1]))

    print(f"Best {min(SWEEP_TOP_N, len(results))} periods (lowest worst-case miss first):")
    for _, units, period_ms, scans, steady_worst, steady_at, jitter_worst, jitter_at in results[:SWEEP_TOP_N]:
        steady_text = f"steady worst {steady_worst * 100:5.2f}% (I={'-' if steady_at is None else f'{steady_at:g}'})"
        line = f"  {units} units = {period_ms:.3f} ms  N {scans}  {steady_text}"
        if use_jitter:
            line += f"  jitter worst {jitter_worst * 100:5.2f}% (I={'-' if jitter_at is None else f'{jitter_at:g}'})"
        print(line)


def build_parser():
    parser = argparse.ArgumentParser(description="Phase-coverage simulation for the MFS_1 scan cadence.")
    parser.add_argument("intervals", nargs="*", type=float, default=list(DEFAULT_INTERVALS_MS),
                        help="Counterpart advertising intervals in ms to check (default: measured/plausible set).")
    parser.add_argument("--period-units", type=int, default=DEFAULT_PERIOD_UNITS,
                        help=f"Scan period in 0.625 ms BLE units (default {DEFAULT_PERIOD_UNITS}).")
    parser.add_argument("--window-ms", type=float, default=DEFAULT_WINDOW_MS,
                        help=f"Scan window in ms (default {DEFAULT_WINDOW_MS:g}).")
    parser.add_argument("--burst-ms", type=float, default=DEFAULT_BURST_MS,
                        help=f"Command advertising burst in ms, B (default {DEFAULT_BURST_MS:g}).")
    parser.add_argument("--scans", type=int, default=None,
                        help="Override N. Default: derived per period as floor((B - W) / P).")
    parser.add_argument("--steps", type=int, default=DEFAULT_STEPS,
                        help=f"Steady model: phase samples per interval (default {DEFAULT_STEPS}).")
    parser.add_argument("--jitter-ms", type=float, default=0.0,
                        help="Also run the Monte Carlo jitter model, advert intervals I + U(0, J) ms (advDelay: 10).")
    parser.add_argument("--jitter-centred", action="store_true",
                        help="Jitter model: draw I - J/2 + U(0, J), so the mean spacing is I (I is a measured mean).")
    parser.add_argument("--trials", type=int, default=DEFAULT_TRIALS,
                        help=f"Jitter model: trials per interval (default {DEFAULT_TRIALS}).")
    parser.add_argument("--seed", type=int, default=DEFAULT_SEED,
                        help=f"Jitter model: random seed (default {DEFAULT_SEED}).")
    parser.add_argument("--sweep", nargs=2, type=int, metavar=("MIN_UNITS", "MAX_UNITS"),
                        help="Rank periods (in BLE units) by worst-case miss fraction over the given intervals.")
    parser.add_argument("--max-interval-ms", type=float, default=None,
                        help="With --sweep, only rank against intervals at or below this value.")
    return parser


def main():
    args = build_parser().parse_args()

    if args.sweep:
        run_sweep(args)
    else:
        run_check(args)
    return 0


if __name__ == "__main__":
    sys.exit(main())
