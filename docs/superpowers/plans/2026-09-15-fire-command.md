# FIRE Command Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add a commanded FIRE that fires an already-armed MFS_1 after a fixed, unstoppable 10 s countdown, and an authenticated confirmation-of-receipt advert, across firmware, crypto vectors and the Flutter app.

**Architecture:** FIRE reuses command type `00` distinguished by an "OMG" magic. It is a one-shot fire condition ANDed with the arm boolean through the existing single output path — never a new path to the pins. A new host-tested `FireSequence` owns the 10 s countdown and its narrow fail-safe exceptions. The device confirms receipt by advertising an authenticated, random-looking `confirmId` burst; the app switches to scanning to hear it.

**Tech Stack:** nRF54L05 / Zephyr NCS v3.2.4, C++20, host tests via `make test`; Flutter/Dart (`ble_peripheral` for advertising, plus a new scan path); Python `cryptography` for known-answer vectors.

**Spec:** `docs/superpowers/specs/2026-09-15-fire-command-amendment.md` (amends the command-types, arming-sequence and disarmed-test-mode amendments, `tan-scheme.md`, and `CLAUDE.md`).

## Global Constraints

- **The arm boolean stays definitive.** FIRE never enables the fire pins. `App::updateOutputState()` remains the ONLY place the arm state and any fire condition combine; `App::IsOutputActive()` the only sanctioned read. FIRE adds a one-shot latch ORed into the detection side, still ANDed with `ArmState::Active`.
- **`DecideCommand()` (`src/arm_policy.hpp`) is the single command-policy path.** Add the FIRE rows there; never a second path in `App`.
- **Magic is `(byte0,byte2,byte3) == (0x4F,0x4D,0x47)` "OMG" with byte 1 == `0x00`** (type field `00`). Never place magic in bytes 6–7 (per-variant extension; MFS_1 must never validate it). Never require bytes 6–7 to be zero.
- **`M_PROTOCOL_VERSION` / `kProtocolVersion` → `0x04`.** It is CCM associated data, never on air.
- **`confirmId(n, event) = HMAC(dayKey, n(4 BE) ‖ event(1) ‖ 0x06)[0..15]`.** Label `0x06`; `0x00`/`0x01` stay retired.
- **The 10 s countdown, once accepted while Active, is unstoppable by Disarm and by scanner loss; only a fire-path hardware fault aborts it (fails safe).** Inactive FIRE is a rehearsal on LED B only, never the pins. Arming FIRE is ignored.
- **Persist-before-acting, silence-on-failure, freshness (10 min), lockout, never reuse a sequence number** — all unchanged; FIRE consumes its `n` like any command.
- **Vectors are the contract:** the Python generator is authoritative; firmware (PSA + host OpenSSL) and app (pointycastle) must reproduce every byte. Regenerate, never hand-edit `access_vectors.hpp` / `test/access_vectors.dart`.
- House style: `~/.claude/CLAUDE.md`; run `clang-format` on every C++ file, `dart format -l 120` on every Dart file; `make test` green before trusting firmware logic changes.

## File Structure

- `class_mfs_1/src/mfs_protocol.{hpp,cpp}` — FIRE recognition, version `0x04`, `Command.isFire`.
- `class_mfs_1/src/arm_policy.hpp` — `ArmAction::Fire`, FIRE rows in `DecideCommand()`.
- `class_mfs_1/src/fire_sequence.{hpp,cpp}` — NEW pure class: the 10 s countdown, one-shot latch, fault/scanner rules, rehearsal flag.
- `class_mfs_1/src/access_keys.{hpp,cpp}` — `confirmId(n, event)` derivation.
- `class_mfs_1/src/app.{hpp,cpp}` — wire `FireSequence`, OR the fire latch into `updateOutputState()`, LED B rehearsal, scanner-health exemption, confirmation burst.
- `class_mfs_1/src/command_advertiser.{hpp,cpp}` — NEW: minimal BLE advertising of a 16-byte service-data/UUID burst (the device has none today).
- `class_mfs_1/tools/gen_access_vectors.py` + generated `tests/access_vectors.hpp`, app `test/access_vectors.dart` — version `0x04`, FIRE case, `confirmId` vectors.
- `class_mfs_1/tests/test_*.cpp` — protocol, arm_policy, fire_sequence, access.
- `class_app/lib/protocol/mfs_protocol.dart` — FIRE encode, version `0x04`.
- `class_app/lib/protocol/access_keys.dart` — `deriveConfirmId`, version `0x04`.
- `class_app/lib/services/command_builder.dart` — `fireCommand()`, `expectedConfirmId()`.
- `class_app/lib/services/confirm_listener.dart` — NEW: scan for the confirmId with a timeout.
- `class_app/lib/devices/mfs1/fire_button.dart` — NEW: red button + red confirm-slider sheet.
- `class_app/lib/devices/mfs1/mfs1_screen.dart` — host the Fire button and the confirmation UI.
- Docs: `CLAUDE.md` (scan-only + always-fail-safe exceptions), bench checklist FIRE section.

---

### Task 1: Protocol — recognise FIRE, bump to version 0x04

**Files:**
- Modify: `class_mfs_1/src/mfs_protocol.hpp`, `class_mfs_1/src/mfs_protocol.cpp`
- Test: `class_mfs_1/tests/test_mfs_protocol.cpp`

**Interfaces:**
- Produces: `Command.isFire` (bool, default false), set true by `DecodeCommand()` for a FIRE payload; `M_PROTOCOL_VERSION == 0x04`; magic constants `M_FIRE_MAGIC_0/2/3`.
- Consumes: existing `Command`, `DecodeCommand()`, `EncodeCommand()`.

- [ ] **Step 1: Write the failing test**

```cpp
// FIRE: type 00, byte1 == 0x00, OMG in bytes 0,2,3, minute valid.
TEST(fire_recognised) {
  const uint8_t pt[8] = { 0x4F, 0x00, 0x4D, 0x47, 0x1E, 0x02, 0x00, 0x00 }; // minute 0x021E=542
  protocol::Command c;
  ASSERT_TRUE(protocol::DecodeCommand(pt, c));
  ASSERT_TRUE(c.isFire);
  ASSERT_EQ(c.type, protocol::CommandType::Reserved); // FIRE is a distinguished Reserved
  ASSERT_EQ(c.minuteOfDay, 542);
}
// Near-misses: still rejected, never fire.
TEST(fire_near_miss_rejected) {
  protocol::Command c;
  const uint8_t noMagic[8] = { 0, 0, 0, 0, 0x1E, 0x02, 0, 0 };            // bare type 00
  ASSERT_FALSE(protocol::DecodeCommand(noMagic, c));
  const uint8_t byte1Set[8] = { 0x4F, 0x01, 0x4D, 0x47, 0x1E, 0x02, 0, 0 }; // byte1 != 0
  ASSERT_FALSE(protocol::DecodeCommand(byte1Set, c));
  const uint8_t wrongMagic[8] = { 0x4F, 0x00, 0x4D, 0x48, 0x1E, 0x02, 0, 0 };
  ASSERT_FALSE(protocol::DecodeCommand(wrongMagic, c));
  const uint8_t badMinute[8] = { 0x4F, 0x00, 0x4D, 0x47, 0xA0, 0x05, 0, 0 }; // 1440
  ASSERT_FALSE(protocol::DecodeCommand(badMinute, c));
}
// Bytes 6-7 are ignored, never validated, even for FIRE.
TEST(fire_ignores_extension_bytes) {
  protocol::Command c;
  const uint8_t ext[8] = { 0x4F, 0x00, 0x4D, 0x47, 0x1E, 0x02, 0xAB, 0xCD };
  ASSERT_TRUE(protocol::DecodeCommand(ext, c));
  ASSERT_TRUE(c.isFire);
}
```

- [ ] **Step 2: Run to verify it fails** — `cd class_mfs_1 && make test`. Expected: FAIL (no `isFire`, magic not recognised).

- [ ] **Step 3: Implement**

In `mfs_protocol.hpp`: set `M_PROTOCOL_VERSION { 0x04 }`; add
```cpp
constexpr uint8_t M_FIRE_MAGIC_0 { 0x4F }; // 'O'
constexpr uint8_t M_FIRE_MAGIC_2 { 0x4D }; // 'M'
constexpr uint8_t M_FIRE_MAGIC_3 { 0x47 }; // 'G'
```
and add `bool isFire { false };` to `struct Command`.

In `mfs_protocol.cpp`, at the top of `DecodeCommand()`, before the reserved-type early return, detect FIRE and (if matched) set `out.isFire = true`, populate `out.type = CommandType::Reserved`, decode and validate the minute, and return true. A payload that is type `00` but not the FIRE magic keeps the existing reject. Precisely:
```cpp
const uint8_t type = (plaintext[M_PT_ACTIVATIONS_MODE] >> M_TYPE_SHIFT) & M_TYPE_MASK;
if (type == static_cast<uint8_t>(CommandType::Reserved)) {
  const bool fireMagic = plaintext[0] == M_FIRE_MAGIC_0 && plaintext[M_PT_ACTIVATIONS_MODE] == 0x00 &&
                         plaintext[M_PT_COOLDOWN] == M_FIRE_MAGIC_2 && plaintext[M_PT_SENSITIVITY] == M_FIRE_MAGIC_3;
  if (!fireMagic) { return false; } // bare reserved: reject as before
  const uint16_t minute = readMinute(plaintext); // existing minute decode + range check
  if (minute >= M_MINUTES_PER_DAY) { return false; }
  out = Command{};
  out.isFire = true;
  out.type = CommandType::Reserved;
  out.minuteOfDay = minute;
  return true;
}
```
Extend `EncodeCommand()` to emit the FIRE layout when `command.isFire` (magic bytes, byte 1 = 0, minute), for the vector generator's C++ side / round-trip tests.

- [ ] **Step 4: Run to verify it passes** — `make test`. Expected: PASS.

- [ ] **Step 5: clang-format and commit**

```bash
/Users/andy/nrfenv/bin/clang-format -i src/mfs_protocol.hpp src/mfs_protocol.cpp
git add src/mfs_protocol.hpp src/mfs_protocol.cpp tests/test_mfs_protocol.cpp
git commit -m "FIRE: recognise type 00 + OMG magic, protocol version 0x04"
```

---

### Task 2: Arm policy — ArmAction::Fire

**Files:**
- Modify: `class_mfs_1/src/arm_policy.hpp`
- Test: `class_mfs_1/tests/test_arm_policy.cpp`

**Interfaces:**
- Consumes: `Command.isFire` (Task 1), `ArmState`.
- Produces: `ArmAction::Fire`; `DecideCommand()` returns it for a FIRE command while Active or Inactive, and `Ignore` while Arming.

- [ ] **Step 1: Write the failing test**

```cpp
TEST(fire_active_fires) {
  protocol::Command c; c.isFire = true;
  auto d = DecideCommand(ArmState::Active, false, c);
  ASSERT_EQ(d.action, ArmAction::Fire);
  ASSERT_FALSE(d.applySettings);
}
TEST(fire_inactive_rehearses) {
  protocol::Command c; c.isFire = true;
  ASSERT_EQ(DecideCommand(ArmState::Inactive, false, c).action, ArmAction::Fire);
}
TEST(fire_arming_ignored) {
  protocol::Command c; c.isFire = true;
  ASSERT_EQ(DecideCommand(ArmState::Arming, false, c).action, ArmAction::Ignore);
}
TEST(fire_trims_clock_like_any_accepted_command) {
  protocol::Command c; c.isFire = true;
  ASSERT_TRUE(DecideCommand(ArmState::Active, false, c).trimClock);
}
```

- [ ] **Step 2: Run to verify it fails** — `make test`. Expected: FAIL (no `Fire`).

- [ ] **Step 3: Implement**

Add `Fire` to `enum class ArmAction`. In `DecideCommand()`, immediately after the `Reserved` guard, branch on `command.isFire` before the Disarm/type logic:
```cpp
if (command.isFire) {
  if (state == ArmState::Arming) { return decision; } // Ignore: only Disarm acts while arming
  decision.action    = ArmAction::Fire;               // Active: fire; Inactive: rehearsal (App decides which)
  decision.trimClock = true;                            // an accepted, fresh command trims like any other
  return decision;
}
```
Update the header's decision table and doc comment to include the three FIRE rows.

- [ ] **Step 4: Run to verify it passes** — `make test`. Expected: PASS.

- [ ] **Step 5: clang-format and commit**

```bash
/Users/andy/nrfenv/bin/clang-format -i src/arm_policy.hpp
git add src/arm_policy.hpp tests/test_arm_policy.cpp
git commit -m "FIRE: ArmAction::Fire and DecideCommand rows (Active fire, Inactive rehearse, Arming ignore)"
```

---

### Task 3: Crypto vectors — version 0x04, FIRE case, confirmId

**Files:**
- Modify: `class_mfs_1/tools/gen_access_vectors.py`
- Regenerate: `class_mfs_1/tests/access_vectors.hpp`, `class_app/test/access_vectors.dart`
- Test: `class_mfs_1/tests/test_access_control.cpp` (or the existing access test) consumes the new vectors; app `test/access_test.dart` likewise.

**Interfaces:**
- Produces: a `fire_slotN` command vector under version `0x04`; a `confirmId` vector list `(day, slot, n, event, confirmId[16])`.
- Consumes: existing generator structure (`seal`, `day_key`).

- [ ] **Step 1: Write the failing test** — add to the firmware access test a check that trial-decrypt of the FIRE vector's `onAir` yields `isFire == true`, and that a firmware `confirmId(dayKey, n, event)` (Task 4) equals the vector. Add the analogous Dart expectations. Both fail until the generator emits the vectors and Task 4 exists.

- [ ] **Step 2: Run to verify it fails** — `make test` (firmware) and `cd class_app && flutter test test/access_test.dart`.

- [ ] **Step 3: Implement the generator**

Set `PROTOCOL_VERSION = 0x04` and add `LABEL_CONFIRM = 0x06`. Add a FIRE case (byte 1 = 0, OMG magic, minute):
```python
("fire_slot1_n1", 256, 1, 1, bytes([0x4F, 0x00, 0x4D, 0x47, 0x1E, 0x02, 0x00, 0x00])),
```
Add the derivation and a vector table:
```python
def confirm_id(dk: bytes, n: int, event: int) -> bytes:
    return hmac256(dk, struct.pack(">I", n) + bytes([event]) + bytes([LABEL_CONFIRM]))[:16]

CONFIRM_CASES = [("fire_recv_slot1_n1", 256, 1, 1, 0x01)]  # event 0x01 = FIRE received
```
Emit a `M_CONFIRMS[]` C++ array and a `kConfirmVectors` Dart list alongside the command vectors, each carrying `dayKey`, `n`, `event`, and the 16-byte `confirmId`. Run `python3 tools/gen_access_vectors.py`.

- [ ] **Step 4: Run to verify it passes** — after Task 4, `make test` and the Dart access test PASS.

- [ ] **Step 5: Commit** (generated files included, never hand-edited)

```bash
git add tools/gen_access_vectors.py tests/access_vectors.hpp ../class_app/test/access_vectors.dart tests/test_access_control.cpp
git commit -m "FIRE: access vectors at protocol 0x04, FIRE command and confirmId known-answer cases"
```

---

### Task 4: Firmware confirmId derivation

**Files:**
- Modify: `class_mfs_1/src/access_keys.hpp`, `class_mfs_1/src/access_keys.cpp`
- Test: `class_mfs_1/tests/test_access_control.cpp` (against Task 3 vectors)

**Interfaces:**
- Produces: `void ConfirmId(const uint8_t dayKey[32], uint32_t n, uint8_t event, uint8_t out[16]);` (or a `std::array` return, matching the file's existing style).
- Consumes: the HMAC-SHA256 primitive already used for `rotatingId`.

- [ ] **Step 1: Write the failing test** — for each `M_CONFIRMS[]` vector, compute `ConfirmId(dayKey, n, event, out)` and `ASSERT` the 16 bytes match.

- [ ] **Step 2: Run to verify it fails** — `make test`. Expected: FAIL (no `ConfirmId`).

- [ ] **Step 3: Implement** — mirror `rotatingId`: `HMAC(dayKey, n(4 BE) ‖ event(1) ‖ 0x06)`, take the first 16 bytes. Add label `M_LABEL_CONFIRM { 0x06 }` next to the existing labels.

- [ ] **Step 4: Run to verify it passes** — `make test`. Expected: PASS (Task 3 vectors now satisfied on the firmware side).

- [ ] **Step 5: clang-format and commit**

```bash
/Users/andy/nrfenv/bin/clang-format -i src/access_keys.hpp src/access_keys.cpp
git add src/access_keys.hpp src/access_keys.cpp
git commit -m "FIRE: confirmId derivation (label 0x06), verified against vectors"
```

---

### Task 5: FireSequence — the host-tested countdown

**Files:**
- Create: `class_mfs_1/src/fire_sequence.hpp`, `class_mfs_1/src/fire_sequence.cpp`
- Create: `class_mfs_1/tests/test_fire_sequence.cpp`, add target to the test Makefile.

**Interfaces:**
- Produces:
  - `enum class FireState { Idle, CountingDown, Fired };`
  - `class FireSequence` with:
    - `void Start(bool live, uint32_t nowMs);` — begins the 10 s countdown; `live` = Active (fires pins) vs rehearsal (LED B only).
    - `void Service(uint32_t nowMs, bool fireSwitchFaulty);` — advances; at expiry sets `Fired` unless a fault aborts.
    - `bool FireLatched() const;` — one-shot, true once fired **live**; consumed by `App::updateOutputState()`.
    - `bool RehearsalFired() const;` — true once fired in rehearsal (LED B).
    - `bool Aborted() const;` / `FireState State() const;`
    - `bool AcceptsDisarm() const;` — false while `CountingDown` (Disarm refused).
  - `constexpr uint32_t M_FIRE_COUNTDOWN_MS { 10000 };`
- Consumes: nothing hardware; pure, `nowMs` injected like `ArmingSequence`.

- [ ] **Step 1: Write the failing tests**

```cpp
TEST(live_fires_after_10s) {
  FireSequence f; f.Start(/*live=*/true, 0);
  f.Service(9999, false); ASSERT_FALSE(f.FireLatched());
  f.Service(10000, false); ASSERT_TRUE(f.FireLatched());
  ASSERT_EQ(f.State(), FireState::Fired);
}
TEST(disarm_refused_during_countdown) {
  FireSequence f; f.Start(true, 0);
  f.Service(5000, false); ASSERT_FALSE(f.AcceptsDisarm());
}
TEST(scanner_loss_does_not_abort) {           // App simply stops calling ServiceScannerHealth's fail-safe;
  FireSequence f; f.Start(true, 0);            // FireSequence has no scanner input, proving the exemption by design.
  f.Service(10000, false); ASSERT_TRUE(f.FireLatched());
}
TEST(fire_switch_fault_aborts_and_fails_safe) {
  FireSequence f; f.Start(true, 0);
  f.Service(5000, /*fireSwitchFaulty=*/true);
  ASSERT_TRUE(f.Aborted());
  f.Service(10000, false); ASSERT_FALSE(f.FireLatched()); // never fires after abort
}
TEST(rehearsal_lights_ledb_not_pins) {
  FireSequence f; f.Start(/*live=*/false, 0);
  f.Service(10000, false);
  ASSERT_FALSE(f.FireLatched());
  ASSERT_TRUE(f.RehearsalFired());
}
TEST(one_shot) {
  FireSequence f; f.Start(true, 0); f.Service(10000, false);
  ASSERT_TRUE(f.FireLatched());
  f.Service(20000, false); ASSERT_EQ(f.State(), FireState::Fired); // stays fired, no re-arm
}
```

- [ ] **Step 2: Run to verify it fails** — add the test target, `make test`. Expected: FAIL (class absent).

- [ ] **Step 3: Implement** the class per the interface, holding `m_state`, `m_live`, `m_deadline_ms`, `m_fired`, `m_rehearsal_fired`, `m_aborted`. `Service` aborts on `fireSwitchFaulty` while counting; at/after the deadline sets `Fired` and latches `m_fired` (live) or `m_rehearsal_fired` (rehearsal). `AcceptsDisarm()` returns `m_state != CountingDown`.

- [ ] **Step 4: Run to verify it passes** — `make test`. Expected: PASS.

- [ ] **Step 5: clang-format and commit**

```bash
/Users/andy/nrfenv/bin/clang-format -i src/fire_sequence.hpp src/fire_sequence.cpp
git add src/fire_sequence.hpp src/fire_sequence.cpp tests/test_fire_sequence.cpp Makefile
git commit -m "FIRE: host-tested FireSequence (unstoppable 10 s, fault aborts, one-shot, rehearsal)"
```

---

### Task 6: App integration — wire FireSequence into the output path

**Files:**
- Modify: `class_mfs_1/src/app.hpp`, `class_mfs_1/src/app.cpp`

**Interfaces:**
- Consumes: `DecideCommand()` → `ArmAction::Fire`; `FireSequence`; `ArmingSequence::ServiceScannerHealth()`.
- Produces: fire latch ORed into `updateOutputState()`; the App calls `m_fire.Service()` each tick and passes the fire-switch fault status.

- [ ] **Step 1 (no host test for App hardware paths — verified on bench; write the wiring carefully):** In `applyCommand()`, on `ArmAction::Fire`, call `m_fire.Start(live = (state == Active), nowMs)`. Persist `n+1` before starting (unchanged ordering). Trim the clock.
- [ ] **Step 2:** In `App::Run()` each tick: call `m_fire.Service(nowMs, m_output_switch.IsFaulty())`. If `m_fire.Aborted()`, run the ordinary fail-safe (isolate pins, disarm, `signalWarning()`), once.
- [ ] **Step 3:** In `updateOutputState()`: `m_output_active = m_arming.State() == Active && (m_detection_met || m_fire.FireLatched());`. When `m_fire.FireLatched()` first becomes true, treat it exactly as a trigger: latch the device Inactive when the output period ends (reuse the existing one-shot trigger path).
- [ ] **Step 4:** LED B: when `m_fire.RehearsalFired()` (Inactive rehearsal), pulse LED B as the "detonation" indicator, gated like the existing bench detection indicator.
- [ ] **Step 5:** Scanner-health exemption: guard the `ServiceScannerHealth()` fail-safe so it does **not** disarm while `m_fire.State() == CountingDown`. Everywhere else it is unchanged.
- [ ] **Step 6:** Disarm during countdown: in `applyCommand()`, if `!m_fire.AcceptsDisarm()`, log `Disarm ignored during fire countdown.` and do nothing (the fire proceeds).
- [ ] **Step 7: Build, clang-format, commit.** `west build -b nrf54l15dk/nrf54l05/cpuapp -p always -- -DEXTRA_CONF_FILE=credentials.conf`.

```bash
/Users/andy/nrfenv/bin/clang-format -i src/app.hpp src/app.cpp
git add src/app.hpp src/app.cpp
git commit -m "FIRE: wire FireSequence into App (OR fire latch, rehearsal LED B, scanner-loss exemption, disarm refused mid-countdown)"
```

---

### Task 7: Device advertising path (NEW capability)

**Files:**
- Create: `class_mfs_1/src/command_advertiser.hpp`, `class_mfs_1/src/command_advertiser.cpp`
- Modify: `class_mfs_1/prj.conf` (enable the peripheral/advertising Bluetooth role), `src/app.cpp`

**Interfaces:**
- Produces: `class CommandAdvertiser { void Burst(const uint8_t uuid[16], uint32_t durationMs); void Service(uint32_t nowMs); };` — advertises a single 128-bit service UUID for a short burst, then stops.

- [ ] **Step 1: Research first.** The device is scan-only today. Read how the app advertises the 16-byte UUID (`class_app` advertiser) so the on-air shape matches what a listener expects, and read a Zephyr peripheral advertising sample via the nordic MCP (`nordicsemi_search_sources`) — do NOT fetch from the web. Confirm the scanner and advertiser can coexist (the device must keep scanning; a brief advertiser burst must not starve the scan that arms/fault-checks depend on — but note the countdown is scanner-loss-exempt, so a short starve during the burst is acceptable).
- [ ] **Step 2:** Implement a minimal advertiser that puts the 16 bytes in the same field the app scans (service UUID / service data — match the app's advertised format exactly), bursts for a few seconds, then stops. No connectable advertising, no GATT.
- [ ] **Step 3:** Enable the required Kconfig in `prj.conf` (Bluetooth peripheral role / advertising) and confirm the build still fits and boots. Keep it off the power-critical path — advertising only happens on FIRE.
- [ ] **Step 4: Build, commit.**

```bash
git add src/command_advertiser.hpp src/command_advertiser.cpp src/app.cpp prj.conf
git commit -m "FIRE: minimal device advertising path for the confirmation burst"
```

---

### Task 8: Confirmation burst on FIRE accept

**Files:**
- Modify: `class_mfs_1/src/app.cpp`

**Interfaces:**
- Consumes: `access_keys::ConfirmId` (Task 4), `CommandAdvertiser` (Task 7), the accepted FIRE's sequence number `n`.

- [ ] **Step 1:** On `ArmAction::Fire` accepted (live **or** rehearsal), compute `ConfirmId(dayKey, n, 0x01, uuid)` and call `m_advertiser.Burst(uuid, M_CONFIRM_BURST_MS)`. Derive `dayKey` for the accepting slot (already available where the command authenticated).
- [ ] **Step 2:** Ensure the burst starts at the top of the countdown (confirm-on-receipt), not at detonation.
- [ ] **Step 3: Build, commit.**

```bash
git add src/app.cpp
git commit -m "FIRE: advertise authenticated confirmId burst on receipt"
```

---

### Task 9: Flutter protocol mirror — FIRE encode, version 0x04, confirmId

**Files:**
- Modify: `class_app/lib/protocol/mfs_protocol.dart`, `class_app/lib/protocol/access_keys.dart`
- Test: `class_app/test/protocol_test.dart`, `class_app/test/access_test.dart`

**Interfaces:**
- Produces: `Mfs1CommandType` gains nothing (FIRE is encoded as a distinguished reserved); add `Mfs1Command.fire({required int minuteOfDay})` or an `isFire` flag; `encodePlaintext` emits the OMG layout for FIRE. `kProtocolVersion = 0x04`. `deriveConfirmId(dayKey, n, event)` mirroring Task 4.

- [ ] **Step 1: Write the failing tests** — encode a FIRE command and assert bytes `4F 00 4D 47 <minute LE> .. ..`; assert `deriveConfirmId` matches `kConfirmVectors` (Task 3); assert the FIRE command vector's `onAir` round-trips.

- [ ] **Step 2: Run to verify it fails** — `flutter test`.

- [ ] **Step 3: Implement** — `kProtocolVersion = 0x04`; a fire branch in `encodePlaintext` writing the magic and minute (all other bytes zero); `deriveConfirmId` = first 16 bytes of `HMAC(dayKey, n BE32 ‖ event ‖ 0x06)` with `kLabelConfirm = 0x06`.

- [ ] **Step 4: Run to verify it passes** — `flutter test`.

- [ ] **Step 5: format and commit**

```bash
cd class_app && dart format -l 120 lib/protocol/mfs_protocol.dart lib/protocol/access_keys.dart test/protocol_test.dart test/access_test.dart
git add lib/protocol/mfs_protocol.dart lib/protocol/access_keys.dart test/protocol_test.dart test/access_test.dart
git commit -m "FIRE: Flutter protocol mirror — OMG encode, version 0x04, confirmId"
```

---

### Task 10: CommandBuilder — fireCommand and expectedConfirmId

**Files:**
- Modify: `class_app/lib/services/command_builder.dart`
- Test: `class_app/test/services_test.dart`

**Interfaces:**
- Produces: `Future<String> fireCommand(Mfs1Device device)` (builds the UUID, reserves `n`, persists `n+1` before returning, exactly like `armCommand`); `Future<String> expectedConfirmId(Mfs1Device device, int n)` (or return `n` from `fireCommand` so the caller can compute it). The listener (Task 12) needs the day key, `n` and event to compute the confirmId UUID string.

- [ ] **Step 1: Write the failing test** — `fireCommand` produces a UUID whose reversed bytes decode to a FIRE plaintext; a second call uses `n+1`; the sequence store advanced before the UUID was returned.
- [ ] **Step 2: fails** — `flutter test`.
- [ ] **Step 3: Implement** mirroring `armCommand`/`disarmCommand`, using the fire plaintext, and expose the `n` used plus a helper to derive the confirmId UUID string for that `n`.
- [ ] **Step 4: passes** — `flutter test`.
- [ ] **Step 5: format and commit**

```bash
dart format -l 120 lib/services/command_builder.dart test/services_test.dart
git add lib/services/command_builder.dart test/services_test.dart
git commit -m "FIRE: CommandBuilder.fireCommand and confirmId helper"
```

---

### Task 11: Fire button and red confirm-slider sheet

**Files:**
- Create: `class_app/lib/devices/mfs1/fire_button.dart`
- Modify: `class_app/lib/devices/mfs1/mfs1_screen.dart`
- Test: `class_app/test/mfs1_flow_test.dart`

**Interfaces:**
- Consumes: `CommandBuilder.fireCommand`, `Advertiser`, `ForegroundGuard`.
- Produces: a large red Fire button on the Arm page; tapping opens a modal red warning sheet with a confirm slider; sliding to the end reveals a Fire button; pressing it sends FIRE (foreground-only, like every Send).

- [ ] **Step 1: Write the failing widget tests** — the Fire button is red and present on the Arm page; tapping opens the sheet (a distinct warning colour, key `fireConfirmSheet`); the send Fire button is absent until the slider is dragged to the end (key `fireConfirmSlider` → `fireSendButton` appears); pressing `fireSendButton` calls `fireCommand` and starts one advert. Foreground-only: `fireSendButton` disabled when not resumed.
- [ ] **Step 2: fails** — `flutter test`.
- [ ] **Step 3: Implement** using `ClassTheme.signalRed` for the button and sheet accents; reuse the foreground/stop logic pattern from the existing Send path; the slider is a `Slider` or a custom confirm-slide that only enables Fire at ≥ 0.98. Two deliberate actions (open sheet, slide, press) — no single-tap fire.
- [ ] **Step 4: passes** — `flutter test`.
- [ ] **Step 5: format and commit**

```bash
dart format -l 120 lib/devices/mfs1/fire_button.dart lib/devices/mfs1/mfs1_screen.dart test/mfs1_flow_test.dart
git add lib/devices/mfs1/fire_button.dart lib/devices/mfs1/mfs1_screen.dart test/mfs1_flow_test.dart
git commit -m "FIRE: red Fire button with confirm-slider sheet on the Arm page"
```

---

### Task 12: Confirmation listener — scan for the confirmId

**Files:**
- Create: `class_app/lib/services/confirm_listener.dart`
- Modify: `class_app/lib/devices/mfs1/mfs1_screen.dart` (or the fire flow) to show the confirmation / timeout
- Test: `class_app/test/confirm_listener_test.dart`

**Interfaces:**
- Produces: `class ConfirmListener { Future<bool> awaitConfirm(String confirmUuid, {Duration timeout}); }` — scans BLE for a service UUID equal to `confirmUuid`, completes true on first sighting, false on timeout. Injected scanner interface so it is unit-testable with a fake, like `Advertiser`.

- [ ] **Step 1: Research first.** The app has only ever advertised (`ble_peripheral`). Scanning needs a central/scan capability — check whether `ble_peripheral` exposes scanning or a separate package (e.g. `flutter_blue_plus`) is required; confirm iOS background/foreground scan constraints. Record the choice in the task report. If a new package is added, pin the version and justify it against the project's dependency-floor discipline.
- [ ] **Step 2: Write the failing test** with a fake scanner that emits a matching / non-matching UUID; assert true on match, false on timeout.
- [ ] **Step 3: Implement** behind the injected interface. After `fireCommand`, the fire flow stops advertising, computes the confirmId UUID for the sent `n`, and calls `awaitConfirm`; the UI shows "Confirmed — countdown running" or "No confirmation heard (the device may still fire)".
- [ ] **Step 4: passes** — `flutter test`.
- [ ] **Step 5: format and commit**

```bash
dart format -l 120 lib/services/confirm_listener.dart lib/devices/mfs1/mfs1_screen.dart test/confirm_listener_test.dart
git add lib/services/confirm_listener.dart lib/devices/mfs1/mfs1_screen.dart test/confirm_listener_test.dart pubspec.yaml
git commit -m "FIRE: app scans for the authenticated confirmId and reports receipt"
```

---

### Task 13: Docs and bench checklist

**Files:**
- Modify: `class_mfs_1/CLAUDE.md`, `class_mfs_1/docs/superpowers/plans/2026-09-13-bench-checklist.md`

- [ ] **Step 1:** In `CLAUDE.md`, add the FIRE exceptions: the scan-only rule gains the confirmId-burst documented exception; the always-fail-safe rule gains the unstoppable-countdown exception (Disarm and scanner loss do not abort; fire-path fault does). Point both at the spec.
- [ ] **Step 2:** Add a bench-checklist FIRE section: (a) armed FIRE fires after 10 s; (b) Disarm during the 10 s is refused and the device still fires; (c) scanner unplugged during the 10 s — still fires; (d) fire-switch fault injected during the 10 s — aborts, disarms, warns; (e) disarmed FIRE — LED B only, pins never move (scope with a meter); (f) confirmId burst seen by the app/observer; (g) arming FIRE ignored.
- [ ] **Step 3: commit**

```bash
git add CLAUDE.md docs/superpowers/plans/2026-09-13-bench-checklist.md
git commit -m "FIRE: document the scan-only and fail-safe exceptions and add bench checks"
```

---

## Self-Review Notes

- **Spec coverage:** §2 (Tasks 1, 9), §2.3 device table (Tasks 2, 6), §3 confirmId (Tasks 3, 4, 8, 9, 12), §4 unstoppable countdown (Tasks 5, 6), §5 app UI (Tasks 11, 12), §7 impact rows all mapped. Open points in §6 (freshness tightening, slot restriction) are deliberately deferred and NOT implemented here.
- **Ordering:** pure/host-tested logic and the shared vectors (1–5, 9, 10) land before hardware wiring (6–8) and the app UI (11, 12). Vectors (Task 3) are the contract both sides meet.
- **Type consistency:** `Command.isFire`, `ArmAction::Fire`, `FireSequence::FireLatched/RehearsalFired/AcceptsDisarm`, `confirmId(n, event)` with label `0x06`, protocol `0x04`, magic `4F 4D 47` — used identically across firmware and app tasks.
- **Bench-only:** Tasks 6–8 have no host tests (hardware paths); they are verified on the bench per Task 13's checklist, not by `make test`.
