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
| **Rapid flash**, 100 ms on / 100 ms off | **Counter mismatch** — an authentic command arrived ahead of the 16-number window. Use another device, or wait for 04:00 UTC | 5 s per such command (nothing was accepted, so nothing to latch) |

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
- a forged tag is `AuthFailed`, counted towards the lockout per distinct ID and
  per-ID guess cap exactly like a window ID;
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
the device flash; it cannot make it act.

## 5. Counters across power cycles and reflashes

A power cycle does **not** reset the counters: `next[]` and the day are in NVS
(`access/v1`), persisted before every accepted command and reloaded at boot.
Only an erase of the settings partition resets them. `tools/flash_device.sh` now
**keeps NVS by default** (plain `west flash`, which erases only the pages the
image touches) and wipes only with `--wipe` (`--recover`). Use `--wipe` when a
board takes a different serial — another device's NVS does not belong to its
keys. The earlier note that `west flash` without `--recover` once reported
success while the old image kept running is to be re-checked on the bench
(checklist §5j.1).

## 6. Verification

Host: `test_access_control.cpp` (ahead verdict, edges at 16 and 64, stale,
forged tag, lockout over ahead IDs, replay still silent, window limit
unchanged), `test_led_sequencer.cpp` (rapid flash timing and priority),
`test_led_indicator.cpp` (slow flash, outranking the disarmed latch). Bench:
checklist §5j.
