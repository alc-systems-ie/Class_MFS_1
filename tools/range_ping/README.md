# Range test — `range_ping` and `range_pong`

A two-board walk test for MFS_1 radio range. Needs no computer once flashed:
the Thingy:53's LED tells you, every second, whether the round trip worked and
how strong the weaker direction was.

| Tool | Board | Role |
|------|-------|------|
| `tools/range_ping` | Thingy:53 (`thingy53/nrf5340/cpuapp`) | Carried. Pings once a second, grades the reply on its RGB LED |
| `tools/range_pong` | CLASS MFS board (`nrf54l15dk/nrf54l05/cpuapp`) | Placed. Answers each ping with the RSSI it heard |
| `tools/range_common` | — | Shared frame format, `range_protocol.hpp` |

**Bench tools only.** Nothing is authenticated, and the main firmware never sends
or accepts these frames.

## How it works

Both sides use non-connectable, non-scannable extended advertising on the **LE
Coded PHY with S8 coding**, primary and secondary, at a 100 ms interval, and
scan the Coded PHY continuously.

1. The ping steps a sequence number in its advertisement every second. Each
   sequence number goes out about ten times.
2. The pong answers the **first** copy of each new sequence number: it updates its
   own advertisement to echo that number plus the RSSI the ping arrived at, and
   flashes LED2. It stops advertising after 5 s with no ping.
3. At the end of each second the ping counts that second as answered if a pong for
   the current sequence number arrived, and records both RSSIs: the pong as heard
   by the Thingy, and the ping as heard by the MFS.

### Frame format

Manufacturer-specific data (AD type `0xFF`):

| Bytes | Field |
|-------|-------|
| 0–1 | Company ID `0xFFFF`, little-endian (Bluetooth SIG: internal testing) |
| 2–3 | Magic `"CR"` (CLASS range) |
| 4 | Type: `0x01` ping, `0x02` pong |
| 5–6 | Sequence number, little-endian — a pong echoes its ping's |
| 7 | Pong only: RSSI of that ping at the MFS, signed dBm |

## Transmit power and the FEM

| Side | Path | TX at antenna | RX |
|------|------|---------------|----|
| Ping | nRF5340 0 dBm + nRF21540 POUTA | **+20 dBm** | nRF21540 LNA, +13 dB |
| Pong | nRF54L05 0 dBm + nRF21540 POUTB | **+10 dBm** | nRF21540 LNA, +13 dB |

**The ping's +20 dBm is bench only.** It sits at the ETSI EN 300 328 limit of
20 dBm EIRP before any antenna gain. The product uses POUTB (+10 dB) for Irish
EIRP compliance, which is why the pong uses it: the pong measures the MFS as it
will ship. To test the pong at full power, set
`CONFIG_MPSL_FEM_NRF21540_TX_GAIN_DB=20` and `CONFIG_BT_CTLR_TX_PWR_ANTENNA=20`
in `range_pong/prj.conf` (bench only, same caveat).

**The pong must drive the FEM.** The MFS board's nRF21540 has no bypass mode; left
powered down it sits in the RF path and attenuates both directions. Before
2026-11-12 the pong did not declare it, and the MFS heard pings to only ~5 m
with the board on the ground. See `docs/power-budget.md` §5.

## Reading the Thingy:53 LED

A pong flashes a colour for 200 ms, graded by the **weaker** of the two RSSIs:

| Colour | Weaker RSSI | Meaning |
|--------|-------------|---------|
| Green | stronger than −70 dBm | Comfortable |
| Yellow | −70 to −85 dBm | Good |
| Red | −85 to −95 dBm | Marginal |
| Magenta | −95 dBm or weaker | At the edge (Coded S8 sensitivity is about −103 dBm) |
| Blue, short blip | — | No pong for that ping |
| Blue, three longer blinks | — | Boot |
| Red, steady | — | The radio failed to start |

On the MFS, **LED2** (P2.03) flashes on each new ping heard. LED2 dark while
the Thingy blinks blue means the MFS is not hearing the ping. LED2 flashing while
the Thingy still blinks blue means the MFS hears the ping but the Thingy misses
the pong.

## Build

From each tool's directory. In Claude's shell, wrap `west` in
`nrfutil toolchain-manager launch --ncs-version v3.2.4 -- ...`.

```sh
# Ping - Thingy:53. Builds the app, the ipc_radio network core and MCUboot.
cd tools/range_ping
west build -b thingy53/nrf5340/cpuapp -p always

# Pong - CLASS MFS board. No credentials needed.
cd tools/range_pong
west build -b nrf54l15dk/nrf54l05/cpuapp -p always
```

The ping's network core is configured by `range_ping/sysbuild/ipc_radio/prj.conf`
(Coded PHY, FEM gain), which replaces ipc_radio's own `prj.conf`.
`range_ping/Kconfig.sysbuild` makes sure the Thingy:53 gets a radio image on its
network core at all — without it the board defaults to an empty network core,
the build succeeds, and `bt_enable()` fails at boot (see the project `CLAUDE.md`).

## Flash

**Pong — MFS board.** Always `--recover` on this board:

```sh
cd tools/range_pong
west flash --recover --dev-id 853003346
```

This **erases the main firmware and its stored settings** (access state, tuning).
Re-flash the main application afterwards and re-provision the clock.

**Ping — Thingy:53.** No on-board debug IC. Either an external probe on the 10-pin
debug connector (power switch SW1 on), then `west flash`, or MCUboot serial
recovery over USB-C with `build/dfu_application.zip`. The second route is
described in `tools/toggle_dongle/README.md`.

## Logs

- **Ping:** USB CDC ACM serial on the Thingy:53's USB-C port. One line per ping
  (`Ping 42: pong, RSSI here -71 dBm, at MFS -64 dBm.` or `no pong`) and a
  summary every 10 pings with the running percentage. Logging is deferred, so an
  unplugged cable never stalls the ping loop.
- **Pong:** RTT. One line per pong sent, and a count of pings heard every 10 s.

## Running a walk test

1. Power both boards. The Thingy blinks blue three times, then flashes green at
   close range.
2. **Baseline at 1 m**, line of sight: note both RSSIs from the ping log. Expect
   roughly −20 to −30 dBm. A baseline 30 dB or more below that points to an RF
   hardware fault before you start walking.
3. Place the MFS where it will be installed: on the floor, in its housing, against
   the surface it will be fixed to. Placement dominates: on the ground the range is
   a fraction of waist height.
4. Walk away with the Thingy. Note the distance where the LED turns red, then
   magenta, and where blue blips start.
5. At the first blue blips, check which direction failed first (LED2, above).

## Results

| Date | Pong firmware | Placement | Result |
|------|---------------|-----------|--------|
| 2026-11-12 | FEM undeclared (powered down) | Ground | ~5 m |
| 2026-11-12 | FEM undeclared (powered down) | Waist height | ~20 m |
| 2026-11-12 | FEM driven, POUTB, LNA on | Same walk | Every ping received by the MFS |

## Notes

- The pong never references the fire pins: they stay at reset, held low by their
  external 10 k pull-downs.
- The pong stops the nPM2100 boot monitor over I2C at boot, with the main
  application's unmodified `npm2100` driver. Without that the SoC resets about
  9 s after boot.
- The pong overlay carries the main application's HFXO (14 pF) and LFXO (9 pF)
  load capacitance, because the radio frequency depends on the HFXO trim.
- `uart20` is disabled on the pong: its DK pins are the nPM2100 I2C bus on this
  board.
