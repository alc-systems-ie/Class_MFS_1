# `uuid_observer` — bench UUID observer

A bare-metal passive BLE scanner for a **second** development board, used
alongside MFS_1 to see exactly what a counterpart (the Flutter class app, or
any other advertiser) is actually broadcasting and when. It scans
continuously — no duty cycle, no window/interval trade-off — and logs every
128-bit service UUID it hears, timestamped, with the advertiser's address and
RSSI. Consecutive lines for the same UUID show the advertiser's real
advertising interval directly; a change of UUID shows exactly when a new
command went on air.

This is what produced the measurements behind
`docs/superpowers/specs/2026-09-14-scan-reliability-amendment.md` §1: the
Mac's steady 187.5 ms advertising interval, the iPhone's ~35 ms interval, and
iOS's brief re-broadcast of the previous payload at the start of a new Send.

Once a new counterpart interval has been measured this way, check it against
the scan period with `tools/scan_phase_check.py` (a phase-coverage simulation
- see the amendment §2), not by inspection: `python3 tools/scan_phase_check.py
<interval_ms>`. A FAIL means the scan period needs revisiting for that
interval; it is not a fault in this observer.

It does not touch MFS_1's protocol, keys or access logic at all — it is a
plain `bt_le_scan_start` passive scan with a UUID-list AD parser. Nothing here
is specific to CLASS; it would work as a general BLE service-UUID sniffer on
any nRF54L board.

## Board

Built for a **second** nRF54L15 DK — not the MFS_1 board under test — running
the full nRF54L15, not the nRF54L05 target MFS_1 uses:

```
nrf54l15dk/nrf54l15/cpuapp
```

## Build and flash

```sh
cd /Users/andy/nordic/ncs/v3.2.4/class_mfs_1
west build -b nrf54l15dk/nrf54l15/cpuapp -d build-observer tools/uuid_observer
west flash -d build-observer --dev-id <observer-DK-serial> --recover
```

Use the **observer** DK's own `--dev-id` / J-Link serial number — not the one
flashed with MFS_1 firmware. Running two DKs on the same bench needs two
distinct J-Link serials; see session memory ("J-Link probe identity") for
which serial belongs to which board.

## Console

The log comes out on the DK's **VCOM1** UART, not RTT:

```
/dev/cu.usbmodem<serial>3
```

(the nRF54L15 DK's on-board debugger exposes several `cu.usbmodem*` ports;
VCOM1 is the third — `...3` suffix). 115200 8N1. `screen`, `minicom`, or any
terminal program that leaves termios alone will show the log lines directly:

```
screen /dev/cu.usbmodem<serial>3 115200
```

## Bounded capture on this Mac

This Mac has no `timeout` binary, and opening `/dev/cu.usbmodem*` resets its
termios settings — so a bounded capture needs a small Python script that opens
the port, configures it *after* opening, and stops after a wall-clock deadline
rather than relying on an external timeout wrapper:

```python
#!/usr/bin/env python3
import sys, termios, time, os

port = sys.argv[1]                 # e.g. /dev/cu.usbmodem1234563
seconds = float(sys.argv[2])       # capture duration

fd = os.open(port, os.O_RDONLY | os.O_NOCTTY)
attrs = termios.tcgetattr(fd)
attrs[4] = attrs[5] = termios.B115200      # ispeed, ospeed
termios.tcsetattr(fd, termios.TCSANOW, attrs)   # set AFTER opening - opening resets it

deadline = time.time() + seconds
with os.fdopen(fd, "rb", buffering=0) as f:
    while time.time() < deadline:
        chunk = f.read(4096)
        if chunk:
            sys.stdout.buffer.write(chunk)
            sys.stdout.flush()
```

Run it as:

```sh
python3 capture.py /dev/cu.usbmodem<serial>3 30 > observer.log
```

capturing 30 seconds of log lines to a file, then inspect it at leisure.

## Computing the advertising interval

Each log line looks like:

```
[00:00:12.345,000] <inf> uuid_observer: AA:BB:CC:DD:EE:FF (random) rssi -42 uuid 0F0E0D0C0B0A09080706050403020100
```

The interval between two consecutive lines carrying the **same** UUID (or the
same advertiser address, for a burst of the same command) is the advertiser's
real advertising interval — grep the log for the address or the UUID and diff
the bracketed uptimes. This is how the 187.5 ms (Mac) and ~35 ms (iPhone)
figures in the scan-reliability amendment were obtained: consecutive deltas
were steady to within a couple of ms.

A change in UUID marks exactly when a new command (a new `Send`) went on air;
counting how many scan-equivalent windows separate that change from the
previous one is how the amendment's "commands missed" observations were made.

**Check for dropped log lines before reading a gap as a missed advert.** The
observer uses deferred logging (`CONFIG_LOG_MODE_DEFERRED=y`, 8 KB buffer). An
iPhone at ~35 ms produces ~800 adverts per 30 s Send, and at that rate the log
backend can fall behind and discard messages - Zephyr then prints a line
reporting how many messages were dropped. Search the capture for `messages
dropped` first; if it appears, gaps in the timestamps near it say nothing about
the advertiser, and the interval must be computed from an undropped stretch.
