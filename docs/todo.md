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
