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
| Delay before triggering | 0 s – 9 h | 7 bits of byte 9, piecewise |
| Operating mode | 3 modes | 2 bits of byte 4 |

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

Amended 2026-09-13:

9. **Delay before triggering**, 0 s to 9 h, in bits 1–7 of byte 9. Runs on a
   GRTC `k_timer`, never the PMIC timer, whose ±10 % would put a 9-hour delay
   anywhere inside a 108-minute window.
10. **A doubled delay interlock.** A software flag *and* the kernel timer must
    both agree no delay is pending before the output can assert, checked
    independently at the derivation point and again inside `OutputSwitch`.
11. **Three operating modes** in bits 6–7 of byte 4: Trigger only (default),
    Report and trigger, Report only. The latter two are **exceptions** to the
    never-advertise rule, for use only when absolutely necessary.
12. **The SoC stays awake for the whole delay** and shortens its scan period, so
    the deactivate path is as responsive as possible. Battery life is explicitly
    not a factor while a trigger is pending.

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

| Byte | Bits | Field | Notes |
|---|---|---|---|
| 0–1 | — | magic `'C' 'L'` (0x43 0x4C) | cheap reject |
| 2–3 | — | device type, LE `uint16` | MFS_1 = `0x0001` |
| 4 | 0–5 | protocol version | `0x01`; **6 bits, so the version check must mask** |
| 4 | 6–7 | **operating mode** | device-level, all variants — §4.3 |
| 5–8 | — | day code, LE `uint32` | `0x00000000` = settings-test |
| 9 | 0 | desired arm state | `0` Inactive, `1` Active |
| 9 | 1–7 | **delay before triggering** | device-level, all variants — §5.1 |
| 10 | — | activations before trigger | 1–16; anything else rejects the payload |
| 11 | — | cooldown | geometric byte, `0` = none |
| 12 | — | sensitivity | geometric byte, 255 = most sensitive |
| 13–15 | — | **per-variant extension** | **MFS_1 MUST ignore, never validate** |

Bytes 4 and 9 carry the two device-level settings because they apply to **every**
MFS variant, unlike bytes 13–15 which are per-variant. Packing them into existing
bytes keeps the payload at nine used bytes with three still spare.

### 4.3 Operating mode — bits 6–7 of byte 4

| Value | Mode | Behaviour |
|---|---|---|
| `00` | **Trigger only** | **Default.** Fires the output. Emits nothing. |
| `01` | Report and trigger | Broadcasts a trigger message immediately before firing |
| `10` | Report only | Broadcasts, does not fire |
| `11` | reserved | reject the payload |

**These modes are exceptions to a standing rule and must be documented as such.**
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

### 5.1 Delay before triggering — bits 1–7 of byte 9

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

### 6.5.1 THE DELAY INTERLOCK — safety critical

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

**Deactivating cancels a pending delay unconditionally.** `setArmState(Inactive)`
stops the timer, clears the flag, zeroes the activation count and re-derives the
output. A delay does not survive disarming, and it does not survive a reset either
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
2a. **Specify the report payload** (§6.5.2). Needs a device identifier that does
   not become a tracking beacon. **Report modes cannot ship until this is
   answered**; Trigger-only is unaffected.
3. ~~**Confirm the two `OutputSwitch` GPIOs.**~~ **RESOLVED 2026-09-12 — see §9.2.**

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
de-energised. A reset loop or a brownout therefore cannot fire the output. Two
rules follow:

- Configure with `GPIO_OUTPUT_INACTIVE`, **never** `GPIO_OUTPUT_ACTIVE`.
- Drive them low **early in boot**, alongside `parkFrontEndModule()`, so the pins
  spend as little time as possible relying on the pull-downs alone.

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
