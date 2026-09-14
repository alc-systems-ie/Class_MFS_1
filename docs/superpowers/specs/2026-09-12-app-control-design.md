# MFS_1 App Control — Design

Recorded 2026-09-12.

Replaces the Thingy:53 toggle tool with a Flutter app that sets MFS_1's parameters
and arms it under a day key. The Thingy:53 tool is **retired** — no co-existence,
one payload format.

Prerequisite reading: `docs/v1-scope.md` (the arm invariant and the ADXL367
traps), `docs/power-budget.md` §8.7 (the tool platform and the time-sync rules),
**`docs/tan-scheme.md` (the day-key access scheme — authoritative for keys, the
wire format's cryptography, acceptance rules and time)**.

> **Amended 2026-09-13 (security).** The hard-coded day codes, the settings-test
> code and the plaintext wire format are replaced by the day-key scheme. Sections
> changed: §1, §2 (items 13–21), §4, §6.1, §6.4–§6.8, §7, §8, §9, §10. Where this
> document and `docs/tan-scheme.md` disagree on anything cryptographic, the scheme
> document wins.

## 1. Scope

Settable parameters, sent as one encrypted command with an arm instruction:

| Parameter | Range | Wire |
|---|---|---|
| Activations before triggering | 1–16 | 1 byte, literal |
| Cooldown between activations | 0–3600 s | 1 byte, geometric |
| Activation threshold sensitivity | 255 levels | 1 byte, geometric |
| Delay before triggering | 0 s – 9 h | 7 bits of plaintext byte 0, piecewise |
| Operating mode | 3 modes | 2 bits of plaintext byte 1 — **slot 0 only** |

Cooldown is meaningful only when activations > 1; the app hides the slider
otherwise, and the device accepts the byte regardless.

**Authority comes from day keys** (`docs/tan-scheme.md`): the Network Manager
derives `dayKey(id, day, slot)` from the device secret, the phone encrypts each
command under it with a fresh sequence number, and the device accepts any of the
next 16. This phase has no Network Manager; the app carries a **bench Network
Manager** holding the bench secret. There are no hard-coded codes and **no test
code**.

## 2. Decisions

**Amended 2026-09-14** — command types: `docs/superpowers/specs/2026-09-14-command-types-amendment.md` supersedes this section where they disagree.

1. **Cooldown is a refractory blanking window.** An activation increments the
   count, then motion is ignored for the cooldown, then the sensor re-arms. The
   count **never expires** — it persists until the device triggers or is
   deactivated, so the device accumulates evidence of tampering over unlimited
   time.
2. **The nPM2100 TIMER runs the cooldown.** GPIO0 was proven reachable on hardware
   (§9.1) but the firmware does not use it: `App::serviceCooldown()` polls
   `TimerIsExpired()` over I²C each loop tick, forced over by an uptime deadline
   (duration + 10% PMIC tolerance + fixed grace) if the PMIC event is missed.
3. **The ADXL367 is held in standby across the blanking window** and reconfigured
   through the full bootstrap on re-arm.
4. **Detection runs in both arm states.** The arm boolean selects the *consumer*,
   not whether detection happens: deactivated, the LED simulates; activated, two
   real GPIOs assert.
5. **Trigger output follows detection** and self-clears after the 5 s ADXL loop
   period. ~~then re-arms~~ — **superseded by decision 23: the device then latches
   Inactive.**
6. **Geometric parameter encodings**, sensitivity byte 255 = most sensitive.
7. **NVS for all persisted state** this phase. nPM2100 SCRATCHA is the intended
   refinement for arm state — see §7.
8. **One Flutter app for the whole device series**, in `../class_app`.

Amended 2026-09-13:

9. **Delay before triggering**, 0 s to 9 h, in bits 1–7 of byte 9 (plaintext byte 0 since the §4 rewrite). Runs on a
   GRTC `k_timer`, never the PMIC timer, whose ±10 % would put a 9-hour delay
   anywhere inside a 108-minute window.
10. **A doubled delay interlock.** A software flag *and* the kernel timer must
    both agree no delay is pending before the output can assert, checked
    independently at the derivation point and again inside `OutputSwitch`.
11. **Three operating modes** in bits 6–7 of byte 4 (plaintext byte 1 bits 4–5 since the §4 rewrite): Trigger only (default),
    Report and trigger, Report only. The latter two are **exceptions** to the
    never-advertise rule, for use only when absolutely necessary.
12. **The SoC stays awake for the whole delay** and shortens its scan period, so
    the deactivate path is as responsive as possible. Battery life is explicitly
    not a factor while a trigger is pending.

Amended 2026-09-13 (security) — full reasoning in `docs/tan-scheme.md`:

13. **Day keys replace day codes.** Per device, per day, per slot. Paper TANs and
    any paper fallback are dropped.
14. **Commands are encrypted and authenticated** with AES-128-CCM, 4-byte tag. A
    listener learns neither the settings nor the arm state.
15. **Rotating IDs replace the `'C' 'L'` prefix.** Bytes 0–3 are unpredictable
    without the day key, so a listener cannot tell which device is addressed and
    garbage cannot count towards a lockout.
16. **Eight key slots.** Slot 0 is the Network Manager's; slots 1–7 are assigned to
    engineers per device per day. **Only slot 0 may change the operating mode.**
17. **A 10-minute freshness window** on the UTC minute each command carries, and the
    same field trims clock drift within tight limits.
18. **Lockout** after 20 authenticated-ID failures: 10 min doubling to 4 h.
    Deactivate does **not** bypass it.
19. **LED A acknowledges accepted commands** (§6.7) so a jammed arm is visible.
    **LED B lights for the 5 s detection period whenever the device detects**, in
    either arm state. This is testbed behaviour; the final product switches the
    output and shows nothing.
20. **No external RTC** — a supercap breaks the BOM budget. The LFXO is trimmed by
    measurement instead.
21. **The clock is invalid on every boot** until an authenticated provisioner sync.
    Nothing resumes from NVS except the day floor.
22. **One path in the armed state.** Armed, the only command that changes state is
    Disarm; an Arm or Settings command to an armed device replays the Armed
    pattern and changes nothing, and a Disarm applies nothing but the disarm — not
    the settings, delay or mode (settings travel only in a Settings command, which
    an armed device cannot apply).
23. **Triggers are one-shot.** Once the output period ends the device latches
    Inactive. Disarm and trigger are the only two ways out of the armed state
    (power loss also ends it, because cold start is Inactive). This supersedes
    decision 5's "then re-arms".

## 3. Spike result — advertising is proven, at a measured cost

**Amended 2026-09-14 — scan reliability:**
`docs/superpowers/specs/2026-09-14-scan-reliability-amendment.md` §1–2 supersedes
the 6 s scan cadence assumed below — the default `CONFIG_MFS_SCAN_PERIOD_MS` is
now **5970 ms** — and adds the measured iPhone advertising interval (~35 ms,
against the Mac's 187.5 ms measured here) and the root cause of the unreliable
commands that measurement explains. Where they disagree, the amendment wins.

Run 2026-09-12. A Flutter app on macOS advertised a 128-bit service UUID; an
nRF54L15 DK running a passive scanner logged it. **119 adverts, every one 16
bytes, AD type 0x07, payload byte-exact:**

```
00 01 02 03 04 05 06 07  08 09 0a 0b 0c 0d 0e 0f
```

**Byte order confirmed.** The UUID string `0F0E0D0C-0B0A-0908-0706-050403020100`
goes on air least-significant byte first, so the on-air sequence is the string
reversed. The app builds the string accordingly.

**`ble_peripheral` 2.4.0 is the package.** iOS, Android, macOS and Windows; its
Darwin implementation maps the strings to `CBUUID` and passes them as
`CBAdvertisementDataServiceUUIDsKey`. No platform channel and no native code
needed. Its manufacturer-data branch is commented out on Darwin, independently
corroborating `docs/power-budget.md` §8.7.4 — Apple genuinely ignores that key.

**The measured advertising interval is ~187 ms**, not the 20–50 ms
`CLAUDE.md` asserts the counterpart must use. Deltas were 186, 190, 185, 189, 190,
185, 189 ms. **A phone's advertising interval is not ours to set**, so that
assertion is wrong and must be corrected.

With a 100 ms window against a 187 ms advertiser, each device wake detects with
probability 100/187 ≈ 53 %:

| Device wakes | Elapsed @ 6 s | Cumulative detection |
|---|---|---|
| 1 | 0 s | 53 % |
| 3 | 12 s | 90 % |
| 5 | 24 s | 98 % |
| 6 | 30 s | 99 % |

**The app therefore advertises for 30 s per Send.** Widening the scan window or
shortening the scan period would both cost battery; advertising duration is free.

Caveat: measured on macOS, not iOS. Same CoreBluetooth API, but **re-measure on
the iPhone** before treating 187 ms as final.

## 4. Wire format

**Amended 2026-09-14** — command types: `docs/superpowers/specs/2026-09-14-command-types-amendment.md` supersedes this section where they disagree.


**Superseded 2026-09-13.** The plaintext layout with a `'C' 'L'` prefix, a device
type and a day-code field is gone. The authoritative definition, with the
derivations and a worked vector, is `docs/tan-scheme.md` §3 and §6.1. Summary:

One 128-bit service UUID, 16 bytes, on-air order:

| Bytes | Content |
|---|---|
| 0–3 | rotating ID — `HMAC(dayKey, n ‖ 0x04)[0..3]` |
| 4–11 | AES-128-CCM ciphertext of the 8-byte plaintext below |
| 12–15 | CCM tag, 4 bytes |

The protocol version (`0x02`) is never on air; both sides supply it as associated
data. The time-sync payload shares the same UUID slot (`docs/tan-scheme.md` §7.2) and
is listened for only while the clock is invalid.

Plaintext:

| Byte | Bits | Field | Notes |
|---|---|---|---|
| 0 | 0 | desired arm state | `0` Inactive, `1` Active |
| 0 | 1–7 | **delay before triggering** | device-level, all variants — §5.1 |
| 1 | 0–3 | activations − 1 | 1–16, so every value is valid |
| 1 | 4–5 | **operating mode** | device-level; `3` rejects; **slot 0 only** — §4.3 |
| 1 | 6–7 | reserved | ignored |
| 2 | — | cooldown | geometric byte, `0` = none |
| 3 | — | sensitivity | geometric byte, 255 = most sensitive |
| 4–5 | 0–10 | UTC minute of day, LE | 0–1439; ≥ 1440 rejects. Freshness and trim |
| 4–5 | 11–15 | reserved | ignored |
| 6–7 | — | **per-variant extension** | **MFS_1 MUST ignore, never validate** |

**Bytes 6–7 belong to other MFS variants.** MFS_1 must not require them to be zero:
doing so would make it reject commands from a future app build the moment another
variant starts using that space. The same holds for the reserved bits. Per-variant
space shrinks from three bytes to two plus spare bits — the cost of the tag and the
minute field.

### 4.1 The command is an absolute state assertion

It says "be in this state with these settings", never "change". Two consequences:

- **Repeats are free.** The phone advertises each command ~160 times in 30 s. Once
  the first copy is accepted its rotating ID leaves the window, so every later copy
  is simply *not for us* — silent and not counted. No payload-identity dedupe is
  needed on the device.
- **The swallowed-second-press bug cannot occur.** The 12 s command cooldown in
  `command_scanner.cpp` existed because the old payload was a toggle; it is removed.

### 4.2 Settings and arm state always travel together

**Amended 2026-09-14** — command types: `docs/superpowers/specs/2026-09-14-command-types-amendment.md` supersedes this section where they disagree.


Every Send carries the live slider values. While Inactive, the engineer tunes with
ordinary commands whose arm bit is clear; each uses the next sequence number, which
costs nothing because the sequence never runs out. To arm, the app sends the final
settings with the arm bit set. **There is no window in which the device is armed with
settings the engineer did not watch being tested**, and there is no unauthenticated
tuning path.

### 4.3 Operating mode — bits 4–5 of plaintext byte 1

| Value | Mode | Behaviour |
|---|---|---|
| `00` | **Trigger only** | **Default.** Fires the output. Emits nothing. |
| `01` | Report and trigger | Broadcasts a trigger message immediately before firing |
| `10` | Report only | Broadcasts, does not fire |
| `11` | reserved | reject the command |

**Only a slot-0 command changes the mode.** From slots 1–7 the field is decoded (a
reserved value still rejects) but ignored, and the stored mode kept. The Network
Manager builds slot-0 commands; the engineer's phone only carries them. A stolen
phone therefore cannot switch reporting on and use it to locate the sensors.

**Report modes are exceptions to a standing rule and must be documented as such.**
`CLAUDE.md` states that the device never advertises, and that rule stands as the
general case. Report and Report-and-trigger are **optional exceptions, to be used
only when deemed absolutely necessary**, because advertising forfeits covertness.
The doctrine is therefore reworded rather than contradicted: *the device never
advertises to solicit contact, and may emit a bounded burst on trigger only when
explicitly configured to.*

Reporting is connectionless — a bounded advertising burst to the hub, beacon-style,
with no counter. It is the Drawer Master behaviour reduced to a beacon.

**Never advertise unbounded** (`docs/power-budget.md` §8.7.1). Power is not the
constraint here: a burst of a few seconds costs a fraction of one 6 s scan wake and
triggers are rare. Covertness is the constraint.

**Refused until Task 18 (final fix wave, 2026-09-13).** The firmware does not yet
implement reporting. `applyCommand()` refuses any slot-0 command asking for Report
or Report-and-trigger — it applies the rest of the command with the mode change
disallowed, logs a warning, and the device stays Trigger only — and
`Settings::Load()` coerces a previously stored non-default mode back to Trigger
only on boot. The table above is the target behaviour once Task 18 lands; until
then only `00` (Trigger only) is ever actually honoured.

## 5. Parameter encodings

Defined by formula in one shared header that generates the Dart constants, so the
two sides cannot drift.

**Sensitivity** — `THRESH_ACT`, 13-bit, 0.25 mg/LSB on the ±2 g range:

```
threshold_lsb(s) = round(4000 * 0.01^(s/255))     # 10 mg .. 1000 mg
```

1.82 % per step. Byte 0 = 4000 LSB (1000 mg); byte 255 = 40 LSB (10 mg). The
present default of 300 LSB (75 mg) sits at **byte 143**, comfortably mid-scale;
`alc_drawer_master`'s 150 LSB (37.5 mg) at ≈185.

Linear was rejected: 32 LSB (8 mg) per step gives barely a dozen usable steps
across the 10–100 mg region where the interesting behaviour lives.

**Cooldown** — seconds:

```
cooldown_secs(c) = c == 0 ? 0 : round(3600^((c-1)/254))    # 1 s .. 3600 s
```

3.28 % per step. Byte 128 = **exactly 60 s**. Linear was rejected: 14.1 s per step
cannot express a 5 s cooldown at all.

The nPM2100 TIMER spans 16 ms to 3 days, so 1–3600 s fits with enormous margin.

### 5.1 Delay before triggering — bits 1–7 of plaintext byte 0

Seven bits, 0–127, piecewise and fully contiguous. Default 0 — no delay, which is
the present behaviour.

| Code | Meaning | Range |
|---|---|---|
| 0–59 | `V` seconds | 0 s … 59 s |
| 60–118 | `V − 59` minutes | 1 min … 59 min |
| 119–127 | `V − 118` hours | 1 h … 9 h |

Contiguous at both seams: 59 → 59 s, 60 → 60 s; 118 → 3540 s, 119 → 3600 s. No
value means two things and there are no gaps. Maximum delay 9 hours (32400 s),
which fits `uint16_t` seconds.

Generated into the shared table alongside the other two encodings, so the app and
firmware cannot disagree.

#### 5.1.1 The delay runs on the GRTC, not the PMIC timer

| Timer | Error on a 9-hour delay |
|---|---|
| nPM2100 TIMER (±10 % over −10…60 °C) | **±54 minutes** |
| Zephyr `k_timer` on the GRTC/LFXO (~50 ppm) | **±1.6 seconds** |

A delay that lands anywhere inside a 108-minute window is not a delay. The
cooldown stays on the PMIC timer, where ±10 % of a blanking window is irrelevant;
the delay uses a `k_timer`. This is also forced by the datasheet's *"TIMER only
runs one configuration at a time"* — sharing the block would stop a cooldown and a
delay ever coexisting.

#### 5.1.2 The SoC stays awake for the whole delay

**Battery life is explicitly not a factor during a pending trigger.** The device
holds a PM policy lock so it cannot enter a deeper state, and **shortens its scan
period** for the duration.

The scan change is the safety-relevant part. At the normal 6 s cadence a deactivate
payload takes ~30 s to be heard with confidence (§3), and during a pending trigger
that abort path is the most important thing the device does. Shortening the scan
collapses that latency at a power cost the operator has already accepted.

## 6. Firmware design

### 6.1 Units

| Unit | Responsibility | Host-tested |
|---|---|---|
| `mfs_protocol` | plaintext layout, `Command`, encode/decode, the generated tables | yes |
| `crypto` | the one seam to a crypto library: PSA on target, OpenSSL on the host | via vectors |
| `access_keys` | day key, encryption key, rotating ID, nonce, seal/open, time-sync tag | yes, against Python vectors |
| `DeviceClock` | UTC from uptime, validity, day index, floor, sync rules, trim rules | yes |
| `AccessControl` | slots, windows, rotating-ID table, lockout, freshness, persist-before-act | yes |
| `LedSequencer` | LED A acknowledgement patterns, time-based | yes |
| `Settings` | the parameters, NVS-backed, mode gated on slot 0 | yes |
| `CommandScanner` | passive scan, AD 0x07, queues raw 16-byte UUIDs to the main loop | no |
| `OutputSwitch` | two GPIOs, the only real consumer of `IsOutputActive()` | no |
| `App` | the state machine; the ONLY caller of `AccessControl` and `DeviceClock` | no |

`CommandScanner` changes from parsing `BT_DATA_MANUFACTURER_DATA` to AD type 0x07,
and **no longer validates anything**: it cannot, without keys. It queues each
distinct 16-byte UUID for the main loop, which owns every decision. The Thingy:53
tool is retired, so no dual parsing.

### 6.2 The arm invariant, restated

> **Further amended 2026-09-14 (bench session 1) — arming sequence and
> fire-pin isolation:**
> `docs/superpowers/specs/2026-09-14-arming-sequence-amendment.md` supersedes
> the snippet below and the pin model it implies. The state lives in
> `m_arming.State()` (`ArmingSequence`), not a bare `m_arm_state` member, and
> has a third value, `ArmState::Arming`, between Inactive and Active. The fire
> pins are `GPIO_DISCONNECTED` at all times **except while the device is
> `Active`**: they become outputs (driven low, read back) as the last step of
> arming, immediately before `Active`, and are disabled first on every disarm,
> trigger latch or failure. Outside `Active` `OutputSwitch` has no driver to
> move, whatever `m_output_switch.Set()` is asked for.

`docs/v1-scope.md` §1.0 stands, with one change: **LED B is no longer the output
mirror.** It becomes a tuning indicator. `OutputSwitch` becomes the worked example
that future consumers copy.

```cpp
// App::updateOutputState() — still the ONLY place the two are combined.
m_output_active = (m_arm_state == ArmState::Active) && m_detection_met;
```

`m_detection_met` is a **latched flag owned by the detection engine** (§6.3), not
something `updateOutputState()` derives. It must not be recomputed here: the
engine zeroes `m_activation_count` at the moment it latches, so deriving
`m_detection_met` from the count would take the output false again immediately.

```cpp
ledB = m_detection_met;                 // tuning indicator, bench only
m_output_switch.Set(IsOutputActive());  // THE consumer — two GPIOs
```

The rule for anything added later is unchanged: call `IsOutputActive()`, never
read INT1, the AWAKE bit or `Adxl367::ReadAwake()` directly, and never re-derive
the condition at the consumer.

### 6.3 Detection engine

> **Amended 2026-09-14 — disarmed test mode:**
> `docs/superpowers/specs/2026-09-14-disarmed-test-mode-amendment.md` supersedes
> this section where they disagree.

Runs in **both** arm states. The arm boolean selects the consumer, nothing else.

The ADXL367 is no longer held in standby while Inactive. Cold start boots
Inactive, **testing** at the stored settings from the first tick — the engine
is not waiting for a Tune command to start it. Every restart — **a disarm, a
Settings command or an arm restarts** the test from zero, along with a cooldown
re-arm — reconfigures the part afresh through the loop-mode bootstrap and
clears the detection latch; a disarm or Settings command also clears the
activation count and any pending cooldown or delay, so nothing counted or
latched in one session can reach the output — or a later armed session — in
the next. The safety guarantees for arming mid-test specifically are the
amendment's section 3.2. (Amended 2026-09-13, revised 2026-09-14.)

```
AWAKE rising edge
  -> m_activation_count++
  -> if m_activation_count >= N:
        m_detection_met = true            // latched; output follows this
        m_activation_count = 0
        // NO blanking here - the trigger's own AWAKE must run to completion
  -> else if N > 1 and cooldown > 0:
        Standby(); PMIC TimerSetDurationMs(cooldown); TimerStart()
        poll each loop tick: TimerIsExpired() OR uptime >= deadline (timer duration
          + 10% PMIC tolerance + fixed grace, in case the PMIC event is missed)
        -> TimerClearExpiredEvent(); enableAccelerometer(), retried every 1 s on failure

AWAKE de-asserts (5 s ADXL loop period)  ->  m_detection_met = false
```

Two rules the pseudocode encodes deliberately:

- **Blanking applies only between counted activations, never after the triggering
  one.** Standing the ADXL down at the moment of trigger would cut short the 5 s
  assertion that §2.5 defines as the output's duration.
- **Cooldown is meaningful only when N > 1.** With N = 1 every activation triggers,
  so there is nothing to blank between; the device accepts the byte and ignores it.

The count clears only on trigger or on a restart (disarm, Settings, or arm). It
does **not** expire on its own.

### 6.4 The command path — one path while armed

**Amended 2026-09-14** — command types: `docs/superpowers/specs/2026-09-14-command-types-amendment.md` supersedes this section where they disagree.

**Further amended 2026-09-14 (bench session 1)** — arming sequence and
fire-pin isolation: `docs/superpowers/specs/2026-09-14-arming-sequence-amendment.md`
supersedes the arm and disarm orders below. Arm no longer sets the boolean at
step 5 below; it starts a 10 s exit delay (state `Arming`) with the fire pins
still isolated, and only then, synchronously, restarts detection armed, enables
the fire pins and sets `Active`. Disarm no longer moves the boolean first: it
disables the fire pins **first**, then Inactive, then re-derives the output,
then restarts the test.


**What an accepted command may do is decided in exactly one place**, the pure
function `DecideCommand()` (`src/arm_policy.hpp`), and `App::applyCommand()` acts on
that decision and on nothing else.

| Device is | Command | Action | Settings | Mode | Clock trim |
|---|---|---|---|---|---|
| **Active** | arm bit set | **Ignore — nothing at all** | no | no | no |
| **Active** | arm bit clear | **Disarm only** | **no** | **no** | yes |
| Inactive | arm bit set | Arm | yes | slot 0 only | yes |
| Inactive | arm bit clear | Tune | yes | slot 0 only | yes |

**Armed, there is one path: disarm.** Re-arming with new settings, retuning, a mode
change, even a Network Manager command — all ignored, silently, with an RTT warning
and no LED. To change anything, the engineer disarms (slow flash), then sends the new
settings (single blink), then arms. **A disarm applies nothing but the disarm**; the
settings it carries are discarded.

**Only two ways out of the armed state: disarm, or trigger.** A trigger is one-shot:
when the output period ends, `updateOutputState()` flags the trigger complete and
the main loop latches the device Inactive. (It is flagged rather than acted on in
place because the disarm path — `App::disarmDevice()` → `ArmingSequence::Disarm()`
— re-enters `updateOutputState()`.) Power loss also ends
the armed state, because cold start is Inactive.

**Arming — order is load-bearing.** Running detection while deactivated re-opens the
stale-level hole that commit `0a50910` closed: at the moment the engineer activates
the device they are handling it, so AWAKE is high and the count may already be
non-zero. The teardown-and-rebuild is what preserves §1.0.1.

1. `AccessControl` authenticates, checks freshness and **persists the sequence
   number** (`docs/tan-scheme.md` §6.2). Nothing below runs otherwise.
2. `DecideCommand()` returns Arm; apply and persist the settings (mode only from slot 0)
3. `Standby()` → `ConfigureLoopMode(new threshold)` → **confirm AWAKE == 0**
4. zero the count, clear `m_detection_met`
5. arm boolean → Active, then `updateOutputState()`
6. play the LED A pattern for the outcome (§6.7)

**The sequence number is consumed first, deliberately** — for ignored commands too.
The old ordering consumed the code last so a failed configure could not burn it.
With an unbounded sequence that protection is worthless and the replay protection is
not. A failed arming (superseded by the arming sequence amendment: the failure
comes at the end of the 10 s exit delay, not at acceptance) shows **no LED A
acknowledgement**; it raises the warning, which until the dedicated light is chosen
is three long pulses on **LED B** (amendment §4). The device stays Inactive; the
engineer may press Send again, which uses the next number, but a repeat means the
device is faulty.

**Disarming is not a mirror any more** (superseded by the arming sequence amendment
§2): disable the fire pins **first** (drive low, disconnect), then Inactive and
cancel any arming or delay, then re-derive the output, then restart the disarmed
test from zero.

### 6.5 There is no test code

**Amended 2026-09-14** — command types: `docs/superpowers/specs/2026-09-14-command-types-amendment.md` supersedes this section where they disagree.

Retired 2026-09-13. Tuning uses ordinary authenticated commands with an explicit
Settings type, not an arm bit (§4.2). The property the test code existed to
guarantee — tuning can never arm the device — now holds trivially: every command
is authenticated, and a command arms the device only if its type is Arm.

### 6.5.1 THE DELAY INTERLOCK — safety critical

> **Further amended 2026-09-14 (bench session 1) — arming sequence and
> fire-pin isolation:**
> `docs/superpowers/specs/2026-09-14-arming-sequence-amendment.md` supersedes
> this section's disarm order and state names. There is no `setArmState()`: the
> arm state lives in `ArmingSequence` (`Inactive`, `Arming`, `Active`), every
> disarm runs its §2 order (fire pins disabled first), and the delay code here
> now lives in `DetectionEngine`. The two-witness interlock itself is unchanged.

> **Amended 2026-09-14 — disarmed test mode:**
> `docs/superpowers/specs/2026-09-14-disarmed-test-mode-amendment.md` supersedes
> this section where they disagree. The delay now runs identically while
> disarmed, on the same `k_timer` — it is not an armed-only mechanism. What
> changes is only the consumer: a disarmed delay logs as a test
> (`TEST trigger pending: LED B in N s.`), never `TRIGGER PENDING`, and its
> expiry lights LED B rather than asserting the fire GPIOs, because the
> interlock below only ever gates `updateOutputState()`'s output term, which is
> already zero while disarmed.

**A trigger that fires after the engineer has deactivated the device is the worst
failure this product has.** With a delay of up to nine hours between the
activation and the firing, that window is now enormous, so the guard is explicit
and doubled rather than implied by the arm boolean alone.

**Two independent conditions must both agree that no delay is pending**, and they
are deliberately derived from different things — a software flag and the kernel
timer itself — so that one being wrong cannot fire the device:

```cpp
// App - both must say "no delay running" before firing is permitted.
bool App::delayPermitsFiring() const
{
  return !m_delay_pending && k_timer_remaining_ticks(&m_delay_timer) == 0;
}
```

A flag left set with a dead timer blocks firing; a running timer with a cleared
flag also blocks firing. **Both failure directions are safe**, which is the point
of choosing these two particular witnesses.

**Expiry is driven by a recorded deadline, not a live poll of the timer alone.**
`k_timer_remaining_ticks()` reads 0 both when a timer has genuinely expired and
when it was never armed, so `updateOutputState()` also tracks `m_delay_deadline_ms`
— the uptime at which the delay is considered genuinely over. A flag left set with
a dead timer therefore now waits for that recorded deadline rather than blocking
firing forever: once `k_uptime_get() >= m_delay_deadline_ms`, the expiry commit
clears the flag (`cancelDelay()`), releases the PM lock, and lets detection
proceed.

**Owner rule, 2026-09-14: always fail safe — a trigger never fires if the scanner
was lost during the delay.** This supersedes the 2026-09-13 ruling that prioritised
the alarm over a missed disarm. A device that cannot scan cannot hear a disarm, so
`App::Run()` fails safe the moment the scanner is not running while Arming or Active
(`ArmingSequence::ServiceScannerHealth()`) — pins isolated first, Inactive, the
pending trigger cancelled, warning raised, logged
`Scanner not running while arming or armed - disarmed (fail safe)!` (arming sequence
amendment §4.2). Behind that, the detection engine still tracks whether the scanner
was running for the whole armed delay (`m_delay_scan_lost`) and checks it again at
expiry, after restoring duty-cycled scanning; if it was lost or is down, the expiry
commit does **not** fire — no detection, no hold — and reports
`Trigger suppressed: the scanner was not running during the delay (fail safe)!`. A
disarmed test delay is unaffected: it drives no fire pins.

**`OutputSwitch` does not learn about delays.** Coupling it to one feature would
destroy its value as a general containment. Instead it gains an **interlock** it
consults immediately before driving the gates:

```cpp
// output_switch.hpp
using InterlockFn = bool (*)(void* context);

/** Refuses to assert unless the interlock returns true. */
void SetInterlock(InterlockFn interlock, void* context);
```

App installs `delayPermitsFiring()` as that interlock at startup. The result is
**two independent layers**, not the same test written twice:

| Layer | Where | What it stops |
|---|---|---|
| 1 | `updateOutputState()` folds the delay into `m_output_active` | the condition ever becoming true |
| 2 | `OutputSwitch::Set()` re-checks via the interlock | a caller asserting anyway |

Layer 2 fires a `LOG_ERR` and latches faulty if it ever refuses, because reaching
it means layer 1 has already failed and that is a bug, not a routine condition.

**Deactivating cancels a pending delay unconditionally.** The disarm path
(`App::disarmDevice()` → `ArmingSequence::Disarm()`, after disabling the fire pins
first) re-derives the output and restarts the engine, which stops the timer, clears
the flag and zeroes the activation count. A delay does not survive disarming, and it does not survive a reset either
— the pending state is deliberately **not** persisted, so a reboot loses the
trigger, which is the fail-safe direction.

**Activations during the delay are ignored.** The trigger is already committed;
re-counting would let a continuing disturbance postpone or duplicate it.

### 6.5.2 Operating modes

`TriggerOnly` is the default and the current behaviour. The other two are
exceptions to the never-advertise rule (§4.3) and are gated behind the mode field:

```
count reaches N
  -> delay (if configured) with the interlock armed
  -> re-check the interlock
  -> mode == ReportOnly or ReportAndTrigger: emit the bounded report burst
  -> mode == TriggerOnly or ReportAndTrigger: OutputSwitch::Set(true)
```

The report goes out **immediately before firing**, after the delay, not when the
count completes. In `ReportOnly` the output is never asserted at all.

**OPEN — the report payload is not yet specified.** It needs a device identifier
so the hub knows which sensor fired, and a stable identifier in a repeated
broadcast is a tracking beacon for anyone listening. That is a covertness decision,
not a formatting one, and it is deferred rather than guessed. Report modes cannot
ship until it is answered.

**Refused, not merely unspecified, as of the final fix wave (2026-09-13).** Until
the report payload is specified and Task 18 implements it, the firmware refuses
the mode field itself at the point of application (§4.3): a Network Manager
command asking for `ReportOnly` or `ReportAndTrigger` is applied with the mode
change disallowed, and the device stays `TriggerOnly`. The sequence above is the
target behaviour; the device does not yet reach `ReportOnly` or `ReportAndTrigger`
at all.

### 6.6 Failures are silent

Per `docs/tan-scheme.md` §6.6: a command that is not for us, fails authentication,
is stale, malformed or arrives during a lockout produces **no radio emission and no
LED whatsoever**. The device logs over RTT and does nothing else.

### 6.7 LED scheme — PROVISIONAL

**Amended 2026-09-14** — command types: `docs/superpowers/specs/2026-09-14-command-types-amendment.md` supersedes this section where they disagree.

**Further amended 2026-09-14 (bench session 1)** — arming sequence and
fire-pin isolation: `docs/superpowers/specs/2026-09-14-arming-sequence-amendment.md`
supersedes the **Armed** row's timing. LED A shows nothing for the 10 s exit
delay after an Arm is accepted (LED B is also suppressed for that window); the
rapid flash below plays only once the fire pins are enabled and the device is
actually `Active`, not at the moment the command was accepted.


The final hardware has **three LEDs visible through a light window**, integral to
the design. This scheme is provisional and is what v1 implements.

**LED A acknowledges accepted commands.** A pattern plays only after an
authenticated command has been accepted, and only once the new state is real — for
arming, after the boolean is set. A new command arriving during a pattern replaces
it.

| Result | LED A pattern | Length |
|---|---|---|
| **Armed** | rapid flash ~8 Hz (60 ms on / 65 ms off) | 3 s |
| *(armed, Arm or Settings received)* | rapid flash — replays Armed; nothing changes | 3 s |
| **Disarmed** | slow flash 1 Hz (500 / 500) | 3 s |
| *(Inactive, Disarm received)* | slow flash — the ordinary disarm | 3 s |
| **Disarmed, pending delay cancelled** | double blink each second (100 on / 100 off / 100 on / 700 off) | 3 s |
| **Settings applied while Inactive** | one 200 ms blink | 0.2 s |
| **Mode changed** (slot 0) | two 200 ms blinks | 0.6 s |

Arm-state results take precedence; Mode Changed plays only when the arm result is
Settings Applied.

- **"Delay cancelled" tells the engineer a trigger really was pending** when they
  disarmed.
- **An arming failure shows nothing on LED A** (arming sequence amendment §3, §4). The
  old "Arm refused" three long pulses on LED A are gone. A failure at the end of the
  exit delay raises the **warning** instead — until the dedicated warning light is
  chosen, **three long pulses (700 on / 300 off, 3 s) on LED B**, in every build —
  so a hardware fault still does not look like a jammed command. It is not a breach
  of silence on failure: that rule covers failed authentication, and an arming
  failure follows an accepted command from a valid key holder.
- **No flash means the command did not take effect.** Every accepted command plays a
  pattern — including a replay that changes nothing while `Active` — with three
  exceptions: an **Arm**, which shows nothing for the 10 s exit delay and then the
  rapid flash only if arming completes; an **Arm or Settings received while
  `Arming`**, which is ignored with no LED; and an **arming failure**, which shows
  LED B's warning instead. So a dark LED A means authentication or freshness failed,
  the command arrived during an exit delay, or — if LED B showed three long pulses —
  the device could not arm. Send again only in the first two cases; a repeated
  warning means the device is faulty.

A 10 ms `k_timer` renders the pattern while one is active, because the 100 ms main
loop cannot draw a 60 ms phase. The patterns total a few seconds per command, so they
cost nothing against the power budget. Between patterns, a bench build
(`CONFIG_MFS_DEBUG_LED`) keeps LED A lit while Inactive as before.

**LED B lights for the 5 s detection period on every detection**, armed or not
(`ledB = m_detection_met`; suppressed while `Arming`, and overridden while the
interim warning plays — arming sequence amendment §4). Inactive, that simulates triggers during tuning; Active,
it confirms a real trigger. **Testbed behaviour**: an LED that lights when an
intruder disturbs the area reveals the sensor, and the final firmware switches the
output and shows nothing.

**LED 3** is unassigned.

### 6.8 Time

`DeviceClock` implements `docs/tan-scheme.md` §7 exactly: invalid on every boot;
valid only after a provisioner sync that passes the floor and 400-day bounds; syncs
refused once valid; trimmed from accepted commands by 2–5 minute steps within a 5
minute daily magnitude budget, never below the floor. While the clock is invalid
the main loop offers each queued UUID to the time-sync check and to nothing else.

## 7. Persistence

**NVS in the nRF54L05's RRAM**, through the Zephyr settings subsystem:

| Key | Content | Written |
|---|---|---|
| `params/v1` | activations, cooldown, sensitivity, delay, mode — 5 bytes | on change only |
| `access/v1` | day index + `next[8]` — 34 bytes | on day rollover and each accepted command |

RRAM is rated 10,000 write/rewrite cycles per 128-bit word line with 10 years
retention at 85 °C. **Never write state to a fixed RRAM address, and write only on
change**, never per scan or per boot — NVS wear-levels across its partition, which
is what makes a record rewritten per command acceptable.

Arm state is **not** persisted: cold start is Inactive, and a reset invalidates the
clock anyway.

### 7.1 Deferred: nPM2100 SCRATCHA for arm state

Recorded because it answers `docs/v1-scope.md` §6 better than any option listed
there, and should be adopted once this phase settles.

| Store | Size | Cleared by |
|---|---|---|
| SCRATCHA (`WRITE` 0xD7 + `STROBE` 0xD8, read `READ` 0xD9) | 8 bits | VBAT below `VBATPOR_FALLING` — battery removal only |
| Sticky (`WRITESTICKY` 0xDB + `STROBESTICKY` 0xDC, read `READSTICKY` 0xDD) | 3 bits | VBAT disconnect only |
| SCRATCHB (0xDA) | 8 bits | VINT below `VINTBOR`, **or any COLD START** |

§6 notes that cold-start-Inactive is fail-safe but **a brown-out silently
disarms** the device, and that persisting to NVS has the wrong failure mode
because it also survives a battery change, so a serviced device comes up armed.

**SCRATCHA has exactly the semantics §6 wanted:**

- brownout / watchdog / SoC reset → VBAT holds → SCRATCHA survives → **stays armed**
- battery removal → SCRATCHA clears → **comes up Inactive**, fail-safe on service

It also has zero wear and unlimited writes.

It cannot hold the access state (34 bytes). Since 2026-09-13 it has a second
candidate use: retaining the clock offset across a soft reset, so a brownout does
not need a provisioner visit (`docs/tan-scheme.md` §11). Eight bits cannot hold an
offset directly; that design is open.

## 8. App design

**Amended 2026-09-14 — Send/Stop:**
`docs/superpowers/specs/2026-09-14-scan-reliability-amendment.md` §4 supersedes
the Send-only flow below — every sending page (Arm, Settings, provisioner) also
has a red **Stop**, and the "Send Disarm (replaces advert)" pre-empt described in
§8.2 below is **removed**: cancelling an in-flight Arm is now Stop, set
Disarmed, Send. Where they disagree, the amendment wins.

`../class_app` — one Flutter app for the device series.

```
lib/protocol/tables.dart          generated encodings           <- tools/gen_protocol_tables.py
lib/protocol/mfs_protocol.dart    plaintext layout, UUID string <- mirrors src/mfs_protocol.hpp
lib/protocol/access_keys.dart     derivations, CCM seal         <- mirrors src/access_keys.cpp
lib/services/advertiser.dart      wraps ble_peripheral, 30 s window
lib/services/sequence_store.dart  next n per (device, day, slot), saved BEFORE advertising
lib/services/key_source.dart      KeySource interface + BenchNetworkManager (bench only)
lib/devices/mfs1/                 MFS_1 screen, settings model, command builder
lib/devices/provisioner/          time-sync screen (bench provisioner)
lib/main.dart                     device picker -> device screen
test/access_vectors.dart          generated                     <- tools/gen_access_vectors.py
```

Dependencies: `ble_peripheral` 2.4.0, `pointycastle` (AES-CCM — proven against the
vectors at 4.0.0), `crypto` (HMAC-SHA256), `shared_preferences` (sequence store).

Flutter rather than native because `docs/power-budget.md` §8.7.4 already chose
**Android** as the production platform. One codebase serves the iOS test tool now
and the Android production tool later, collapsing what was going to be two apps.

### 8.0 Keys in this phase

A `KeySource` gives the app today's day key and slot for a device. The only
implementation this phase is `BenchNetworkManager`, which holds the bench device
secret and derives keys locally — standing in for the real Network Manager so the
rest of the app is written against the production interface. It also builds slot-0
commands for the mode setting. **It is bench-only by construction**: the production
app must never hold a device secret.

### 8.1 MFS_1 screen

**Amended 2026-09-14** — command types: `docs/superpowers/specs/2026-09-14-command-types-amendment.md` supersedes this section where they disagree.


Device picker (one bench device this phase); arm toggle; a 1–16 selector for
activations; a cooldown slider shown only when activations > 1, reading out mapped
seconds; a delay slider; a sensitivity slider reading out mg, insensitive left to
sensitive right; one Send button.

**Every Send is the same operation**: build the plaintext from the toggle and sliders
with the current UTC minute, take the next `n` for the slot, **save `n + 1`**, seal,
advertise for 30 s. No prompt, no code to pick, no used-code list.

The operating mode lives in a separate **Network Manager (bench)** section, visibly
distinct, because it sends a slot-0 command. In production that section is replaced
by a request to the real Network Manager.

### 8.2 Workflow

**Amended 2026-09-14** — command types: `docs/superpowers/specs/2026-09-14-command-types-amendment.md` supersedes this section where they disagree.


1. Engineer picks the device.
2. Toggle to Deactivated; Send. Watch LED A: **slow flash** — disarmed (or **double
   blink** — a pending trigger was cancelled). No flash — send again.
3. Adjust settings; Send. **Single blink** — applied.
4. Observe LED B simulating triggers at the chosen sensitivity.
5. Toggle to Armed; Send. Leave the area: nothing shows for the 10 s exit delay
   (arming sequence amendment §3). **Rapid flash** on LED A — armed with exactly the
   settings just watched. **Three long pulses on LED B** and no LED A flash — the
   device could not arm (a fault) and stays disarmed; if it repeats, the device is
   faulty. To cancel during the exit delay: press **Stop**, toggle to Disarmed,
   and press **Send**
   (`docs/superpowers/specs/2026-09-14-scan-reliability-amendment.md` §4 — the
   previous "Disarm replaces the advert" pre-empt is removed).

### 8.3 What the app must be honest about

The device is radio-silent by design, so:

- **Send is never confirmed over the air.** The UI shows "Advertising… 30 s" with a
  countdown and directs the engineer to LED A, which is the only feedback channel.
- **Never resend a sequence number.** A retry is a new Send with a new `n`.
- **No local name in the advertisement.** A covert device's counterpart must not
  broadcast a string.

### 8.3.1 Provisioner screen (bench)

Builds a time-sync payload from the phone's clock with the bench provisioning key and
advertises it for 30 s. Used after every device reset (`docs/tan-scheme.md` §7.1).
The phone's clock must be network-synced; a phone that is minutes out will make every
subsequent command stale.

### 8.4 iOS constraints

- **Foreground only.** Backgrounded, iOS moves service UUIDs to an overflow area
  that only Apple devices can read; an nRF54L05 scanner sees nothing.
- **The advertising interval is not controllable.** Hence the 30 s Send.

## 9. Verification required before this can work

1. ~~**PMIC GPIO0 has never been driven on this project.**~~ **RESOLVED 2026-09-12
   on hardware — see §9.1.**
2. **Re-measure the advertising interval on the iPhone.** 187 ms is a macOS figure.
2a. **Specify the report payload** (§6.5.2). Needs a device identifier that does
   not become a tracking beacon. **Report modes cannot ship until this is
   answered**; Trigger-only is unaffected.
3. ~~**Confirm the two `OutputSwitch` GPIOs.**~~ **RESOLVED 2026-09-12 — see §9.2.**
4. ~~**AES-CCM with a 4-byte tag on Dart.**~~ **RESOLVED 2026-09-13** — `pointycastle`
   4.0.0 reproduces every vector from `tools/gen_access_vectors.py` byte for byte.
5. ~~**AES-CCM with a 4-byte tag on the nRF54L05 (PSA / CRACEN).**~~ **RESOLVED
   2026-09-14 on hardware** (J-Link 853003346): `Crypto self-test passed: 3 command
   vectors and the time-sync vector match.` The first attempt failed with
   `HMAC key import failed: -141` (`PSA_ERROR_INSUFFICIENT_MEMORY`): PSA allocates
   imported key buffers from the mbedtls heap, which an observer-only build does not
   get implicitly. Fixed with `CONFIG_MBEDTLS_ENABLE_HEAP=y` in `prj.conf`.
6. **Trim `&lfxo` by measurement.** The clock is now a security component with a
   10-minute freshness window riding on it.
7. **ADXL367 reset while armed is undetected** — a brown-out or spontaneous reset
   of the ADXL367 leaves INT1 quiet with no signal, so the device is deaf and
   nothing reports it. Candidate: periodic readback of POWER_CTL/INTMAP1 while
   armed, reconfigure on mismatch.

### 9.1 PMIC GPIO0 / TIMER — proven on hardware, 2026-09-12

Probed on the bespoke MFS_1 board (J-Link 853003346). The TIMER was set to
`GeneralPurpose` with a 3000 ms target and PMIC GPIO0 watched with
`gpio_pin_get_raw()`, alongside an I²C poll of `TimerIsExpired()` so a failure
would separate into "timer did not fire" versus "pin did not move".

| Usage | Pin config | Idle | After expiry | Timer |
|---|---|---|---|---|
| `InterruptLo` | default | 1 | 0 | fired |
| `InterruptLo` | output enabled | 1 | 0 | fired |
| `InterruptHi` | default | 0 | 1 | fired |
| `InterruptHi` | output enabled | 0 | 1 | fired |

**Use `GpioUsage::InterruptHi`.** It idles low and asserts high, which makes the
overlay's inherited `pmic-gpio0-gpios = <&gpio1 6 GPIO_ACTIVE_HIGH>` **correct as
written**. The overlay's "confirm against the PMIC / ADXL pin configuration once
driven" caveat is discharged for this pin; it still stands for `pmic-reset-gpios`
and `pmic-gpio1-gpios`, which remain undriven.

**`GpioSetUsage()` alone is sufficient.** The output-enabled variants behaved
identically, so `GpioConfigure()` is not needed on this path.

**Timing.** A 3000 ms target fired between 3000 and 3100 ms — the 100 ms poll
resolution bounds it no tighter — comfortably inside the ±3 % (25 °C) spec.

Probe source kept in the session scratchpad as `pmic_probe/`. It reuses
`src/npm2100.cpp` and the board overlay in place rather than copying either.

### 9.2 OutputSwitch pins — specified 2026-09-12

> **Superseded 2026-09-14 (bench session 1) — arming sequence and fire-pin
> isolation:** `docs/superpowers/specs/2026-09-14-arming-sequence-amendment.md`
> replaces the pin model below. The pins are **not** configured as outputs from
> boot: they are `GPIO_DISCONNECTED` (no driver at all, input and output
> buffers both off) whenever the device is not `Active`, and Enable() attaches
> `GPIO_OUTPUT_INACTIVE` only as the last step of arming. "Drive them low early
> in boot" no longer applies — there is no driver to drive low that early, and
> that is now the point: the external pull-downs hold the lines with nothing
> from this firmware to fight or fail.

**Fire1 = P2.05, Fire2 = P2.09. Both assert together on activation** — one logical
channel, two lines, not a set/reset pair. **Both are pulled down by external 10 kΩ
resistors**, so active-high.

```dts
fire1-gpios = <&gpio2 5 GPIO_ACTIVE_HIGH>;
fire2-gpios = <&gpio2 9 GPIO_ACTIVE_HIGH>;
```

**The hardware is fail-safe at boot and the firmware must not undo it.** The
nRF54L brings GPIOs up as high-Z inputs, so for the first milliseconds of every
boot the fire lines are undriven — and the 10 kΩ pull-downs hold them
de-energised. A reset loop or a brownout therefore cannot fire the output. As
originally specified here the two rules below followed; see the banner above
for how the arming sequence amendment actually implements this:

- Configure with `GPIO_OUTPUT_INACTIVE`, **never** `GPIO_OUTPUT_ACTIVE`.
- ~~Drive them low **early in boot**, alongside `parkFrontEndModule()`, so the
  pins spend as little time as possible relying on the pull-downs alone.~~
  Superseded: the pins are left disconnected, not driven, until armed.

#### Latent hazard — both pins are claimed by the DK board files

Both are free in this build, but only through two overrides made for unrelated
reasons:

| Pin | DK board files assign it to | Freed here by |
|---|---|---|
| P2.05 | `spi00` `cs-gpios`, the external-flash chip select | `&spi00 { status = "disabled"; }` |
| P2.09 | `led0`, "Green LED 0" | `&led0` remapped to `<&gpio1 2>` |

**Re-enabling `spi00` would hand Fire1 to the SPI driver as a chip select**, which
would toggle a fire line on every transaction. The `spi00` disable was originally
added only to silence a spurious `spi_nor` probe error, so nothing in the overlay
currently records that a safety-relevant output now depends on it. **Both
dependencies must be stated in a comment beside the fire-pin declarations.**

## 10. Documents this invalidates

To be corrected as part of the work, not left to rot:

- `docs/v1-scope.md` §1.0 — LED B is no longer the output mirror; `OutputSwitch`
  becomes the worked example.
- `docs/v1-scope.md` §1.1 — the deferred list shrinks.
- `docs/v1-scope.md` §6 — arm-state persistence is answered; record SCRATCHA.
- `CLAUDE.md` — "The counterpart must advertise at 20–50 ms" is **disproved**. A
  phone advertises at ~187 ms and the interval is not ours to set.
- `CLAUDE.md` — the Thingy:53 toggle tool section, for its retirement.
- `CLAUDE.md` — **"The device never advertises"** must be reworded, not deleted.
  It stands as the general rule; Report and Report-and-trigger are documented
  exceptions for use only when absolutely necessary. Proposed wording: *the
  device never advertises to solicit contact, and may emit a bounded burst on
  trigger only when explicitly configured to.*
- `CLAUDE.md` — the opening "this project is a fresh skeleton" paragraph, stale
  since 2026-08-17.

Added 2026-09-13 (security). **Done in the amendment commit:**

- `docs/tan-scheme.md` — rewritten as the day-key access scheme.
- `docs/power-budget.md` §8.1–8.3 — requirement restated for day keys; the 4-byte
  storage and six-digit guess-rate figures marked superseded.
- `docs/power-budget.md` §8.5 — "no external RTC" reaffirmed on BOM grounds.
- `docs/power-budget.md` §8.6 — the power-loss hole is closed by the invalid-on-boot
  clock.
- `docs/power-budget.md` §8.7.4 — the fixed 4-byte prefix and payload table replaced
  by rotating IDs and the new layouts.
- `CLAUDE.md` — the TAN rules restated for day keys.

**Left for the implementation (plan Task 16):**

- `docs/v1-scope.md` §1.0 — LED A is now the command acknowledgement, not only the
  arm-state indicator.
- `docs/v1-scope.md` §4 — "TAN (3) + version (1) + toggle (1) = 5 bytes" is obsolete.
- `Kconfig`, `credentials.conf.template`, `prj.conf` comments — `MFS_TAN_SEED`
  becomes `MFS_DEVICE_SECRET`, `MFS_DEVICE_ID` is added, `MFS_INSECURE_TOGGLE` is
  deleted, and the "5-byte payload" comment goes.
