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

**Rule:** the scan period must not be within a few ms of an integer multiple of any
measured counterpart advertising interval longer than the scan window.

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
