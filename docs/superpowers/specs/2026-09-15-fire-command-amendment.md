# MFS_1 FIRE Command — Design Amendment

**Date:** 2026-09-15
**Amends:** `docs/superpowers/specs/2026-09-14-command-types-amendment.md` §2.1, §2.2,
§2.3, §5; `docs/tan-scheme.md` §3 (derivation, `protocolVersion`), §6.1, §6.2, §6.6;
`docs/superpowers/specs/2026-09-14-arming-sequence-amendment.md` (the fail-safe rule);
project `CLAUDE.md` (scan-only rule, always-fail-safe rule).
Where this document and those sections disagree, **this document wins**.

## 1. The requirement

An operator needs to **fire an armed device on command**, without waiting for
motion — from the phone, or (more usually) from a drone or a BLE hub in the same
building. Owner decisions, 2026-09-15:

1. **The device must already be armed (Active).** FIRE never enables the fire pins
   itself. It is one more fire condition, ANDed with the arm boolean through
   `App::updateOutputState()` exactly like a detection — so the cardinal invariant
   (`CLAUDE.md`: the arm boolean is definitive, one output path) is untouched.
2. **A fixed 10 s countdown precedes firing**, so whoever pressed the button (a
   person with the phone) can leave. It is **not** the stored delay and **not** the
   activation count — FIRE skips both.
3. **The 10 s countdown cannot be stopped by a Disarm.** This is the one deliberate
   exception to "disarm pre-empts / always fail safe" (§4). A hardware fault on the
   fire path still aborts it; a scanner loss does not.
4. **The device confirms receipt** by breaking radio silence once — but with an
   authenticated, random-looking UUID that reveals nothing to a third party (§3).

## 2. Wire format — protocol version `0x04`

FIRE reuses command type **`00`**, which every prior version rejects
(`command-types-amendment` §2.1: "type `00` is reserved and rejects"). It is
distinguished from a bare reserved payload by a magic constant.

### 2.1 Why not literal "FIRE" in bytes 0–3

The command-type field is **byte 1, bits 6–7**. ASCII "FIRE" is `46 49 52 45`, so
byte 1 would be `0x49` — bits 6–7 `01`, which decodes as a **Settings** command, not
type `00`. Recognising the raw "FIRE" bytes ahead of the type dispatch would instead
steal one exact valid Settings combination (delay 35, 10 activations, cooldown byte
82, sensitivity byte 69) and overload it — the single-overloaded-value bug this
feature was explicitly designed to avoid.

So the magic lives in the bytes the base protocol fully owns and a non-Settings
command leaves free — **bytes 0, 2, 3** — with **byte 1 held at `0x00`** (a genuine
type-`00`, no activations or mode smuggled in). Bytes 6–7 are **not** used: CLAUDE.md
forbids MFS_1 from ever validating the per-variant extension space. Three bytes hold
the magic; the owner chose the memorable **"OMG"** (`4F 4D 47`), which fits exactly and
leaves byte 1 clear of the type field.

### 2.2 Plaintext layout for a FIRE command

| Byte | Bits | Value | Notes |
|---|---|---|---|
| 0 | — | `0x4F` `'O'` | Magic |
| 1 | 0–5 | `0` | Must be zero |
| 1 | 6–7 | `00` | Command type — Reserved dispatch |
| 2 | — | `0x4D` `'M'` | Magic |
| 3 | — | `0x47` `'G'` | Magic |
| 4–5 | 0–10 | UTC minute of day, LE | Freshness; ≥ 1440 rejects |
| 4–5 | 11–15 | reserved — ignored | |
| 6–7 | — | per-variant extension | **MFS_1 ignores, never validates** |

- **FIRE is recognised iff** the type field is `00`, byte 1 is `0x00`, and
  `(byte 0, byte 2, byte 3) == (0x4F, 0x4D, 0x47)` ("OMG"). Any other type-`00` payload
  still rejects as malformed, so an all-zero plaintext still does nothing.
- The magic's job is to stop **our own bugs** from manufacturing a type-`00` fire (a
  dropped shift, a mis-packed field). It is not an attacker control: only a day-key
  holder can produce any authentic command at all. 24 bits of magic plus byte 1 == 0
  is far more than enough for that.
- **`protocolVersion` becomes `0x04`.** It is associated data, never on air, so a
  `0x03` app cannot send FIRE and a `0x03` payload fails authentication rather than
  being reinterpreted. Nothing is deployed, so there is no migration. (An old-firmware
  device would in any case reject a FIRE payload — type `00` rejects — but the bump
  keeps the versions cleanly separated, as the `0x02`→`0x03` bump did.)

### 2.3 What the device does — extends `command-types-amendment` §2.2

`DecideCommand()` (`src/arm_policy.hpp`) stays the single place this is decided. A new
`ArmAction::Fire` is added.

| Device is | FIRE | Action | Pins | Confirmation | Indicator |
|---|---|---|---|---|---|
| **Active** | FIRE | **Start the 10 s fire countdown**, then fire (one-shot, latches Inactive) | already live (armed) | yes | fire pins after 10 s |
| **Inactive** | FIRE | **Rehearsal** — same 10 s countdown and confirmation, but the "detonation" shows on **LED B only** | untouched | yes | LED B after 10 s |
| **Arming** | FIRE | **Ignore** — logged, no confirmation | isolated | no | — |

- **Active/FIRE fires through the ordinary output path.** FIRE sets a one-shot fire
  latch; `updateOutputState()` computes `output = armState.Active AND (detection OR
  fireLatch)`. There is no new path to the pins, and no new pin-enable step — the
  pins are already enabled because the device is Active. After firing, the device
  latches Inactive exactly as a motion trigger does (`arm_policy.hpp`: firing is one
  of the two ways out of the armed state).
- **Inactive/FIRE is a full, safe rehearsal**, consistent with disarmed test mode
  (`disarmed-test-mode-amendment`): the countdown runs, the confirmation is sent, and
  the "fire" appears on LED B instead of the pins. It lets the button be tested on the
  bench without risk. **It never touches the fire pins.**
- **Arming/FIRE is ignored**, like every command but Disarm during the exit delay.
- **Freshness applies (10 min, `tan-scheme.md` §6.2 step 7)** and the sequence number
  is consumed before acting, so a FIRE cannot be replayed once accepted. A captured,
  undelivered FIRE could still be delivered within its 10-minute freshness window —
  the same bound as any command; see §6 for whether FIRE should tighten it.

## 3. Confirmation of receipt — a bounded exception to scan-only

The device is scan-only so it never announces its presence to a third party
(`CLAUDE.md`). A FIRE confirmation does not have to break that: the device already
holds the day key, so it can emit a **single authenticated UUID that looks exactly as
random as a command's rotating ID**. To anyone else scanning the band it is
indistinguishable from noise or a random BLE address; only a key holder can recognise
it. Covertness against third parties is preserved; only the fact-of-a-transmission is
new, and only at the moment of firing.

### 3.1 Derivation — extends `tan-scheme.md` §3

```
confirmId(n, event) = HMAC(dayKey, n(4) ‖ event(1) ‖ 0x06)[0..15]
```

- **Label `0x06`** joins `0x02`–`0x05`; `0x00`/`0x01` remain retired and must not be
  reused.
- **Bound to `n`** — the sequence number of the FIRE command being confirmed — so a
  confirmation is unique to one FIRE and cannot be replayed against another.
- **`event`** is `0x01` = "FIRE received, countdown started". One byte leaves room for
  later events without a new label. Full 16 bytes, so the whole UUID is unforgeable
  and carries no cleartext.
- The listener (phone, drone, hub) holds the same day key and computes the expected
  `confirmId(n, 0x01)` for the FIRE it just sent, then scans for it.

### 3.2 Timing and behaviour

- **Confirm on receipt, not on detonation.** Detonation destroys the device, so a
  "detonated" advert would usually never be heard. The useful, reliable signal is
  that FIRE was authenticated and the countdown is running.
- The device advertises `confirmId(n, 0x01)` as a **short burst over the first few
  seconds of the 10 s countdown** — a burst, not one advert, to beat the ~50 %
  per-wake detection odds. Covertness is spent at the moment of firing, so a brief
  burst costs nothing that matters.
- The **rehearsal (Inactive/FIRE) sends the same confirmation**, so a bench test
  exercises the whole path.

## 4. The unstoppable countdown — a bounded exception to always-fail-safe

`CLAUDE.md`'s owner rule is that any fault, or a Disarm, isolates the pins and stands
the device down. FIRE carves out one narrow, deliberate exception, and no more:

- **Once FIRE is accepted while Active, the 10 s countdown runs to firing.** A Disarm
  received during it is **refused and logged** (`Disarm ignored during fire countdown.`)
  and the fire proceeds. This is the intended semantics of a fire command: it must not
  be cancellable by the same channel that could be jammed or spoofed into cancelling it.
- **A scanner loss during the countdown does NOT abort it.** The command has already
  landed; there is nothing left to hear. `ServiceScannerHealth()` must therefore **not**
  fail-safe a device that is in the fire countdown (it still fails-safe an ordinary
  Active or Arming device).
- **A fire-path hardware fault DOES abort it, failing safe.** If the fire switch
  reports a fault (`OutputSwitch` disable/clear/read-back failure, per the
  arming-sequence amendment §4.1), the hardware physically cannot fire safely; forcing
  it achieves nothing and risks an undefined state. The countdown aborts, the pins are
  isolated, the device disarms, and `signalWarning()` plays. No confirmation of firing
  is (or could be) sent.

Summary: the countdown is unstoppable **by command and by scanner loss**, and stoppable
**only by a fire-path hardware fault**.

## 5. App — a FIRE button on the Arm page

Extends `command-types-amendment` §3.1.

- A **large red Fire button** on the Arm / Disarm page.
- Pressing it opens a **large red warning field with a confirmation slider**. Moving
  the slider to the end reveals a **Fire** button. Pressing that sends the FIRE command.
  Two deliberate actions, because FIRE cannot be recalled once the device hears it
  (Stop only ends the advert; a FIRE already received still fires).
- After sending, the app **switches from advertising to scanning** for
  `confirmId(n, 0x01)` and shows the confirmation when it is heard (or a timeout if it
  is not). This is a **new capability in the app** — it has only ever advertised
  (iOS `CBPeripheralManager`); scanning is `CBCentralManager` — see §7.
- The 10 s countdown belongs to the **device**, not the app. The app shows it for the
  operator's benefit but does not own or gate it.

## 6. Open points

- **FIRE freshness window.** FIRE inherits the 10-minute freshness bound. Because FIRE
  cannot be recalled, a tighter window (e.g. 1–2 min) would shrink the capture-and-delay
  attack surface. Deferred to owner review; not changed here.
- **Slot restriction.** FIRE is currently allowed from any slot, like Arm/Disarm. The
  owner may wish to restrict it (slot 0 only, or a named slot). Deferred.
- **LED A during the countdown.** Whether LED A shows anything distinct during the fire
  countdown (versus the confirmation advert being the only acknowledgement) is a small
  UX point to settle at implementation.

## 7. Impact

| Area | Change |
|---|---|
| `mfs_protocol` | recognise FIRE (type `00` + byte 1 `0x00` + magic `4F 4D 47` "OMG" in bytes 0/2/3); `M_PROTOCOL_VERSION` `0x04`; `CommandType` unchanged (FIRE is a distinguished Reserved), or a decode-level `isFire` flag on `Command` |
| `arm_policy.hpp` | `ArmAction::Fire`; `DecideCommand()` rows per §2.3 |
| `App` | fire latch ORed into `updateOutputState()`; 10 s fire countdown timer; Inactive rehearsal on LED B; Disarm refused during countdown; fire-path fault aborts, scanner loss does not; confirmation burst |
| `arming_sequence` | `ServiceScannerHealth()` must not fail-safe during the fire countdown |
| advertising | **new** — the device must advertise a `confirmId` burst; the scan-only device has no advertising path today (report modes anticipated it but are unbuilt) |
| `access_keys` / crypto | `confirmId(n, event)` derivation, label `0x06` |
| `tools/gen_access_vectors.py`, `access_vectors.hpp`, app `test/access_vectors.dart` | regenerated for version `0x04`; add FIRE and `confirmId` vectors |
| `crypto_selftest` | new vectors, via the regenerated header |
| `class_app` | Fire button, red confirm-slider sheet, send FIRE, switch to scanning for `confirmId`, confirmation/timeout UI |
| host tests | `test_mfs_protocol` (FIRE recognition, near-miss rejection), `test_arm_policy` (Fire rows), `test_arming_sequence` (scanner loss vs fire-path fault during countdown), `test_access_*` |
| docs | `tan-scheme.md` and `command-types-amendment` pointed here; `CLAUDE.md` scan-only and always-fail-safe rules gain the FIRE exceptions; bench checklist gains a FIRE section |
