# Three-LED Indication — Design

Project CLASS MFS_1. Agreed with the owner 2026-11-12. Supersedes the two-LED
acknowledgement scheme (LED A acknowledgements, LED B warning and bench detection
indicator) in `docs/superpowers/specs/2026-09-12-app-control-design.md` §6.7 and the
LED parts of the arming-sequence, disarmed-test-mode and FIRE amendments.

## 1. Why

The first CLASS MFS board brought up two usable LEDs; the board now has three, each a
different colour. The owner wants the LEDs to read as a simple traffic-light language
an engineer understands at a glance: green = safe (disarmed), red = arming / firing,
orange = "the device heard you" or "something stopped it". The existing scheme
overloaded two single-colour LEDs with patterns an engineer had to memorise.

## 2. Hardware

| LED | Colour | Pin | Today's alias / role |
|-----|--------|-----|----------------------|
| LED1 | **Red** | **P2.02** | Undriven (wrongly believed shared with nPM2100 GPIO_1) |
| LED2 | **Orange** | P2.03 | `led1`, "LED B" — warning, bench detection |
| LED3 | **Green** | P2.04 | `led0`, "LED A" — acknowledgements |

**Correction (owner, 2026-11-12):** nPM2100 GPIO_1 is on **P2.01**, not P2.02. The
"OPEN — P2.02" item in `CLAUDE.md` and the overlay header comment came from a typo in
the board pin list. P2.02 is LED1 alone and is safe to drive. Fix both documents.

The DK's nRF54L05 board files define only `led0` and `led1`. The overlay must add a
third LED node (or equivalent `zephyr,user` GPIO) for P2.02. *My call:* LED1 is assumed
**active-high**, like LED2 and LED3; the bench checklist gets a first-power-on polarity
check, because a wrong polarity would light red throughout the Armed state. Naming the LEDs by colour
in code (red / orange / green) instead of A / B is recommended — *builder's call*.

## 3. Behaviour

The device state decides a **base level**; short **overlays** play on top of it. All of
this applies in **every build, production included** (see §5).

### 3.1 Base level by state

| State | Green | Red | Orange |
|-------|-------|-----|--------|
| **Disarmed, clock not set** — every boot, until the provisioner time sync | **steady on** | off | off |
| **Disarmed, clock set** (Inactive) | **slow flash: 500 ms on, 2500 ms off** | off | off |
| **Disarmed by the device itself** — latched after an armed trigger, a fail-safe disarm, or an arming that failed at the end of the exit delay | off | off | **steady on** until the next accepted command |
| **Arming** (10 s exit delay) | off | **steady on** | off |
| **Armed** (Active) | off | off | off — **nothing visible** |

**Device-initiated disarm shows orange, not green** (owner, 2026-11-12, "show yellow
(fault)"). The device can fall back to disarmed with nobody present: after every armed
trigger (`TakeTriggerComplete()` → `disarmDevice()`, and a live FIRE's end), after every
fail-safe disarm (scanner lost, fire switch fault while armed), and after an arming that
fails at the end of the exit delay (owner, 2026-11-12: the engineer has usually walked
away believing it armed; a Disarm from the app acknowledges it and turns it green). Green there would sit beside whoever set it
off and read as "safe". Instead orange stays on, steadily, until the next **accepted,
authenticated** command (any type, including a time set), which returns it to the normal
base level for the state that command leaves. This needs a small display latch tracked
apart from the arm state: set by every disarm the device makes on its own, cleared by any
accepted command. It never affects the arm boolean or the output.

- **Boot does not set the latch** (owner, 2026-11-12): a booted device shows steady
  green, which now also means "clock not set — needs a provisioner time sync". The
  clock is invalid on every boot, so steady green after a field power loss or battery
  change is itself the sign.
- *My call:* an engineer's **Disarm** command clears it and shows green; a Disarm is the
  ordinary way back to a known-safe display.
- Steady orange costs the same as steady green (§3.1 note below).

**Green therefore has two meanings** (owner, 2026-11-12): **steady = disarmed, clock
not set**; **slow flash (500 ms on / 2500 ms off) = disarmed, clock set, ready**. The
switch from steady to flashing happens when an authenticated provisioner time sync is
applied, after its orange triple confirmation. The flash is a background level: the
overlays in §3.2 interrupt it and it resumes afterwards (phase need not be kept).

Armed shows nothing, including during an armed FIRE countdown and an armed trigger
delay. Covertness while armed is unchanged.

**The green level is a deliberate, temporary battery cost** (owner, 2026-11-12). An LED
at ~1–2 mA is 1,000–2,000 µA steady against a ~117 µA budget. The slow flash is a 1-in-6
duty cycle, ~170–330 µA — still two to four times the whole budget, so a device left
disarmed with its clock set lasts months, not years. Steady green lasts only until the
time sync, which every installation does at once. Accepted for now; a display-status
option set from the app will bound it later (§7). Do not change the duty cycle, dim or
pulse further without the owner.

### 3.2 Overlays

| Event | Pattern | LED | Then |
|-------|---------|-----|------|
| **Arm accepted** | Steady red for the whole exit delay (the Arming base level) | Red | — |
| **Arming completed** (device Active) | **Red flashing** — today's Armed pattern (~8 Hz, 3 s) moved to red | Red | Dark (Armed) |
| **Accepted command confirmation** — Disarm (incl. one that cancels an arming or a pending trigger), Settings applied, Mode changed, **Time set** (new) | **Orange triple flash**, short (~1 s in all) | Orange | Back to the base level |
| **Arming refused / fault** — every source that today raises `App::signalWarning()` | **Orange steady ~3 s** | Orange | Back to the base level: green for an Arm refused on receipt (the engineer is there); the orange latch for a failure at the end of the exit delay or a fail-safe disarm |
| **Simulated firing while disarmed** — a disarmed test trigger, and a disarmed (rehearsal) FIRE | **Red on** for the window in which the fire output *would* be asserted if armed | Red | Back to green |

Notes:

- **Arm has no orange confirmation** — the red sequence is its confirmation.
- **Time set** is new: today an authenticated, applied provisioner time sync logs
  `Clock set by provisioner` but shows nothing. It gets the orange triple flash.
- **The distinct patterns go**: Disarmed (1 Hz), DisarmedDelayCancelled (double blink),
  SettingsApplied (single blink) and ModeChanged (double blink) all become the one
  orange triple. The reason is no longer shown by the LED; it remains in the log.
- **Faults share the refused-arm orange for now** (owner): fire switch fault while armed,
  scanner loss while arming or armed, fire-pin disable failure, boot pin check failure,
  fire switch `Set()` failure. Telling them apart is the reason-code to-do (§7).
- **Simulated firing follows the output, not raw motion.** Disarmed differs from armed
  in exactly two ways (CLAUDE.md, §1.0 of `docs/v1-scope.md`), one of which is that a
  trigger shows on an LED instead of the fire pins. Red therefore lights when the
  detection engine's trigger would assert the output if armed — after activations,
  cooldown and delay — not on every ADXL wake. Checked against the code (2026-11-12):
  `DetectionEngine::DetectionMet()` is that signal — set only on the final activation or
  delay expiry, then held 5 s. A rehearsal FIRE's window
  (`FireSequence::RehearsalFired()`) is ORed in, as it is today.
- **Simulated firing shows only while Inactive.** *My call:* a disarmed rehearsal FIRE
  runs 10 s and an Arm accepted during it is not cancelled (`FireSequence` has no cancel
  path, and §8 leaves FIRE untouched), so its red window can fall into Arming or Active.
  The LED code masks it there: Armed shows nothing, and Arming shows only its steady red.

### 3.3 Precedence

When more than one thing wants an LED at once, highest first:

1. Orange steady (refusal / fault, and the device-disarmed latch) — shown even if a
   confirmation was playing.
2. Red flashing (arming completed).
3. Orange triple (confirmation).
4. Red simulated firing (disarmed).
5. Base level.

While an overlay plays, the **green base level is turned off** so the overlay reads
cleanly on its own — the same reason today's patterns are framed by
`LedSequencer::M_FRAME_GAP_MS` of dark. *My call, not the owner's:* simulated firing
also turns green off while red is lit, so "red" is unambiguous; revisit if the owner
wants green to stay on beneath it.

### 3.4 Unchanged rules

- **Only an authenticated, accepted command produces a confirmation.** A failed
  authentication, replay or malformed payload shows nothing (design spec §6.7) — the LED
  must never tell an attacker which guesses got through. An arming *refusal* after an
  authenticated Arm is not an authentication failure; it shows orange as today's
  warning does.
- **Persist before acting**, and the confirmation plays only once the new state is real.
- **While Arming, Arm and Settings are ignored outright — no LED** (arming-sequence
  amendment §3.1). Disarm during Arming cancels the arming → orange triple → green.
- **Armed, Arm or Settings change nothing** and today replay the Armed pattern. Under
  this scheme they replay the **red flashing** pattern — it briefly shows an armed
  device to whoever sent the authenticated command, as today. *Builder:* keep that, it
  is the existing behaviour moved to red.
- **One writer per pin.** Today each LED has exactly one writer at a time (main loop, or
  its 10 ms pattern timer while a pattern plays). Keep that invariant for all three.
- The arming-sequence order, fail-safe rules and the arm boolean are **untouched**. This
  is presentation only: LEDs read state through `m_arming.State()`, the engine and
  `IsOutputActive()`; nothing new derives the output (CLAUDE.md architectural invariant).

## 4. Timing

| Item | Value | Source |
|------|-------|--------|
| Disarmed, clock set (green flash) | 500 ms on, 2500 ms off (3 s period) | owner, 2026-11-12 |
| Exit delay (steady red) | 10 s — `ArmingSequence::M_EXIT_DELAY_MS` | existing; to become variable (§7) |
| Red flashing | ~8 Hz, 3 s | existing Armed pattern |
| Orange triple | 3 flashes, ~1 s total | *builder's call* on exact on/off times; must read as distinct from the 3 s steady orange |
| Orange steady | ~3 s | existing warning duration |
| Simulated firing | the would-be output assertion window | existing engine / FIRE timing |

## 5. Build options

- **Production gets all of §3.** Today `CONFIG_MFS_DEBUG_LED=n` (production) leaves the
  idle level dark and hides the detection indicator. The owner wants green-while-disarmed
  and red simulated firing in production ("an important behaviour" — engineers walk-test
  installed devices).
- `CONFIG_MFS_DEBUG_LED` therefore loses its LED meaning. *Builder's call:* remove it, or
  keep it only for the bench-only detection *logging* it also gates — but no LED
  behaviour may differ between bench and production builds. Update its Kconfig help and
  `prj.conf` comments either way.
- `CONFIG_MFS_BATTERY_TEST` repurposes LED A as a liveness blink. Keep its intent: in a
  battery-test build the green level (steady or slow flash) is replaced by its liveness blink (or by
  nothing when `CONFIG_MFS_BLINK_MS=0`), since a steady LED would swamp the measurement.

## 6. Code areas (for orientation, not prescription)

- `src/led_sequencer.{hpp,cpp}` — `LedPattern` and the pure, host-tested pattern player.
  New patterns: red flashing (rename of Armed), orange triple, orange steady. Remove the
  four distinct acknowledgement patterns. Update `tests/test_led_sequencer.cpp`.
- `src/app.{hpp,cpp}` — the main-loop LED derivation (base level by state, simulated
  firing), `playLedPattern()` call sites (the command path and the arming completion),
  `signalWarning()` (now orange steady), `initLeds()` (three pins), the time-sync path
  (new confirmation), and the per-LED timers.
- `boards/nrf54l15dk_nrf54l05_cpuapp.overlay` — third LED on P2.02; fix the P2.02 comment.
- `Kconfig`, `prj.conf` — §5.
- Docs: `CLAUDE.md` (pin map: LED1 red P2.02, LED2 orange, LED3 green; remove the
  P2.02 OPEN item; the LED rules in the invariant section and access rules that name
  LED A / LED B), `docs/v1-scope.md` (LED section), the app-control design spec §6.7 and
  the amendments that name LED A / LED B (point them here), and the bench checklist (a
  new section to verify §3 on the board).

## 7. To do (owner, not part of this change)

- **Reason codes for orange.** A flash code after the steady orange naming why arming
  was refused or what faulted (scanner down, fire supply, pin check, switch fault...).
- **Variable exit delay.** Make the 10 s pre-arming delay configurable (owner, 2026-11-12).
  The steady red follows whatever the delay is.
- **Display-status option in the app.** Bound the steady-green battery cost — e.g. LEDs
  on only for a while after a command, or off entirely — set from the app (§3.1).

- **Fault message.** When the device disarms itself (trigger, fail-safe, boot), send a
  message confirming the fault (owner, 2026-11-12). Needs its own design: the device
  never advertises to solicit contact, so it would be a further documented exception
  like the FIRE confirmation burst.

Add these to `docs/todo.md`.

## 8. Non-goals

- No change to arming, disarming, fail-safe, detection or FIRE behaviour.
- No new LED for warnings; the dedicated warning light remains undecided.
- No dimming / PWM.
- No change to what is logged.
