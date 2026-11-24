# Event reports — MFS firmware and protocol — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** The MFS sends encrypted event reports (activated, deactivated, fired from command, fired from motion, low battery) under a fixed per-device report key, fires FIRE immediately with no countdown or receipt confirmation, and takes the four report switches in Settings — protocol 0x05.

**Architecture:** New pure, host-tested units — `report_keys` (derivations and sealing), `report_counter` (block-reserved, never-reused counter), `report_policy` (which report is due, low-battery hysteresis, switch mask) and `fire_gate` (the announce-then-assert stage) — wired into `App` at the existing single derivation point `App::updateOutputState()` and the main loop. `tools/flash_device.sh` bumps a per-device report generation on every flash. This is plan 1 of 4; `class_hub`, the Network Manager stand-in and `class_app` follow in their own plans.

**Tech Stack:** C++20 on Zephyr / NCS v3.2.4 (nRF54L05, PSA/CRACEN, KMU), host tests with `g++` + OpenSSL (`make test`), Python `cryptography` for vectors, POSIX `sh` for the flash script.

**Spec:** `docs/superpowers/specs/2026-11-24-event-reports-design.md` (approved 2026-11-24). Read §4–§5 and §11 before starting.

## Global Constraints

- House style: `~/.claude/CLAUDE.md` (alc namespace, `m_`/`s_`/`M_` prefixes, PascalCase public methods, no literals in calls, `LOG_ERR` ends with `!`). Run `/Users/andi/nrfenv/bin/clang-format -i` on every C++ file touched.
- `make test` **and** `sh tools/test_flash_device.sh` pass after every task. Host flags `-std=c++20 -Wall -Wextra -Wpedantic -Werror`.
- Labels: `0x08` report key, `0x09` report enc key, `0x0A` report ID. `0x06` (confirmId) is **retired and never reused**; `0x07` stays the boot key check.
- `reportVersion` = `0x01`; `protocolVersion` = `0x05`.
- Report on air: one 128-bit service UUID, 16 bytes = `reportId(4) ‖ ciphertext(8) ‖ tag(4)`.
- Event codes: `0x01` low battery, `0x02` activated, `0x03` deactivated, `0x10` fired from command, `0x11` fired from motion, `0x12` fired from sound level (never sent by MFS_1).
- Battery byte: tenths of a volt, rounded; `0xFF` = not measured.
- **A disarmed device (Inactive or Arming) sends nothing.** Deactivated goes out as the device leaves Active, never after a fire.
- Low battery: below **2700 mV**; re-armed at **≥ 2800 mV** or boot; sent only while Active.
- Report burst: **100 ms** at a **20 ms** interval. Fire assertion **100 ms** after the burst starts; never blocks beyond the existing supply settle; a report that cannot be built fires anyway.
- Settings byte 6 bits 0–3 **suppress** low battery / activated / deactivated / fired; zero = all on.
- Counter block **16**, refill when **4** remain. Battery read at boot, after each arming battery test, and every **6 h**.
- Secrets never reach `west build`; `CONFIG_MFS_REPORT_GENERATION` is not secret.
- Agents may not flash or use a J-Link. On-target behaviour goes into the bench checklist (Task 9).

## Review Focus

1. **A Disarm arriving between the fire report and the assertion** must cancel the fire — owner rule "always fail safe". Test in Task 5 (`FireGate` drops when the condition falls) and Task 6 (bench).
2. **Two reports due in the same tick** (activated and a pending low battery; a fire during another burst): the fire report must never wait; a non-fire report waits for the burst on air to end. Test in Task 4 (`TakeLowBattery` only when nothing is on air is App's job — pinned in Task 6's step).
3. **The counter running out of reserved values when a persist fails** — a report must be skipped, never sent under a reused or unpersisted counter. Test in Task 3 (`Take()` returns false after a failed refill).
4. **An old 5-byte `params/v1` record** after the firmware update must load with all reports on. Test in Task 2.
5. **Generation 0 (a build without the flash script, e.g. plain `west build`)** must send no reports rather than reports under a key the NM cannot know. Test in Task 6 step (boot log) and Task 1 is unaffected.

---

### Task 1: Report derivations, protocol 0x05, vectors; retire confirmId

**Files:**
- Modify: `tools/gen_access_vectors.py` (add report functions and cases; `PROTOCOL_VERSION = 0x05`; drop `confirm_id` and the confirm cases)
- Regenerate: `src/access_vectors.hpp` (and `../class_app/test/access_vectors.dart`, written by the script when present)
- Create: `src/report_keys.hpp`, `src/report_keys.cpp`
- Modify: `src/access_keys.hpp`, `src/access_keys.cpp` (remove `ConfirmId`, `M_LABEL_CONFIRM`; leave a comment that 0x06 is retired), `src/mfs_protocol.hpp` (`M_PROTOCOL_VERSION { 0x05 }`), `src/crypto_selftest.cpp` (also check the report vectors), `Makefile` (`HOST_SRCS += src/report_keys.cpp`)
- Test: `tests/test_report_keys.cpp` (new, `run_report_key_tests()` registered in `tests/test_main.cpp`), `tests/test_access_keys.cpp` (remove the confirm loop), `tests/test_key_check.cpp` (label list: drop 0x06, add 0x08–0x0A)

**Interfaces:**
- Produces, in `namespace alc::report` (`src/report_keys.hpp`):
  - `constexpr uint8_t M_LABEL_REPORT_KEY { 0x08 }`, `M_LABEL_REPORT_ENC_KEY { 0x09 }`, `M_LABEL_REPORT_ID { 0x0A }`, `M_REPORT_VERSION { 0x01 }`, `M_NONCE_MARKER { 0xFF }`, `M_REPORT_KEY_BYTES { 32 }`, `M_PLAINTEXT_BYTES { 8 }`, `M_BATTERY_UNKNOWN { 0xFF }`
  - `enum class ReportEvent : uint8_t { LowBattery = 0x01, Activated = 0x02, Deactivated = 0x03, FiredFromCommand = 0x10, FiredFromMotion = 0x11, FiredFromSound = 0x12 };`
  - `int DeriveReportKey(const crypto::KeyRef& secret, uint32_t deviceId, uint16_t generation, uint8_t* reportKey);` — `HMAC(secret, id BE32 ‖ g BE16 ‖ 0x08)`
  - `int DeriveReportEncKey(const uint8_t* reportKey, uint8_t* encKey);` — first 16 of `HMAC(reportKey, 0x09)`
  - `int DeriveReportId(const uint8_t* reportKey, uint32_t counter, uint8_t* reportId);` — first 4 of `HMAC(reportKey, r BE32 ‖ 0x0A)`
  - `void BuildReportNonce(uint32_t deviceId, uint16_t generation, uint32_t counter, uint8_t* nonce);` — `id BE32 ‖ g BE16 ‖ 0xFF ‖ r BE32`
  - `uint8_t BatteryTenths(int32_t millivolts);` — `(mv + 50) / 100`, clamped to 254; negative → `0xFF`
  - `void EncodeReport(ReportEvent event, uint8_t batteryTenths, uint8_t* plaintext);` — byte 0 event, byte 1 battery, bytes 2–7 zero
  - `int SealReport(const uint8_t* reportKey, uint32_t deviceId, uint16_t generation, uint32_t counter, const uint8_t* plaintext, uint8_t* onAir);`
  - `int OpenReport(const uint8_t* reportKey, uint32_t deviceId, uint16_t generation, uint32_t counter, const uint8_t* onAir, uint8_t* plaintext);` (tests and NM parity)
- Produces in `access_vectors.hpp`: `struct ReportVector { uint16_t generation; uint32_t counter; uint8_t reportKey[32]; uint8_t encKey[16]; uint8_t reportId[4]; uint8_t plaintext[8]; uint8_t onAir[16]; }; constexpr ReportVector M_REPORTS[...]`.

- [ ] **Step 1: Extend the generator.** In `tools/gen_access_vectors.py` add `report_key`, `report_enc_key`, `report_id`, `report_nonce`, `seal_report` mirroring §4.1 exactly, `REPORT_VERSION = 0x01`, and these cases (bench secret and `DEVICE_ID`):
  `("activated_g1_r0", 1, 0, [0x02, 29, 0,0,0,0,0,0])`, `("fired_motion_g1_r17", 1, 17, [0x11, 30, 0,0,0,0,0,0])`, `("low_battery_g2_r0", 2, 0, [0x01, 26, 0,0,0,0,0,0])`, `("deactivated_g65535_r4294967295", 65535, 0xFFFFFFFF, [0x03, 0xFF, 0,0,0,0,0,0])`. Emit `M_REPORTS` (C++) and the Dart equivalent. Set `PROTOCOL_VERSION = 0x05`; remove `confirm_id`, `LABEL_CONFIRM` and the confirm cases.
- [ ] **Step 2: Regenerate.** Run: `python3 tools/gen_access_vectors.py` — Expected: `wrote .../src/access_vectors.hpp` (and the Dart file).
- [ ] **Step 3: Write the failing test** `tests/test_report_keys.cpp`: for every `M_REPORTS` vector assert `DeriveReportKey(KeyRef::Raw(M_SECRET, 32), M_DEVICE_ID, g, key) == 0` and `memcmp(key, v.reportKey, 32) == 0`; same for `DeriveReportEncKey`, `DeriveReportId`; `SealReport(...)` equals `v.onAir`; `OpenReport` of `v.onAir` returns 0 and `v.plaintext`; flipping `onAir[15]` makes `OpenReport` return `-EBADMSG`; opening under `generation + 1` fails. `BatteryTenths(2949) == 29`, `BatteryTenths(2950) == 30`, `BatteryTenths(-1) == 0xFF`, `BatteryTenths(30000) == 254`. `EncodeReport(ReportEvent::FiredFromMotion, 30, pt)` gives `{0x11, 30, 0, 0, 0, 0, 0, 0}`. A KMU `KeyRef` for an unprovisioned slot returns `-ENOKEY` from `DeriveReportKey`.
- [ ] **Step 4: Run** `make test` — Expected: compile failure (`report_keys.hpp` missing).
- [ ] **Step 5: Implement** `src/report_keys.{hpp,cpp}` with the signatures above, using `crypto::HmacSha256` and `crypto::AesCcmEncrypt/Decrypt` with tag 4 and AAD `reportId ‖ M_REPORT_VERSION`. Remove `ConfirmId`/`M_LABEL_CONFIRM` from `access_keys`, the confirm test loop, and update the key-check label list. Add the report vectors to `crypto_selftest.cpp` alongside the command vectors (same pass/fail reporting).
- [ ] **Step 6: Run** `make test` — Expected: `ALL TESTS PASSED`, including `report keys: OK`.
- [ ] **Step 7: Commit** — `git commit -m "Report keys: derivations, sealing and vectors (labels 0x08-0x0A); protocol 0x05; confirmId retired"`

### Task 2: Settings byte 6 — the four report switches

**Files:**
- Modify: `src/mfs_protocol.hpp`, `src/mfs_protocol.cpp`, `src/settings.hpp`, `src/settings.cpp`
- Test: `tests/test_protocol.cpp`, `tests/test_settings.cpp`

**Interfaces:**
- Produces in `alc::protocol`: `constexpr uint8_t M_PT_REPORTS { 6 }`, `M_REPORT_SUPPRESS_MASK { 0x0F }`, `M_SUPPRESS_LOW_BATTERY { 1U << 0 }`, `M_SUPPRESS_ACTIVATED { 1U << 1 }`, `M_SUPPRESS_DEACTIVATED { 1U << 2 }`, `M_SUPPRESS_FIRED { 1U << 3 }` (all `uint8_t`; `BIT()` is not available in the host build); `Command::reportSuppress` (`uint8_t`, default 0).
- Produces in `Settings`: `uint8_t ReportSuppress() const`.

- [ ] **Step 1: Write failing tests.** Protocol: a Settings plaintext with byte 6 = `0xFA` decodes `reportSuppress == 0x0A` (high nibble ignored); Arm/Disarm/FIRE decode `reportSuppress == 0`; `EncodeCommand` of a Settings command with `reportSuppress = 0x05` writes byte 6 = `0x05` and byte 7 = 0. Settings: `ApplyFrom` with a changed `reportSuppress` alone returns true and `ReportSuppress()` reflects it; a fresh `Settings` has `ReportSuppress() == 0`.
- [ ] **Step 2: Run** `make test` — Expected: FAIL (no member `reportSuppress`).
- [ ] **Step 3: Implement.** Decode byte 6 low nibble **for Settings only**; Arm/Disarm/FIRE leave it 0. Settings stores `m_report_suppress`; `ApplyFrom` compares and saves it. On target, `params/v1` becomes **6 bytes** (`..., mode, reportSuppress`); `settingsSet` accepts **5 or 6** bytes — a 5-byte record loads with `reportSuppress = 0` — and `save()` always writes 6. Update the "bytes 6-7 must be ignored" comments: byte 6 low nibble is now MFS_1's.
- [ ] **Step 4: Run** `make test` — Expected: `ALL TESTS PASSED`.
- [ ] **Step 5: Commit** — `git commit -m "Settings: four report switches in byte 6 (suppress-sense, zero = all on)"`

### Task 3: Report counter and its store

**Files:**
- Create: `src/report_counter.hpp`, `src/report_counter.cpp` (pure), `src/report_store.hpp`, `src/report_store.cpp` (Zephyr settings, `report/v1`)
- Modify: `Makefile` (`HOST_SRCS += src/report_counter.cpp`), `CMakeLists.txt` (target sources)
- Test: `tests/test_report_counter.cpp` (`run_report_counter_tests()`)

**Interfaces:**
- Produces `class alc::ReportCounter`:
  - `using PersistFn = int (*)(uint32_t reservedUpTo, void* context);`
  - `ReportCounter(PersistFn persist, void* context);`
  - `static constexpr uint32_t M_BLOCK { 16 }; static constexpr uint32_t M_REFILL_AT { 4 };`
  - `int Begin(uint32_t storedFloor);` — persists `storedFloor + M_BLOCK` **first**, then makes `[storedFloor, storedFloor + M_BLOCK)` usable; on persist failure nothing is usable.
  - `bool Take(uint32_t& counter);` — next usable value, or false if none.
  - `bool NeedsRefill() const;` — true when ≤ `M_REFILL_AT` reserved values remain unused.
  - `int Refill();` — persists `reservedUpTo + M_BLOCK` first, then extends the usable range.
- Produces `alc::report_store`: `int Load(uint16_t generation, uint32_t& floor);` (floor 0 if absent **or stored generation differs**), `int Persist(uint32_t reservedUpTo, void* context);` (context points at the current `uint16_t` generation; record = `g BE16 ‖ reservedUpTo BE32`).

- [ ] **Step 1: Write failing tests** with a fake persist that records calls and can fail: `Begin(0)` persists 16 before any `Take`; 16 `Take`s yield 0..15 then false; after `Begin(32)` the first `Take` is 32; `NeedsRefill()` becomes true after the 12th `Take`; `Refill()` persists 32 then values 16..31 follow; a failing persist in `Begin` leaves `Take` false; a failing `Refill()` leaves the remaining reserved values usable but nothing beyond them (`Take` false after they run out); values never repeat across `Begin`/`Refill` sequences; `Begin(0xFFFFFFF8)` refuses (returns `-EOVERFLOW`, `Take` false) rather than wrapping.
- [ ] **Step 2: Run** `make test` — Expected: FAIL (missing header).
- [ ] **Step 3: Implement** `ReportCounter` (pure, no Zephyr headers) and `report_store` modelled on `src/access_store.cpp` (settings static handler, `settings_save_one("report/v1", ...)`).
- [ ] **Step 4: Run** `make test` — Expected: `ALL TESTS PASSED`.
- [ ] **Step 5: Commit** — `git commit -m "Report counter: block-reserved, persisted before use, never reused"`

### Task 4: Report policy

**Files:**
- Create: `src/report_policy.hpp`, `src/report_policy.cpp` (pure)
- Modify: `Makefile`
- Test: `tests/test_report_policy.cpp` (`run_report_policy_tests()`)

**Interfaces:**
- Consumes: `ReportEvent` (Task 1), `ArmState` (`src/arming_sequence.hpp`), suppress bits (Task 2).
- Produces `class alc::ReportPolicy`:
  - `static constexpr uint16_t M_LOW_BATTERY_MV { 2700 }; static constexpr uint16_t M_BATTERY_RECOVERED_MV { 2800 };`
  - `static bool Enabled(report::ReportEvent event, uint8_t suppressMask);` — LowBattery→bit 0, Activated→bit 1, Deactivated→bit 2, all three Fired→bit 3.
  - `std::optional<report::ReportEvent> ServiceArmState(ArmState state);` — call once per tick; `Activated` on entering Active; `Deactivated` on leaving Active unless `NoteFired()` was called this Active session; nothing otherwise.
  - `void NoteFired();`
  - `void NoteBattery(uint16_t millivolts);`
  - `bool TakeLowBattery(ArmState state);` — true once while Active when a reading below 2700 is unreported; a reading ≥ 2800 re-arms it.

- [ ] **Step 1: Write failing tests:** Inactive→Arming→Active gives one `Activated` (on the Active tick only); Active→Inactive gives `Deactivated`; Active, `NoteFired()`, →Inactive gives nothing; Arming→Inactive (cancelled) gives nothing; a second session after a fire reports Deactivated again (flag cleared on Activated); `NoteBattery(2650)` while Inactive then `TakeLowBattery(Inactive)` false, then `TakeLowBattery(Active)` true once and false after; 2650 → 2750 → 2650 stays reported (no second); 2650 → 2800 → 2650 reports again; `Enabled(FiredFromSound, M_SUPPRESS_FIRED)` false, `Enabled(Activated, M_SUPPRESS_FIRED)` true, `Enabled(x, 0)` true for all six.
- [ ] **Step 2: Run** `make test` — Expected: FAIL.
- [ ] **Step 3: Implement** `ReportPolicy`.
- [ ] **Step 4: Run** `make test` — Expected: `ALL TESTS PASSED`.
- [ ] **Step 5: Commit** — `git commit -m "Report policy: arm-state reports, fired suppresses deactivated, low-battery hysteresis, switches"`

### Task 5: FIRE without a countdown; the fire gate

**Files:**
- Modify: `src/fire_sequence.hpp`, `src/fire_sequence.cpp`
- Create: `src/fire_gate.hpp`, `src/fire_gate.cpp` (pure)
- Modify: `Makefile`
- Test: `tests/test_fire_sequence.cpp` (rewrite), `tests/test_fire_gate.cpp` (`run_fire_gate_tests()`)

**Interfaces:**
- `FireSequence`: `enum class FireState : uint8_t { Idle = 0, Firing = 1, Completed = 2 };` `Start(bool live, int64_t nowMs, int64_t assertMs)` goes **straight to Firing** (window `nowMs + assertMs`); `Service(int64_t nowMs, bool fireSwitchFaulty)` ends the window on expiry or fault → Completed. **Removed:** `M_FIRE_COUNTDOWN_MS`, `CountingDown`, `Aborted()`, `LiveInProgress()`, `AcceptsDisarm()`. Kept: `FireLatched()`, `RehearsalFired()`, `State()`. Add `void Cancel();` → Idle (a Disarm during a live window).
- Produces `class alc::FireGate`:
  - `static constexpr int64_t M_ANNOUNCE_MS { 100 };`
  - `bool Update(bool fireCondition, int64_t nowMs);` — condition false → idle, returns false. First true → announcing from `nowMs`, announce due, returns false. While announcing → true once `nowMs − start ≥ M_ANNOUNCE_MS` or `Skip()` was called.
  - `bool TakeAnnounceDue();` — true once per new fire condition.
  - `void Skip();` — permit at once (report off or failed).

- [ ] **Step 1: Write failing tests.** FireSequence: live `Start` at t → `FireLatched()` true at t, false at t + assertMs (Completed); rehearsal sets only `RehearsalFired()`; a fault while Firing completes at once; `Cancel()` while Firing → Idle, nothing latched; restart after Completed works. FireGate: `Update(true, 0)` false and `TakeAnnounceDue()` true once; `Update(true, 99)` false; `Update(true, 100)` true; condition false at 50 then true at 60 → a new announce (due again) and permit only at 160; `Skip()` after the announce → `Update(true, 1)` true; announce due is not raised again while the condition stays true.
- [ ] **Step 2: Run** `make test` — Expected: FAIL.
- [ ] **Step 3: Implement** both.
- [ ] **Step 4: Run** `make test` — Expected: `ALL TESTS PASSED`. (`app.cpp` is target-only, so the target build stays broken until Task 6 fixes its call sites — expected.)
- [ ] **Step 5: Commit** — `git commit -m "FIRE fires at once; FireGate: announce, then assert after 100 ms"`

### Task 6: App integration

**Files:**
- Modify: `src/app.hpp`, `src/app.cpp`, `src/command_advertiser.hpp`, `src/command_advertiser.cpp`, `Kconfig`, `CMakeLists.txt`, `src/arm_policy.hpp` (comments only: FIRE table, no countdown)

**Interfaces:**
- Consumes: Tasks 1–5.
- Kconfig: `config MFS_REPORT_GENERATION` — `int`, range 0–65535, default 0; help: set by `tools/flash_device.sh`, 0 = reports disabled.
- `CommandAdvertiser::Burst(const uint8_t uuid[16], int64_t nowMs, int64_t durationMs)` now advertises at `M_REPORT_ADV_INTERVAL_UNITS { 32 }` (20 ms, 0.625 ms units) min and max, non-connectable, no identity (`BT_LE_ADV_PARAM(BT_LE_ADV_OPT_NONE, ...)`). Log text says "Report advert".
- New `App` private members: `uint8_t m_report_key[report::M_REPORT_KEY_BYTES]`, `bool m_reports_ready`, `ReportCounter m_report_counter`, `ReportPolicy m_report_policy`, `FireGate m_fire_gate`, `int64_t m_last_battery_read_ms`, `int32_t m_battery_mv` (−1 = unknown).
- New `App` private methods: `int initReports();` (from `initAccess()`, after the key check), `int sendReport(report::ReportEvent event);`, `void readBattery();`.

- [ ] **Step 1: Boot.** `initReports()`: if `CONFIG_MFS_REPORT_GENERATION == 0` log `LOG_WRN("Report generation 0 - event reports disabled (flash with tools/flash_device.sh).")` and leave `m_reports_ready` false; else derive `m_report_key` from `KeyRef::Kmu(kmu::M_SECRET_SLOT)`, `s_device_id` and the generation, `report_store::Load()` the floor, `m_report_counter.Begin(floor)`; any failure → `LOG_ERR(...!)`, reports off, **not** a warning LED (the device still works). Never log the key.
- [ ] **Step 2: `sendReport(event)`.** Returns `-EPERM` if not ready or `!ReportPolicy::Enabled(event, m_settings.ReportSuppress())`; takes a counter (`-ENOSPC` if none); `EncodeReport` with `BatteryTenths(m_battery_mv)`; `SealReport`; `m_advertiser.Burst(uuid, now, M_REPORT_BURST_MS { 100 })`; logs `Report %s sent (r %u).` (event name, counter). Refill the counter **outside** the fire path: in the main loop, when `NeedsRefill()` and no burst is on air.
- [ ] **Step 3: Fire path in `updateOutputState()`.** Keep the single derivation:
  ```
  bool want = Active && ((DetectionMet && delayPermitsFiring()) || m_fire.FireLatched());
  bool permit = m_fire_gate.Update(want, k_uptime_get());
  if (m_fire_gate.TakeAnnounceDue()) {
    cause = m_fire.FireLatched() ? FiredFromCommand : FiredFromMotion;
    if (sendReport(cause) < 0) { m_fire_gate.Skip(); }   // log "Fire report not sent (%d) - firing anyway!" unless -EPERM from the switch
    m_report_policy.NoteFired();
    prepareFireSupply();                                  // existing blocking settle, if not yet raised
    permit = m_fire_gate.Update(want, k_uptime_get());
  }
  m_output_active = want && permit;
  ```
  The existing backstop `prepareFireSupply()` and `m_output_switch.Set()` stay where they are. A Disarm or fault before the next tick drops `want`, so the fire is cancelled.
- [ ] **Step 4: FIRE command.** In `applyCommand()`'s FIRE case: `m_fire.Start(live, now, DetectionEngine::M_DELAYED_TRIGGER_HOLD_MS)` and, if live, `prepareFireSupply()`; **delete** the confirmId derivation and burst, `M_CONFIRM_EVENT_*`, `M_CONFIRM_BURST_MS`. Delete the `AcceptsDisarm()` refusal (a Disarm now always acts; call `m_fire.Cancel()` in `disarmDevice()`), and the `LiveInProgress()` scanner exemption at the scanner fail-safe (it always applies now). Log `FIRE received from slot %u n %u - %s.` (LIVE / rehearsal).
- [ ] **Step 5: Main loop, after every state change of the tick** (after the switch-fault and trigger-complete disarms): `if (auto event = m_report_policy.ServiceArmState(m_arming.State())) { (void)sendReport(*event); }`; then, only if `!m_advertiser.IsAdvertising()`, `if (m_report_policy.TakeLowBattery(m_arming.State())) { (void)sendReport(LowBattery); }`. A fire report is never deferred (it is sent from `updateOutputState()`); it may cut another burst short — accepted (speed first).
- [ ] **Step 6: Battery.** `readBattery()`: `m_pmic.AdcReadVbat(volts)`, store mV in `m_battery_mv`, `m_report_policy.NoteBattery(mV)`, log `Battery %d mV.` on change of tenths only. Call at boot (after `initPmic()`), after each arming battery test (`ArmingStep::FireSupply` path, pass or fail), and from the main loop when `now − m_last_battery_read_ms ≥ M_BATTERY_READ_INTERVAL_MS { 6 * 60 * 60 * 1000 }`. A failed read logs `LOG_ERR(...!)` and keeps the last value.
- [ ] **Step 7: Build.** Run: `nrfutil toolchain-manager launch --ncs-version v3.2.4 -- west build -b nrf54l15dk/nrf54l05/cpuapp -p always -- -DEXTRA_CONF_FILE=<a scratch conf with CONFIG_MFS_REPORT_GENERATION=1, CONFIG_ALC_DEVICE_SERIAL and CONFIG_MFS_DEVICE_ID only>` — Expected: build succeeds with no warnings in `src/`. Then `make test` — `ALL TESTS PASSED`.
- [ ] **Step 8: Commit** — `git commit -m "App: event reports on arm, disarm, fire and low battery; FIRE immediate; confirm burst removed"`

### Task 7: Flash script — the report generation

**Files:**
- Modify: `tools/flash_device.sh`, `tools/test_flash_device.sh`, `secrets/MFS1-0926-00001.conf` is **not** touched by the agent (gitignored; the script handles a missing key)

**Interfaces:**
- Reads `CONFIG_MFS_REPORT_GENERATION` from `secrets/<serial>.conf` (missing → 0). New value = old + 1 (refuse above 65535). Adds `CONFIG_MFS_REPORT_GENERATION=<new>` to the derived conf; checks the built `.config` carries it (like the serial check).
- **Real flash only** (not `--build`, not `--dry-run`): writes the new value back to `secrets/<serial>.conf` (replace the line, or append), keeping mode 600, **before** the build, and prints `Report generation: <old> -> <new>.`. `--build` and `--dry-run` print `Report generation: <old> -> <new> (not saved).`

- [ ] **Step 1: Write failing tests** in `tools/test_flash_device.sh`: a dry run with no generation line prints `Report generation: 0 -> 1 (not saved).` and the derived conf (dry-run output) holds `CONFIG_MFS_REPORT_GENERATION=1`; with `CONFIG_MFS_REPORT_GENERATION=41` it prints `41 -> 42`; the conf file is byte-identical after the dry run; `65535` is refused with a message naming the generation; a non-numeric value is refused.
- [ ] **Step 2: Run** `sh tools/test_flash_device.sh` — Expected: the new cases FAIL.
- [ ] **Step 3: Implement** in `flash_device.sh` (POSIX sh; no `set -x`; the value is not secret but the file holds keys — edit through a temp file in `$work`, then `mv`, then `chmod 600`).
- [ ] **Step 4: Run** `sh tools/test_flash_device.sh` and `make test` — Expected: `0 failed` and `ALL TESTS PASSED`.
- [ ] **Step 5: Commit** — `git commit -m "flash_device.sh: bump the report generation on every flash"`

### Task 8: Fire fob — fire-and-forget, protocol 0x05

**Files:**
- Modify: `tools/fire_fob/gen_fob_keys.py` (`PROTOCOL_VERSION = 0x05`; drop `LABEL_CONFIRM`, confirm IDs and `confirmIds` in the header), `tools/fire_fob/src/main.cpp` (`M_PROTOCOL_VERSION { 0x05 }`; remove the confirm scan, `s_expected`, `s_confirmed`, `M_CONFIRM_TIMEOUT_MS`; after each FIRE send, a single GREEN flash = "sent"; BLUE while the run is in progress), `tools/fire_fob/README.md`

- [ ] **Step 1:** Make the changes; regenerate the fob key header only through `flash_fob.sh` on the bench (the agent does not run it — it needs real keys).
- [ ] **Step 2: Build check.** Run: `nrfutil toolchain-manager launch --ncs-version v3.2.4 -- west build -b thingy53/nrf5340/cpuapp -p always tools/fire_fob -d tools/fire_fob/build` with a placeholder `secrets/fire_fob_keys.h` generated by `python3 tools/fire_fob/gen_fob_keys.py --fake` — add a `--fake` option that writes all-zero keys for a compile check only. Expected: build succeeds.
- [ ] **Step 3: Commit** — `git commit -m "fire_fob: protocol 0x05, fire-and-forget (no confirmation)"`

### Task 9: Documents

**Files:**
- Modify: `CLAUDE.md`, `docs/tan-scheme.md` (§3 derivations and label list; confirmId retired), `docs/v1-scope.md`, `docs/superpowers/specs/2026-09-15-fire-command-amendment.md` (superseded note at the top: §3–§4 replaced by the event-reports design §5.2), `docs/superpowers/specs/2026-09-14-command-types-amendment.md` (§2.1 byte 6), `docs/power-budget.md` (reports and the 6 h battery read: negligible — one line with the arithmetic), `docs/superpowers/plans/2026-09-13-bench-checklist.md` (new §5n)

- [ ] **Step 1: `CLAUDE.md`:** the "never advertises" access rule gains production event reports as a documented, switchable exception (design §5); delete the FIRE countdown exception paragraph and the "Confirm on receipt" mentions; add a short "Event reports" paragraph (report key, generation bump, disarmed sends nothing, labels 0x08–0x0A, protocol 0x05).
- [ ] **Step 2: Bench checklist §5n**, with `tools/uuid_observer` before the hub/NM exist: (a) Arm → one report ~10 s after the Arm (on Active); (b) Disarm → one report; (c) armed motion → report then fire ~100 ms later (logic analyser on FIRE_1 and an RTT timestamp); (d) armed FIRE → report then fire within ~100 ms of the command; (e) disarmed FIRE → red LED, **no** advert; (f) Settings with all switches off → no reports for (a)–(d); (g) a Disarm within 100 ms of an armed delay expiry cancels the fire (best effort); (h) a VBAT below 2.7 V on a bench supply while armed → one low-battery report; (i) `flash_device.sh` twice → generation bumped twice, reports still sent; (j) a plain `west build` image logs "Report generation 0" and sends nothing.
- [ ] **Step 3: Commit** — `git commit -m "Docs: event reports - CLAUDE.md, tan-scheme, amendments, bench checklist 5n"`
