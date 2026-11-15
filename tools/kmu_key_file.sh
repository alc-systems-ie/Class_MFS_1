#!/bin/sh
# Writes an nRF Util KMU key file for MFS_1's long-term HMAC keys, reading the
# keys from standard input so that no key is ever on a command line.
#
#   printf '%s\n%s\n' "$secret" "$provision" | tools/kmu_key_file.sh <out.json> <slot> <slot>
#
# One key per slot, in order: 64 hex characters (32 bytes) each. The output is
# created mode 600 and must not already exist. Nothing is printed except errors,
# which never contain a key. Used by tools/flash_device.sh.
#
# Why not generate_psa_key_attributes.py: it takes the key as an argument, and
# run through `nrfutil toolchain-manager launch` that argument is logged in plain
# text to ~/.nrfutil/logs. The file format is the generator's own (indent 4):
#     {"version": 0, "keyslots": [{"metadata": "0x...", "value": "0x<key>"}, ...]}
#
# Metadata: PSA attributes for a 256-bit HMAC key, algorithm HMAC_SHA256, usage
# SIGN, LOCATION_CRACEN_KMU, PERSISTENCE_DEFAULT, CRACEN usage RAW. Derived on
# 2026-11-15 by running, with an obviously fake key, for slots 7, 100-103:
#     generate_psa_key_attributes.py --usage SIGN --id <slot> --type HMAC --key-bits 256 \
#       --algorithm HMAC_SHA256 --location LOCATION_CRACEN_KMU \
#       --persistence PERSISTENCE_DEFAULT --cracen-usage RAW --key 0xabab...ab
# giving 0x00110001014B4E80001400000900800300000000 <slot> 30FF7F00000000: the key
# ID 0x7FFF30<slot>, little-endian, is the only part that varies.
# tools/test_flash_device.sh checks this file against the generator's output.

set -eu
umask 077

M_METADATA_HEAD="0x00110001014B4E80001400000900800300000000"
M_METADATA_TAIL="30FF7F00000000"
M_KEY_HEX_LENGTH=64
M_MAX_SLOT=255

die()
{
  echo "kmu_key_file.sh: $*" >&2
  exit 1
}

[ $# -ge 2 ] || die "usage: <out.json> <slot> [<slot>...], keys on standard input."
out="$1"
shift
[ ! -e "$out" ] && [ ! -L "$out" ] || die "$out already exists."

json='{
    "version": 0,
    "keyslots": ['
separator=""
index=0
for slot in "$@"; do
  index=$((index + 1))
  case "$slot" in
    "" | *[!0-9]* | 0?*) die "slot '$slot' is not a plain decimal number." ;;
  esac
  [ "$slot" -le "$M_MAX_SLOT" ] || die "slot $slot is above $M_MAX_SLOT."
  IFS= read -r key || die "no key on standard input for slot $slot (key $index)."
  key="$(printf '%s' "$key" | tr 'A-F' 'a-f')"
  case "$key" in
    "" | *[!0-9a-f]*) die "key $index is not a hex string." ;;
  esac
  [ "${#key}" -eq "$M_KEY_HEX_LENGTH" ] || die "key $index must be $M_KEY_HEX_LENGTH hex characters."
  metadata="$M_METADATA_HEAD$(printf '%02X' "$slot")$M_METADATA_TAIL"
  json="$json$separator
        {
            \"metadata\": \"$metadata\",
            \"value\": \"0x$key\"
        }"
  separator=","
done
json="$json
    ]
}"

# Written only once every key has passed, so a refusal leaves no file.
printf '%s\n' "$json" > "$out"
chmod 600 "$out"
