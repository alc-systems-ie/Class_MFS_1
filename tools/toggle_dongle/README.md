# MFS_1 Toggle Tool

Bench advertiser for the MFS_1 engineer toggle. Press the button and it advertises
a toggle command for 8 s; MFS_1 catches it in a scan window and flips its arm
state — so the same button both activates and deactivates the sensor.

This exists to prove the scan, parse, arm-state and LED path before TAN
validation and provisioner time sync are implemented. It sends **no TAN and no
authentication of any kind**, and MFS_1 only obeys it when built with
`CONFIG_MFS_INSECURE_TOGGLE=y`.

The directory is still called `toggle_dongle` for the nRF52840 Dongle it was first
written for. It is no longer dongle-specific.

## Boards

The firmware is board-agnostic: it takes its button from the `sw0` alias and its
LED from `led0`, so it builds unchanged anywhere both exist. What those resolve to
is a per-board question, answered in `boards/<board>.overlay`.

| Board | Board target | Button (`sw0`) | LED (`led0`) |
|---|---|---|---|
| **Thingy:53** | `thingy53/nrf5340/cpuapp` | `button0`, the single enclosure pushbutton — P1.14 | RGB LED, **green** channel — P1.06 |
| nRF54L15 DK | `nrf54l15dk/nrf54l15/cpuapp` | `button0`, "Push button 0" — P1.13 | "Green LED 0" — P2.09 |
| nRF52840 Dongle | `nrf52840dongle/nrf52840` | `button0`, the side button — P1.06 | "Green LED 0" — P0.06 |

Pins and labels are from each board's devicetree, so the tool lights a green
indicator on all three.

On the **Dongle** only, note that its single button is also aliased
`mcuboot-button0`: the same button sends a toggle at run time and enters the
bootloader when held through a reset. That is harmless in use — the bootloader
samples it only at reset — but it is why the Thingy:53 overlay takes care to keep
the two roles on different buttons where the board has two.

### Thingy:53 — why that button and that LED

**Button.** The Thingy:53 exposes one user pushbutton, and it is `button0`, which
the board already aliases as `sw0`. Nordic's own Thingy:53 applications treat
`button0` as the single user button.

The devicetree declares a second button, `button1` (P1.13), and the tool
deliberately does **not** use it: the board aliases it as `mcuboot-button0`, so
holding it while the device resets is the MCUboot serial-recovery gesture. A demo
whose button sometimes drops the device into the bootloader instead of sending a
toggle is worse than no demo.

Note also that **SW1 on the Thingy:53 is the power slide switch**, not a
pushbutton — it must be **ON** for anything to happen.

**LED.** The Thingy:53 has a single RGB LED exposed as three GPIO channels:
`led0` = red (P1.08), `led1` = green (P1.06), `led2` = blue (P1.07). The tool
lights one LED for the duration of the burst, so on this board the channel it
picks *is* the colour:

- **red** — the board's only LED showing red is what an audience reads as a fault,
  and this indication means the opposite: the burst is going out.
- **blue** — aliased as `mcuboot-led0`, so MCUboot blinks it during DFU. Reusing
  it at run time makes the two states indistinguishable.
- **green** — unambiguously "transmitting", and claimed by nothing else. Chosen.

`boards/thingy53_nrf5340_cpuapp.overlay` therefore remaps the `led0` alias to
`&green_led`. The application code is unchanged and every other board keeps its
own `led0`.

### Thingy:53 — the network core is not optional

The nRF5340 keeps its radio on the **network core**, so an application-core image
with `CONFIG_BT=y` is only half a Bluetooth build. `Kconfig.sysbuild` selects the
`ipc_radio` network-core image with HCI serialisation:

```
config NRF_DEFAULT_IPC_RADIO
	default y

config NETCORE_IPC_RADIO_BT_HCI_IPC
	default y
```

Without it the Thingy:53 builds an **empty** network core — the board's own
`Kconfig.sysbuild` sets `SECURE_BOOT_NETCORE=y`, which sets `NRF_DEFAULT_EMPTY=y`,
and `NETCORE_EMPTY` then wins the choice. The build would succeed, `bt_enable()`
would fail at boot, and with no console attached the button would simply do
nothing.

These are Kconfig *defaults*, not hard assignments, so the file is inert on the
single-core targets: they leave `SUPPORT_NETCORE` unset and the choice never
appears. All three boards above build with it in place.

## Why the burst is 8 seconds

MFS_1 listens for only **100 ms in every 6000 ms** (`docs/power-budget.md` §3).
A short burst would overlap a scan window roughly **1.7%** of the time and appear
to do nothing.

So the burst must exceed one full scan period:

| Constant | Value | Constraint |
|---|---|---|
| `M_ADVERTISE_MS` (this tool) | 8000 ms | **>** `CONFIG_MFS_SCAN_PERIOD_MS` (6000) so a scan window must land inside it |
| `M_COMMAND_COOLDOWN_MS` (MFS_1) | 12000 ms | **>** the burst, so one press cannot be caught by two windows and toggle twice |

Keep that ordering if either value changes. **The same constraint applies to the
production Android app** — it must advertise for longer than the configured scan
period, which at the slowest preset (30 s) means a 30 s+ burst. Worth surfacing in
the app's UI as a progress indicator rather than an instant "sent".

Advertising uses `BT_LE_ADV_NCONN` at its default 100–150 ms interval. Within a
100 ms scan window that gives roughly a 95% chance of capture per window, and the
8 s burst spans one or two windows, so capture is effectively certain.

## Payload

Manufacturer-specific AD, 6 bytes. Must match `alc::CommandScanner` in
`../../src/command_scanner.cpp`.

| Offset | Bytes | Meaning |
|---|---|---|
| 0–1 | `FF FF` | Company ID 0xFFFF, reserved for test, little-endian |
| 2–3 | `4D 46` | Magic `'M'`, `'F'` |
| 4 | `01` | Protocol version |
| 5 | `01` | Command: `ToggleArm` |

0xFFFF is a test identifier. **A production build needs an assigned Bluetooth SIG
company ID.** The payload stays within the 12-byte ceiling from
`docs/power-budget.md` §8.7.4 so an iOS tool remains possible later.

## Build

Run from the repository root.

```sh
# Thingy:53 — also builds the ipc_radio network-core image and MCUboot.
west build -b thingy53/nrf5340/cpuapp -d build-thingy53 -p always tools/toggle_dongle

# nRF54L15 DK.
west build -b nrf54l15dk/nrf54l15/cpuapp -d build-dk -p always tools/toggle_dongle

# nRF52840 Dongle.
west build -b nrf52840dongle/nrf52840 -d build-52840dongle -p always tools/toggle_dongle
```

Sysbuild puts the artefacts one level down, e.g.
`build-thingy53/toggle_dongle/zephyr/zephyr.hex`. The Thingy:53 build additionally
produces `build-thingy53/dfu_application.zip` for the serial-recovery route below.

## Flash

**Any of these overwrites whatever is currently on the target** — including a
Thingy:53's preloaded demo firmware, or nRF Sniffer firmware on a Dongle.

### Thingy:53

There is no on-board debug IC. Two routes:

**External debug probe (fastest during bring-up).** Connect the Thingy:53's
10-pin debug connector to the *debug out* port of an nRF5340 DK or similar, set
the Thingy:53 power switch **SW1** to **ON**, then:

```sh
west flash -d build-thingy53
```

**USB-C, no probe.** Use MCUboot serial recovery with
`build-thingy53/dfu_application.zip`, via the Programmer app in nRF Connect for
Desktop or `nrfutil device`. Serial recovery is entered by holding the button the
board aliases as `mcuboot-button0` (`button1`, P1.13 — *not* the one this tool
uses) while powering the device on. See Nordic's "Programming Nordic Thingy
prototyping platforms" guide for the current procedure.

### nRF54L15 DK

```sh
west flash -d build-dk --snr <dk-serial>
```

Pass `--snr` if more than one probe is attached, so it does not flash the bespoke
board by mistake.

### nRF52840 Dongle

No debug connector — it programs over USB via the Open Bootloader, which needs a
signed DFU zip.

```sh
# One-time: install the packaging tool.
nrfutil install nrf5sdk-tools

# Package the application.
nrfutil pkg generate \
  --hw-version 52 --sd-req 0x00 \
  --application build-52840dongle/toggle_dongle/zephyr/zephyr.hex \
  --application-version 1 \
  build-52840dongle/toggle_dongle.zip

# Put the Dongle in bootloader mode: press the sideways RESET button.
# The red LED pulses and the device gains the nordicDfu trait.
nrfutil device list

# Program it.
nrfutil device program \
  --firmware build-52840dongle/toggle_dongle.zip \
  --traits nordicDfu
```

## Alternative: no firmware at all

**nRF Connect for Mobile on Android** can advertise arbitrary manufacturer data
from its Advertiser tab. Configure a non-connectable advertiser carrying
`FF FF 4D 46 01 01` as manufacturer data and enable it for 10 s or more. That
needs no tool firmware and exercises the actual production platform.

Useful if the hardware is wanted as a sniffer instead — with sniffer firmware a
Dongle can watch the adverts and MFS_1's scan windows directly, which is worth
more during bring-up than being the transmitter.

## Verifying

The tool has no console — `CONFIG_LOG=n`, `CONFIG_SERIAL=n`. Its LED is the only
local feedback: it lights for the full 8 s burst and goes out at the end.

MFS_1's RTT should show, on capture:

```
<inf> scanner: Command received, RSSI -xx dBm.
<inf> app: Arm state: Active.
```

On MFS_1, LED A (Inactive indicator) goes out and LED B becomes live — it lights
while the ADXL367 reports motion and clears about 5 s after movement stops.
