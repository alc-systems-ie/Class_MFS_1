# MFS_1 Keys in the KMU and Fleet Debug Lock Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** `deviceSecret` and `provisionKey` live in the nRF54L05 KMU (never in an image), every MFS build locks the debug port, and `tools/flash_device.sh <serial>` erases, provisions, flashes and locks one device.

**Architecture:** The crypto seam gains a `KeyRef` (raw bytes or a KMU slot). The access code takes `KeyRef`s for the two long-term keys; day keys stay raw. On target a KMU `KeyRef` is a PSA built-in key ID used in place; on the host a test KMU table maps slots to bytes. A pure, host-tested boot self-check replaces the Kconfig parse. The flash script derives a secrets-free build conf, writes a temporary KMU key file with Nordic's generator, and runs recover → provision → program → protection-set.

**Tech Stack:** NCS v3.2.4, C++20, PSA Crypto (CRACEN, `cracen_psa_kmu.h`), host `g++` + OpenSSL runner (`make test`), POSIX sh, `nrfutil device`, `nrf/scripts/generate_psa_key_attributes.py`.

**Spec:** `docs/superpowers/specs/2026-11-15-kmu-keys-and-debug-lock-design.md`

## Global Constraints

- KMU slots: `deviceSecret` **100–101**, `provisionKey` **102–103**; type HMAC, 256 bits, algorithm HMAC_SHA256, usage SIGN, **RAW** scheme, persistence DEFAULT; no export/copy flags. Slot numbers defined **once**, in `src/kmu_slots.hpp`; the script reads them from that file.
- Self-check message: ASCII `"MFS-KCV"` followed by byte `0x07` (8 bytes). Nothing derived from a key is ever logged.
- Every build: `CONFIG_NRF_APPROTECT_LOCK=y`, `CONFIG_NRF_SECURE_APPROTECT_LOCK=y`. Only `debug-unlocked.conf` selects `CONFIG_NRF_APPROTECT_DISABLE=y`, `CONFIG_NRF_SECURE_APPROTECT_DISABLE=y` and sets `CONFIG_MFS_DEBUG_UNLOCKED=y`, which logs `DEBUG PORT UNLOCKED - not for the field!` at boot (`LOG_WRN`).
- `CONFIG_MFS_DEVICE_SECRET` / `CONFIG_MFS_PROVISION_KEY` must be empty in any firmware build: CMake `FATAL_ERROR` otherwise. Firmware never reads them.
- Erase protection (`UICR.ERASEPROTECT`) is never set.
- House style `~/.claude/CLAUDE.md`; `clang-format -i` on touched C++; host flags `-std=c++20 -Wall -Wextra -Wpedantic -Werror`.
- Branch `kmu-keys-debug-lock` (from `main`). MFS1-1126-00007 is not migrated (it runs tag `trial-00007-2026-11-14`).
- **Hardware:** agents never touch the hub DK (J-Link 1057733814). Hardware steps (Task 6) run only on the owner's go-ahead: the key-test DK (1057755043) first, then one fleet device.
- Never read, print or commit `secrets/*.conf`, `credentials.conf` or a generated key file.

## Plan-level rulings

- **No `MFS_KEYS_IN_KMU` symbol.** The spec named one "default y", but with no fallback path (spec anti-goal: "two code paths") an `n` setting would have nothing to select. The rule is enforced by the empty-secret `FATAL_ERROR` instead.
- **Host KMU table lives in `tests/`** (`tests/host_kmu.hpp`), so test-only code stays out of `src/`.

## Review Focus

1. **A `KeyRef` for an unprovisioned slot** (blank KMU) → `HmacSha256` returns an error, the self-check returns `-ENOKEY`, commands are refused; never a zero-filled MAC treated as valid. Test in Task 2.
2. **The script interrupted between recover and protection-set** (Ctrl-C, a failed provision) → the temp key file is still deleted (`trap`), the device is left unlocked and unprovisioned, re-running recovers it. Test in Task 5 (`--dry-run` with a forced failure).
3. **A secret leaking into the build** (someone passes `secrets/<serial>.conf` straight to `west build`) → the build fails. Test in Task 4.
4. **Wrong J-Link** (the hub DK connected alongside) → the script uses only `MFS_DEV_ID` or its default 853003346 and refuses 1057733814 outright. Test in Task 5.
5. **Equal or all-zero keys in `secrets/<serial>.conf`** → refused on the host before any erase. Test in Task 5.

---

### Task 1: `KeyRef` in the crypto seam, host KMU table

**Files:**
- Modify: `src/crypto.hpp`, `src/crypto_psa.cpp`, `tests/crypto_openssl.cpp`
- Create: `src/kmu_slots.hpp`, `tests/host_kmu.hpp`, `tests/test_key_ref.cpp`; register in `tests/test_main.cpp`

**Interfaces:**
- Produces:
  ```cpp
  namespace alc::crypto {
    struct KeyRef {
        static KeyRef Raw(const uint8_t* bytes, size_t length);
        static KeyRef Kmu(uint8_t slot);
        bool IsKmu() const;
        const uint8_t* bytes; size_t length; uint8_t slot; bool kmu;
    };
    int HmacSha256(const KeyRef& key, const uint8_t* message, size_t messageLength, uint8_t* out);  // new overload
  }
  // src/kmu_slots.hpp
  namespace alc::kmu { constexpr uint8_t M_SECRET_SLOT { 100 }; constexpr uint8_t M_PROVISION_SLOT { 102 }; }
  // tests/host_kmu.hpp (host only)
  namespace alc::crypto::host_kmu { void Provision(uint8_t slot, const uint8_t* key, size_t length); void Clear(); }
  ```
  Target: a KMU ref uses `PSA_KEY_HANDLE_FROM_CRACEN_KMU_SLOT(CRACEN_KMU_KEY_USAGE_SCHEME_RAW, slot)` with `psa_mac_compute` (no import); a PSA failure → `-ENOKEY` if `PSA_ERROR_INVALID_HANDLE`/`PSA_ERROR_DOES_NOT_EXIST`, else `-EIO`. A raw ref uses the existing import path. Host: an unprovisioned slot → `-ENOKEY`. The raw-pointer overload stays (day keys).
- [ ] **Step 1: Failing tests** (`run_key_ref_tests()`): raw ref of `00..1F` over the dayKey message equals the vector `AD84…7786`; `host_kmu::Provision(100, 00..1F)` then `KeyRef::Kmu(100)` gives the same MAC; `KeyRef::Kmu(102)` unprovisioned → `-ENOKEY` and `out` untouched; `kmu::M_SECRET_SLOT + 2 == kmu::M_PROVISION_SLOT` (256-bit key = 2 slots).
- [ ] **Step 2:** `make test` → compile failure.
- [ ] **Step 3:** Implement; target file builds in Task 4.
- [ ] **Step 4:** `make test` → exits 0.
- [ ] **Step 5:** Commit `"Crypto: KeyRef - raw bytes or a KMU slot (host KMU table for tests)"`.

### Task 2: Access code takes `KeyRef`s; boot self-check

**Files:**
- Modify: `src/access_keys.{hpp,cpp}`, `src/access_control.{hpp,cpp}`, `src/crypto_selftest.cpp`, `tests/test_access_keys.cpp`, `tests/test_access_control.cpp`
- Create: `src/key_check.{hpp,cpp}`, `tests/test_key_check.cpp`; add `src/key_check.cpp` to `HOST_SRCS` and CMake

**Interfaces:**
- Consumes: Task 1.
- Produces:
  ```cpp
  int access::DeriveDayKey(const crypto::KeyRef& secret, uint32_t deviceId, uint16_t day, uint8_t slot, uint8_t* dayKey);
  bool access::OpenTimeSync(const crypto::KeyRef& provisionKey, uint32_t deviceId, const uint8_t* onAir, uint32_t& unixSeconds);
  AccessControl(uint32_t deviceId, const crypto::KeyRef& secret, PersistFn persist, void* context);  // stores the KeyRef by value
  namespace alc::key_check {
    /** 0; -ENOKEY if either key cannot be used; -EINVAL if equal or either equals the all-zero key. */
    int CheckLongTermKeys(const crypto::KeyRef& secret, const crypto::KeyRef& provisionKey);
  }
  ```
  Self-check compares full 32-byte HMACs of `"MFS-KCV" 0x07` with `crypto::ConstantTimeEqual`; the zero reference is `KeyRef::Raw` of 32 zero bytes. The self-test (`crypto_selftest.cpp`) passes `KeyRef::Raw(vectors::…)`.
- [ ] **Step 1: Failing tests:** existing access tests updated to pass `KeyRef::Raw(...)` and still pass unchanged vectors; new: an `AccessControl` built on `KeyRef::Kmu(100)` (host table holds the vector secret) accepts the vector command; `CheckLongTermKeys` → 0 for distinct provisioned keys, `-ENOKEY` with slot 102 unprovisioned, `-EINVAL` for equal keys, `-EINVAL` for an all-zero key.
- [ ] **Step 2:** `make test` → fails.
- [ ] **Step 3:** Implement.
- [ ] **Step 4:** `make test` → exits 0.
- [ ] **Step 5:** Commit `"Access: long-term keys by KeyRef; boot key self-check"`.

### Task 3: App uses the KMU keys

**Files:** Modify `src/app.cpp` (`initAccess`, `s_device_secret`/`s_provision_key` removed, `OpenTimeSync` call), `src/app.hpp` if needed.

**Interfaces:** Consumes Tasks 1–2. `initAccess()`: parse `CONFIG_MFS_DEVICE_ID` only; `crypto::Init()`; `RunSelfTest()`; `key_check::CheckLongTermKeys(KeyRef::Kmu(kmu::M_SECRET_SLOT), KeyRef::Kmu(kmu::M_PROVISION_SLOT))` — on failure `LOG_ERR("Device keys missing or invalid in the KMU (%d) - commands refused; reprovision with tools/flash_device.sh!", result)` and the existing failure return (warning, commands ignored). `m_access` built with `KeyRef::Kmu(kmu::M_SECRET_SLOT)`; time sync with `KeyRef::Kmu(kmu::M_PROVISION_SLOT)`.
- [ ] **Step 1:** `make test` still exits 0 (no host change).
- [ ] **Step 2:** Implement; build verified in Task 4.
- [ ] **Step 3:** Commit `"App: device secret and provisioning key from the KMU"`.

### Task 4: Build configuration — lock, unlocked overlay, no secrets

**Files:** Modify `prj.conf`, `Kconfig`, `CMakeLists.txt`, `credentials.conf.template`; create `debug-unlocked.conf`.

**Interfaces:** `prj.conf` adds the two `*_APPROTECT_LOCK=y` lines and any PSA options the KMU path needs (`CONFIG_PSA_WANT_KEY_TYPE_HMAC` etc. already present for the volatile path — confirm). `Kconfig`: `MFS_DEVICE_SECRET` / `MFS_PROVISION_KEY` help says "must stay empty; keys are provisioned into the KMU by tools/flash_device.sh"; new `config MFS_DEBUG_UNLOCKED` (bool, default n). `CMakeLists.txt`: `FATAL_ERROR` if either secret is non-empty. App logs the unlocked warning at boot when `CONFIG_MFS_DEBUG_UNLOCKED`. Template keeps serial + device ID; the two secret lines are documented as tool-only (read by `flash_device.sh` and `gen_bench_credentials.py`, never by the build).
- [ ] **Step 1 (failing check):** build with a conf that sets a non-empty `CONFIG_MFS_DEVICE_SECRET` → expect the CMake error (`nrfutil toolchain-manager launch --ncs-version v3.2.4 -- west build -b nrf54l15dk/nrf54l05/cpuapp -p always -d build-check -- -DEXTRA_CONF_FILE=<temp conf with a dummy 64-hex secret>`; the temp conf uses an obviously fake secret).
- [ ] **Step 2:** Implement; re-run → `FATAL_ERROR` with the message.
- [ ] **Step 3:** Build with a secrets-free conf (serial + ID only, dummy values) → succeeds; `.config` shows both `*_APPROTECT_LOCK=y`; with `debug-unlocked.conf` added → both `*_DISABLE=y` and `CONFIG_MFS_DEBUG_UNLOCKED=y`. `make test` exits 0. Remove `build-check/`.
- [ ] **Step 4:** Commit `"Build: every image locks the debug port; secrets never reach a build"`.

### Task 5: `flash_device.sh` — erase, provision, program, lock

**Files:** Modify `tools/flash_device.sh`; create `tools/test_flash_device.sh` (a `--dry-run` harness).

**Interfaces:** `tools/flash_device.sh <serial> [--build] [--unlocked] [--extra-conf <file>] [--dry-run]`.
- Steps per spec §5.5. Slots read from `src/kmu_slots.hpp` (`sed` on `M_SECRET_SLOT { N }`). Derived conf and key file via `mktemp`, `chmod 600`, removed by `trap … EXIT INT TERM`. Host-side refusal of all-zero or equal keys before any device step. After the build: `grep -c` for each secret's hex in `build/class_mfs_1/zephyr/.config` and `zephyr.hex` (as ASCII in the .config; as bytes in the .hex via `xxd -p`) must be 0. J-Link: `MFS_DEV_ID` or default `853003346`; **refuse `1057733814`** (the live hub DK). Program onto the erased chip without erasing (builder chooses `nrfutil device program … chip_erase_mode=ERASE_NONE` or equivalent and records it). `protection-set All` (and `SecureRegions` if needed — verified in Task 6). Final message per spec step 8. `--unlocked` adds `debug-unlocked.conf` and skips `protection-set`, printing a warning. `--dry-run` prints every device command instead of running it (no build, no device), for the harness.
- `tools/test_flash_device.sh` (uses a throwaway `secrets/TEST-…` conf in a temp `secrets` dir via an env override `MFS_SECRETS_DIR`): asserts the dry-run command order (recover → x-provision-keys → program → protection-set), that `MFS_DEV_ID=1057733814` is refused, equal/zero keys are refused before any device command, the temp key file is gone after a forced failure (`MFS_DRY_RUN_FAIL_AT=provision`), and `--unlocked` omits protection-set.
- [ ] **Step 1:** Write `tools/test_flash_device.sh`; run → fails (options unknown).
- [ ] **Step 2:** Implement.
- [ ] **Step 3:** `sh tools/test_flash_device.sh` → all checks pass; `sh -n tools/flash_device.sh`.
- [ ] **Step 4:** Commit `"flash_device.sh: erase, provision KMU keys, program and lock"`.

### Task 6: Docs, then hardware (owner's go-ahead)

**Files:** Modify `CLAUDE.md`, `docs/tan-scheme.md` §8, `tools/gen_fleet_credentials.py` (help text), `docs/superpowers/plans/2026-09-13-bench-checklist.md` (new section); move the bench identity: `secrets/MFS1-0926-00001.conf` from `credentials.conf` (owner does the copy; the plan never reads either file).
- [ ] **Step 1:** Docs per spec §5.6–5.9 (keys in KMU, slots, Raw caveat, locked fleet, unlock = re-run the script, `--unlocked` for RTT, `--keep-nvs` withdrawn, every flash erases NVS, 00007 on its tag). Commit `"Docs: keys in the KMU and the fleet debug lock"`.
- [ ] **Step 2 (owner go-ahead; key-test DK 1057755043):** a DK build needs a DK identity — owner provides `secrets/DK-TEST.conf` (or reuses the spike's public test vector keys). Run `MFS_DEV_ID=1057755043 tools/flash_device.sh <dk serial>`. Check: `nrfutil device protection-get --serial-number 1057755043` shows protected; RTT unavailable; `nrfutil device recover --serial-number 1057755043` unlocks; a re-run with `--unlocked` shows the boot self-check passing on RTT, and with the KMU erased, refusing.
- [ ] **Step 3 (owner go-ahead; one fleet device, not 00007):** run the script; sync the clock and send one Disarm from the app; confirm the green LED behaviour and the orange triple. Record in the bench checklist.
- [ ] **Step 4 (owner):** the remaining fleet devices; delete old `build/` directories that held Kconfig keys.
