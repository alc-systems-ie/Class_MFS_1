#!/bin/sh
# Dry-run harness for tools/flash_device.sh. Never touches a device and never
# reads secrets/: every case uses a throwaway secrets directory (MFS_SECRETS_DIR)
# holding obviously fake keys, and its own TMPDIR so leftovers can be counted.
#
#   sh tools/test_flash_device.sh
#
# Needs the NCS v3.2.4 toolchain (nrfutil toolchain-manager), because the dry run
# still writes the KMU key file with generate_psa_key_attributes.py - a host step.

set -u

root="$(cd "$(dirname "$0")/.." && pwd)"
script="$root/tools/flash_device.sh"
work="$(mktemp -d "${TMPDIR:-/tmp}/mfs_test.XXXXXX")"
trap 'rm -rf "$work"' EXIT INT TERM

serial="MFS1-0000-00001"
device_id="FA4E0001"
fake_secret="1111111111111111111111111111111111111111111111111111111111111111"
fake_provision="2222222222222222222222222222222222222222222222222222222222222222"
zero_key="0000000000000000000000000000000000000000000000000000000000000000"

passed=0
failed=0
out="$work/out.txt"

pass() { passed=$((passed + 1)); echo "  PASS: $1"; }
fail() { failed=$((failed + 1)); echo "  FAIL: $1"; sed 's/^/      | /' "$out"; }

# Writes secrets/<serial>.conf in a fresh secrets directory: <serial> <secret> <provision>.
make_conf()
{
  rm -rf "$work/secrets" "$work/tmp"
  mkdir -p "$work/secrets" "$work/tmp"
  {
    echo "# FAKE test credentials - not a real device."
    echo "CONFIG_ALC_DEVICE_SERIAL=\"$1\""
    echo "CONFIG_MFS_DEVICE_ID=\"$device_id\""
    echo "CONFIG_MFS_DEVICE_SECRET=\"$2\""
    echo "CONFIG_MFS_PROVISION_KEY=\"$3\""
  } > "$work/secrets/$serial.conf"
}

# Runs the script in dry-run mode; sets status. Extra environment via env(1) args.
run()
{
  env MFS_SECRETS_DIR="$work/secrets" TMPDIR="$work/tmp" "$@" > "$out" 2>&1
  status=$?
}

# Line number of the first dry-run device command matching $1 (0 if none).
line_of()
{
  number="$(grep -n "^DRY-RUN: nrfutil device $1" "$out" | head -n 1 | cut -d: -f1)"
  echo "${number:-0}"
}

device_commands() { grep -c '^DRY-RUN: nrfutil device' "$out"; }
leftovers() { find "$work/tmp" -mindepth 1 | wc -l | tr -d ' '; }
leaks_secret() { grep -qi -e "$fake_secret" -e "$fake_provision" "$out"; }

echo "Syntax"
if sh -n "$script" > "$out" 2>&1; then pass "sh -n flash_device.sh"; else fail "sh -n flash_device.sh"; fi

echo "Full sequence (locked)"
make_conf "$serial" "$fake_secret" "$fake_provision"
run sh "$script" "$serial" --dry-run
recover=$(line_of recover)
provision=$(line_of x-provision-keys)
program=$(line_of program)
protect=$(line_of "protection-set All")
if [ "$status" -eq 0 ]; then pass "exits 0"; else fail "exits 0 (got $status)"; fi
if [ "$recover" -gt 0 ] && [ "$provision" -gt "$recover" ] && [ "$program" -gt "$provision" ] && [ "$protect" -gt "$program" ]; then
  pass "order recover -> x-provision-keys -> program -> protection-set All"
else
  fail "order recover -> x-provision-keys -> program -> protection-set All ($recover $provision $program $protect)"
fi
if grep -q '^DRY-RUN: nrfutil device program .*chip_erase_mode=ERASE_RANGES_TOUCHED_BY_FIRMWARE,' "$out"; then pass "program erases only the image ranges"; else fail "program erases only the image ranges"; fi
if grep -q '^DRY-RUN: nrfutil device .*--serial-number 853003346' "$out"; then pass "default J-Link 853003346"; else fail "default J-Link 853003346"; fi
if grep -q 'Key file written: 2 keys, slots 100 and 102' "$out"; then pass "key file holds both keys at the slots from kmu_slots.hpp"; else fail "key file holds both keys"; fi
if grep -q 'Derived conf: CONFIG_ALC_DEVICE_SERIAL CONFIG_MFS_DEVICE_ID$' "$out"; then pass "derived conf holds only serial and ID"; else fail "derived conf holds only serial and ID"; fi
if grep -q 'debug-unlocked.conf' "$out"; then fail "locked build must not use debug-unlocked.conf"; else pass "locked build omits debug-unlocked.conf"; fi
if grep -q 'Sync the clock from the app, then send one Disarm' "$out"; then pass "final end-to-end message"; else fail "final end-to-end message"; fi
if leaks_secret; then fail "output never shows a key"; else pass "output never shows a key"; fi
if [ "$(leftovers)" -eq 0 ]; then pass "temp files removed"; else fail "temp files removed ($(leftovers) left)"; fi

echo "Hub DK refused"
make_conf "$serial" "$fake_secret" "$fake_provision"
run env MFS_DEV_ID=1057733814 sh "$script" "$serial" --dry-run
if [ "$status" -ne 0 ] && [ "$(device_commands)" -eq 0 ] && grep -q '1057733814' "$out"; then pass "MFS_DEV_ID=1057733814 refused before any device command"; else fail "MFS_DEV_ID=1057733814 refused"; fi

echo "Key-test DK allowed"
run env MFS_DEV_ID=1057755043 sh "$script" "$serial" --dry-run
if [ "$status" -eq 0 ] && grep -q '^DRY-RUN: nrfutil device recover --serial-number 1057755043' "$out"; then pass "MFS_DEV_ID=1057755043 used"; else fail "MFS_DEV_ID=1057755043 used"; fi

echo "Bad keys refused before any device command"
make_conf "$serial" "$fake_secret" "$fake_secret"
run sh "$script" "$serial" --dry-run
if [ "$status" -ne 0 ] && [ "$(device_commands)" -eq 0 ] && ! grep -q 'west build' "$out"; then pass "equal keys refused"; else fail "equal keys refused"; fi
if leaks_secret; then fail "equal-key refusal shows no key"; else pass "equal-key refusal shows no key"; fi
make_conf "$serial" "$zero_key" "$fake_provision"
run sh "$script" "$serial" --dry-run
if [ "$status" -ne 0 ] && [ "$(device_commands)" -eq 0 ]; then pass "all-zero secret refused"; else fail "all-zero secret refused"; fi
make_conf "$serial" "$fake_secret" "$zero_key"
run sh "$script" "$serial" --dry-run
if [ "$status" -ne 0 ] && [ "$(device_commands)" -eq 0 ]; then pass "all-zero provisioning key refused"; else fail "all-zero provisioning key refused"; fi
make_conf "$serial" "$fake_secret" "22222222"
run sh "$script" "$serial" --dry-run
if [ "$status" -ne 0 ] && [ "$(device_commands)" -eq 0 ]; then pass "short key refused"; else fail "short key refused"; fi
if [ "$(leftovers)" -eq 0 ]; then pass "temp files removed after refusal"; else fail "temp files removed after refusal"; fi

echo "Forced failure at provision"
make_conf "$serial" "$fake_secret" "$fake_provision"
run env MFS_DRY_RUN_FAIL_AT=provision sh "$script" "$serial" --dry-run
if [ "$status" -ne 0 ]; then pass "exits non-zero"; else fail "exits non-zero"; fi
if [ "$(line_of program)" -eq 0 ] && [ "$(line_of protection-set)" -eq 0 ]; then pass "nothing programmed or locked"; else fail "nothing programmed or locked"; fi
if [ "$(leftovers)" -eq 0 ]; then pass "key file gone"; else fail "key file gone ($(leftovers) left)"; fi

echo "Forced failure at program"
run env MFS_DRY_RUN_FAIL_AT=program sh "$script" "$serial" --dry-run
if [ "$status" -ne 0 ] && [ "$(line_of protection-set)" -eq 0 ] && [ "$(leftovers)" -eq 0 ]; then pass "stops, temp files gone"; else fail "stops, temp files gone"; fi

echo "--unlocked"
run sh "$script" "$serial" --unlocked --dry-run
if [ "$status" -eq 0 ] && [ "$(line_of protection-set)" -eq 0 ] && [ "$(line_of program)" -gt 0 ]; then pass "programs, no protection-set"; else fail "programs, no protection-set"; fi
if grep -q 'debug-unlocked.conf' "$out" && grep -q 'DEBUG PORT LEFT UNLOCKED' "$out"; then pass "adds debug-unlocked.conf and warns"; else fail "adds debug-unlocked.conf and warns"; fi

echo "--extra-conf"
echo 'CONFIG_MFS_SCAN_DIAG=y' > "$work/diag.conf"
run sh "$script" "$serial" --unlocked --extra-conf "$work/diag.conf" --dry-run
if [ "$status" -eq 0 ] && grep -q "diag.conf" "$out"; then pass "extra conf reaches the build"; else fail "extra conf reaches the build"; fi
echo 'CONFIG_MFS_DEVICE_SECRET=""' > "$work/bad.conf"
run sh "$script" "$serial" --extra-conf "$work/bad.conf" --dry-run
if [ "$status" -ne 0 ] && ! grep -q 'west build' "$out" && [ "$(device_commands)" -eq 0 ]; then pass "extra conf naming a secret refused"; else fail "extra conf naming a secret refused"; fi
echo 'CONFIG_MFS_PROVISION_KEY="AB"' > "$work/bad.conf"
run sh "$script" "$serial" --extra-conf "$work/bad.conf" --dry-run
if [ "$status" -ne 0 ] && [ "$(device_commands)" -eq 0 ]; then pass "extra conf naming the provisioning key refused"; else fail "extra conf naming the provisioning key refused"; fi

echo 'CONFIG_NRF_APPROTECT_DISABLE=y' > "$work/bad.conf"
run sh "$script" "$serial" --extra-conf "$work/bad.conf" --dry-run
if [ "$status" -ne 0 ] && ! grep -q 'west build' "$out"; then pass "extra conf touching the debug lock refused"; else fail "extra conf touching the debug lock refused"; fi

echo "--build"
run sh "$script" "$serial" --build --dry-run
if [ "$status" -eq 0 ] && [ "$(device_commands)" -eq 0 ] && grep -q 'west build' "$out"; then pass "builds only"; else fail "builds only"; fi

echo "Argument and conf checks"
run sh "$script" "$serial" --keep-nvs --dry-run
if [ "$status" -ne 0 ] && grep -q 'withdrawn' "$out"; then pass "--keep-nvs withdrawn"; else fail "--keep-nvs withdrawn"; fi
run sh "$script" "$serial" --bogus --dry-run
if [ "$status" -ne 0 ]; then pass "unknown option refused"; else fail "unknown option refused"; fi
run sh "$script" "mfs1-0000-1" --dry-run
if [ "$status" -ne 0 ] && grep -q 'not a CLASS serial' "$out"; then pass "malformed serial refused"; else fail "malformed serial refused"; fi
run sh "$script" "MFS1-0000-00002" --dry-run
if [ "$status" -ne 0 ] && grep -q 'not found' "$out"; then pass "missing conf refused"; else fail "missing conf refused"; fi
make_conf "MFS1-0000-00009" "$fake_secret" "$fake_provision"
run sh "$script" "$serial" --dry-run
if [ "$status" -ne 0 ] && [ "$(device_commands)" -eq 0 ]; then pass "conf for another serial refused"; else fail "conf for another serial refused"; fi

echo
echo "$passed passed, $failed failed."
[ "$failed" -eq 0 ]
