# Arming failure codes and the counter mismatch — amendment (owner, 2026-11-14)

Amends `2026-11-12-three-led-indication-design.md` (§3, and the §7 "reason codes
for orange" to-do), `docs/tan-scheme.md` §6.2 and §6.6, and the "failures emit
nothing" rule in `CLAUDE.md`.

## 1. Why

On 2026-11-14 a board failed to arm twice and could not say why. Once the cause
was a battery test (fire supply) that could not reach 3.2 V; the rest of the
time an Arm was silently ignored because the device's sequence counters had been
erased by a `--recover` reflash while the app's carried on — more than 16 of the
day's Sends past the window, so every Arm was "not for us" until 04:00 UTC.

## 2. Codes (orange, LED2)

| Code | Meaning | Duration |
|---|---|---|
| **Slow flash**, 500 ms on / 500 ms off | Arming refused: **low battery** — the fire-supply battery test could not hold 3.2 V | Latched until the next accepted command or time sync (as the device-disarmed latch) |
| **Rapid flash**, 100 ms on / 100 ms off | **Counter mismatch** — an authentic command arrived ahead of the 16-number window. Use another device, or wait for 04:00 UTC | 5 s, **once per sequence number**, and **only while Inactive** (§4) |

The low-battery failure still raises the warning (orange steady 3 s) first, then
the slow flash. In the base level the low-battery latch outranks the
device-disarmed latch. The rapid flash is an overlay: above the orange triple,
below the red flash and the warning.

## 3. Recognising a counter mismatch (owner: flash only)

`AccessControl` now derives rotating IDs for **64** sequence numbers from `next`
(`M_LOOKAHEAD`), of which the first **16** (`M_WINDOW`) are acceptable, exactly as
before. A match beyond the window is trial-decrypted; if it is **authentic and
fresh** (minute within 10 min) the verdict is `AheadOfWindow`:

- **never consumed and never acted on** — nothing is persisted, the window does
  not move. The acceptance rules are unchanged (owner chose "flash only", not
  resync);
- a forged tag is `AuthFailed` and burns that ID after 8 attempts (per-ID guess
  cap), but **never counts towards the 20-failure lockout** (security review,
  2026-11-14): a slot exposes only 16 acceptable IDs, fewer than 20, so an
  eavesdropper replaying a phone's unheard Sends with forged tags could never
  lock a slot out, and the look-ahead must not change that;
- each ahead number is reported **once**: afterwards its ID is skipped like a
  burned one (`m_mismatch_shown`, cleared when the window moves);
- stale or malformed ahead commands get their usual silent verdicts;
- a command **behind** the window (a replay of an accepted one) is still
  `NotForUs` — silent.

The acceptance window's sequence limit is unchanged; only the look-ahead shrinks
near the (unreachable) 2^32 limit, so no derived number wraps.

Cost: 512 IDs (2 KB RAM) instead of 128; 64 HMACs per accepted command and 512
at each day rollover / time sync, instead of 16 and 128.

## 4. The silence rule

"Failures emit nothing — no advert, no LED" stands for every unauthenticated
input. The rapid flash is shown only for a command that **authenticated under
the day key** and is fresh, so it tells nothing to anyone without that key — the
same reasoning as the orange triple for an accepted command. A captured,
unheard ahead command replayed within its 10-minute freshness window can make
the device flash; it cannot make it act. So (security review, owner 2026-11-14):

- **once per sequence number** — a replay of the same capture shows nothing more;
- **never while Arming or Active** — an armed device stays silent, as it always
  has; the mismatch is logged over RTT only. An engineer finds a mismatch on a
  disarmed device (Arm or Disarm attempt) or by its silence.

## 5. Counters across power cycles and reflashes

A power cycle does **not** reset the counters: `next[]` and the day are in NVS
(`access/v1`), persisted before every accepted command and reloaded at boot.
Only an erase of the settings partition resets them — which every
`tools/flash_device.sh` flash does, because it flashes `--recover`.

**Keeping NVS across a reflash does not work yet on this board** (bench,
2026-11-14): plain `west flash` (which erases only the pages the image touches)
programmed and verified the image, but the old firmware kept running and the
new one did not boot even after a J-Link reset. `--recover` therefore stays the
default; `--keep-nvs` is an explicit, unverified option until the cause is found
(checklist §5j.1). After a `--recover` flash, expect the counter mismatch on any
slot already used more than 16 times that day: use a fresh serial or slot, or
wait for 04:00 UTC.

## 6. Verification

Host: `test_access_control.cpp` (ahead verdict, edges at 16 and 64, stale,
forged tag, lockout over ahead IDs, replay still silent, window limit
unchanged), `test_led_sequencer.cpp` (rapid flash timing and priority),
`test_led_indicator.cpp` (slow flash, outranking the disarmed latch). Bench:
checklist §5j.
