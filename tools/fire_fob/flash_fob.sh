#!/bin/sh
# Generate today's fob keys, build, and flash the Thingy:53 fire fob - in one go, because
# the fob's clock starts at key generation (README.md). BENCH / DEMO ONLY.
#
#   FOB_DEV_ID=<J-Link serial> tools/fire_fob/flash_fob.sh MFS1-1126-00004 MFS1-1126-00006 MFS1-1126-00009
set -eu
# A stale FOB_KEYS_DIR export from a compile check (gen_fob_keys.py --fake) must never
# build a flashable fob with fake keys: this script always uses secrets/.
unset FOB_KEYS_DIR
M_HUB_PROBE=1057733814
cd "$(dirname "$0")"
[ $# -ge 1 ] || { echo "usage: FOB_DEV_ID=<J-Link> $0 <serial>..." >&2; exit 2; }
[ -n "${FOB_DEV_ID:-}" ] || { echo "Set FOB_DEV_ID to the J-Link wired to the Thingy:53's debug connector." >&2; exit 2; }
[ "$FOB_DEV_ID" != "$M_HUB_PROBE" ] || { echo "Refusing the live hub DK's probe $M_HUB_PROBE." >&2; exit 2; }

python3 gen_fob_keys.py "$@"
nrfutil toolchain-manager launch --ncs-version v3.2.4 -- west build -b thingy53/nrf5340/cpuapp -d build
nrfutil toolchain-manager launch --ncs-version v3.2.4 -- west flash -d build --dev-id "$FOB_DEV_ID"
echo "Fob flashed. Expect one long GREEN flash at boot (ready); RED triple means see the RTT log."
