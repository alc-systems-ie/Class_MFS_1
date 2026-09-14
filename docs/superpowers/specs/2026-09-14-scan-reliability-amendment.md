# MFS_1 Scan Reliability, Send/Stop — Design Amendment

**Date:** 2026-09-14 (bench session 1)
**Amends:** `docs/power-budget.md` (scan period), `docs/superpowers/specs/2026-09-14-arming-sequence-amendment.md` §3 (scanning during Arming), `docs/superpowers/specs/2026-09-12-app-control-design.md` §3 and §8 (Send UI), `docs/superpowers/specs/2026-09-14-command-types-amendment.md` §3 (pre-empt removed). Where this document and those disagree, **this document wins**.

## 1. Measurements (bench, 2026-09-14)

A second nRF54L15 DK ran a continuous passive observer (`tools/uuid_observer`) logging
every 128-bit service UUID advert with a timestamp; MFS_1 ran `CONFIG_MFS_SCAN_DIAG`
reception counters.

| Advertiser (class app) | Interval | 30 s advert | MFS_1 catch per 100 ms scan |
|---|---|---|---|
| macOS (MacBook, foreground) | **187.5 ms**, steady | ~155 adverts | 0 or 1; **whole commands missed** |
| iPhone (iOS 26.6.1, foreground, release build) | **~35 ms**, steady for 30 s | ~800 adverts | 2–3, **every scan** |

**Root cause of today's unreliable commands (Mac):** 6000 ms = exactly 32 × 187.5 ms.
Every scan lands at the same phase of the Mac's advertising cycle, drifting only a few
ms per scan, so a command is caught on almost every scan or on almost none. Observed:
a time sync missed by all 6 of its scans; four consecutive Disarms unheard.

**Replay of the previous advert (iOS):** when a new advert starts, iOS briefly (1.7 s
observed) re-broadcasts the previous advertising payload before the new one, even a
minute after the previous advert ended.

## 2. Scan period 5906 ms

`CONFIG_MFS_SCAN_PERIOD_MS` default **6000 → 5906**. 5906 ms (9449 BLE units,
5905.625 ms) is 31.5 × 187.5 ms, so each scan advances half an advertising interval
against a 187.5 ms advertiser and consecutive scans sample opposite halves of its
cycle. Against 35 ms (iOS) and 100 ms (Android low-latency) the window already spans
an interval, so nothing changes. Window stays 100 ms; average current rises by
6000/5906 on the scan terms (~1.6 %), negligible against the ~69 µA budget.

**Rule.** Let P be the scan period, W the scan window and N the number of scans inside
one 30 s command (30 000 / 5905.625 ≈ 5.08, so **N = 5**). For every counterpart
advertising interval **I longer than W**, the per-scan phase drift

  d = min(P mod I, I − (P mod I))

must satisfy **d × N ≥ I − W**. Each scan lands d ms later (or earlier) in the
advertiser's cycle than the one before; if N scans cannot sweep the I − W of the cycle
the window does not already cover, a command that starts at an unlucky phase is missed
by every one of its scans. Being "not within a few ms of a multiple" is necessary but
not sufficient: a drift of 9 ms is not a multiple, and still fails. An interval I ≤ W is
exempt — a 100 ms window always contains at least one advert.

Check for P = 5905.625 ms (the real value of 9449 × 0.625 ms), W = 100 ms, N = 5:

| Counterpart | I (ms) | P mod I | d (ms) | d × N | I − W | Verdict |
|---|---|---|---|---|---|---|
| macOS, measured | 187.5 | 93.125 | 93.125 | 465.6 | 87.5 | **Pass** |
| iPhone, measured | ~35 | — | — | — | — | Exempt (I ≤ W) |
| Android low-latency, nominal | ~100 | — | — | — | — | Exempt (I ≤ W) — **to be measured** on the production phone |
| Apple recommended interval | 211.25 | 201.875 | 9.375 | 46.9 | 111.25 | **FAIL** |

(Against 187.5 ms, d ≈ I/2, so the scans alternate between two phases 93 ms apart; two
100 ms windows at those phases cover 193 ms of a 187.5 ms cycle, so the pass is real,
not an artefact of the formula.)

**OPEN — 211.25 ms fails.** An advertiser at Apple's recommended 211.25 ms drifts only
9.375 ms per scan against 5905.625 ms — five scans sweep 47 ms of the 111 ms the window
misses, so a command starting at a bad phase is unheard by all of them. Neither Apple
device measured so far used 211.25 ms (Mac 187.5 ms, iPhone ~35 ms in the foreground),
but iOS moves a backgrounded advertiser to a slower interval, and a future OS or the
production Android phone could land on this or another failing value. The period is
**not** changed by this amendment; resolving this — a period that passes against every
interval the app can plausibly present, or a guarantee that the app only advertises in
the foreground — is an open item. The production Android phone's interval must be
measured with `tools/uuid_observer` and checked against this rule (bench checklist §5d).

## 3. Continuous scanning during Arming

While the state is **Arming** the scanner runs **continuously** (the same cadence an
armed trigger delay uses), so a Disarm sent during the 10 s exit delay is heard within
a fraction of a second of the phone advertising. It returns to duty-cycled scanning
when Arming ends (Active, cancelled, or failed) unless an armed trigger delay still
needs it.

**One arbiter:** the requested cadence is `fast = (state == Arming) || armedDelayPending`,
decided in one pure function and applied in one place in `App`; the detection engine's
`SetTriggerPendingScan` request becomes one input to it. The PM lock stays tied to the
armed trigger delay only. Scanner failures keep today's handling and retry.

## 4. App: Send and Stop

Every page that sends (Arm page, Settings page, provisioner) has **Send** and a red
**Stop**:

- **Idle:** Send enabled (subject to Bluetooth and page rules), Stop dimmed.
- **Send pressed:** Send dims immediately; Stop becomes active once advertising has
  started (or at once, cancelling the start).
- **Stop pressed, or the 30 s window ends:** advertising stops; Stop dims; Send
  becomes active.

The "Send Disarm (replaces advert)" pre-empt is **removed**. To disarm straight after
arming — or to cancel an arming — the engineer presses **Stop**, sets Disarmed, and
presses **Send**. The disarm prompt still appears only for a Disarm Send that started
advertising. The legend explains Stop.

## 5. Replay of a previous command

Because the platform can re-broadcast the previous payload for a moment when a new
advert starts (§1), a command that was **never heard** can reach the device just
before the next one. Consequences, all acceptable:

- The replayed command is authentic, fresh-checked (10-minute window) and consumes its
  own sequence number; the new command, with a higher `n`, follows.
- A spent command replays silently (not for us).
- The worst realistic case is an unheard Settings or Arm acted on moments before the
  engineer's next command; the next command still takes effect after it. Stop (§4)
  lets the engineer end an advert once LED A confirms, which reduces what can linger.

No firmware change; documented so an unexpected acknowledgement is explicable.
