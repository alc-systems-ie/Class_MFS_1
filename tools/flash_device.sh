#!/bin/sh
# Builds MFS_1 for one device, provisions its two long-term keys into the
# nRF54L05 KMU, programs it and locks the debug port.
#
#   tools/flash_device.sh <serial>                         erase, program, provision, lock
#   tools/flash_device.sh <serial> --unlocked              same, debug port left open (BENCH ONLY)
#   tools/flash_device.sh <serial> --unlocked --extra-conf <file>   bench/diagnostic build
#   tools/flash_device.sh <serial> --build                 build and check only, no device
#   tools/flash_device.sh <serial> --dry-run               print every device command instead of running it
#
# RULE: secrets never reach west build; never pass a secret with -D. The build
# gets a derived conf holding only CONFIG_ALC_DEVICE_SERIAL and
# CONFIG_MFS_DEVICE_ID; the keys go only into a temporary KMU key file
# (mktemp, mode 600, removed on exit) and from there into the KMU. CMakeLists.txt
# fails any build that sets either key, and this script refuses to start one.
#
# Sequence (design 2026-11-15 section 5.5):
#   1. Validate the serial and secrets/<serial>.conf; refuse all-zero or equal keys.
#   2. Build from the derived conf; check the image carries the serial and ID,
#      the debug-lock settings, and neither key (.config, autoconf.h, hex files).
#   3. Write the KMU key file (tools/kmu_key_file.sh, keys on stdin, slots from
#      src/kmu_slots.hpp). No key is ever in the argv of anything, least of all
#      of nrfutil, which logs its argv to ~/.nrfutil/logs.
#   4-7. ONE nrfutil batch (--x-append-batch, then a single x-execute-batch on
#      the J-Link), as Zephyr's west nrfutil runner builds it: recover (unlocks
#      and ERASES EVERYTHING: KMU, NVS, image) -> program merged.hex
#      (chip_erase_mode=ERASE_NONE, as west uses for nRF54L - recover has just
#      erased it; verify=VERIFY_READ) -> x-provision-keys -> protection-set All
#      (left out with --unlocked) -> reset RESET_PIN. The key file and the batch
#      file (which names the key file by path, never a key) are deleted straight
#      after. One batch because separate calls fail on the MFS board: with the app
#      erased nothing stops the nPM2100 boot monitor, so the board power-cycles
#      every ~11 s, and each nRF54L cold boot re-locks the debug port (2026-11-15).
#      A failed batch may leave the chip erased and power-cycling: re-run.
#   8. nrfutil device protection-get (skipped with --unlocked): the run fails
#      loudly unless its "access status" line says "status value: All".
#
# Every run erases NVS: the engineer slots' sequence counters and the day floor
# restart at 0 while the app's counters carry on, so a slot with more than 16
# Sends today sees the counter mismatch (orange rapid flash) until 04:00 UTC.
# --keep-nvs is withdrawn.
#
# Credentials come from secrets/<serial>.conf (tools/gen_fleet_credentials.py);
# MFS_SECRETS_DIR overrides the directory (tools/test_flash_device.sh only).
# The J-Link defaults to the MFS bench probe 853003346; MFS_DEV_ID overrides it
# (leading zeros, as nrfutil device list shows them, are stripped). J-Link
# 1057733814 (the live hub DK) is always refused.
# --dry-run exists for tools/test_flash_device.sh and requires MFS_SECRETS_DIR,
# so a dry run never handles real keys, and runs no nrfutil at all: it prints
# every batch append and the x-execute-batch, and records the appends in the
# batch file as text. MFS_DRY_RUN_FAIL_AT=<steps> (any of build, keyfile,
# append, batch, protection-get, space-separated) fails those steps;
# MFS_DRY_RUN_PROTECTION=<status> is the status value a dry-run protection-get
# reports (default All), MFS_DRY_RUN_PROTECTION_FILE=<file> its whole output;
# MFS_DRY_RUN_PAUSE_AT=<device step> (append, batch) waits there so a signal
# can be sent.
#
# Never add set -x: it would trace the key file step's printf arguments, which are the keys.

set -eu
umask 077

M_DEFAULT_DEV_ID="853003346"
M_HUB_DEV_ID="1057733814"
M_BOARD="nrf54l15dk/nrf54l05/cpuapp"
M_KEY_HEX_LENGTH=64
M_ID_HEX_LENGTH=8
ncs="nrfutil toolchain-manager launch --ncs-version v3.2.4 --"

die()
{
  echo "$*" >&2
  exit 1
}

usage()
{
  echo "Usage: $0 <device serial> [--build] [--unlocked] [--extra-conf <file>] [--dry-run]" >&2
  exit 2
}

# ---- Arguments -------------------------------------------------------------

[ $# -ge 1 ] || usage
serial="$1"
shift
build_only=0
unlocked=0
dry_run=0
extra_conf=""
while [ $# -gt 0 ]; do
  case "$1" in
    --build) build_only=1 ;;
    --unlocked) unlocked=1 ;;
    --dry-run) dry_run=1 ;;
    --extra-conf)
      [ $# -ge 2 ] || usage
      extra_conf="$2"
      shift
      ;;
    --keep-nvs)
      die "--keep-nvs is withdrawn: every flash recovers the chip (erasing NVS and the KMU) and provisions it afresh."
      ;;
    *)
      echo "Unknown option $1." >&2
      usage
      ;;
  esac
  shift
done

case "$serial" in
  [A-Z0-9][A-Z0-9][A-Z0-9][A-Z0-9]-[0-9][0-9][0-9][0-9]-[0-9A-F][0-9A-F][0-9A-F][0-9A-F][0-9A-F]) ;;
  *) die "$serial is not a CLASS serial (MODEL-MMYY-NNNNN, e.g. MFS1-1126-00003)." ;;
esac

if [ "$dry_run" -eq 1 ] && [ -z "${MFS_SECRETS_DIR:-}" ]; then
  die "--dry-run requires MFS_SECRETS_DIR (a throwaway secrets directory with fake keys): a dry run never handles real keys."
fi

raw_dev_id="${MFS_DEV_ID:-$M_DEFAULT_DEV_ID}"
case "$raw_dev_id" in
  "" | *[!0-9]*) die "MFS_DEV_ID must be a J-Link serial number (digits only)." ;;
esac
# Normalised: nrfutil lists J-Link serials zero-padded to 12 digits.
dev_id="$(printf '%s' "$raw_dev_id" | sed 's/^0*//')"
[ -n "$dev_id" ] || die "MFS_DEV_ID $raw_dev_id is not a J-Link serial number."
[ "$dev_id" != "$M_HUB_DEV_ID" ] || die "Refusing J-Link $M_HUB_DEV_ID: that is the live hub DK, never an MFS target."

root="$(cd "$(dirname "$0")/.." && pwd)"
secrets_dir="${MFS_SECRETS_DIR:-$root/secrets}"
conf="$secrets_dir/$serial.conf"
build_dir="$root/build"
dotconfig="$build_dir/class_mfs_1/zephyr/.config"
autoconf="$build_dir/class_mfs_1/zephyr/include/generated/zephyr/autoconf.h"
app_hex="$build_dir/class_mfs_1/zephyr/zephyr.hex"
merged_hex="$build_dir/merged.hex"

if [ ! -f "$conf" ]; then
  echo "No credentials for $serial: $conf not found." >&2
  echo "Known devices:" >&2
  ls "$secrets_dir" 2>/dev/null | sed -n 's/\.conf$//p' | sed 's/^/  /' >&2
  exit 1
fi

if [ -n "$extra_conf" ]; then
  [ -f "$extra_conf" ] || die "Extra conf $extra_conf not found."
  extra_conf="$(cd "$(dirname "$extra_conf")" && pwd)/$(basename "$extra_conf")"
  case "$extra_conf" in *";"*) die "Extra conf path must not contain ';'." ;; esac
fi

# ---- Temporary files: one private directory, removed on every exit ----------

work="$(mktemp -d "${TMPDIR:-/tmp}/mfs_flash.XXXXXX")"
chmod 700 "$work"
cleanup()
{
  rm -rf "$work"
}
trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM
trap 'exit 129' HUP

# In a dry run, fails the named step when MFS_DRY_RUN_FAIL_AT names it.
dryRunFailAt()
{
  if [ "$dry_run" -eq 1 ]; then
    case " ${MFS_DRY_RUN_FAIL_AT:-} " in
      *" $1 "*)
        echo "DRY-RUN: forced failure at $1." >&2
        return 1
        ;;
    esac
  fi
  return 0
}

# device <step> <nrfutil device arguments...>: runs it, or prints it in a dry run.
device()
{
  step="$1"
  shift
  if [ "$dry_run" -eq 1 ]; then
    if [ "${MFS_DRY_RUN_PAUSE_AT:-}" = "$step" ]; then
      echo "DRY-RUN: paused at $step."
      sleep 30 &
      wait $!
    fi
    echo "DRY-RUN: nrfutil device $*"
    dryRunFailAt "$step"
    return $?
  fi
  nrfutil device "$@"
}

# ---- 1. Credentials ----------------------------------------------------------

# Prints the quoted value of symbol $1 in the conf; it must be set exactly once.
confValue()
{
  count="$(grep -c "^$1=" "$conf" || true)"
  [ "$count" -eq 1 ] || die "$conf must set $1 exactly once."
  sed -n "s/^$1=\"\\(.*\\)\"\$/\\1/p" "$conf"
}

# Lower-cases a key without putting it on a command line.
lowerHex()
{
  printf '%s' "$1" | tr 'A-F' 'a-f'
}

# Checks key $1 (named $2) is 64 hex characters and not all zero. Never prints it.
checkKey()
{
  case "$1" in
    "" | *[!0-9a-f]*) die "$2 in $conf is not a hex string." ;;
  esac
  [ "${#1}" -eq "$M_KEY_HEX_LENGTH" ] || die "$2 in $conf must be $M_KEY_HEX_LENGTH hex characters (32 bytes)."
  case "$1" in
    *[!0]*) ;;
    *) die "$2 in $conf is all zero - refusing it." ;;
  esac
}

conf_serial="$(confValue CONFIG_ALC_DEVICE_SERIAL)"
[ "$conf_serial" = "$serial" ] || die "$conf is for serial '$conf_serial', not $serial."

device_id="$(confValue CONFIG_MFS_DEVICE_ID)"
case "$device_id" in
  "" | *[!0-9A-Fa-f]*) die "CONFIG_MFS_DEVICE_ID in $conf is not a hex string." ;;
esac
[ "${#device_id}" -eq "$M_ID_HEX_LENGTH" ] || die "CONFIG_MFS_DEVICE_ID in $conf must be $M_ID_HEX_LENGTH hex characters."

# Read raw first: a die inside a nested substitution would only leave the subshell.
secret_raw="$(confValue CONFIG_MFS_DEVICE_SECRET)"
provision_raw="$(confValue CONFIG_MFS_PROVISION_KEY)"
secret="$(lowerHex "$secret_raw")"
provision="$(lowerHex "$provision_raw")"
checkKey "$secret" CONFIG_MFS_DEVICE_SECRET
checkKey "$provision" CONFIG_MFS_PROVISION_KEY
[ "$secret" != "$provision" ] || die "The device secret and the provisioning key in $conf are equal - refusing them."

# Slots are defined once, in src/kmu_slots.hpp.
slotOf()
{
  value="$(sed -n "s/^[[:space:]]*constexpr uint8_t $1 { \\([0-9][0-9]*\\) };.*/\\1/p" "$root/src/kmu_slots.hpp")"
  case "$value" in
    "" | *[!0-9]*) die "Cannot read $1 from src/kmu_slots.hpp." ;;
  esac
  echo "$value"
}
secret_slot="$(slotOf M_SECRET_SLOT)"
provision_slot="$(slotOf M_PROVISION_SLOT)"

echo "Credentials for $serial: device ID $device_id, keys valid (not shown). KMU slots $secret_slot and $provision_slot."

# ---- 2. Build ------------------------------------------------------------------

derived="$work/identity.conf"
printf 'CONFIG_ALC_DEVICE_SERIAL="%s"\nCONFIG_MFS_DEVICE_ID="%s"\n' "$serial" "$device_id" > "$derived"
chmod 600 "$derived"
echo "Derived conf: $(sed 's/=.*//' "$derived" | tr '\n' ' ' | sed 's/ $//')"

conf_list="$derived"
if [ "$unlocked" -eq 1 ]; then
  conf_list="$conf_list;$root/debug-unlocked.conf"
fi
if [ -n "$extra_conf" ]; then
  conf_list="$conf_list;$extra_conf"
fi

# Refuse a build whose conf list names a key, before west build ever sees it.
old_ifs="$IFS"
IFS=';'
set -f
for listed in $conf_list; do
  [ -f "$listed" ] || die "Conf $listed not found."
  if [ -n "$(grep -v '^[[:space:]]*#' "$listed" | grep -e CONFIG_MFS_DEVICE_SECRET -e CONFIG_MFS_PROVISION_KEY || true)" ]; then
    die "Refusing to build: $listed names CONFIG_MFS_DEVICE_SECRET or CONFIG_MFS_PROVISION_KEY. Keys never reach a build."
  fi
done
set +f
IFS="$old_ifs"

# The debug-port lock is set only by prj.conf and debug-unlocked.conf.
if [ -n "$extra_conf" ] && [ -n "$(grep -v '^[[:space:]]*#' "$extra_conf" | grep -e APPROTECT -e MFS_DEBUG_UNLOCKED || true)" ]; then
  die "Refusing $extra_conf: it touches the debug-port lock. Use --unlocked for an unlocked bench build."
fi

if [ "$unlocked" -eq 1 ]; then
  echo "WARNING: --unlocked - DEBUG PORT LEFT UNLOCKED. Anyone with an SWD probe can read this device and use its keys. BENCH ONLY." >&2
fi

if [ "$dry_run" -eq 1 ]; then
  echo "DRY-RUN: $ncs west build -b $M_BOARD -p always -- -DEXTRA_CONF_FILE=$conf_list"
  dryRunFailAt build
else
  echo "Building $serial..."
  cd "$root"
  $ncs west build -b "$M_BOARD" -p always -- "-DEXTRA_CONF_FILE=$conf_list"
fi
rm -f "$derived"

# Prints the data bytes of Intel hex $1 as one line of lower-case hex.
hexBytes()
{
  awk '{ sub(/\r$/, "") } substr($0, 8, 2) == "00" { printf "%s", tolower(substr($0, 10, length($0) - 11)) } END { print "" }' "$1"
}

if [ "$dry_run" -eq 1 ]; then
  echo "DRY-RUN: would check the image carries $serial and $device_id, the debug-lock settings, and neither key."
else
  grep -qx "CONFIG_ALC_DEVICE_SERIAL=\"$serial\"" "$dotconfig" || die "Built image does not carry serial $serial - not flashing."
  grep -qx "CONFIG_MFS_DEVICE_ID=\"$device_id\"" "$dotconfig" || die "Built image does not carry device ID $device_id - not flashing."
  if [ "$unlocked" -eq 1 ]; then
    grep -qx 'CONFIG_MFS_DEBUG_UNLOCKED=y' "$dotconfig" || die "--unlocked build lacks CONFIG_MFS_DEBUG_UNLOCKED - not flashing."
  else
    if grep -qx 'CONFIG_MFS_DEBUG_UNLOCKED=y' "$dotconfig"; then die "Locked build has CONFIG_MFS_DEBUG_UNLOCKED - not flashing."; fi
    grep -qx 'CONFIG_NRF_APPROTECT_LOCK=y' "$dotconfig" || die "Build lacks CONFIG_NRF_APPROTECT_LOCK - not flashing."
    grep -qx 'CONFIG_NRF_SECURE_APPROTECT_LOCK=y' "$dotconfig" || die "Build lacks CONFIG_NRF_SECURE_APPROTECT_LOCK - not flashing."
  fi

  # Neither key may appear: as text in the configuration, or as raw bytes or
  # text in either hex. Patterns live in the private directory, never in argv.
  text_patterns="$work/text.patterns"
  byte_patterns="$work/byte.patterns"
  printf '%s\n%s\n' "$secret" "$provision" > "$text_patterns"
  {
    printf '%s\n%s\n' "$secret" "$provision"
    for key in "$secret" "$provision"; do
      printf '%s' "$key" | xxd -p | tr -d '\n'
      echo
      printf '%s' "$key" | tr 'a-f' 'A-F' | xxd -p | tr -d '\n'
      echo
    done
  } > "$byte_patterns"
  found=""
  for file in "$dotconfig" "$autoconf"; do
    [ -f "$file" ] || die "$file missing after the build."
    count="$(grep -c -i -F -f "$text_patterns" "$file" || true)"
    [ "$count" -eq 0 ] || found="$found $file"
  done
  for file in "$app_hex" "$merged_hex"; do
    [ -f "$file" ] || die "$file missing after the build."
    count="$(hexBytes "$file" | grep -c -F -f "$byte_patterns" || true)"
    [ "$count" -eq 0 ] || found="$found $file"
  done
  rm -f "$text_patterns" "$byte_patterns"
  [ -z "$found" ] || die "A key was found in:$found - not flashing. Delete $build_dir."

  # The image must not reach the configuration regions at 0x00FFxxxx (UICR,
  # SICR/KMU): programming there could overwrite the KMU or the lock.
  # RRAM ends below 0x00200000, so every extended address must be 0x0000-0x001F.
  if [ -n "$(grep -i '^:02000004' "$merged_hex" | grep -v -i '^:0200000400[01]' || true)" ]; then
    die "$merged_hex reaches beyond RRAM (towards UICR, SICR/KMU) - programming it could overwrite the KMU or UICR. Not flashing."
  fi
  echo "Image built for $serial: serial, ID and lock settings checked; neither key in .config, autoconf.h, zephyr.hex or merged.hex."
fi

[ "$build_only" -eq 0 ] || exit 0

# ---- 3. Key file -----------------------------------------------------------------

keyfile="$work/kmu_keys.json"

# Keys go to the writer on stdin (printf is a shell builtin), never in argv.
if ! printf '%s\n%s\n' "$secret" "$provision" | sh "$root/tools/kmu_key_file.sh" "$keyfile" "$secret_slot" "$provision_slot"; then
  die "Writing the KMU key file failed."
fi
chmod 600 "$keyfile"
key_count="$(grep -c '"metadata"' "$keyfile" || true)"
[ "$key_count" -eq 2 ] || die "KMU key file holds $key_count keys, expected 2."
dryRunFailAt keyfile
echo "Key file written: 2 keys, slots $secret_slot and $provision_slot."

# ---- 4-7. Device: one batch -------------------------------------------------------
#
# Recover, program, provision, lock and reset run as ONE nrfutil batch, built the
# way Zephyr's west nrfutil runner builds it (runners/nrfutil.py _append_batch,
# _exec_batch). Separate calls fail on the MFS board: once recover has erased the
# app nothing stops the nPM2100 boot monitor, so the board power-cycles every
# ~10.5-11 s, and every nRF54L cold boot re-locks the debug port unless running
# firmware opens it; a step that straddles a power cycle fails (2026-11-15). The
# batch holds one J-Link session from recover to reset, and the new app stops the
# boot monitor as soon as it boots. The batch file names the key file by PATH only
# (checked 2026-11-15 with a fake key); no key is ever in it.
#
# protection-set All is in the batch, before the reset, so the UICR lock is
# written before the new firmware ever runs. Appending names no probe: nrfutil
# records the operation and exits without touching a device.
batch="$work/batch.json"

# appendOp <nrfutil device arguments...>: appends one operation to the batch, or
# prints it (and records it in the batch file, as text) in a dry run.
appendOp()
{
  if [ "$dry_run" -eq 1 ]; then
    printf 'nrfutil device %s --x-family nrf54l --x-append-batch %s\n' "$*" "$batch" >> "$batch"
  fi
  device append "$@" --x-family nrf54l --x-append-batch "$batch"
}

# Removes the key file and the batch file (the EXIT trap also covers both).
removeBatchFiles()
{
  rm -f "$keyfile" "$batch"
}

# ERASE_NONE (west's choice for nRF54L, nrf_common.py program_hex): recover has
# just erased the chip and RRAM needs no page erase. The image check in step 2
# keeps the image out of the configuration regions (UICR, SICR/KMU).
if ! { appendOp recover &&
  appendOp program --firmware "$merged_hex" --options chip_erase_mode=ERASE_NONE,verify=VERIFY_READ &&
  appendOp x-provision-keys --key-file "$keyfile" &&
  { [ "$unlocked" -eq 1 ] || appendOp protection-set All; } &&
  appendOp reset --reset-kind RESET_PIN; }; then
  removeBatchFiles
  die "Building the device batch failed. Nothing has been done to the device; re-run this script."
fi

if [ "$unlocked" -eq 1 ]; then
  echo "WARNING: --unlocked - protection-set left out of the batch. DEBUG PORT LEFT UNLOCKED. Never deploy this device." >&2
fi

echo "Running the device batch on J-Link $dev_id: recover (ERASES the chip: KMU, NVS, image), program, provision the KMU$([ "$unlocked" -eq 1 ] || echo ', lock the debug port'), reset..."
if device batch x-execute-batch --batch-path "$batch" --serial-number "$dev_id"; then
  batch_ok=1
else
  batch_ok=0
fi
removeBatchFiles
[ "$batch_ok" -eq 1 ] ||
  die "The device batch failed. The chip may be erased and power-cycling every ~11 s (the nPM2100 boot monitor, with no app to stop it), and its debug port re-locks on every cold boot. Re-run this script: the batch starts with recover."

# Reads the access-port state into $work/protection.txt and prints it; returns
# non-zero if protection-get itself fails. On nRF54L it reports what the port
# allows now, not what protection-set asked for.
protection_file="$work/protection.txt"
protectionGet()
{
  if [ "$dry_run" -eq 1 ]; then
    echo "DRY-RUN: nrfutil device protection-get --serial-number $dev_id"
    dryRunFailAt protection-get || return 1
    if [ -n "${MFS_DRY_RUN_PROTECTION_FILE:-}" ]; then
      cat "$MFS_DRY_RUN_PROTECTION_FILE" > "$protection_file"
    else
      printf 'access status: Debug access (dry run) (status value: %s)\n' "${MFS_DRY_RUN_PROTECTION:-All}" > "$protection_file"
    fi
  else
    nrfutil device protection-get --serial-number "$dev_id" > "$protection_file" 2>&1 || {
      sed 's/^/  protection-get: /' "$protection_file" >&2
      return 1
    }
  fi
  sed 's/^/  protection-get: /' "$protection_file"
}

# The status value (None, Secure or All) from the one "access status" line, e.g.
#   access status: Debug access is enabled (status value: None)
# (real unlocked output, 2026-11-15). Prints "unparsed" unless exactly one such
# line is found, and "unknown" for any other value: both fail closed. Only that
# line counts, so a stray "All" elsewhere in the merged output never does.
protectionStatus()
{
  values="$(sed -n 's/^.*access status:.*(status value: \([^)]*\)).*$/\1/p' "$protection_file")"
  if [ -z "$values" ] || [ "$(printf '%s\n' "$values" | wc -l | tr -d ' ')" -ne 1 ]; then
    echo "unparsed"
    return
  fi
  case "$values" in
    None | Secure | All) echo "$values" ;;
    *) echo "unknown" ;;
  esac
}

if [ "$unlocked" -eq 0 ]; then
  echo "Verifying the lock..."
  protectionGet || die "DEBUG PORT NOT LOCKED? protection-get failed after the batch, so the lock is unverified. Run 'nrfutil device protection-get --serial-number $dev_id'; if it does not report 'status value: All', re-run this script. Never deploy an unverified device."
  status="$(protectionStatus)"
  [ "$status" = "All" ] ||
    die "DEBUG PORT NOT LOCKED: protection-get reports '$status' after the batch's protection-set All. Never deploy this device; re-run this script."
  echo "Debug port verified locked (protection-get: All)."
fi

echo "Flashed $serial (J-Link $dev_id)."
echo "Sync the clock from the app, then send one Disarm:"
echo "  - after the clock sync the green LED changes from steady to 500 ms on / 2500 ms off;"
echo "  - the Disarm gives the orange triple."
echo "An orange 3 s warning at boot (device keys missing or unusable), or no confirmation, means re-run this script."
