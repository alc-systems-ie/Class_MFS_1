# Command Types Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Separate Arm, Disarm and Settings command types so an engineer can arm and disarm a device without knowing its settings, and reach settings in the app only through a confirmed disarm.

**Architecture:** A 2-bit command type in plaintext byte 1 bits 6–7 replaces the arm bit; protocol version (CCM associated data) goes `0x02` → `0x03`. `DecideCommand()` in `src/arm_policy.hpp` stays the single decision point and gains a replay-state action. The Flutter app splits its one screen into an Arm page and a Settings page gated by a disarm confirmation prompt, and its advertiser stops counting down when advertising never started.

**Tech Stack:** C++20 / Zephyr NCS v3.2.4 (nRF54L05), host tests with g++ + OpenSSL (`make test`), Python `cryptography` vector generator, Flutter (`ble_peripheral` 2.4.0, `pointycastle`, `flutter_test`).

**Spec:** `docs/superpowers/specs/2026-09-14-command-types-amendment.md` (binding), amending `docs/superpowers/specs/2026-09-12-app-control-design.md` and `docs/tan-scheme.md`.

## Global Constraints

- Firmware house style: `~/.claude/CLAUDE.md` — C++20, `alc` namespace, `enum class` always, `M_` constants, `m_` members, camelCase locals, PascalCase public methods, no literals in function calls, `LOG_ERR` ends with `!`. Run `/Users/andy/nrfenv/bin/clang-format -i` on every C++ file touched.
- Command type field: plaintext byte 1 bits 6–7. `00` Reserved — **rejects** (decode fails, verdict Malformed, not consumed). `01` Settings. `10` Arm. `11` Disarm.
- Plaintext byte 0 bit 0 is reserved and ignored. Bytes 6–7, byte 1 bits 6–7's neighbours (minute bits 11–15) stay unvalidated.
- Mode `3` rejects on every command type. Minute ≥ 1440 rejects on every command type.
- `M_PROTOCOL_VERSION` / `kProtocolVersion` = `0x03`.
- Arm and Disarm carry no settings: the app sends zero in delay, activations−1, mode, cooldown and sensitivity; the device ignores those fields for Arm and Disarm.
- Decision table (amendment §2.2): Active+Disarm → Disarm (trim yes); Active+Arm → ReplayArmed (no trim); Active+Settings → ReplayArmed (no trim); Inactive+Arm → Arm with stored settings (trim yes, no settings, no mode); Inactive+Disarm → Disarm, i.e. the ordinary deactivation (trim yes); Inactive+Settings → Tune (settings yes, mode slot 0 only, trim yes).
- Persist before acting, silence on failed authentication, freshness and lockout are unchanged.
- The app never stores device settings (owner decision, Option A).
- Disarm prompt text, verbatim: title **"Confirm LED A shows the device is disarmed"**; body **"A slow flash for 3 seconds (or a double blink if a pending trigger was cancelled).\n\nIf the device is not showing disarmed, please move closer and confirm that the correct device has been selected. If the device does not respond, the battery may be discharged."**; buttons **"Not seen"** and **"Disarmed — open settings"**.
- Never commit `credentials.conf` or `class_app/lib/services/bench_credentials.dart`. No pushes. No flashing, J-Link or RTT — hardware steps go in the bench checklist.
- `class_app` is its own git repository at `/Users/andy/nordic/ncs/v3.2.4/class_app`; commit app changes there.
- Commit trailer on every commit:
  ```
  Co-Authored-By: Claude Opus 5 (1M context) <noreply@anthropic.com>
  Claude-Session: https://claude.ai/code/session_01UySHgbzqJcwKqpRntC1PtN
  ```

---

### Task 1: Firmware protocol, vectors and arm policy (host-tested)

**Files:**
- Modify: `src/mfs_protocol.hpp`, `src/mfs_protocol.cpp`, `src/arm_policy.hpp`
- Modify: `tools/gen_access_vectors.py`; regenerate `src/access_vectors.hpp` (also rewrites `../class_app/test/access_vectors.dart` — leave that file uncommitted for Task 4)
- Modify: `tests/test_protocol.cpp`, `tests/test_arm_policy.cpp`, `tests/test_access_control.cpp`
- Modify (compile only): `src/app.cpp` line ~1198 log statement, so the firmware still builds — full App change is Task 2

**Interfaces:**
- Produces: `enum class alc::protocol::CommandType : uint8_t { Reserved = 0, Settings = 1, Arm = 2, Disarm = 3 };` `protocol::Command::type` (replaces `armActive`); `M_TYPE_SHIFT { 6 }`, `M_TYPE_MASK { 0x03 }`; `enum class ArmAction : uint8_t { Ignore, Disarm, Arm, Tune, ReplayArmed };` `ArmDecision DecideCommand(bool armed, bool fromNetworkManager, const protocol::Command& command)`; `const char* protocol::CommandTypeName(CommandType type)`.

- [ ] **Step 1: Write failing protocol tests.** In `tests/test_protocol.cpp` `run_command_codec_tests()`, replace every `armActive` use:

```cpp
  // Round trip, every field away from its default.
  command.type            = CommandType::Settings;
  ...
  assert(decoded.type == CommandType::Settings);
  ...
  // Byte 0: bit 0 is reserved and encodes as zero; delay code in bits 1-7.
  assert(plaintext[M_PT_ARM_DELAY] == (119 << 1));

  // Byte 1: activations - 1 low nibble, mode bits 4-5, type bits 6-7.
  assert(plaintext[M_PT_ACTIVATIONS_MODE] == (0x0F | (1 << 4) | (1 << 6)));

  // Every type round-trips.
  for (CommandType type : { CommandType::Settings, CommandType::Arm, CommandType::Disarm }) {
    command.type = type;
    EncodeCommand(command, plaintext);
    assert(DecodeCommand(plaintext, decoded) && decoded.type == type);
  }
  command.type = CommandType::Settings;

  // AN ALL-ZERO PLAINTEXT IS NOT A COMMAND. Type 00 is reserved and rejects, so
  // zeros can never mean "apply all-zero settings" (amendment section 2.1).
  for (uint8_t& byte : plaintext) {
    byte = 0;
  }
  assert(!DecodeCommand(plaintext, decoded));
```

Keep the mode-3 and minute-1440 rejection cases (they encode `command` with `type = Settings`). Add: mode 3 with `type = Arm` also rejects. Change the "reserved bits" tolerance case: it currently ORs `0xC0` into byte 1 — that is now the type field, so instead OR `0x01` into byte 0 (reserved arm bit) and keep the minute high-bit `0xF8` and bytes 6–7 `0xAA 0xBB`; assert it still decodes with `decoded.type == CommandType::Settings`.

- [ ] **Step 2: Write failing arm policy tests.** Replace the body of `tests/test_arm_policy.cpp` `run_arm_policy_tests()`:

```cpp
  using namespace alc;

  constexpr bool M_BOTH_SLOT_KINDS[] { false, true };
  protocol::Command command;
  ArmDecision decision;

  // Settings fields deliberately non-default, so a leak into Arm or Disarm shows.
  command.activations = 5;
  command.delayCode   = 127;
  command.mode        = protocol::Mode::ReportOnly;

  for (bool fromNetworkManager : M_BOTH_SLOT_KINDS) {
    // ARMED + Disarm: disarm and trim, nothing else.
    command.type = protocol::CommandType::Disarm;
    decision     = DecideCommand(true, fromNetworkManager, command);
    assert(decision.action == ArmAction::Disarm);
    assert(!decision.applySettings && !decision.applyMode && decision.trimClock);

    // ARMED + Arm or Settings: the state is unchanged, LED A replays Armed.
    // No settings, no mode, not even a trim - armed, only a disarm acts.
    for (protocol::CommandType type : { protocol::CommandType::Arm, protocol::CommandType::Settings }) {
      command.type = type;
      decision     = DecideCommand(true, fromNetworkManager, command);
      assert(decision.action == ArmAction::ReplayArmed);
      assert(!decision.applySettings && !decision.applyMode && !decision.trimClock);
    }

    // INACTIVE + Arm: arm with the STORED settings. The command's are not applied.
    command.type = protocol::CommandType::Arm;
    decision     = DecideCommand(false, fromNetworkManager, command);
    assert(decision.action == ArmAction::Arm);
    assert(!decision.applySettings && !decision.applyMode && decision.trimClock);

    // INACTIVE + Disarm: the ordinary deactivation, which also ends tuning.
    command.type = protocol::CommandType::Disarm;
    decision     = DecideCommand(false, fromNetworkManager, command);
    assert(decision.action == ArmAction::Disarm);
    assert(!decision.applySettings && !decision.applyMode && decision.trimClock);

    // RESERVED never acts, armed or not (decode rejects it first; defence in depth).
    command.type = protocol::CommandType::Reserved;
    for (bool armed : M_BOTH_SLOT_KINDS) {
      decision = DecideCommand(armed, fromNetworkManager, command);
      assert(decision.action == ArmAction::Ignore);
      assert(!decision.applySettings && !decision.applyMode && !decision.trimClock);
    }
  }

  // INACTIVE + Settings: tune; mode only from slot 0.
  command.type = protocol::CommandType::Settings;
  decision     = DecideCommand(false, false, command);
  assert(decision.action == ArmAction::Tune && decision.applySettings && !decision.applyMode && decision.trimClock);
  decision = DecideCommand(false, true, command);
  assert(decision.action == ArmAction::Tune && decision.applySettings && decision.applyMode);

  printf("arm policy: OK\n");
```

- [ ] **Step 3: Update `tests/test_access_control.cpp`.** `buildCommand(..., bool arm, ...)` becomes `buildCommand(..., protocol::CommandType type, ...)` setting `command.type = type`. Every call passing `true` passes `protocol::CommandType::Arm`; `false` passes `protocol::CommandType::Settings`. Line ~82 asserts `evaluation.command.type == protocol::CommandType::Arm`. In the "Malformed plaintext not consumed" block set `plaintext[1] = 0x40; // type Settings, so only the minute is wrong`. Add a new block after it:

```cpp
  // Reserved command type (00) is Malformed and NOT consumed - an authentic
  // all-zero plaintext does nothing.
  {
    PersistSpy spy;
    AccessControl access(access::vectors::M_DEVICE_ID, access::vectors::M_SECRET, &persistSpy, &spy);
    DeviceClock clock { syncedClock() };
    uint8_t dayKey[access::M_DAY_KEY_BYTES] {};
    uint8_t plaintext[protocol::M_PLAINTEXT_BYTES] {};

    assert(access::DeriveDayKey(access::vectors::M_SECRET, access::vectors::M_DEVICE_ID, 256, 1, dayKey) == 0);
    plaintext[4] = static_cast<uint8_t>(M_MINUTE_0500 & 0xFF);
    plaintext[5] = static_cast<uint8_t>(M_MINUTE_0500 >> 8);
    assert(access::SealCommand(dayKey, access::vectors::M_DEVICE_ID, 256, 1, 0, plaintext, onAir) == 0);
    assert(access.Evaluate(onAir, sizeof(onAir), clock, 0).verdict == Verdict::Malformed);
    assert(access.State().next[1] == 0);
    assert(access.ConsecutiveFailures() == 0);
  }
```

(If `M_MINUTE_0500` is not the constant name in that file, use the file's existing 05:00 minute constant.)

- [ ] **Step 4: Run `make test`; expect compile failure** (`CommandType` / `ReplayArmed` / `type` undefined).

- [ ] **Step 5: Implement the protocol.** In `src/mfs_protocol.hpp`:
  - `M_PROTOCOL_VERSION { 0x03 }`, with the comment extended: "0x03 since 2026-09-14: the command type field. A 0x02 payload fails authentication instead of having its reserved bits read as a type."
  - Comment on `M_PT_ARM_DELAY`: `// bit 0 reserved (was arm), bits 1-7 delay code`; on `M_PT_ACTIVATIONS_MODE`: `// bits 0-3 activations - 1, bits 4-5 mode, bits 6-7 command type`.
  - Remove `M_ARM_BIT`. Add `constexpr uint8_t M_TYPE_SHIFT { 6 };` and `constexpr uint8_t M_TYPE_MASK { 0x03 };`.
  - Add, above `Command`:

```cpp
  /**
   * @brief What a command asks for. See the command types amendment section 2.
   *
   * Arm and Disarm carry NO settings - the device ignores every settings field in
   * them, so an engineer can arm or disarm without knowing the device's tuning.
   * Reserved (00) rejects, so an all-zero plaintext is never a command.
   */
  enum class CommandType : uint8_t {
    Reserved = 0, ///< Rejected by DecodeCommand().
    Settings = 1, ///< Apply the settings fields. Acted on only while Inactive.
    Arm      = 2, ///< Be Active with the settings already stored.
    Disarm   = 3, ///< Be Inactive.
  };

  /** @brief Short name for logs. */
  const char* CommandTypeName(CommandType type);
```

  - In `Command`, replace `bool armActive { false };` with `CommandType type { CommandType::Reserved };`. Update the `DecodeCommand` doc: "False for a reserved command type, a reserved mode or a minute outside 0-1439."

  In `src/mfs_protocol.cpp`: decode `decoded.type = static_cast<CommandType>((plaintext[M_PT_ACTIVATIONS_MODE] >> M_TYPE_SHIFT) & M_TYPE_MASK);` (drop the arm bit line) and, before the mode check:

```cpp
    // Type 00 is reserved and refused, so an all-zero plaintext does nothing -
    // it can never be read as "apply all-zero settings".
    if (decoded.type == CommandType::Reserved) { return false; }
```

  Update the trailing comment to "Plaintext bytes 6-7, byte 0 bit 0 and minute bits 11-15 are deliberately not examined." Encode: byte 0 = `(command.delayCode & M_DELAY_MASK) << M_DELAY_SHIFT`; byte 1 ORs `(static_cast<uint8_t>(command.type) & M_TYPE_MASK) << M_TYPE_SHIFT`. Replace `plaintext[6] = 0; plaintext[7] = 0;` literals with named `M_PT_EXTENSION { 6 }` / `M_PT_EXTENSION_BYTES { 2 }` constants if convenient (optional). Add:

```cpp
  const char* CommandTypeName(CommandType type)
  {
    switch (type) {
      case CommandType::Settings:
        return "Settings";
      case CommandType::Arm:
        return "Arm";
      case CommandType::Disarm:
        return "Disarm";
      default:
        return "Reserved";
    }
  }
```

- [ ] **Step 6: Implement the policy.** Replace `src/arm_policy.hpp` enum and function:

```cpp
  /** @brief What an accepted command is allowed to do. */
  enum class ArmAction : uint8_t {
    Ignore,      ///< Reserved type. NOTHING happens (decode already rejects it).
    Disarm,      ///< -> Inactive. From Active the only state change a command can make; from Inactive it ends tuning.
    Arm,         ///< Inactive -> Active with the STORED settings.
    Tune,        ///< Inactive stays Inactive; the command's settings applied for tuning.
    ReplayArmed, ///< Armed, and the command does not disarm. State unchanged; LED A replays Armed.
  };
```

`ArmDecision` unchanged. Doc comment on `DecideCommand` rewritten to state the §2.2 table, keeping the "THE SINGLE PATH" heading, the "only two ways out of the armed state: a disarm command, or firing" paragraph and "App::applyCommand() must act on this decision and on nothing else." Body:

```cpp
  inline ArmDecision DecideCommand(bool armed, bool fromNetworkManager, const protocol::Command& command)
  {
    ArmDecision decision {};

    if (command.type == protocol::CommandType::Reserved) { return decision; }

    if (command.type == protocol::CommandType::Disarm) {
      // Armed or not. From Inactive it is the ordinary deactivation, so a Disarm
      // sent blind to an Inactive device is harmless and ends any tuning session.
      decision.action    = ArmAction::Disarm;
      decision.trimClock = true;
      return decision;
    }

    if (armed) {
      // Arm or Settings while armed: no settings, no mode, no trim, no re-arm.
      // LED A replays Armed so an engineer who did not know the state learns it.
      decision.action = ArmAction::ReplayArmed;
      return decision;
    }

    decision.trimClock = true;
    if (command.type == protocol::CommandType::Arm) {
      // The command carries no settings; arming uses those already stored.
      decision.action = ArmAction::Arm;
      return decision;
    }

    decision.action        = ArmAction::Tune;
    decision.applySettings = true;
    decision.applyMode     = fromNetworkManager;
    return decision;
  }
```

- [ ] **Step 7: Regenerate vectors.** In `tools/gen_access_vectors.py` set `PROTOCOL_VERSION = 0x03` and replace `CASES`:

```python
CASES = [
    # (name, day, slot, n, plaintext). Byte 1 bits 6-7: 01 Settings, 10 Arm, 11 Disarm.
    ("arm_slot1_n0", 256, 1, 0, bytes([0x00, 0x80, 0, 0, 0x1E, 0x02, 0x00, 0x00])),
    ("disarm_slot7_n15", 256, 7, 15, bytes([0x00, 0xC0, 0, 0, 0x9F, 0x05, 0xAA, 0xBB])),
    ("settings_slot0_n3", 257, 0, 3, bytes([0x04, 0x62, 128, 200, 0x00, 0x00, 0x00, 0x00])),
]
```

(settings_slot0_n3: delay code 2, 3 activations, mode Report only, Settings type, cooldown 128, sensitivity 200, minute 0.) Run `python3 tools/gen_access_vectors.py` from `class_mfs_1`. Confirm `src/access_vectors.hpp` changed and `../class_app/test/access_vectors.dart` was written.

- [ ] **Step 8: Keep the firmware compiling.** In `src/app.cpp` `handleCommandCandidate` change the log to print the type instead of the arm bit: `"Command slot %u n %u: %s, delay %u s, activations %u, mode %u, cooldown %u s, threshold %u LSB, minute %u."` with `protocol::CommandTypeName(evaluation.command.type)`. In `applyCommand` add `case ArmAction::ReplayArmed:` alongside `Ignore` handling only as far as needed to compile (Task 2 does the real work) — simplest: leave `applyCommand` otherwise untouched; the `default: return;` already covers the new enumerator. Grep `src tests` for any remaining `armActive`; there must be none.

- [ ] **Step 9: Run `make test`; expect all suites OK.** Then build the firmware to prove it compiles: `west build -b nrf54l15dk/nrf54l05/cpuapp -d build-task1 -p always -- -DEXTRA_CONF_FILE=credentials.conf` (if `credentials.conf` is missing, build without the extra conf file). Delete `build-task1` afterwards. Do not flash.

- [ ] **Step 10: clang-format and commit** (firmware repo only):

```bash
/Users/andy/nrfenv/bin/clang-format -i src/mfs_protocol.hpp src/mfs_protocol.cpp src/arm_policy.hpp src/app.cpp tests/test_protocol.cpp tests/test_arm_policy.cpp tests/test_access_control.cpp
make test
git add src/mfs_protocol.hpp src/mfs_protocol.cpp src/arm_policy.hpp src/app.cpp src/access_vectors.hpp tools/gen_access_vectors.py tests/test_protocol.cpp tests/test_arm_policy.cpp tests/test_access_control.cpp
git commit -m "Protocol 0x03: an explicit command type replaces the arm bit"
```

---

### Task 2: App command path acts on the new decision (firmware)

**Files:**
- Modify: `src/app.cpp` (`App::applyCommand`)
- Modify: `src/led_sequencer.hpp` only if a doc comment references "armed, command ignored"

**Interfaces:**
- Consumes: `ArmAction::{Ignore, Disarm, Arm, Tune, ReplayArmed}`, `ArmDecision`, `protocol::CommandTypeName()` from Task 1.

- [ ] **Step 1: Rewrite the top of `applyCommand`.** Replace the `Ignore` early return with:

```cpp
    // THE SINGLE PATH. Everything below acts on `decision` and on nothing else -
    // see DecideCommand(). On command, an armed device only ever disarms.
    if (decision.action == ArmAction::Ignore) {
      LOG_WRN("Command slot %u n %u has a reserved type - ignored.", evaluation.slot, evaluation.n);
      return;
    }

    // Armed, and not a disarm: nothing changes - no settings, mode or trim - but
    // LED A replays Armed so an engineer who did not know the state learns it.
    // The command authenticated, so this is not a breach of silence on failure.
    if (decision.action == ArmAction::ReplayArmed) {
      LOG_INF("Armed: %s from slot %u n %u changes nothing - replaying Armed.", protocol::CommandTypeName(command.type), evaluation.slot,
              evaluation.n);
      playLedPattern(LedPattern::Armed);
      return;
    }
```

- [ ] **Step 2: Disarm case.** Keep `setArmState(ArmState::Inactive)` and the pattern choice. Replace its comment with: "Armed or not. From Active this is the only state change a command can make; from Inactive it is the same deactivation - count, latch, cooldown cleared, ADXL367 to standby - which ends a tuning session. A disarm carries no settings." Before writing, read `setArmState(ArmState::Inactive)` and `disableAccelerometer()` and confirm both are safe to call when already Inactive with the ADXL367 already in standby (no error log, no double timer stop failure). If either would log an error or fail in that case, guard it inside `setArmState`'s Inactive branch and say so in the report.

- [ ] **Step 3: Arm case.** Replace the settings comment so it states the command carries no settings and arming uses `m_settings` as stored. No other change: `decision.applySettings` is false for Arm, so the `if (decision.applySettings)` block already skips.

- [ ] **Step 4: Tune case** unchanged. Update the stale comment in the settings block if it mentions the arm bit.

- [ ] **Step 5: Grep** `src/` for `arm bit`, `armActive`, `only a disarm is accepted while armed` and fix remaining stale comments/logs.

- [ ] **Step 6: Verify.** `make test` passes. Firmware builds: `west build -b nrf54l15dk/nrf54l05/cpuapp -d build-task2 -p always -- -DEXTRA_CONF_FILE=credentials.conf`, zero warnings from `src/app.cpp`; delete `build-task2`. Do not flash.

- [ ] **Step 7: clang-format `src/app.cpp`, commit:** `git commit -m "Act on command types: replay Armed, Inactive disarm deactivates, arm keeps stored settings"`.

---

### Task 3: Project docs and bench checklist

**Files:**
- Modify: `CLAUDE.md` (project), `docs/superpowers/plans/2026-09-13-bench-checklist.md`, `docs/superpowers/specs/2026-09-12-app-control-design.md` §6.7 table row

**Interfaces:** none (docs).

- [ ] **Step 1: `CLAUDE.md` access rules.** Replace the "Armed, the only command is disarm…" bullet with: "**Arm, Disarm and Settings are separate command types** (`docs/superpowers/specs/2026-09-14-command-types-amendment.md`). Arm and Disarm carry no settings, so an engineer can arm or disarm without knowing the device's tuning; settings are applied only by a Settings command while Inactive. **Armed, the only state change is disarm** — Arm or Settings to an armed device changes nothing and replays the Armed pattern on LED A. **Triggers are one-shot** and latch the device Inactive, so disarm and trigger are the only two ways out of the armed state. `DecideCommand()` (`src/arm_policy.hpp`) is the single place this is decided — never add a second path in `App`." In the Settled table row "Access", append "; protocol version 0x03 with an explicit command type". Add a bullet: "**The app never stores device settings** (owner decision 2026-09-14, amendment §4) — a lost phone must not become a map of every sensor's tuning."

- [ ] **Step 2: Design spec §6.7.** Replace the table row `| *(armed, command ignored)* | *nothing* | — |` with `| *(armed, Arm or Settings received)* | rapid flash — replays Armed; nothing changes | 3 s |` and add `| *(Inactive, Disarm received)* | slow flash — the ordinary disarm | 3 s |`. Change "**No flash means the command did not land.**" bullet to keep its meaning.

- [ ] **Step 3: Bench checklist.** Read the whole file. Add a new section before §9 titled `## 5a. Command types (plan 2026-09-14) — do this first on the new build`, with checkbox items:
  1. Flash with `--recover`, provision the clock.
  2. Inactive, Arm page Send Armed → rapid flash; RTT `Command slot 1 n X: Arm` and `Applied:` shows the **stored** settings (not app defaults).
  3. Armed, Send Armed again → rapid flash replay; RTT `Armed: Arm from slot 1 n X changes nothing - replaying Armed.`; still armed (tap: fires).
  4. Armed, Send Disarmed → slow flash; the app prompt appears with the fault-finding text; press Disarmed — open settings.
  5. Settings page: sliders at defaults; Send → single blink; LED B simulates at those settings.
  6. Restore defaults resets the sliders; nothing is sent until Send.
  7. Back → Arm page shows Armed. Send → rapid flash; RTT `Applied:` shows the settings from step 5.
  8. Inactive, Send Disarmed → slow flash replay, LED B stops if tuning.
  9. Prompt "Not seen" stays on the Arm page, and Settings is unreachable without a confirmed disarm.
  10. Turn Bluetooth off on the Mac, Send → an on-screen advertising error, no countdown, no prompt.
  11. Old-app regression: none needed (nothing deployed); note that a `0x02` command is silent.

  Then in §5 and §6, reword any step that says "Armed off + Send" to tune or "arm with settings" to the new flow (Disarm → Settings page → Send settings → Arm page → Send Armed), and any step expecting "armed, command ignored — nothing" to expect the Armed replay. Keep item numbers stable where possible.

- [ ] **Step 4: Commit:** `git commit -m "Docs: command types in CLAUDE.md, LED scheme and the bench checklist"`.

---

### Task 4: App protocol mirror and command builder (Dart, host-tested)

**Files (in `/Users/andy/nordic/ncs/v3.2.4/class_app`):**
- Modify: `lib/protocol/mfs_protocol.dart`, `lib/protocol/access_keys.dart` (`kProtocolVersion`), `lib/services/command_builder.dart`
- Modify: `test/protocol_test.dart`, `test/services_test.dart`; commit the regenerated `test/access_vectors.dart` from Task 1

**Interfaces:**
- Produces: `enum Mfs1CommandType { reserved, settings, arm, disarm }` (index = wire value); `Mfs1Command({required Mfs1CommandType type, required int minuteOfDay, int activations = 1, int cooldownByte = 0, int sensitivityByte = 0, int delayCode = 0, Mfs1Mode mode = Mfs1Mode.triggerOnly})`; `class Mfs1Settings({required int activations, required int cooldownByte, required int sensitivityByte, int delayCode = 0})` with `static const Mfs1Settings defaults = Mfs1Settings(activations: 1, cooldownByte: 0, sensitivityByte: 143, delayCode: 0);`; `CommandBuilder.armCommand(Mfs1Device) → Future<String>`, `CommandBuilder.disarmCommand(Mfs1Device) → Future<String>`, `CommandBuilder.settingsCommand(Mfs1Device, Mfs1Settings) → Future<String>`, `CommandBuilder.networkManagerSettingsCommand(Mfs1Device, Mfs1Settings, Mfs1Mode) → Future<String>`.

- [ ] **Step 1: Failing tests.** Replace `test/protocol_test.dart` "plaintext encodes exactly as the vectors expect" with:

```dart
  test('plaintext encodes exactly as the vectors expect', () {
    // arm_slot1_n0: Arm carries no settings - only the type and 09:02 UTC.
    expect(encodePlaintext(const Mfs1Command(type: Mfs1CommandType.arm, minuteOfDay: 9 * 60 + 2)), kCommandVectors[0].plaintext);
    // settings_slot0_n3: delay 2, 3 activations, Report only, cooldown 128, sensitivity 200, minute 0.
    expect(
      encodePlaintext(const Mfs1Command(
        type: Mfs1CommandType.settings,
        delayCode: 2,
        activations: 3,
        mode: Mfs1Mode.reportOnly,
        cooldownByte: 128,
        sensitivityByte: 200,
        minuteOfDay: 0,
      )),
      kCommandVectors[2].plaintext,
    );
  });

  test('arm and disarm never carry settings, whatever the builder is given', () {
    final Uint8List arm = encodePlaintext(const Mfs1Command(type: Mfs1CommandType.arm, minuteOfDay: 0));
    expect(arm.sublist(0, 4), <int>[0x00, 0x80, 0x00, 0x00]);
    final Uint8List disarm = encodePlaintext(const Mfs1Command(type: Mfs1CommandType.disarm, minuteOfDay: 0));
    expect(disarm.sublist(0, 4), <int>[0x00, 0xC0, 0x00, 0x00]);
  });

  test('a reserved type is refused', () {
    expect(() => encodePlaintext(const Mfs1Command(type: Mfs1CommandType.reserved, minuteOfDay: 0)), throwsArgumentError);
  });
```

Update "plaintext refuses values the device would reject" to construct `Mfs1Command(type: Mfs1CommandType.settings, ...)`. Add `import 'dart:typed_data';`.

In `test/services_test.dart`, the end-to-end test becomes: builder clock `DateTime.utc(2026, 9, 14, 9, 2, 30)`; `expect(await builder.armCommand(device), buildUuidFromBytes(kCommandVectors[0].onAir));` then a second `armCommand` is `isNot(...)`. Add: a builder on day 257 (`DateTime.utc(2026, 9, 15, 4, 0, 30)`) whose `MemorySequenceStore` has reserved slot 0 three times first (call `reserve(kVectorDeviceId, 257, 0)` ×3), then `networkManagerSettingsCommand(device, const Mfs1Settings(activations: 3, cooldownByte: 128, sensitivityByte: 200, delayCode: 2), Mfs1Mode.reportOnly)` equals `buildUuidFromBytes(kCommandVectors[2].onAir)`. Add a test that `Mfs1Settings.defaults` has activations 1, cooldown 0, sensitivity 143, delay 0.

- [ ] **Step 2: Run `flutter test`; expect failures.**

- [ ] **Step 3: Implement.** `lib/protocol/access_keys.dart`: `kProtocolVersion = 0x03`. `lib/protocol/mfs_protocol.dart`: add the enum with a doc comment mirroring `CommandType` in `class_mfs_1/src/mfs_protocol.hpp`; `kTypeShift = 6`; `Mfs1Command` per the Interfaces signature (drop `armActive`); `encodePlaintext` throws `ArgumentError('reserved command type')` for `reserved`, and for `arm`/`disarm` writes zero in bytes 0–3 regardless of the other fields (only type in byte 1 bits 6–7), for `settings` writes the fields as today plus type; byte 0 bit 0 always zero. `lib/services/command_builder.dart`: `Mfs1Settings` per Interfaces (no `armActive`), the four public methods, one private `_seal(DayGrant, Mfs1Device, Mfs1Command Function(int minute))` or equivalent; reserve-before-seal ordering and its comment unchanged. Engineer Arm/Disarm/Settings use `keys.engineerGrant`; the Network Manager method uses `keys.networkManagerGrant`.

- [ ] **Step 4: `flutter test` passes; `flutter analyze` clean for touched files.** The screen will not compile yet against the new builder — temporarily adapt `lib/devices/mfs1/mfs1_screen.dart` only as far as needed to compile (Task 6 rewrites it): map `_armActive ? armCommand : settingsCommand`. `flutter analyze` must report no errors.

- [ ] **Step 5: Commit in `class_app`:** `git add lib test && git commit -m "Protocol 0x03: Arm, Disarm and Settings command types"`. Verify `git status` does not list `lib/services/bench_credentials.dart` as staged.

---

### Task 5: Honest advertising countdown (Dart)

**Files (class_app):**
- Modify: `lib/services/advertiser.dart`
- Create: `test/advertiser_test.dart`

**Interfaces:**
- Produces: `Advertiser({AdvertisingPlatform? platform})`; `abstract class AdvertisingPlatform { Future<void> initialize(); void onStateChange(void Function(bool on)); void onAdvertisingStatus(void Function(bool on, String? error)); Future<void> start(String uuid); Future<void> stop(); }` with `BlePeripheralPlatform` as the default implementation; `Future<bool> Advertiser.send(String uuid)` — true only once advertising reported on; `String? Advertiser.lastError`.

- [ ] **Step 1: Failing tests** in `test/advertiser_test.dart` with a `FakePlatform` implementing `AdvertisingPlatform` that records calls and lets the test fire the status callback:
  - `send` returns true and `secondsLeft == 30` only after the fake reports `(true, null)`; `advertising` is true.
  - If the fake reports `(false, 'boom')` instead, `send` returns false, `secondsLeft == 0`, `advertising` false, `lastError == 'boom'`.
  - If `start` throws, `send` returns false and `lastError` holds the message.
  - If no status arrives within `kAdvertiseStartTimeout` (2 s; test with an injected shorter timeout parameter `Advertiser(platform:, startTimeout:)`), `send` returns false with `lastError` `'advertising did not start'`, and `stop` was called.
- [ ] **Step 2: `flutter test test/advertiser_test.dart` fails.**
- [ ] **Step 3: Implement.** `send`: `await stop()`; clear `lastError`; create a `Completer<bool>`; the status callback completes it on the first report after `start`; `await platform.start(uuid)` inside try/catch; await the completer with `startTimeout`; on success start the 30 s countdown exactly as today; on failure set `lastError`, call `stop()`, `notifyListeners()`, return false. Keep the `kAdvertiseWindow` doc comment. `debugPrint` of errors may stay in addition.
- [ ] **Step 4: All `flutter test` pass; `flutter analyze` no errors.** Adjust the screen call sites minimally (`await widget.advertiser.send(uuid)` result may be ignored until Task 6).
- [ ] **Step 5: Commit in `class_app`:** `git commit -m "Start the Send countdown only once advertising has actually started"`.

---

### Task 6: Arm page, disarm prompt, Settings page (Dart UI)

**Files (class_app):**
- Rewrite: `lib/devices/mfs1/mfs1_screen.dart` → the Arm page (`Mfs1ArmScreen`)
- Create: `lib/devices/mfs1/mfs1_settings_screen.dart` (`Mfs1SettingsScreen`)
- Create: `lib/devices/mfs1/disarm_prompt.dart` (`Future<bool> showDisarmPrompt(BuildContext context)`)
- Modify: `lib/main.dart` (push `Mfs1ArmScreen`)
- Create: `test/mfs1_flow_test.dart` (widget tests)

**Interfaces:**
- Consumes: `CommandBuilder.armCommand/disarmCommand/settingsCommand/networkManagerSettingsCommand`, `Mfs1Settings.defaults`, `Advertiser.send → Future<bool>`, `Advertiser.lastError`, `AdvertisingPlatform` (for a fake in widget tests).

Behaviour (amendment §3, binding):

- **Arm page** (`Mfs1ArmScreen`, title `MFS_1 — <label>`): a `SwitchListTile` titled `Armed`, **initial value true**, subtitle `Send arms the device with its stored settings` / `Send disarms the device`; the Send button (label `Advertising… N s` while advertising, disabled unless `ready && !advertising`); `Not sent: <lastError>` when `send` returned false or threw; a short LED legend: `Watch LED A. Rapid flash: armed (also shown if it was already armed). Slow flash: disarmed. Double blink: a pending trigger was cancelled. Three long pulses: arming refused — check the device. No flash: the command did not land — move closer and send again.` **No settings controls and no Network Manager section on this page.**
- Send with Armed → `armCommand`; Send with Disarmed → `disarmCommand`; then `advertiser.send(uuid)`. **Only if the Send was a disarm and `send` returned true**, call `showDisarmPrompt(context)`. If it returns true, `Navigator.push` the Settings page; when that route pops, `setState` the switch back to **true** (Armed).
- The Arm page's switch resets to Armed on every return from the Settings page. Leaving the Arm page entirely discards everything (new State on re-entry).
- **Disarm prompt** (`showDisarmPrompt`): `showDialog<bool>` with `barrierDismissible: false`; title and body text and button labels exactly as in Global Constraints; "Not seen" pops false, "Disarmed — open settings" pops true. Returns `result ?? false`.
- **Settings page** (`Mfs1SettingsScreen`, title `MFS_1 settings — <label>`): local state initialised from `Mfs1Settings.defaults` on every entry; the controls moved verbatim from today's screen (activations chips 1–16, cooldown slider shown only when activations > 1, delay slider, sensitivity slider with the mg readout and the `less sensitive → more sensitive` hint); **Send** → `settingsCommand(device, settings)` → `advertiser.send`; **Restore defaults** (`OutlinedButton`) → `setState` back to `Mfs1Settings.defaults`, sends nothing; hint text `One blink: settings applied. Two blinks: mode changed. Rapid flash: the device is still ARMED — go back and disarm. No flash: send again.`; then the Network Manager (bench) section moved verbatim from today's screen (mode segmented button with Report segments disabled, the not-implemented note, the transmit warning), its button `Send as Network Manager (sets mode)` → `networkManagerSettingsCommand(device, settings, mode)`. Remove today's "Turn Armed off first" note (the page is only reachable disarmed). A back arrow returns to the Arm page.

- [ ] **Step 1: Failing widget tests** in `test/mfs1_flow_test.dart`. Build `ClassApp`-free harnesses: `MaterialApp(home: Mfs1ArmScreen(device:, builder:, advertiser:))` with a `CommandBuilder` on `BenchNetworkManager(device: Mfs1Device(label: 'Bench', deviceId: kVectorDeviceId), secret: kVectorSecret)` + `MemorySequenceStore`, and an `Advertiser(platform: FakePlatform(...))` whose fake reports ready and reports advertising on (or fails, per test) immediately on `start`. Record the UUIDs started. Tests:
  1. The Arm page starts with the Armed switch on and has no `Slider` and no `ChoiceChip`.
  2. Send with Armed starts one advert and shows no dialog.
  3. Toggle to Disarmed, Send → the dialog with title `Confirm LED A shows the device is disarmed` and both fault-finding sentences is shown.
  4. "Not seen" → dialog closes, still on the Arm page (no `Slider` found).
  5. "Disarmed — open settings" → the Settings page (`Slider` found); pop → back on the Arm page with the switch **on**.
  6. On the Settings page, change sensitivity, tap Restore defaults → the sensitivity readout shows the default (`75.5 mg` for byte 143, i.e. `302 * 0.25`), and no advert was started by the button.
  7. Advertising fails (fake reports `(false, 'boom')`) on a Disarmed Send → no dialog, `Not sent: boom` shown.
  Inject a fixed clock into `CommandBuilder` for determinism.
- [ ] **Step 2: `flutter test` fails.**
- [ ] **Step 3: Implement** the three files and `main.dart` wiring per the behaviour above. Keep the existing comment style (`///` docs, explanatory `//` comments where the old screen had them, especially the Network Manager separation and transmit warning comments).
- [ ] **Step 4: `flutter test` all pass; `flutter analyze` no errors or warnings in touched files.** `flutter build macos --debug` succeeds (the bench runs the macOS build). Do not launch the app.
- [ ] **Step 5: Commit in `class_app`:** `git commit -m "Arm page, disarm confirmation and a gated Settings page with Restore defaults"`. Confirm `bench_credentials.dart` is not staged.

---

## Self-review notes

- Spec coverage: §2.1 wire format → Task 1 (firmware) and Task 4 (app); §2.2 decision table → Task 1 (policy) and Task 2 (App); §2.3 LED → Task 2 and Task 3; §3.1–3.3 app flow → Task 6; §3.4 honest countdown → Task 5; §4 decision (do not store) → Task 3 CLAUDE.md, Task 6 (settings page resets on entry, no persistence); §5 impact rows all mapped. Bench verification → Task 3 checklist §5a.
- Task 1 leaves `applyCommand` compiling via its `default:` case; Task 2 replaces that behaviour. Task 4 leaves the old screen compiling against the new builder; Task 6 rewrites it.
