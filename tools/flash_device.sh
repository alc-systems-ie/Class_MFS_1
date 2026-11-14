#!/bin/sh
# Builds MFS_1 with one device's credentials and flashes it.
#
#   tools/flash_device.sh MFS1-1126-00003            build + flash, KEEPING NVS
#   tools/flash_device.sh MFS1-1126-00003 --wipe     build + flash --recover (erases NVS)
#   tools/flash_device.sh MFS1-1126-00003 --build    build only
#
# KEEP NVS by default (owner, 2026-11-14). NVS holds each engineer slot's next
# sequence number and the day floor. Erasing it restarts the device at 0 while
# the app's counters carry on, so after more than 16 of today's Sends the
# device ignores that slot until 04:00 UTC. Use --wipe only for a new device,
# a corrupt access record, or a deliberately fresh start - and expect the
# counter mismatch (orange rapid flash) for slots already used today.
#
# Credentials come from secrets/<serial>.conf (tools/gen_fleet_credentials.py).
# The J-Link defaults to the MFS bench probe; override with MFS_DEV_ID=<serial>.
# After flashing, provision the clock - it is invalid after every boot.

set -eu

if [ $# -lt 1 ]; then
  echo "Usage: $0 <device serial> [--build]" >&2
  exit 2
fi

serial="$1"
mode="${2:-flash}"
root="$(cd "$(dirname "$0")/.." && pwd)"
conf="$root/secrets/$serial.conf"
dev_id="${MFS_DEV_ID:-853003346}"
ncs="nrfutil toolchain-manager launch --ncs-version v3.2.4 --"

if [ ! -f "$conf" ]; then
  echo "No credentials for $serial: $conf not found." >&2
  echo "Known devices:" >&2
  ls "$root/secrets" 2>/dev/null | sed -n 's/\.conf$//p' | sed 's/^/  /' >&2
  exit 1
fi

cd "$root"
echo "Building $serial..."
$ncs west build -b nrf54l15dk/nrf54l05/cpuapp -p always -- -DEXTRA_CONF_FILE="$conf"

# Refuse to flash an image that does not carry the requested serial.
if ! grep -qx "CONFIG_ALC_DEVICE_SERIAL=\"$serial\"" build/class_mfs_1/zephyr/.config; then
  echo "Built image does not carry serial $serial - not flashing." >&2
  exit 1
fi
echo "Image built for $serial."

case "$mode" in
  --build)
    exit 0
    ;;
  --wipe)
    echo "Flashing $serial to J-Link $dev_id with --recover (ERASES NVS: sequence counters and day floor)..."
    $ncs west flash --recover --dev-id "$dev_id"
    ;;
  flash)
    # Erases only the pages the image touches, so the settings partition (NVS)
    # survives. Another serial's NVS does not belong to this device's keys:
    # use --wipe when changing a board's serial.
    echo "Flashing $serial to J-Link $dev_id, keeping NVS..."
    $ncs west flash --dev-id "$dev_id"
    ;;
  *)
    echo "Unknown option $mode (use --build or --wipe)." >&2
    exit 2
    ;;
esac
echo "Flashed $serial. Provision the clock next."
