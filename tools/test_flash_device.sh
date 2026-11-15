#!/bin/sh
# Dry-run harness for tools/flash_device.sh. Never touches a device and never
# reads secrets/: every case uses a throwaway secrets directory (MFS_SECRETS_DIR)
# holding obviously fake keys, and its own TMPDIR so leftovers can be counted.
#
#   sh tools/test_flash_device.sh
#
# Needs the NCS v3.2.4 toolchain: the KMU key file the script writes is compared
# with generate_psa_key_attributes.py's output for a fake key, run with the
# toolchain's python3 directly (never through nrfutil, which logs its argv).

set -u

root="$(cd "$(dirname "$0")/.." && pwd)"
script="$root/tools/flash_device.sh"
work="$(mktemp -d "${TMPDIR:-/tmp}/mfs_test.XXXXXX")"
trap 'rm -rf "$work"' EXIT INT TERM

serial="MFS1-0000-00001"
device_id="FA4E0001"
# Unique per run, so a later grep of nrfutil's logs can only find this run's keys.
randomKey() { od -An -N32 -tx1 /dev/urandom | tr -d ' \n'; }
fake_secret="$(randomKey)"
fake_provision="$(randomKey)"
zero_key="0000000000000000000000000000000000000000000000000000000000000000"
# Slots from src/kmu_slots.hpp, the one definition flash_device.sh also reads.
slotOf() { sed -n "s/^[[:space:]]*constexpr uint8_t $1 { \\([0-9][0-9]*\\) };.*/\\1/p" "$root/src/kmu_slots.hpp"; }
secret_slot="$(slotOf M_SECRET_SLOT)"
provision_slot="$(slotOf M_PROVISION_SLOT)"
if [ -z "$secret_slot" ] || [ -z "$provision_slot" ]; then
  echo "Cannot read the KMU slots from src/kmu_slots.hpp."
  exit 1
fi

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

# Runs the script in the background, paused in its dry run at step $1, then sends
# signal $2; sets status. Job control (set -m) keeps SIGINT deliverable.
run_signalled()
{
  set -m
  env MFS_SECRETS_DIR="$work/secrets" TMPDIR="$work/tmp" MFS_DRY_RUN_PAUSE_AT="$1" sh "$script" "$serial" --dry-run > "$out" 2>&1 &
  pid=$!
  tries=0
  while ! grep -q "DRY-RUN: paused at $1" "$out" && [ "$tries" -lt 100 ]; do
    tries=$((tries + 1))
    sleep 0.1
  done
  kept="$(find "$work/tmp" -name kmu_keys.json | wc -l | tr -d ' ')"
  kept_batch="$(find "$work/tmp" -name batch.json | wc -l | tr -d ' ')"
  batch_leaks=0
  for file in $(find "$work/tmp" -name batch.json); do
    if grep -qi -e "$fake_secret" -e "$fake_provision" "$file"; then batch_leaks=1; fi
  done
  kill "-$2" "$pid"
  wait "$pid"
  status=$?
  set +m
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
# The batch operations appended, in order, as one space-separated line.
batch_ops() { grep '^DRY-RUN: nrfutil device .* --x-append-batch ' "$out" | awk '{ print $4 }' | tr '\n' ' ' | sed 's/ $//'; }
# The distinct batch paths named by the append and execute commands.
batch_paths()
{
  {
    sed -n 's/^DRY-RUN: nrfutil device .* --x-append-batch \([^ ]*\)$/\1/p' "$out"
    sed -n 's/^DRY-RUN: nrfutil device x-execute-batch --batch-path \([^ ]*\) .*/\1/p' "$out"
  } | sort -u
}
executes() { grep -c '^DRY-RUN: nrfutil device x-execute-batch' "$out"; }
leftovers() { find "$work/tmp" -mindepth 1 | wc -l | tr -d ' '; }
leaks_secret() { grep -qi -e "$fake_secret" -e "$fake_provision" "$out"; }

echo "Syntax"
if sh -n "$script" > "$out" 2>&1; then pass "sh -n flash_device.sh"; else fail "sh -n flash_device.sh"; fi

echo "Full sequence (locked)"
make_conf "$serial" "$fake_secret" "$fake_provision"
run sh "$script" "$serial" --dry-run
execute=$(line_of x-execute-batch)
verify=$(line_of protection-get)
last_append="$(grep -n '^DRY-RUN: nrfutil device .* --x-append-batch ' "$out" | tail -n 1 | cut -d: -f1)"
last="$(grep '^DRY-RUN: nrfutil device' "$out" | tail -n 1)"
if [ "$status" -eq 0 ]; then pass "exits 0"; else fail "exits 0 (got $status)"; fi
if [ "$(batch_ops)" = "recover program x-provision-keys protection-set reset" ]; then
  pass "batch is recover -> program -> x-provision-keys -> protection-set -> reset"
else
  fail "batch is recover -> program -> x-provision-keys -> protection-set -> reset (got '$(batch_ops)')"
fi
if grep -q '^DRY-RUN: nrfutil device recover --x-family nrf54l --x-append-batch [^ ]*$' "$out"; then pass "recover appended"; else fail "recover appended"; fi
if grep -q "^DRY-RUN: nrfutil device program --firmware [^ ]*/build/merged.hex --options chip_erase_mode=ERASE_NONE,verify=VERIFY_READ --x-family nrf54l --x-append-batch [^ ]*\$" "$out"; then
  pass "program merged.hex with ERASE_NONE and VERIFY_READ"
else
  fail "program merged.hex with ERASE_NONE and VERIFY_READ"
fi
if grep -q '^DRY-RUN: nrfutil device x-provision-keys --key-file [^ ]*/kmu_keys.json --x-family nrf54l --x-append-batch [^ ]*$' "$out"; then pass "x-provision-keys by key-file path"; else fail "x-provision-keys by key-file path"; fi
if grep -q '^DRY-RUN: nrfutil device protection-set All --x-family nrf54l --x-append-batch [^ ]*$' "$out"; then pass "protection-set All in the batch"; else fail "protection-set All in the batch"; fi
if grep -q '^DRY-RUN: nrfutil device reset --reset-kind RESET_PIN --x-family nrf54l --x-append-batch [^ ]*$' "$out"; then pass "reset RESET_PIN"; else fail "reset RESET_PIN"; fi
if [ "$(batch_paths | wc -l | tr -d ' ')" -eq 1 ]; then pass "one batch file throughout"; else fail "one batch file throughout ($(batch_paths | tr '\n' ' '))"; fi
case "$(batch_paths)" in
  "$work/tmp/"*) pass "batch file in the private temp directory" ;;
  *) fail "batch file in the private temp directory ($(batch_paths))" ;;
esac
if grep '^DRY-RUN: nrfutil device .* --x-append-batch ' "$out" | grep -q -e '--serial-number'; then fail "appends name no probe"; else pass "appends name no probe"; fi
if [ "$(executes)" -eq 1 ] && grep -q '^DRY-RUN: nrfutil device x-execute-batch --batch-path [^ ]* --serial-number 853003346$' "$out"; then
  pass "one x-execute-batch on the default J-Link 853003346"
else
  fail "one x-execute-batch on the default J-Link 853003346"
fi
if [ "$execute" -gt "${last_append:-0}" ] && [ "$verify" -gt "$execute" ]; then pass "appends -> x-execute-batch -> protection-get"; else fail "appends -> x-execute-batch -> protection-get"; fi
if [ "$(device_commands)" -eq 7 ]; then pass "no device command outside the batch but protection-get"; else fail "no device command outside the batch but protection-get ($(device_commands))"; fi
if [ "$last" = "DRY-RUN: nrfutil device protection-get --serial-number 853003346" ]; then pass "ends with protection-get"; else fail "ends with protection-get ($last)"; fi
if grep -q 'Debug port verified locked (protection-get: All)' "$out"; then pass "lock verified"; else fail "lock verified"; fi
if grep -q "Key file written: 2 keys, slots $secret_slot and $provision_slot" "$out"; then pass "key file holds both keys at the slots from kmu_slots.hpp"; else fail "key file holds both keys"; fi
if grep -q 'Derived conf: CONFIG_ALC_DEVICE_SERIAL CONFIG_MFS_DEVICE_ID$' "$out"; then pass "derived conf holds only serial and ID"; else fail "derived conf holds only serial and ID"; fi
if grep -q 'debug-unlocked.conf' "$out"; then fail "locked build must not use debug-unlocked.conf"; else pass "locked build omits debug-unlocked.conf"; fi
if grep -q 'Sync the clock from the app, then send one Disarm' "$out" && grep -q '500 ms on / 2500 ms off' "$out" && grep -q 'orange triple' "$out" &&
  grep -q 'orange 3 s warning at boot' "$out"; then
  pass "final end-to-end message says what success looks like"
else
  fail "final end-to-end message says what success looks like"
fi
if leaks_secret; then fail "output never shows a key"; else pass "output never shows a key"; fi
if [ "$(leftovers)" -eq 0 ]; then pass "key file and batch file removed"; else fail "key file and batch file removed ($(leftovers) left)"; fi
logged="$(grep -r -l -i -F -e "$fake_secret" -e "$fake_provision" "$HOME/.nrfutil" /opt/nordic/ncs/toolchains/*/nrfutil/home 2>/dev/null | wc -l | tr -d ' ')"
if [ "$logged" -eq 0 ]; then pass "no nrfutil log holds this run's keys"; else fail "no nrfutil log holds this run's keys ($logged files)"; fi

echo "KMU key file matches generate_psa_key_attributes.py"
toolchain_bin="$(nrfutil toolchain-manager env --ncs-version v3.2.4 --as-script sh | sed -n 's/^export PATH=\([^:]*\):.*/\1/p')"
generator="/opt/nordic/ncs/v3.2.4/nrf/scripts/generate_psa_key_attributes.py"
rm -f "$work/generated.json" "$work/written.json"
for pair in "$secret_slot $fake_secret" "$provision_slot $fake_provision"; do
  set -- $pair
  "$toolchain_bin/python3" "$generator" --usage SIGN --id "$1" --type HMAC --key-bits 256 --algorithm HMAC_SHA256 --location LOCATION_CRACEN_KMU \
    --persistence PERSISTENCE_DEFAULT --cracen-usage RAW --key "0x$2" --file "$work/generated.json" > /dev/null 2>&1
done
printf '%s\n%s\n' "$fake_secret" "$fake_provision" | sh "$root/tools/kmu_key_file.sh" "$work/written.json" "$secret_slot" "$provision_slot" > "$out" 2>&1
if [ -s "$work/generated.json" ] && [ "$(tr -d ' \n' < "$work/written.json")" = "$(tr -d ' \n' < "$work/generated.json")" ]; then
  pass "slots $secret_slot and $provision_slot identical to the generator's output"
else
  fail "slots $secret_slot and $provision_slot identical to the generator's output"
fi
if [ "$(ls -l "$work/written.json" | cut -c1-10)" = "-rw-------" ]; then pass "key file mode 600"; else fail "key file mode 600"; fi
if leaks_secret; then fail "key file writer prints no key"; else pass "key file writer prints no key"; fi
printf '%s\n' "$fake_secret" | sh "$root/tools/kmu_key_file.sh" "$work/short.json" "$secret_slot" "$provision_slot" > "$out" 2>&1
if [ $? -ne 0 ] && [ ! -e "$work/short.json" ]; then pass "missing key refused, nothing written"; else fail "missing key refused, nothing written"; fi
printf '%s\n%s\n' "$fake_secret" "xyz" | sh "$root/tools/kmu_key_file.sh" "$work/bad.json" "$secret_slot" "$provision_slot" > "$out" 2>&1
if [ $? -ne 0 ] && [ ! -e "$work/bad.json" ] && ! leaks_secret; then pass "malformed key refused, nothing written or shown"; else fail "malformed key refused"; fi
rm -f "$work/generated.json" "$work/written.json"

echo "Hub DK refused"
make_conf "$serial" "$fake_secret" "$fake_provision"
run env MFS_DEV_ID=1057733814 sh "$script" "$serial" --dry-run
if [ "$status" -ne 0 ] && [ "$(device_commands)" -eq 0 ] && grep -q '1057733814' "$out"; then pass "MFS_DEV_ID=1057733814 refused before any device command"; else fail "MFS_DEV_ID=1057733814 refused"; fi

for padded in 001057733814 01057733814; do
  run env MFS_DEV_ID=$padded sh "$script" "$serial" --dry-run
  if [ "$status" -ne 0 ] && [ "$(device_commands)" -eq 0 ]; then pass "MFS_DEV_ID=$padded refused"; else fail "MFS_DEV_ID=$padded refused"; fi
done
run env MFS_DEV_ID=000 sh "$script" "$serial" --dry-run
if [ "$status" -ne 0 ] && [ "$(device_commands)" -eq 0 ]; then pass "all-zero MFS_DEV_ID refused"; else fail "all-zero MFS_DEV_ID refused"; fi

echo "Key-test DK allowed"
run env MFS_DEV_ID=1057755043 sh "$script" "$serial" --dry-run
if [ "$status" -eq 0 ] && grep -q '^DRY-RUN: nrfutil device x-execute-batch --batch-path [^ ]* --serial-number 1057755043$' "$out" &&
  grep -q '^DRY-RUN: nrfutil device protection-get --serial-number 1057755043$' "$out"; then
  pass "MFS_DEV_ID=1057755043 used"
else
  fail "MFS_DEV_ID=1057755043 used"
fi
run env MFS_DEV_ID=001057755043 sh "$script" "$serial" --dry-run
if [ "$status" -eq 0 ] && grep -q '^DRY-RUN: nrfutil device x-execute-batch --batch-path [^ ]* --serial-number 1057755043$' "$out" && ! grep -q '00105775' "$out"; then
  pass "MFS_DEV_ID=001057755043 normalised to 1057755043"
else
  fail "MFS_DEV_ID=001057755043 normalised to 1057755043"
fi

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

make_conf "$serial" "$fake_secret" "$fake_provision"
echo "CONFIG_MFS_DEVICE_SECRET=\"$fake_secret\"" >> "$work/secrets/$serial.conf"
run sh "$script" "$serial" --dry-run
if [ "$status" -ne 0 ] && grep -q 'must set CONFIG_MFS_DEVICE_SECRET exactly once' "$out" && ! grep -q 'not a hex string' "$out"; then
  pass "duplicate secret refused with its own message"
else
  fail "duplicate secret refused with its own message"
fi
if [ "$(device_commands)" -eq 0 ] && ! leaks_secret; then pass "duplicate refusal shows no key"; else fail "duplicate refusal shows no key"; fi

echo "Forced failure building the batch"
make_conf "$serial" "$fake_secret" "$fake_provision"
run env MFS_DRY_RUN_FAIL_AT=append sh "$script" "$serial" --dry-run
if [ "$status" -ne 0 ] && [ "$(executes)" -eq 0 ] && [ "$(line_of protection-get)" -eq 0 ] && grep -q 'Building the device batch failed' "$out"; then
  pass "stops before x-execute-batch with its own message"
else
  fail "stops before x-execute-batch with its own message"
fi
if [ "$(leftovers)" -eq 0 ]; then pass "key file and batch file gone"; else fail "key file and batch file gone ($(leftovers) left)"; fi

echo "Forced failure at the batch"
run env MFS_DRY_RUN_FAIL_AT=batch sh "$script" "$serial" --dry-run
if [ "$status" -ne 0 ] && [ "$(executes)" -eq 1 ] && [ "$(line_of protection-get)" -eq 0 ] && ! grep -q 'Sync the clock' "$out"; then
  pass "stops after x-execute-batch, no protection-get"
else
  fail "stops after x-execute-batch, no protection-get"
fi
if grep -q 'power-cycling every ~11 s' "$out" && grep -q 'debug port re-locks on every cold boot' "$out" && grep -q 'Re-run this script' "$out"; then
  pass "says the chip may be erased and power-cycling, and to re-run"
else
  fail "says the chip may be erased and power-cycling, and to re-run"
fi
if [ "$(leftovers)" -eq 0 ]; then pass "key file and batch file gone"; else fail "key file and batch file gone ($(leftovers) left)"; fi
if leaks_secret; then fail "batch failure shows no key"; else pass "batch failure shows no key"; fi

echo "Forced failure at protection-get"
run env MFS_DRY_RUN_FAIL_AT=protection-get sh "$script" "$serial" --dry-run
if [ "$status" -ne 0 ] && grep -q 'lock is unverified' "$out" && ! grep -q 'Sync the clock' "$out" && [ "$(leftovers)" -eq 0 ]; then
  pass "fails loudly, lock unverified"
else
  fail "fails loudly, lock unverified"
fi

echo "protection-get does not report All"
for reported in Secure None; do
  run env MFS_DRY_RUN_PROTECTION=$reported sh "$script" "$serial" --dry-run
  if [ "$status" -ne 0 ] && grep -q "DEBUG PORT NOT LOCKED: protection-get reports '$reported'" "$out" && ! grep -q 'Sync the clock' "$out"; then
    pass "$reported fails loudly"
  else
    fail "$reported fails loudly (exit $status)"
  fi
done

echo "protection-get output parsing"
# Runs with protection-get printing file $1; expects status $2 (or FAIL for fail closed).
parse_case()
{
  run env MFS_DRY_RUN_PROTECTION_FILE="$1" sh "$script" "$serial" --dry-run
  if [ "$2" = "All" ]; then
    if [ "$status" -eq 0 ] && grep -q 'Debug port verified locked (protection-get: All)' "$out"; then pass "$3"; else fail "$3 (exit $status)"; fi
  else
    if [ "$status" -ne 0 ] && grep -q "DEBUG PORT NOT LOCKED: protection-get reports '$2'" "$out" && ! grep -q 'Sync the clock' "$out"; then
      pass "$3"
    else
      fail "$3 (exit $status)"
    fi
  fi
}
printf 'access status: Debug access is enabled (status value: None)\n' > "$work/protection_unlocked.txt"
parse_case "$work/protection_unlocked.txt" None "real unlocked output (2026-11-15) parses as None"
printf '[00:00:00] Some probe warning\naccess status: Debug access is disabled (status value: All)\n' > "$work/protection_locked.txt"
parse_case "$work/protection_locked.txt" All "status value All parses as locked"
printf 'WARNING: All access ports could not be enumerated\naccess status: Debug access is enabled (status value: None)\n' > "$work/protection_stray.txt"
parse_case "$work/protection_stray.txt" None "a stray All in a warning does not count as locked"
printf 'protection status: All\nAll\n' > "$work/protection_missing.txt"
parse_case "$work/protection_missing.txt" unparsed "no access status line fails closed"
printf 'access status: Debug access is disabled (status value: All)\naccess status: Debug access is enabled (status value: None)\n' > "$work/protection_two.txt"
parse_case "$work/protection_two.txt" unparsed "two access status lines fail closed"
printf 'access status: Debug access is odd (status value: AllBut)\n' > "$work/protection_odd.txt"
parse_case "$work/protection_odd.txt" unknown "an unknown status value fails closed"
: > "$work/protection_empty.txt"
parse_case "$work/protection_empty.txt" unparsed "empty output fails closed"

echo "Signals clean up"
for signal in TERM INT HUP; do
  run_signalled batch "$signal"
  if [ "$kept" -eq 1 ] && [ "$kept_batch" -eq 1 ] && [ "$batch_leaks" -eq 0 ] && [ "$status" -ne 0 ] && [ "$(leftovers)" -eq 0 ] && [ "$(executes)" -eq 0 ]; then
    pass "SIG$signal before x-execute-batch: key file and batch file existed (no key in the batch), then removed (exit $status)"
  else
    fail "SIG$signal before x-execute-batch: key file ($kept) and batch file ($kept_batch, leaks $batch_leaks) existed, then removed (exit $status, $(leftovers) left)"
  fi
done

echo "--dry-run needs a throwaway secrets directory"
run env -u MFS_SECRETS_DIR sh "$script" "$serial" --dry-run
if [ "$status" -ne 0 ] && grep -q 'MFS_SECRETS_DIR' "$out"; then pass "dry run without MFS_SECRETS_DIR refused"; else fail "dry run without MFS_SECRETS_DIR refused"; fi

echo "--unlocked"
run sh "$script" "$serial" --unlocked --dry-run
if [ "$status" -eq 0 ] && [ "$(batch_ops)" = "recover program x-provision-keys reset" ] && [ "$(executes)" -eq 1 ]; then
  pass "batch is recover -> program -> x-provision-keys -> reset, no protection-set"
else
  fail "batch is recover -> program -> x-provision-keys -> reset, no protection-set (got '$(batch_ops)')"
fi
if [ "$(leftovers)" -eq 0 ]; then pass "key file and batch file removed"; else fail "key file and batch file removed"; fi
if [ "$(line_of protection-get)" -eq 0 ]; then pass "no lock verification"; else fail "no lock verification"; fi
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
