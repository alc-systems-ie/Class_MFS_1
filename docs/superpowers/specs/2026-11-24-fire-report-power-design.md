# Fired reports at +20 dBm — short design

**Owner:** Andy, 2026-11-24. **Status:** approved by the owner 2026-11-24 ("Plus twenty is fine for that one case").

## Intent
A fired report is the one message the Network Manager must not miss: the device is gone after it. The
first end-to-end relay (00008, motion fire, 40 cm from the hub) arrived at −53 dBm with the standard
+10 dBm. The owner wants the fired report sent at **+20 dBm at the antenna**; nothing else changes.

## Decisions
- **Fired reports only** (`FiredFromCommand`, `FiredFromMotion`, `FiredFromSound`): +20 dBm requested
  at the antenna — 0 dBm from the SoC plus the nRF21540's POUTA (+20 dB). Activated, deactivated and
  low battery stay at **+10 dBm** (POUTB). The LNA/receive path is unchanged.
- **Regulatory (owner ruling 2026-11-24):** the standing rule "POUTB (+10 dB) only, per the Irish EIRP
  allocation" gains one exception — the fired report at +20 dBm. +20 dBm conducted is already at the
  ETSI EN 300 328 100 mW EIRP limit before antenna gain; the owner accepted that for this case.
- **Mechanism:** `CONFIG_MPSL_FEM_NRF21540_RUNTIME_PA_GAIN_CONTROL=y` (MPSL drives the MODE pin per
  transmission, choosing POUTA or POUTB from the requested power; GPIO-only mode is kept) and
  `CONFIG_BT_CTLR_TX_PWR_DYNAMIC_CONTROL=y` (per-advert-set power via the vendor HCI command
  `BT_HCI_OP_VS_WRITE_TX_POWER_LEVEL`, handle type ADV, handle 0 — as in Zephyr's `hci_pwr_ctrl`
  sample). `CONFIG_BT_CTLR_TX_PWR_ANTENNA=10` stays the default.
- **Each burst sets its own power, before and after `bt_le_adv_start()`** (+20 for fired, +10 otherwise):
  the SDC ignores the handle for legacy advertising, so the first call targets the single legacy advertiser
  and the second covers a controller that resets power on LE Set Advertising Parameters.
- **A power-setting failure never blocks or delays a fire**: it is logged (`LOG_WRN`) and the burst goes
  out at whatever power the controller has (the previous power).
- Which event gets which power is pure, host-tested logic beside `ReportPolicy::Enabled()`.

## Approaches considered
- **A (chosen): per-burst power through dynamic control + runtime PA gain.** Only the fired burst uses POUTA.
- B: raise `CONFIG_BT_CTLR_TX_PWR_ANTENNA` to 20 for everything — rejected: puts every report (and any
  future transmission) at the limit, against the owner's "for that one case".
- C: GPIO+SPI mode to set gain — rejected: GPIO+SPI transmitted ~40 dB low (2026-11-16) and CLAUDE.md
  forbids it without an RSSI check.

## Risks left to the bench
- POUTA draws ~110 mA typ against 38 mA at POUTB, during the fire-supply raise and verify (boost to
  3.3 V, VOUT ≥ 3.2 V). A sag could log a verify failure; the device fires anyway. Bench check added.
- Whether MODE actually switches: hub RSSI of a fired report vs an activated report from the same spot
  should differ by ~10 dB.
