# MFS_1 Toggle Dongle

Bench advertiser for the MFS_1 engineer toggle. Press **SW1** and it advertises a
toggle command for 8 s; MFS_1 catches it in a scan window and flips its arm state.

This exists to prove the scan, parse, arm-state and LED path before TAN
validation and provisioner time sync are implemented. It sends **no TAN and no
authentication of any kind**, and MFS_1 only obeys it when built with
`CONFIG_MFS_INSECURE_TOGGLE=y`.

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

```sh
west build -b nrf52840dongle/nrf52840 -d build-dongle -p always tools/toggle_dongle
```

Artefacts land in `build-dongle/toggle_dongle/zephyr/zephyr.hex` (sysbuild puts
them one level down).

## Flash

The nRF52840 Dongle has no debug connector — it programs over USB via the Open
Bootloader, which needs a signed DFU zip.

```sh
# One-time: install the packaging tool.
nrfutil install nrf5sdk-tools

# Package the application.
nrfutil pkg generate \
  --hw-version 52 --sd-req 0x00 \
  --application build-dongle/toggle_dongle/zephyr/zephyr.hex \
  --application-version 1 \
  build-dongle/toggle_dongle.zip

# Put the Dongle in bootloader mode: press the sideways RESET button.
# The red LED pulses and the device gains the nordicDfu trait.
nrfutil device list

# Program it.
nrfutil device program \
  --firmware build-dongle/toggle_dongle.zip \
  --traits nordicDfu
```

**This overwrites whatever is on the Dongle.** If it is currently running the
nRF Sniffer firmware, that is replaced; restore it later from nRF Connect for
Desktop.

## Alternative: no firmware at all

**nRF Connect for Mobile on Android** can advertise arbitrary manufacturer data
from its Advertiser tab. Configure a non-connectable advertiser carrying
`FF FF 4D 46 01 01` as manufacturer data and enable it for 10 s or more. That
needs no dongle firmware and exercises the actual production platform.

Useful if the Dongle is wanted as a sniffer instead — with sniffer firmware it can
watch the adverts and MFS_1's scan windows directly, which is worth more during
bring-up than being the transmitter.

## Verifying

MFS_1's RTT should show, on capture:

```
<inf> scanner: Command received, RSSI -xx dBm.
<inf> app: Arm state: Active.
```

LED A (Inactive indicator) goes out and LED B becomes live — it lights while the
ADXL367 reports motion and clears about 5 s after movement stops.
