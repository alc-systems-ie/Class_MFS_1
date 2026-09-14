# MFS_1 Access Scheme — Day Keys

Project CLASS — Multi-Function Sensor 1.

**Rewritten 2026-09-13.** This replaces the paper-TAN scheme recorded 2026-08-17.
The filename is kept because `CLAUDE.md` and several documents cite it. What was
retired, and why, is in §10.

How an engineer's phone gets authority over a device for one day, how the device
checks it, and how the device keeps the time that bounds it. Power and timekeeping
constraints this depends on are in `docs/power-budget.md` §8. The wire format and
firmware units are in `docs/superpowers/specs/2026-09-12-app-control-design.md`.

## 1. Requirement being met

The **Network Manager** knows every device's secret. An engineer's phone is issued
**day keys** for the devices that engineer is assigned, for one day. If the phone is
lost or stolen, only **those devices** are exposed, and only **until the next
04:00 UTC**. Keys are worthless the next day whether or not they were used.

Three further requirements came out of the adversarial review of 2026-09-13 (§9):

- **Nobody listening can read the settings.** A readable command tells an attacker
  the sensitivity, activation count, delay and mode — how to beat the sensor.
- **Nobody listening can tell which device is being addressed**, or link one
  command to the next.
- **Two engineers can hold keys for the same device on the same day** without
  interfering with each other.

## 2. The central idea

**The day is not a label attached to a key. It is an input to the key itself.**

```
deviceSecret  (per device, forever)          held by: device, Network Manager
    │
    └─ dayKey(id, day, slot)                 held by: one engineer's phone, one day
          ├─ encKey                          AES-128 key for the command
          └─ rotatingId(n)                   what goes on air in place of an address
```

A key for day 256 is arithmetically unrelated to day 257's. The device never asks
"which day is this command for?" — it derives today's keys from its own clock and
asks "does this decrypt under one of them?"

- **Nothing about the date is transmitted, so nothing about the date can be
  forged.** A sender that could say "this is for day 256" would let a lost day-256
  key work forever.
- **Keys are device-specific**, because the secret is. A key for one MFS_1 does
  nothing to another.
- **Nothing is stored but a counter.** The device derives the day's keys on demand.

**It does not contradict the ban on counter-indexed codes** (`docs/power-budget.md`
§8.1). That ban exists because HOTP-style codes *never expire unused*. Here the
sequence number is nested **under** a day key, so every unused code dies at the day
boundary regardless. The rule survives as: *every code must expire at the day
boundary.*

## 3. Derivation

All HMACs are HMAC-SHA256. `‖` is concatenation. Integers are big-endian in every
derivation input.

```
dayKey(id, day, slot) = HMAC( deviceSecret,  id(4) ‖ day(2) ‖ slot(1) ‖ 0x02 )
encKey                = HMAC( dayKey, 0x03 )[0..15]
rotatingId(n)         = HMAC( dayKey, n(4) ‖ 0x04 )[0..3]
nonce(n)              = id(4) ‖ day(2) ‖ slot(1) ‖ n(4)                  (11 bytes)
aad(n)                = rotatingId(n) ‖ protocolVersion(1)                (5 bytes)
command(n)            = rotatingId(n) ‖ AES-128-CCM(encKey, nonce, aad, plaintext(8), tag 4)
timeSync(t)           = t(4, LE) ‖ HMAC( provisionKey, id(4) ‖ t(4) ‖ 0x05 )[0..11]
```

| Symbol | Size | Notes |
|---|---|---|
| `deviceSecret` | 256 bits | Per device, generated at manufacture (§8) |
| `provisionKey` | 256 bits | Per device, **distinct from the secret** (§8.1) |
| `id` | 32 bits | Public device identity. Never on air. Binds every derivation to one device, so two devices accidentally given one secret still get different keys |
| `day` | 16 bits | Day index (§4) |
| `slot` | 8 bits | 0–7. **Slot 0 is the Network Manager's**; 1–7 are engineers (§5) |
| `n` | 32 bits | Sequence number within (device, day, slot) |
| `protocolVersion` | 8 bits | `0x02`. Never on air — both sides supply it, so a payload from another version fails authentication instead of mis-parsing |

**Labels `0x02`–`0x05` give domain separation.** `0x00` and `0x01` were the retired
paper-TAN and session-key labels and **must not be reused**.

**Why CCM.** It encrypts and authenticates in one standard step, the 4-byte tag
fits the payload, and PSA supports it directly as
`PSA_ALG_AEAD_WITH_SHORTENED_TAG(PSA_ALG_CCM, 4)` — CRACEN accepts tag lengths
{4, 6, …, 16} (`nrf/subsys/nrf_security/src/drivers/cracen/cracenpsa/src/aead.c`).
**Do not substitute GCM**: its security collapses with a tag this short.

**Proven cross-platform, 2026-09-13.** `tools/gen_access_vectors.py` (Python
`cryptography`) generates known-answer vectors. Both OpenSSL on the host and
`pointycastle` 4.0.0 on Dart 3.11 reproduce every vector byte for byte, including the
4-byte CCM tag. PSA on the nRF54L05 reproduces them too: the firmware's boot self-test checks it
against the same vectors, and passed on hardware on 2026-09-14.

Bench vector, for reference — secret `00 01 … 1F`, `id = 0x4D465331`, day 256,
slot 1, n 0:

```
dayKey     AD 84 79 12 AA 46 96 7B AF 7B 87 38 A5 DC 95 D2 EB 7F 40 AF 5A 6D 3A 76 10 18 63 46 5A FD 77 86
rotatingId 45 E9 10 47
plaintext  01 02 80 8F 1E 02 00 00        (arm, 3 activations, 60 s, 302 LSB, 09:02 UTC)
on air     45 E9 10 47 E3 7F 62 27 72 7F 9B 51 37 9C 11 F3
```

### 3.1 The sequence number is the nonce — never reuse it

CCM's confidentiality depends on each (key, nonce) pair encrypting one plaintext.
Two different commands sent under the same `n` can be XORed together to reveal the
XOR of their settings, from which the arm state and delay usually fall out.

- **The app saves `n + 1` to storage before it starts advertising**, never after.
  A crash between the two then wastes a number instead of reusing one.
- **Two phones never share a slot.** That is what slots are for (§5).
- **Reinstalling the app or clearing its data must not reset `n`.** In this phase
  the store is local to the app; the production Network Manager should issue a
  fresh slot to a reinstalled app rather than trust the phone.

## 4. The day index — UTC, boundary at 04:00

```cpp
// Guard the subtraction: a time before the first boundary would underflow.
day = (unixSeconds - M_EPOCH_UNIX - M_DAY_BOUNDARY_OFFSET_SECS) / M_SECONDS_PER_DAY;
```

`M_EPOCH_UNIX = 1767225600` (2026-01-01T00:00:00Z), boundary offset 4 h. `uint16`
covers 179 years.

**Everything is UTC on the device. Never local time.** Ireland runs GMT in winter
and IST in summer, so a device and a back office each computing "04:00 local" would
disagree by an hour twice a year. The device has no timezone database and must not
acquire one.

04:00 UTC lands in the small hours in both target markets — 04:00/05:00 in Ireland,
05:00/06:00 in Germany — so clock error at the boundary can neither reject a working
engineer nor extend a lost key into a working day. See `docs/power-budget.md` §8.5.

## 5. Key issue — the Network Manager

1. The engineer logs in to the Network Manager at the start of the shift.
2. The Network Manager checks the app with **Play Integrity** (Android) or **App
   Attest** (iOS). *Deciding who gets keys is the whitelist*; the device cannot
   verify an app offline, and does not try.
3. For each device assigned to that engineer that day, it assigns a **slot 1–7**
   and issues `dayKey(id, day, slot)`, with a human-readable label for the device
   picker.
4. It logs the issue: who, which device, which slot, which day. **Attribution comes
   free** — the device records which slot armed it, and the Network Manager knows
   who held that slot.

**Slot 0 is never issued.** The Network Manager builds slot-0 commands itself and
the phone merely broadcasts them. The phone is the courier but cannot read or alter
them. **Only a slot-0 command may change the operating mode** (§6.5); the device
ignores the mode field from slots 1–7.

Consequences to accept knowingly:

- **The phone needs connectivity at shift start**, and again at the moment a mode
  change is sent. Caching tomorrow's keys would fix the first and immediately
  double the exposure window. Don't.
- **A revoked phone still works until 04:00.** The device has no way to hear about
  a revocation.
- **The engineer must pick the device.** Codes are per device and the phone
  advertises blind. Sending device A's command near device B is harmless — B finds
  no matching rotating ID — but the app needs an explicit picker.
- **Slots are per device, per day**, not a permanent identity.

**This phase has no Network Manager.** The app carries a **bench Network Manager**
holding the bench device secret, behind the same interface the real one will
implement (`docs/superpowers/plans/2026-09-12-app-control.md`). It is bench-only by
construction and must never ship.

## 6. The exchange

The device is scan-only. The phone advertises one 128-bit service UUID carrying the
16 bytes of §3 for 30 s per Send. See the design spec §3 for why 30 s.

### 6.1 Wire format

On-air order (a scanner's hex dump; the app builds the UUID string reversed):

| Bytes | Content | Visible to a listener |
|---|---|---|
| 0–3 | `rotatingId(n)` | Random-looking, changes every command |
| 4–11 | ciphertext | No |
| 12–15 | CCM tag | No |

Plaintext, after decryption:

| Byte | Bits | Field |
|---|---|---|
| 0 | 0 | arm state: `0` Inactive, `1` Active |
| 0 | 1–7 | delay code (spec §5.1) |
| 1 | 0–3 | activations − 1 (so 1–16 always valid) |
| 1 | 4–5 | operating mode; `3` rejects. **Honoured from slot 0 only** |
| 1 | 6–7 | reserved — ignored |
| 2 | — | cooldown byte |
| 3 | — | sensitivity byte |
| 4–5 | 0–10 | **UTC minute of day**, LE, 0–1439; ≥ 1440 rejects |
| 4–5 | 11–15 | reserved — ignored |
| 6–7 | — | **per-variant extension — MFS_1 must ignore, never validate** |

### 6.2 Acceptance

The device keeps `next[slot]` for each of the 8 slots and a table of the 16
expected rotating IDs per slot — **128 IDs, 512 bytes of RAM**.

1. **No valid clock → listen only for a provisioner time sync** (§7). No keys can be
   derived without a day.
2. **New day** (`today > state.day`): zero every `next`, **persist**, raise the clock
   floor, rebuild all tables. A failed persist refuses the day. The rollover is
   also adopted when a time sync is applied and at least once a minute while the
   clock is valid, so the floor tracks the real day even on a device that
   receives no commands.
3. **Match bytes 0–3 against the table.** No match → *not for us*: silent and **not
   counted**. This is the fate of every other advert in range, and of every repeat
   of a command already accepted, because its ID has left the window.
4. **Locked out** (§6.4) → ignore without decrypting.
5. **Trial-decrypt** each candidate `(slot, n)`. None authentic → count a failure.
6. **Decode.** Reserved mode or minute ≥ 1440 → reject, not consumed.
7. **Freshness.** Minute more than **10 min** from the device's own → reject, **not
   consumed**. A command captured, jammed and released later dies here; its minute
   is never fresh again and its key dies at 04:00.
8. **Persist `next[slot] = n + 1` before acting.** A failed persist → the command is
   not acted on. Reversed, a power loss between acting and saving would leave the
   command replayable.
9. Rebuild that slot's 16 IDs and hand the command to the app state machine.

**The window is 16.** The phone cannot know whether a command landed (~53 %
detection per wake), so it never resends a number: each Send uses the next `n`, and
the device accepts any of the next 16 and skips past it. Numbers skipped are dead.

**The sequence is consumed before the command is carried out**, so a command the
device then fails to execute — an arm refused because the ADXL367 would not
configure — is still spent. That costs nothing: the sequence never runs out, and the
engineer simply presses Send again, which uses the next number. It is why the old
"consume last so a failed configure cannot burn a code" ordering is gone.

State persisted: `day` (2 bytes) + `next[8]` (32 bytes). Written on day rollover and
on each accepted command.

### 6.3 What an attacker gets per guess

Without a day key, a guess must first hit one of 128 unpredictable 32-bit IDs, then a
32-bit tag: 128/2³² × 2⁻³², about **2⁻⁵⁷ per advert**. A dongle parked beside the
device at 20 ms lands ~5 guesses per 6 s wake, ~72,000 (≈ 2¹⁶) a day — **~2⁻⁴¹ per
day**. Brute force is not a
threat, with or without the lockout.

**A captured, unaccepted ID is a better target than blind guessing**, because
resending it matches the ID filter every time instead of needing a fresh hit. The
per-ID failure cap (§6.4) bounds that: at most **8 tag guesses per captured ID**
before it burns, about **2⁻²⁹ per captured ID** — still far short of useful, and
worse than blind guessing only by the cost of capturing the ID in the first place.

### 6.4 Lockout

- **Only an ID match followed by a failed tag counts, and each expected ID counts at most
  once.** Garbage cannot count, because nobody without the day key can produce a matching
  ID. An attacker who captures an unaccepted command can corrupt and resend it, but that ID
  counts once however often it is sent, so a lockout needs 20 distinct unaccepted commands.
  (Corrected 2026-09-13: without the count-once rule, one captured advert resent 20 times
  would lock out disarm.)
- **Independently, each expected ID also carries its own wrong-tag counter**
  (`M_MAX_ID_FAILURES = 8`): once one ID has failed 8 times it burns — treated
  exactly like no match at all, not even decrypted — so resending one captured ID
  cannot be used to guess its tag indefinitely, however many times it is sent. This
  is separate from the count-once lockout bit above: that bit counts an ID once
  towards the lockout regardless of how many times it is retried; this cap keeps
  counting every retry against that one ID until it burns. (Added 2026-09-13, final
  review: without it, the count-once rule left one captured, unaccepted ID guessable
  forever, since later failures on it were silently free.)
- 20 consecutive failures → locked for 10 min, doubling per lockout to a 4 h cap.
- Any authentic command clears the count and resets the duration.
- Timed on uptime, not UTC, so a clock trim cannot shorten or extend it.
- Held in RAM only. A reset clears it — but a reset also invalidates the clock, so it
  buys an attacker nothing.
- **Deactivate does not bypass the lockout** (decided 2026-09-13). With rotating IDs
  only a key holder or a corrupted packet can trigger one, so this is now an
  engineering-fault case rather than an attack surface.

### 6.5 What the device does with an accepted command

It is an **absolute state assertion** — "be in this state with these settings".
Settings travel with every command, so there is no window in which the device is
armed with settings the engineer did not watch being tested. Tuning while Inactive
uses ordinary commands with the arm bit clear; **there is no test code**.

The mode field is applied only from slot 0. The minute field then trims the clock
(§7.3).

**An armed device accepts one command: disarm** (design spec §6.4). Any other
authentic command is ignored — no settings, no mode, no trim, no re-arm — and a
disarm applies nothing but the disarm. A trigger is one-shot and latches the device
Inactive, so **disarm and trigger are the only two ways out of the armed state**. A
stolen phone can therefore do nothing to an armed device except disarm it.

### 6.6 Silence, and the one exception

**Failures emit nothing** — no advert, no LED. Silence denies an attacker any signal,
including whether a device is present.

**Accepted commands are acknowledged on LED A** (design spec §6.7), because without
it a jammed arm is invisible: the app says "sent", and the device sits disarmed all
night. A pattern plays only after authentication and only once the new state is real.
**A failed authentication never lights anything**, or the LED would tell an attacker
which guesses got through.

## 7. Time

The day index is a security parameter. See `docs/power-budget.md` §8.5–8.7 for the
LFXO, the crystal load capacitance and the threat analysis.

### 7.1 The clock is invalid on every boot

`DeviceClock` is `uptime + offset`, with uptime from the GRTC on the LFXO. **It
starts invalid on every boot** — cold start, brownout, watchdog, anything — and
becomes valid only through an authenticated provisioner sync. There is **no resume
from NVS**.

This closes the hole `docs/power-budget.md` §8.6 accepted. A device that resumed on a
persisted day would revive that day's keys after a power cut; a device that resumes
on *nothing* revives nothing. The persisted day index survives only as a **floor**.

The cost: **any reset needs a provisioner visit** before the device obeys commands
again. Arm state already defaults to Inactive on cold start, so a reset device is
safe, merely unresponsive. Retaining the offset across a *soft* reset (GRTC or
nPM2100 SCRATCHA) would remove that cost for brownouts and is recorded as a
refinement (§11).

### 7.2 Provisioner sync

`timeSync(t)` (§3): a 12-byte truncated HMAC under the **provisioning key**. A 96-bit
tag is ample: the device checks at most a handful of adverts per 6 s wake.

The device accepts it only when **all** hold:

1. **The clock is invalid.** A valid clock refuses every sync, however authentic.
   Otherwise a sync captured at a battery change and released hours later would pull
   the clock back and stretch a day key past 04:00.
2. `t` is at or after the first boundary of the epoch.
3. `day(t) ≥ floor`. Never below a day already seen.
4. `day(t) ≤ floor + 400`. A leaked provisioning key could otherwise push the clock
   years ahead (denial of service, not entry). A device stored unpowered longer than
   that needs wired recovery.

A first-ever boot has no floor, which is correct: that is the factory case.

Residual, accepted, stated generally: an authentic captured sync for any day at or
above the floor and within floor + 400 can be replayed later, while the clock is
invalid, and pins the clock to that captured day until the next reset. Capturing a
sync and getting the clock back to invalid both need physical access — the usual
route is cutting power, which also invalidates the clock itself (§7.1) — and physical
access already defeats the product more directly (`docs/power-budget.md` §8.6).

### 7.3 Trimming drift from commands

Every accepted, fresh command carries the phone's UTC minute. After acceptance:

| `|phone − device|` | Action |
|---|---|
| < 2 min | Nothing — 1-minute resolution, advert up to 30 s old |
| 2–5 min | Trim by the difference |
| > 5 min, ≤ 10 min | **Ignore the time field** — not clamped |
| > 10 min | Command already rejected as stale (§6.2) |

- **At most 5 min of trim per day index**, counted by **magnitude**, so alternating
  +5/−5 hints cannot walk the clock. A correctly loaded LFXO drifts ~2 s/day, so this
  is ~150× margin.
- **A trim may never move the day index below the floor.**
- Minutes compare to the nearest match across midnight (difference taken into
  [−720, 720)).

**A clock more than 10 min out cannot be corrected by commands**, because every
command is then stale. Recovery is a power cycle plus a provisioner sync. With the
LFXO trimmed and a command every few days, that should never happen; if it does, it
shows the crystal needs trimming.

## 8. Secrets and provisioning

At manufacture: 256-bit `deviceSecret`, 256-bit `provisionKey`, 32-bit `id`,
recorded against the serial.

**On the device both keys belong in the nRF54L05 KMU**, not in a Kconfig string. This
phase carries them as `CONFIG_MFS_DEVICE_SECRET` / `CONFIG_MFS_PROVISION_KEY` via the
gitignored `credentials.conf` — bench only, and the keys are then readable in the
image.

**The whole scheme rests on the secret staying secret:**

- **Lock the debug port in production.** A readable secret yields every day key for
  that device, forever. Secrets are per device, so it compromises one device, not the
  fleet.
- **The Network Manager holds every secret.** A breach is every device, every day,
  permanently — and there is no over-the-air re-keying, by design. **Keep the
  secrets in an HSM that derives day keys internally and never releases a secret.**
  This is the single most valuable security investment in the system.
- **Store day keys on the phone as non-exportable keys** where the platform allows.
  Android Keystore offers no AES-CCM, so that needs CCM composed from Keystore AES
  primitives — unresolved (§11).

### 8.1 The provisioning key is separate from the secret

| Credential | Confers |
|---|---|
| `deviceSecret` | Every day key — full control of the device |
| `provisionKey` | Setting an invalid clock, and nothing else |

A clock-setting capability is an attack on the scheme: set the date to day N and a
lost day-N key works again. Keeping the keys distinct means a leaked provisioning key
yields denial of service at most, since a key for the new date still needs the
secret. **Provision a key, never a BLE address** — addresses are unauthenticated and
RPAs rotate (`docs/power-budget.md` §8.7.2).

Unique production MCUboot signing keys are a prerequisite, not a later hardening
step: a device that trusts the public NCS default keys accepts anyone's firmware,
which is another route to the secret.

## 9. Adversarial review — 2026-09-13

| # | Attack | Status |
|---|---|---|
| 1 | **Jam the arming**; nobody notices | **Mitigated** by LED A acknowledgement (§6.6). No flash means not armed — send again |
| 2 | Two phones share a key and **reuse nonces** | **Fixed** by slots (§5) and save-before-advertise (§3.1) |
| 3 | Copy a static ID and **lock the engineer out** | **Fixed** by rotating IDs; an ID match counts at most once towards the lockout, and repeated tag guesses against one captured ID burn after 8 tries (§6.4) |
| 4 | Capture, jam, **release a command later** | **Bounded to 10 min** by freshness (§6.2) |
| 5 | Stolen or malware-infected phone | **Bounded to today, assigned devices.** Mode changes need slot 0, so the phone cannot switch on reporting to locate sensors. An armed device accepts only a disarm, so the phone cannot retune it, lengthen its delay or desensitise it without first disarming it — which the Disarmed pattern shows |
| 6 | Compromise the Network Manager | **Out of the device's hands.** HSM (§8) |
| 7 | Physical access: SWD, battery pull | SWD: lock debug, KMU. Battery: disarms (cold start Inactive) and **invalidates the clock** — no stale day revived (§7.1) |
| 8 | Traffic analysis | **Accepted.** 30 s of random-looking UUIDs from one phone reveals the app is in use |

**What held up:** forgery; offline attacks on keys; replay after a later command;
tampering with settings or time; setting the clock back; one lost key compromising
other days.

## 10. Retired on 2026-09-13

| Retired | Why |
|---|---|
| Paper TAN sheets, ten codes per day | Hard to administer; no confidentiality possible — a key derived from a 9-digit TAN falls to offline brute force in seconds |
| Six decimal digits | **Brute-forceable in about a day** by a dongle left beside the device |
| Paper fallback | Dropped outright rather than kept readable — it would be the weakest path, and attackers take the weakest path |
| The `0x00000000` settings-test code | Tuning under encryption needs a fresh nonce per Send anyway; the sequence never runs out, so every Send just uses the next `n`. Also removes the one unauthenticated path |
| `sessionKey` and the outward GATT connection | v1 has no connection at all; the command *is* the session |
| Fixed `'C' 'L'` magic prefix | A readable prefix identifies CLASS traffic and the addressed device. Rotating IDs filter better — a stray UUID matches 1 in 2³² per slot entry rather than 1 in 65,536 |
| External RTC with a supercap | Considered for battery-change continuity; **breaks the BOM budget**. The LFXO is trimmed instead (`docs/power-budget.md` §8.5.3) |

## 11. Open items

| Item | Detail |
|---|---|
| **Trim `&lfxo` by measurement** | 9000 fF is nominal. The clock is a security component (`docs/power-budget.md` §8.5.3) |
| Retain the clock across soft resets | GRTC retention or nPM2100 SCRATCHA, so a brownout needs no provisioner |
| Android Keystore for day keys | No native AES-CCM; compose from Keystore AES or accept app-private storage |
| Report modes | Still blocked on the report payload (design spec §6.5.2). Rotating IDs derived from a slot-0 key are a candidate for the device identifier |
| Serial prefix and device type | `MFS-` provisional |
| Network Manager | Real implementation, HSM, attestation, slot assignment, reinstall policy (§3.1) |
| **DECISION NEEDED: unheard Sends exhaust the window** | Each Send reserves a new `n` whether or not the device hears it; more than 16 unheard Sends to one device in one day lock that slot out until 04:00 UTC. Options (app throttle, app counting with a refusal at 16, wider window) in the bench log, `docs/superpowers/plans/2026-09-13-bench-checklist.md` §9 |
