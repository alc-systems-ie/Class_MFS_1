# Fired Reports at +20 dBm Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Send the fired event report at +20 dBm at the antenna (nRF21540 POUTA) while every other report stays at +10 dBm (POUTB).

**Architecture:** A pure, host-tested `ReportPolicy::TxPowerDbm(event)` picks the power; `CommandAdvertiser::Burst()` takes it and sets the advert set's power with the vendor HCI command after starting; Kconfig enables runtime PA gain control and dynamic TX power.

**Tech Stack:** NCS v3.2.4 / Zephyr, SoftDevice Controller, MPSL FEM (nRF21540 GPIO-only), C++20 host tests (`make test`).

**Spec:** `docs/superpowers/specs/2026-11-24-fire-report-power-design.md`

## Global Constraints

- House style: `~/.claude/CLAUDE.md` (C++20, `alc` namespace, `M_` constants, PascalCase public methods, no literals in calls, `LOG_ERR` ends `!`), and run `/Users/andi/nrfenv/bin/clang-format -i` on every touched `src/` and `tests/` file.
- Fired = `FiredFromCommand`, `FiredFromMotion`, `FiredFromSound` → **+20 dBm**. Every other event → **+10 dBm**.
- GPIO-only FEM mode stays (`CONFIG_MPSL_FEM_NRF21540_GPIO=y`). **Never** enable `CONFIG_MPSL_FEM_NRF21540_GPIO_SPI`. Keep `CONFIG_BT_CTLR_TX_PWR_ANTENNA=10`, `CONFIG_MPSL_FEM_NRF21540_TX_GAIN_DB=10`.
- A failure to set power is `LOG_WRN` and the burst continues — it never fails `Burst()`, never blocks or delays a fire.
- **No secrets, no flashing, no J-Link.** Never read `secrets/`. Build check only: `nrfutil toolchain-manager launch --ncs-version v3.2.4 -- west build -b nrf54l15dk/nrf54l05/cpuapp -p always -d /tmp/<unique>` (a plain build is generation 0, no keys — allowed). Never `-D` a secret.
- `make test` must pass; `sh tools/test_flash_device.sh` must still pass (unchanged script).

## Review Focus

1. **A fired burst followed by a non-fired burst** — the second must go out at +10 dBm, not inherit +20. Covered by setting power on every burst (Task 1 Step 3) and the bench check (Task 2).
2. **`FiredFromSound`** (never sent by MFS_1) still maps to +20 — host test in Task 1.
3. **Power-set failure** — burst still advertises and `Burst()` returns 0; reviewer checks the code path (target-only, no host seam).
4. **Kconfig actually resolves** — the built `.config` shows `CONFIG_MPSL_FEM_NRF21540_RUNTIME_PA_GAIN_CONTROL=y` and `CONFIG_BT_CTLR_TX_PWR_DYNAMIC_CONTROL=y` (Task 1 Step 6).
5. **Docs no longer say "+10 only"** without the exception — CLAUDE.md, power-budget §5, prj.conf comments (Task 2).

---

### Task 1: Per-burst TX power — fired reports at +20 dBm

**Files:**
- Modify: `src/report_policy.hpp`, `src/report_policy.cpp`, `tests/test_report_policy.cpp`
- Modify: `src/command_advertiser.hpp`, `src/command_advertiser.cpp`, `src/app.cpp` (`App::sendReport`)
- Modify: `prj.conf`

**Interfaces:**
- Produces: `static int8_t ReportPolicy::TxPowerDbm(report::ReportEvent event);` with `static constexpr int8_t M_TX_POWER_DBM { 10 };` and `static constexpr int8_t M_FIRED_TX_POWER_DBM { 20 };` in `ReportPolicy`.
- Produces: `int CommandAdvertiser::Burst(const uint8_t uuid[16], int64_t nowMs, int64_t durationMs, int8_t txPowerDbm);`

- [ ] **Step 1: Failing host tests in `tests/test_report_policy.cpp`** (follow the file's existing assert style): `TxPowerDbm(FiredFromCommand) == 20`, `FiredFromMotion == 20`, `FiredFromSound == 20`, `LowBattery == 10`, `Activated == 10`, `Deactivated == 10`.
- [ ] **Step 2: `make test`** — fails to compile (`TxPowerDbm` undefined).
- [ ] **Step 3: Implement.** `TxPowerDbm` in report_policy. `Burst()` gains `txPowerDbm`; after a successful `bt_le_adv_start()` it calls a private `setTxPower(int8_t dbm)` that sends `BT_HCI_OP_VS_WRITE_TX_POWER_LEVEL` with `handle_type = BT_HCI_VS_LL_HANDLE_TYPE_ADV`, `handle = 0` (pattern: `/opt/nordic/ncs/v3.2.4/zephyr/samples/bluetooth/hci_pwr_ctrl/src/main.c` `set_tx_power`), checks the response status, logs `LOG_INF("Report advert at %d dBm (controller selected %d dBm).", ...)` on success and `LOG_WRN("Report advert power not set (%d) - default power.", ...)` on failure, and never changes `Burst()`'s return value. `App::sendReport()` passes `ReportPolicy::TxPowerDbm(event)`. Update the `Burst()` Doxygen (power parameter, "set on every burst so a fired burst never leaves the next one at +20").
- [ ] **Step 4: Kconfig in `prj.conf`** (FEM block): add `CONFIG_MPSL_FEM_NRF21540_RUNTIME_PA_GAIN_CONTROL=y` and `CONFIG_BT_CTLR_TX_PWR_DYNAMIC_CONTROL=y` (plus `CONFIG_BT_HCI_VS=y` only if the build needs it), with a comment: fired reports request +20 dBm, MPSL selects POUTA by the MODE pin for those transmissions only; everything else stays at the +10 dBm default (POUTB). Fix the stale comment that says the PA is used "on the FIRE confirmation burst" (it is the event reports now).
- [ ] **Step 5: `make test`** — ALL TESTS PASSED.
- [ ] **Step 6: Build check** — the build command in Global Constraints succeeds; `grep -E "RUNTIME_PA_GAIN_CONTROL|TX_PWR_DYNAMIC_CONTROL|TX_PWR_ANTENNA|TX_GAIN_DB" <builddir>/class_mfs_1/zephyr/.config` shows both new options `=y`, antenna 10, gain 10. Remove the temp build dir afterwards.
- [ ] **Step 7: Commit** — `Fired reports at +20 dBm: per-burst TX power, runtime PA gain (owner 2026-11-24)`.

### Task 2: Documents

**Files:**
- Modify: `CLAUDE.md` (settled-decisions FEM row; the "POUTB (+10 dB) only … Irish EIRP" bullet; the Event reports paragraph), `docs/power-budget.md` (§5 Transmit paragraph and the TX table row — add POUTA ~110 mA typ, nRF21540 PS, for fired reports only, still negligible: one 100 ms burst per device life), `docs/superpowers/plans/2026-09-13-bench-checklist.md` (§5n: new items).

- [ ] **Step 1: Update the three documents** — state the rule as: +10 dBm (POUTB) for every transmission **except the fired report at +20 dBm (POUTA), owner 2026-11-24, at the ETSI 100 mW EIRP limit before antenna gain**; cite the design note. Keep "Never GPIO+SPI".
- [ ] **Step 2: Bench checklist §5n items:** (a) hub `rssi` of a fired report is ~10 dB above an activated report from the same position (40 cm: −53 dBm baseline at +10); (b) the fire still asserts on time and RTT (unlocked bench build) shows `Report advert at 20 dBm`; (c) the fire-supply verify does not log a failure caused by the POUTA load — if it does, record it (the device fires anyway).
- [ ] **Step 3: Commit** — `Docs: fired report at +20 dBm (POUTA) - the one exception to POUTB only`.
