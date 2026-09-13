# MFS_1 App Control Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

> **Re-cut 2026-09-13** for the day-key access scheme. No task of the 2026-09-12 plan had started. The mapping from old to new tasks is at the end. The host-side C++ and Dart in Tasks 1–7 and 10–12 was prototyped and run before this plan was written: 9 host C++ suites pass, 14 Dart tests pass, and both reproduce the Python vectors byte for byte. Tasks 8, 9 and 14–16 were built for `nrf54l15dk/nrf54l05/cpuapp` with no warnings. **None of it has run on hardware.**

**Goal:** Replace the Thingy:53 toggle tool with a Flutter app that tunes, arms and disarms MFS_1 under encrypted, day-keyed commands, with LED A acknowledging every accepted command.

**Architecture:** The phone advertises one 128-bit service UUID: a rotating ID, an AES-128-CCM ciphertext and a 4-byte tag. MFS_1 stays scan-only. It keeps UTC from the GRTC, which is invalid on every boot until an authenticated provisioner sync. It matches the rotating ID against 128 expected IDs, trial-decrypts, checks freshness, persists the sequence number and only then acts. All the security rules live in pure, host-tested units (`AccessControl`, `DeviceClock`, `access_keys`); `App` only wires them to the radio, the ADXL367 and the LEDs.

**Tech Stack:** C++20 / Zephyr / NCS v3.2.4 on nRF54L05, PSA Crypto on CRACEN; host tests via plain `g++`, `assert()` and OpenSSL 3; Flutter 3.41 / Dart 3.11 with `ble_peripheral` 2.4.0, `pointycastle` 4.0.0, `crypto`, `shared_preferences`; Python `cryptography` for known-answer vectors.

**Spec:** `docs/superpowers/specs/2026-09-12-app-control-design.md`, **and `docs/tan-scheme.md`**, which is authoritative for keys, the wire format's cryptography, acceptance and time.

## Global Constraints

- **Two repos.** Firmware in `/Users/andy/nordic/ncs/v3.2.4/class_mfs_1`; app in `/Users/andy/nordic/ncs/v3.2.4/class_app`. Each is its own git repo. Commit to whichever repo a task touches.
- **House style is `~/.claude/CLAUDE.md`**, and `.clang-format` is authoritative for layout. Run `/Users/andy/nrfenv/bin/clang-format -i <file>` on every C++ file touched.
- **Always flash with `--recover`.** `west flash` without it has reported success on this board while the old image kept running.
- **Board target:** `nrf54l15dk/nrf54l05/cpuapp`. **J-Link for MFS_1 is `--dev-id 853003346`** — check with `nrfutil device device-info --serial-number 853003346` that it reports an nRF54L before flashing, because that probe moves between boards. RTT device name `nRF54L05_M33`.
- **Every build uses the bench credentials:** `west build -b nrf54l15dk/nrf54l05/cpuapp -p always -- -DEXTRA_CONF_FILE=credentials.conf`. Set Kconfig in `prj.conf` or `credentials.conf`, never `-DCONFIG_X` (it does not reliably reach the app under sysbuild).
- **Payload: exactly one 128-bit service UUID, 16 bytes.** Command = rotating ID (4) + ciphertext (8) + tag (4). Time sync = unix LE (4) + tag (12).
- **Plaintext bytes 6–7 and the reserved bits are per-variant space. MFS_1 must IGNORE them and never validate them.**
- **Never reuse a sequence number.** The app reserves `n` and persists `n + 1` before advertising. Two phones never share a slot.
- **Persist before acting.** The device saves `next[slot]` before a command has any effect.
- **UTC only**, day boundary 04:00 UTC. Never local time on either side.
- **The clock is invalid on every boot.** No resume from NVS; the stored day is a floor only.
- **Only slot 0 changes the operating mode.**
- **Secrets are never committed.** `class_mfs_1/credentials.conf` and `class_app/lib/services/bench_credentials.dart` are gitignored. Check `git status` before every commit in both repos.
- **The arm invariant** (`docs/v1-scope.md` §1.0): `App::updateOutputState()` is the only place the arm state and detection are combined; `App::IsOutputActive()` is the only sanctioned read.
- **`OutputSwitch` already exists** and owns the fire pins privately. Do not add an accessor or a second handle to P2.05/P2.09.
- **Failures emit nothing** — no advert, no LED. LED A plays a pattern only for an *accepted* command.
- **Armed, the device does one thing on command: disarm.** Everything else is ignored, and a disarm applies nothing else. **Triggers are one-shot** — the device latches Inactive after firing. `DecideCommand()` is the only place this is decided.

---

## File Structure

**Firmware — `class_mfs_1`:**

| File | Responsibility | Host-tested |
|---|---|---|
| `tools/gen_protocol_tables.py` (new) | encoding tables for C++ **and** Dart | via tests |
| `tools/gen_access_vectors.py` (new) | known-answer vectors, Python `cryptography`, for C++ **and** Dart | — |
| `tools/gen_bench_credentials.py` (new) | writes the app's bench credentials from `credentials.conf` | — |
| `src/mfs_protocol_tables.hpp` (new, generated) | the three encoding tables | yes |
| `src/mfs_protocol.{hpp,cpp}` (new) | on-air and plaintext layout, `Command`, encode/decode | yes |
| `src/crypto.hpp` (new) | the crypto seam: HMAC-SHA256, AES-128-CCM | — |
| `tests/crypto_openssl.cpp` (new) | host backend | — |
| `src/crypto_psa.cpp` (new) | target backend | on-target self-test |
| `src/access_vectors.hpp` (new, generated) | the vectors | — |
| `src/access_keys.{hpp,cpp}` (new) | day key, enc key, rotating ID, nonce, seal/open, time sync | yes |
| `src/device_clock.{hpp,cpp}` (new) | UTC, validity, floor, sync and trim rules | yes |
| `src/access_control.{hpp,cpp}` (new) | slots, window, lockout, freshness, persist-before-act | yes |
| `src/led_sequencer.{hpp,cpp}` (new) | LED A patterns | yes |
| `src/settings.{hpp,cpp}` (new) | parameters, NVS, mode gated on slot 0 | yes |
| `src/credentials.{hpp,cpp}` (new) | strict hex parsing of the bench credentials | yes |
| `src/arm_policy.hpp` (new) | **the single armed path**: what an accepted command may do | yes |
| `src/crypto_selftest.{hpp,cpp}` (new) | proves PSA against the vectors at boot | — |
| `src/access_store.{hpp,cpp}` (new) | NVS record for `AccessState` | — |
| `src/command_scanner.{hpp,cpp}` (modify) | AD 0x07 → queue of raw UUIDs; fast scan | — |
| `src/output_switch.{hpp,cpp}` (modify) | adds the interlock | — |
| `src/app.{hpp,cpp}` (modify) | wiring, detection engine, delay, arm transitions, LEDs | — |
| `Makefile`, `tests/` (new) | host test runner | — |

**App — `class_app`:**

| File | Responsibility | Tested |
|---|---|---|
| `lib/protocol/tables.dart` (generated) | encoding tables | yes |
| `lib/protocol/mfs_protocol.dart` | plaintext encoding, UUID string | yes |
| `lib/protocol/access_keys.dart` | derivations, CCM seal, time sync | yes, vectors |
| `lib/protocol/day_clock.dart` | UTC day index and minute | yes |
| `lib/services/sequence_store.dart` | `SequenceStore` interface, in-memory test store | yes |
| `lib/services/prefs_sequence_store.dart` | persistent store | yes |
| `lib/services/key_source.dart` | `KeySource`, `BenchNetworkManager` | yes |
| `lib/services/command_builder.dart` | settings → sealed UUID | yes |
| `lib/services/advertiser.dart` | 30 s advertising window | — |
| `lib/services/bench_credentials.dart` (generated, **gitignored**) | bench keys | — |
| `lib/devices/mfs1/mfs1_screen.dart` | control screen | — |
| `lib/devices/provisioner/provisioner_screen.dart` | time sync | — |
| `lib/main.dart` | wiring, device picker | — |
| `test/access_vectors.dart` (generated) | vectors | — |

---

# PHASE 1 — Protocol and access logic, host-tested

## Task 1: Generated encoding tables and the host-test harness

The three encodings must produce **byte-identical results on both platforms**. A formula evaluated independently in C++ (`powf`) and Dart (`pow`) can differ by an LSB at some inputs, and the two sides would then disagree about what a slider means. One generator, two outputs.

**Files:**
- Create: `tools/gen_protocol_tables.py`
- Create: `src/mfs_protocol_tables.hpp` (generated — commit the output)
- Create: `src/mfs_protocol.hpp` (minimal; Task 2 completes it)
- Create: `Makefile`, `tests/test_main.cpp`, `tests/test_protocol.cpp`

**Interfaces:**
- Consumes: nothing.
- Produces: `alc::protocol::M_THRESHOLD_TABLE[256]`, `M_COOLDOWN_TABLE[256]`, `M_DELAY_TABLE[128]` (`uint16_t`); `SensitivityToThresholdLsb(uint8_t)`, `CooldownToSeconds(uint8_t)`, `DelayToSeconds(uint8_t)`, `M_DELAY_MASK`.

- [ ] **Step 1: Write the generator**

Create `tools/gen_protocol_tables.py`:

```python
#!/usr/bin/env python3
"""Generates the MFS_1 parameter encoding tables for BOTH firmware and app.

The two sides must agree byte for byte. Evaluating the formula independently in
C++ and Dart risks a one-LSB divergence at some inputs, which would mean a
slider position meaning two different things. Generating once removes the
possibility rather than testing for it.

Run from the class_mfs_1 root:
    python3 tools/gen_protocol_tables.py
"""
import pathlib

# Sensitivity: THRESH_ACT in 13-bit LSB at 0.25 mg/LSB. Byte 255 is MOST
# sensitive, i.e. the LOWEST threshold - the app's slider runs insensitive-left.
THRESHOLD_MAX_LSB = 4000   # 1000 mg, least sensitive, byte 0
THRESHOLD_MIN_LSB = 40     # 10 mg, most sensitive, byte 255

# Cooldown in seconds. Byte 0 means no cooldown; 1..255 spans 1 s to 3600 s.
COOLDOWN_MAX_SECS = 3600

def threshold(byte: int) -> int:
    return round(THRESHOLD_MAX_LSB * (THRESHOLD_MIN_LSB / THRESHOLD_MAX_LSB) ** (byte / 255))

def cooldown(byte: int) -> int:
    return 0 if byte == 0 else round(COOLDOWN_MAX_SECS ** ((byte - 1) / 254))

def delay(code: int) -> int:
    """Trigger delay in seconds. Seven bits, piecewise, fully contiguous:
         0..59   -> V seconds        (0 s .. 59 s)
        60..118  -> V-59 minutes     (1 min .. 59 min)
       119..127  -> V-118 hours      (1 h .. 9 h)
    Contiguous at both seams: 59->59 s and 60->60 s; 118->3540 s and 119->3600 s.
    No code means two things, and there are no gaps."""
    if code < 60:
        return code
    if code < 119:
        return (code - 59) * 60
    return (code - 118) * 3600

BANNER = "GENERATED by tools/gen_protocol_tables.py - DO NOT EDIT BY HAND."

def rows(fn, count=256, per_line=8):
    values = [fn(b) for b in range(count)]
    return [values[i:i + per_line] for i in range(0, count, per_line)]

def write_cpp(path: pathlib.Path):
    out = [f"// {BANNER}", "// Regenerate after changing the formula, and commit both outputs.",
           "", "#pragma once", "", "#include <cstdint>", "", "namespace alc::protocol", "{", ""]
    for name, fn, count, comment in (
        ("M_THRESHOLD_TABLE", threshold, 256, "Sensitivity byte -> ADXL367 THRESH_ACT in LSB (0.25 mg each)."),
        ("M_COOLDOWN_TABLE", cooldown, 256, "Cooldown byte -> seconds. Index 0 is 'no cooldown'."),
        ("M_DELAY_TABLE", delay, 128, "Delay code (7 bits) -> seconds before the trigger fires."),
    ):
        out.append(f"  // {comment}")
        out.append(f"  constexpr uint16_t {name}[{count}] {{")
        for row in rows(fn, count=count):
            out.append("    " + ", ".join(f"{v:5d}" for v in row) + ",")
        out.append("  };")
        out.append("")
    out += ["}", ""]
    path.write_text("\n".join(out))
    print(f"wrote {path}")

def write_dart(path: pathlib.Path):
    if not path.parent.exists():
        print(f"skipped {path} (app not scaffolded yet)")
        return
    out = [f"// {BANNER}", "// Regenerate after changing the formula, and commit both outputs.", ""]
    for name, fn, count, comment in (
        ("kThresholdTable", threshold, 256, "Sensitivity byte -> ADXL367 THRESH_ACT in LSB (0.25 mg each)."),
        ("kCooldownTable", cooldown, 256, "Cooldown byte -> seconds. Index 0 is 'no cooldown'."),
        ("kDelayTable", delay, 128, "Delay code (7 bits) -> seconds before the trigger fires."),
    ):
        out.append(f"/// {comment}")
        out.append(f"const List<int> {name} = <int>[")
        for row in rows(fn, count=count):
            out.append("  " + ", ".join(str(v) for v in row) + ",")
        out.append("];")
        out.append("")
    path.write_text("\n".join(out))
    print(f"wrote {path}")

if __name__ == "__main__":
    here = pathlib.Path(__file__).resolve().parent.parent
    write_cpp(here / "src" / "mfs_protocol_tables.hpp")
    write_dart(here.parent / "class_app" / "lib" / "protocol" / "tables.dart")
```

- [ ] **Step 2: Run it and check the anchors**

```bash
cd /Users/andy/nordic/ncs/v3.2.4/class_mfs_1
python3 tools/gen_protocol_tables.py
python3 -c "
import importlib.util
spec = importlib.util.spec_from_file_location('g', 'tools/gen_protocol_tables.py')
g = importlib.util.module_from_spec(spec); spec.loader.exec_module(g)
print([g.threshold(b) for b in (0, 143, 255)], [g.delay(c) for c in (0, 59, 60, 118, 119, 127)])
"
```

Expected: `wrote .../src/mfs_protocol_tables.hpp`, a `skipped ... (app not scaffolded yet)` line, then `[4000, 302, 40] [0, 59, 60, 3540, 3600, 32400]`. If byte 143 is not 302 the formula has drifted from the spec; the delay pairs 59/60 and 118/119 must be contiguous.

- [ ] **Step 3: Write the failing test**

Create `tests/test_protocol.cpp`:

```cpp
#include <cassert>
#include <cstdio>

#include "mfs_protocol.hpp"

void run_protocol_tests()
{
  using namespace alc::protocol;

  // Table anchors, straight from the design spec section 5.
  assert(M_THRESHOLD_TABLE[0] == 4000);  // 1000 mg, least sensitive
  assert(M_THRESHOLD_TABLE[143] == 302); // ~75 mg, the present default
  assert(M_THRESHOLD_TABLE[255] == 40);  // 10 mg, most sensitive

  assert(M_COOLDOWN_TABLE[0] == 0); // reserved: no cooldown
  assert(M_COOLDOWN_TABLE[1] == 1);
  assert(M_COOLDOWN_TABLE[128] == 60); // the clean midpoint
  assert(M_COOLDOWN_TABLE[255] == 3600);

  // Sensitivity must be MONOTONICALLY DECREASING in threshold: a higher byte
  // means more sensitive, which means a lower number. Getting this backwards
  // would make the app's slider work in reverse, which is easy to miss on the
  // bench because the device still triggers - just at the wrong setting.
  for (int i = 1; i < 256; i++) {
    assert(M_THRESHOLD_TABLE[i] <= M_THRESHOLD_TABLE[i - 1]);
  }

  // Cooldown must be monotonically increasing from index 1 upward.
  for (int i = 2; i < 256; i++) {
    assert(M_COOLDOWN_TABLE[i] >= M_COOLDOWN_TABLE[i - 1]);
  }

  // Delay: the two seams are where a piecewise encoding goes wrong, so they are
  // asserted explicitly rather than left to the monotonicity check.
  assert(M_DELAY_TABLE[0] == 0);       // default - no delay
  assert(M_DELAY_TABLE[59] == 59);     // 59 s
  assert(M_DELAY_TABLE[60] == 60);     // 1 min - contiguous with 59 s
  assert(M_DELAY_TABLE[118] == 3540);  // 59 min
  assert(M_DELAY_TABLE[119] == 3600);  // 1 h - contiguous with 59 min
  assert(M_DELAY_TABLE[127] == 32400); // 9 h, the maximum

  // Strictly increasing: no delay code may mean the same as another.
  for (int i = 1; i < 128; i++) {
    assert(M_DELAY_TABLE[i] > M_DELAY_TABLE[i - 1]);
  }

  // The ADXL367 threshold register is 13-bit. A table entry that overflowed it
  // would be silently truncated by writeThreshold() into a DIFFERENT threshold.
  for (int i = 0; i < 256; i++) {
    assert(M_THRESHOLD_TABLE[i] <= 0x1FFF);
    assert(M_THRESHOLD_TABLE[i] >= 1);
  }

  printf("protocol tables: OK\n");
}
```

Create `tests/test_main.cpp`:

```cpp
#include <cstdio>

void run_protocol_tests();

int main()
{
  run_protocol_tests();
  printf("ALL TESTS PASSED\n");
  return 0;
}
```

Create `Makefile` (flags from `../class_templates/npm2100/Makefile`, the workspace reference). `HOST_SRCS` grows task by task:

```make
CXX      ?= g++
CXXFLAGS ?= -std=c++20 -Wall -Wextra -Wpedantic -Werror -O0 -g -Isrc -Itests

HOST_SRCS =
TEST_SRCS = $(wildcard tests/test_*.cpp)
SRCS      = $(HOST_SRCS) $(TEST_SRCS)

test: $(SRCS)
	$(CXX) $(CXXFLAGS) $(SRCS) -o test_runner $(LDFLAGS)
	./test_runner

clean:
	rm -rf test_runner test_runner.dSYM

.PHONY: test clean
```

Add `test_runner` and `test_runner.dSYM/` to `.gitignore`.

- [ ] **Step 4: Run the test to verify it fails**

Run: `make test`
Expected: FAIL — `fatal error: 'mfs_protocol.hpp' file not found`.

- [ ] **Step 5: Create the minimal header**

Create `src/mfs_protocol.hpp`:

```cpp
#pragma once

#include <cstdint>

#include "mfs_protocol_tables.hpp"

namespace alc::protocol
{

  constexpr uint8_t M_DELAY_MASK { 0x7F };

  /** @brief Sensitivity byte to ADXL367 THRESH_ACT, in 0.25 mg LSB. */
  inline uint16_t SensitivityToThresholdLsb(uint8_t sensitivityByte)
  {
    return M_THRESHOLD_TABLE[sensitivityByte];
  }

  /** @brief Cooldown byte to seconds. Zero means no cooldown. */
  inline uint16_t CooldownToSeconds(uint8_t cooldownByte)
  {
    return M_COOLDOWN_TABLE[cooldownByte];
  }

  /** @brief Delay code (7 bits) to seconds. Zero means fire immediately. */
  inline uint16_t DelayToSeconds(uint8_t delayCode)
  {
    return M_DELAY_TABLE[delayCode & M_DELAY_MASK];
  }

}
```

- [ ] **Step 6: Run the test to verify it passes**

Run: `make test`
Expected: `protocol tables: OK` then `ALL TESTS PASSED`.

- [ ] **Step 7: Commit**

```bash
/Users/andy/nrfenv/bin/clang-format -i src/mfs_protocol.hpp tests/test_protocol.cpp tests/test_main.cpp
git add tools/gen_protocol_tables.py src/mfs_protocol_tables.hpp src/mfs_protocol.hpp Makefile tests/ .gitignore
git commit -m "Add generated protocol encoding tables and a host-test harness

The encodings are generated once and emitted for both C++ and Dart. Evaluating
the formula independently on each side risks a one-LSB divergence, which would
mean a slider position meaning two different things.

Tests pin the spec's anchors, that sensitivity is monotonically DECREASING in
threshold so the slider cannot run backwards, that the delay encoding is
contiguous at both seams, and that no entry overflows the ADXL367's 13-bit
threshold register."
```

---

## Task 2: Command plaintext codec

**Files:**
- Modify: `src/mfs_protocol.hpp` (replace with the full layout)
- Create: `src/mfs_protocol.cpp`
- Modify: `tests/test_protocol.cpp`, `tests/test_main.cpp`, `Makefile`

**Interfaces:**
- Consumes: Task 1's tables and conversions.
- Produces: `protocol::Command { bool armActive; uint8_t delayCode; uint8_t activations; Mode mode; uint8_t cooldownByte; uint8_t sensitivityByte; uint16_t minuteOfDay; }`, `enum class protocol::Mode { TriggerOnly, ReportAndTrigger, ReportOnly, Reserved }`, `bool DecodeCommand(const uint8_t*, Command&)`, `void EncodeCommand(const Command&, uint8_t*)`, and the constants `M_UUID_BYTES`, `M_ROTATING_ID_BYTES`, `M_OFFSET_ROTATING_ID`, `M_OFFSET_CIPHERTEXT`, `M_PLAINTEXT_BYTES`, `M_OFFSET_TAG`, `M_TAG_BYTES`, `M_PROTOCOL_VERSION`, `M_ACTIVATIONS_MIN/MAX`.

- [ ] **Step 1: Write the failing tests**

Append to `tests/test_protocol.cpp`:

```cpp
void run_command_codec_tests()
{
  using namespace alc::protocol;
  uint8_t plaintext[M_PLAINTEXT_BYTES] {};
  Command command;
  Command decoded;

  // The on-air layout must fill exactly one 128-bit UUID.
  assert(M_ROTATING_ID_BYTES + M_PLAINTEXT_BYTES + M_TAG_BYTES == M_UUID_BYTES);
  assert(M_OFFSET_CIPHERTEXT == M_ROTATING_ID_BYTES);
  assert(M_OFFSET_TAG == M_OFFSET_CIPHERTEXT + M_PLAINTEXT_BYTES);

  // Round trip, every field away from its default.
  command.armActive       = true;
  command.delayCode       = 119; // 1 h
  command.activations     = 16;  // the 4-bit maximum, stored as 15
  command.mode            = Mode::ReportAndTrigger;
  command.cooldownByte    = 128;
  command.sensitivityByte = 143;
  command.minuteOfDay     = 1439; // 23:59, the largest legal minute
  EncodeCommand(command, plaintext);
  assert(DecodeCommand(plaintext, decoded));
  assert(decoded.armActive);
  assert(decoded.delayCode == 119);
  assert(decoded.activations == 16);
  assert(decoded.mode == Mode::ReportAndTrigger);
  assert(decoded.cooldownByte == 128);
  assert(decoded.sensitivityByte == 143);
  assert(decoded.minuteOfDay == 1439);

  // Byte 0 is shared: arm bit and delay code must not bleed into each other.
  assert(plaintext[M_PT_ARM_DELAY] == ((119 << 1) | 0x01));

  // Byte 1 is shared: activations - 1 in the low nibble, mode in bits 4-5.
  assert(plaintext[M_PT_ACTIVATIONS_MODE] == (0x0F | (1 << 4)));

  // Activations 1 encodes as zero, so an all-zero plaintext is a legal command:
  // Inactive, no delay, one activation, Trigger only, minute 0.
  for (uint8_t& byte : plaintext) {
    byte = 0;
  }
  assert(DecodeCommand(plaintext, decoded));
  assert(decoded.activations == 1);
  assert(!decoded.armActive);

  // Mode 3 is reserved and refused rather than treated as one of the others.
  EncodeCommand(command, plaintext);
  plaintext[M_PT_ACTIVATIONS_MODE] = static_cast<uint8_t>(plaintext[M_PT_ACTIVATIONS_MODE] | (3 << M_MODE_SHIFT));
  assert(!DecodeCommand(plaintext, decoded));

  // Minute 1440 fits in eleven bits but is not a minute of any day.
  command.mode        = Mode::TriggerOnly;
  command.minuteOfDay = 1439;
  EncodeCommand(command, plaintext);
  plaintext[M_PT_MINUTE]     = static_cast<uint8_t>(1440 & 0xFF);
  plaintext[M_PT_MINUTE + 1] = static_cast<uint8_t>(1440 >> 8);
  assert(!DecodeCommand(plaintext, decoded));

  // PLAINTEXT BYTES 6-7 AND THE RESERVED BITS BELONG TO OTHER VARIANTS. Anything
  // there must still decode - validating them would make MFS_1 reject a future
  // app build the moment another variant starts using that space.
  EncodeCommand(command, plaintext);
  plaintext[6]                     = 0xAA;
  plaintext[7]                     = 0xBB;
  plaintext[M_PT_ACTIVATIONS_MODE] = static_cast<uint8_t>(plaintext[M_PT_ACTIVATIONS_MODE] | 0xC0);
  plaintext[M_PT_MINUTE + 1]       = static_cast<uint8_t>(plaintext[M_PT_MINUTE + 1] | 0xF8);
  assert(DecodeCommand(plaintext, decoded));
  assert(decoded.minuteOfDay == 1439);
  assert(decoded.mode == Mode::TriggerOnly);

  printf("command codec: OK\n");
}
```

In `tests/test_main.cpp`, declare `void run_command_codec_tests();` and call it after `run_protocol_tests();`.

- [ ] **Step 2: Run to verify it fails**

Run: `make test`
Expected: FAIL — `no member named 'Command' in namespace 'alc::protocol'` (or similar).

- [ ] **Step 3: Implement**

Replace `src/mfs_protocol.hpp` with:

```cpp
#pragma once

#include <cstdint>

#include "mfs_protocol_tables.hpp"

namespace alc::protocol
{

  // Wire format - see docs/superpowers/specs/2026-09-12-app-control-design.md
  // section 4. Byte positions are ON-AIR order, which is the 128-bit service
  // UUID transmitted least-significant byte first. The app builds its UUID
  // string as this sequence REVERSED.
  constexpr uint8_t M_UUID_BYTES { 16 };

  // On air: rotating ID | ciphertext | CCM tag. Only the rotating ID is in the
  // clear, and it is unpredictable without the day key.
  constexpr uint8_t M_OFFSET_ROTATING_ID { 0 };
  constexpr uint8_t M_ROTATING_ID_BYTES { 4 };
  constexpr uint8_t M_OFFSET_CIPHERTEXT { 4 };
  constexpr uint8_t M_PLAINTEXT_BYTES { 8 };
  constexpr uint8_t M_OFFSET_TAG { 12 };
  constexpr uint8_t M_TAG_BYTES { 4 };

  // Never transmitted. Both sides supply it as CCM associated data, so a payload
  // built for another protocol version fails authentication rather than parsing.
  constexpr uint8_t M_PROTOCOL_VERSION { 0x02 };

  // Plaintext layout, after decryption.
  constexpr uint8_t M_PT_ARM_DELAY { 0 };        // bit 0 arm, bits 1-7 delay code
  constexpr uint8_t M_PT_ACTIVATIONS_MODE { 1 }; // bits 0-3 activations - 1, bits 4-5 mode
  constexpr uint8_t M_PT_COOLDOWN { 2 };
  constexpr uint8_t M_PT_SENSITIVITY { 3 };
  constexpr uint8_t M_PT_MINUTE { 4 }; // uint16 LE, bits 0-10 UTC minute of day
  // Plaintext bytes 6-7 are per-variant extension space. MFS_1 MUST IGNORE THEM
  // and must never require them to be zero.

  constexpr uint8_t M_ARM_BIT { 0x01 };
  constexpr uint8_t M_DELAY_SHIFT { 1 };
  constexpr uint8_t M_DELAY_MASK { 0x7F };
  constexpr uint8_t M_ACTIVATIONS_MASK { 0x0F };
  constexpr uint8_t M_MODE_SHIFT { 4 };
  constexpr uint8_t M_MODE_MASK { 0x03 };
  constexpr uint16_t M_MINUTE_MASK { 0x07FF };
  constexpr uint16_t M_MINUTES_PER_DAY { 1440 };

  constexpr uint8_t M_ACTIVATIONS_MIN { 1 };
  constexpr uint8_t M_ACTIVATIONS_MAX { 16 };

  /**
   * @brief What the device does when the activation count is reached.
   *
   * Report and ReportAndTrigger make the device ADVERTISE, which is an exception
   * to the standing rule in CLAUDE.md that it never does. They are for use only
   * when absolutely necessary - advertising forfeits covertness. Only a slot-0
   * (Network Manager) command may change the mode.
   */
  enum class Mode : uint8_t {
    TriggerOnly      = 0, ///< Default. Fires the output, emits nothing.
    ReportAndTrigger = 1, ///< Broadcasts immediately before firing.
    ReportOnly       = 2, ///< Broadcasts, never fires.
    Reserved         = 3, ///< Rejected.
  };

  /** @brief A decrypted, decoded command. Byte encodings not yet resolved. */
  struct Command
  {
      bool armActive { false };
      uint8_t delayCode { 0 };
      uint8_t activations { 1 };
      Mode mode { Mode::TriggerOnly };
      uint8_t cooldownByte { 0 };
      uint8_t sensitivityByte { 0 };
      uint16_t minuteOfDay { 0 };
  };

  /**
   * @brief Decode an 8-byte plaintext.
   *
   * @param plaintext Exactly M_PLAINTEXT_BYTES bytes.
   * @param out       Populated only on success.
   * @return False for a reserved mode or a minute outside 0-1439.
   */
  bool DecodeCommand(const uint8_t* plaintext, Command& out);

  /** @brief Encode a command into M_PLAINTEXT_BYTES bytes. Extension bytes are zero. */
  void EncodeCommand(const Command& command, uint8_t* plaintext);

  /** @brief Sensitivity byte to ADXL367 THRESH_ACT, in 0.25 mg LSB. */
  inline uint16_t SensitivityToThresholdLsb(uint8_t sensitivityByte)
  {
    return M_THRESHOLD_TABLE[sensitivityByte];
  }

  /** @brief Cooldown byte to seconds. Zero means no cooldown. */
  inline uint16_t CooldownToSeconds(uint8_t cooldownByte)
  {
    return M_COOLDOWN_TABLE[cooldownByte];
  }

  /** @brief Delay code (7 bits) to seconds. Zero means fire immediately. */
  inline uint16_t DelayToSeconds(uint8_t delayCode)
  {
    return M_DELAY_TABLE[delayCode & M_DELAY_MASK];
  }

}
```

Create `src/mfs_protocol.cpp`:

```cpp
#include "mfs_protocol.hpp"

namespace alc::protocol
{

  bool DecodeCommand(const uint8_t* plaintext, Command& out)
  {
    Command decoded;
    uint16_t minuteField { 0 };

    if (plaintext == nullptr) { return false; }

    minuteField = static_cast<uint16_t>(plaintext[M_PT_MINUTE] | (plaintext[M_PT_MINUTE + 1] << 8));

    decoded.armActive       = (plaintext[M_PT_ARM_DELAY] & M_ARM_BIT) != 0;
    decoded.delayCode       = (plaintext[M_PT_ARM_DELAY] >> M_DELAY_SHIFT) & M_DELAY_MASK;
    decoded.activations     = static_cast<uint8_t>((plaintext[M_PT_ACTIVATIONS_MODE] & M_ACTIVATIONS_MASK) + 1);
    decoded.mode            = static_cast<Mode>((plaintext[M_PT_ACTIVATIONS_MODE] >> M_MODE_SHIFT) & M_MODE_MASK);
    decoded.cooldownByte    = plaintext[M_PT_COOLDOWN];
    decoded.sensitivityByte = plaintext[M_PT_SENSITIVITY];
    decoded.minuteOfDay     = minuteField & M_MINUTE_MASK;

    // Reserved mode is refused rather than quietly treated as one of the others.
    // Silently downgrading an unknown mode to TriggerOnly would be worse: the
    // operator would believe a report had been configured.
    if (decoded.mode == Mode::Reserved) { return false; }

    // Eleven bits hold up to 2047, but a day has 1440 minutes. An out-of-range
    // minute is a bug in the sender, and the freshness check cannot judge it.
    if (decoded.minuteOfDay >= M_MINUTES_PER_DAY) { return false; }

    // Plaintext bytes 6-7 and the reserved bits are deliberately not examined.
    // They belong to other MFS variants and MFS_1 must tolerate whatever they hold.

    out = decoded;
    return true;
  }

  void EncodeCommand(const Command& command, uint8_t* plaintext)
  {
    uint16_t minuteField { static_cast<uint16_t>(command.minuteOfDay & M_MINUTE_MASK) };

    plaintext[M_PT_ARM_DELAY] = static_cast<uint8_t>((command.armActive ? M_ARM_BIT : 0) | ((command.delayCode & M_DELAY_MASK) << M_DELAY_SHIFT));
    plaintext[M_PT_ACTIVATIONS_MODE] =
        static_cast<uint8_t>(((command.activations - 1) & M_ACTIVATIONS_MASK) | ((static_cast<uint8_t>(command.mode) & M_MODE_MASK) << M_MODE_SHIFT));
    plaintext[M_PT_COOLDOWN]    = command.cooldownByte;
    plaintext[M_PT_SENSITIVITY] = command.sensitivityByte;
    plaintext[M_PT_MINUTE]      = static_cast<uint8_t>(minuteField & 0xFF);
    plaintext[M_PT_MINUTE + 1]  = static_cast<uint8_t>(minuteField >> 8);
    plaintext[6]                = 0;
    plaintext[7]                = 0;
  }

}
```

In `Makefile`, set `HOST_SRCS = src/mfs_protocol.cpp`.

- [ ] **Step 4: Run to verify it passes**

Run: `make test`
Expected: `protocol tables: OK`, `command codec: OK`, `ALL TESTS PASSED`.

- [ ] **Step 5: Commit**

```bash
/Users/andy/nrfenv/bin/clang-format -i src/mfs_protocol.hpp src/mfs_protocol.cpp tests/test_protocol.cpp tests/test_main.cpp
git add src/mfs_protocol.hpp src/mfs_protocol.cpp tests/ Makefile
git commit -m "Add the command plaintext codec

Eight bytes: arm and delay, activations and mode, cooldown, sensitivity, the
UTC minute of day, and two per-variant bytes. Activations are stored minus one,
so every 4-bit value is legal and an out-of-range count cannot exist.

A reserved mode or a minute of 1440 or more is refused. The per-variant bytes and
reserved bits are deliberately not examined - a test proves they may hold
anything, because validating them would make MFS_1 reject a future app build."
```

---

## Task 3: Crypto seam, access keys and known-answer vectors

The firmware and the app implement the same derivations in different languages on different crypto libraries. **Neither side's tests may be the evidence for the other.** A third implementation — Python's `cryptography` — generates the vectors, and both sides must reproduce them.

**Files:**
- Create: `tools/gen_access_vectors.py`
- Create: `src/access_vectors.hpp` (generated — commit it)
- Create: `src/crypto.hpp`, `tests/crypto_openssl.cpp`
- Create: `src/access_keys.hpp`, `src/access_keys.cpp`
- Create: `tests/test_access_keys.cpp`
- Modify: `Makefile`, `tests/test_main.cpp`

**Interfaces:**
- Consumes: `protocol::M_*` layout constants from Task 2.
- Produces (namespace `alc::access`): `M_SECRET_BYTES`, `M_DAY_KEY_BYTES`, `M_NONCE_BYTES`, `M_SLOT_COUNT` (8), `M_SLOT_NETWORK_MANAGER` (0); `int DeriveDayKey(const uint8_t* secret, uint32_t deviceId, uint16_t day, uint8_t slot, uint8_t* dayKey)`; `int DeriveEncKey(const uint8_t* dayKey, uint8_t* encKey)`; `int DeriveRotatingId(const uint8_t* dayKey, uint32_t n, uint8_t* rotatingId)`; `void BuildNonce(...)`; `int SealCommand(const uint8_t* dayKey, uint32_t deviceId, uint16_t day, uint8_t slot, uint32_t n, const uint8_t* plaintext, uint8_t* onAir)`; `int OpenCommand(..., const uint8_t* onAir, uint8_t* plaintext)` returning `-EBADMSG` when not authentic; `int BuildTimeSync(const uint8_t* provisionKey, uint32_t deviceId, uint32_t unixSeconds, uint8_t* onAir)`; `bool OpenTimeSync(const uint8_t* provisionKey, uint32_t deviceId, const uint8_t* onAir, uint32_t& unixSeconds)`. Namespace `alc::crypto`: `Init()`, `HmacSha256(...)`, `AesCcmEncrypt(...)`, `AesCcmDecrypt(...)`, `ConstantTimeEqual(...)`. Namespace `alc::access::vectors`: `M_SECRET`, `M_PROVISION_KEY`, `M_DEVICE_ID`, `M_COMMANDS[3]`, `M_TIME_SYNC_UNIX`, `M_TIME_SYNC_ON_AIR`.

- [ ] **Step 1: Write the vector generator**

Check the reference library is present: `python3 -c "import cryptography; print(cryptography.__version__)"` (46.0.5 was used; any version with `AESCCM` supporting `tag_length=4` works).

Create `tools/gen_access_vectors.py`:

```python
#!/usr/bin/env python3
"""Generates known-answer vectors for the MFS_1 access scheme.

A THIRD, INDEPENDENT IMPLEMENTATION. The firmware (PSA on target, OpenSSL on the
host) and the app (pointycastle) must each reproduce these bytes exactly. Python's
`cryptography` package is the reference because it is neither of the two
implementations under test - a vector generated by one side and checked by the
same side proves nothing.

Scheme: docs/tan-scheme.md. Run from the class_mfs_1 root:
    python3 tools/gen_access_vectors.py
"""
import hashlib
import hmac
import pathlib
import struct

from cryptography.hazmat.primitives.ciphers.aead import AESCCM

LABEL_DAY_KEY = 0x02
LABEL_ENC_KEY = 0x03
LABEL_ROTATING_ID = 0x04
LABEL_TIME_SYNC = 0x05
PROTOCOL_VERSION = 0x02
TAG_BYTES = 4

SECRET = bytes(range(0x00, 0x20))          # bench secret 00 01 .. 1F
PROVISION_KEY = bytes(range(0x20, 0x40))   # bench provisioning key 20 .. 3F
DEVICE_ID = 0x4D465331                     # 'MFS1'


def hmac256(key: bytes, message: bytes) -> bytes:
    return hmac.new(key, message, hashlib.sha256).digest()


def day_key(secret: bytes, device_id: int, day: int, slot: int) -> bytes:
    return hmac256(secret, struct.pack(">IHB", device_id, day, slot) + bytes([LABEL_DAY_KEY]))


def enc_key(dk: bytes) -> bytes:
    return hmac256(dk, bytes([LABEL_ENC_KEY]))[:16]


def rotating_id(dk: bytes, n: int) -> bytes:
    return hmac256(dk, struct.pack(">I", n) + bytes([LABEL_ROTATING_ID]))[:4]


def nonce(device_id: int, day: int, slot: int, n: int) -> bytes:
    return struct.pack(">IHBI", device_id, day, slot, n)


def seal(secret, device_id, day, slot, n, plaintext: bytes) -> bytes:
    dk = day_key(secret, device_id, day, slot)
    rid = rotating_id(dk, n)
    aad = rid + bytes([PROTOCOL_VERSION])
    sealed = AESCCM(enc_key(dk), tag_length=TAG_BYTES).encrypt(nonce(device_id, day, slot, n), plaintext, aad)
    return rid + sealed  # rid(4) | ciphertext(8) | tag(4)


def time_sync(provision_key, device_id, unix: int) -> bytes:
    tag = hmac256(provision_key, struct.pack(">II", device_id, unix) + bytes([LABEL_TIME_SYNC]))[:12]
    return struct.pack("<I", unix) + tag


CASES = [
    # (name, day, slot, n, plaintext)
    ("arm_slot1_n0", 256, 1, 0, bytes([0x01, 0x02, 128, 143, 0x1E, 0x02, 0x00, 0x00])),
    ("disarm_slot7_n15", 256, 7, 15, bytes([0x00, 0x00, 0, 143, 0x9F, 0x05, 0xAA, 0xBB])),
    ("mode_slot0_n3", 257, 0, 3, bytes([0x00, 0x20, 0, 200, 0x00, 0x00, 0x00, 0x00])),
]
TIME_SYNC_UNIX = 1767225600 + 256 * 86400 + 4 * 3600 + 3723  # day 256, 05:02:03 UTC

BANNER = "GENERATED by tools/gen_access_vectors.py - DO NOT EDIT BY HAND."


def cpp_bytes(b: bytes) -> str:
    return "{ " + ", ".join(f"0x{x:02X}" for x in b) + " }"


def dart_bytes(b: bytes) -> str:
    return "<int>[" + ", ".join(f"0x{x:02X}" for x in b) + "]"


def main():
    here = pathlib.Path(__file__).resolve().parent.parent
    cpp = [f"// {BANNER}", "", "#pragma once", "", "#include <cstdint>", "", "namespace alc::access::vectors", "{", ""]
    cpp.append(f"  constexpr uint8_t M_SECRET[32] {cpp_bytes(SECRET)};")
    cpp.append(f"  constexpr uint8_t M_PROVISION_KEY[32] {cpp_bytes(PROVISION_KEY)};")
    cpp.append(f"  constexpr uint32_t M_DEVICE_ID {{ 0x{DEVICE_ID:08X} }};")
    cpp.append("")
    cpp.append("  struct CommandVector")
    cpp.append("  {")
    cpp.append("      uint16_t day;")
    cpp.append("      uint8_t slot;")
    cpp.append("      uint32_t n;")
    cpp.append("      uint8_t dayKey[32];")
    cpp.append("      uint8_t encKey[16];")
    cpp.append("      uint8_t rotatingId[4];")
    cpp.append("      uint8_t plaintext[8];")
    cpp.append("      uint8_t onAir[16];")
    cpp.append("  };")
    cpp.append("")
    cpp.append(f"  constexpr CommandVector M_COMMANDS[{len(CASES)}] {{")
    dart = [f"// {BANNER}", "", f"const List<int> kVectorSecret = {dart_bytes(SECRET)};",
            f"const List<int> kVectorProvisionKey = {dart_bytes(PROVISION_KEY)};",
            f"const int kVectorDeviceId = 0x{DEVICE_ID:08X};", "",
            "class CommandVector {",
            "  const CommandVector(this.day, this.slot, this.n, this.dayKey, this.encKey, this.rotatingId, this.plaintext, this.onAir);",
            "  final int day, slot, n;",
            "  final List<int> dayKey, encKey, rotatingId, plaintext, onAir;",
            "}", "", "const List<CommandVector> kCommandVectors = <CommandVector>["]
    for name, day, slot, n, pt in CASES:
        dk = day_key(SECRET, DEVICE_ID, day, slot)
        ek = enc_key(dk)
        rid = rotating_id(dk, n)
        air = seal(SECRET, DEVICE_ID, day, slot, n, pt)
        cpp.append(f"    // {name}")
        cpp.append(f"    {{ {day}, {slot}, {n}, {cpp_bytes(dk)}, {cpp_bytes(ek)}, {cpp_bytes(rid)}, {cpp_bytes(pt)}, {cpp_bytes(air)} }},")
        dart.append(f"  // {name}")
        dart.append(f"  CommandVector({day}, {slot}, {n}, {dart_bytes(dk)}, {dart_bytes(ek)}, {dart_bytes(rid)}, {dart_bytes(pt)}, {dart_bytes(air)}),")
    cpp.append("  };")
    cpp.append("")
    ts = time_sync(PROVISION_KEY, DEVICE_ID, TIME_SYNC_UNIX)
    cpp.append(f"  constexpr uint32_t M_TIME_SYNC_UNIX {{ {TIME_SYNC_UNIX} }};")
    cpp.append(f"  constexpr uint8_t M_TIME_SYNC_ON_AIR[16] {cpp_bytes(ts)};")
    cpp += ["", "}", ""]
    dart.append("];")
    dart.append("")
    dart.append(f"const int kVectorTimeSyncUnix = {TIME_SYNC_UNIX};")
    dart.append(f"const List<int> kVectorTimeSyncOnAir = {dart_bytes(ts)};")
    dart.append("")

    cpp_path = here / "src" / "access_vectors.hpp"
    cpp_path.write_text("\n".join(cpp))
    print(f"wrote {cpp_path}")
    dart_path = here.parent / "class_app" / "test" / "access_vectors.dart"
    if dart_path.parent.exists():
        dart_path.write_text("\n".join(dart))
        print(f"wrote {dart_path}")
    else:
        print(f"skipped {dart_path} (app not scaffolded yet)")


if __name__ == "__main__":
    main()
```

- [ ] **Step 2: Generate and check the first vector**

```bash
python3 tools/gen_access_vectors.py
grep -A2 "arm_slot1_n0" src/access_vectors.hpp | tail -1 | grep -o "0x45, 0xE9, 0x10, 0x47, 0xE3, 0x7F, 0x62, 0x27, 0x72, 0x7F, 0x9B, 0x51, 0x37, 0x9C, 0x11, 0xF3"
```

Expected: the on-air bytes print, matching `docs/tan-scheme.md` §3. If not, the generator does not match the scheme document — stop and reconcile before writing any C++.

- [ ] **Step 3: Write the crypto seam and the host backend**

Create `src/crypto.hpp`:

```cpp
#pragma once

#include <cstddef>
#include <cstdint>

namespace alc::crypto
{

  // The one seam between the access scheme and a crypto library. On target it
  // is backed by PSA (CRACEN, src/crypto_psa.cpp); on the host by OpenSSL
  // (tests/crypto_openssl.cpp). Both are proven against the same Python-generated
  // vectors, so neither is trusted on its own word.

  constexpr size_t M_HMAC_SHA256_BYTES { 32 };
  constexpr size_t M_AES128_KEY_BYTES { 16 };

  /** @brief Initialise the backend. Idempotent. @return 0 or negative errno. */
  int Init();

  /** @brief HMAC-SHA256. @param out M_HMAC_SHA256_BYTES bytes. @return 0 or negative errno. */
  int HmacSha256(const uint8_t* key, size_t keyLength, const uint8_t* message, size_t messageLength, uint8_t* out);

  /**
   * @brief AES-128-CCM encrypt with a shortened tag.
   * @return 0 or negative errno.
   */
  int AesCcmEncrypt(const uint8_t* key, const uint8_t* nonce, size_t nonceLength, const uint8_t* aad, size_t aadLength, const uint8_t* plaintext,
                    size_t length, uint8_t* ciphertext, uint8_t* tag, size_t tagLength);

  /**
   * @brief AES-128-CCM decrypt and verify.
   * @return 0 if authentic; -EBADMSG if the tag does not verify; other negative errno on a backend fault.
   *         On any failure `plaintext` must not be used.
   */
  int AesCcmDecrypt(const uint8_t* key, const uint8_t* nonce, size_t nonceLength, const uint8_t* aad, size_t aadLength, const uint8_t* ciphertext,
                    size_t length, const uint8_t* tag, size_t tagLength, uint8_t* plaintext);

  /** @brief Compare without an early exit, so timing does not reveal how many bytes matched. */
  inline bool ConstantTimeEqual(const uint8_t* a, const uint8_t* b, size_t length)
  {
    uint8_t difference { 0 };

    for (size_t index = 0; index < length; index++) {
      difference = static_cast<uint8_t>(difference | (a[index] ^ b[index]));
    }
    return difference == 0;
  }

}
```

Create `tests/crypto_openssl.cpp`:

```cpp
// HOST-ONLY crypto backend for the unit tests. Never linked into firmware.

#include <cerrno>
#include <memory>

#include <openssl/evp.h>
#include <openssl/hmac.h>

#include "crypto.hpp"

namespace alc::crypto
{

  int Init()
  {
    return 0;
  }

  int HmacSha256(const uint8_t* key, size_t keyLength, const uint8_t* message, size_t messageLength, uint8_t* out)
  {
    unsigned int outLength { 0 };

    if (HMAC(EVP_sha256(), key, static_cast<int>(keyLength), message, messageLength, out, &outLength) == nullptr) { return -EIO; }
    return outLength == M_HMAC_SHA256_BYTES ? 0 : -EIO;
  }

  namespace
  {
    using ContextPtr = std::unique_ptr<EVP_CIPHER_CTX, decltype(&EVP_CIPHER_CTX_free)>;

    int ccm(bool encrypt, const uint8_t* key, const uint8_t* nonce, size_t nonceLength, const uint8_t* aad, size_t aadLength, const uint8_t* input,
            size_t length, uint8_t* output, uint8_t* tag, size_t tagLength)
    {
      ContextPtr context { EVP_CIPHER_CTX_new(), &EVP_CIPHER_CTX_free };
      int outLength { 0 };
      int op { encrypt ? 1 : 0 };

      if (!context) { return -ENOMEM; }

      // CCM in OpenSSL is order-sensitive: nonce and tag lengths first, then key
      // and nonce, then the total data length, then AAD, then the data in ONE update.
      if (EVP_CipherInit_ex(context.get(), EVP_aes_128_ccm(), nullptr, nullptr, nullptr, op) != 1) { return -EIO; }
      if (EVP_CIPHER_CTX_ctrl(context.get(), EVP_CTRL_AEAD_SET_IVLEN, static_cast<int>(nonceLength), nullptr) != 1) { return -EIO; }
      if (EVP_CIPHER_CTX_ctrl(context.get(), EVP_CTRL_AEAD_SET_TAG, static_cast<int>(tagLength), encrypt ? nullptr : tag) != 1) { return -EIO; }
      if (EVP_CipherInit_ex(context.get(), nullptr, nullptr, key, nonce, op) != 1) { return -EIO; }
      if (EVP_CipherUpdate(context.get(), nullptr, &outLength, nullptr, static_cast<int>(length)) != 1) { return -EIO; }
      if (EVP_CipherUpdate(context.get(), nullptr, &outLength, aad, static_cast<int>(aadLength)) != 1) { return -EIO; }

      // For decrypt, this is where CCM reports a tag mismatch.
      if (EVP_CipherUpdate(context.get(), output, &outLength, input, static_cast<int>(length)) != 1) { return encrypt ? -EIO : -EBADMSG; }

      if (encrypt && EVP_CIPHER_CTX_ctrl(context.get(), EVP_CTRL_AEAD_GET_TAG, static_cast<int>(tagLength), tag) != 1) { return -EIO; }
      return 0;
    }
  }

  int AesCcmEncrypt(const uint8_t* key, const uint8_t* nonce, size_t nonceLength, const uint8_t* aad, size_t aadLength, const uint8_t* plaintext,
                    size_t length, uint8_t* ciphertext, uint8_t* tag, size_t tagLength)
  {
    return ccm(true, key, nonce, nonceLength, aad, aadLength, plaintext, length, ciphertext, tag, tagLength);
  }

  int AesCcmDecrypt(const uint8_t* key, const uint8_t* nonce, size_t nonceLength, const uint8_t* aad, size_t aadLength, const uint8_t* ciphertext,
                    size_t length, const uint8_t* tag, size_t tagLength, uint8_t* plaintext)
  {
    uint8_t tagCopy[16] {};

    if (tagLength > sizeof(tagCopy)) { return -EINVAL; }
    for (size_t index = 0; index < tagLength; index++) {
      tagCopy[index] = tag[index];
    }
    return ccm(false, key, nonce, nonceLength, aad, aadLength, ciphertext, length, plaintext, tagCopy, tagLength);
  }

}
```

Replace the first three lines of `Makefile` so OpenSSL 3 from Homebrew is found (`brew install openssl@3` if absent):

```make
CXX      ?= g++
OPENSSL  ?= $(shell brew --prefix openssl@3 2>/dev/null)
CXXFLAGS ?= -std=c++20 -Wall -Wextra -Wpedantic -Werror -O0 -g -Isrc -Itests -I$(OPENSSL)/include
LDFLAGS  ?= -L$(OPENSSL)/lib -lcrypto
```

- [ ] **Step 4: Write the failing test**

Create `tests/test_access_keys.cpp`:

```cpp
#include <cassert>
#include <cerrno>
#include <cstdio>
#include <cstring>

#include "access_keys.hpp"
#include "access_vectors.hpp"

void run_access_key_tests()
{
  using namespace alc;
  using namespace alc::access;

  // Every vector was produced by tools/gen_access_vectors.py using Python's
  // `cryptography` package - an implementation that is neither this one nor the
  // app's. Matching it is the only evidence the two sides agree.
  for (const vectors::CommandVector& vector : vectors::M_COMMANDS) {
    uint8_t dayKey[M_DAY_KEY_BYTES] {};
    uint8_t encKey[crypto::M_AES128_KEY_BYTES] {};
    uint8_t rotatingId[protocol::M_ROTATING_ID_BYTES] {};
    uint8_t onAir[protocol::M_UUID_BYTES] {};
    uint8_t plaintext[protocol::M_PLAINTEXT_BYTES] {};

    assert(DeriveDayKey(vectors::M_SECRET, vectors::M_DEVICE_ID, vector.day, vector.slot, dayKey) == 0);
    assert(memcmp(dayKey, vector.dayKey, sizeof(dayKey)) == 0);

    assert(DeriveEncKey(dayKey, encKey) == 0);
    assert(memcmp(encKey, vector.encKey, sizeof(encKey)) == 0);

    assert(DeriveRotatingId(dayKey, vector.n, rotatingId) == 0);
    assert(memcmp(rotatingId, vector.rotatingId, sizeof(rotatingId)) == 0);

    assert(SealCommand(dayKey, vectors::M_DEVICE_ID, vector.day, vector.slot, vector.n, vector.plaintext, onAir) == 0);
    assert(memcmp(onAir, vector.onAir, sizeof(onAir)) == 0);

    assert(OpenCommand(dayKey, vectors::M_DEVICE_ID, vector.day, vector.slot, vector.n, vector.onAir, plaintext) == 0);
    assert(memcmp(plaintext, vector.plaintext, sizeof(plaintext)) == 0);

    // A single flipped bit anywhere authenticated must fail - ID, ciphertext or tag.
    const uint8_t positions[] { protocol::M_OFFSET_ROTATING_ID, protocol::M_OFFSET_CIPHERTEXT, protocol::M_OFFSET_TAG };
    for (uint8_t position : positions) {
      memcpy(onAir, vector.onAir, sizeof(onAir));
      onAir[position] ^= 0x01;
      assert(OpenCommand(dayKey, vectors::M_DEVICE_ID, vector.day, vector.slot, vector.n, onAir, plaintext) == -EBADMSG);
    }

    // The right bytes opened as the WRONG sequence number, slot or day must fail.
    // Each is bound into the nonce, which is what stops a command being replayed
    // under another n.
    assert(OpenCommand(dayKey, vectors::M_DEVICE_ID, vector.day, vector.slot, vector.n + 1, vector.onAir, plaintext) == -EBADMSG);
    assert(OpenCommand(dayKey, vectors::M_DEVICE_ID, vector.day, static_cast<uint8_t>(vector.slot ^ 1), vector.n, vector.onAir, plaintext) ==
           -EBADMSG);
    assert(OpenCommand(dayKey, vectors::M_DEVICE_ID, static_cast<uint16_t>(vector.day + 1), vector.slot, vector.n, vector.onAir, plaintext) ==
           -EBADMSG);
  }

  // Distinct slots on the same day must have unrelated day keys.
  {
    uint8_t slotZero[M_DAY_KEY_BYTES] {};
    uint8_t slotOne[M_DAY_KEY_BYTES] {};
    assert(DeriveDayKey(vectors::M_SECRET, vectors::M_DEVICE_ID, 256, 0, slotZero) == 0);
    assert(DeriveDayKey(vectors::M_SECRET, vectors::M_DEVICE_ID, 256, 1, slotOne) == 0);
    assert(memcmp(slotZero, slotOne, sizeof(slotZero)) != 0);
  }

  // Time sync.
  {
    uint8_t onAir[protocol::M_UUID_BYTES] {};
    uint32_t unixSeconds { 0 };

    assert(BuildTimeSync(vectors::M_PROVISION_KEY, vectors::M_DEVICE_ID, vectors::M_TIME_SYNC_UNIX, onAir) == 0);
    assert(memcmp(onAir, vectors::M_TIME_SYNC_ON_AIR, sizeof(onAir)) == 0);

    assert(OpenTimeSync(vectors::M_PROVISION_KEY, vectors::M_DEVICE_ID, vectors::M_TIME_SYNC_ON_AIR, unixSeconds));
    assert(unixSeconds == vectors::M_TIME_SYNC_UNIX);

    // Changing the time without the key must fail - this is the whole point.
    memcpy(onAir, vectors::M_TIME_SYNC_ON_AIR, sizeof(onAir));
    onAir[0] ^= 0x01;
    unixSeconds = 0;
    assert(!OpenTimeSync(vectors::M_PROVISION_KEY, vectors::M_DEVICE_ID, onAir, unixSeconds));
    assert(unixSeconds == 0);

    // The DEVICE SECRET must not authorise a time sync. The two keys are
    // deliberately separate: a provisioner can set the clock and nothing else.
    assert(!OpenTimeSync(vectors::M_SECRET, vectors::M_DEVICE_ID, vectors::M_TIME_SYNC_ON_AIR, unixSeconds));

    // Nor may a sync for one device work on another.
    assert(!OpenTimeSync(vectors::M_PROVISION_KEY, vectors::M_DEVICE_ID + 1, vectors::M_TIME_SYNC_ON_AIR, unixSeconds));
  }

  printf("access keys: OK\n");
}
```

Declare and call `run_access_key_tests();` in `tests/test_main.cpp`.

- [ ] **Step 5: Run to verify it fails**

Run: `make test`
Expected: FAIL — `'access_keys.hpp' file not found`.

- [ ] **Step 6: Implement the derivations**

Create `src/access_keys.hpp`:

```cpp
#pragma once

#include <cstdint>

#include "crypto.hpp"
#include "mfs_protocol.hpp"

namespace alc::access
{

  // The derivations of docs/tan-scheme.md section 3. Every function here is
  // mirrored byte for byte by class_app/lib/protocol/access_keys.dart, and both
  // are checked against tools/gen_access_vectors.py.

  constexpr uint8_t M_SECRET_BYTES { 32 };
  constexpr uint8_t M_DAY_KEY_BYTES { 32 };
  constexpr uint8_t M_NONCE_BYTES { 11 };
  constexpr uint8_t M_AAD_BYTES { 5 };

  // Eight key slots per device per day. Slot 0 is the Network Manager's own and
  // is the only slot whose commands may change the operating mode. Slots 1-7 are
  // assigned to engineers by the Network Manager, per device and per day.
  constexpr uint8_t M_SLOT_COUNT { 8 };
  constexpr uint8_t M_SLOT_NETWORK_MANAGER { 0 };

  // Domain-separation labels. 0x00 and 0x01 were the retired paper-TAN and
  // session-key labels and must not be reused.
  constexpr uint8_t M_LABEL_DAY_KEY { 0x02 };
  constexpr uint8_t M_LABEL_ENC_KEY { 0x03 };
  constexpr uint8_t M_LABEL_ROTATING_ID { 0x04 };
  constexpr uint8_t M_LABEL_TIME_SYNC { 0x05 };

  constexpr uint8_t M_TIME_SYNC_TAG_BYTES { 12 };

  /** @brief dayKey = HMAC-SHA256(secret, id BE32 | day BE16 | slot | 0x02). */
  int DeriveDayKey(const uint8_t* secret, uint32_t deviceId, uint16_t day, uint8_t slot, uint8_t* dayKey);

  /** @brief encKey = first 16 bytes of HMAC-SHA256(dayKey, 0x03). */
  int DeriveEncKey(const uint8_t* dayKey, uint8_t* encKey);

  /** @brief rotatingId(n) = first 4 bytes of HMAC-SHA256(dayKey, n BE32 | 0x04). */
  int DeriveRotatingId(const uint8_t* dayKey, uint32_t n, uint8_t* rotatingId);

  /** @brief nonce = id BE32 | day BE16 | slot | n BE32 (11 bytes). */
  void BuildNonce(uint32_t deviceId, uint16_t day, uint8_t slot, uint32_t n, uint8_t* nonce);

  /**
   * @brief Build the 16 on-air bytes for command number n.
   *
   * Used by the host tests and the on-target self-test. The device itself only
   * ever opens commands; the app seals them.
   */
  int SealCommand(const uint8_t* dayKey, uint32_t deviceId, uint16_t day, uint8_t slot, uint32_t n, const uint8_t* plaintext, uint8_t* onAir);

  /**
   * @brief Authenticate and decrypt 16 on-air bytes as command number n.
   * @return 0 if authentic; -EBADMSG if not; other negative errno on a backend fault.
   */
  int OpenCommand(const uint8_t* dayKey, uint32_t deviceId, uint16_t day, uint8_t slot, uint32_t n, const uint8_t* onAir, uint8_t* plaintext);

  /** @brief Time sync on air: unix LE32 | first 12 bytes of HMAC-SHA256(provisionKey, id BE32 | unix BE32 | 0x05). */
  int BuildTimeSync(const uint8_t* provisionKey, uint32_t deviceId, uint32_t unixSeconds, uint8_t* onAir);

  /**
   * @brief Verify a time sync. Says nothing about whether the time is acceptable -
   *        that is DeviceClock::ApplyProvisionerSync().
   * @return True only if the tag verifies; `unixSeconds` is written only then.
   */
  bool OpenTimeSync(const uint8_t* provisionKey, uint32_t deviceId, const uint8_t* onAir, uint32_t& unixSeconds);

}
```

Create `src/access_keys.cpp`:

```cpp
#include <cerrno>
#include <cstring>

#include "access_keys.hpp"

namespace alc::access
{

  namespace
  {
    void putBe32(uint8_t* out, uint32_t value)
    {
      out[0] = static_cast<uint8_t>(value >> 24);
      out[1] = static_cast<uint8_t>(value >> 16);
      out[2] = static_cast<uint8_t>(value >> 8);
      out[3] = static_cast<uint8_t>(value);
    }

    void putBe16(uint8_t* out, uint16_t value)
    {
      out[0] = static_cast<uint8_t>(value >> 8);
      out[1] = static_cast<uint8_t>(value);
    }

    void buildAad(const uint8_t* onAir, uint8_t* aad)
    {
      // The version is NEVER on air. Both sides supply it, so a payload from
      // another protocol version fails authentication instead of mis-parsing.
      memcpy(aad, &onAir[protocol::M_OFFSET_ROTATING_ID], protocol::M_ROTATING_ID_BYTES);
      aad[protocol::M_ROTATING_ID_BYTES] = protocol::M_PROTOCOL_VERSION;
    }
  }

  int DeriveDayKey(const uint8_t* secret, uint32_t deviceId, uint16_t day, uint8_t slot, uint8_t* dayKey)
  {
    constexpr uint8_t M_MESSAGE_BYTES { 8 };
    uint8_t message[M_MESSAGE_BYTES] {};

    putBe32(&message[0], deviceId);
    putBe16(&message[4], day);
    message[6] = slot;
    message[7] = M_LABEL_DAY_KEY;
    return crypto::HmacSha256(secret, M_SECRET_BYTES, message, sizeof(message), dayKey);
  }

  int DeriveEncKey(const uint8_t* dayKey, uint8_t* encKey)
  {
    const uint8_t message[] { M_LABEL_ENC_KEY };
    uint8_t mac[crypto::M_HMAC_SHA256_BYTES] {};
    int result { crypto::HmacSha256(dayKey, M_DAY_KEY_BYTES, message, sizeof(message), mac) };

    if (result < 0) { return result; }
    memcpy(encKey, mac, crypto::M_AES128_KEY_BYTES);
    return 0;
  }

  int DeriveRotatingId(const uint8_t* dayKey, uint32_t n, uint8_t* rotatingId)
  {
    constexpr uint8_t M_MESSAGE_BYTES { 5 };
    uint8_t message[M_MESSAGE_BYTES] {};
    uint8_t mac[crypto::M_HMAC_SHA256_BYTES] {};
    int result { 0 };

    putBe32(&message[0], n);
    message[4] = M_LABEL_ROTATING_ID;
    result     = crypto::HmacSha256(dayKey, M_DAY_KEY_BYTES, message, sizeof(message), mac);
    if (result < 0) { return result; }
    memcpy(rotatingId, mac, protocol::M_ROTATING_ID_BYTES);
    return 0;
  }

  void BuildNonce(uint32_t deviceId, uint16_t day, uint8_t slot, uint32_t n, uint8_t* nonce)
  {
    putBe32(&nonce[0], deviceId);
    putBe16(&nonce[4], day);
    nonce[6] = slot;
    putBe32(&nonce[7], n);
  }

  int SealCommand(const uint8_t* dayKey, uint32_t deviceId, uint16_t day, uint8_t slot, uint32_t n, const uint8_t* plaintext, uint8_t* onAir)
  {
    uint8_t encKey[crypto::M_AES128_KEY_BYTES] {};
    uint8_t nonce[M_NONCE_BYTES] {};
    uint8_t aad[M_AAD_BYTES] {};
    int result { DeriveRotatingId(dayKey, n, &onAir[protocol::M_OFFSET_ROTATING_ID]) };

    if (result < 0) { return result; }
    result = DeriveEncKey(dayKey, encKey);
    if (result < 0) { return result; }

    BuildNonce(deviceId, day, slot, n, nonce);
    buildAad(onAir, aad);
    return crypto::AesCcmEncrypt(encKey, nonce, sizeof(nonce), aad, sizeof(aad), plaintext, protocol::M_PLAINTEXT_BYTES,
                                 &onAir[protocol::M_OFFSET_CIPHERTEXT], &onAir[protocol::M_OFFSET_TAG], protocol::M_TAG_BYTES);
  }

  int OpenCommand(const uint8_t* dayKey, uint32_t deviceId, uint16_t day, uint8_t slot, uint32_t n, const uint8_t* onAir, uint8_t* plaintext)
  {
    uint8_t encKey[crypto::M_AES128_KEY_BYTES] {};
    uint8_t nonce[M_NONCE_BYTES] {};
    uint8_t aad[M_AAD_BYTES] {};
    int result { DeriveEncKey(dayKey, encKey) };

    if (result < 0) { return result; }

    BuildNonce(deviceId, day, slot, n, nonce);
    buildAad(onAir, aad);
    return crypto::AesCcmDecrypt(encKey, nonce, sizeof(nonce), aad, sizeof(aad), &onAir[protocol::M_OFFSET_CIPHERTEXT], protocol::M_PLAINTEXT_BYTES,
                                 &onAir[protocol::M_OFFSET_TAG], protocol::M_TAG_BYTES, plaintext);
  }

  namespace
  {
    int timeSyncTag(const uint8_t* provisionKey, uint32_t deviceId, uint32_t unixSeconds, uint8_t* tag)
    {
      constexpr uint8_t M_MESSAGE_BYTES { 9 };
      uint8_t message[M_MESSAGE_BYTES] {};
      uint8_t mac[crypto::M_HMAC_SHA256_BYTES] {};
      int result { 0 };

      putBe32(&message[0], deviceId);
      putBe32(&message[4], unixSeconds);
      message[8] = M_LABEL_TIME_SYNC;
      result     = crypto::HmacSha256(provisionKey, M_SECRET_BYTES, message, sizeof(message), mac);
      if (result < 0) { return result; }
      memcpy(tag, mac, M_TIME_SYNC_TAG_BYTES);
      return 0;
    }
  }

  int BuildTimeSync(const uint8_t* provisionKey, uint32_t deviceId, uint32_t unixSeconds, uint8_t* onAir)
  {
    onAir[0] = static_cast<uint8_t>(unixSeconds);
    onAir[1] = static_cast<uint8_t>(unixSeconds >> 8);
    onAir[2] = static_cast<uint8_t>(unixSeconds >> 16);
    onAir[3] = static_cast<uint8_t>(unixSeconds >> 24);
    return timeSyncTag(provisionKey, deviceId, unixSeconds, &onAir[4]);
  }

  bool OpenTimeSync(const uint8_t* provisionKey, uint32_t deviceId, const uint8_t* onAir, uint32_t& unixSeconds)
  {
    uint8_t expected[M_TIME_SYNC_TAG_BYTES] {};
    uint32_t presented { static_cast<uint32_t>(onAir[0]) | (static_cast<uint32_t>(onAir[1]) << 8) | (static_cast<uint32_t>(onAir[2]) << 16) |
                         (static_cast<uint32_t>(onAir[3]) << 24) };

    if (timeSyncTag(provisionKey, deviceId, presented, expected) < 0) { return false; }
    if (!crypto::ConstantTimeEqual(expected, &onAir[4], M_TIME_SYNC_TAG_BYTES)) { return false; }

    unixSeconds = presented;
    return true;
  }

}
```

In `Makefile`: `HOST_SRCS = src/mfs_protocol.cpp src/access_keys.cpp tests/crypto_openssl.cpp`.

- [ ] **Step 7: Run to verify it passes**

Run: `make test`
Expected: `access keys: OK` and `ALL TESTS PASSED`. **A vector mismatch here is a scheme bug, not a test to adjust** — regenerate nothing and fix the C++.

- [ ] **Step 8: Commit**

```bash
/Users/andy/nrfenv/bin/clang-format -i src/crypto.hpp src/access_keys.hpp src/access_keys.cpp tests/crypto_openssl.cpp tests/test_access_keys.cpp tests/test_main.cpp
git add tools/gen_access_vectors.py src/access_vectors.hpp src/crypto.hpp src/access_keys.hpp src/access_keys.cpp tests/ Makefile
git commit -m "Add the access-key derivations behind a crypto seam

Day key, encryption key, rotating ID, nonce, CCM seal and open, and the
time-sync tag, exactly as docs/tan-scheme.md section 3 defines them.

The evidence is independent: tools/gen_access_vectors.py produces known-answer
vectors with Python's cryptography package, and the host build reproduces them
on OpenSSL. The app will reproduce the same vectors on pointycastle, and the
firmware on PSA at boot, so no implementation is trusted on its own word.

Tests also prove that a flipped bit in the ID, ciphertext or tag fails, that the
right bytes opened under the wrong n, slot or day fail, and that the device
secret cannot authorise a time sync."
```

---

## Task 4: DeviceClock

**A security component.** The day boundary it defines is what makes a lost key expire.

**Files:**
- Create: `src/device_clock.hpp`, `src/device_clock.cpp`, `tests/test_device_clock.cpp`
- Modify: `Makefile`, `tests/test_main.cpp`

**Interfaces:**
- Consumes: nothing.
- Produces: `class DeviceClock` with `static bool DayIndexOf(uint32_t, uint16_t&)`, `static int16_t MinuteDifference(uint16_t, uint16_t)`, `void RaiseFloorDay(uint16_t)`, `bool HasFloor() const`, `uint16_t FloorDay() const`, `bool IsValid() const`, `uint32_t NowUnix(int64_t uptimeSecs) const`, `uint16_t DayIndex(int64_t) const`, `uint16_t MinuteOfDay(int64_t) const`, `bool IsFresh(uint16_t, int64_t) const`, `SyncResult ApplyProvisionerSync(uint32_t, int64_t)`, `TrimResult ApplyMinuteHint(uint16_t, int64_t)`; enums `SyncResult { Applied, AlreadyValid, BeforeEpoch, BelowFloor, TooFarAhead }`, `TrimResult { Unchanged, Trimmed, Ignored, OverBudget, WouldRewindDay, ClockInvalid }`.

- [ ] **Step 1: Write the failing test**

Create `tests/test_device_clock.cpp`:

```cpp
#include <cassert>
#include <cstdio>

#include "device_clock.hpp"

namespace
{
  using alc::DeviceClock;

  // Day 256 starts at 04:00 UTC on 2026-09-14.
  constexpr uint32_t M_DAY_256_START { DeviceClock::M_EPOCH_UNIX + DeviceClock::M_DAY_BOUNDARY_OFFSET_SECS + 256U * DeviceClock::M_SECONDS_PER_DAY };
  constexpr uint32_t M_HOUR { 3600 };
  constexpr uint32_t M_MINUTE { 60 };
}

void run_device_clock_tests()
{
  uint16_t day { 0 };

  // Day index arithmetic, including the guard against underflow.
  assert(!DeviceClock::DayIndexOf(DeviceClock::M_EPOCH_UNIX, day)); // 00:00, before the first 04:00 boundary
  assert(DeviceClock::DayIndexOf(DeviceClock::M_EPOCH_UNIX + DeviceClock::M_DAY_BOUNDARY_OFFSET_SECS, day) && day == 0);
  assert(DeviceClock::DayIndexOf(M_DAY_256_START, day) && day == 256);
  assert(DeviceClock::DayIndexOf(M_DAY_256_START - 1, day) && day == 255); // 03:59:59 is still yesterday

  // Minute differences wrap across midnight to the nearest match.
  assert(DeviceClock::MinuteDifference(1, 1439) == 2);
  assert(DeviceClock::MinuteDifference(1439, 1) == -2);
  assert(DeviceClock::MinuteDifference(600, 590) == 10);
  assert(DeviceClock::MinuteDifference(0, 720) == -720);

  // A fresh boot is INVALID. There is no resume from NVS.
  {
    DeviceClock clock;
    assert(!clock.IsValid());
    assert(!clock.IsFresh(0, 0));
    assert(clock.ApplyMinuteHint(0, 0) == DeviceClock::TrimResult::ClockInvalid);
  }

  // Factory case: no floor, any post-epoch time is accepted.
  {
    DeviceClock clock;
    constexpr int64_t M_UPTIME { 1000 };
    assert(clock.ApplyProvisionerSync(DeviceClock::M_EPOCH_UNIX - 1, M_UPTIME) == DeviceClock::SyncResult::BeforeEpoch);
    assert(clock.ApplyProvisionerSync(M_DAY_256_START + M_HOUR, M_UPTIME) == DeviceClock::SyncResult::Applied);
    assert(clock.IsValid());
    assert(clock.NowUnix(M_UPTIME) == M_DAY_256_START + M_HOUR);
    assert(clock.NowUnix(M_UPTIME + 10) == M_DAY_256_START + M_HOUR + 10);
    assert(clock.DayIndex(M_UPTIME) == 256);
    assert(clock.MinuteOfDay(M_UPTIME) == 5 * 60); // 05:00 UTC
    assert(clock.HasFloor() && clock.FloorDay() == 256);

    // A valid clock refuses further syncs, however authentic. A captured sync
    // released later would otherwise pull the day back.
    assert(clock.ApplyProvisionerSync(M_DAY_256_START, M_UPTIME) == DeviceClock::SyncResult::AlreadyValid);
  }

  // The floor: never below it, never too far past it.
  {
    DeviceClock clock;
    clock.RaiseFloorDay(256);
    clock.RaiseFloorDay(10); // never lowers
    assert(clock.FloorDay() == 256);
    assert(clock.ApplyProvisionerSync(M_DAY_256_START - 1, 0) == DeviceClock::SyncResult::BelowFloor);
    assert(clock.ApplyProvisionerSync(M_DAY_256_START + 401U * DeviceClock::M_SECONDS_PER_DAY, 0) == DeviceClock::SyncResult::TooFarAhead);
    assert(clock.ApplyProvisionerSync(M_DAY_256_START + 400U * DeviceClock::M_SECONDS_PER_DAY, 0) == DeviceClock::SyncResult::Applied);
  }

  // Freshness and trimming.
  {
    DeviceClock clock;
    constexpr uint16_t M_OWN_MINUTE { 5 * 60 }; // 05:00
    assert(clock.ApplyProvisionerSync(M_DAY_256_START + M_HOUR, 0) == DeviceClock::SyncResult::Applied);

    assert(clock.IsFresh(M_OWN_MINUTE + 10, 0));
    assert(!clock.IsFresh(M_OWN_MINUTE + 11, 0));
    assert(clock.IsFresh(M_OWN_MINUTE - 10, 0));

    // Deadband: one minute is noise.
    assert(clock.ApplyMinuteHint(M_OWN_MINUTE + 1, 0) == DeviceClock::TrimResult::Unchanged);
    assert(clock.NowUnix(0) == M_DAY_256_START + M_HOUR);

    // Beyond five minutes but still fresh: IGNORED, not clamped.
    assert(clock.ApplyMinuteHint(M_OWN_MINUTE + 6, 0) == DeviceClock::TrimResult::Ignored);
    assert(clock.NowUnix(0) == M_DAY_256_START + M_HOUR);

    // Three minutes: applied.
    assert(clock.ApplyMinuteHint(M_OWN_MINUTE + 3, 0) == DeviceClock::TrimResult::Trimmed);
    assert(clock.NowUnix(0) == M_DAY_256_START + M_HOUR + 3 * M_MINUTE);

    // The daily budget counts magnitude: 3 used, so -3 would make 6 > 5.
    assert(clock.ApplyMinuteHint(M_OWN_MINUTE, 0) == DeviceClock::TrimResult::OverBudget);
    assert(clock.ApplyMinuteHint(M_OWN_MINUTE + 3 - 2, 0) == DeviceClock::TrimResult::Trimmed); // -2, total 5
    assert(clock.ApplyMinuteHint(M_OWN_MINUTE + 1 + 2, 0) == DeviceClock::TrimResult::OverBudget);

    // A new day index gets a new budget.
    constexpr int64_t M_NEXT_DAY_UPTIME { DeviceClock::M_SECONDS_PER_DAY };
    uint16_t minuteTomorrow { clock.MinuteOfDay(M_NEXT_DAY_UPTIME) };
    assert(clock.DayIndex(M_NEXT_DAY_UPTIME) == 257);
    assert(clock.ApplyMinuteHint(static_cast<uint16_t>(minuteTomorrow + 2), M_NEXT_DAY_UPTIME) == DeviceClock::TrimResult::Trimmed);
  }

  // A trim may never take the day index below the floor.
  {
    DeviceClock clock;
    // 04:02 on day 256: a -3 minute trim would land at 03:59, day 255.
    assert(clock.ApplyProvisionerSync(M_DAY_256_START + 2 * M_MINUTE, 0) == DeviceClock::SyncResult::Applied);
    assert(clock.ApplyMinuteHint(static_cast<uint16_t>(4 * 60 - 1), 0) == DeviceClock::TrimResult::WouldRewindDay);
    assert(clock.DayIndex(0) == 256);
  }

  printf("device clock: OK\n");
}
```

Declare and call `run_device_clock_tests();` in `tests/test_main.cpp`.

- [ ] **Step 2: Run to verify it fails**

Run: `make test`
Expected: FAIL — `'device_clock.hpp' file not found`.

- [ ] **Step 3: Implement**

Create `src/device_clock.hpp`:

```cpp
#pragma once

#include <cstdint>

namespace alc
{

  /**
   * @brief UTC wall time for the access scheme. A SECURITY COMPONENT.
   *
   * The day boundary this defines is what makes a lost day key expire, so every
   * rule here exists to stop the day index being moved by anyone but the trusted
   * provisioner, and never backwards. See docs/tan-scheme.md section 4 and
   * docs/power-budget.md section 8.7.3.
   *
   * Time is `uptime + offset`. Uptime is the GRTC clocked from the LFXO, passed in
   * by the caller in whole seconds so every rule is host-testable.
   *
   * **The clock starts INVALID on every boot** and becomes valid only through an
   * authenticated provisioner sync. There is no resume-from-NVS path: a device
   * that resumed on a persisted day would revive that day's keys, which is the
   * hole power-budget.md section 8.6 describes. The persisted value is used only
   * as a FLOOR that a sync may not go below.
   */
  class DeviceClock
  {
    public:
      // 2026-01-01T00:00:00Z.
      static constexpr uint32_t M_EPOCH_UNIX { 1767225600 };
      // Day boundary at 04:00 UTC - never local time. See tan-scheme.md section 4.
      static constexpr uint32_t M_DAY_BOUNDARY_OFFSET_SECS { 4 * 3600 };
      static constexpr uint32_t M_SECONDS_PER_DAY { 86400 };
      static constexpr uint16_t M_SECONDS_PER_MINUTE { 60 };
      static constexpr uint16_t M_MINUTES_PER_DAY { 1440 };

      // A sync more than this far past the floor is refused. Bounds the damage a
      // leaked provisioning key can do (pushing the clock years ahead is a denial
      // of service). A device stored unpowered for longer needs wired recovery.
      static constexpr uint16_t M_MAX_FORWARD_JUMP_DAYS { 400 };

      // A command whose minute is further than this from the device's own is
      // stale: it was captured, held and released. Rejected outright.
      static constexpr uint8_t M_FRESHNESS_MINUTES { 10 };
      // Differences below this are noise: 1-minute resolution and an advert up to
      // 30 s old when heard.
      static constexpr uint8_t M_TRIM_DEADBAND_MINUTES { 2 };
      // Differences above this, but still fresh, are ignored rather than clamped.
      static constexpr uint8_t M_TRIM_STEP_MAX_MINUTES { 5 };
      // Total trim magnitude permitted per day index. A correctly loaded LFXO
      // drifts ~2 s/day, so this is ~150x margin, and it caps a time-shift attack.
      static constexpr uint16_t M_TRIM_DAILY_BUDGET_SECS { 300 };

      enum class SyncResult : uint8_t {
        Applied,      ///< The clock is now valid.
        AlreadyValid, ///< Refused: syncs are accepted only while the clock is invalid.
        BeforeEpoch,  ///< Refused: earlier than the first 04:00 UTC of 2026.
        BelowFloor,   ///< Refused: would put the day before one already seen.
        TooFarAhead,  ///< Refused: more than M_MAX_FORWARD_JUMP_DAYS past the floor.
      };

      enum class TrimResult : uint8_t {
        Unchanged,      ///< Inside the deadband.
        Trimmed,        ///< Offset adjusted.
        Ignored,        ///< Fresh but beyond the step limit - the time field is not trusted.
        OverBudget,     ///< Would exceed today's trim budget.
        WouldRewindDay, ///< Would move the day index below the floor.
        ClockInvalid,
      };

      DeviceClock();

      /** @brief Day index of a UNIX time. @return False before the epoch's first boundary. */
      static bool DayIndexOf(uint32_t unixSeconds, uint16_t& day);

      /** @brief Signed minute difference presented - own, wrapped into [-720, 720). */
      static int16_t MinuteDifference(uint16_t presentedMinute, uint16_t ownMinute);

      /** @brief Raise the floor. Never lowers it. */
      void RaiseFloorDay(uint16_t day);

      bool HasFloor() const { return m_has_floor; }
      uint16_t FloorDay() const { return m_floor_day; }
      bool IsValid() const { return m_valid; }

      uint32_t NowUnix(int64_t uptimeSecs) const;

      /** @brief The current day index, never below the floor. Only meaningful when valid. */
      uint16_t DayIndex(int64_t uptimeSecs) const;

      /** @brief UTC minute of day, 0-1439. Only meaningful when valid. */
      uint16_t MinuteOfDay(int64_t uptimeSecs) const;

      /** @brief Whether a presented minute is within M_FRESHNESS_MINUTES. False while invalid. */
      bool IsFresh(uint16_t presentedMinute, int64_t uptimeSecs) const;

      /** @brief Apply an authenticated provisioner time. The tag must already have been verified. */
      SyncResult ApplyProvisionerSync(uint32_t unixSeconds, int64_t uptimeSecs);

      /** @brief Trim drift from an authenticated, fresh command's minute field. */
      TrimResult ApplyMinuteHint(uint16_t presentedMinute, int64_t uptimeSecs);

    private:
      bool m_valid;
      int64_t m_offset_secs;
      bool m_has_floor;
      uint16_t m_floor_day;
      uint16_t m_trim_day;
      uint32_t m_trim_used_secs;
  };

}
```

Create `src/device_clock.cpp`:

```cpp
#include <cstdlib>

#include "device_clock.hpp"

namespace alc
{

  DeviceClock::DeviceClock()
      : m_valid(false)
      , m_offset_secs(0)
      , m_has_floor(false)
      , m_floor_day(0)
      , m_trim_day(0)
      , m_trim_used_secs(0)
  {}

  bool DeviceClock::DayIndexOf(uint32_t unixSeconds, uint16_t& day)
  {
    // Guard the subtraction: a time before the epoch's first boundary would
    // underflow into an enormous day index.
    if (unixSeconds < M_EPOCH_UNIX + M_DAY_BOUNDARY_OFFSET_SECS) { return false; }

    day = static_cast<uint16_t>((unixSeconds - M_EPOCH_UNIX - M_DAY_BOUNDARY_OFFSET_SECS) / M_SECONDS_PER_DAY);
    return true;
  }

  int16_t DeviceClock::MinuteDifference(uint16_t presentedMinute, uint16_t ownMinute)
  {
    constexpr int16_t M_HALF_DAY_MINUTES { M_MINUTES_PER_DAY / 2 };
    int16_t difference { static_cast<int16_t>(static_cast<int16_t>(presentedMinute) - static_cast<int16_t>(ownMinute)) };

    // Nearest match across midnight: 23:59 against 00:01 is -2, not +1438.
    if (difference >= M_HALF_DAY_MINUTES) { difference = static_cast<int16_t>(difference - M_MINUTES_PER_DAY); }
    if (difference < -M_HALF_DAY_MINUTES) { difference = static_cast<int16_t>(difference + M_MINUTES_PER_DAY); }
    return difference;
  }

  void DeviceClock::RaiseFloorDay(uint16_t day)
  {
    if (!m_has_floor || day > m_floor_day) { m_floor_day = day; }
    m_has_floor = true;
  }

  uint32_t DeviceClock::NowUnix(int64_t uptimeSecs) const
  {
    return static_cast<uint32_t>(uptimeSecs + m_offset_secs);
  }

  uint16_t DeviceClock::DayIndex(int64_t uptimeSecs) const
  {
    uint16_t day { 0 };

    if (!DayIndexOf(NowUnix(uptimeSecs), day)) { day = 0; }

    // Belt and braces. Sync and trim both refuse to go below the floor, so this
    // should never bind - but the day index must never move backwards.
    if (m_has_floor && day < m_floor_day) { day = m_floor_day; }
    return day;
  }

  uint16_t DeviceClock::MinuteOfDay(int64_t uptimeSecs) const
  {
    // UTC minute since 00:00 UTC, not since the 04:00 boundary. The app sends
    // the same, so neither side needs to know the boundary to compare minutes.
    return static_cast<uint16_t>((NowUnix(uptimeSecs) % M_SECONDS_PER_DAY) / M_SECONDS_PER_MINUTE);
  }

  bool DeviceClock::IsFresh(uint16_t presentedMinute, int64_t uptimeSecs) const
  {
    if (!m_valid) { return false; }
    return std::abs(MinuteDifference(presentedMinute, MinuteOfDay(uptimeSecs))) <= M_FRESHNESS_MINUTES;
  }

  DeviceClock::SyncResult DeviceClock::ApplyProvisionerSync(uint32_t unixSeconds, int64_t uptimeSecs)
  {
    uint16_t day { 0 };

    // Only an invalid clock accepts a sync. A valid clock is trimmed by commands
    // and never jumped: a captured sync released hours later would otherwise
    // pull the clock back and extend a day key past its 04:00 expiry.
    if (m_valid) { return SyncResult::AlreadyValid; }
    if (!DayIndexOf(unixSeconds, day)) { return SyncResult::BeforeEpoch; }

    // A first-ever boot has no floor, which is correct: that is the factory case.
    if (m_has_floor && day < m_floor_day) { return SyncResult::BelowFloor; }
    if (m_has_floor && day > m_floor_day + M_MAX_FORWARD_JUMP_DAYS) { return SyncResult::TooFarAhead; }

    m_offset_secs    = static_cast<int64_t>(unixSeconds) - uptimeSecs;
    m_valid          = true;
    m_trim_day       = day;
    m_trim_used_secs = 0;
    RaiseFloorDay(day);
    return SyncResult::Applied;
  }

  DeviceClock::TrimResult DeviceClock::ApplyMinuteHint(uint16_t presentedMinute, int64_t uptimeSecs)
  {
    int16_t differenceMinutes { 0 };
    int32_t deltaSecs { 0 };
    uint32_t magnitudeSecs { 0 };
    uint16_t today { 0 };
    uint16_t trimmedDay { 0 };

    if (!m_valid) { return TrimResult::ClockInvalid; }

    differenceMinutes = MinuteDifference(presentedMinute, MinuteOfDay(uptimeSecs));
    if (std::abs(differenceMinutes) < M_TRIM_DEADBAND_MINUTES) { return TrimResult::Unchanged; }
    if (std::abs(differenceMinutes) > M_TRIM_STEP_MAX_MINUTES) { return TrimResult::Ignored; }

    deltaSecs     = static_cast<int32_t>(differenceMinutes) * M_SECONDS_PER_MINUTE;
    magnitudeSecs = static_cast<uint32_t>(std::abs(deltaSecs));

    // The budget counts MAGNITUDE, not net movement, so alternating +5 and -5
    // minute hints cannot walk the clock around indefinitely.
    today = DayIndex(uptimeSecs);
    if (today != m_trim_day) {
      m_trim_day       = today;
      m_trim_used_secs = 0;
    }
    if (m_trim_used_secs + magnitudeSecs > M_TRIM_DAILY_BUDGET_SECS) { return TrimResult::OverBudget; }

    if (!DayIndexOf(static_cast<uint32_t>(uptimeSecs + m_offset_secs + deltaSecs), trimmedDay) || (m_has_floor && trimmedDay < m_floor_day)) {
      return TrimResult::WouldRewindDay;
    }

    m_offset_secs += deltaSecs;
    m_trim_used_secs += magnitudeSecs;
    return TrimResult::Trimmed;
  }

}
```

Append `src/device_clock.cpp` to `HOST_SRCS` in `Makefile`.

- [ ] **Step 4: Run to verify it passes**

Run: `make test`
Expected: `device clock: OK` and `ALL TESTS PASSED`.

- [ ] **Step 5: Commit**

```bash
/Users/andy/nrfenv/bin/clang-format -i src/device_clock.hpp src/device_clock.cpp tests/test_device_clock.cpp tests/test_main.cpp
git add src/device_clock.hpp src/device_clock.cpp tests/ Makefile
git commit -m "Add DeviceClock, the security timebase

Invalid on every boot until an authenticated provisioner sync; there is no
resume from NVS, which closes the power-loss hole power-budget.md section 8.6 had
accepted. A valid clock refuses further syncs, so a captured sync released later
cannot pull the day back.

Syncs are bounded below by the persisted day floor and 400 days above it. Command
minute hints trim drift only in 2-5 minute steps, within a 5-minute daily budget
counted by magnitude, and never below the floor.

Every rule is host-tested with uptime passed in, including the trim that would
cross back over 04:00."
```

---

## Task 5: AccessControl

**Files:**
- Create: `src/access_control.hpp`, `src/access_control.cpp`, `tests/test_access_control.cpp`
- Modify: `Makefile`, `tests/test_main.cpp`

**Interfaces:**
- Consumes: `access::*` from Task 3, `DeviceClock` from Task 4, `protocol::DecodeCommand` from Task 2.
- Produces: `struct AccessState { uint16_t day; uint32_t next[8]; }`; `class AccessControl` with `M_WINDOW` (16), `M_LOCKOUT_THRESHOLD` (20), `enum class Verdict { Accepted, NotForUs, ClockInvalid, LockedOut, AuthFailed, Malformed, Stale, PersistFailed, CryptoError }`, `struct Evaluation { Verdict verdict; uint8_t slot; uint32_t n; protocol::Command command; }`, `using PersistFn = int (*)(const AccessState&, void*)`, constructor `AccessControl(uint32_t deviceId, const uint8_t* secret, PersistFn persist, void* context)`, `void Restore(const AccessState&)`, `const AccessState& State() const`, `bool IsLockedOut(int64_t) const`, `uint8_t ConsecutiveFailures() const`, `Evaluation Evaluate(const uint8_t* onAir, uint8_t length, DeviceClock& clock, int64_t uptimeSecs)`.

- [ ] **Step 1: Write the failing test**

Create `tests/test_access_control.cpp`:

```cpp
#include <cassert>
#include <cerrno>
#include <cstdio>
#include <cstring>

#include "access_control.hpp"
#include "access_vectors.hpp"

namespace
{
  using namespace alc;
  using Verdict = AccessControl::Verdict;

  constexpr uint32_t M_DAY_256_START { DeviceClock::M_EPOCH_UNIX + DeviceClock::M_DAY_BOUNDARY_OFFSET_SECS + 256U * DeviceClock::M_SECONDS_PER_DAY };
  // 05:00 UTC on day 256, at uptime zero.
  constexpr uint32_t M_SYNC_UNIX { M_DAY_256_START + 3600 };
  constexpr uint16_t M_MINUTE_0500 { 300 };

  struct PersistSpy
  {
      int calls { 0 };
      int failWith { 0 };
      AccessState last {};
  };

  int persistSpy(const AccessState& state, void* context)
  {
    PersistSpy* spy { static_cast<PersistSpy*>(context) };
    spy->calls++;
    if (spy->failWith != 0) { return spy->failWith; }
    spy->last = state;
    return 0;
  }

  // Builds what the engineer's app would send.
  void buildCommand(uint16_t day, uint8_t slot, uint32_t n, bool arm, uint16_t minute, uint8_t* onAir)
  {
    uint8_t dayKey[access::M_DAY_KEY_BYTES] {};
    uint8_t plaintext[protocol::M_PLAINTEXT_BYTES] {};
    protocol::Command command;

    command.armActive   = arm;
    command.activations = 3;
    command.minuteOfDay = minute;
    protocol::EncodeCommand(command, plaintext);
    assert(access::DeriveDayKey(access::vectors::M_SECRET, access::vectors::M_DEVICE_ID, day, slot, dayKey) == 0);
    assert(access::SealCommand(dayKey, access::vectors::M_DEVICE_ID, day, slot, n, plaintext, onAir) == 0);
  }

  DeviceClock syncedClock()
  {
    DeviceClock clock;
    assert(clock.ApplyProvisionerSync(M_SYNC_UNIX, 0) == DeviceClock::SyncResult::Applied);
    return clock;
  }
}

void run_access_control_tests()
{
  uint8_t onAir[protocol::M_UUID_BYTES] {};

  // No clock, no access - even for an authentic command.
  {
    PersistSpy spy;
    AccessControl access(access::vectors::M_DEVICE_ID, access::vectors::M_SECRET, &persistSpy, &spy);
    DeviceClock clock;
    buildCommand(256, 1, 0, true, M_MINUTE_0500, onAir);
    assert(access.Evaluate(onAir, sizeof(onAir), clock, 0).verdict == Verdict::ClockInvalid);
    assert(spy.calls == 0);
  }

  // Happy path, sequence handling and replay.
  {
    PersistSpy spy;
    AccessControl access(access::vectors::M_DEVICE_ID, access::vectors::M_SECRET, &persistSpy, &spy);
    DeviceClock clock { syncedClock() };

    buildCommand(256, 1, 0, true, M_MINUTE_0500, onAir);
    AccessControl::Evaluation evaluation { access.Evaluate(onAir, sizeof(onAir), clock, 0) };
    assert(evaluation.verdict == Verdict::Accepted);
    assert(evaluation.slot == 1 && evaluation.n == 0);
    assert(evaluation.command.armActive && evaluation.command.activations == 3);

    // Persisted BEFORE returning: the rollover to day 256, then next[1] = 1.
    assert(spy.calls == 2);
    assert(spy.last.day == 256 && spy.last.next[1] == 1);

    // The same bytes again - the phone advertises each command ~160 times. The
    // spent ID is no longer expected, so it is NotForUs: silent AND not counted.
    assert(access.Evaluate(onAir, sizeof(onAir), clock, 0).verdict == Verdict::NotForUs);
    assert(access.ConsecutiveFailures() == 0);

    // Desync: the phone sent 1..9 and none arrived. n = 10 is inside the window.
    buildCommand(256, 1, 10, false, M_MINUTE_0500, onAir);
    evaluation = access.Evaluate(onAir, sizeof(onAir), clock, 0);
    assert(evaluation.verdict == Verdict::Accepted && evaluation.n == 10);
    assert(access.State().next[1] == 11);

    // The skipped numbers are dead: n = 5 is now behind the window.
    buildCommand(256, 1, 5, false, M_MINUTE_0500, onAir);
    assert(access.Evaluate(onAir, sizeof(onAir), clock, 0).verdict == Verdict::NotForUs);

    // Window edge: next = 11 accepts 26 (11 + 15) but not 27.
    buildCommand(256, 1, 27, false, M_MINUTE_0500, onAir);
    assert(access.Evaluate(onAir, sizeof(onAir), clock, 0).verdict == Verdict::NotForUs);
    buildCommand(256, 1, 26, false, M_MINUTE_0500, onAir);
    assert(access.Evaluate(onAir, sizeof(onAir), clock, 0).verdict == Verdict::Accepted);

    // Slots are independent: slot 2 still starts at zero.
    buildCommand(256, 2, 0, false, M_MINUTE_0500, onAir);
    evaluation = access.Evaluate(onAir, sizeof(onAir), clock, 0);
    assert(evaluation.verdict == Verdict::Accepted && evaluation.slot == 2);
  }

  // Yesterday's key is dead, and a new day resets every slot.
  {
    PersistSpy spy;
    AccessControl access(access::vectors::M_DEVICE_ID, access::vectors::M_SECRET, &persistSpy, &spy);
    DeviceClock clock { syncedClock() };
    AccessState restored {};
    restored.day     = 256;
    restored.next[1] = 7;
    access.Restore(restored);

    buildCommand(255, 1, 7, true, M_MINUTE_0500, onAir);
    assert(access.Evaluate(onAir, sizeof(onAir), clock, 0).verdict == Verdict::NotForUs);

    // Restored state is honoured: n = 6 on day 256 is spent.
    buildCommand(256, 1, 6, true, M_MINUTE_0500, onAir);
    assert(access.Evaluate(onAir, sizeof(onAir), clock, 0).verdict == Verdict::NotForUs);

    // Tomorrow at 05:00: slot 1 is back at zero under the new key.
    constexpr int64_t M_TOMORROW { DeviceClock::M_SECONDS_PER_DAY };
    buildCommand(257, 1, 0, true, M_MINUTE_0500, onAir);
    assert(access.Evaluate(onAir, sizeof(onAir), clock, M_TOMORROW).verdict == Verdict::Accepted);
    assert(access.State().day == 257 && access.State().next[1] == 1);
    assert(clock.FloorDay() == 257);
  }

  // Freshness: authentic but held for 11 minutes is rejected AND NOT CONSUMED.
  {
    PersistSpy spy;
    AccessControl access(access::vectors::M_DEVICE_ID, access::vectors::M_SECRET, &persistSpy, &spy);
    DeviceClock clock { syncedClock() };
    buildCommand(256, 3, 0, true, M_MINUTE_0500 - 11, onAir);
    assert(access.Evaluate(onAir, sizeof(onAir), clock, 0).verdict == Verdict::Stale);
    assert(access.State().next[3] == 0);
    assert(access.ConsecutiveFailures() == 0);
  }

  // Persist before acting: a failed save is NOT an acceptance, and does not advance.
  {
    PersistSpy spy;
    AccessControl access(access::vectors::M_DEVICE_ID, access::vectors::M_SECRET, &persistSpy, &spy);
    DeviceClock clock { syncedClock() };
    buildCommand(256, 1, 0, true, M_MINUTE_0500, onAir);
    assert(access.Evaluate(onAir, sizeof(onAir), clock, 0).verdict == Verdict::Accepted); // rolls the day in
    spy.failWith = -EIO;
    buildCommand(256, 1, 1, true, M_MINUTE_0500, onAir);
    assert(access.Evaluate(onAir, sizeof(onAir), clock, 0).verdict == Verdict::PersistFailed);
    assert(access.State().next[1] == 1);
    spy.failWith = 0;
    assert(access.Evaluate(onAir, sizeof(onAir), clock, 0).verdict == Verdict::Accepted);
  }

  // Garbage never counts. Only an expected ID with a bad tag does.
  {
    PersistSpy spy;
    AccessControl access(access::vectors::M_DEVICE_ID, access::vectors::M_SECRET, &persistSpy, &spy);
    DeviceClock clock { syncedClock() };
    uint8_t garbage[protocol::M_UUID_BYTES] {};

    for (int attempt = 0; attempt < 100; attempt++) {
      garbage[0] = static_cast<uint8_t>(attempt);
      assert(access.Evaluate(garbage, sizeof(garbage), clock, 0).verdict == Verdict::NotForUs);
    }
    assert(access.ConsecutiveFailures() == 0);

    // A correct ID with a corrupted tag: counted. Twenty trigger a lockout.
    buildCommand(256, 1, 0, true, M_MINUTE_0500, onAir);
    onAir[protocol::M_OFFSET_TAG] ^= 0x01;
    for (uint8_t attempt = 0; attempt < AccessControl::M_LOCKOUT_THRESHOLD; attempt++) {
      assert(access.Evaluate(onAir, sizeof(onAir), clock, 0).verdict == Verdict::AuthFailed);
    }
    assert(access.IsLockedOut(0));

    // During the lockout even an authentic command is not decrypted.
    buildCommand(256, 1, 0, true, M_MINUTE_0500, onAir);
    assert(access.Evaluate(onAir, sizeof(onAir), clock, 599).verdict == Verdict::LockedOut);

    // After ten minutes it is. Uptime 600 is 05:10, and the command says 05:10.
    buildCommand(256, 1, 0, true, M_MINUTE_0500 + 10, onAir);
    assert(access.Evaluate(onAir, sizeof(onAir), clock, 600).verdict == Verdict::Accepted);
    assert(!access.IsLockedOut(600));
  }

  // The lockout doubles and caps at four hours.
  {
    PersistSpy spy;
    AccessControl access(access::vectors::M_DEVICE_ID, access::vectors::M_SECRET, &persistSpy, &spy);
    DeviceClock clock { syncedClock() };
    int64_t now { 0 };
    const uint32_t expectedSecs[] { 600, 1200, 2400, 4800, 9600, 14400, 14400 };

    buildCommand(256, 1, 0, true, M_MINUTE_0500, onAir);
    onAir[protocol::M_OFFSET_TAG] ^= 0x01;
    for (uint32_t lockoutSecs : expectedSecs) {
      for (uint8_t attempt = 0; attempt < AccessControl::M_LOCKOUT_THRESHOLD; attempt++) {
        access.Evaluate(onAir, sizeof(onAir), clock, now);
      }
      assert(access.IsLockedOut(now + lockoutSecs - 1));
      assert(!access.IsLockedOut(now + lockoutSecs));
      now += lockoutSecs;
    }
  }

  printf("access control: OK\n");
}
```

Declare and call `run_access_control_tests();` in `tests/test_main.cpp`.

- [ ] **Step 2: Run to verify it fails**

Run: `make test`
Expected: FAIL — `'access_control.hpp' file not found`.

- [ ] **Step 3: Implement**

Create `src/access_control.hpp`:

```cpp
#pragma once

#include <cstdint>

#include "access_keys.hpp"
#include "device_clock.hpp"
#include "mfs_protocol.hpp"

namespace alc
{

  /** @brief The persisted access state. 36 bytes. Written only on change. */
  struct AccessState
  {
      uint16_t day { 0 };
      uint32_t next[access::M_SLOT_COUNT] {};
  };

  /**
   * @brief Decides whether 16 on-air bytes are an authentic, fresh command.
   *
   * Pure logic apart from the persistence hook, so the whole rule set is
   * host-tested. See docs/tan-scheme.md section 6.
   *
   * **Silence is the default.** Every verdict other than Accepted results in no
   * radio emission and no LED - the caller logs over RTT and does nothing else.
   */
  class AccessControl
  {
    public:
      // Look-ahead per slot. The phone never learns whether a command landed, so
      // the device accepts any of the next 16 sequence numbers and skips past it.
      static constexpr uint8_t M_WINDOW { 16 };

      // Consecutive authentication failures that trigger a lockout.
      static constexpr uint8_t M_LOCKOUT_THRESHOLD { 20 };
      static constexpr uint32_t M_LOCKOUT_INITIAL_SECS { 600 };
      static constexpr uint32_t M_LOCKOUT_MAX_SECS { 14400 };

      enum class Verdict : uint8_t {
        Accepted,
        NotForUs,      ///< No expected rotating ID matched. The normal case for any other advert. Not counted.
        ClockInvalid,  ///< No trustworthy time yet - only a provisioner sync is listened for.
        LockedOut,     ///< An ID matched during a lockout. Not decrypted, not counted.
        AuthFailed,    ///< An ID matched but no candidate authenticated. Counted.
        Malformed,     ///< Authentic, but the plaintext is invalid. Not consumed.
        Stale,         ///< Authentic, but the minute is outside the freshness window. Not consumed.
        PersistFailed, ///< Authentic and fresh, but the sequence number could not be saved. NOT acted on.
        CryptoError,   ///< Backend fault. Not counted.
      };

      struct Evaluation
      {
          Verdict verdict { Verdict::NotForUs };
          uint8_t slot { 0 };
          uint32_t n { 0 };
          protocol::Command command {};
      };

      /** @brief Persistence hook. Must return only once the state is durable. */
      using PersistFn = int (*)(const AccessState& state, void* context);

      /**
       * @param deviceId Public 32-bit identity used in every derivation.
       * @param secret   M_SECRET_BYTES. Must outlive this object.
       * @param persist  Called before any command is acted on.
       */
      AccessControl(uint32_t deviceId, const uint8_t* secret, PersistFn persist, void* context);

      /** @brief Adopt a state loaded from NVS. Call before the first Evaluate(). */
      void Restore(const AccessState& state);

      const AccessState& State() const { return m_state; }
      bool IsLockedOut(int64_t uptimeSecs) const { return m_locked && uptimeSecs < m_lockout_until_secs; }
      uint8_t ConsecutiveFailures() const { return m_failures; }

      /**
       * @brief Evaluate one 128-bit service UUID's bytes.
       *
       * On Accepted the slot's sequence number has ALREADY been persisted past n,
       * so the command cannot be replayed however the caller then fails.
       */
      Evaluation Evaluate(const uint8_t* onAir, uint8_t length, DeviceClock& clock, int64_t uptimeSecs);

    private:
      int prepareDay(uint16_t today, DeviceClock& clock);
      int rebuildSlot(uint8_t slot);
      void recordFailure(int64_t uptimeSecs);

      uint32_t m_device_id;
      const uint8_t* m_secret;
      PersistFn m_persist;
      void* m_persist_context;

      AccessState m_state;
      bool m_tables_ready;
      uint8_t m_day_keys[access::M_SLOT_COUNT][access::M_DAY_KEY_BYTES];
      uint8_t m_expected_ids[access::M_SLOT_COUNT][M_WINDOW][protocol::M_ROTATING_ID_BYTES];
      uint8_t m_window_size[access::M_SLOT_COUNT];

      uint8_t m_failures;
      bool m_locked;
      int64_t m_lockout_until_secs;
      uint32_t m_lockout_secs;
  };

}
```

Create `src/access_control.cpp`:

```cpp
#include <cerrno>
#include <cstring>

#include "access_control.hpp"

namespace alc
{

  namespace
  {
    // The largest n whose full window cannot overflow. Unreachable in practice -
    // four billion commands in a day - but an overflow would wrap the window
    // back onto spent sequence numbers, which is a replay.
    constexpr uint32_t M_SEQUENCE_LIMIT { UINT32_MAX - AccessControl::M_WINDOW };

    struct Candidate
    {
        uint8_t slot;
        uint32_t n;
    };

    // A rotating-ID collision needs several candidates, but 128 in 2^32 per
    // advert means more than a handful is never going to happen.
    constexpr uint8_t M_MAX_CANDIDATES { 4 };
  }

  AccessControl::AccessControl(uint32_t deviceId, const uint8_t* secret, PersistFn persist, void* context)
      : m_device_id(deviceId)
      , m_secret(secret)
      , m_persist(persist)
      , m_persist_context(context)
      , m_state {}
      , m_tables_ready(false)
      , m_day_keys {}
      , m_expected_ids {}
      , m_window_size {}
      , m_failures(0)
      , m_locked(false)
      , m_lockout_until_secs(0)
      , m_lockout_secs(M_LOCKOUT_INITIAL_SECS)
  {}

  void AccessControl::Restore(const AccessState& state)
  {
    m_state        = state;
    m_tables_ready = false;
  }

  int AccessControl::rebuildSlot(uint8_t slot)
  {
    uint32_t base { m_state.next[slot] };
    uint8_t size { base > M_SEQUENCE_LIMIT ? static_cast<uint8_t>(0) : M_WINDOW };
    int result { 0 };

    for (uint8_t offset = 0; offset < size; offset++) {
      result = access::DeriveRotatingId(m_day_keys[slot], base + offset, m_expected_ids[slot][offset]);
      if (result < 0) { return result; }
    }
    m_window_size[slot] = size;
    return 0;
  }

  int AccessControl::prepareDay(uint16_t today, DeviceClock& clock)
  {
    AccessState previous { m_state };
    int result { 0 };

    if (m_tables_ready && today == m_state.day) { return 0; }

    // A new day: every slot starts again at zero, because the day keys are new.
    // Nothing about previous days is kept - their keys can no longer be derived
    // by the device, so their sequence numbers are dead weight.
    if (today > m_state.day) {
      m_state.day = today;
      memset(m_state.next, 0, sizeof(m_state.next));

      // Persist the rollover BEFORE building tables. If it cannot be saved the
      // day is not adopted, so no command can be accepted against unsaved state.
      result = m_persist(m_state, m_persist_context);
      if (result < 0) {
        m_state = previous;
        return result;
      }
    }

    // The floor follows the persisted day, never the other way round.
    clock.RaiseFloorDay(m_state.day);

    for (uint8_t slot = 0; slot < access::M_SLOT_COUNT; slot++) {
      result = access::DeriveDayKey(m_secret, m_device_id, m_state.day, slot, m_day_keys[slot]);
      if (result < 0) { return result; }
      result = rebuildSlot(slot);
      if (result < 0) { return result; }
    }

    m_tables_ready = true;
    return 0;
  }

  void AccessControl::recordFailure(int64_t uptimeSecs)
  {
    m_failures++;
    if (m_failures < M_LOCKOUT_THRESHOLD) { return; }

    // Doubling lockout, capped. Uptime rather than UTC, so a clock trim can
    // neither shorten nor extend it.
    m_locked             = true;
    m_lockout_until_secs = uptimeSecs + m_lockout_secs;
    m_lockout_secs       = m_lockout_secs * 2 > M_LOCKOUT_MAX_SECS ? M_LOCKOUT_MAX_SECS : m_lockout_secs * 2;
    m_failures           = 0;
  }

  AccessControl::Evaluation AccessControl::Evaluate(const uint8_t* onAir, uint8_t length, DeviceClock& clock, int64_t uptimeSecs)
  {
    Evaluation evaluation {};
    Candidate candidates[M_MAX_CANDIDATES] {};
    uint8_t candidateCount { 0 };
    uint8_t plaintext[protocol::M_PLAINTEXT_BYTES] {};
    uint16_t today { 0 };
    uint32_t previousNext { 0 };
    int result { 0 };
    bool authentic { false };

    if (onAir == nullptr || length < protocol::M_UUID_BYTES) { return evaluation; }

    // No trustworthy time, no day, no keys. See DeviceClock.
    if (!clock.IsValid()) {
      evaluation.verdict = Verdict::ClockInvalid;
      return evaluation;
    }

    today = clock.DayIndex(uptimeSecs);
    if (today < m_state.day) {
      // Cannot happen with a correct floor. Refuse rather than accept against a
      // day the device has already left.
      evaluation.verdict = Verdict::ClockInvalid;
      return evaluation;
    }

    result = prepareDay(today, clock);
    if (result < 0) {
      evaluation.verdict = Verdict::PersistFailed;
      return evaluation;
    }

    // Cheap filter first. Only a matching rotating ID costs a decryption, and
    // only a matching ID can count towards a lockout - an attacker without the
    // day key cannot predict one, so garbage cannot lock the engineer out.
    for (uint8_t slot = 0; slot < access::M_SLOT_COUNT; slot++) {
      for (uint8_t offset = 0; offset < m_window_size[slot]; offset++) {
        if (memcmp(m_expected_ids[slot][offset], &onAir[protocol::M_OFFSET_ROTATING_ID], protocol::M_ROTATING_ID_BYTES) != 0) { continue; }
        if (candidateCount < M_MAX_CANDIDATES) { candidates[candidateCount++] = Candidate { slot, m_state.next[slot] + offset }; }
      }
    }

    if (candidateCount == 0) { return evaluation; }

    if (m_locked && uptimeSecs < m_lockout_until_secs) {
      evaluation.verdict = Verdict::LockedOut;
      return evaluation;
    }
    m_locked = false;

    for (uint8_t index = 0; index < candidateCount && !authentic; index++) {
      result = access::OpenCommand(m_day_keys[candidates[index].slot], m_device_id, m_state.day, candidates[index].slot, candidates[index].n, onAir,
                                   plaintext);
      if (result == 0) {
        authentic       = true;
        evaluation.slot = candidates[index].slot;
        evaluation.n    = candidates[index].n;
      } else if (result != -EBADMSG) {
        evaluation.verdict = Verdict::CryptoError;
        return evaluation;
      }
    }

    if (!authentic) {
      recordFailure(uptimeSecs);
      evaluation.verdict = Verdict::AuthFailed;
      return evaluation;
    }

    // Any authentic command clears the failure history.
    m_failures     = 0;
    m_lockout_secs = M_LOCKOUT_INITIAL_SECS;

    if (!protocol::DecodeCommand(plaintext, evaluation.command)) {
      evaluation.verdict = Verdict::Malformed;
      return evaluation;
    }

    // Freshness before consumption. A command captured, jammed and released
    // later fails here and stays unconsumed - which is harmless, because its
    // minute will never be fresh again and its key dies at 04:00.
    if (!clock.IsFresh(evaluation.command.minuteOfDay, uptimeSecs)) {
      evaluation.verdict = Verdict::Stale;
      return evaluation;
    }

    // PERSIST BEFORE ACTING. Reversed, a power loss between acting and saving
    // would leave the command replayable.
    previousNext                  = m_state.next[evaluation.slot];
    m_state.next[evaluation.slot] = evaluation.n + 1;
    result                        = m_persist(m_state, m_persist_context);
    if (result < 0) {
      m_state.next[evaluation.slot] = previousNext;
      evaluation.verdict            = Verdict::PersistFailed;
      return evaluation;
    }

    // A failed rebuild leaves stale IDs in the table, so force a full rebuild
    // on the next evaluation rather than trust it.
    if (rebuildSlot(evaluation.slot) < 0) { m_tables_ready = false; }
    evaluation.verdict = Verdict::Accepted;
    return evaluation;
  }

}
```

Append `src/access_control.cpp` to `HOST_SRCS` in `Makefile`.

- [ ] **Step 4: Run to verify it passes**

Run: `make test`
Expected: `access control: OK` and `ALL TESTS PASSED`.

- [ ] **Step 5: Commit**

```bash
/Users/andy/nrfenv/bin/clang-format -i src/access_control.hpp src/access_control.cpp tests/test_access_control.cpp tests/test_main.cpp
git add src/access_control.hpp src/access_control.cpp tests/ Makefile
git commit -m "Add AccessControl: slots, window, lockout, freshness

Matches bytes 0-3 against the 128 rotating IDs expected across eight slots,
trial-decrypts the candidates, checks the plaintext and its freshness, and only
then persists next[slot] - before the caller can act.

Only an expected ID followed by a bad tag counts towards the lockout, so an
attacker without the day key cannot lock an engineer out. Repeats of an accepted
command fall outside the window and are silently not-for-us. Stale and malformed
commands are rejected without being consumed.

Host tests cover replay, desync inside the window, both window edges, slot
independence, yesterday's key, day rollover, freshness, persist failure, and the
doubling lockout to its four-hour cap."
```

---

## Task 6: LedSequencer

**Files:**
- Create: `src/led_sequencer.hpp`, `src/led_sequencer.cpp`, `tests/test_led_sequencer.cpp`
- Modify: `Makefile`, `tests/test_main.cpp`

**Interfaces:**
- Consumes: nothing.
- Produces: `enum class LedPattern { None, Armed, Disarmed, DisarmedDelayCancelled, ArmRefused, SettingsApplied, ModeChanged }`; `class LedSequencer` with `void Start(LedPattern, int64_t nowMs)`, `void Stop()`, `LedPattern Current() const`, `bool IsActive(int64_t) const`, `bool Level(int64_t) const`.

- [ ] **Step 1: Write the failing test**

Create `tests/test_led_sequencer.cpp`:

```cpp
#include <cassert>
#include <cstdio>

#include "led_sequencer.hpp"

void run_led_sequencer_tests()
{
  using alc::LedPattern;
  using alc::LedSequencer;

  constexpr int64_t M_START { 1000 };
  LedSequencer leds;

  assert(!leds.IsActive(M_START));
  assert(!leds.Level(M_START));

  // Armed: 60 ms on, 65 ms off, for 3 s.
  leds.Start(LedPattern::Armed, M_START);
  assert(leds.Level(M_START));
  assert(leds.Level(M_START + 59));
  assert(!leds.Level(M_START + 60));
  assert(!leds.Level(M_START + 124));
  assert(leds.Level(M_START + 125));
  assert(leds.IsActive(M_START + 2999));
  assert(!leds.IsActive(M_START + 3000));
  assert(!leds.Level(M_START + 3000));

  // Double blink each second: on 0-99, off 100-199, on 200-299, off 300-999.
  leds.Start(LedPattern::DisarmedDelayCancelled, M_START);
  assert(leds.Level(M_START + 50));
  assert(!leds.Level(M_START + 150));
  assert(leds.Level(M_START + 250));
  assert(!leds.Level(M_START + 500));
  assert(leds.Level(M_START + 1050));

  // Mode changed: two 200 ms blinks, then done at 600 ms.
  leds.Start(LedPattern::ModeChanged, M_START);
  assert(leds.Level(M_START + 100));
  assert(!leds.Level(M_START + 300));
  assert(leds.Level(M_START + 500));
  assert(!leds.IsActive(M_START + 600));

  // A new command replaces the pattern playing.
  leds.Start(LedPattern::Disarmed, M_START);
  leds.Start(LedPattern::SettingsApplied, M_START + 100);
  assert(leds.Current() == LedPattern::SettingsApplied);
  assert(leds.Level(M_START + 250));
  assert(!leds.IsActive(M_START + 300));

  printf("led sequencer: OK\n");
}
```

Declare and call `run_led_sequencer_tests();` in `tests/test_main.cpp`.

- [ ] **Step 2: Run to verify it fails**

Run: `make test`
Expected: FAIL — `'led_sequencer.hpp' file not found`.

- [ ] **Step 3: Implement**

Create `src/led_sequencer.hpp`:

```cpp
#pragma once

#include <cstdint>

namespace alc
{

  /**
   * @brief The acknowledgement patterns LED A plays. PROVISIONAL scheme.
   *
   * A pattern plays only after an AUTHENTICATED command has been accepted and
   * only once the new state is real. A failed authentication never shows
   * anything, or the LED would tell an attacker which guesses got through.
   * See the design spec section 6.7.
   */
  enum class LedPattern : uint8_t {
    None,
    Armed,                  ///< Rapid flash ~8 Hz, 3 s.
    Disarmed,               ///< Slow flash 1 Hz, 3 s.
    DisarmedDelayCancelled, ///< Double blink each second, 3 s - a pending trigger really was cancelled.
    ArmRefused,             ///< Three long pulses, 3 s - the accelerometer would not configure.
    SettingsApplied,        ///< One 200 ms blink.
    ModeChanged,            ///< Two 200 ms blinks - slot-0 command changed the operating mode.
  };

  /**
   * @brief Time-based pattern player. Pure: the caller supplies the time.
   *
   * Queried from a 10 ms timer while a pattern is active, so the 60 ms phases of
   * the Armed pattern are rendered faithfully despite the 100 ms main loop.
   */
  class LedSequencer
  {
    public:
      LedSequencer();

      /** @brief Start a pattern, replacing any pattern already playing. */
      void Start(LedPattern pattern, int64_t nowMs);

      void Stop() { m_pattern = LedPattern::None; }

      LedPattern Current() const { return m_pattern; }

      /** @brief True until the pattern's duration has elapsed. */
      bool IsActive(int64_t nowMs) const;

      /** @brief The LED level the pattern calls for at this instant. False once inactive. */
      bool Level(int64_t nowMs) const;

    private:
      LedPattern m_pattern;
      int64_t m_start_ms;
  };

}
```

Create `src/led_sequencer.cpp`:

```cpp
#include "led_sequencer.hpp"

namespace alc
{

  namespace
  {
    struct Step
    {
        uint16_t onMs;
        uint16_t offMs;
    };

    struct Definition
    {
        const Step* steps;
        uint8_t stepCount;
        uint16_t durationMs;
    };

    constexpr uint16_t M_LONG_PATTERN_MS { 3000 };

    constexpr Step M_ARMED[] { { 60, 65 } };
    constexpr Step M_DISARMED[] { { 500, 500 } };
    constexpr Step M_DELAY_CANCELLED[] { { 100, 100 }, { 100, 700 } };
    constexpr Step M_ARM_REFUSED[] { { 700, 300 } };
    constexpr Step M_SINGLE_BLINK[] { { 200, 0 } };
    constexpr Step M_DOUBLE_BLINK[] { { 200, 200 }, { 200, 0 } };

    constexpr uint16_t M_SINGLE_BLINK_MS { 200 };
    constexpr uint16_t M_DOUBLE_BLINK_MS { 600 };

    Definition definitionOf(LedPattern pattern)
    {
      switch (pattern) {
        case LedPattern::Armed:
          return { M_ARMED, 1, M_LONG_PATTERN_MS };
        case LedPattern::Disarmed:
          return { M_DISARMED, 1, M_LONG_PATTERN_MS };
        case LedPattern::DisarmedDelayCancelled:
          return { M_DELAY_CANCELLED, 2, M_LONG_PATTERN_MS };
        case LedPattern::ArmRefused:
          return { M_ARM_REFUSED, 1, M_LONG_PATTERN_MS };
        case LedPattern::SettingsApplied:
          return { M_SINGLE_BLINK, 1, M_SINGLE_BLINK_MS };
        case LedPattern::ModeChanged:
          return { M_DOUBLE_BLINK, 2, M_DOUBLE_BLINK_MS };
        default:
          return { nullptr, 0, 0 };
      }
    }
  }

  LedSequencer::LedSequencer()
      : m_pattern(LedPattern::None)
      , m_start_ms(0)
  {}

  void LedSequencer::Start(LedPattern pattern, int64_t nowMs)
  {
    m_pattern  = pattern;
    m_start_ms = nowMs;
  }

  bool LedSequencer::IsActive(int64_t nowMs) const
  {
    Definition definition { definitionOf(m_pattern) };
    int64_t elapsed { nowMs - m_start_ms };

    return definition.stepCount > 0 && elapsed >= 0 && elapsed < definition.durationMs;
  }

  bool LedSequencer::Level(int64_t nowMs) const
  {
    Definition definition { definitionOf(m_pattern) };
    uint32_t cycleMs { 0 };
    uint32_t position { 0 };

    if (!IsActive(nowMs)) { return false; }

    for (uint8_t index = 0; index < definition.stepCount; index++) {
      cycleMs += definition.steps[index].onMs + definition.steps[index].offMs;
    }

    position = static_cast<uint32_t>(nowMs - m_start_ms) % cycleMs;
    for (uint8_t index = 0; index < definition.stepCount; index++) {
      if (position < definition.steps[index].onMs) { return true; }
      position -= definition.steps[index].onMs;
      if (position < definition.steps[index].offMs) { return false; }
      position -= definition.steps[index].offMs;
    }
    return false;
  }

}
```

Append `src/led_sequencer.cpp` to `HOST_SRCS` in `Makefile`.

- [ ] **Step 4: Run to verify it passes**

Run: `make test`
Expected: `led sequencer: OK` and `ALL TESTS PASSED`.

- [ ] **Step 5: Commit**

```bash
/Users/andy/nrfenv/bin/clang-format -i src/led_sequencer.hpp src/led_sequencer.cpp tests/test_led_sequencer.cpp tests/test_main.cpp
git add src/led_sequencer.hpp src/led_sequencer.cpp tests/ Makefile
git commit -m "Add the LED A acknowledgement patterns

The provisional scheme from the design spec section 6.7: rapid flash armed, slow
flash disarmed, double blink when a pending trigger was cancelled, three long
pulses when arming is refused, and one or two short blinks for settings and mode.

Pure and time-based, so the patterns are host-tested to the millisecond and the
firmware only has to render Level() from a timer."
```

---

## Task 7: Settings

**Files:**
- Create: `src/settings.hpp`, `src/settings.cpp`, `tests/test_settings.cpp`
- Modify: `Makefile`, `tests/test_main.cpp`

**Interfaces:**
- Consumes: `protocol::Command`, the table conversions.
- Produces: `class Settings` with `int Load()`, `bool ApplyFrom(const protocol::Command&, bool allowModeChange)`, `uint8_t Activations() const`, `uint16_t ThresholdLsb() const`, `uint16_t CooldownSeconds() const`, `uint8_t DelayCode() const`, `uint16_t DelaySeconds() const`, `protocol::Mode OperatingMode() const`. On target: NVS key `params/v1`. `Load()` requires `settings_subsys_init()` to have run.

- [ ] **Step 1: Write the failing test**

Create `tests/test_settings.cpp`:

```cpp
#include <cassert>
#include <cstdio>

#include "settings.hpp"

void run_settings_tests()
{
  using namespace alc;

  Settings settings;
  protocol::Command command;

  // Defaults match the firmware's historical Kconfig values, so a device that
  // has never been configured behaves exactly as it did before this feature.
  assert(settings.Activations() == 1);
  assert(settings.ThresholdLsb() == 302);
  assert(settings.CooldownSeconds() == 0);
  assert(settings.DelaySeconds() == 0);
  assert(settings.OperatingMode() == protocol::Mode::TriggerOnly);

  command.activations     = 3;
  command.cooldownByte    = 128;
  command.sensitivityByte = 255;
  command.delayCode       = 119;
  command.mode            = protocol::Mode::ReportOnly;

  // An ENGINEER's command (slot 1-7) applies everything EXCEPT the mode.
  assert(settings.ApplyFrom(command, false));
  assert(settings.Activations() == 3);
  assert(settings.CooldownSeconds() == 60);
  assert(settings.ThresholdLsb() == 40);
  assert(settings.DelaySeconds() == 3600);
  assert(settings.OperatingMode() == protocol::Mode::TriggerOnly);

  // The same command again from an engineer changes nothing: the differing mode
  // is ignored, not counted as a change, so NVS is not rewritten.
  assert(!settings.ApplyFrom(command, false));

  // The Network Manager's slot-0 command does change the mode.
  assert(settings.ApplyFrom(command, true));
  assert(settings.OperatingMode() == protocol::Mode::ReportOnly);

  // A change to the delay ALONE must still be detected.
  command.delayCode = 127;
  assert(settings.ApplyFrom(command, false));
  assert(settings.DelaySeconds() == 32400);
  assert(!settings.ApplyFrom(command, true));

  printf("settings: OK\n");
}
```

Declare and call `run_settings_tests();` in `tests/test_main.cpp`.

- [ ] **Step 2: Run to verify it fails**

Run: `make test`
Expected: FAIL — `'settings.hpp' file not found`.

- [ ] **Step 3: Implement**

Create `src/settings.hpp`:

```cpp
#pragma once

#include <cstdint>

#include "mfs_protocol.hpp"

namespace alc
{

  /**
   * @brief The engineer-settable parameters, NVS-backed.
   *
   * Stored as the raw wire bytes rather than the converted values, so the table
   * lookup stays the single place a byte becomes a physical quantity.
   */
  class Settings
  {
    public:
      Settings();

      /** @brief Load from NVS, leaving defaults in place if nothing is stored. */
      int Load();

      /**
       * @brief Adopt the parameters from an accepted command and persist if they changed.
       *
       * @param allowModeChange True only for a slot-0 (Network Manager) command.
       *        Otherwise the command's mode field is IGNORED and the stored mode
       *        kept - an engineer's key never confers the power to set it.
       * @return True if anything changed. **Only writes NVS on change.**
       */
      bool ApplyFrom(const protocol::Command& command, bool allowModeChange);

      uint8_t Activations() const { return m_activations; }
      uint16_t ThresholdLsb() const { return protocol::SensitivityToThresholdLsb(m_sensitivity_byte); }
      uint16_t CooldownSeconds() const { return protocol::CooldownToSeconds(m_cooldown_byte); }
      uint8_t DelayCode() const { return m_delay_code; }
      uint16_t DelaySeconds() const { return protocol::DelayToSeconds(m_delay_code); }
      protocol::Mode OperatingMode() const { return m_mode; }

    private:
      int save();

      uint8_t m_activations;
      uint8_t m_cooldown_byte;
      uint8_t m_sensitivity_byte;
      uint8_t m_delay_code;
      protocol::Mode m_mode;
  };

}
```

The Zephyr half registers a static settings handler; **the old plan's version called `settings_load_subtree()` with no handler registered, so nothing would ever have loaded.**

Create `src/settings.cpp`:

```cpp
#include "settings.hpp"

#if defined(__ZEPHYR__)
#include <cstring>

#include <zephyr/logging/log.h>
#include <zephyr/settings/settings.h>

LOG_MODULE_REGISTER(settings_store, LOG_LEVEL_INF);
#endif

namespace alc
{

  namespace
  {
    // Byte 143 is ~75 mg, the threshold the firmware used before this feature.
    // A device that has never been configured therefore behaves as it always did.
    constexpr uint8_t M_DEFAULT_ACTIVATIONS { 1 };
    constexpr uint8_t M_DEFAULT_COOLDOWN_BYTE { 0 };
    constexpr uint8_t M_DEFAULT_SENSITIVITY_BYTE { 143 };

    // No delay and Trigger-only: the behaviour the device had before either
    // setting existed. A device that has never been configured must not
    // suddenly acquire a delay or start advertising.
    constexpr uint8_t M_DEFAULT_DELAY_CODE { 0 };
    constexpr protocol::Mode M_DEFAULT_MODE { protocol::Mode::TriggerOnly };
  }

  Settings::Settings()
      : m_activations(M_DEFAULT_ACTIVATIONS)
      , m_cooldown_byte(M_DEFAULT_COOLDOWN_BYTE)
      , m_sensitivity_byte(M_DEFAULT_SENSITIVITY_BYTE)
      , m_delay_code(M_DEFAULT_DELAY_CODE)
      , m_mode(M_DEFAULT_MODE)
  {}

  bool Settings::ApplyFrom(const protocol::Command& command, bool allowModeChange)
  {
    protocol::Mode mode { allowModeChange ? command.mode : m_mode };

    if (command.activations == m_activations && command.cooldownByte == m_cooldown_byte && command.sensitivityByte == m_sensitivity_byte &&
        command.delayCode == m_delay_code && mode == m_mode) {
      return false;
    }

    m_activations      = command.activations;
    m_cooldown_byte    = command.cooldownByte;
    m_sensitivity_byte = command.sensitivityByte;
    m_delay_code       = command.delayCode;
    m_mode             = mode;
    save();
    return true;
  }

#if defined(__ZEPHYR__)

  namespace
  {
    // activations, cooldown, sensitivity, delay, mode.
    constexpr uint8_t M_RECORD_BYTES { 5 };

    // The static handler has no instance, so loaded bytes land here and Load()
    // copies them into the object.
    uint8_t s_loaded_record[M_RECORD_BYTES] {};
    bool s_record_loaded { false };

    int settingsSet(const char* key, size_t length, settings_read_cb readCallback, void* callbackArgument)
    {
      const char* next { nullptr };
      ssize_t readLength { 0 };

      if (!settings_name_steq(key, "v1", &next) || next != nullptr) { return -ENOENT; }
      if (length != sizeof(s_loaded_record)) { return -EINVAL; }

      readLength = readCallback(callbackArgument, s_loaded_record, sizeof(s_loaded_record));
      if (readLength != static_cast<ssize_t>(sizeof(s_loaded_record))) { return -EIO; }

      s_record_loaded = true;
      return 0;
    }

    SETTINGS_STATIC_HANDLER_DEFINE(mfs_params, "params", nullptr, settingsSet, nullptr, nullptr);
  }

  int Settings::Load()
  {
    int result { settings_load_subtree("params") };

    if (result < 0) {
      LOG_ERR("Failed to load settings: %d!", result);
      return result;
    }
    if (!s_record_loaded) { return 0; }

    // A stored record is validated like a payload. A corrupt activation count
    // would otherwise become a device that silently never triggers.
    if (s_loaded_record[0] < protocol::M_ACTIVATIONS_MIN || s_loaded_record[0] > protocol::M_ACTIVATIONS_MAX ||
        s_loaded_record[4] >= static_cast<uint8_t>(protocol::Mode::Reserved)) {
      LOG_ERR("Stored settings are invalid - keeping defaults!");
      return -EINVAL;
    }

    m_activations      = s_loaded_record[0];
    m_cooldown_byte    = s_loaded_record[1];
    m_sensitivity_byte = s_loaded_record[2];
    m_delay_code       = s_loaded_record[3] & protocol::M_DELAY_MASK;
    m_mode             = static_cast<protocol::Mode>(s_loaded_record[4]);
    return 0;
  }

  int Settings::save()
  {
    const uint8_t record[M_RECORD_BYTES] { m_activations, m_cooldown_byte, m_sensitivity_byte, m_delay_code, static_cast<uint8_t>(m_mode) };
    int result { settings_save_one("params/v1", record, sizeof(record)) };

    if (result < 0) { LOG_ERR("Failed to persist settings: %d!", result); }
    return result;
  }

#else

  // Host build: the conversion and change-detection logic is what the tests
  // exercise; persistence is a Zephyr concern.
  int Settings::Load()
  {
    return 0;
  }
  int Settings::save()
  {
    return 0;
  }

#endif

}
```

Append `src/settings.cpp` to `HOST_SRCS` in `Makefile`.

- [ ] **Step 4: Run to verify it passes**

Run: `make test`
Expected: `settings: OK` and `ALL TESTS PASSED`.

- [ ] **Step 5: Commit**

```bash
/Users/andy/nrfenv/bin/clang-format -i src/settings.hpp src/settings.cpp tests/test_settings.cpp tests/test_main.cpp
git add src/settings.hpp src/settings.cpp tests/ Makefile
git commit -m "Add NVS-backed settings with the mode gated on slot 0

Stores the raw wire bytes, so the generated table stays the single place a byte
becomes a physical quantity. ApplyFrom() writes only on change.

The mode is applied only when the caller says the command came from the Network
Manager's slot. From an engineer's slot the field is ignored and does not count
as a change, so a stolen phone cannot switch reporting on to locate sensors.

A stored record is validated on load like a payload, and the Zephyr half
registers a static settings handler - without one, settings_load_subtree()
loads nothing."
```


---

# PHASE 2 — Make the device the app's test target

## Task 8: PSA backend, bench credentials and the boot self-test

After this task the nRF54L05 proves at boot that CRACEN computes exactly what Python, OpenSSL and — later — the app compute. **This closes the last unproven link in the cryptography**, and nothing after it is trustworthy until it passes on hardware.

**Files:**
- Create: `src/crypto_psa.cpp`, `src/crypto_selftest.hpp`, `src/crypto_selftest.cpp`
- Create: `src/credentials.hpp`, `src/credentials.cpp`, `tests/test_credentials.cpp`
- Create: `credentials.conf` (from the template — **gitignored, never committed**)
- Modify: `Kconfig`, `credentials.conf.template`, `prj.conf`, `CMakeLists.txt`, `src/app.hpp`, `src/app.cpp`, `Makefile`, `tests/test_main.cpp`

**Interfaces:**
- Consumes: `crypto.hpp` and `access_keys` from Task 3, `access_vectors.hpp`.
- Produces: `credentials::ParseHexBytes(const char*, uint8_t*, size_t) -> bool`, `credentials::ParseDeviceId(const char*, uint32_t&) -> bool`, `credentials::IsAllZero(const uint8_t*, size_t) -> bool`; `crypto::RunSelfTest() -> int`; Kconfig `CONFIG_MFS_DEVICE_ID`, `CONFIG_MFS_DEVICE_SECRET`, `CONFIG_MFS_PROVISION_KEY`; `App::initAccess()`, `App::m_access_ready`, file-scope `s_device_id`, `s_device_secret`, `s_provision_key` in `app.cpp`.

- [ ] **Step 1: Write the failing credentials test**

Create `tests/test_credentials.cpp`:

```cpp
#include <cassert>
#include <cstdio>

#include "credentials.hpp"

void run_credentials_tests()
{
  using namespace alc::credentials;

  uint8_t bytes[4] {};
  uint32_t deviceId { 0 };

  // Mixed case is accepted.
  assert(ParseHexBytes("DEADbeef", bytes, sizeof(bytes)));
  assert(bytes[0] == 0xDE && bytes[1] == 0xAD && bytes[2] == 0xBE && bytes[3] == 0xEF);

  // STRICT LENGTH. A short secret must fail at boot, not become a key padded
  // with zeros that every such build shares.
  assert(!ParseHexBytes("DEADBE", bytes, sizeof(bytes)));
  assert(!ParseHexBytes("DEADBEEF00", bytes, sizeof(bytes)));
  assert(!ParseHexBytes("", bytes, sizeof(bytes)));

  // Non-hex anywhere fails, including the last digit.
  assert(!ParseHexBytes("DEADBEEG", bytes, sizeof(bytes)));
  assert(!ParseHexBytes("DE ADBEEF", bytes, sizeof(bytes)));
  assert(!ParseHexBytes(nullptr, bytes, sizeof(bytes)));

  // The device ID is big-endian as written: "4D465331" is 0x4D465331, 'MFS1'.
  assert(ParseDeviceId("4D465331", deviceId));
  assert(deviceId == 0x4D465331);
  assert(!ParseDeviceId("4D46533", deviceId));

  const uint8_t zeros[3] {};
  const uint8_t notZero[3] { 0, 0, 1 };
  assert(IsAllZero(zeros, sizeof(zeros)));
  assert(!IsAllZero(notZero, sizeof(notZero)));

  printf("credentials: OK\n");
}
```

Declare and call `run_credentials_tests();` in `tests/test_main.cpp`. Run `make test` — expected FAIL, `'credentials.hpp' file not found`.

- [ ] **Step 2: Implement credentials parsing**

Create `src/credentials.hpp`:

```cpp
#pragma once

#include <cstddef>
#include <cstdint>

namespace alc::credentials
{

  /**
   * @brief Parse exactly `length` bytes from a hex string of exactly 2 * length digits.
   *
   * Strict on purpose. A short or malformed secret must fail loudly at boot rather
   * than silently become a key of zeros that every build shares.
   *
   * @return True only if the whole string was consumed and every digit was hex.
   */
  bool ParseHexBytes(const char* hex, uint8_t* out, size_t length);

  /** @brief Parse an 8-digit hex device ID, e.g. "4D465331". */
  bool ParseDeviceId(const char* hex, uint32_t& deviceId);

  /** @brief True if every byte is zero - a secret nobody filled in. */
  bool IsAllZero(const uint8_t* bytes, size_t length);

}
```

Create `src/credentials.cpp`:

```cpp
#include "credentials.hpp"

namespace alc::credentials
{

  namespace
  {
    constexpr uint8_t M_DEVICE_ID_BYTES { 4 };

    int hexValue(char digit)
    {
      if (digit >= '0' && digit <= '9') { return digit - '0'; }
      if (digit >= 'a' && digit <= 'f') { return digit - 'a' + 10; }
      if (digit >= 'A' && digit <= 'F') { return digit - 'A' + 10; }
      return -1;
    }
  }

  bool ParseHexBytes(const char* hex, uint8_t* out, size_t length)
  {
    if (hex == nullptr || out == nullptr) { return false; }

    for (size_t index = 0; index < length; index++) {
      int high { hexValue(hex[2 * index]) };
      int low { high < 0 ? -1 : hexValue(hex[2 * index + 1]) };

      if (high < 0 || low < 0) { return false; }
      out[index] = static_cast<uint8_t>((high << 4) | low);
    }

    // Exactly the right length: a longer string is a mistake, not a bonus.
    return hex[2 * length] == '\0';
  }

  bool ParseDeviceId(const char* hex, uint32_t& deviceId)
  {
    uint8_t bytes[M_DEVICE_ID_BYTES] {};

    if (!ParseHexBytes(hex, bytes, sizeof(bytes))) { return false; }
    deviceId = (static_cast<uint32_t>(bytes[0]) << 24) | (static_cast<uint32_t>(bytes[1]) << 16) | (static_cast<uint32_t>(bytes[2]) << 8) | bytes[3];
    return true;
  }

  bool IsAllZero(const uint8_t* bytes, size_t length)
  {
    uint8_t accumulated { 0 };

    for (size_t index = 0; index < length; index++) {
      accumulated = static_cast<uint8_t>(accumulated | bytes[index]);
    }
    return accumulated == 0;
  }

}
```

Append `src/credentials.cpp` to `HOST_SRCS` in `Makefile`. Run `make test` — expected `credentials: OK`, `ALL TESTS PASSED`.

- [ ] **Step 3: Write the PSA backend and the self-test**

Create `src/crypto_psa.cpp`:

```cpp
// TARGET crypto backend: PSA Crypto, hardware-accelerated by CRACEN on the nRF54L05.
// Proven against tools/gen_access_vectors.py by crypto_selftest.cpp at boot.

#include <cerrno>

#include <psa/crypto.h>
#include <zephyr/logging/log.h>

#include "crypto.hpp"

LOG_MODULE_REGISTER(crypto_psa, LOG_LEVEL_INF);

namespace alc::crypto
{

  namespace
  {
    constexpr size_t M_HMAC_KEY_BITS { 256 };
    constexpr size_t M_AES128_KEY_BITS { 128 };
    constexpr size_t M_BITS_PER_BYTE { 8 };

    bool s_initialised { false };

    // Keys are imported, used once and destroyed. This phase holds raw key bytes
    // in RAM from Kconfig; production moves the device secret into the KMU and
    // derives there, at which point this seam is where that change lands.
    psa_status_t importKey(psa_key_type_t type, size_t bits, psa_key_usage_t usage, psa_algorithm_t algorithm, const uint8_t* key, psa_key_id_t& id)
    {
      psa_key_attributes_t attributes = PSA_KEY_ATTRIBUTES_INIT;

      psa_set_key_usage_flags(&attributes, usage);
      psa_set_key_lifetime(&attributes, PSA_KEY_LIFETIME_VOLATILE);
      psa_set_key_algorithm(&attributes, algorithm);
      psa_set_key_type(&attributes, type);
      psa_set_key_bits(&attributes, bits);
      return psa_import_key(&attributes, key, bits / M_BITS_PER_BYTE, &id);
    }

    psa_algorithm_t ccmAlgorithm(size_t tagLength)
    {
      return PSA_ALG_AEAD_WITH_SHORTENED_TAG(PSA_ALG_CCM, tagLength);
    }
  }

  int Init()
  {
    psa_status_t status { PSA_SUCCESS };

    if (s_initialised) { return 0; }

    status = psa_crypto_init();
    if (status != PSA_SUCCESS) {
      LOG_ERR("psa_crypto_init failed: %d!", status);
      return -EIO;
    }

    s_initialised = true;
    return 0;
  }

  int HmacSha256(const uint8_t* key, size_t keyLength, const uint8_t* message, size_t messageLength, uint8_t* out)
  {
    psa_key_id_t id { PSA_KEY_ID_NULL };
    size_t outLength { 0 };
    psa_status_t status { PSA_SUCCESS };

    // Every key in the scheme is exactly 256 bits. Anything else is a bug.
    if (keyLength * M_BITS_PER_BYTE != M_HMAC_KEY_BITS) { return -EINVAL; }

    status = importKey(PSA_KEY_TYPE_HMAC, M_HMAC_KEY_BITS, PSA_KEY_USAGE_SIGN_MESSAGE, PSA_ALG_HMAC(PSA_ALG_SHA_256), key, id);
    if (status != PSA_SUCCESS) {
      LOG_ERR("HMAC key import failed: %d!", status);
      return -EIO;
    }

    status = psa_mac_compute(id, PSA_ALG_HMAC(PSA_ALG_SHA_256), message, messageLength, out, M_HMAC_SHA256_BYTES, &outLength);
    psa_destroy_key(id);

    if (status != PSA_SUCCESS || outLength != M_HMAC_SHA256_BYTES) {
      LOG_ERR("psa_mac_compute failed: %d!", status);
      return -EIO;
    }
    return 0;
  }

  int AesCcmEncrypt(const uint8_t* key, const uint8_t* nonce, size_t nonceLength, const uint8_t* aad, size_t aadLength, const uint8_t* plaintext,
                    size_t length, uint8_t* ciphertext, uint8_t* tag, size_t tagLength)
  {
    constexpr size_t M_MAX_SEALED_BYTES { 64 };
    uint8_t sealed[M_MAX_SEALED_BYTES] {};
    psa_key_id_t id { PSA_KEY_ID_NULL };
    size_t outLength { 0 };
    psa_status_t status { PSA_SUCCESS };

    if (length + tagLength > sizeof(sealed)) { return -EINVAL; }

    status = importKey(PSA_KEY_TYPE_AES, M_AES128_KEY_BITS, PSA_KEY_USAGE_ENCRYPT, ccmAlgorithm(tagLength), key, id);
    if (status != PSA_SUCCESS) {
      LOG_ERR("AES key import failed: %d!", status);
      return -EIO;
    }

    // PSA emits ciphertext followed by the tag in one buffer.
    status = psa_aead_encrypt(id, ccmAlgorithm(tagLength), nonce, nonceLength, aad, aadLength, plaintext, length, sealed, sizeof(sealed), &outLength);
    psa_destroy_key(id);

    if (status != PSA_SUCCESS || outLength != length + tagLength) {
      LOG_ERR("psa_aead_encrypt failed: %d!", status);
      return -EIO;
    }

    for (size_t index = 0; index < length; index++) {
      ciphertext[index] = sealed[index];
    }
    for (size_t index = 0; index < tagLength; index++) {
      tag[index] = sealed[length + index];
    }
    return 0;
  }

  int AesCcmDecrypt(const uint8_t* key, const uint8_t* nonce, size_t nonceLength, const uint8_t* aad, size_t aadLength, const uint8_t* ciphertext,
                    size_t length, const uint8_t* tag, size_t tagLength, uint8_t* plaintext)
  {
    constexpr size_t M_MAX_SEALED_BYTES { 64 };
    uint8_t sealed[M_MAX_SEALED_BYTES] {};
    psa_key_id_t id { PSA_KEY_ID_NULL };
    size_t outLength { 0 };
    psa_status_t status { PSA_SUCCESS };

    if (length + tagLength > sizeof(sealed)) { return -EINVAL; }

    for (size_t index = 0; index < length; index++) {
      sealed[index] = ciphertext[index];
    }
    for (size_t index = 0; index < tagLength; index++) {
      sealed[length + index] = tag[index];
    }

    status = importKey(PSA_KEY_TYPE_AES, M_AES128_KEY_BITS, PSA_KEY_USAGE_DECRYPT, ccmAlgorithm(tagLength), key, id);
    if (status != PSA_SUCCESS) {
      LOG_ERR("AES key import failed: %d!", status);
      return -EIO;
    }

    status =
        psa_aead_decrypt(id, ccmAlgorithm(tagLength), nonce, nonceLength, aad, aadLength, sealed, length + tagLength, plaintext, length, &outLength);
    psa_destroy_key(id);

    // A tag mismatch is the ROUTINE outcome for a trial decryption, so it is
    // not logged here - AccessControl decides what it means.
    if (status == PSA_ERROR_INVALID_SIGNATURE) { return -EBADMSG; }
    if (status != PSA_SUCCESS || outLength != length) {
      LOG_ERR("psa_aead_decrypt failed: %d!", status);
      return -EIO;
    }
    return 0;
  }

}
```

Create `src/crypto_selftest.hpp`:

```cpp
#pragma once

namespace alc::crypto
{

  /**
   * @brief Prove the on-target crypto backend against the generated vectors.
   *
   * Runs every command vector and the time-sync vector from
   * tools/gen_access_vectors.py through access_keys on PSA. The host tests prove
   * the same derivations on OpenSSL; only this proves CRACEN.
   *
   * @return 0 if every vector matches; -EBADMSG on the first mismatch. The caller
   *         must refuse to process commands on failure - a backend that disagrees
   *         with the app would reject every genuine command, or worse.
   */
  int RunSelfTest();

}
```

Create `src/crypto_selftest.cpp`:

```cpp
#include <cerrno>
#include <cstring>

#include <zephyr/logging/log.h>

#include "access_keys.hpp"
#include "access_vectors.hpp"
#include "crypto_selftest.hpp"

LOG_MODULE_REGISTER(crypto_selftest, LOG_LEVEL_INF);

namespace alc::crypto
{

  int RunSelfTest()
  {
    using namespace alc::access;

    uint8_t dayKey[M_DAY_KEY_BYTES] {};
    uint8_t onAir[protocol::M_UUID_BYTES] {};
    uint8_t plaintext[protocol::M_PLAINTEXT_BYTES] {};
    uint32_t unixSeconds { 0 };
    uint8_t index { 0 };

    for (const vectors::CommandVector& vector : vectors::M_COMMANDS) {
      if (DeriveDayKey(vectors::M_SECRET, vectors::M_DEVICE_ID, vector.day, vector.slot, dayKey) != 0 ||
          memcmp(dayKey, vector.dayKey, sizeof(dayKey)) != 0) {
        LOG_ERR("Self-test vector %u: day key mismatch!", index);
        return -EBADMSG;
      }
      if (SealCommand(dayKey, vectors::M_DEVICE_ID, vector.day, vector.slot, vector.n, vector.plaintext, onAir) != 0 ||
          memcmp(onAir, vector.onAir, sizeof(onAir)) != 0) {
        LOG_ERR("Self-test vector %u: sealed command mismatch - CCM disagrees with the app!", index);
        return -EBADMSG;
      }
      if (OpenCommand(dayKey, vectors::M_DEVICE_ID, vector.day, vector.slot, vector.n, vector.onAir, plaintext) != 0 ||
          memcmp(plaintext, vector.plaintext, sizeof(plaintext)) != 0) {
        LOG_ERR("Self-test vector %u: open failed!", index);
        return -EBADMSG;
      }

      // A corrupted tag must be REJECTED. A backend that accepts anything would
      // pass every check above.
      memcpy(onAir, vector.onAir, sizeof(onAir));
      onAir[protocol::M_OFFSET_TAG] ^= 0x01;
      if (OpenCommand(dayKey, vectors::M_DEVICE_ID, vector.day, vector.slot, vector.n, onAir, plaintext) != -EBADMSG) {
        LOG_ERR("Self-test vector %u: a corrupted tag was not rejected!", index);
        return -EBADMSG;
      }
      index++;
    }

    if (!OpenTimeSync(vectors::M_PROVISION_KEY, vectors::M_DEVICE_ID, vectors::M_TIME_SYNC_ON_AIR, unixSeconds) ||
        unixSeconds != vectors::M_TIME_SYNC_UNIX) {
      LOG_ERR("Self-test: time sync vector failed!");
      return -EBADMSG;
    }

    LOG_INF("Crypto self-test passed: %u command vectors and the time-sync vector match.", index);
    return 0;
  }

}
```

- [ ] **Step 4: Kconfig, credentials template, prj.conf and CMake**

1. In `Kconfig`, replace:

```kconfig
config MFS_TAN_SEED
	string "TAN seed (64-char hex string)"
	default ""
	help
	  32-byte per-device secret from which every day's TANs are derived.
	  Loaded from credentials.conf (gitignored) for bench work only.
	  PRODUCTION MUST USE THE nRF54L05 KMU — a seed in the image is readable,
	  and a readable seed makes TAN expiry decorative. See
	  docs/tan-scheme.md section 7.

config MFS_PROVISION_KEY
	string "Provisioning key (64-char hex string)"
	default ""
	help
	  32-byte per-device secret authorising time-of-day sync, and nothing
	  else. Must be distinct from MFS_TAN_SEED so that a compromised
	  provisioner can cause denial of service but not entry. See
	  docs/tan-scheme.md section 7.1.
```

   with:

```kconfig
config MFS_DEVICE_ID
	string "Device ID (8-char hex string)"
	default ""
	help
	  Public 32-bit identity bound into every key derivation, e.g. 4D465331.
	  Not secret and never transmitted. The Network Manager looks the device
	  up by it. See docs/tan-scheme.md section 3.

config MFS_DEVICE_SECRET
	string "Device secret (64-char hex string)"
	default ""
	help
	  32-byte per-device secret from which every day key is derived.
	  Loaded from credentials.conf (gitignored) for bench work only.
	  PRODUCTION MUST USE THE nRF54L05 KMU - a secret in the image is
	  readable, and a readable secret yields every day key for this device
	  forever. See docs/tan-scheme.md section 8.

config MFS_PROVISION_KEY
	string "Provisioning key (64-char hex string)"
	default ""
	help
	  32-byte per-device key authorising a time sync while the clock is
	  invalid, and nothing else. Must differ from MFS_DEVICE_SECRET so that a
	  compromised provisioner can cause denial of service but not entry. The
	  firmware refuses to enable commands if the two are equal. See
	  docs/tan-scheme.md section 8.1.
```

2. In `credentials.conf.template`, replace:

```ini
# Omitting -DEXTRA_CONF_FILE leaves the seed and key empty, and every TAN and
# time sync will be rejected.
#
# BENCH ONLY. Production must provision both secrets into the nRF54L05 KMU —
# a seed in the image is readable, and a readable seed makes TAN expiry
# decorative. See docs/tan-scheme.md section 7.

CONFIG_ALC_DEVICE_SERIAL="MFS-0000-0000"

# 32-byte per-device TAN seed, 64 hex chars. Derives every day's sheet.
CONFIG_MFS_TAN_SEED=""

# 32-byte per-device provisioning key, 64 hex chars. Authorises time-of-day
# sync only. MUST be different from the TAN seed — see docs/tan-scheme.md 7.1.
CONFIG_MFS_PROVISION_KEY=""
```

   with:

```ini
# Omitting -DEXTRA_CONF_FILE leaves the credentials empty: the firmware boots,
# logs "Credentials missing or malformed", and ignores every command and sync.
#
# BENCH ONLY. Production must provision both keys into the nRF54L05 KMU - a
# secret in the image is readable, and a readable secret yields every day key
# for this device forever. See docs/tan-scheme.md section 8.
#
# Generate fresh keys with:
#     python3 -c "import secrets; print(secrets.token_hex(32).upper())"
#
# The class_app bench Network Manager reads THIS FILE through
# tools/gen_bench_credentials.py, so the app and the firmware cannot disagree.

CONFIG_ALC_DEVICE_SERIAL="MFS-0000-0000"

# 32-bit device ID, 8 hex chars. Public; bound into every key derivation.
CONFIG_MFS_DEVICE_ID=""

# 32-byte per-device secret, 64 hex chars. Derives every day key.
CONFIG_MFS_DEVICE_SECRET=""

# 32-byte per-device provisioning key, 64 hex chars. Authorises a time sync
# while the clock is invalid, and nothing else. MUST differ from the secret -
# the firmware refuses to enable commands otherwise. See docs/tan-scheme.md 8.1.
CONFIG_MFS_PROVISION_KEY=""
```

3. In `prj.conf`, replace:

```ini
# Crypto for HMAC-SHA256 — TAN derivation and provisioner time-sync
# authentication. Hardware-accelerated via CRACEN on the nRF54L05.
CONFIG_PSA_WANT_ALG_HMAC=y
CONFIG_PSA_WANT_ALG_SHA_256=y
CONFIG_PSA_WANT_KEY_TYPE_HMAC=y
```

   with:

```ini
# Crypto for the day-key scheme: HMAC-SHA256 for key derivation, rotating IDs
# and the time-sync tag; AES-128-CCM with a 4-byte tag for commands.
# Hardware-accelerated via CRACEN on the nRF54L05. See docs/tan-scheme.md.
CONFIG_PSA_WANT_ALG_HMAC=y
CONFIG_PSA_WANT_ALG_SHA_256=y
CONFIG_PSA_WANT_KEY_TYPE_HMAC=y
CONFIG_PSA_WANT_ALG_CCM=y
CONFIG_PSA_WANT_KEY_TYPE_AES=y
```

4. In `CMakeLists.txt`, replace:

```cmake
  src/npm2100_zephyr.cpp
)
```

   with:

```cmake
  src/npm2100_zephyr.cpp
  src/mfs_protocol.cpp
  src/access_keys.cpp
  src/credentials.cpp
  src/crypto_psa.cpp
  src/crypto_selftest.cpp
)
```

- [ ] **Step 5: Wire it into App**

1. In `src/app.hpp`, replace:

```cpp
      void toggleArmState();
```

   with:

```cpp
      void toggleArmState();

      // Parses the bench credentials from Kconfig, initialises PSA and runs the
      // crypto self-test. A failure leaves commands disabled for the whole boot -
      // see m_access_ready.
      int initAccess();
```

2. In `src/app.hpp`, replace:

```cpp
      CommandScanner m_scanner;
```

   with:

```cpp
      CommandScanner m_scanner;

      // False if credentials, PSA or the self-test failed. Commands and syncs are
      // then ignored for the whole boot: a backend that disagrees with the app
      // must not be trusted to judge anything.
      bool m_access_ready;
```

3. In `src/app.cpp`, replace:

```cpp
#include <zephyr/drivers/gpio.h>
```

   with:

```cpp
#include <cstring>

#include <zephyr/drivers/gpio.h>
```

4. In `src/app.cpp`, replace:

```cpp
#include "app.hpp"
#include "npm2100_zephyr.hpp"
```

   with:

```cpp
#include "access_keys.hpp"
#include "app.hpp"
#include "credentials.hpp"
#include "crypto.hpp"
#include "crypto_selftest.hpp"
#include "npm2100_zephyr.hpp"
```

5. In `src/app.cpp`, replace:

```cpp
    const struct gpio_dt_spec* const s_fem_pins[] { &s_fem_pdn, &s_fem_tx_en, &s_fem_rx_en, &s_fem_mode };
```

   with:

```cpp
    const struct gpio_dt_spec* const s_fem_pins[] { &s_fem_pdn, &s_fem_tx_en, &s_fem_rx_en, &s_fem_mode };

    // BENCH credentials, parsed from Kconfig (credentials.conf) at boot. Production
    // moves both keys into the KMU - see docs/tan-scheme.md section 8.
    uint32_t s_device_id { 0 };
    uint8_t s_device_secret[access::M_SECRET_BYTES] {};
    uint8_t s_provision_key[access::M_SECRET_BYTES] {};
```

6. In `src/app.cpp`, replace:

```cpp
      , m_scanner()
      , m_output_switch()
```

   with:

```cpp
      , m_scanner()
      , m_access_ready(false)
      , m_output_switch()
```

7. In `src/app.cpp`, replace:

```cpp
    result = lowerLsoutToUlp();
    if (result < 0) { return result; }
```

   with:

```cpp
    result = lowerLsoutToUlp();
    if (result < 0) { return result; }

    // Not fatal. A device that cannot authenticate commands still boots, safe
    // and Inactive, so its hardware can be diagnosed over RTT.
    result = initAccess();
    if (result < 0) { LOG_ERR("Access control unavailable (%d) - commands will be ignored this boot!", result); }
```

8. In `src/app.cpp`, replace:

```cpp
  void App::toggleArmState()
```

   with:

```cpp
  int App::initAccess()
  {
    int result { 0 };

    if (!credentials::ParseDeviceId(CONFIG_MFS_DEVICE_ID, s_device_id) || !credentials::ParseHexBytes(CONFIG_MFS_DEVICE_SECRET, s_device_secret, sizeof(s_device_secret))
        || !credentials::ParseHexBytes(CONFIG_MFS_PROVISION_KEY, s_provision_key, sizeof(s_provision_key))) {
      LOG_ERR("Credentials missing or malformed - build with -DEXTRA_CONF_FILE=credentials.conf!");
      return -EINVAL;
    }

    // An all-zero key is a template nobody filled in, and would be shared by every
    // such build. The two keys must also differ, or the provisioner holds entry.
    if (credentials::IsAllZero(s_device_secret, sizeof(s_device_secret)) || credentials::IsAllZero(s_provision_key, sizeof(s_provision_key))
        || memcmp(s_device_secret, s_provision_key, sizeof(s_device_secret)) == 0) {
      LOG_ERR("Device secret and provisioning key must be set and must differ!");
      return -EINVAL;
    }

    result = crypto::Init();
    if (result < 0) { return result; }

    result = crypto::RunSelfTest();
    if (result < 0) { return result; }

    m_access_ready = true;
    LOG_INF("Device 0x%08X: credentials loaded, crypto proven.", s_device_id);
    return 0;
  }

  void App::toggleArmState()
```

- [ ] **Step 6: Create the bench credentials**

```bash
cd /Users/andy/nordic/ncs/v3.2.4/class_mfs_1
git check-ignore credentials.conf   # must print "credentials.conf" - STOP if it prints nothing
cp credentials.conf.template credentials.conf
python3 - <<'EOF'
import re, secrets
path = "credentials.conf"
text = open(path).read()
text = text.replace('CONFIG_MFS_DEVICE_ID=""', f'CONFIG_MFS_DEVICE_ID="{secrets.token_hex(4).upper()}"')
text = text.replace('CONFIG_MFS_DEVICE_SECRET=""', f'CONFIG_MFS_DEVICE_SECRET="{secrets.token_hex(32).upper()}"')
text = text.replace('CONFIG_MFS_PROVISION_KEY=""', f'CONFIG_MFS_PROVISION_KEY="{secrets.token_hex(32).upper()}"')
open(path, "w").write(text)
EOF
grep -c '=""' credentials.conf   # expect 0
```

The self-test uses the fixed vector keys, not these, so it is independent of what you generate here.

- [ ] **Step 7: Build, flash and verify the self-test on hardware**

```bash
nrfutil device device-info --serial-number 853003346 | grep -i family   # expect NRF54L
west build -b nrf54l15dk/nrf54l05/cpuapp -p always -- -DEXTRA_CONF_FILE=credentials.conf
west flash --dev-id 853003346 --recover
```

Expected build: no warnings from `src/`. Expected in RTT (`open /Applications/SEGGER/JLink/JLinkRTTViewer.app`, device `nRF54L05_M33`, SWD, 4000 kHz):

```
<inf> crypto_selftest: Crypto self-test passed: 3 command vectors and the time-sync vector match.
<inf> app: Device 0x........: credentials loaded, crypto proven.
```

**If the self-test fails, stop.** Record the failing line. The most likely cause is CRACEN rejecting the 4-byte tag or the 11-byte nonce; check `psa_aead_encrypt`'s status in the log against `PSA_ERROR_NOT_SUPPORTED` (-134) before changing any code. Do not weaken the self-test to get past it.

Then rebuild **without** `-DEXTRA_CONF_FILE`, flash, and confirm the refusal path: `Credentials missing or malformed - build with -DEXTRA_CONF_FILE=credentials.conf!`, and the device still boots Inactive with LED A lit.

- [ ] **Step 8: Update the spec and commit**

In `docs/superpowers/specs/2026-09-12-app-control-design.md` §9, mark item 5 **RESOLVED** with today's date and the self-test log line. In `docs/tan-scheme.md` §11, delete the "PSA self-test on target" row.

```bash
/Users/andy/nrfenv/bin/clang-format -i src/crypto_psa.cpp src/crypto_selftest.hpp src/crypto_selftest.cpp src/credentials.hpp src/credentials.cpp src/app.hpp src/app.cpp tests/test_credentials.cpp tests/test_main.cpp
git status --short | grep credentials.conf && echo "STOP: credentials.conf is not ignored"
git add src/crypto_psa.cpp src/crypto_selftest.hpp src/crypto_selftest.cpp src/credentials.hpp src/credentials.cpp src/app.hpp src/app.cpp \
        tests/ Makefile Kconfig credentials.conf.template prj.conf CMakeLists.txt docs/
git commit -m "Prove AES-CCM and HMAC on CRACEN with a boot self-test

Adds the PSA crypto backend and runs the Python-generated vectors through it at
every boot. This is the only evidence that the nRF54L05 computes what the app
computes; a mismatch leaves commands disabled for the boot rather than letting a
disagreeing backend judge anything.

The bench credentials become a device ID, a device secret and a provisioning
key, parsed strictly: a short or malformed key fails at boot instead of silently
becoming zeros, and equal or all-zero keys are refused.

Verified on hardware: <paste the self-test log line>."
```

---

## Task 9: Scanner rewrite and access dispatch

After this task MFS_1 decodes and logs every authentic command field by field and accepts a provisioner time sync. That makes the real device the app's test target for Phase 3.

**Files:**
- Create: `src/access_store.hpp`, `src/access_store.cpp`
- Modify (replace): `src/command_scanner.hpp`, `src/command_scanner.cpp`
- Modify: `Kconfig`, `prj.conf`, `CMakeLists.txt`, `src/app.hpp`, `src/app.cpp`

**Interfaces:**
- Consumes: `AccessControl`, `DeviceClock`, `access::OpenTimeSync`, Task 8's `initAccess()` and credentials.
- Produces: `CommandScanner::Candidate { uint8_t bytes[16]; }`, `bool CommandScanner::TakeCandidate(Candidate&)`; `access_store::Load(AccessState&) -> int` (1 loaded, 0 none, negative errno), `access_store::Persist(const AccessState&, void*) -> int`; `App::m_clock`, `App::m_access`, `App::serviceCandidates()`, `App::handleTimeSyncCandidate(...)`, `App::handleCommandCandidate(...)`. Tasks 14–16 build on these.

- [ ] **Step 1: Write the access store**

Create `src/access_store.hpp`:

```cpp
#pragma once

#include "access_control.hpp"

namespace alc::access_store
{

  /**
   * @brief Load the persisted access state.
   *
   * Requires settings_subsys_init() to have run.
   *
   * @param state Written only if a valid record exists.
   * @return 1 if a record was loaded, 0 if none exists (a first boot), negative errno on failure.
   */
  int Load(AccessState& state);

  /**
   * @brief AccessControl::PersistFn. Returns only once the write has completed.
   *
   * `context` is unused.
   */
  int Persist(const AccessState& state, void* context);

}
```

Create `src/access_store.cpp`:

```cpp
#include <cerrno>
#include <cstring>

#include <zephyr/logging/log.h>
#include <zephyr/settings/settings.h>

#include "access_store.hpp"

LOG_MODULE_REGISTER(access_store, LOG_LEVEL_INF);

namespace alc::access_store
{

  namespace
  {
    // Fixed-size record, little-endian as the nRF54L05 stores it. The layout is
    // versioned by the key name, so a changed layout is a new key, never a
    // reinterpretation of old bytes.
    constexpr const char* M_KEY { "access/v1" };

    AccessState s_loaded {};
    bool s_record_loaded { false };

    int settingsSet(const char* key, size_t length, settings_read_cb readCallback, void* callbackArgument)
    {
      const char* next { nullptr };
      ssize_t readLength { 0 };

      if (!settings_name_steq(key, "v1", &next) || next != nullptr) { return -ENOENT; }
      if (length != sizeof(s_loaded)) {
        LOG_ERR("Stored access state is %u bytes, expected %u - ignoring it!", static_cast<unsigned>(length),
                static_cast<unsigned>(sizeof(s_loaded)));
        return -EINVAL;
      }

      readLength = readCallback(callbackArgument, &s_loaded, sizeof(s_loaded));
      if (readLength != static_cast<ssize_t>(sizeof(s_loaded))) { return -EIO; }

      s_record_loaded = true;
      return 0;
    }

    SETTINGS_STATIC_HANDLER_DEFINE(mfs_access, "access", nullptr, settingsSet, nullptr, nullptr);
  }

  int Load(AccessState& state)
  {
    int result { settings_load_subtree("access") };

    if (result < 0) {
      LOG_ERR("Failed to load the access state: %d!", result);
      return result;
    }
    if (!s_record_loaded) { return 0; }

    state = s_loaded;
    return 1;
  }

  int Persist(const AccessState& state, void* context)
  {
    int result { settings_save_one(M_KEY, &state, sizeof(state)) };

    ARG_UNUSED(context);

    if (result < 0) { LOG_ERR("Failed to persist the access state: %d!", result); }
    return result;
  }

}
```

- [ ] **Step 2: Replace the scanner**

It no longer validates anything — it cannot, without keys — and the 12 s toggle cooldown goes with the toggle. Replace `src/command_scanner.hpp` with:

```cpp
#pragma once

#include <cstdint>

#include "mfs_protocol.hpp"

namespace alc
{

  /**
   * @brief BLE observer that hands candidate command bytes to the main loop.
   *
   * MFS_1 never advertises and never connects. The engineer's phone advertises a
   * 128-bit service UUID; this scans passively for 16-byte UUIDs and queues them.
   *
   * **It validates nothing.** It cannot: a command is only recognisable with the
   * day keys, and a time sync only with the provisioning key. Every decision is
   * made by the main loop through AccessControl and DeviceClock, so no security
   * logic runs on the Bluetooth RX thread.
   *
   * It suppresses repeats of the UUIDs it queued most recently. A phone advertises
   * each command ~160 times in 30 s; without this the queue would fill with copies
   * and a distinct command arriving alongside them could be dropped.
   */
  class CommandScanner
  {
    public:
      /** @brief One queued 128-bit service UUID, on-air byte order. */
      struct Candidate
      {
          uint8_t bytes[protocol::M_UUID_BYTES];
      };

      CommandScanner();

      /**
       * @brief Enable Bluetooth and start the passive scan.
       * @return 0 on success; negative errno from bt_enable() or bt_le_scan_start().
       */
      int Start();

      /**
       * @brief Take the oldest queued candidate, if any. Never blocks.
       * @return True if `out` was written.
       */
      bool TakeCandidate(Candidate& out);

    private:
      bool m_started;
  };

}
```

Replace `src/command_scanner.cpp` with:

```cpp
#include <cstring>

#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include "command_scanner.hpp"

LOG_MODULE_REGISTER(scanner, LOG_LEVEL_INF);

namespace alc
{

  namespace
  {

    // Scan interval and window are expressed in 0.625 ms units. The controller does
    // the duty cycling, so the SoC sleeps between windows with no software timer.
    constexpr uint16_t M_UNITS_PER_MS_NUM { 8 };
    constexpr uint16_t M_UNITS_PER_MS_DEN { 5 };
    constexpr uint16_t M_SCAN_INTERVAL_UNITS { CONFIG_MFS_SCAN_PERIOD_MS * M_UNITS_PER_MS_NUM / M_UNITS_PER_MS_DEN };
    constexpr uint16_t M_SCAN_WINDOW_UNITS { CONFIG_MFS_SCAN_WINDOW_MS * M_UNITS_PER_MS_NUM / M_UNITS_PER_MS_DEN };

    // Complete list of 128-bit service UUIDs. iOS cannot send manufacturer data at
    // all, so the payload travels as a service UUID - see the design spec section 3.
    constexpr uint8_t M_AD_UUID128_ALL { 0x07 };

    // The main loop drains this every 100 ms. Eight distinct UUIDs in 100 ms is far
    // beyond any real radio environment around a covert sensor.
    constexpr size_t M_QUEUE_DEPTH { 8 };

    // How many recently queued UUIDs are remembered for repeat suppression.
    constexpr uint8_t M_RECENT_COUNT { 4 };

    K_MSGQ_DEFINE(s_candidates, sizeof(CommandScanner::Candidate), M_QUEUE_DEPTH, 1);

    // Touched only from the Bluetooth RX thread, so no lock is needed.
    uint8_t s_recent[M_RECENT_COUNT][protocol::M_UUID_BYTES] {};
    uint8_t s_recent_next { 0 };
    uint8_t s_recent_filled { 0 };

    bool seenRecently(const uint8_t* bytes)
    {
      for (uint8_t index = 0; index < s_recent_filled; index++) {
        if (memcmp(s_recent[index], bytes, protocol::M_UUID_BYTES) == 0) { return true; }
      }
      return false;
    }

    void remember(const uint8_t* bytes)
    {
      memcpy(s_recent[s_recent_next], bytes, protocol::M_UUID_BYTES);
      s_recent_next = static_cast<uint8_t>((s_recent_next + 1) % M_RECENT_COUNT);
      if (s_recent_filled < M_RECENT_COUNT) { s_recent_filled++; }
    }

    bool parseAdStructure(struct bt_data* data, void* userData)
    {
      CommandScanner::Candidate candidate {};

      ARG_UNUSED(userData);

      // Exactly one 128-bit UUID. A list of several is not our phone, which never
      // advertises anything else.
      if (data->type != M_AD_UUID128_ALL || data->data_len != protocol::M_UUID_BYTES) { return true; }
      if (seenRecently(data->data)) { return false; }

      memcpy(candidate.bytes, data->data, protocol::M_UUID_BYTES);
      if (k_msgq_put(&s_candidates, &candidate, K_NO_WAIT) != 0) {
        // Dropped, not remembered - so a later copy of the same advert can still
        // get in once the loop has drained the queue.
        LOG_WRN("Candidate queue full - advert dropped!");
        return false;
      }

      remember(data->data);
      return false;
    }

    void scanRecvCallback(const bt_addr_le_t* addr, int8_t rssi, uint8_t advType, struct net_buf_simple* buf)
    {
      ARG_UNUSED(addr);
      ARG_UNUSED(rssi);
      ARG_UNUSED(advType);

      bt_data_parse(buf, &parseAdStructure, nullptr);
    }

  }

  CommandScanner::CommandScanner()
      : m_started(false)
  {}

  int CommandScanner::Start()
  {
    int result { bt_enable(nullptr) };

    const struct bt_le_scan_param scanParam {
      .type     = BT_LE_SCAN_TYPE_PASSIVE,
      .options  = BT_LE_SCAN_OPT_NONE,
      .interval = M_SCAN_INTERVAL_UNITS,
      .window   = M_SCAN_WINDOW_UNITS,
    };

    if (result < 0) {
      LOG_ERR("bt_enable failed: %d!", result);
      return result;
    }

    result = bt_le_scan_start(&scanParam, &scanRecvCallback);
    if (result < 0) {
      LOG_ERR("bt_le_scan_start failed: %d!", result);
      return result;
    }

    m_started = true;
    LOG_INF("Passive scan started: %u ms window every %u ms.", CONFIG_MFS_SCAN_WINDOW_MS, CONFIG_MFS_SCAN_PERIOD_MS);
    return 0;
  }

  bool CommandScanner::TakeCandidate(Candidate& out)
  {
    return k_msgq_get(&s_candidates, &out, K_NO_WAIT) == 0;
  }

}
```

- [ ] **Step 3: Remove the insecure toggle, and add the new sources to the build**

1. In `Kconfig`, delete:

```kconfig
config MFS_INSECURE_TOGGLE
	bool "Accept an unauthenticated plaintext toggle command (BENCH ONLY)"
	default n
	help
	  Accepts a magic advertising payload that toggles the arm state with NO
	  TAN and NO authentication of any kind. Anyone in radio range can arm or
	  disarm the device.

	  This exists solely to prove the scan, parse, arm-state and LED path
	  before TAN validation and provisioner time sync are implemented. It MUST
	  be n in any build that leaves the bench. See docs/tan-scheme.md for the
	  scheme that replaces it.
```

2. In `prj.conf`, delete:

```ini
# BENCH ONLY - unauthenticated toggle. Must be n before this leaves the bench.
CONFIG_MFS_INSECURE_TOGGLE=y
```

3. In `CMakeLists.txt`, replace:

```cmake
  src/crypto_selftest.cpp
)
```

   with:

```cmake
  src/crypto_selftest.cpp
  src/access_store.cpp
  src/access_control.cpp
  src/device_clock.cpp
)
```

- [ ] **Step 4: Wire the clock and access control into App**

1. In `src/app.hpp`, replace:

```cpp
#include "adxl367.hpp"
#include "command_scanner.hpp"
```

   with:

```cpp
#include "access_control.hpp"
#include "adxl367.hpp"
#include "command_scanner.hpp"
#include "device_clock.hpp"
```

2. In `src/app.hpp`, replace:

```cpp
      void toggleArmState();

      // Parses the bench credentials from Kconfig, initialises PSA and runs the
      // crypto self-test. A failure leaves commands disabled for the whole boot -
      // see m_access_ready.
      int initAccess();
```

   with:

```cpp
      // Parses the bench credentials from Kconfig, initialises PSA, runs the crypto
      // self-test and restores the access state. A failure leaves commands
      // disabled for the whole boot - see m_access_ready.
      int initAccess();

      // Drains the scanner queue. While the clock is invalid a candidate is offered
      // ONLY to the time-sync check; once valid, ONLY to AccessControl.
      void serviceCandidates();

      void handleTimeSyncCandidate(const CommandScanner::Candidate& candidate, int64_t uptimeSecs);

      void handleCommandCandidate(const CommandScanner::Candidate& candidate, int64_t uptimeSecs);
```

3. In `src/app.hpp`, replace:

```cpp
      CommandScanner m_scanner;
```

   with:

```cpp
      CommandScanner m_scanner;

      // UTC for the access scheme. Invalid on every boot until a provisioner sync.
      DeviceClock m_clock;

      // The only judge of whether a candidate is an authentic, fresh command.
      AccessControl m_access;
```

4. In `src/app.cpp`, replace:

```cpp
#include "access_keys.hpp"
#include "app.hpp"
```

   with:

```cpp
#include <zephyr/settings/settings.h>

#include "access_keys.hpp"
#include "access_store.hpp"
#include "app.hpp"
```

5. In `src/app.cpp`, replace:

```cpp
    uint8_t s_provision_key[access::M_SECRET_BYTES] {};
```

   with:

```cpp
    uint8_t s_provision_key[access::M_SECRET_BYTES] {};

    const char* verdictName(AccessControl::Verdict verdict)
    {
      switch (verdict) {
        case AccessControl::Verdict::Accepted:
          return "Accepted";
        case AccessControl::Verdict::NotForUs:
          return "NotForUs";
        case AccessControl::Verdict::ClockInvalid:
          return "ClockInvalid";
        case AccessControl::Verdict::LockedOut:
          return "LockedOut";
        case AccessControl::Verdict::AuthFailed:
          return "AuthFailed";
        case AccessControl::Verdict::Malformed:
          return "Malformed";
        case AccessControl::Verdict::Stale:
          return "Stale";
        case AccessControl::Verdict::PersistFailed:
          return "PersistFailed";
        case AccessControl::Verdict::CryptoError:
          return "CryptoError";
        default:
          return "Unknown";
      }
    }
```

6. In `src/app.cpp`, replace:

```cpp
      , m_scanner()
      , m_access_ready(false)
```

   with:

```cpp
      , m_scanner()
      , m_clock()
      , m_access(0, s_device_secret, &access_store::Persist, nullptr)
      , m_access_ready(false)
```

7. In `src/app.cpp`, replace:

```cpp
      if (m_scanner.TakePendingCommand() == CommandScanner::Command::ToggleArm) { toggleArmState(); }
```

   with:

```cpp
      serviceCandidates();
```

8. In `src/app.cpp`, replace:

```cpp
  int App::initAccess()
  {
    int result { 0 };
```

   with:

```cpp
  int App::initAccess()
  {
    int result { 0 };
    AccessState restored {};
```

9. In `src/app.cpp`, replace:

```cpp
    result = crypto::RunSelfTest();
    if (result < 0) { return result; }

    m_access_ready = true;
    LOG_INF("Device 0x%08X: credentials loaded, crypto proven.", s_device_id);
    return 0;
  }
```

   with:

```cpp
    result = crypto::RunSelfTest();
    if (result < 0) { return result; }

    result = settings_subsys_init();
    if (result < 0) {
      LOG_ERR("settings_subsys_init failed: %d!", result);
      return result;
    }

    result = access_store::Load(restored);
    if (result < 0) { return result; }

    // The device ID is known only now, so AccessControl is rebuilt with it. The
    // restored day is the floor a provisioner sync may not go below; the clock
    // itself stays INVALID - there is no resume from NVS.
    m_access = AccessControl(s_device_id, s_device_secret, &access_store::Persist, nullptr);
    if (result > 0) {
      m_access.Restore(restored);
      m_clock.RaiseFloorDay(restored.day);
      LOG_INF("Access state restored: day floor %u.", restored.day);
    } else {
      LOG_INF("No access state stored - first boot, no day floor.");
    }

    m_access_ready = true;
    LOG_INF("Device 0x%08X ready. Clock INVALID until a provisioner time sync.", s_device_id);
    return 0;
  }

  void App::serviceCandidates()
  {
    CommandScanner::Candidate candidate {};
    int64_t uptimeSecs { k_uptime_get() / MSEC_PER_SEC };

    while (m_scanner.TakeCandidate(candidate)) {
      if (!m_access_ready) { continue; }

      if (!m_clock.IsValid()) {
        handleTimeSyncCandidate(candidate, uptimeSecs);
      } else {
        handleCommandCandidate(candidate, uptimeSecs);
      }
    }
  }

  void App::handleTimeSyncCandidate(const CommandScanner::Candidate& candidate, int64_t uptimeSecs)
  {
    uint32_t unixSeconds { 0 };
    DeviceClock::SyncResult syncResult { DeviceClock::SyncResult::BeforeEpoch };

    // Almost every advert in range lands here and fails the tag. That is routine
    // and is not logged, or the RTT buffer would fill with other people's phones.
    if (!access::OpenTimeSync(s_provision_key, s_device_id, candidate.bytes, unixSeconds)) { return; }

    syncResult = m_clock.ApplyProvisionerSync(unixSeconds, uptimeSecs);
    if (syncResult != DeviceClock::SyncResult::Applied) {
      LOG_WRN("Authentic time sync refused: result %u, unix %u, floor %u!", static_cast<unsigned>(syncResult), unixSeconds, m_clock.FloorDay());
      return;
    }

    LOG_INF("Clock set by provisioner: unix %u, day %u, %02u:%02u UTC.", unixSeconds, m_clock.DayIndex(uptimeSecs), m_clock.MinuteOfDay(uptimeSecs) / 60U,
            m_clock.MinuteOfDay(uptimeSecs) % 60U);
  }

  void App::handleCommandCandidate(const CommandScanner::Candidate& candidate, int64_t uptimeSecs)
  {
    AccessControl::Evaluation evaluation { m_access.Evaluate(candidate.bytes, sizeof(candidate.bytes), m_clock, uptimeSecs) };

    if (evaluation.verdict == AccessControl::Verdict::NotForUs) { return; }

    if (evaluation.verdict != AccessControl::Verdict::Accepted) {
      // RTT only. Nothing on the radio, nothing on the LEDs.
      LOG_WRN("Command rejected: %s (failures %u).", verdictName(evaluation.verdict), m_access.ConsecutiveFailures());
      return;
    }

    // Decoded field by field on purpose: when a slider produces the wrong byte
    // this is where you see it, rather than inferring it from an LED.
    LOG_INF("Command slot %u n %u: arm %s, delay %u s, activations %u, mode %u, cooldown %u s, threshold %u LSB, minute %u.", evaluation.slot,
            evaluation.n, evaluation.command.armActive ? "ACTIVE" : "INACTIVE", protocol::DelayToSeconds(evaluation.command.delayCode),
            evaluation.command.activations, static_cast<unsigned>(evaluation.command.mode), protocol::CooldownToSeconds(evaluation.command.cooldownByte),
            protocol::SensitivityToThresholdLsb(evaluation.command.sensitivityByte), evaluation.command.minuteOfDay);

    // PHASE 2 SHIM. Applies only the arm bit so the device stays usable while the
    // app is built against it. Settings, the clock trim and the LED patterns
    // arrive in Task 16, which replaces this.
    setArmState(evaluation.command.armActive ? ArmState::Active : ArmState::Inactive);
  }
```

10. In `src/app.cpp`, delete:

```cpp
  void App::toggleArmState()
  {
    setArmState(m_arm_state == ArmState::Active ? ArmState::Inactive : ArmState::Active);
  }
```

- [ ] **Step 5: Build, flash and verify boot on hardware**

```bash
grep -rn "INSECURE_TOGGLE\|TakePendingCommand\|toggleArmState" src Kconfig prj.conf   # expect nothing
west build -b nrf54l15dk/nrf54l05/cpuapp -p always -- -DEXTRA_CONF_FILE=credentials.conf
west flash --dev-id 853003346 --recover
```

Expected in RTT, in order:

```
<inf> crypto_selftest: Crypto self-test passed: ...
<inf> app: No access state stored - first boot, no day floor.
<inf> app: Device 0x........ ready. Clock INVALID until a provisioner time sync.
<inf> scanner: Passive scan started: 100 ms window every 6000 ms.
```

Nothing decodes yet because nothing is advertising — that is Task 13. `make test` must still pass.

- [ ] **Step 6: Commit**

```bash
/Users/andy/nrfenv/bin/clang-format -i src/access_store.hpp src/access_store.cpp src/command_scanner.hpp src/command_scanner.cpp src/app.hpp src/app.cpp
git add src/access_store.hpp src/access_store.cpp src/command_scanner.hpp src/command_scanner.cpp src/app.hpp src/app.cpp Kconfig prj.conf CMakeLists.txt
git commit -m "Dispatch scanned UUIDs to the clock and access control

The scanner now queues raw 16-byte service UUIDs and decides nothing: a command
is only recognisable with the day keys, so every decision moves to the main loop.
While the clock is invalid a candidate is offered only to the time-sync check;
once valid, only to AccessControl.

The access state is restored at boot as a day floor, never as a clock. The
unauthenticated toggle, its Kconfig gate and the 12 s command cooldown are
removed.

The arm-bit handling in handleCommandCandidate() is a deliberate temporary shim
so the device stays usable while the app is built; Task 16 replaces it."
```

---

# PHASE 3 — The app

## Task 10: Scaffold `class_app`

**Files:**
- Create: `/Users/andy/nordic/ncs/v3.2.4/class_app/` (Flutter project)
- Create (in `class_mfs_1`): `tools/gen_bench_credentials.py`

**Interfaces:**
- Consumes: `tools/gen_protocol_tables.py`, `tools/gen_access_vectors.py`, `credentials.conf`.
- Produces: a buildable Flutter project with `ble_peripheral` 2.4.0, `pointycastle` 4.0.0, `crypto`, `shared_preferences`; generated `lib/protocol/tables.dart`, `test/access_vectors.dart`, and the gitignored `lib/services/bench_credentials.dart`.

- [ ] **Step 1: Create the project and pin the dependencies**

```bash
cd /Users/andy/nordic/ncs/v3.2.4
flutter create --org ie.alcsystems --platforms=ios,android,macos --project-name class_app class_app
cd class_app
flutter pub add ble_peripheral:2.4.0 pointycastle:4.0.0 crypto shared_preferences
rm test/widget_test.dart
```

`pointycastle` is pinned to 4.0.0 because that is the version proven against the vectors; a bump must re-run Task 11's vector test before anything else.

- [ ] **Step 2: Keep the bench credentials out of git — before they exist**

```bash
printf '\n# BENCH secrets, generated from class_mfs_1/credentials.conf. Never commit.\nlib/services/bench_credentials.dart\n' >> .gitignore
git init && git add -A && git commit -m "Scaffold the CLASS control app"
```

- [ ] **Step 3: Add the Bluetooth permissions**

macOS needs a sandbox entitlement; iOS needs a usage string. Without these the radio silently never powers on.

```bash
cd /Users/andy/nordic/ncs/v3.2.4/class_app
for f in macos/Runner/DebugProfile.entitlements macos/Runner/Release.entitlements; do
  /usr/libexec/PlistBuddy -c "Add :com.apple.security.device.bluetooth bool true" "$f" 2>/dev/null \
    || /usr/libexec/PlistBuddy -c "Set :com.apple.security.device.bluetooth true" "$f"
done
/usr/libexec/PlistBuddy -c "Add :NSBluetoothAlwaysUsageDescription string 'Sends CLASS device commands over Bluetooth.'" ios/Runner/Info.plist 2>/dev/null
```

- [ ] **Step 4: Write the bench credentials generator (in `class_mfs_1`)**

Create `class_mfs_1/tools/gen_bench_credentials.py`:

```python
#!/usr/bin/env python3
"""Writes the class_app bench credentials from THIS repo's credentials.conf.

BENCH ONLY. The output holds a device secret and a provisioning key, which the
production app must never contain. It is written to a gitignored file in
class_app so it cannot be committed there, and it is generated rather than typed
so the app and the firmware cannot disagree about the keys.

Run from the class_mfs_1 root:
    python3 tools/gen_bench_credentials.py
"""
import pathlib
import re
import sys

KEYS = ("CONFIG_MFS_DEVICE_ID", "CONFIG_MFS_DEVICE_SECRET", "CONFIG_MFS_PROVISION_KEY", "CONFIG_ALC_DEVICE_SERIAL")
LENGTHS = {"CONFIG_MFS_DEVICE_ID": 8, "CONFIG_MFS_DEVICE_SECRET": 64, "CONFIG_MFS_PROVISION_KEY": 64}


def parse(path: pathlib.Path) -> dict:
    values = {}
    for line in path.read_text().splitlines():
        match = re.match(r'^(CONFIG_[A-Z0-9_]+)="([^"]*)"\s*$', line.strip())
        if match and match.group(1) in KEYS:
            values[match.group(1)] = match.group(2)
    for key in KEYS:
        if key not in values:
            sys.exit(f"{path}: {key} is missing")
    for key, length in LENGTHS.items():
        if not re.fullmatch(rf"[0-9A-Fa-f]{{{length}}}", values[key]):
            sys.exit(f"{path}: {key} must be exactly {length} hex digits")
    if values["CONFIG_MFS_DEVICE_SECRET"].upper() == values["CONFIG_MFS_PROVISION_KEY"].upper():
        sys.exit(f"{path}: the device secret and provisioning key must differ")
    return values


def dart_bytes(hex_string: str) -> str:
    return "<int>[" + ", ".join(f"0x{hex_string[i:i + 2].upper()}" for i in range(0, len(hex_string), 2)) + "]"


def main():
    here = pathlib.Path(__file__).resolve().parent.parent
    values = parse(here / "credentials.conf")
    out = here.parent / "class_app" / "lib" / "services" / "bench_credentials.dart"
    if not out.parent.exists():
        sys.exit(f"{out.parent} does not exist - scaffold class_app first")
    out.write_text("\n".join([
        "// GENERATED by class_mfs_1/tools/gen_bench_credentials.py - DO NOT EDIT OR COMMIT.",
        "// BENCH ONLY: the production app must never hold a device secret.",
        "",
        f"const String kBenchDeviceLabel = '{values['CONFIG_ALC_DEVICE_SERIAL']}';",
        f"const int kBenchDeviceId = 0x{values['CONFIG_MFS_DEVICE_ID'].upper()};",
        f"const List<int> kBenchDeviceSecret = {dart_bytes(values['CONFIG_MFS_DEVICE_SECRET'])};",
        f"const List<int> kBenchProvisionKey = {dart_bytes(values['CONFIG_MFS_PROVISION_KEY'])};",
        "",
    ]))
    print(f"wrote {out}")


if __name__ == "__main__":
    main()
```

- [ ] **Step 5: Generate everything the app needs**

```bash
mkdir -p /Users/andy/nordic/ncs/v3.2.4/class_app/lib/protocol /Users/andy/nordic/ncs/v3.2.4/class_app/lib/services /Users/andy/nordic/ncs/v3.2.4/class_app/test
cd /Users/andy/nordic/ncs/v3.2.4/class_mfs_1
python3 tools/gen_protocol_tables.py
python3 tools/gen_access_vectors.py
python3 tools/gen_bench_credentials.py
cd ../class_app && git check-ignore lib/services/bench_credentials.dart   # must print the path
```

Expected: three pairs of `wrote ...` lines, the second of each pair inside `class_app`, and `git check-ignore` printing the credentials path. **If it prints nothing, stop and fix `.gitignore` before any commit.**

- [ ] **Step 6: Verify it builds, and commit both repos**

```bash
cd /Users/andy/nordic/ncs/v3.2.4/class_app && flutter build macos --debug
git add -A && git status --short | grep bench_credentials && echo "STOP"
git commit -m "Add dependencies, Bluetooth entitlements and the generated tables

pointycastle is pinned to 4.0.0, the version proven to reproduce the firmware's
AES-CCM vectors with a 4-byte tag. The bench credentials file is gitignored
before it is ever generated."
cd ../class_mfs_1
/Users/andy/nrfenv/bin/clang-format --version >/dev/null
git add tools/gen_bench_credentials.py
git commit -m "Generate the app's bench credentials from credentials.conf

The bench Network Manager in class_app needs the same device secret and
provisioning key as the firmware. Generating them from the one file removes any
chance of the two disagreeing, and the output lands in a gitignored file."
```

Expected build: `✓ Built build/macos/Build/Products/Debug/class_app.app`.

---

## Task 11: Dart protocol and access keys, against the vectors

**Files:**
- Create: `class_app/lib/protocol/mfs_protocol.dart`, `lib/protocol/access_keys.dart`, `lib/protocol/day_clock.dart`
- Create: `class_app/test/access_test.dart`, `test/protocol_test.dart`

**Interfaces:**
- Consumes: `lib/protocol/tables.dart`, `test/access_vectors.dart`.
- Produces: `Mfs1Mode`, `Mfs1Command`, `Uint8List encodePlaintext(Mfs1Command)`, `String formatDelay(int)`, `String buildUuidFromBytes(List<int>)`; `deriveDayKey`, `deriveEncKey`, `deriveRotatingId`, `buildNonce`, `Uint8List sealCommand(List<int> dayKey, int deviceId, int day, int slot, int n, List<int> plaintext)`, `Uint8List buildTimeSync(List<int> provisionKey, int deviceId, int unixSeconds)`, `kSlotCount`, `kSlotNetworkManager`, `kSecretBytes`; `int dayIndexOf(DateTime)`, `int minuteOfDay(DateTime)`, `int unixSecondsOf(DateTime)`.

- [ ] **Step 1: Write the failing tests**

Create `class_app/test/access_test.dart`:

```dart
import 'package:class_app/protocol/access_keys.dart';
import 'package:flutter_test/flutter_test.dart';

import 'access_vectors.dart';

void main() {
  // Generated by Python's `cryptography` package, which is neither the
  // firmware's implementation nor this one.
  for (final CommandVector v in kCommandVectors) {
    test('command vector day ${v.day} slot ${v.slot} n ${v.n}', () {
      final dayKey = deriveDayKey(kVectorSecret, kVectorDeviceId, v.day, v.slot);
      expect(dayKey, v.dayKey);
      expect(deriveEncKey(dayKey), v.encKey);
      expect(deriveRotatingId(dayKey, v.n), v.rotatingId);
      expect(sealCommand(dayKey, kVectorDeviceId, v.day, v.slot, v.n, v.plaintext), v.onAir);
    });
  }

  test('time sync vector', () {
    expect(buildTimeSync(kVectorProvisionKey, kVectorDeviceId, kVectorTimeSyncUnix), kVectorTimeSyncOnAir);
  });
}
```

Create `class_app/test/protocol_test.dart`:

```dart
import 'package:class_app/protocol/day_clock.dart';
import 'package:class_app/protocol/mfs_protocol.dart';
import 'package:class_app/protocol/tables.dart';
import 'package:flutter_test/flutter_test.dart';

import 'access_vectors.dart';

void main() {
  test('table anchors match the firmware', () {
    expect(kThresholdTable[0], 4000);
    expect(kThresholdTable[143], 302);
    expect(kThresholdTable[255], 40);
    expect(kCooldownTable[128], 60);
    expect(kDelayTable[59], 59);
    expect(kDelayTable[60], 60);
    expect(kDelayTable[119], 3600);
    expect(kDelayTable[127], 32400);
  });

  test('plaintext encodes exactly as the vectors expect', () {
    // arm_slot1_n0: arm, 3 activations, 60 s cooldown, 302 LSB, 09:02 UTC.
    final bytes = encodePlaintext(const Mfs1Command(
      armActive: true,
      activations: 3,
      cooldownByte: 128,
      sensitivityByte: 143,
      minuteOfDay: 9 * 60 + 2,
    ));
    expect(bytes, kCommandVectors[0].plaintext);

    // mode_slot0_n3: Report only in bits 4-5 of byte 1, sensitivity 200.
    expect(
      encodePlaintext(const Mfs1Command(
        armActive: false,
        activations: 1,
        cooldownByte: 0,
        sensitivityByte: 200,
        minuteOfDay: 0,
        mode: Mfs1Mode.reportOnly,
      )),
      kCommandVectors[2].plaintext,
    );
  });

  test('plaintext refuses values the device would reject', () {
    Mfs1Command withMinute(int minute) =>
        Mfs1Command(armActive: false, activations: 1, cooldownByte: 0, sensitivityByte: 0, minuteOfDay: minute);
    expect(() => encodePlaintext(withMinute(1440)), throwsRangeError);
    expect(
      () => encodePlaintext(
          const Mfs1Command(armActive: false, activations: 17, cooldownByte: 0, sensitivityByte: 0, minuteOfDay: 0)),
      throwsRangeError,
    );
  });

  test('delay formatting follows the encoding', () {
    expect(formatDelay(0), 'none');
    expect(formatDelay(59), '59 s');
    expect(formatDelay(60), '1 min');
    expect(formatDelay(127), '9 h');
  });

  test('the UUID string is the on-air bytes REVERSED', () {
    final uuid = buildUuidFromBytes(List<int>.generate(16, (i) => i));
    expect(uuid, '0F0E0D0C-0B0A-0908-0706-050403020100');
  });

  test('day index and minute are UTC with a 04:00 boundary', () {
    // Day 256 starts at 04:00 UTC on 2026-09-14.
    expect(dayIndexOf(DateTime.utc(2026, 9, 14, 4, 0)), 256);
    expect(dayIndexOf(DateTime.utc(2026, 9, 14, 3, 59, 59)), 255);
    expect(minuteOfDay(DateTime.utc(2026, 9, 14, 9, 2)), 542);
    // A local time is converted, not taken at face value.
    expect(dayIndexOf(DateTime.utc(2026, 9, 14, 4, 0).toLocal()), 256);
  });
}
```

- [ ] **Step 2: Run to verify they fail**

Run: `cd /Users/andy/nordic/ncs/v3.2.4/class_app && flutter test`
Expected: FAIL — `Error when reading 'lib/protocol/access_keys.dart'`.

- [ ] **Step 3: Implement**

Create `lib/protocol/mfs_protocol.dart`:

```dart
import 'dart:typed_data';

import 'tables.dart';

/// Plaintext layout — mirrors class_mfs_1/src/mfs_protocol.hpp.
/// See class_mfs_1/docs/tan-scheme.md section 6.1.
const int kPtArmDelay = 0;
const int kPtActivationsMode = 1;
const int kPtCooldown = 2;
const int kPtSensitivity = 3;
const int kPtMinute = 4;
const int kMinutesPerDay = 1440;

/// What the device does when the activation count is reached.
///
/// reportAndTrigger and reportOnly make the device ADVERTISE, an exception to
/// the standing rule that it never does. Only a slot-0 (Network Manager)
/// command may set them — the device ignores the field from engineers' slots.
enum Mfs1Mode { triggerOnly, reportAndTrigger, reportOnly }

class Mfs1Command {
  const Mfs1Command({
    required this.armActive,
    required this.activations,
    required this.cooldownByte,
    required this.sensitivityByte,
    required this.minuteOfDay,
    this.delayCode = 0,
    this.mode = Mfs1Mode.triggerOnly,
  });

  final bool armActive;
  final int activations; // 1..16
  final int cooldownByte; // 0 = none
  final int sensitivityByte;
  final int minuteOfDay; // UTC, 0..1439
  final int delayCode; // 0..127, see kDelayTable
  final Mfs1Mode mode;

  int get thresholdLsb => kThresholdTable[sensitivityByte];
  int get cooldownSeconds => kCooldownTable[cooldownByte];
  int get delaySeconds => kDelayTable[delayCode];
}

/// Encodes the 8 plaintext bytes. Bytes 6-7 are per-variant space, left zero.
Uint8List encodePlaintext(Mfs1Command c) {
  if (c.activations < 1 || c.activations > 16) {
    throw RangeError.range(c.activations, 1, 16, 'activations');
  }
  if (c.minuteOfDay < 0 || c.minuteOfDay >= kMinutesPerDay) {
    throw RangeError.range(c.minuteOfDay, 0, kMinutesPerDay - 1, 'minuteOfDay');
  }
  final Uint8List bytes = Uint8List(8);
  bytes[kPtArmDelay] = (c.armActive ? 0x01 : 0x00) | ((c.delayCode & 0x7F) << 1);
  bytes[kPtActivationsMode] = ((c.activations - 1) & 0x0F) | ((c.mode.index & 0x03) << 4);
  bytes[kPtCooldown] = c.cooldownByte;
  bytes[kPtSensitivity] = c.sensitivityByte;
  bytes[kPtMinute] = c.minuteOfDay & 0xFF;
  bytes[kPtMinute + 1] = (c.minuteOfDay >> 8) & 0x07;
  return bytes;
}

/// Formats a delay code the way the encoding is defined, not as raw seconds:
/// the operator picked "5 minutes", so show "5 min".
String formatDelay(int code) {
  if (code == 0) return 'none';
  if (code < 60) return '$code s';
  if (code < 119) return '${code - 59} min';
  return '${code - 118} h';
}

/// Formats on-air bytes as a UUID string.
///
/// Bluetooth transmits a 128-bit UUID least-significant byte first, so the
/// string is the on-air sequence REVERSED. Getting this backwards makes every
/// rotating ID miss on the device, with no error anywhere.
String buildUuidFromBytes(List<int> onAir) {
  if (onAir.length != 16) {
    throw ArgumentError('a 128-bit UUID is 16 bytes');
  }
  final String hex = onAir.reversed.map((int b) => b.toRadixString(16).padLeft(2, '0')).join().toUpperCase();
  return '${hex.substring(0, 8)}-${hex.substring(8, 12)}-'
      '${hex.substring(12, 16)}-${hex.substring(16, 20)}-${hex.substring(20)}';
}
```

Create `lib/protocol/access_keys.dart`:

```dart
import 'dart:typed_data';

import 'package:crypto/crypto.dart' as hashing;
import 'package:pointycastle/export.dart';

/// Mirrors class_mfs_1/src/access_keys.cpp byte for byte. Both are checked
/// against tools/gen_access_vectors.py — see test/access_test.dart.
///
/// Scheme: class_mfs_1/docs/tan-scheme.md section 3.

const int kSecretBytes = 32;
const int kSlotCount = 8;
const int kSlotNetworkManager = 0;
const int kProtocolVersion = 0x02;
const int kRotatingIdBytes = 4;
const int kPlaintextBytes = 8;
const int kTagBytes = 4;
const int kUuidBytes = 16;
const int kTimeSyncTagBytes = 12;

const int kLabelDayKey = 0x02;
const int kLabelEncKey = 0x03;
const int kLabelRotatingId = 0x04;
const int kLabelTimeSync = 0x05;

Uint8List _hmac(List<int> key, List<int> message) =>
    Uint8List.fromList(hashing.Hmac(hashing.sha256, key).convert(message).bytes);

Uint8List _be32(int value) => Uint8List(4)..buffer.asByteData().setUint32(0, value, Endian.big);

Uint8List _be16(int value) => Uint8List(2)..buffer.asByteData().setUint16(0, value, Endian.big);

/// dayKey = HMAC-SHA256(secret, id BE32 | day BE16 | slot | 0x02).
Uint8List deriveDayKey(List<int> secret, int deviceId, int day, int slot) =>
    _hmac(secret, <int>[..._be32(deviceId), ..._be16(day), slot, kLabelDayKey]);

/// encKey = first 16 bytes of HMAC-SHA256(dayKey, 0x03).
Uint8List deriveEncKey(List<int> dayKey) => _hmac(dayKey, <int>[kLabelEncKey]).sublist(0, 16);

/// rotatingId(n) = first 4 bytes of HMAC-SHA256(dayKey, n BE32 | 0x04).
Uint8List deriveRotatingId(List<int> dayKey, int n) =>
    _hmac(dayKey, <int>[..._be32(n), kLabelRotatingId]).sublist(0, kRotatingIdBytes);

/// nonce = id BE32 | day BE16 | slot | n BE32 (11 bytes).
Uint8List buildNonce(int deviceId, int day, int slot, int n) =>
    Uint8List.fromList(<int>[..._be32(deviceId), ..._be16(day), slot, ..._be32(n)]);

/// Builds the 16 on-air bytes for command number [n]:
/// rotating ID (4) | AES-128-CCM ciphertext (8) | tag (4).
///
/// THE CALLER MUST NEVER REUSE [n] for the same device, day and slot. The nonce
/// is derived from it, and two different plaintexts under one nonce can be
/// XORed together to reveal the settings.
Uint8List sealCommand(List<int> dayKey, int deviceId, int day, int slot, int n, List<int> plaintext) {
  if (plaintext.length != kPlaintextBytes) {
    throw ArgumentError('plaintext must be $kPlaintextBytes bytes');
  }
  final Uint8List rotatingId = deriveRotatingId(dayKey, n);
  final CCMBlockCipher cipher = CCMBlockCipher(AESEngine())
    ..init(
      true,
      AEADParameters(
        KeyParameter(deriveEncKey(dayKey)),
        kTagBytes * 8,
        buildNonce(deviceId, day, slot, n),
        // The version is never on air. Both sides supply it as associated data.
        Uint8List.fromList(<int>[...rotatingId, kProtocolVersion]),
      ),
    );
  final Uint8List sealed = cipher.process(Uint8List.fromList(plaintext));
  return Uint8List.fromList(<int>[...rotatingId, ...sealed]);
}

/// Time sync on air: unix LE32 | first 12 bytes of
/// HMAC-SHA256(provisionKey, id BE32 | unix BE32 | 0x05).
Uint8List buildTimeSync(List<int> provisionKey, int deviceId, int unixSeconds) {
  final Uint8List tag = _hmac(provisionKey, <int>[..._be32(deviceId), ..._be32(unixSeconds), kLabelTimeSync]);
  final Uint8List time = Uint8List(4)..buffer.asByteData().setUint32(0, unixSeconds, Endian.little);
  return Uint8List.fromList(<int>[...time, ...tag.sublist(0, kTimeSyncTagBytes)]);
}
```

Create `lib/protocol/day_clock.dart`:

```dart
/// Day index and minute of day — mirrors class_mfs_1/src/device_clock.cpp.
///
/// UTC ONLY. The day boundary is 04:00 UTC, never local time: Ireland's GMT/IST
/// switch would otherwise put the phone and the device a day apart across every
/// DST transition. See class_mfs_1/docs/tan-scheme.md section 4.
const int kEpochUnix = 1767225600; // 2026-01-01T00:00:00Z
const int kDayBoundaryOffsetSecs = 4 * 3600;
const int kSecondsPerDay = 86400;

/// The day index for [instant], which may be in any time zone — it is
/// converted to UTC first.
int dayIndexOf(DateTime instant) {
  final int unix = instant.toUtc().millisecondsSinceEpoch ~/ 1000;
  if (unix < kEpochUnix + kDayBoundaryOffsetSecs) {
    throw ArgumentError('before the first day boundary of the epoch');
  }
  return (unix - kEpochUnix - kDayBoundaryOffsetSecs) ~/ kSecondsPerDay;
}

/// UTC minute since 00:00 UTC, 0..1439 — NOT since the 04:00 boundary.
int minuteOfDay(DateTime instant) {
  final DateTime utc = instant.toUtc();
  return utc.hour * 60 + utc.minute;
}

/// Whole UNIX seconds, for the provisioner's time sync.
int unixSecondsOf(DateTime instant) => instant.toUtc().millisecondsSinceEpoch ~/ 1000;
```

- [ ] **Step 4: Run to verify they pass**

Run: `flutter analyze && flutter test`
Expected: `No issues found!` and `All tests passed!`. **A vector mismatch means the app and the firmware disagree** — fix the Dart, never the vectors.

- [ ] **Step 5: Commit**

```bash
git add lib/protocol test
git commit -m "Add the command encoding and access keys, proven against the vectors

The app reproduces the Python-generated vectors byte for byte - day key,
encryption key, rotating ID, the sealed command including its 4-byte CCM tag,
and the time sync - which is the evidence it and the firmware agree.

The UUID string is the on-air bytes reversed, pinned by a test: getting it
backwards makes every rotating ID miss on the device with no error anywhere.
Day index and minute are computed in UTC with the 04:00 boundary."
```

---

## Task 12: Sequence store, key source, command builder and advertiser

**Files:**
- Create: `class_app/lib/services/sequence_store.dart`, `prefs_sequence_store.dart`, `key_source.dart`, `command_builder.dart`, `advertiser.dart`
- Create: `class_app/test/services_test.dart`, `test/sequence_store_test.dart`

**Interfaces:**
- Consumes: Task 11's protocol and keys.
- Produces: `abstract class SequenceStore { Future<int> reserve(int deviceId, int day, int slot); }`, `MemorySequenceStore`, `PrefsSequenceStore.open()`; `Mfs1Device { label, deviceId }`, `DayGrant { day, slot, dayKey }`, `abstract class KeySource { devices; engineerGrant(device, day); networkManagerGrant(device, day); }`, `BenchNetworkManager({device, secret, engineerSlot = 1})`; `Mfs1Settings { armActive, activations, cooldownByte, sensitivityByte, delayCode }`, `CommandBuilder({keys, sequence, clock})` with `Future<String> engineerCommand(Mfs1Device, Mfs1Settings)` and `Future<String> networkManagerCommand(Mfs1Device, Mfs1Settings, Mfs1Mode)`; `Advertiser` with `initialise()`, `send(String uuid)`, `stop()`, `ready`, `advertising`, `secondsLeft`.

- [ ] **Step 1: Write the failing tests**

Create `class_app/test/services_test.dart`:

```dart
import 'package:class_app/protocol/mfs_protocol.dart';
import 'package:class_app/services/command_builder.dart';
import 'package:class_app/services/key_source.dart';
import 'package:class_app/services/sequence_store.dart';
import 'package:flutter_test/flutter_test.dart';

import 'access_vectors.dart';

void main() {
  test('the sequence store never hands out the same number twice', () async {
    final store = MemorySequenceStore();
    expect(await store.reserve(1, 256, 1), 0);
    expect(await store.reserve(1, 256, 1), 1);
    expect(await store.reserve(1, 256, 2), 0); // slots are independent
    expect(await store.reserve(1, 257, 1), 0); // so are days
  });

  test('the bench Network Manager never issues slot 0 to an engineer', () {
    const device = Mfs1Device(label: 'Bench', deviceId: kVectorDeviceId);
    expect(() => BenchNetworkManager(device: device, secret: kVectorSecret, engineerSlot: 0), throwsRangeError);
    final keys = BenchNetworkManager(device: device, secret: kVectorSecret);
    expect(keys.engineerGrant(device, 256).slot, 1);
    expect(keys.engineerGrant(device, 256).dayKey, kCommandVectors[0].dayKey);
    expect(keys.networkManagerGrant(device, 257).dayKey, kCommandVectors[2].dayKey);
  });

  test('the command builder produces the vector bytes end to end', () async {
    const device = Mfs1Device(label: 'Bench', deviceId: kVectorDeviceId);
    final builder = CommandBuilder(
      keys: BenchNetworkManager(device: device, secret: kVectorSecret),
      sequence: MemorySequenceStore(),
      // 09:02 UTC on day 256 - the minute baked into the first vector.
      clock: () => DateTime.utc(2026, 9, 14, 9, 2, 30),
    );
    const settings = Mfs1Settings(armActive: true, activations: 3, cooldownByte: 128, sensitivityByte: 143);

    expect(await builder.engineerCommand(device, settings), buildUuidFromBytes(kCommandVectors[0].onAir));

    // The SAME settings sent again must not produce the same bytes: a new n.
    expect(await builder.engineerCommand(device, settings), isNot(buildUuidFromBytes(kCommandVectors[0].onAir)));
  });
}
```

Create `class_app/test/sequence_store_test.dart`:

```dart
import 'package:class_app/services/prefs_sequence_store.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:shared_preferences/shared_preferences.dart';

void main() {
  test('the persistent store survives a restart without reusing a number', () async {
    SharedPreferences.setMockInitialValues(<String, Object>{});

    final PrefsSequenceStore first = await PrefsSequenceStore.open();
    expect(await first.reserve(0x4D465331, 256, 1), 0);
    expect(await first.reserve(0x4D465331, 256, 1), 1);

    // A fresh instance - as after an app restart - continues, never restarts.
    final PrefsSequenceStore second = await PrefsSequenceStore.open();
    expect(await second.reserve(0x4D465331, 256, 1), 2);
    expect(await second.reserve(0x4D465331, 256, 2), 0);
  });
}
```

- [ ] **Step 2: Run to verify they fail**

Run: `flutter test`
Expected: FAIL — `Error when reading 'lib/services/command_builder.dart'`.

- [ ] **Step 3: Implement the services**

Create `lib/services/sequence_store.dart`:

```dart
/// Hands out sequence numbers, never the same one twice.
///
/// The sequence number is the CCM nonce. Reusing one for the same device, day
/// and slot with different settings lets a listener XOR the two commands and
/// recover the settings. Implementations MUST have durably recorded n + 1
/// before [reserve] returns n — the caller advertises only after that.
abstract class SequenceStore {
  Future<int> reserve(int deviceId, int day, int slot);
}

/// For tests only. Forgets everything when the process ends, which in the app
/// would be exactly the nonce-reuse failure this interface exists to prevent.
class MemorySequenceStore implements SequenceStore {
  final Map<String, int> _next = <String, int>{};

  @override
  Future<int> reserve(int deviceId, int day, int slot) async {
    final String key = sequenceKey(deviceId, day, slot);
    final int n = _next[key] ?? 0;
    _next[key] = n + 1;
    return n;
  }
}

String sequenceKey(int deviceId, int day, int slot) =>
    'seq/${deviceId.toRadixString(16).padLeft(8, '0')}/$day/$slot';
```

Create `lib/services/prefs_sequence_store.dart`:

```dart
import 'package:shared_preferences/shared_preferences.dart';

import 'sequence_store.dart';

/// Sequence numbers persisted with shared_preferences.
///
/// [reserve] writes n + 1 and waits for the platform to confirm the write before
/// returning n, so a crash between reserving and advertising wastes a number
/// instead of reusing one.
///
/// LIMITATION, accepted for the bench: uninstalling the app or clearing its data
/// resets every counter, which WOULD reuse nonces for today's keys. The
/// production Network Manager must issue a fresh slot to a reinstalled app
/// rather than trust the phone (class_mfs_1/docs/tan-scheme.md section 3.1).
class PrefsSequenceStore implements SequenceStore {
  PrefsSequenceStore(this._prefs);

  final SharedPreferences _prefs;

  static Future<PrefsSequenceStore> open() async => PrefsSequenceStore(await SharedPreferences.getInstance());

  @override
  Future<int> reserve(int deviceId, int day, int slot) async {
    final String key = sequenceKey(deviceId, day, slot);
    final int n = _prefs.getInt(key) ?? 0;
    final bool saved = await _prefs.setInt(key, n + 1);
    if (!saved) {
      // Refuse rather than risk handing out a number that was not recorded.
      throw StateError('could not persist the next sequence number for $key');
    }
    return n;
  }
}
```

Create `lib/services/key_source.dart`:

```dart
import 'dart:typed_data';

import '../protocol/access_keys.dart';

/// A device the engineer can pick.
class Mfs1Device {
  const Mfs1Device({required this.label, required this.deviceId});

  final String label;
  final int deviceId;
}

/// Authority over one device for one day: the key and the slot it belongs to.
class DayGrant {
  const DayGrant({required this.day, required this.slot, required this.dayKey});

  final int day;
  final int slot;
  final Uint8List dayKey;
}

/// Where day keys come from. In production: the Network Manager, after login
/// and app attestation. This phase: [BenchNetworkManager].
abstract class KeySource {
  List<Mfs1Device> get devices;

  /// The engineer's grant for [device] on [day]. Never slot 0.
  DayGrant engineerGrant(Mfs1Device device, int day);

  /// The Network Manager's own slot-0 grant, used ONLY to build mode-change
  /// commands. In production this never reaches the phone: the Network Manager
  /// builds the sealed command and the phone merely advertises it.
  DayGrant networkManagerGrant(Mfs1Device device, int day);
}

/// BENCH ONLY. Holds a device secret, which the production app must never do.
///
/// Stands in for the Network Manager so everything above it is written against
/// the production [KeySource] interface.
class BenchNetworkManager implements KeySource {
  BenchNetworkManager({required this.device, required List<int> secret, this.engineerSlot = 1})
      : _secret = List<int>.unmodifiable(secret) {
    if (secret.length != kSecretBytes) {
      throw ArgumentError('the device secret is $kSecretBytes bytes');
    }
    if (engineerSlot <= kSlotNetworkManager || engineerSlot >= kSlotCount) {
      throw RangeError.range(engineerSlot, 1, kSlotCount - 1, 'engineerSlot');
    }
  }

  final Mfs1Device device;
  final int engineerSlot;
  final List<int> _secret;

  @override
  List<Mfs1Device> get devices => <Mfs1Device>[device];

  @override
  DayGrant engineerGrant(Mfs1Device device, int day) => _grant(device, day, engineerSlot);

  @override
  DayGrant networkManagerGrant(Mfs1Device device, int day) => _grant(device, day, kSlotNetworkManager);

  DayGrant _grant(Mfs1Device target, int day, int slot) {
    if (target.deviceId != device.deviceId) {
      throw ArgumentError('this bench Network Manager knows only ${device.label}');
    }
    return DayGrant(day: day, slot: slot, dayKey: deriveDayKey(_secret, target.deviceId, day, slot));
  }
}
```

Create `lib/services/command_builder.dart`:

```dart
import '../protocol/access_keys.dart';
import '../protocol/day_clock.dart';
import '../protocol/mfs_protocol.dart';
import 'key_source.dart';
import 'sequence_store.dart';

/// The settings a Send carries, without the time — the builder stamps that.
class Mfs1Settings {
  const Mfs1Settings({
    required this.armActive,
    required this.activations,
    required this.cooldownByte,
    required this.sensitivityByte,
    this.delayCode = 0,
  });

  final bool armActive;
  final int activations;
  final int cooldownByte;
  final int sensitivityByte;
  final int delayCode;
}

/// Turns settings into a UUID string ready to advertise.
///
/// Every call RESERVES a new sequence number before sealing, so two Sends can
/// never share a nonce — including a retry of the same settings.
class CommandBuilder {
  CommandBuilder({required this.keys, required this.sequence, DateTime Function()? clock})
      : _clock = clock ?? DateTime.now;

  final KeySource keys;
  final SequenceStore sequence;
  final DateTime Function() _clock;

  /// An engineer's command. The mode field is sent as triggerOnly and ignored
  /// by the device anyway — only slot 0 can change it.
  Future<String> engineerCommand(Mfs1Device device, Mfs1Settings settings) async {
    final DateTime now = _clock();
    return _seal(keys.engineerGrant(device, dayIndexOf(now)), device, settings, Mfs1Mode.triggerOnly, now);
  }

  /// A Network Manager command that also sets the operating mode.
  Future<String> networkManagerCommand(Mfs1Device device, Mfs1Settings settings, Mfs1Mode mode) async {
    final DateTime now = _clock();
    return _seal(keys.networkManagerGrant(device, dayIndexOf(now)), device, settings, mode, now);
  }

  Future<String> _seal(DayGrant grant, Mfs1Device device, Mfs1Settings settings, Mfs1Mode mode, DateTime now) async {
    final Mfs1Command command = Mfs1Command(
      armActive: settings.armActive,
      activations: settings.activations,
      cooldownByte: settings.cooldownByte,
      sensitivityByte: settings.sensitivityByte,
      delayCode: settings.delayCode,
      mode: mode,
      minuteOfDay: minuteOfDay(now),
    );
    // Reserve FIRST. The store has recorded n + 1 before this returns.
    final int n = await sequence.reserve(device.deviceId, grant.day, grant.slot);
    return buildUuidFromBytes(
      sealCommand(grant.dayKey, device.deviceId, grant.day, grant.slot, n, encodePlaintext(command)),
    );
  }
}
```

Create `lib/services/advertiser.dart`:

```dart
import 'dart:async';

import 'package:ble_peripheral/ble_peripheral.dart';
import 'package:flutter/foundation.dart';

/// Advertises a payload UUID for a fixed window.
///
/// THIRTY SECONDS IS A MEASURED NUMBER, not a guess. A phone advertises at
/// roughly 187 ms and the interval is not ours to set; the device listens for
/// 100 ms every 6 s, so each wake detects with probability ~53%. Thirty seconds
/// spans six wakes, giving ~99%. Shortening this trades reliability directly.
const Duration kAdvertiseWindow = Duration(seconds: 30);

class Advertiser extends ChangeNotifier {
  bool _ready = false;
  bool _advertising = false;
  int _secondsLeft = 0;
  Timer? _countdown;

  bool get ready => _ready;
  bool get advertising => _advertising;
  int get secondsLeft => _secondsLeft;

  Future<void> initialise() async {
    BlePeripheral.setBleStateChangeCallback((bool on) {
      _ready = on;
      notifyListeners();
    });
    BlePeripheral.setAdvertisingStatusUpdateCallback((bool on, String? error) {
      _advertising = on;
      if (error != null) debugPrint('[advertiser] error: $error');
      notifyListeners();
    });
    await BlePeripheral.initialize();
  }

  /// Advertises [uuid] for [kAdvertiseWindow], then stops.
  ///
  /// No local name is sent. A covert device's counterpart must not broadcast a
  /// string, and the name would consume advertising bytes for nothing.
  Future<void> send(String uuid) async {
    await stop();
    await BlePeripheral.startAdvertising(services: <String>[uuid]);

    _secondsLeft = kAdvertiseWindow.inSeconds;
    notifyListeners();
    _countdown = Timer.periodic(const Duration(seconds: 1), (Timer t) {
      _secondsLeft--;
      notifyListeners();
      if (_secondsLeft <= 0) stop();
    });
  }

  Future<void> stop() async {
    _countdown?.cancel();
    _countdown = null;
    _secondsLeft = 0;
    await BlePeripheral.stopAdvertising();
    notifyListeners();
  }

  @override
  void dispose() {
    _countdown?.cancel();
    super.dispose();
  }
}
```

- [ ] **Step 4: Run to verify they pass**

Run: `flutter analyze && flutter test`
Expected: `No issues found!` and `All tests passed!` (14 tests).

- [ ] **Step 5: Commit**

```bash
git add lib/services test
git status --short | grep bench_credentials && echo "STOP"
git commit -m "Add the sequence store, bench Network Manager and command builder

Every Send reserves a new sequence number, and the store has durably recorded
n + 1 before it returns n, so a crash between reserving and advertising wastes a
number instead of reusing a nonce. A test proves the same settings sent twice
produce different bytes, and that the persistent store continues across a
restart.

Keys come through a KeySource interface. The only implementation this phase is
BenchNetworkManager, which holds the bench secret and never issues slot 0 to an
engineer; the production Network Manager replaces it without touching anything
above it.

The command builder reproduces the first vector end to end from settings and a
fixed clock."
```

---

## Task 13: The screens, and the end-to-end check on hardware

**Files:**
- Create: `class_app/lib/devices/mfs1/mfs1_screen.dart`, `lib/devices/provisioner/provisioner_screen.dart`
- Modify (replace): `class_app/lib/main.dart`

**Interfaces:**
- Consumes: everything from Task 12, `bench_credentials.dart`.
- Produces: the running app. Nothing later consumes it.

- [ ] **Step 1: Write the screens and main**

Create `lib/devices/mfs1/mfs1_screen.dart`:

```dart
import 'package:flutter/material.dart';

import '../../protocol/mfs_protocol.dart';
import '../../protocol/tables.dart';
import '../../services/advertiser.dart';
import '../../services/command_builder.dart';
import '../../services/key_source.dart';

/// The MFS_1 control screen.
///
/// Every Send is the same operation: build the command from the toggle and the
/// sliders, reserve the next sequence number, seal, advertise. There is no code
/// to pick and no test code - tuning while Inactive is an ordinary command with
/// the arm bit clear.
class Mfs1Screen extends StatefulWidget {
  const Mfs1Screen({super.key, required this.device, required this.builder, required this.advertiser});

  final Mfs1Device device;
  final CommandBuilder builder;
  final Advertiser advertiser;

  @override
  State<Mfs1Screen> createState() => _Mfs1ScreenState();
}

class _Mfs1ScreenState extends State<Mfs1Screen> {
  bool _armActive = false;
  int _activations = 1;
  int _cooldownByte = 0;
  int _sensitivityByte = 143; // the firmware's default, ~75 mg
  int _delayCode = 0;
  Mfs1Mode _mode = Mfs1Mode.triggerOnly;
  String? _lastError;

  @override
  void initState() {
    super.initState();
    widget.advertiser.addListener(_refresh);
  }

  @override
  void dispose() {
    widget.advertiser.removeListener(_refresh);
    super.dispose();
  }

  void _refresh() => setState(() {});

  Mfs1Settings get _settings => Mfs1Settings(
        armActive: _armActive,
        activations: _activations,
        cooldownByte: _cooldownByte,
        sensitivityByte: _sensitivityByte,
        delayCode: _delayCode,
      );

  Future<void> _send({required bool networkManager}) async {
    try {
      final String uuid = networkManager
          ? await widget.builder.networkManagerCommand(widget.device, _settings, _mode)
          : await widget.builder.engineerCommand(widget.device, _settings);
      await widget.advertiser.send(uuid);
      setState(() => _lastError = null);
    } on Object catch (error) {
      // Nothing was advertised. The sequence number may have been reserved,
      // which only wastes it.
      setState(() => _lastError = '$error');
    }
  }

  @override
  Widget build(BuildContext context) {
    final Advertiser advertiser = widget.advertiser;
    final bool canSend = advertiser.ready && !advertiser.advertising;

    return Scaffold(
      appBar: AppBar(title: Text('MFS_1 — ${widget.device.label}')),
      body: ListView(
        padding: const EdgeInsets.all(16),
        children: <Widget>[
          SwitchListTile(
            title: const Text('Armed'),
            subtitle: Text(_armActive ? 'Send arms with these settings' : 'Send applies settings for tuning'),
            value: _armActive,
            onChanged: (bool v) => setState(() => _armActive = v),
          ),
          const Divider(),
          const Text('Activations before triggering'),
          Wrap(
            spacing: 6,
            children: <Widget>[
              for (int n = 1; n <= 16; n++)
                ChoiceChip(
                  label: Text('$n'),
                  selected: _activations == n,
                  onSelected: (_) => setState(() => _activations = n),
                ),
            ],
          ),
          const SizedBox(height: 16),
          // Only meaningful with more than one activation - with N = 1 every
          // activation triggers, so there is nothing to blank between.
          if (_activations > 1) ...<Widget>[
            Text('Cooldown between activations — ${kCooldownTable[_cooldownByte]} s'),
            Slider(
              value: _cooldownByte.toDouble(),
              max: 255,
              divisions: 255,
              onChanged: (double v) => setState(() => _cooldownByte = v.round()),
            ),
            const SizedBox(height: 16),
          ],
          Text('Delay before triggering — ${formatDelay(_delayCode)}'),
          Slider(
            value: _delayCode.toDouble(),
            max: 127,
            divisions: 127,
            onChanged: (double v) => setState(() => _delayCode = v.round()),
          ),
          const SizedBox(height: 16),
          Text('Sensitivity — ${(kThresholdTable[_sensitivityByte] * 0.25).toStringAsFixed(1)} mg'),
          const Text('less sensitive  →  more sensitive', style: TextStyle(fontSize: 11)),
          Slider(
            value: _sensitivityByte.toDouble(),
            max: 255,
            divisions: 255,
            onChanged: (double v) => setState(() => _sensitivityByte = v.round()),
          ),
          const SizedBox(height: 24),
          FilledButton(
            onPressed: canSend ? () => _send(networkManager: false) : null,
            child: Text(advertiser.advertising ? 'Advertising… ${advertiser.secondsLeft} s' : 'Send'),
          ),
          const Padding(
            padding: EdgeInsets.only(top: 8),
            child: Text(
              'Watch LED A. Rapid flash: armed. Slow flash: disarmed. Double blink: a pending '
              'trigger was cancelled. Three long pulses: arming refused. One blink: settings '
              'applied. No flash: the command did not land — send again.',
              style: TextStyle(fontSize: 12),
            ),
          ),
          if (!advertiser.ready) const Padding(padding: EdgeInsets.only(top: 8), child: Text('Bluetooth is off.')),
          if (_lastError != null) Padding(padding: const EdgeInsets.only(top: 8), child: Text('Not sent: $_lastError')),
          const Divider(height: 40),
          // Visibly separate: this is a slot-0 command, which in production only
          // the Network Manager can build.
          const Text('Network Manager (bench)', style: TextStyle(fontWeight: FontWeight.bold)),
          const Text('Operating mode'),
          SegmentedButton<Mfs1Mode>(
            segments: const <ButtonSegment<Mfs1Mode>>[
              ButtonSegment<Mfs1Mode>(value: Mfs1Mode.triggerOnly, label: Text('Trigger')),
              ButtonSegment<Mfs1Mode>(value: Mfs1Mode.reportAndTrigger, label: Text('Report+Trig')),
              ButtonSegment<Mfs1Mode>(value: Mfs1Mode.reportOnly, label: Text('Report')),
            ],
            selected: <Mfs1Mode>{_mode},
            onSelectionChanged: (Set<Mfs1Mode> v) => setState(() => _mode = v.first),
          ),
          // The warning is not decoration. The device is scan-only by design, and
          // these modes are documented exceptions to that.
          if (_mode != Mfs1Mode.triggerOnly)
            const Padding(
              padding: EdgeInsets.only(top: 8),
              child: Text(
                'This device will TRANSMIT when triggered. Advertising forfeits covertness — use only when necessary.',
                style: TextStyle(fontSize: 11, fontWeight: FontWeight.bold),
              ),
            ),
          const SizedBox(height: 8),
          OutlinedButton(
            onPressed: canSend ? () => _send(networkManager: true) : null,
            child: const Text('Send as Network Manager (sets mode)'),
          ),
        ],
      ),
    );
  }
}
```

Create `lib/devices/provisioner/provisioner_screen.dart`:

```dart
import 'package:flutter/material.dart';

import '../../protocol/access_keys.dart';
import '../../protocol/day_clock.dart';
import '../../protocol/mfs_protocol.dart';
import '../../services/advertiser.dart';
import '../../services/key_source.dart';

/// BENCH provisioner: sets a device's clock after a reset.
///
/// The device clock is invalid on every boot and obeys no command until this
/// arrives (class_mfs_1/docs/tan-scheme.md section 7). The phone's own clock
/// must be network-synced: a phone minutes out makes every later command stale.
class ProvisionerScreen extends StatefulWidget {
  const ProvisionerScreen({super.key, required this.device, required this.provisionKey, required this.advertiser});

  final Mfs1Device device;
  final List<int> provisionKey;
  final Advertiser advertiser;

  @override
  State<ProvisionerScreen> createState() => _ProvisionerScreenState();
}

class _ProvisionerScreenState extends State<ProvisionerScreen> {
  @override
  void initState() {
    super.initState();
    widget.advertiser.addListener(_refresh);
  }

  @override
  void dispose() {
    widget.advertiser.removeListener(_refresh);
    super.dispose();
  }

  void _refresh() => setState(() {});

  Future<void> _send() async {
    // Stamped at the moment of sending. The device accepts it only while its
    // clock is invalid, so resending later is harmless.
    final int unix = unixSecondsOf(DateTime.now());
    await widget.advertiser.send(buildUuidFromBytes(buildTimeSync(widget.provisionKey, widget.device.deviceId, unix)));
  }

  @override
  Widget build(BuildContext context) {
    final Advertiser advertiser = widget.advertiser;
    final DateTime now = DateTime.now().toUtc();

    return Scaffold(
      appBar: AppBar(title: Text('Provision — ${widget.device.label}')),
      body: Padding(
        padding: const EdgeInsets.all(16),
        child: Column(
          crossAxisAlignment: CrossAxisAlignment.stretch,
          children: <Widget>[
            Text('Phone UTC: ${now.toIso8601String()}'),
            Text('Day index: ${dayIndexOf(now)}'),
            const SizedBox(height: 24),
            FilledButton(
              onPressed: advertiser.ready && !advertiser.advertising ? _send : null,
              child: Text(advertiser.advertising ? 'Advertising… ${advertiser.secondsLeft} s' : 'Send time sync'),
            ),
            const Padding(
              padding: EdgeInsets.only(top: 8),
              child: Text(
                'Nothing lights on the device. Confirm "Clock set by provisioner" over RTT.',
                style: TextStyle(fontSize: 12),
              ),
            ),
          ],
        ),
      ),
    );
  }
}
```

Replace `lib/main.dart` with:

```dart
import 'package:flutter/material.dart';

import 'devices/mfs1/mfs1_screen.dart';
import 'devices/provisioner/provisioner_screen.dart';
import 'services/advertiser.dart';
import 'services/bench_credentials.dart';
import 'services/command_builder.dart';
import 'services/key_source.dart';
import 'services/prefs_sequence_store.dart';

Future<void> main() async {
  WidgetsFlutterBinding.ensureInitialized();

  // BENCH wiring. In production the KeySource is the Network Manager and there
  // is no provisioning key on the phone at all.
  const Mfs1Device device = Mfs1Device(label: kBenchDeviceLabel, deviceId: kBenchDeviceId);
  final Advertiser advertiser = Advertiser();
  final CommandBuilder builder = CommandBuilder(
    keys: BenchNetworkManager(device: device, secret: kBenchDeviceSecret),
    sequence: await PrefsSequenceStore.open(),
  );
  await advertiser.initialise();

  runApp(ClassApp(device: device, builder: builder, advertiser: advertiser));
}

class ClassApp extends StatelessWidget {
  const ClassApp({super.key, required this.device, required this.builder, required this.advertiser});

  final Mfs1Device device;
  final CommandBuilder builder;
  final Advertiser advertiser;

  @override
  Widget build(BuildContext context) => MaterialApp(
        title: 'CLASS',
        theme: ThemeData(useMaterial3: true, colorSchemeSeed: Colors.teal),
        home: DevicePicker(device: device, builder: builder, advertiser: advertiser),
      );
}

/// One bench device this phase. The picker exists because commands are per
/// device and the phone advertises blind: the engineer must say which device
/// they are standing at.
class DevicePicker extends StatelessWidget {
  const DevicePicker({super.key, required this.device, required this.builder, required this.advertiser});

  final Mfs1Device device;
  final CommandBuilder builder;
  final Advertiser advertiser;

  @override
  Widget build(BuildContext context) {
    return Scaffold(
      appBar: AppBar(title: const Text('CLASS — pick a device')),
      body: ListView(
        children: <Widget>[
          ListTile(
            title: Text(device.label),
            subtitle: Text('0x${device.deviceId.toRadixString(16).toUpperCase().padLeft(8, '0')}'),
            onTap: () => Navigator.of(context).push(MaterialPageRoute<void>(
              builder: (_) => Mfs1Screen(device: device, builder: builder, advertiser: advertiser),
            )),
            trailing: IconButton(
              icon: const Icon(Icons.schedule),
              tooltip: 'Provision clock',
              onPressed: () => Navigator.of(context).push(MaterialPageRoute<void>(
                builder: (_) => ProvisionerScreen(device: device, provisionKey: kBenchProvisionKey, advertiser: advertiser),
              )),
            ),
          ),
        ],
      ),
    );
  }
}
```

Run: `flutter analyze && flutter test` — expected `No issues found!`, `All tests passed!`.

- [ ] **Step 2: End-to-end on hardware — provisioning**

MFS_1 must be running Task 9's firmware, with RTT open. The Mac's clock must be network-synced (`sntp -d time.apple.com` shows the offset).

```bash
cd /Users/andy/nordic/ncs/v3.2.4/class_app && flutter run -d macos
```

1. Pick the device → **clock icon** → **Send time sync**. Within 30 s: `Clock set by provisioner: unix ..., day ..., HH:MM UTC.` HH:MM must match the Mac's UTC time.
2. Press **Send time sync** again. Expected: **nothing** in RTT — the clock is valid, so the candidate goes to AccessControl and is simply not for us.

- [ ] **Step 3: End-to-end on hardware — commands**

3. Open the MFS_1 screen, leave **Armed** off, move the sliders, **Send**. Within 30 s:
   `Command slot 1 n 0: arm INACTIVE, delay ... s, activations ..., mode 0, cooldown ... s, threshold ... LSB, minute ....`
   **Every value must match the app's readouts exactly**, and `minute` must be the current UTC minute. A mismatch is a protocol bug caught at the cheapest moment.
4. **Send** again without changing anything: `n 1`. The same settings produce new bytes.
5. Toggle **Armed**, **Send**: `arm ACTIVE`, then `Arm state: Active`.
6. **Send as Network Manager** with Report selected: `Command slot 0 ...: mode 2`.
7. Reset the board (`nrfutil device reset --serial-number 853003346`). Expected: `Access state restored: day floor <today>` and `Clock INVALID`. **Send** a command: nothing logged. Provision, then **Send**: accepted, and `n` continues from where it was — the phone never reuses a number.

Record each log line in the commit message.

- [ ] **Step 4: Measure the iPhone advertising interval**

This closes spec §9 item 2. In `src/command_scanner.cpp`, **temporarily** add `LOG_INF("UUID seen at %lld ms.", k_uptime_get());` as the first line of `parseAdStructure()` inside the `M_AD_UUID128_ALL` check, rebuild and flash. Run `flutter run -d <iphone-device-id>`, Send once, and take the differences between consecutive timestamps. **Revert the log line** and reflash.

If the interval differs materially from 187 ms, update `kAdvertiseWindow` in `lib/services/advertiser.dart` and the detection table in spec §3.

- [ ] **Step 5: Commit**

```bash
cd /Users/andy/nordic/ncs/v3.2.4/class_app
git add -A && git status --short | grep bench_credentials && echo "STOP"
git commit -m "Add the MFS_1 control and provisioner screens

Every Send is the same operation - reserve the next sequence number, seal, advertise
for 30 s - so there are no codes to pick and no test code. The mode lives in a
visibly separate Network Manager section because it sends a slot-0 command.

The screen tells the engineer what each LED A pattern means and that no flash
means the command did not land.

Verified end to end on MFS_1: <paste the RTT lines from steps 2-3>.
iPhone advertising interval: <measured> ms."
```


---

# PHASE 4 — Device behaviour

## Task 14: The detection engine

**Files:**
- Modify: `src/app.hpp`, `src/app.cpp`, `CMakeLists.txt`

**Interfaces:**
- Consumes: `Settings` (Task 7), the Task 9 App, `Npm2100` timer API.
- Produces: `App::m_settings`, `App::m_detection_met`, `App::m_activation_count`, `App::m_in_cooldown`, `App::m_previous_awake`, `App::beginCooldown()`, `App::serviceCooldown()`. `IsOutputActive()` semantics unchanged. Tasks 15–16 build on these.

> **Note on GPIO0.** The probe proved `GpioUsage::InterruptHi` asserts P1.06 high on expiry. Polling `TimerIsExpired()` over I²C from the existing 100 ms loop is used here rather than a GPIO interrupt: the loop already runs at that cadence and polling adds no new interrupt path.

- [ ] **Step 1: Add the settings source to the build**

In `CMakeLists.txt`, add `src/settings.cpp` after `src/device_clock.cpp` in `target_sources`.

- [ ] **Step 2: Apply the engine**

1. In `src/app.hpp`, replace:

```cpp
#include "output_switch.hpp"
```

   with:

```cpp
#include "output_switch.hpp"
#include "settings.hpp"
```

2. In `src/app.hpp`, replace:

```cpp
      void setArmState(ArmState state);
```

   with:

```cpp
      void setArmState(ArmState state);

      // Stands the ADXL367 down and starts the PMIC timer for the cooldown between
      // counted activations. No-op when the cooldown is zero.
      int beginCooldown();

      // Polls the PMIC timer; on expiry re-arms the ADXL367 through the full
      // bootstrap so the engine cannot inherit a level from the blanking window.
      void serviceCooldown();
```

3. In `src/app.hpp`, replace:

```cpp
      // Consecutive loop ticks with the ADXL awake, for the stuck-AWAKE watchdog.
      uint32_t m_awake_ticks;
```

   with:

```cpp
      // Consecutive loop ticks with the ADXL awake, for the stuck-AWAKE watchdog.
      uint32_t m_awake_ticks;

      // The engineer-settable parameters, NVS-backed.
      Settings m_settings;

      // Latched by the detection engine when the activation count reaches the
      // configured threshold; cleared when AWAKE de-asserts. NOT derived in
      // updateOutputState() - the engine zeroes the count when it latches, so
      // deriving this from the count would take the output false immediately.
      bool m_detection_met;

      // Activations seen since the last trigger or deactivation. Does not expire.
      uint8_t m_activation_count;

      // True while the ADXL is standing down for a cooldown window.
      bool m_in_cooldown;

      // Previous INT1 level, for edge detection. The engine counts RISING edges,
      // not levels - a level would count the same activation on every loop tick.
      bool m_previous_awake;
```

4. In `src/app.cpp`, replace:

```cpp
      , m_awake_ticks(0)
      , m_initialised(false)
```

   with:

```cpp
      , m_awake_ticks(0)
      , m_settings()
      , m_detection_met(false)
      , m_activation_count(0)
      , m_in_cooldown(false)
      , m_previous_awake(false)
      , m_initialised(false)
```

5. In `src/app.cpp`, replace:

```cpp
    result = initAccess();
    if (result < 0) { LOG_ERR("Access control unavailable (%d) - commands will be ignored this boot!", result); }
```

   with:

```cpp
    result = initAccess();
    if (result < 0) { LOG_ERR("Access control unavailable (%d) - commands will be ignored this boot!", result); }

    // After initAccess(), which initialises the settings subsystem. Defaults stand
    // if nothing is stored or the record is invalid.
    result = m_settings.Load();
    if (result < 0) { LOG_WRN("Settings not loaded (%d) - using defaults.", result); }
    LOG_INF("Settings: %u activations, %u s cooldown, %u LSB, %u s delay, mode %u.", m_settings.Activations(), m_settings.CooldownSeconds(),
            m_settings.ThresholdLsb(), m_settings.DelaySeconds(), static_cast<unsigned>(m_settings.OperatingMode()));
```

6. In `src/app.cpp`, replace:

```cpp
      // The ONE place the output state is derived. See updateOutputState().
      updateOutputState();
```

   with:

```cpp
      serviceCooldown();

      // The ONE place the output state is derived. See updateOutputState().
      updateOutputState();
```

7. In `src/app.cpp`, replace:

```cpp
      // LED B is a CONSUMER of the output state, exactly like the future voltage
      // switch will be. It does not re-derive the condition.
      ledB = IsOutputActive();
```

   with:

```cpp
      // LED B shows DETECTION, in either arm state, for the 5 s ADXL loop period.
      // Inactive it simulates triggers while tuning; Active it confirms one. It is
      // a bench indicator, not an output consumer - OutputSwitch is the example
      // future consumers copy (design spec section 6.2).
      ledB = m_detection_met;
```

8. In `src/app.cpp`, replace:

```cpp
    result = m_accelerometer.ConfigureLoopMode(CONFIG_MFS_ADXL_THRESHOLD, CONFIG_MFS_ADXL_ACTIVITY_SAMPLES, CONFIG_MFS_ADXL_INACTIVITY_THRESHOLD,
                                               CONFIG_MFS_ADXL_INACTIVITY_SECS);
```

   with:

```cpp
    result = m_accelerometer.ConfigureLoopMode(m_settings.ThresholdLsb(), CONFIG_MFS_ADXL_ACTIVITY_SAMPLES, CONFIG_MFS_ADXL_INACTIVITY_THRESHOLD,
                                               CONFIG_MFS_ADXL_INACTIVITY_SECS);
```

9. In `src/app.cpp`, replace:

```cpp
    m_output_active = (m_arm_state == ArmState::Active) && awake && !m_ignore_stale_trigger;
```

   with:

```cpp
    // RISING EDGES, not levels. AWAKE stays asserted for the whole inactivity
    // period, so counting the level would add one activation per loop tick.
    bool risingEdge { awake && !m_previous_awake && !m_ignore_stale_trigger && !m_in_cooldown };
    m_previous_awake = awake;

    if (risingEdge) {
      m_activation_count++;
      LOG_INF("Activation %u of %u.", m_activation_count, m_settings.Activations());

      if (m_activation_count >= m_settings.Activations()) {
        m_detection_met    = true;
        m_activation_count = 0;
        // No blanking here. Standing the ADXL down at the moment of trigger
        // would cut short the assertion that IS the output's 5 s duration.
      } else {
        beginCooldown();
      }
    }

    // The trigger's own AWAKE running to completion is what clears detection.
    if (m_detection_met && !awake) { m_detection_met = false; }

    m_output_active = (m_arm_state == ArmState::Active) && m_detection_met;
```

10. In `src/app.cpp`, replace:

```cpp
        if (m_accelerometer.ConfigureLoopMode(CONFIG_MFS_ADXL_THRESHOLD, CONFIG_MFS_ADXL_ACTIVITY_SAMPLES, CONFIG_MFS_ADXL_INACTIVITY_THRESHOLD,
                                              CONFIG_MFS_ADXL_INACTIVITY_SECS) < 0) {
```

   with:

```cpp
        if (m_accelerometer.ConfigureLoopMode(m_settings.ThresholdLsb(), CONFIG_MFS_ADXL_ACTIVITY_SAMPLES, CONFIG_MFS_ADXL_INACTIVITY_THRESHOLD,
                                              CONFIG_MFS_ADXL_INACTIVITY_SECS) < 0) {
```

11. In `src/app.cpp`, replace:

```cpp
  int App::initAccess()
  {
```

   with:

```cpp
  int App::beginCooldown()
  {
    uint16_t seconds { m_settings.CooldownSeconds() };
    int result { 0 };

    if (seconds == 0) { return 0; }

    // Stand the accelerometer down for the window. Leaving it running would let
    // a continuous disturbance hold AWAKE asserted right through the blanking
    // period, so the re-arm would inherit a stale level - exactly the bug commit
    // 0a50910 fixed for the arming path.
    result = m_accelerometer.Standby();
    if (result < 0) {
      LOG_ERR("Failed to stand the ADXL down for cooldown: %d!", result);
      return result;
    }

    result = m_pmic.TimerStop();
    if (result == 0) { result = m_pmic.TimerSetMode(Npm2100::TimerMode::GeneralPurpose); }
    if (result == 0) { result = m_pmic.TimerSetDurationMs(static_cast<uint32_t>(seconds) * MSEC_PER_SEC); }
    if (result == 0) { result = m_pmic.TimerClearExpiredEvent(); }
    if (result == 0) { result = m_pmic.TimerStart(); }
    if (result < 0) {
      LOG_ERR("Failed to start the cooldown timer: %d!", result);
      return result;
    }

    m_in_cooldown = true;
    LOG_INF("Cooldown started: %u s.", seconds);
    return 0;
  }

  void App::serviceCooldown()
  {
    bool expired { false };

    if (!m_in_cooldown) { return; }
    if (m_pmic.TimerIsExpired(expired) < 0 || !expired) { return; }

    m_pmic.TimerClearExpiredEvent();
    m_in_cooldown = false;

    // Full bootstrap, not a bare restart. Re-arming must confirm AWAKE is clear
    // so the engine cannot inherit an assertion from during the blanking window.
    if (enableAccelerometer() < 0) {
      LOG_ERR("Failed to re-arm the ADXL after cooldown!");
      return;
    }

    m_previous_awake = false;
    LOG_INF("Cooldown elapsed - detection re-armed.");
  }

  int App::initAccess()
  {
```

12. In `src/app.cpp`, replace:

```cpp
    // PHASE 2 SHIM. Applies only the arm bit so the device stays usable while the
    // app is built against it. Settings, the clock trim and the LED patterns
    // arrive in Task 16, which replaces this.
    setArmState(evaluation.command.armActive ? ArmState::Active : ArmState::Inactive);
```

   with:

```cpp
    // PHASE 2 SHIM, extended in Task 14 to carry the settings so the detection
    // engine can be bench-tested from the app. The clock trim, re-arm ordering and
    // LED patterns arrive in Task 16, which replaces this.
    m_settings.ApplyFrom(evaluation.command, evaluation.slot == access::M_SLOT_NETWORK_MANAGER);
    setArmState(evaluation.command.armActive ? ArmState::Active : ArmState::Inactive);
```

- [ ] **Step 3: Build and bench-test**

```bash
west build -b nrf54l15dk/nrf54l05/cpuapp -p always -- -DEXTRA_CONF_FILE=credentials.conf
west flash --dev-id 853003346 --recover
```

Provision the clock from the app. Set **activations = 3, cooldown = 8 s (byte 66)**, toggle **Armed**, **Send**. Tap the device three times with pauses. Expected:

```
<inf> app: Activation 1 of 3.
<inf> app: Cooldown started: 8 s.
<inf> app: Cooldown elapsed - detection re-armed.
<inf> app: Activation 2 of 3.
...
<inf> app: Activation 3 of 3.
<inf> app: Output ASSERTED. Arm Active, LED A off, LED B ON.
```

**Verify that tapping during the cooldown does not increment the count** — that is the whole point of the blanking window. Power-cycle and confirm the boot log's `Settings:` line shows the values you sent, which proves NVS.

- [ ] **Step 4: Commit**

```bash
/Users/andy/nrfenv/bin/clang-format -i src/app.cpp src/app.hpp
git add src/app.cpp src/app.hpp CMakeLists.txt
git commit -m "Add activation counting and the cooldown blanking window

Counts RISING edges of AWAKE, not levels. AWAKE stays asserted for the whole
inactivity period, so counting the level would add one activation per loop tick.

Blanking never applies after the triggering activation - standing the ADXL down
at that moment would cut short the assertion that IS the output's 5 s duration.
Between counted activations the part is stood down for the window rather than
left running, because a continuous disturbance would otherwise hold AWAKE right
through the blanking period and the re-arm would inherit a stale level.

The threshold now comes from the NVS-backed settings. The count does not expire.
LED B shows detection in either arm state; OutputSwitch is the consumer future
code copies."
```

---

## Task 15: The trigger delay and its interlock — SAFETY CRITICAL

**A trigger that fires after the engineer deactivated the device is the worst failure this product has.** With a delay of up to nine hours, that window is enormous. The delay and the doubled guard land together — the delay without the interlock is the hazard.

**Files:**
- Modify: `src/output_switch.hpp`, `src/output_switch.cpp`, `src/command_scanner.hpp`, `src/command_scanner.cpp`, `src/app.hpp`, `src/app.cpp`

**Interfaces:**
- Consumes: `Settings::DelaySeconds()`, the Task 14 engine.
- Produces: `OutputSwitch::InterlockFn`, `OutputSwitch::SetInterlock(InterlockFn, void*)`; `CommandScanner::SetFastScan(bool) -> int`; `App::delayPermitsFiring() const`, `App::interlockThunk(void*)`, `App::beginDelay()`, `App::cancelDelay()`, `App::m_delay_pending`, `App::m_delay_timer`, `App::m_detection_hold_until_ms`. Task 16 reads `m_delay_pending`.

**A bug in the 2026-09-12 version of this task is fixed here.** When a delay expired it set `m_detection_met = true`, but the ADXL367's AWAKE had cleared hours earlier, so the very next line cleared detection again and **the output would never have asserted**. Delayed triggers now hold detection for the same 5 s (`M_DELAYED_TRIGGER_HOLD_MS`) an undelayed trigger gets from its own AWAKE.

- [ ] **Step 1: Add the interlock to OutputSwitch**

`OutputSwitch` must **not** learn what a delay is — coupling a general containment to one feature destroys its value. It gains a predicate instead.

1. In `src/output_switch.hpp`, replace:

```cpp
      /** @brief Whether the output is currently energised. */
```

   with:

```cpp
      /**
       * @brief Predicate consulted immediately before the gates are driven high.
       *
       * Returning false REFUSES the assertion. This is a second, independent
       * layer: the caller is expected to have already declined to ask. Reaching
       * a refusal therefore means the first layer failed, which is a bug rather
       * than a routine condition, and is treated as one.
       */
      using InterlockFn = bool (*)(void* context);

      /** @brief Install the interlock. Passing nullptr removes it. */
      void SetInterlock(InterlockFn interlock, void* context);

      /** @brief Whether the output is currently energised. */
```

2. In `src/output_switch.hpp`, replace:

```cpp
      bool m_readback_supported;
  };
```

   with:

```cpp
      bool m_readback_supported;
      InterlockFn m_interlock;
      void* m_interlock_context;
  };
```

3. In `src/output_switch.cpp`, replace:

```cpp
      , m_readback_supported(false)
  {}
```

   with:

```cpp
      , m_readback_supported(false)
      , m_interlock(nullptr)
      , m_interlock_context(nullptr)
  {}

  void OutputSwitch::SetInterlock(InterlockFn interlock, void* context)
  {
    m_interlock         = interlock;
    m_interlock_context = context;
  }
```

4. In `src/output_switch.cpp`, replace:

```cpp
    if (!IsUsable()) {
      LOG_ERR("Refusing to assert the fire output: switch is %s!", m_faulted ? "faulted" : "not initialised");
      driveBoth(false);
      return -EPERM;
    }
```

   with:

```cpp
    if (!IsUsable()) {
      LOG_ERR("Refusing to assert the fire output: switch is %s!", m_faulted ? "faulted" : "not initialised");
      driveBoth(false);
      return -EPERM;
    }

    // SECOND LAYER. The caller should already have declined to ask, so being
    // refused here means the first layer failed - a bug, not a routine
    // condition. Latch faulty and say so loudly rather than quietly declining.
    if (m_interlock != nullptr && !m_interlock(m_interlock_context)) {
      driveBoth(false);
      enterFaultState("interlock refused the assertion - a caller bypassed the derivation point", -EPERM);
      return -EPERM;
    }
```

- [ ] **Step 2: Add fast scan to CommandScanner**

1. In `src/command_scanner.hpp`, replace:

```cpp
      bool TakeCandidate(Candidate& out);

    private:
      bool m_started;
```

   with:

```cpp
      bool TakeCandidate(Candidate& out);

      /**
       * @brief Switch between the duty-cycled scan and a continuous one.
       *
       * Continuous while a trigger is pending, so a deactivate is heard within one
       * advert rather than ~30 s. Battery life is explicitly not a factor then.
       */
      int SetFastScan(bool fast);

    private:
      bool m_started;
      bool m_fast;
```

2. In `src/command_scanner.cpp`, replace:

```cpp
      : m_started(false)
  {}
```

   with:

```cpp
      : m_started(false)
      , m_fast(false)
  {}
```

3. In `src/command_scanner.cpp`, replace:

```cpp
  bool CommandScanner::TakeCandidate(Candidate& out)
```

   with:

```cpp
  int CommandScanner::SetFastScan(bool fast)
  {
    int result { 0 };

    const struct bt_le_scan_param scanParam {
      .type     = BT_LE_SCAN_TYPE_PASSIVE,
      .options  = BT_LE_SCAN_OPT_NONE,
      .interval = fast ? M_SCAN_WINDOW_UNITS : M_SCAN_INTERVAL_UNITS,
      .window   = M_SCAN_WINDOW_UNITS,
    };

    if (!m_started || fast == m_fast) { return 0; }

    bt_le_scan_stop();
    result = bt_le_scan_start(&scanParam, &scanRecvCallback);
    if (result < 0) {
      LOG_ERR("Failed to change scan cadence: %d!", result);
      return result;
    }

    m_fast = fast;
    LOG_INF("Scan cadence now %s.", fast ? "CONTINUOUS (trigger pending)" : "duty-cycled");
    return 0;
  }

  bool CommandScanner::TakeCandidate(Candidate& out)
```

- [ ] **Step 3: Add the delay, both witnesses and the cancellation to App**

1. In `src/app.hpp`, replace:

```cpp
#include <zephyr/device.h>
```

   with:

```cpp
#include <zephyr/device.h>
#include <zephyr/kernel.h>
```

2. In `src/app.hpp`, replace:

```cpp
      void serviceCooldown();
```

   with:

```cpp
      void serviceCooldown();

      /**
       * @brief True only when BOTH witnesses agree no delay is running.
       *
       * A flag left set with a dead timer blocks firing; a running timer with a
       * cleared flag also blocks firing. Both failure directions are safe, which
       * is the whole reason for using two witnesses of different kinds.
       */
      bool delayPermitsFiring() const;

      // Installed into OutputSwitch as the second, independent layer.
      static bool interlockThunk(void* context);

      void beginDelay();
      void cancelDelay();
```

3. In `src/app.hpp`, replace:

```cpp
      bool m_previous_awake;
```

   with:

```cpp
      bool m_previous_awake;

      // The delay's two independent witnesses. Deliberately different in kind so
      // that either being wrong still BLOCKS firing - see delayPermitsFiring().
      bool m_delay_pending;
      struct k_timer m_delay_timer;

      // Held for the whole delay so the SoC cannot enter a deeper state. Battery
      // life is explicitly not a factor while a trigger is pending.
      bool m_delay_pm_lock_held;
```

4. In `src/app.cpp`, replace:

```cpp
#include <zephyr/logging/log.h>
```

   with:

```cpp
#include <zephyr/logging/log.h>
#include <zephyr/pm/policy.h>
```

5. In `src/app.cpp`, replace:

```cpp
      , m_previous_awake(false)
      , m_initialised(false)
```

   with:

```cpp
      , m_previous_awake(false)
      , m_delay_pending(false)
      , m_delay_timer {}
      , m_delay_pm_lock_held(false)
      , m_initialised(false)
```

6. In `src/app.cpp`, replace:

```cpp
    LOG_INF("MFS_1 starting, serial %s.", CONFIG_ALC_DEVICE_SERIAL);
```

   with:

```cpp
    LOG_INF("MFS_1 starting, serial %s.", CONFIG_ALC_DEVICE_SERIAL);

    k_timer_init(&m_delay_timer, nullptr, nullptr);
```

7. In `src/app.cpp`, replace:

```cpp
    result = m_output_switch.Init();
    if (result < 0) {
      LOG_ERR("Failed to initialise the fire output: %d!", result);
      return result;
    }
```

   with:

```cpp
    result = m_output_switch.Init();
    if (result < 0) {
      LOG_ERR("Failed to initialise the fire output: %d!", result);
      return result;
    }

    // Installed before anything can ask the switch to assert.
    m_output_switch.SetInterlock(&App::interlockThunk, this);
```

8. In `src/app.cpp`, replace:

```cpp
      if (m_activation_count >= m_settings.Activations()) {
        m_detection_met    = true;
        m_activation_count = 0;
        // No blanking here. Standing the ADXL down at the moment of trigger
        // would cut short the assertion that IS the output's 5 s duration.
      } else {
        beginCooldown();
      }
    }
```

   with:

```cpp
      if (m_activation_count >= m_settings.Activations()) {
        m_activation_count = 0;
        // No blanking here. Standing the ADXL down at the moment of trigger
        // would cut short the assertion that IS the output's 5 s duration.
        if (m_settings.DelaySeconds() > 0) {
          beginDelay(); // m_detection_met waits for the timer
        } else {
          m_detection_met = true;
        }
      } else {
        beginCooldown();
      }
    }

    // The delay elapsed and was not cancelled - commit the trigger.
    if (m_delay_pending && k_timer_remaining_ticks(&m_delay_timer) == 0) {
      cancelDelay(); // clears the flag and releases the PM lock
      m_detection_met = true;

      // The delay outlived the AWAKE that started it, so AWAKE is already clear
      // and would end detection on this very tick - the output would never
      // assert. Hold detection for the same 5 s the loop period gives an
      // undelayed trigger.
      m_detection_hold_until_ms = k_uptime_get() + M_DELAYED_TRIGGER_HOLD_MS;
    }
```

9. In `src/app.cpp`, replace:

```cpp
    m_output_active = (m_arm_state == ArmState::Active) && m_detection_met;
```

   with:

```cpp
    // Activations during a pending delay are ignored: the trigger is already
    // committed, and re-counting would let a continuing disturbance postpone or
    // duplicate it. LAYER ONE of the delay interlock is the last term.
    m_output_active = (m_arm_state == ArmState::Active) && m_detection_met && delayPermitsFiring();
```

10. In `src/app.cpp`, replace:

```cpp
    bool risingEdge { awake && !m_previous_awake && !m_ignore_stale_trigger && !m_in_cooldown };
```

   with:

```cpp
    bool risingEdge { awake && !m_previous_awake && !m_ignore_stale_trigger && !m_in_cooldown && !m_delay_pending };
```

11. In `src/app.cpp`, replace:

```cpp
    } else {
      // Boolean first, sensor second - see disableAccelerometer().
      m_arm_state = ArmState::Inactive;
      disableAccelerometer();
    }
```

   with:

```cpp
    } else {
      // UNCONDITIONAL, and before anything else. A pending trigger must not
      // outlive disarming. The pending state is also deliberately not persisted,
      // so a reset loses the trigger too - the fail-safe direction.
      cancelDelay();
      m_detection_met    = false;
      m_activation_count = 0;

      // Boolean first, sensor second - see disableAccelerometer().
      m_arm_state = ArmState::Inactive;
      disableAccelerometer();
    }
```

12. In `src/app.cpp`, replace:

```cpp
  int App::initAccess()
  {
```

   with:

```cpp
  bool App::delayPermitsFiring() const
  {
    // BOTH must agree. Not one, not either - both.
    return !m_delay_pending && k_timer_remaining_ticks(&m_delay_timer) == 0;
  }

  bool App::interlockThunk(void* context)
  {
    return static_cast<const App*>(context)->delayPermitsFiring();
  }

  void App::beginDelay()
  {
    uint16_t seconds { m_settings.DelaySeconds() };

    if (seconds == 0) { return; }

    // GRTC, not the PMIC timer. At +/-10% over temperature the PMIC would put a
    // 9-hour delay anywhere inside a 108-minute window.
    k_timer_start(&m_delay_timer, K_SECONDS(seconds), K_NO_WAIT);
    m_delay_pending = true;

    // Stay awake for the duration and scan continuously. The deactivate path is
    // the most important thing the device does while a trigger is pending, and at
    // the normal 6 s cadence an abort takes ~30 s to be heard with confidence.
    if (!m_delay_pm_lock_held) {
      pm_policy_state_lock_get(PM_STATE_SUSPEND_TO_IDLE, PM_ALL_SUBSTATES);
      m_delay_pm_lock_held = true;
    }
    m_scanner.SetFastScan(true);

    LOG_WRN("TRIGGER PENDING: firing in %u s. Deactivating cancels it.", seconds);
  }

  void App::cancelDelay()
  {
    k_timer_stop(&m_delay_timer);
    m_delay_pending = false;

    if (m_delay_pm_lock_held) {
      pm_policy_state_lock_put(PM_STATE_SUSPEND_TO_IDLE, PM_ALL_SUBSTATES);
      m_delay_pm_lock_held = false;
    }
    m_scanner.SetFastScan(false);
  }

  int App::initAccess()
  {
```

13. In `src/app.cpp`, replace:

```cpp
    // The trigger's own AWAKE running to completion is what clears detection.
    if (m_detection_met && !awake) { m_detection_met = false; }
```

   with:

```cpp
    // The trigger's own AWAKE running to completion is what clears detection -
    // or, for a delayed trigger, the hold set when the delay expired.
    if (m_detection_met && !awake && k_uptime_get() >= m_detection_hold_until_ms) { m_detection_met = false; }
```

14. In `src/app.hpp`, replace:

```cpp
      bool m_delay_pm_lock_held;
```

   with:

```cpp
      bool m_delay_pm_lock_held;

      // Uptime until which a DELAYED trigger holds detection regardless of AWAKE.
      // Zero for an undelayed trigger, whose own AWAKE sets the duration.
      int64_t m_detection_hold_until_ms;
```

15. In `src/app.cpp`, replace:

```cpp
      , m_delay_pm_lock_held(false)
      , m_initialised(false)
```

   with:

```cpp
      , m_delay_pm_lock_held(false)
      , m_detection_hold_until_ms(0)
      , m_initialised(false)
```

16. In `src/app.cpp`, replace:

```cpp
    constexpr uint32_t M_AWAKE_STUCK_TICKS { (CONFIG_MFS_ADXL_INACTIVITY_SECS * 10U * 6U) };
```

   with:

```cpp
    constexpr uint32_t M_AWAKE_STUCK_TICKS { (CONFIG_MFS_ADXL_INACTIVITY_SECS * 10U * 6U) };

    // How long a delayed trigger asserts: the ADXL loop period an undelayed
    // trigger gets from its own AWAKE.
    constexpr int64_t M_DELAYED_TRIGGER_HOLD_MS { CONFIG_MFS_ADXL_INACTIVITY_SECS * MSEC_PER_SEC };
```

- [ ] **Step 4: Bench-verify the abort path — this is the point of the task**

```bash
west build -b nrf54l15dk/nrf54l05/cpuapp -p always -- -DEXTRA_CONF_FILE=credentials.conf
west flash --dev-id 853003346 --recover
```

Provision the clock. Then:

1. Set **activations = 1, delay = 30 s**, arm. Trigger it. Expect `TRIGGER PENDING: firing in 30 s. Deactivating cancels it.` and `Scan cadence now CONTINUOUS (trigger pending).`
2. **Wait the full 30 s.** Confirm `Output ASSERTED`, LED B lit **for about 5 s**, then `Output cleared`. (This is the step the old version would have failed.)
3. Repeat, but **deactivate at ~15 s**. Confirm the arm state goes Inactive, **the output NEVER asserts, then or later**, and the scan returns to duty-cycled.
4. Repeat, and **reset the board mid-delay**. Confirm it comes up Inactive, clock invalid, no pending trigger.
5. Set delay = 0 and confirm firing is still immediate.

**Step 3 is the one that matters. If the output asserts there, stop and fix it before going further.**

- [ ] **Step 5: Commit**

```bash
/Users/andy/nrfenv/bin/clang-format -i src/app.cpp src/app.hpp src/output_switch.cpp src/output_switch.hpp src/command_scanner.cpp src/command_scanner.hpp
git add src/app.cpp src/app.hpp src/output_switch.cpp src/output_switch.hpp src/command_scanner.cpp src/command_scanner.hpp
git commit -m "Add the trigger delay and its doubled interlock

A trigger firing after the engineer deactivated the device is the worst failure
this product has, and a delay of up to nine hours makes that window enormous. The
delay and the guard land together on purpose.

Two witnesses must both agree no delay is pending before the output can assert,
deliberately different in kind - a software flag and the kernel timer. Either
being wrong blocks firing. OutputSwitch is not taught what a delay is; it gains an
interlock predicate, giving two independent layers, and a refusal at the second
layer latches it faulty because reaching it means the first layer failed.

A delayed trigger holds detection for the 5 s loop period. Without the hold the
AWAKE that started the delay has long cleared, detection would end on the same
tick, and the output would never assert - a bug in the previous plan.

Deactivating cancels unconditionally; the pending state is not persisted, so a
reset loses the trigger too. The SoC holds a PM lock and scans continuously for
the whole delay.

Verified on hardware: <results of steps 1-5, in particular step 3>."
```

---

## Task 16: Command handling, the single armed path, and LED A

**Decided 2026-09-13: an armed device does exactly one thing on command — disarm.** There is one path out of the armed state on command, and only two ways out at all: **disarm** or **trigger**. A trigger is **one-shot**: once the output period ends the device latches Inactive, and re-arming needs an engineer. (Power loss also leaves it Inactive, because cold start is Inactive.)

The rule lives in one pure, host-tested function, `DecideCommand()`, and `App::applyCommand()` acts on its decision and nothing else.

**Files:**
- Create: `src/arm_policy.hpp`, `tests/test_arm_policy.cpp`
- Modify: `src/app.hpp`, `src/app.cpp`, `CMakeLists.txt`, `tests/test_main.cpp`

**Interfaces:**
- Consumes: `LedSequencer` (Task 6), `AccessControl::Evaluation`, `DeviceClock::ApplyMinuteHint`, the Task 14–15 engine and delay.
- Produces: `enum class ArmAction { Ignore, Disarm, Arm, Tune }`, `struct ArmDecision { action; applySettings; applyMode; trimClock; }`, `ArmDecision DecideCommand(bool armed, bool fromNetworkManager, const protocol::Command&)`; `App::applyCommand(...)`, `App::playLedPattern(LedPattern)`, `App::ledTimerHandler(struct k_timer*)`, `App::m_trigger_fired`, `App::m_trigger_complete`.

| Device is | Command | Action | Settings | Mode | Clock trim | LED A |
|---|---|---|---|---|---|---|
| **Active** | arm bit set | **Ignore — nothing at all** | no | no | no | none |
| **Active** | arm bit clear | **Disarm only** (cancels any delay) | **no** | **no** | yes | Disarmed, or Delay Cancelled |
| Inactive | arm bit set | Arm: configure, confirm AWAKE clear, Active | yes | slot 0 only | yes | Armed, or Arm Refused |
| Inactive | arm bit clear | Tune: (re)start the engine | yes | slot 0 only | yes | Settings Applied, or Mode Changed |
| **Active** | — (output period ends) | **Latch Inactive** | — | — | — | none |

An ignored command has still consumed its sequence number (AccessControl persisted it before App saw it), which is harmless. **LED B simulates triggers while Inactive only after a Tune Send**, because disarming stands the ADXL367 down as the arming invariant requires.

- [ ] **Step 1: Write the failing policy test**

Create `tests/test_arm_policy.cpp`:

```cpp
#include <cassert>
#include <cstdio>

#include "arm_policy.hpp"

void run_arm_policy_tests()
{
  using namespace alc;

  protocol::Command armCommand;
  protocol::Command disarmCommand;
  ArmDecision decision;
  const bool M_BOTH_SLOT_KINDS[] { false, true };

  armCommand.armActive    = true;
  armCommand.activations  = 5;
  armCommand.delayCode    = 127;
  armCommand.mode         = protocol::Mode::ReportOnly;
  disarmCommand           = armCommand;
  disarmCommand.armActive = false;

  // ARMED + anything but disarm: NOTHING. Not even from the Network Manager, not
  // even a clock trim. There is one path out of the armed state on command.
  for (bool fromNetworkManager : M_BOTH_SLOT_KINDS) {
    decision = DecideCommand(true, fromNetworkManager, armCommand);
    assert(decision.action == ArmAction::Ignore);
    assert(!decision.applySettings && !decision.applyMode && !decision.trimClock);
  }

  // ARMED + disarm: disarm, and nothing else. The settings, delay and mode the
  // command carries are not applied - from any slot.
  for (bool fromNetworkManager : M_BOTH_SLOT_KINDS) {
    decision = DecideCommand(true, fromNetworkManager, disarmCommand);
    assert(decision.action == ArmAction::Disarm);
    assert(!decision.applySettings && !decision.applyMode);
    assert(decision.trimClock);
  }

  // INACTIVE + arm: arm with the command's settings; mode only from slot 0.
  decision = DecideCommand(false, false, armCommand);
  assert(decision.action == ArmAction::Arm && decision.applySettings && !decision.applyMode && decision.trimClock);
  decision = DecideCommand(false, true, armCommand);
  assert(decision.action == ArmAction::Arm && decision.applyMode);

  // INACTIVE + disarm bit: tune.
  decision = DecideCommand(false, false, disarmCommand);
  assert(decision.action == ArmAction::Tune && decision.applySettings && !decision.applyMode);
  decision = DecideCommand(false, true, disarmCommand);
  assert(decision.action == ArmAction::Tune && decision.applyMode);

  printf("arm policy: OK\n");
}
```

Declare and call `run_arm_policy_tests();` in `tests/test_main.cpp`.

Run: `make test`
Expected: FAIL — `'arm_policy.hpp' file not found`.

- [ ] **Step 2: Implement the policy**

Create `src/arm_policy.hpp`:

```cpp
#pragma once

#include <cstdint>

#include "mfs_protocol.hpp"

namespace alc
{

  /** @brief What an accepted command is allowed to do. */
  enum class ArmAction : uint8_t {
    Ignore, ///< Armed, and the command does not disarm. NOTHING happens.
    Disarm, ///< Armed -> Inactive. The only thing an armed device will do on command.
    Arm,    ///< Inactive -> Active with the command's settings.
    Tune,   ///< Inactive stays Inactive; settings applied for tuning.
  };

  struct ArmDecision
  {
      ArmAction action { ArmAction::Ignore };
      bool applySettings { false };
      bool applyMode { false };
      bool trimClock { false };
  };

  /**
   * @brief THE SINGLE PATH. Decides what an accepted command may do. Pure.
   *
   * **An armed device does exactly one thing on command: disarm.** A command with
   * the arm bit set is ignored outright - no settings, no mode, no clock trim,
   * no re-arm. A disarm applies nothing but the disarm: the settings, delay and
   * mode it carries are ignored, and the engineer sends settings once Inactive.
   *
   * An armed device therefore leaves the armed state only two ways: a disarm
   * command, or firing (App latches Inactive when the output period ends). Power
   * loss also leaves it Inactive, because cold start is Inactive.
   *
   * App::applyCommand() must act on this decision and on nothing else.
   *
   * @param armed              Whether the device is Active now.
   * @param fromNetworkManager Whether the command came from slot 0.
   */
  inline ArmDecision DecideCommand(bool armed, bool fromNetworkManager, const protocol::Command& command)
  {
    ArmDecision decision {};

    if (armed) {
      if (command.armActive) { return decision; }

      // The clock trim is the only side effect a disarm keeps. It changes no
      // device behaviour, and the command is authentic and fresh.
      decision.action    = ArmAction::Disarm;
      decision.trimClock = true;
      return decision;
    }

    decision.action        = command.armActive ? ArmAction::Arm : ArmAction::Tune;
    decision.applySettings = true;
    decision.applyMode     = fromNetworkManager;
    decision.trimClock     = true;
    return decision;
  }

}
```

Run: `make test`
Expected: `arm policy: OK` and `ALL TESTS PASSED`.

- [ ] **Step 3: Add the sequencer source to the build**

In `CMakeLists.txt`, add `src/led_sequencer.cpp` after `src/settings.cpp`.

- [ ] **Step 4: Replace the shim with the real handling and the one-shot latch**

1. In `src/app.hpp`, replace:

```cpp
#include "device_clock.hpp"
```

   with:

```cpp
#include "arm_policy.hpp"
#include "device_clock.hpp"
#include "led_sequencer.hpp"
```

2. In `src/app.hpp`, replace:

```cpp
      void handleCommandCandidate(const CommandScanner::Candidate& candidate, int64_t uptimeSecs);
```

   with:

```cpp
      void handleCommandCandidate(const CommandScanner::Candidate& candidate, int64_t uptimeSecs);

      // Carries out an ACCEPTED command: arm transitions, settings, clock trim and
      // the LED A acknowledgement. Called only after AccessControl has persisted
      // the sequence number.
      void applyCommand(const AccessControl::Evaluation& evaluation, int64_t uptimeSecs);

      // Starts an LED A pattern and the 10 ms timer that renders it.
      void playLedPattern(LedPattern pattern);

      // k_timer expiry: renders LED A while a pattern plays, then stops itself.
      static void ledTimerHandler(struct k_timer* timer);
```

3. In `src/app.hpp`, replace:

```cpp
      int64_t m_detection_hold_until_ms;
```

   with:

```cpp
      int64_t m_detection_hold_until_ms;

      // LED A acknowledgement patterns. The sequencer is read from the timer
      // handler and written from the main loop; see playLedPattern().
      LedSequencer m_led_sequencer;
      struct k_timer m_led_timer;
```

4. In `src/app.cpp`, replace:

```cpp
      , m_detection_hold_until_ms(0)
      , m_initialised(false)
```

   with:

```cpp
      , m_detection_hold_until_ms(0)
      , m_led_sequencer()
      , m_led_timer {}
      , m_initialised(false)
```

5. In `src/app.cpp`, replace:

```cpp
    k_timer_init(&m_delay_timer, nullptr, nullptr);
```

   with:

```cpp
    k_timer_init(&m_delay_timer, nullptr, nullptr);
    k_timer_init(&m_led_timer, &App::ledTimerHandler, nullptr);
    k_timer_user_data_set(&m_led_timer, this);
```

6. In `src/app.cpp`, replace:

```cpp
#if defined(CONFIG_MFS_DEBUG_LED)
#if !defined(CONFIG_MFS_BATTERY_TEST)
      ledA = (m_arm_state == ArmState::Inactive);
#endif
```

   with:

```cpp
#if !defined(CONFIG_MFS_BATTERY_TEST)
      // While a pattern plays the LED timer owns LED A, and this mirrors it so the
      // loop's write cannot fight the timer. Patterns play in EVERY build: they are
      // the command acknowledgement, not a debug aid.
      ledA = m_led_sequencer.Level(k_uptime_get());
#endif

#if defined(CONFIG_MFS_DEBUG_LED)
#if !defined(CONFIG_MFS_BATTERY_TEST)
      // Bench only: between patterns, LED A is lit while Inactive, as before.
      if (!m_led_sequencer.IsActive(k_uptime_get())) { ledA = (m_arm_state == ArmState::Inactive); }
#endif
```

7. In `src/app.cpp`, replace:

```cpp
    // PHASE 2 SHIM, extended in Task 14 to carry the settings so the detection
    // engine can be bench-tested from the app. The clock trim, re-arm ordering and
    // LED patterns arrive in Task 16, which replaces this.
    m_settings.ApplyFrom(evaluation.command, evaluation.slot == access::M_SLOT_NETWORK_MANAGER);
    setArmState(evaluation.command.armActive ? ArmState::Active : ArmState::Inactive);
```

   with:

```cpp
    applyCommand(evaluation, uptimeSecs);
```

8. In `src/app.cpp`, replace:

```cpp
  int App::applyLeds(bool ledA, bool ledB)
```

   with:

```cpp
  void App::applyCommand(const AccessControl::Evaluation& evaluation, int64_t uptimeSecs)
  {
    const protocol::Command& command { evaluation.command };
    bool fromNetworkManager { evaluation.slot == access::M_SLOT_NETWORK_MANAGER };
    ArmDecision decision { DecideCommand(m_arm_state == ArmState::Active, fromNetworkManager, command) };
    protocol::Mode previousMode { m_settings.OperatingMode() };
    bool delayWasPending { m_delay_pending };
    LedPattern pattern { LedPattern::None };

    // THE SINGLE PATH. Everything below acts on `decision` and on nothing else -
    // see DecideCommand(). On command, an armed device only ever disarms.
    if (decision.action == ArmAction::Ignore) {
      LOG_WRN("Armed: command slot %u n %u ignored - only a disarm is accepted while armed.", evaluation.slot, evaluation.n);
      return;
    }

    if (decision.trimClock && m_clock.ApplyMinuteHint(command.minuteOfDay, uptimeSecs) == DeviceClock::TrimResult::Trimmed) {
      LOG_INF("Clock trimmed from slot %u: now minute %u.", evaluation.slot, m_clock.MinuteOfDay(uptimeSecs));
    }

    if (decision.applySettings) {
      if (command.mode != protocol::Mode::TriggerOnly && !decision.applyMode) {
        LOG_WRN("Mode field from slot %u ignored - only the Network Manager may change the mode.", evaluation.slot);
      }
      m_settings.ApplyFrom(command, decision.applyMode);
    }

    switch (decision.action) {
      case ArmAction::Disarm:
        // Nothing but the disarm. The settings this command carries were not
        // applied above; the engineer sends them once the device is Inactive.
        setArmState(ArmState::Inactive);
        pattern = delayWasPending ? LedPattern::DisarmedDelayCancelled : LedPattern::Disarmed;
        if (delayWasPending) { LOG_WRN("Disarmed with a trigger PENDING - the trigger is cancelled."); }
        break;

      case ArmAction::Arm:
        m_activation_count = 0;
        m_detection_met    = false;

        // setArmState() performs the standby -> configure -> confirm-AWAKE-clear
        // sequence that makes arming edge-triggered, and refuses if it cannot.
        setArmState(ArmState::Active);
        pattern = (m_arm_state == ArmState::Active) ? LedPattern::Armed : LedPattern::ArmRefused;
        if (pattern == LedPattern::ArmRefused) { LOG_ERR("Arming refused - command slot %u n %u is spent; send again.", evaluation.slot, evaluation.n); }
        break;

      case ArmAction::Tune:
        // (Re)start the engine at the new threshold so LED B simulates triggers
        // straight away. Always, not only on change - the engineer may be
        // restarting the simulation after a disarm stood the part down.
        m_activation_count = 0;
        m_detection_met    = false;
        m_in_cooldown      = false;
        if (enableAccelerometer() < 0) { LOG_ERR("Could not start the engine for tuning!"); }
        pattern = (m_settings.OperatingMode() != previousMode) ? LedPattern::ModeChanged : LedPattern::SettingsApplied;
        break;

      default:
        return;
    }

    if (m_settings.OperatingMode() != protocol::Mode::TriggerOnly) {
      LOG_WRN("Mode %u stored but reporting is not implemented - behaving as Trigger only!", static_cast<unsigned>(m_settings.OperatingMode()));
    }

    LOG_INF("Applied: arm %s, %u activations, %u s cooldown, %u LSB, %u s delay.", m_arm_state == ArmState::Active ? "Active" : "Inactive",
            m_settings.Activations(), m_settings.CooldownSeconds(), m_settings.ThresholdLsb(), m_settings.DelaySeconds());
    playLedPattern(pattern);
  }

  void App::playLedPattern(LedPattern pattern)
  {
    constexpr k_timeout_t M_LED_TICK { K_MSEC(10) };

    // Stop the timer before touching the sequencer, so the handler never reads it
    // half-written. A new command replaces whatever was playing.
    k_timer_stop(&m_led_timer);
    m_led_sequencer.Start(pattern, k_uptime_get());
    k_timer_start(&m_led_timer, K_NO_WAIT, M_LED_TICK);
  }

  void App::ledTimerHandler(struct k_timer* timer)
  {
    App* self { static_cast<App*>(k_timer_user_data_get(timer)) };
    int64_t now { k_uptime_get() };

    if (!self->m_led_sequencer.IsActive(now)) {
      k_timer_stop(timer);
      return;
    }

    // ISR context. gpio_pin_set_dt() is ISR-safe on the nRF GPIO driver.
    gpio_pin_set_dt(&s_led_a, self->m_led_sequencer.Level(now) ? 1 : 0);
  }

  int App::applyLeds(bool ledA, bool ledB)
```

9. In `src/app.hpp`, replace:

```cpp
      struct k_timer m_led_timer;
```

   with:

```cpp
      struct k_timer m_led_timer;

      // ONE-SHOT trigger. Set while the output is asserted; when it clears, the
      // trigger is complete and the main loop latches the device Inactive.
      bool m_trigger_fired;
      bool m_trigger_complete;
```

10. In `src/app.cpp`, replace:

```cpp
      , m_led_timer {}
      , m_initialised(false)
```

   with:

```cpp
      , m_led_timer {}
      , m_trigger_fired(false)
      , m_trigger_complete(false)
      , m_initialised(false)
```

11. In `src/app.cpp`, replace:

```cpp
      // The ONE place the output state is derived. See updateOutputState().
      updateOutputState();
```

   with:

```cpp
      // The ONE place the output state is derived. See updateOutputState().
      updateOutputState();

      // Firing is one of the only two ways out of the armed state. Acted on here,
      // not inside updateOutputState(), because setArmState() re-enters it.
      if (m_trigger_complete) {
        m_trigger_complete = false;
        LOG_WRN("Trigger complete - latched Inactive. Re-arming needs an engineer command.");
        setArmState(ArmState::Inactive);
      }
```

12. In `src/app.cpp`, replace:

```cpp
    m_output_switch.Set(m_output_active);
```

   with:

```cpp
    m_output_switch.Set(m_output_active);

    // ONE-SHOT. Once the output has asserted and its period has ended, the
    // trigger is complete. Only flagged here - see the main loop.
    if (m_output_active) { m_trigger_fired = true; }
    if (m_trigger_fired && !m_output_active) {
      m_trigger_fired    = false;
      m_trigger_complete = true;
    }
```

13. In `src/app.cpp`, replace:

```cpp
      // Boolean first, sensor second - see disableAccelerometer().
      m_arm_state = ArmState::Inactive;
      disableAccelerometer();
```

   with:

```cpp
      // Boolean first, sensor second - see disableAccelerometer().
      m_arm_state = ArmState::Inactive;
      disableAccelerometer();

      // Whatever the route to Inactive, a trigger in progress is over. Cleared
      // after disableAccelerometer(), which re-derives the output.
      m_trigger_fired    = false;
      m_trigger_complete = false;
```

Check the single path by reading, not just by testing: `grep -n "m_settings.ApplyFrom\|setArmState(ArmState::Active)" src/app.cpp` must show `ApplyFrom` **only** inside `applyCommand()` behind `decision.applySettings`, and `setArmState(ArmState::Active)` **only** in the `ArmAction::Arm` case. Any other call site is a second path — remove it.

- [ ] **Step 5: Build and bench-test the whole flow**

```bash
west build -b nrf54l15dk/nrf54l05/cpuapp -p always -- -DEXTRA_CONF_FILE=credentials.conf
west flash --dev-id 853003346 --recover
make test
```

With the app and RTT open:

1. Boot: LED A lit (bench build), `Clock INVALID`. Provision.
2. Armed off, adjust sliders, **Send**: **one blink**, `Applied: arm Inactive, ...`. Handle the device: **LED B** lights ~5 s per detection.
3. Armed on (activations 1, delay 0), **Send**: **rapid flash**, then LED A off. `Arm state: Active`.
4. **Still armed**, change the sensitivity slider, **Send**: **no flash, nothing changes**, `Armed: command slot 1 n ... ignored - only a disarm is accepted while armed.` Then **Send as Network Manager** with Report: also ignored.
5. Trigger it: fire output and LED B for ~5 s, then `Trigger complete - latched Inactive.` and `Arm state: Inactive`; LED A lights. Trigger again: LED B only if tuning was restarted — **the output never asserts**.
6. Armed on with **delay 60 s**, **Send** (rapid flash). Trigger. Within the minute: Armed off, **change activations to 7**, **Send**: **double blink**, `Disarmed with a trigger PENDING`. The output never asserts, and the `Applied:` line still shows the **old** activation count — the disarm applied nothing else.
7. Armed off, **Send** again: **one blink**, and now `Applied:` shows 7.
8. Inactive, **Send as Network Manager** with Report: **two blinks**, then `Mode 2 stored but reporting is not implemented`.
9. Build with `CONFIG_MFS_DEBUG_LED=n` in `prj.conf`, flash, provision, repeat step 3: **the rapid flash still plays**, and LED A stays dark between patterns. Restore `CONFIG_MFS_DEBUG_LED=y`.

**Steps 4, 5 and 6 are the ones that matter.** Record which patterns were confirmed by eye. Arm Refused cannot be provoked without disconnecting the ADXL367; note it as unverified on hardware if not attempted.

- [ ] **Step 6: Commit**

```bash
/Users/andy/nrfenv/bin/clang-format -i src/arm_policy.hpp src/app.cpp src/app.hpp tests/test_arm_policy.cpp tests/test_main.cpp
git add src/arm_policy.hpp src/app.cpp src/app.hpp CMakeLists.txt tests/
git commit -m "Allow an armed device only to disarm, and make triggers one-shot

On command, an armed device does exactly one thing: disarm. Any other command
is ignored outright - no settings, no mode, no clock trim, no re-arm - and a
disarm applies nothing but the disarm. The rule is one pure function,
DecideCommand(), host-tested across every state, slot and arm bit, and
applyCommand() acts on its decision and nothing else.

A trigger is one-shot: once the output period ends the device latches Inactive.
Disarm and trigger are therefore the only two ways out of the armed state.

LED A acknowledges accepted commands from a 10 ms timer, in every build. An
ignored command shows nothing.

Verified on hardware: <steps 4-6 results; patterns confirmed by eye>."
```

---

## Task 17: Correct the documents the implementation invalidates

The design amendment of 2026-09-13 already corrected `docs/tan-scheme.md`, `docs/power-budget.md` §8 and `CLAUDE.md`'s access rules. What remains could only be written once the code existed.

**Files:**
- Modify: `docs/v1-scope.md`, `CLAUDE.md`, `tools/toggle_dongle/README.md`

- [ ] **Step 1: Fix `docs/v1-scope.md`**

- **§1 items 2, 5, 6** — the engineer's action is an authenticated command, not a toggle; date/time comes from the provisioner on **every** boot; LED A acknowledges commands (spec §6.7), LED B shows detection in both states.
- **§1.0** — replace the `m_output_active = ... awake && !m_ignore_stale_trigger` snippet with the current `(m_arm_state == ArmState::Active) && m_detection_met && delayPermitsFiring()`, and replace the `ledB = IsOutputActive();` worked example with `m_output_switch.Set(m_output_active);` — OutputSwitch is now the example consumers copy.
- **§1.1** — remove "no configuration beyond the single toggle".
- **§4** — replace the 5-byte TAN table with the 16-byte encrypted command, pointing at `docs/tan-scheme.md` §6.1. The "no acknowledgement" trade-off now reads: LED A is the acknowledgement.
- **§5** — the production LED question is answered for LED A: bounded patterns only, a few seconds per command.
- **§6** — record the answer: arm state is not persisted, and a reset also invalidates the clock, so a brown-out leaves the device Inactive **and** unresponsive until provisioned. nPM2100 SCRATCHA (spec §7.1) is the intended refinement for both.

- [ ] **Step 2: Fix `CLAUDE.md`**

- Replace the opening "this project is a **fresh skeleton**" section — stale since 2026-08-17 — with the current state: the units in spec §6.1, `make test` for the host suite, and the credentials build line.
- Replace "**The counterpart must advertise at 20–50 ms.**" with the measured finding: a phone advertises at ~187 ms (Task 13's iPhone figure if different), the interval is not ours to set, and the app compensates with a 30 s window.
- Replace the `tools/toggle_dongle` section with a short retirement note: superseded by `../class_app`; its lessons about Thingy:53 netcore sysbuild, `mcuboot-button0` and LED aliases stay valid for any future Thingy:53 work, so keep those three bullets.
- Add `../class_app` to the workspace-context tree.
- In **Build commands**, add the `-DEXTRA_CONF_FILE=credentials.conf` line as the normal build and the rule **always flash this board with `--recover`**.

- [ ] **Step 3: Mark the toggle tool retired**

At the top of `tools/toggle_dongle/README.md` add:

```markdown
> **RETIRED 2026-09-13.** MFS_1 no longer accepts this payload — commands are
> encrypted under day keys (`docs/tan-scheme.md`). Use `../class_app`. Kept for its
> Thingy:53 build notes only.
```

Do **not** delete the directory without Andy's say-so.

- [ ] **Step 4: Commit**

```bash
git add docs/v1-scope.md CLAUDE.md tools/toggle_dongle/README.md
git commit -m "Correct the documents the app-control work invalidates

v1-scope section 1.0's worked example now points at OutputSwitch and the current
derivation, section 4's five-byte TAN payload gives way to the encrypted command,
and section 6 records that a reset leaves the device Inactive and unprovisioned.

CLAUDE.md no longer calls the repo a fresh skeleton, drops the disproved 20-50 ms
counterpart claim, and retires the toggle tool while keeping its Thingy:53
lessons."
```

---

## Task 18: Report modes — BLOCKED, do not start

**This task cannot begin until the report payload is specified** (spec §6.5.2). It is listed so the gap stays visible, and so Tasks 1–17 ship a complete Trigger-only device without it. The mode is already stored, gated on slot 0, and logged as not implemented.

**What is blocking.** The hub needs to know *which* sensor fired, so the report carries a device identifier. **A stable identifier in a repeated broadcast is a tracking beacon.** The day-key scheme offers a candidate: a rotating ID derived from a slot-0 key that the hub can precompute. That is a design decision to take with Andy, not a formatting one.

Questions to answer first:

1. **What identifies the sensor to the hub?** A rotating ID from the Network Manager's key is unlinkable to a listener; the hub then needs the day keys, which makes it a key holder.
2. **How long is the burst, and at what interval?** `docs/power-budget.md` §8.7.1 requires it be **bounded**.
3. **Is the report authenticated?** CCM under a report-specific key derived from the day key fits 16 bytes the same way commands do.
4. **What does the hub do with a report it cannot attribute?**

**Also to decide:** the one-shot latch (Task 16) completes when the output period ends. In `ReportOnly` the output never asserts, so Task 18 must define when a report-only trigger is complete — most simply, when the report burst ends.

**When unblocked:** a `Reporter` class owning a bounded advertising burst; a call site in `updateOutputState()` immediately before `m_output_switch.Set(true)` for `ReportAndTrigger`, and in place of it for `ReportOnly`; removal of the Task 16 warning; the `CLAUDE.md` wording extended with the burst's bounds.

---

## Mapping from the 2026-09-12 plan

| Old task | New task | Change |
|---|---|---|
| 1 Tables + harness | 1 | Harness gains `HOST_SRCS`; header split across Tasks 1–2 |
| 2 Payload parsing | 2 | **Rewritten** — plaintext codec, no prefix, no test code |
| — | 3, 4, 5 | **New** — access keys and vectors, DeviceClock, AccessControl |
| — | 6 | **New** — LED sequencer |
| 7 Settings | 7 | Mode gated on slot 0; static settings handler (old version never loaded) |
| 8 Day codes | — | **Removed** — replaced by Tasks 3 and 5 |
| — | 8 | **New** — PSA backend, credentials, boot self-test |
| 3 Scanner rewrite | 9 | **Rewritten** — raw UUID queue, clock and access dispatch |
| 4 Scaffold | 10 | + pointycastle, crypto, shared_preferences; generated vectors and credentials |
| 5 Protocol + advertiser | 11, 12 | **Rewritten** — access keys, sequence store, key source, builder |
| 6 Screen | 13 | **Rewritten** — no codes; provisioner screen; hardware E2E |
| 9 Detection engine | 14 | Settings loaded here; shim carries settings |
| 9a Delay interlock | 15 | **Bug fixed** — delayed trigger never asserted |
| 10 Payload handling | 16 | **Rewritten** — single armed path (`ArmPolicy`), one-shot trigger, LED A |
| 11 Documents | 17 | Reduced — access documents already amended |
| 12 Report modes | 18 | Still blocked; rotating IDs noted as a candidate |

---

## Self-Review

**Spec coverage** (`docs/superpowers/specs/2026-09-12-app-control-design.md` and `docs/tan-scheme.md`):

- Spec §1 scope, §5 encodings → Tasks 1, 2, 7, 11. §2 decisions 1–12 → Tasks 14, 15; 13–21 → Tasks 3–9, 16. §3 spike → Tasks 12, 13 (30 s window, iPhone measurement). §4 wire format → Tasks 2, 3, 11. §4.3 modes, slot 0 only → Tasks 2, 7, 12, 13, 16, 18. §6.1 units → Tasks 2–9. §6.2 invariant → Tasks 14, 17. §6.3 engine → Task 14. §6.4 activation order and the single armed path → Task 16. §6.5 no test code → Tasks 2, 12 (nothing implements one). §6.5.1 interlock → Task 15. §6.6 silence → Tasks 9, 16. §6.7 LEDs → Tasks 6, 16. §6.8 time → Tasks 4, 9, 16. §7 persistence → Tasks 7, 9. §8 app → Tasks 10–13. §9 verification: item 2 → Task 13; item 5 → Task 8; item 6 (LFXO trim) → **not in this plan**, a bench measurement outside the software. §10 documents → Task 17.
- tan-scheme §3 derivations → Tasks 3, 11; §3.1 never reuse n → Task 12 (reserve before advertise, tested); §4 day index → Tasks 4, 11; §5 key issue → Task 12 (bench stand-in only); §6.2 acceptance → Task 5; §6.4 lockout → Task 5; §6.6 LED ack → Task 16; §7.1 invalid on boot → Tasks 4, 9; §7.2 sync rules → Tasks 3, 4, 9, 13; §7.3 trim → Tasks 4, 16; §8 secrets → Tasks 8, 10 (bench); KMU → **not in this plan**.

**Type consistency.** `protocol::Command` fields are identical in Tasks 2, 5, 7, 9, 14, 16 and mirrored by `Mfs1Command` in Task 11. `AccessControl::Evaluation { verdict, slot, n, command }` is used unchanged in Tasks 5, 9, 16. `Settings::ApplyFrom(const protocol::Command&, bool)` in Tasks 7, 14, 16. `CommandScanner::TakeCandidate` in Tasks 9 and 16; `SetFastScan` in Task 15. Dart `sealCommand(dayKey, deviceId, day, slot, n, plaintext)` matches the C++ `SealCommand` argument order.

**Verified before writing.** Every C++ block in Tasks 1–9 and 14–16 was built at its stage in a scratch worktree: the host suite passes, and the firmware builds for `nrf54l15dk/nrf54l05/cpuapp` with no warnings. Every Dart block passes `flutter analyze` and `flutter test`. **Nothing has run on hardware.**

**Known gaps, deliberately deferred.**

1. The `OutputSwitch` fault-latch paths and the delay interlock have no automated tests — both need GPIO or kernel-timer seams the workspace has no mock for. Task 15 step 4 is a scripted bench procedure instead, and **its step 3 — deactivating mid-delay — is the single most important verification in this plan.**
2. `App::applyCommand()` itself is not host-tested, but the decision it acts on is (`DecideCommand()`, Task 16), and every security rule beneath it is host-tested in Tasks 4–5. The one-shot latch is bench-verified in Task 16 step 5.
3. The LED A write in the 100 ms loop mirrors the 10 ms timer and may be one timer tick late at a phase edge — a ≤ 10 ms visual artefact, accepted.
4. Uninstalling the app resets its sequence counters (Task 12). Acceptable on the bench; the production Network Manager must issue a fresh slot to a reinstalled app.
5. KMU key storage, Android Keystore, the real Network Manager and the LFXO trim are outside this plan (`docs/tan-scheme.md` §11).
6. Task 18 is blocked on a design decision.
