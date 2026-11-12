# MFS_1 — To do

Open work items that are not yet planned. Each one names what it needs and why.

## FEM receive amplifier as an app-set option

**Added 2026-11-12.** The nRF21540 LNA is on for every scan window
(`docs/power-budget.md` §5). It costs 48.6 µA average, taking CR123A life from
~2.4 years to ~1.4 years, and without it the device hears commands at only ~5 m
(measured with `tools/range_pong`). That trade belongs to the installation, not
the firmware: a sensor a metre from where the engineer stands does not need the
range, and a sensor across a building does.

Make FEM use a per-device setting, set from the app:

- **Protocol:** a new field in the Settings command
  (`docs/superpowers/specs/2026-09-12-app-control-design.md` §6.1, units table).
  Settings are applied only while Inactive, like every other parameter.
- **Firmware:** persist it in NVS with the other settings and apply it at boot
  and on change. Default: **FEM on** — the safe choice, since a device that
  cannot hear a Disarm is the failure that matters.
- **Investigate first:** how to switch the FEM at runtime. `CONFIG_MPSL_FEM` is
  a build-time choice; check what MPSL offers for disabling the FEM or the LNA
  per radio event on nRF54L, and what the controller's TX power split does when
  the FEM is off (the +10 dBm antenna request must fall back to the SoC's own
  output, not leave the PA path unpowered).
- **App:** `../class_app` — the setting on the Settings page, with the battery
  life each choice gives.
- **Consider:** whether the "FEM off" setting should be refused while arming or
  armed, in line with "no scanner, no arming" — a device that cannot hear the
  phone must not reach Active.

## Anti-tamper switch: closed case must draw no current

**Added 2026-11-12. Hardware, next board revision.** The casing is meant to hold
the ANTI_TAMP button (P1.02, active low) down in normal use. With a normally-open
button that grounds the pin permanently, so its pull-up conducts all the time:

- **Internal pull-up** (14 kΩ typ, 12–16 kΩ, nRF54L datasheet R<sub>PU</sub>):
  3.0 V ÷ 14 kΩ ≈ **214 µA** (188–250 µA). Budget 117 µA → ~331 µA; CR123A life
  ~1.4 years → **~6 months**.
- **NFC-pad leakage:** P1.02 and P1.03 (ADXL INT1, idles high) are the NFCT pad
  pair. The datasheet warns of leakage between them when held at different logic
  levels (I<sub>NFC_LEAK</sub>; nRF54L figure still to look up). A held-down
  button keeps them different permanently.

v1 does not configure P1.02, so neither cost applies today — **unless the board
has an external pull-up on P1.02.** Check the schematic: if it does, the closed
case already draws 3.0 V ÷ R.

**Fix: an SPDT snap-action microswitch wired COM–NC** (NO unconnected), so the
closed case holds the contact open:

```
VDD ─ internal pull-up ─┬─ P1.02
                        COM
                        NC ─ GND
Case closed: NC open   → pin high, 0 µA, P1.02 and P1.03 both high (no leakage)
Case open:   NC closed → pin low, tamper
```

- Subminiature microswitches commonly come SPDT (e.g. Omron D2F / D2HW
  families) — confirm the exact variant. A short hinge-lever or roller actuator
  absorbs casing tolerance; check overtravel across the gap range.
- **Board-mounted, no flying leads.** In this wiring an open circuit reads as
  "case closed", so a broken lead or lifted joint would look secure. Inside the
  case it cannot be reached without opening the case first.
- A wall tamper, if wanted, is a second NC switch in parallel on the same pin.
- Alternatives if the mechanics are awkward: a lid magnet plus a Hall switch
  (~0.5–2 µA continuous), or the fitted OPT3001 as a light-on-opening backup
  (misses an opening in the dark).

**Firmware, when tamper is implemented:** `GPIO_ACTIVE_LOW | GPIO_PULL_UP`, no
external resistor; an edge interrupt wakes the SoC from System ON idle, as INT1
does. While the case is open the pull-up draws ~214 µA, so latch the event,
disable the pull-up, and re-check periodically for the case closing again.

## LED follow-ups (three-LED indication)

**Added 2026-11-12** (owner), from `docs/superpowers/specs/2026-11-12-three-led-indication-design.md` §7.

- **Orange reason codes.** After the 3 s orange steady, a flash code naming why
  arming was refused or what faulted (scanner down, fire supply, pin check,
  switch fault...). Today every fault shows the same orange.
- **Variable exit delay.** Make the 10 s pre-arming delay
  (`ArmingSequence::M_EXIT_DELAY_MS`) configurable. The steady red follows it.
- **Display-status option in the app.** Bound the green battery cost (steady
  ~1–2 mA before the time sync, ~170–330 µA flashing after it) — e.g. LEDs on only
  for a while after a command, or off entirely — set from the app.
- **Fault message.** When the device disarms itself (armed trigger, fail-safe),
  send a message confirming the fault. Needs its own design: the device never
  advertises to solicit contact, so it would be a further documented exception
  like the FIRE confirmation burst.
