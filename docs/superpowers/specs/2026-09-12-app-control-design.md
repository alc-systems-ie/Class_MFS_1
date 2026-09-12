# MFS_1 App Control — Design

Recorded 2026-09-12.

Replaces the Thingy:53 toggle tool with a Flutter app that sets three parameters
on MFS_1 and arms it against a day code. The Thingy:53 tool is **retired** — no
co-existence, one payload format.

Prerequisite reading: `docs/v1-scope.md` (the arm invariant and the ADXL367
traps), `docs/power-budget.md` §8.7 (the tool platform and the time-sync rules),
`docs/tan-scheme.md` (what the day codes become).

## 1. Scope

Three settable parameters, sent as one payload with an arm instruction:

| Parameter | Range | Wire |
|---|---|---|
| Activations before triggering | 1–16 | 1 byte, literal |
| Cooldown between activations | 0–3600 s | 1 byte, geometric |
| Activation threshold sensitivity | 255 levels | 1 byte, geometric |

Cooldown is meaningful only when activations > 1; the app hides the slider
otherwise, and the device accepts the byte regardless.

Day codes are **ten hard-coded constants** this phase — `0x11111111`,
`0x22222222` … `0xAAAAAAAA` — plus `0x00000000` as a settings-test code. Each real
code is single-use; when all ten are spent the device releases all ten for the
next cycle. Derivation from the TAN seed is deferred to `docs/tan-scheme.md`.

## 2. Decisions

1. **Cooldown is a refractory blanking window.** An activation increments the
   count, then motion is ignored for the cooldown, then the sensor re-arms. The
   count **never expires** — it persists until the device triggers or is
   deactivated, so the device accumulates evidence of tampering over unlimited
   time.
2. **The nPM2100 TIMER runs the cooldown**, signalling through PMIC GPIO0.
3. **The ADXL367 is held in standby across the blanking window** and reconfigured
   through the full bootstrap on re-arm.
4. **Detection runs in both arm states.** The arm boolean selects the *consumer*,
   not whether detection happens: deactivated, the LED simulates; activated, two
   real GPIOs assert.
5. **Trigger output follows detection** and self-clears after the 5 s ADXL loop
   period, then re-arms. Unchanged from present LED behaviour.
6. **Geometric parameter encodings**, sensitivity byte 255 = most sensitive.
7. **NVS for all persisted state** this phase. nPM2100 SCRATCHA is the intended
   refinement for arm state — see §7.
8. **One Flutter app for the whole device series**, in `../class_app`.

## 3. Spike result — advertising is proven, at a measured cost

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

One 128-bit service UUID. On-air byte order — exactly what a scanner's hex dump
shows:

| Byte | Field | Notes |
|---|---|---|
| 0–1 | magic `'C' 'L'` (0x43 0x4C) | cheap reject |
| 2–3 | device type, LE `uint16` | MFS_1 = `0x0001` |
| 4 | protocol version | `0x01` |
| 5–8 | day code, LE `uint32` | `0x00000000` = settings-test |
| 9 | desired arm state | `0x00` Inactive, `0x01` Active |
| 10 | activations before trigger | 1–16; anything else rejects the payload |
| 11 | cooldown | geometric byte, `0` = none |
| 12 | sensitivity | geometric byte, 255 = most sensitive |
| 13–15 | **per-variant extension** | **MFS_1 MUST ignore, never validate** |

Bytes 0–3 are the 4-byte filter prefix reserved by §8.7.4; bytes 4–15 are the 12
usable payload bytes, of which nine are used here.

**Bytes 13–15 belong to other MFS variants.** MFS_1 must not require them to be
zero: doing so would make it reject payloads from a future app build the moment
another variant starts using that space.

### 4.1 The payload is an absolute state assertion

It says "be in this state with these settings", never "change". Two consequences:

- **It is idempotent.** At ~187 ms the device hears the same advert across
  several scan windows. The device dedupes on payload identity — a payload
  byte-identical to the last accepted one is ignored. No nonce, no counter.
- **The swallowed-second-press bug cannot occur.** The present 12 s command
  cooldown in `command_scanner.cpp` exists because the old payload was a toggle;
  it is removed.

### 4.2 Settings and arm state always travel together

Deactivated, the app sends `0x00000000` plus the live slider values and the device
applies them immediately for tuning. To arm, the app sends a real day code plus
the final settings in one payload. **There is no window in which the device is
armed with settings the engineer did not watch being tested.**

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

## 6. Firmware design

### 6.1 Units

| Unit | Responsibility |
|---|---|
| `CommandScanner` | parse AD 0x07, validate prefix/type/version, dedupe, hand 12 bytes to the loop |
| `DayCodes` | the hard-coded table, the used-mask in NVS, auto-release when all ten spend |
| `Settings` | the three parameters, NVS-backed, byte↔physical conversion |
| `OutputSwitch` | **stub** — two GPIOs, the only real consumer of `IsOutputActive()` |
| `App` | the state machine |

`CommandScanner` changes from parsing `BT_DATA_MANUFACTURER_DATA` to AD type 0x07.
The Thingy:53 tool is retired, so no dual parsing.

### 6.2 The arm invariant, restated

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

Runs in **both** arm states. The arm boolean selects the consumer, nothing else.

```
AWAKE rising edge
  -> m_activation_count++
  -> if m_activation_count >= N:
        m_detection_met = true            // latched; output follows this
        m_activation_count = 0
        // NO blanking here - the trigger's own AWAKE must run to completion
  -> else if N > 1 and cooldown > 0:
        Standby(); PMIC TimerSetDurationMs(cooldown); TimerStart()
        on GPIO0 interrupt: TimerClearExpiredEvent(); enableAccelerometer()

AWAKE de-asserts (5 s ADXL loop period)  ->  m_detection_met = false
```

Two rules the pseudocode encodes deliberately:

- **Blanking applies only between counted activations, never after the triggering
  one.** Standing the ADXL down at the moment of trigger would cut short the 5 s
  assertion that §2.5 defines as the output's duration.
- **Cooldown is meaningful only when N > 1.** With N = 1 every activation triggers,
  so there is nothing to blank between; the device accepts the byte and ignores it.

The count clears only on trigger or on deactivation. It does **not** expire.

### 6.4 Activation — order is load-bearing

Running detection while deactivated re-opens the stale-level hole that commit
`0a50910` closed: at the moment the engineer activates the device they are
handling it, so AWAKE is high and the count may already be non-zero. The
teardown-and-rebuild is what preserves §1.0.1.

1. validate the day code against the table and the used-mask
2. apply and persist the settings
3. `Standby()` → `ConfigureLoopMode(new threshold)` → **confirm AWAKE == 0**
4. zero the count, clear `m_detection_met`
5. arm boolean → Active, then `updateOutputState()`
6. **only now** consume the code and persist the mask

**Step 6 is last so a failed configure cannot burn a day code.** Arming is
refused, the code stays unused, and the engineer sends it again. Deactivation is
the mirror: boolean first, re-derive the output, and the engine keeps running so
tuning can continue.

### 6.5 The test code is strictly weaker

`0x00000000` is accepted **only while Inactive**, applies settings immediately, is
never consumed, and is **rejected outright if the payload asks for Active**. The
test code can never arm the device. This property holds even in a bench build.

### 6.6 Failures are silent

Per `docs/tan-scheme.md`: an invalid code, a used code, an out-of-range parameter
or a test code asking to arm all produce **no radio emission whatsoever**. The
device logs over RTT and does nothing else.

## 7. Persistence

**NVS in the nRF54L05's RRAM** for the used-code mask, the settings and the arm
state. RRAM is rated 10,000 write/rewrite cycles per 128-bit word line with 10
years retention at 85 °C.

Ten codes a day over 2.4 years is ~8,760 writes — **88 % of a single word line's
rated endurance**. That is acceptable only because NVS wear-levels across its
partition. Two rules follow: never write state to a fixed RRAM address, and
**write only on change**, never per scan or per boot.

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

It also has zero wear and unlimited writes, which suits a value that changes far
more often than a day code is consumed.

**It cannot hold the used-code mask**: ten codes need ten bits and SCRATCHA has
eight. Spanning SCRATCHA plus the three sticky bits would give eleven, at the cost
of two registers, two strobe sequences and an arbitrary split, for no gain.

## 8. App design

`../class_app` — one Flutter app for the device series.

```
lib/protocol/      wire format, device types, geometric mappings  <- mirrors the C++ header
lib/services/      advertiser.dart (wraps ble_peripheral)
lib/devices/mfs1/  MFS_1 screen + settings model
lib/main.dart      device picker -> device screen
```

Flutter rather than native because `docs/power-budget.md` §8.7.4 already chose
**Android** as the production platform. One codebase serves the iOS test tool now
and the Android production tool later, collapsing what was going to be two apps.

### 8.1 MFS_1 screen

Arm toggle; a 1–16 selector for activations; a cooldown slider shown only when
activations > 1, reading out mapped seconds; a sensitivity slider reading out mg,
insensitive left to sensitive right; one Send button.

Send behaviour follows the toggle invisibly. **Toggle showing Deactivated → sends
`0x00000000`** with the live slider values, so tuning costs no codes. **Moving the
toggle prompts for a hex day code**, sent with the final settings in one payload.

### 8.2 Workflow

1. Engineer inspects the device. LED A off ⇒ assume armed.
2. Move the toggle to Deactivate; the app prompts for a day code; Send.
3. Device deactivates, LED A lights, the code is consumed.
4. Adjust settings; Send. The app uses `0x00000000` automatically.
5. Observe LED B simulating triggers at the chosen sensitivity.
6. Move the toggle to Activate; the app prompts for a real code; Send with the
   final settings. The device arms with exactly the settings just watched.

### 8.3 What the app must be honest about

The device is radio-silent by design, so:

- **Send can never be confirmed.** The UI shows "Advertising… 30 s" with a
  countdown and directs the engineer to the LED. The LED is the only feedback
  channel that exists.
- **The used-code list is a guess**, not device state. The device is the sole
  arbiter. The list is advisory and has a manual reset.
- **No local name in the advertisement.** A covert device's counterpart must not
  broadcast a string.

### 8.4 iOS constraints

- **Foreground only.** Backgrounded, iOS moves service UUIDs to an overflow area
  that only Apple devices can read; an nRF54L05 scanner sees nothing.
- **The advertising interval is not controllable.** Hence the 30 s Send.

## 9. Verification required before this can work

1. ~~**PMIC GPIO0 has never been driven on this project.**~~ **RESOLVED 2026-09-12
   on hardware — see §9.1.**
2. **Re-measure the advertising interval on the iPhone.** 187 ms is a macOS figure.
3. **Confirm the two `OutputSwitch` GPIOs** — pins, active levels, and whether they
   assert together as one channel or are a set/reset pair. Assumed here: one
   channel, active-high, pins declared under `zephyr,user`.

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

## 10. Documents this invalidates

To be corrected as part of the work, not left to rot:

- `docs/v1-scope.md` §1.0 — LED B is no longer the output mirror; `OutputSwitch`
  becomes the worked example.
- `docs/v1-scope.md` §1.1 — the deferred list shrinks.
- `docs/v1-scope.md` §6 — arm-state persistence is answered; record SCRATCHA.
- `CLAUDE.md` — "The counterpart must advertise at 20–50 ms" is **disproved**. A
  phone advertises at ~187 ms and the interval is not ours to set.
- `CLAUDE.md` — the Thingy:53 toggle tool section, for its retirement.
- `CLAUDE.md` — the opening "this project is a fresh skeleton" paragraph, stale
  since 2026-08-17.
