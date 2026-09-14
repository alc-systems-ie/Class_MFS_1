# MFS_1 Command Types — Design Amendment

**Date:** 2026-09-14 (bench session 1)
**Amends:** `docs/superpowers/specs/2026-09-12-app-control-design.md` §2, §4, §4.1,
§4.2, §6.4, §6.5, §6.7, §8.1, §8.2; `docs/tan-scheme.md` §3 (`protocolVersion`), §6.1,
§6.5.
Where this document and those sections disagree, **this document wins**.

## 1. The finding

Bench, 2026-09-14: the device held 3 activations / 8 s cooldown. An engineer
sent Arm from the app with its default sliders, and the device armed **and
overwrote its tuning** with 1 activation / 0 s cooldown (`n 14`). A second Arm
carrying the real settings was then correctly ignored as armed (`n 15`).

Because every command is an absolute state assertion carrying every setting
(old §4.2), **arming or disarming is only safe if the engineer already knows the
device's settings.** An engineer servicing many devices does not, and must not
need to. A Disarm has the same flaw: sent to a device that is already Inactive, it
is a Tune and overwrites the settings.

Owner requirement: **arm and disarm without any knowledge of settings.**

## 2. Three command types

The plaintext gains an explicit command type. Arm and Disarm carry **no settings**;
settings travel only in a Settings command.

| Type | Meaning | Fields the device reads |
|---|---|---|
| **Arm** | be Active, with the settings already stored | minute only |
| **Disarm** | be Inactive | minute only |
| **Settings** | apply these settings (Inactive only) | delay, activations, mode (slot 0), cooldown, sensitivity, minute |

### 2.1 Wire format — protocol version `0x03`

Plaintext, replacing `docs/tan-scheme.md` §6.1:

| Byte | Bits | Field | Notes |
|---|---|---|---|
| 0 | 0 | reserved — ignored | was the arm bit |
| 0 | 1–7 | delay code | Settings only |
| 1 | 0–3 | activations − 1 | Settings only |
| 1 | 4–5 | operating mode | `3` rejects on every type; honoured from slot 0, Settings only |
| 1 | 6–7 | **command type** | `00` **reserved — rejects**, `01` Settings, `10` Arm, `11` Disarm |
| 2 | — | cooldown byte | Settings only |
| 3 | — | sensitivity byte | Settings only |
| 4–5 | 0–10 | UTC minute of day, LE | every type; ≥ 1440 rejects |
| 4–5 | 11–15 | reserved — ignored | |
| 6–7 | — | per-variant extension | MFS_1 must ignore, never validate |

- **Type `00` is reserved and rejects**, like reserved mode: not consumed, logged
  as malformed. An all-zero plaintext therefore does nothing, rather than
  meaning "apply all-zero settings".
- **The app sends zero in every field an Arm or Disarm does not read.** The
  device ignores them regardless; it does not validate them beyond the mode
  check that applies to every type.
- **`protocolVersion` becomes `0x03`.** It is associated data and never on air, so
  a `0x02` command from an old app build fails authentication and is silent,
  instead of having its reserved bits read as a command type. Nothing is deployed,
  so there is no migration.

### 2.2 What the device does — replaces design spec §6.4

`DecideCommand()` (`src/arm_policy.hpp`) stays the single place this is decided.

| Device is | Command | Action | Settings | Mode | Clock trim | LED A |
|---|---|---|---|---|---|---|
| **Active** | Disarm | **Disarm** | no | no | yes | slow flash (or double blink if a delay was cancelled) |
| **Active** | Arm | **No change** — replay state | no | no | no | rapid flash (Armed) |
| **Active** | Settings | **No change** — replay state | no | no | no | rapid flash (Armed) |
| Inactive | Arm | **Arm with the stored settings** | no | no | yes | rapid flash, or three long pulses if refused |
| Inactive | Disarm | **Stay Inactive**; detection engine cleared, ADXL367 to standby | no | no | yes | slow flash (Disarmed) |
| Inactive | Settings | **Tune** | yes | slot 0 only | yes | single blink (+ two blinks if mode changed) |

> **Pointer — amended 2026-09-14 (disarmed test mode):** the Inactive rows above
> predate `docs/superpowers/specs/2026-09-14-disarmed-test-mode-amendment.md`,
> which supersedes them where they disagree. "ADXL367 to standby" on the
> Inactive/Disarm row is no longer accurate — a disarm now **restarts the test
> from zero**, reconfiguring the ADXL367 afresh rather than standing it down,
> because the detection engine runs continuously while disarmed.

Rules this preserves or introduces:

- **Armed, the only state change is disarm.** Unchanged. An armed device still
  applies no settings, no mode and no trim from anything but a Disarm (trim only),
  and still leaves the armed state only by disarm or one-shot trigger.
- **A command that changes nothing replays the current state on LED A** (owner
  decision, 2026-09-14). An engineer who does not know the state sends Arm and sees
  Armed, or sends Disarm and sees Disarmed. This is not a breach of silence on
  failure: the command authenticated. It reveals the arm state only to a key
  holder, who could learn it anyway by sending Disarm.
- **Settings sent to an armed device also replay Armed** (ruling, 2026-09-14). The
  app only offers Settings after a confirmed disarm, so this case means the
  engineer's confirmation was wrong; the rapid flash tells them so instead of
  silence.
- **Disarm while Inactive runs the ordinary deactivation** (clear the count, the
  latch and any cooldown, then restart the test from zero — see the pointer
  above; the ADXL367 is no longer stood down). Its only visible effect is a
  restarted test, so LED B stops and relights on the next completed count.
- **Arm uses exactly the stored settings** — the last Settings command applied, or
  `params/v1` restored at boot. The old guarantee (§4.2: "no window in which the
  device is armed with settings the engineer did not watch being tested") becomes a
  workflow property, not a protocol property: the app's flow (§3) goes Disarm →
  Settings → watch LED B → Arm, and the stored settings are those just watched.
  **What is given up**, deliberately: an engineer may now arm a device whose
  settings they have never seen. That is the owner's requirement.
- **Persist before acting, silence on failure, freshness, lockout** — all unchanged.
  A replay-state command is authentic and fresh, so it consumes its sequence
  number like any other.

### 2.3 LED A — additions to design spec §6.7

| Result | LED A pattern |
|---|---|
| Armed, Arm or Settings received | **Armed** pattern (rapid flash, 3 s) |
| Inactive, Disarm received | **Disarmed** pattern (slow flash, 3 s) |

The row "*(armed, command ignored)* — nothing" is removed. Every other row stands.

## 3. App flow — replaces design spec §8.1 and §8.2

**The app never knows the device state, so it assumes the device is armed.**

### 3.1 Arm page — the MFS_1 home screen

- Device picker, the arm slider, **Send**, the advertising countdown. **No settings
  controls on this page at all.**
- The slider **defaults to Armed** every time the page is shown.
- **Send with Armed** → an Arm command. The engineer watches for the rapid flash.
- **Send with Disarmed** → a Disarm command, then, once advertising has started,
  a confirmation prompt:

  > **Confirm LED A shows the device is disarmed** — a slow flash for 3 seconds
  > (or a double blink if a pending trigger was cancelled).
  >
  > If the device is not showing disarmed, please move closer and confirm that the
  > correct device has been selected. If the device does not respond, the battery
  > may be discharged.
  >
  > [ Disarmed — open settings ]  [ Not seen ]

  **Disarmed** opens the Settings page. **Not seen** closes the prompt and stays on
  the Arm page; the engineer sends again (a new sequence number, as always).

- **The prompt describes the slow flash, not a solid LED.** A solid LED A while
  Inactive is a bench-build aid (`CONFIG_MFS_DEBUG_LED`); in a production build
  LED A is dark while Inactive, and only the patterns are real.

### 3.2 Settings page — reachable only through a confirmed disarm

- **Every slider resets to its default on entry** (existing behaviour, kept).
- Controls: activations, cooldown (shown when activations > 1), delay, sensitivity;
  **Send**, which builds a Settings command; **Restore defaults**, which resets the
  sliders to the factory defaults **in the app only** — the engineer then sends
  them like any other settings. There is no on-air factory-reset command.
- The **Network Manager (bench)** mode section moves here: a mode change is a
  slot-0 Settings command and only works while Inactive, so it belongs behind the
  same gate.
- **Leaving the page returns to the Arm page with the slider at Armed.** The gate
  is per visit and per device: changing the device, or leaving and returning,
  requires a fresh confirmed disarm.
- The engineer arms from the Arm page. The device arms with the settings last
  applied.

### 3.3 Workflow

1. Pick the device. The Arm page shows Armed.
2. Set Disarmed, Send. Watch LED A; confirm the slow flash, or follow the
   fault-finding text and send again.
3. On the Settings page adjust, Send. **Single blink** — applied. Watch LED B
   simulate triggers. Repeat as needed; Restore defaults if lost.
4. Back to the Arm page (slider at Armed), Send. **Rapid flash** — armed with the
   settings just watched. **Three long pulses** — refused; check the device.

An engineer who only needs to arm or disarm never sees a setting.

### 3.4 The advertising countdown must be honest

Found on the same bench session: `Advertiser.send()` starts the 30 s countdown
whether or not `BlePeripheral.startAdvertising` succeeded, and the advertising
status error only reaches `debugPrint`. With the disarm prompt the engineer now
makes a decision on the strength of that countdown, so:

- the countdown starts only once the platform reports advertising **on**;
- an advertising error is shown on screen and the Send counts as failed (its
  sequence number is still spent — never reused);
- the disarm prompt is shown only for a Send that actually started advertising.

## 4. DECIDED — do not remember device settings in the app (Option A, 2026-09-14)

It would help the engineer to see a device's last-applied settings on the Settings
page instead of defaults. **It is a major security flaw**, and is not being built
without an explicit decision from the end user. The decision is recorded at the end of this section.

- **A lost or stolen phone becomes a map of the installation**: which devices exist,
  and for each, how sensitive it is, how many activations it needs, its cooldown and
  its delay — exactly what an intruder needs to defeat a sensor (move slowly under
  the threshold, or trigger it once and act inside the cooldown or delay).
- The current design limits a lost phone to **day keys that die at 04:00 UTC**
  (`docs/tan-scheme.md` §1). Stored settings have no expiry, so they would outlive the
  keys indefinitely, and nothing on the device can revoke them.
- The phone's knowledge would also go stale silently — another engineer, the
  Network Manager, or a later Settings command changes the device, and the app would
  show confident, wrong values.

Options for the end user:

- **A. Do not store** (current design). Settings always start from defaults.
- **B. Store in the back office, not on the phone** — the Network Manager records
  each Settings command it issues keys for and returns them with the day key, so
  they expire with it. Needs the real Network Manager.
- **C. Store on the phone, encrypted under the day key**, so they are unreadable once
  the key has expired. Still exposes them for the rest of the day on a lost phone,
  and still goes stale.

**Decided by the owner, 2026-09-14: Option A.** The app never stores device settings. B and C are recorded for a future review, not scheduled.

## 5. Impact

| Area | Change |
|---|---|
| `mfs_protocol` | `CommandType` enum, field in byte 1 bits 6–7, type `00` rejects; arm bit removed; `M_PROTOCOL_VERSION` `0x03` |
| `tools/gen_access_vectors.py`, `access_vectors.hpp`, app `test/access_vectors.dart` | regenerated for version `0x03` and the new field |
| `crypto_selftest` | new vectors, via the regenerated header |
| `arm_policy.hpp` | `DecideCommand()` per §2.2, including replay-state actions |
| `App::applyCommand()` | replay patterns; Inactive Disarm deactivates; Arm uses stored settings |
| tests | `test_mfs_protocol`, `test_arm_policy`, `test_access_*` updated |
| `class_app` | protocol mirror, Arm page, disarm prompt, Settings page with Restore defaults, honest countdown |
| docs | design spec and `tan-scheme.md` sections pointed here; project `CLAUDE.md` access rules; bench checklist §5–§6 re-cut |
