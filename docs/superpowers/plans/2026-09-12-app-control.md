# MFS_1 App Control Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace the Thingy:53 toggle tool with a Flutter app that sets three parameters on MFS_1 and arms it against a day code.

**Architecture:** The phone advertises a 128-bit service UUID carrying a 16-byte payload; MFS_1 stays scan-only and never advertises. The payload is an absolute state assertion, so it is idempotent across the several scan windows that will hear it. Firmware work is ordered so that phase 2 turns the real device into the app's test target, avoiding a throwaway stub.

**Tech Stack:** C++20 / Zephyr / NCS v3.2.4 on nRF54L05; Flutter 3.41 / Dart 3.11 with `ble_peripheral` 2.4.0; host tests via plain `g++` and `assert()`.

**Spec:** `docs/superpowers/specs/2026-09-12-app-control-design.md`

## Global Constraints

- **Two repos.** Firmware in `/Users/andy/nordic/ncs/v3.2.4/class_mfs_1`; app in `/Users/andy/nordic/ncs/v3.2.4/class_app`. Each is its own git repo. Commit to whichever repo a task touches.
- **House style is `~/.claude/CLAUDE.md`**, and `.clang-format` is authoritative for layout. Run `/Users/andy/nrfenv/bin/clang-format -i src/<file>` on every C++ file touched.
- **Always flash with `--recover`.** `west flash` without it has reported success on this board while the old image kept running. If freshly flashed firmware appears not to run, re-flash with `--recover` before diagnosing anything else.
- **Board target:** `nrf54l15dk/nrf54l05/cpuapp`. **J-Link for MFS_1 is `--dev-id 853003346`.** J-Link device name for RTT is `nRF54L05_M33` (not `_XXAA`).
- **Payload budget: 16 bytes total**, 4-byte prefix + 12 payload. Never exceed it — an iOS tool depends on fitting one 128-bit service UUID.
- **Bytes 13–15 are per-variant extension space. MFS_1 must IGNORE them and must never validate them as zero.**
- **The arm invariant** (`docs/v1-scope.md` §1.0): `App::updateOutputState()` is the only place the arm state and detection are combined, and `App::IsOutputActive()` is the only sanctioned read. Nothing may read INT1, the AWAKE bit or `Adxl367::ReadAwake()` and act on it.
- **`OutputSwitch` already exists** and owns the fire pins privately. Do not add an accessor, do not declare a second handle to P2.05/P2.09.
- **Failures are silent on the radio.** An invalid code, a used code, an out-of-range parameter or a test code asking to arm produce no emission whatsoever — RTT logging only.

---

## File Structure

**Firmware — `class_mfs_1`:**

| File | Responsibility |
|---|---|
| `src/mfs_protocol.hpp` (new) | wire format constants, offsets, `Payload`, `ParseAdvert()` |
| `src/mfs_protocol.cpp` (new) | `ParseAdvert()` and the table lookups |
| `src/mfs_protocol_tables.hpp` (new, **generated**) | the two 256-entry encoding tables |
| `tools/gen_protocol_tables.py` (new) | generates the C++ **and** Dart tables from one formula |
| `src/command_scanner.{hpp,cpp}` (modify) | parse AD 0x07, validate, dedupe, hand `Payload` to the loop |
| `src/settings.{hpp,cpp}` (new) | the three parameters, NVS-backed |
| `src/day_codes.{hpp,cpp}` (new) | code table, used-mask in NVS, auto-release |
| `src/app.{hpp,cpp}` (modify) | detection engine, activation counting, cooldown, arm transitions |
| `tests/` + `Makefile` (new) | host tests for everything that is pure logic |

**App — `class_app`:**

| File | Responsibility |
|---|---|
| `lib/protocol/tables.dart` (**generated**) | the same two tables |
| `lib/protocol/mfs_protocol.dart` | payload construction, UUID string assembly |
| `lib/services/advertiser.dart` | wraps `ble_peripheral`, 30 s advertise window |
| `lib/devices/mfs1/mfs1_screen.dart` | the control UI |
| `lib/devices/mfs1/mfs1_settings.dart` | the settings model |
| `lib/main.dart` | device picker → device screen |

---

# PHASE 1 — The protocol contract

## Task 1: Generated encoding tables and the host-test harness

The two geometric encodings must produce **byte-identical results on both platforms**. A formula evaluated independently in C++ (`powf`) and Dart (`pow`) can differ by an LSB at some inputs, and the two sides would then silently disagree about what a slider means. One generator, two outputs, no drift.

**Files:**
- Create: `tools/gen_protocol_tables.py`
- Create: `src/mfs_protocol_tables.hpp` (generated — commit the output)
- Create: `Makefile`
- Create: `tests/test_main.cpp`
- Create: `tests/test_protocol.cpp`

**Interfaces:**
- Consumes: nothing.
- Produces: `alc::protocol::M_THRESHOLD_TABLE[256]` (`uint16_t`, LSB), `alc::protocol::M_COOLDOWN_TABLE[256]` (`uint16_t`, seconds). Task 2 consumes both.

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

- [ ] **Step 2: Run the generator and sanity-check the anchors**

```bash
cd /Users/andy/nordic/ncs/v3.2.4/class_mfs_1
python3 tools/gen_protocol_tables.py
python3 -c "
import re
t = open('src/mfs_protocol_tables.hpp').read()
nums = [int(n) for n in re.findall(r'\d+', t.split('M_THRESHOLD_TABLE')[1].split('};')[0])]
print('threshold[0]  =', nums[1], '(expect 4000)')
print('threshold[143]=', nums[144], '(expect 302 - the present default, 75 mg)')
print('threshold[255]=', nums[256], '(expect 40)')
"
```

Expected: `4000`, `302`, `40`. If byte 143 is not 302 the formula has drifted from the spec.

Also check the delay seams, which is where a piecewise encoding goes wrong:

```bash
python3 -c "
import importlib.util, pathlib
spec = importlib.util.spec_from_file_location('g', 'tools/gen_protocol_tables.py')
g = importlib.util.module_from_spec(spec); spec.loader.exec_module(g)
for code in (0, 59, 60, 118, 119, 127):
    print(f'delay[{code:3d}] = {g.delay(code):5d} s')
"
```

Expected: `0, 59, 60, 3540, 3600, 32400`. The pairs 59/60 and 118/119 must be
contiguous — 59 s then 60 s, and 3540 s then 3600 s.

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
  assert(M_THRESHOLD_TABLE[0] == 4000);     // 1000 mg, least sensitive
  assert(M_THRESHOLD_TABLE[143] == 302);    // ~75 mg, the present default
  assert(M_THRESHOLD_TABLE[255] == 40);     // 10 mg, most sensitive

  assert(M_COOLDOWN_TABLE[0] == 0);         // reserved: no cooldown
  assert(M_COOLDOWN_TABLE[1] == 1);
  assert(M_COOLDOWN_TABLE[128] == 60);      // the clean midpoint
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
  assert(M_DELAY_TABLE[0] == 0);        // default - no delay
  assert(M_DELAY_TABLE[59] == 59);      // 59 s
  assert(M_DELAY_TABLE[60] == 60);      // 1 min - contiguous with 59 s
  assert(M_DELAY_TABLE[118] == 3540);   // 59 min
  assert(M_DELAY_TABLE[119] == 3600);   // 1 h - contiguous with 59 min
  assert(M_DELAY_TABLE[127] == 32400);  // 9 h, the maximum

  // Strictly increasing: no delay code may mean the same as another, or the app
  // would present two slider positions that do the same thing.
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

Create `Makefile` (flags copied from `../class_templates/npm2100/Makefile`, the workspace reference):

```make
CXX      ?= g++
CXXFLAGS ?= -std=c++20 -Wall -Wextra -Wpedantic -Werror -O0 -g -Isrc -Itests

TEST_SRCS = $(wildcard tests/test_*.cpp)
SRCS      = $(TEST_SRCS)

test: $(SRCS)
	$(CXX) $(CXXFLAGS) $(SRCS) -o test_runner
	./test_runner

clean:
	rm -f test_runner

.PHONY: test clean
```

- [ ] **Step 4: Run the test to verify it fails**

```bash
make test
```

Expected: FAIL — `fatal error: mfs_protocol.hpp: No such file or directory`.

- [ ] **Step 5: Create the minimal header to make it pass**

Create `src/mfs_protocol.hpp`:

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

  constexpr uint8_t M_OFFSET_MAGIC_0 { 0 };
  constexpr uint8_t M_OFFSET_MAGIC_1 { 1 };
  constexpr uint8_t M_OFFSET_DEVICE_TYPE { 2 }; // uint16, little-endian
  constexpr uint8_t M_OFFSET_VERSION { 4 };
  constexpr uint8_t M_OFFSET_DAY_CODE { 5 }; // uint32, little-endian
  constexpr uint8_t M_OFFSET_ARM_STATE { 9 };
  constexpr uint8_t M_OFFSET_ACTIVATIONS { 10 };
  constexpr uint8_t M_OFFSET_COOLDOWN { 11 };
  constexpr uint8_t M_OFFSET_SENSITIVITY { 12 };
  // Bytes 13-15 are per-variant extension space. MFS_1 MUST IGNORE THEM and
  // must never require them to be zero: another variant will use them, and
  // validating them would make MFS_1 reject a future app build.

  constexpr uint8_t M_MAGIC_0 { 'C' };
  constexpr uint8_t M_MAGIC_1 { 'L' };
  constexpr uint16_t M_DEVICE_TYPE_MFS1 { 0x0001 };
  constexpr uint8_t M_PROTOCOL_VERSION { 0x01 };

  // Byte 4 is shared: the low six bits are the version, the top two the mode.
  // The version check MUST mask, or a mode other than TriggerOnly makes every
  // payload look like the wrong protocol version.
  constexpr uint8_t M_VERSION_MASK { 0x3F };
  constexpr uint8_t M_MODE_SHIFT { 6 };
  constexpr uint8_t M_MODE_MASK { 0x03 };

  // Byte 9 is shared: bit 0 is the arm state, bits 1-7 the delay code.
  constexpr uint8_t M_ARM_BIT { 0x01 };
  constexpr uint8_t M_DELAY_SHIFT { 1 };
  constexpr uint8_t M_DELAY_MASK { 0x7F };

  /**
   * @brief What the device does when the activation count is reached.
   *
   * Report and ReportAndTrigger make the device ADVERTISE, which is an exception
   * to the standing rule in CLAUDE.md that it never does. They are for use only
   * when absolutely necessary - advertising forfeits covertness.
   */
  enum class Mode : uint8_t
  {
    TriggerOnly      = 0, ///< Default. Fires the output, emits nothing.
    ReportAndTrigger = 1, ///< Broadcasts immediately before firing.
    ReportOnly       = 2, ///< Broadcasts, never fires.
    Reserved         = 3, ///< Rejected.
  };

  constexpr uint8_t M_ACTIVATIONS_MIN { 1 };
  constexpr uint8_t M_ACTIVATIONS_MAX { 16 };

  /** @brief A decoded command payload. Byte encodings already resolved. */
  struct Payload
  {
      uint32_t dayCode { 0 };
      bool armActive { false };
      Mode mode { Mode::TriggerOnly };
      uint8_t delayCode { 0 };
      uint8_t activations { 1 };
      uint8_t cooldownByte { 0 };
      uint8_t sensitivityByte { 0 };
  };

  /**
   * @brief Validate and decode an on-air 16-byte sequence.
   *
   * @param bytes  On-air bytes, least-significant first.
   * @param length Number of bytes available.
   * @param out    Populated only on success.
   * @return True if this is a well-formed MFS_1 payload.
   */
  bool ParseAdvert(const uint8_t* bytes, uint8_t length, Payload& out);

  /** @brief Sensitivity byte to ADXL367 THRESH_ACT, in 0.25 mg LSB. */
  inline uint16_t SensitivityToThresholdLsb(uint8_t sensitivityByte) { return M_THRESHOLD_TABLE[sensitivityByte]; }

  /** @brief Cooldown byte to seconds. Zero means no cooldown. */
  inline uint16_t CooldownToSeconds(uint8_t cooldownByte) { return M_COOLDOWN_TABLE[cooldownByte]; }

  /** @brief Delay code (7 bits) to seconds. Zero means fire immediately. */
  inline uint16_t DelayToSeconds(uint8_t delayCode) { return M_DELAY_TABLE[delayCode & M_DELAY_MASK]; }

}
```

- [ ] **Step 6: Run the test to verify it passes**

```bash
make test
```

Expected: `protocol tables: OK` then `ALL TESTS PASSED`.

- [ ] **Step 7: Commit**

```bash
cd /Users/andy/nordic/ncs/v3.2.4/class_mfs_1
/Users/andy/nrfenv/bin/clang-format -i src/mfs_protocol.hpp
git add tools/gen_protocol_tables.py src/mfs_protocol_tables.hpp src/mfs_protocol.hpp Makefile tests/
git commit -m "Add generated protocol encoding tables and a host-test harness

The two geometric encodings are generated once and emitted for both C++ and
Dart. Evaluating the formula independently on each side risks a one-LSB
divergence, which would mean a slider position meaning two different things -
generating removes the possibility rather than testing for it.

Tests assert the spec's anchors (byte 143 = 302 LSB, the present default;
cooldown byte 128 = 60 s), that sensitivity is monotonically DECREASING in
threshold so the slider cannot run backwards, and that no entry overflows the
ADXL367's 13-bit threshold register - an overflow would be silently truncated
into a different threshold."
```

---

## Task 2: Payload parsing

**Files:**
- Create: `src/mfs_protocol.cpp`
- Modify: `tests/test_protocol.cpp`
- Modify: `Makefile:5` (add the source to the build)

**Interfaces:**
- Consumes: `Payload`, offsets and `M_*` constants from Task 1.
- Produces: `bool alc::protocol::ParseAdvert(const uint8_t*, uint8_t, Payload&)`. Task 3 consumes it.

- [ ] **Step 1: Write the failing tests**

Append to `tests/test_protocol.cpp`, and add `run_parse_tests();` to `tests/test_main.cpp`:

```cpp
#include <cstring>

static void buildValid(uint8_t* b)
{
  memset(b, 0, alc::protocol::M_UUID_BYTES);
  b[0]  = 'C';
  b[1]  = 'L';
  b[2]  = 0x01; // device type 0x0001, little-endian
  b[3]  = 0x00;
  b[4]  = 0x01; // version
  b[5]  = 0x11; // day code 0x11111111, little-endian
  b[6]  = 0x11;
  b[7]  = 0x11;
  b[8]  = 0x11;
  b[9]  = 0x01; // arm active, delay code 0
  b[10] = 3;    // activations
  b[11] = 128;  // cooldown -> 60 s
  b[12] = 143;  // sensitivity -> 302 LSB
}

void run_parse_tests()
{
  using namespace alc::protocol;
  uint8_t bytes[M_UUID_BYTES];
  Payload payload;

  // Happy path.
  buildValid(bytes);
  assert(ParseAdvert(bytes, M_UUID_BYTES, payload));
  assert(payload.dayCode == 0x11111111);
  assert(payload.armActive);
  assert(payload.activations == 3);
  assert(CooldownToSeconds(payload.cooldownByte) == 60);
  assert(SensitivityToThresholdLsb(payload.sensitivityByte) == 302);
  assert(payload.mode == Mode::TriggerOnly);   // the default, bits 6-7 clear
  assert(payload.delayCode == 0);

  // Byte 9 is shared. Delay code 119 (1 hour) with the arm bit set must decode
  // as BOTH - packing them into one byte is where a shift error hides.
  buildValid(bytes);
  bytes[9] = static_cast<uint8_t>((119 << 1) | 0x01);
  assert(ParseAdvert(bytes, M_UUID_BYTES, payload));
  assert(payload.armActive);
  assert(payload.delayCode == 119);
  assert(DelayToSeconds(payload.delayCode) == 3600);

  // Same byte, arm bit CLEAR, delay preserved.
  buildValid(bytes);
  bytes[9] = static_cast<uint8_t>(127 << 1);
  assert(ParseAdvert(bytes, M_UUID_BYTES, payload));
  assert(!payload.armActive);
  assert(payload.delayCode == 127);
  assert(DelayToSeconds(payload.delayCode) == 32400);   // 9 h

  // Byte 4 is shared too. The version check MUST mask off the mode bits, or any
  // mode but TriggerOnly makes every payload look like the wrong version.
  buildValid(bytes);
  bytes[4] = static_cast<uint8_t>(M_PROTOCOL_VERSION | (1 << M_MODE_SHIFT));
  assert(ParseAdvert(bytes, M_UUID_BYTES, payload));
  assert(payload.mode == Mode::ReportAndTrigger);

  buildValid(bytes);
  bytes[4] = static_cast<uint8_t>(M_PROTOCOL_VERSION | (2 << M_MODE_SHIFT));
  assert(ParseAdvert(bytes, M_UUID_BYTES, payload));
  assert(payload.mode == Mode::ReportOnly);

  // Mode 3 is reserved and must be refused rather than silently treated as one
  // of the others.
  buildValid(bytes);
  bytes[4] = static_cast<uint8_t>(M_PROTOCOL_VERSION | (3 << M_MODE_SHIFT));
  assert(!ParseAdvert(bytes, M_UUID_BYTES, payload));

  // BYTES 13-15 ARE EXTENSION SPACE FOR OTHER VARIANTS. Non-zero there must
  // still parse - validating them would make MFS_1 reject a future app build
  // the moment another variant starts using that space.
  buildValid(bytes);
  bytes[13] = 0xAA;
  bytes[14] = 0xBB;
  bytes[15] = 0xCC;
  assert(ParseAdvert(bytes, M_UUID_BYTES, payload));

  // Rejections.
  buildValid(bytes); bytes[0] = 'X';
  assert(!ParseAdvert(bytes, M_UUID_BYTES, payload));       // wrong magic
  buildValid(bytes); bytes[2] = 0x02;
  assert(!ParseAdvert(bytes, M_UUID_BYTES, payload));       // another variant
  buildValid(bytes); bytes[4] = 0x02;
  assert(!ParseAdvert(bytes, M_UUID_BYTES, payload));       // future version
  buildValid(bytes);
  assert(!ParseAdvert(bytes, M_UUID_BYTES - 1, payload));   // short

  // Activation count is range-checked: 0 and 17 are both invalid. An
  // out-of-range count would otherwise become a device that never triggers.
  buildValid(bytes); bytes[10] = 0;
  assert(!ParseAdvert(bytes, M_UUID_BYTES, payload));
  buildValid(bytes); bytes[10] = 17;
  assert(!ParseAdvert(bytes, M_UUID_BYTES, payload));
  buildValid(bytes); bytes[10] = 16;
  assert(ParseAdvert(bytes, M_UUID_BYTES, payload));        // boundary is valid

  // The test code may configure but must NEVER arm.
  buildValid(bytes);
  bytes[5] = bytes[6] = bytes[7] = bytes[8] = 0x00;
  bytes[9] = M_ARM_ACTIVE;
  assert(!ParseAdvert(bytes, M_UUID_BYTES, payload));
  bytes[9] = M_ARM_INACTIVE;
  assert(ParseAdvert(bytes, M_UUID_BYTES, payload));
  assert(payload.dayCode == 0);

  printf("protocol parse: OK\n");
}
```

- [ ] **Step 2: Run to verify it fails**

```bash
make test
```

Expected: link error — `undefined reference to 'alc::protocol::ParseAdvert'`.

- [ ] **Step 3: Implement**

Create `src/mfs_protocol.cpp`:

```cpp
#include "mfs_protocol.hpp"

namespace alc::protocol
{

  namespace
  {
    uint16_t readLe16(const uint8_t* bytes) { return static_cast<uint16_t>(bytes[0] | (bytes[1] << 8)); }

    uint32_t readLe32(const uint8_t* bytes)
    {
      return static_cast<uint32_t>(bytes[0]) | (static_cast<uint32_t>(bytes[1]) << 8) | (static_cast<uint32_t>(bytes[2]) << 16)
           | (static_cast<uint32_t>(bytes[3]) << 24);
    }

    constexpr uint32_t M_TEST_CODE { 0x00000000 };
  }

  bool ParseAdvert(const uint8_t* bytes, uint8_t length, Payload& out)
  {
    if (bytes == nullptr || length < M_UUID_BYTES) { return false; }

    if (bytes[M_OFFSET_MAGIC_0] != M_MAGIC_0 || bytes[M_OFFSET_MAGIC_1] != M_MAGIC_1) { return false; }
    if (readLe16(&bytes[M_OFFSET_DEVICE_TYPE]) != M_DEVICE_TYPE_MFS1) { return false; }

    // MASK. Byte 4's top two bits are the mode, so comparing the whole byte
    // would make every non-default mode look like the wrong protocol version.
    if ((bytes[M_OFFSET_VERSION] & M_VERSION_MASK) != M_PROTOCOL_VERSION) { return false; }

    Payload parsed;
    parsed.mode            = static_cast<Mode>((bytes[M_OFFSET_VERSION] >> M_MODE_SHIFT) & M_MODE_MASK);
    parsed.dayCode         = readLe32(&bytes[M_OFFSET_DAY_CODE]);
    parsed.armActive       = (bytes[M_OFFSET_ARM_STATE] & M_ARM_BIT) != 0;
    parsed.delayCode       = (bytes[M_OFFSET_ARM_STATE] >> M_DELAY_SHIFT) & M_DELAY_MASK;
    parsed.activations     = bytes[M_OFFSET_ACTIVATIONS];
    parsed.cooldownByte    = bytes[M_OFFSET_COOLDOWN];
    parsed.sensitivityByte = bytes[M_OFFSET_SENSITIVITY];

    // Reserved mode is refused rather than quietly treated as one of the others.
    // Silently downgrading an unknown mode to TriggerOnly would be worse: the
    // operator would believe a report had been configured.
    if (parsed.mode == Mode::Reserved) { return false; }

    // An out-of-range activation count would otherwise become a device that
    // silently never triggers, which is the worst failure an alarm sensor has.
    if (parsed.activations < M_ACTIVATIONS_MIN || parsed.activations > M_ACTIVATIONS_MAX) { return false; }

    // The settings-test code configures and nothing else. Refusing it here, in
    // the parser, means no later code path can be tricked into arming on it.
    if (parsed.dayCode == M_TEST_CODE && parsed.armActive) { return false; }

    // Bytes 13-15 are deliberately not examined. They belong to other MFS
    // variants and MFS_1 must tolerate whatever they contain.

    out = parsed;
    return true;
  }

}
```

Add to `Makefile`, changing the `SRCS` line:

```make
SRCS      = src/mfs_protocol.cpp $(TEST_SRCS)
```

- [ ] **Step 4: Run to verify it passes**

```bash
make test
```

Expected: `protocol tables: OK`, `protocol parse: OK`, `ALL TESTS PASSED`.

- [ ] **Step 5: Commit**

```bash
/Users/andy/nrfenv/bin/clang-format -i src/mfs_protocol.cpp
git add src/mfs_protocol.cpp tests/test_protocol.cpp tests/test_main.cpp Makefile
git commit -m "Add payload parsing with range and test-code validation

Two rejections are enforced in the parser rather than downstream. An
out-of-range activation count would become a device that silently never
triggers, and refusing the test code's arm request here means no later code
path can be tricked into arming on it.

Bytes 13-15 are deliberately not examined - a test asserts that non-zero
extension bytes still parse, because validating them would make MFS_1 reject a
future app build the moment another variant uses that space."
```

---

# PHASE 2 — Make the device the app's test target

## Task 3: CommandScanner rewrite

After this task MFS_1 decodes and logs every payload the app sends, field by field. That turns the real device into the app's test target and means phase 3 needs no stub.

**Files:**
- Modify: `src/command_scanner.hpp` (replace the `Command` enum with `Payload`)
- Modify: `src/command_scanner.cpp` (parse AD 0x07 instead of manufacturer data)
- Modify: `src/app.cpp` (the `TakePendingCommand()` call site)
- Modify: `CMakeLists.txt` (add `src/mfs_protocol.cpp`)

**Interfaces:**
- Consumes: `alc::protocol::ParseAdvert()`, `Payload` from Task 2.
- Produces: `bool CommandScanner::TakePendingPayload(protocol::Payload&)`. Tasks 9–10 consume it.

- [ ] **Step 1: Replace the scanner header**

In `src/command_scanner.hpp`, replace the `Command` enum and `TakePendingCommand()` with:

```cpp
#pragma once

#include <cstdint>

#include "mfs_protocol.hpp"

namespace alc
{

  /**
   * @brief BLE observer that receives engineer commands from advertisements.
   *
   * MFS_1 never advertises and never connects. The engineer's phone advertises a
   * 128-bit service UUID carrying the payload; this scans passively for it.
   *
   * The payload is an ABSOLUTE STATE ASSERTION, not a toggle, so hearing it
   * repeatedly is harmless. At the ~187 ms interval a phone actually advertises
   * at, the device will hear the same advert across several scan windows - so
   * this dedupes on payload identity rather than on a time window. The old 12 s
   * command cooldown existed only because the payload used to be a toggle, and
   * it could swallow a genuine second press.
   */
  class CommandScanner
  {
    public:
      CommandScanner();

      /**
       * @brief Enable Bluetooth and start the passive scan.
       * @return 0 on success; negative errno from bt_enable() or bt_le_scan_start().
       */
      int Start();

      /**
       * @brief Consume the pending payload, if any.
       *
       * Payloads arrive on the Bluetooth RX thread; this hands them to the main
       * loop so no application work happens in that context.
       *
       * @param out Populated when the return value is true.
       * @return True if a payload was pending.
       */
      bool TakePendingPayload(protocol::Payload& out);

    private:
      bool m_started;
  };

}
```

- [ ] **Step 2: Rewrite the scanner body**

In `src/command_scanner.cpp`, replace the file-scope constants and `parseAdvertData` with:

```cpp
namespace
{
  constexpr uint16_t M_UNITS_PER_MS_NUM { 8 };
  constexpr uint16_t M_UNITS_PER_MS_DEN { 5 };
  constexpr uint16_t M_SCAN_INTERVAL_UNITS { CONFIG_MFS_SCAN_PERIOD_MS * M_UNITS_PER_MS_NUM / M_UNITS_PER_MS_DEN };
  constexpr uint16_t M_SCAN_WINDOW_UNITS { CONFIG_MFS_SCAN_WINDOW_MS * M_UNITS_PER_MS_NUM / M_UNITS_PER_MS_DEN };

  // Complete list of 128-bit service UUIDs. iOS cannot send manufacturer data at
  // all, so the payload is smuggled into a service UUID - see the design spec
  // section 3. Proven on hardware 2026-09-12.
  constexpr uint8_t M_AD_UUID128_ALL { 0x07 };

  atomic_t s_payload_pending { 0 };
  protocol::Payload s_pending_payload {};
  uint8_t s_last_accepted[protocol::M_UUID_BYTES] {};
  bool s_have_last_accepted { false };

  bool parseAdvertData(struct bt_data* data, void* userData)
  {
    ARG_UNUSED(userData);

    if (data->type != M_AD_UUID128_ALL || data->data_len < protocol::M_UUID_BYTES) { return true; }

    protocol::Payload payload;
    if (!protocol::ParseAdvert(data->data, data->data_len, payload)) { return true; }

    // Dedupe on payload identity. The payload is an absolute state assertion, so
    // re-applying it would be harmless - but the device hears each advert
    // several times and the log would be unreadable.
    if (s_have_last_accepted && memcmp(s_last_accepted, data->data, protocol::M_UUID_BYTES) == 0) { return false; }

    memcpy(s_last_accepted, data->data, protocol::M_UUID_BYTES);
    s_have_last_accepted = true;
    s_pending_payload    = payload;
    atomic_set(&s_payload_pending, 1);

    // Decoded field by field on purpose: when a slider produces the wrong byte
    // this is where you see it, rather than inferring it from an LED.
    LOG_INF("Payload: code 0x%08X, arm %s, activations %u, cooldown %u s, threshold %u LSB.", payload.dayCode,
            payload.armActive ? "ACTIVE" : "INACTIVE", payload.activations, protocol::CooldownToSeconds(payload.cooldownByte),
            protocol::SensitivityToThresholdLsb(payload.sensitivityByte));

    return false;
  }
}
```

Replace `TakePendingCommand()` with:

```cpp
  bool CommandScanner::TakePendingPayload(protocol::Payload& out)
  {
    if (atomic_set(&s_payload_pending, 0) == 0) { return false; }

    out = s_pending_payload;
    return true;
  }
```

Add `#include <cstring>` and `#include "mfs_protocol.hpp"` to the includes.

- [ ] **Step 3: Update the call site in app.cpp**

Find the `TakePendingCommand()` call in `src/app.cpp` and replace it with the following. This is a **temporary** shim so the device stays usable this phase; Task 10 replaces it with the real handling.

```cpp
    // PHASE 2 SHIM. Applies only the arm bit so the device remains usable while
    // the app is built against it. Settings, day codes and activation counting
    // arrive in Task 10 - until then a payload only toggles arm state, and the
    // day code is logged but not validated.
    protocol::Payload payload;
    if (m_scanner.TakePendingPayload(payload)) {
      setArmState(payload.armActive ? ArmState::Active : ArmState::Inactive);
    }
```

Add `#include "mfs_protocol.hpp"` to `src/app.cpp`.

- [ ] **Step 4: Add the protocol source to the firmware build**

In `CMakeLists.txt`, add to `target_sources`:

```cmake
  src/mfs_protocol.cpp
```

- [ ] **Step 5: Build, flash and verify on hardware**

```bash
cd /Users/andy/nordic/ncs/v3.2.4/class_mfs_1
west build -b nrf54l15dk/nrf54l05/cpuapp -p always
west flash --dev-id 853003346 --recover
```

Expected in RTT: the normal boot sequence, and `Passive scan started: 100 ms window every 6000 ms.` Nothing decodes yet because nothing is advertising — that is Task 6.

To watch RTT: `open /Applications/SEGGER/JLink/JLinkRTTViewer.app`, device `nRF54L05_M33`, SWD, 4000 kHz.

- [ ] **Step 6: Commit**

```bash
/Users/andy/nrfenv/bin/clang-format -i src/command_scanner.cpp src/command_scanner.hpp src/app.cpp
git add src/command_scanner.cpp src/command_scanner.hpp src/app.cpp CMakeLists.txt
git commit -m "Parse the new payload from a 128-bit service UUID

Switches from manufacturer data to AD type 0x07, because iOS cannot send
manufacturer data at all. The device now decodes and logs every payload field by
field, which makes MFS_1 itself the app's test target and avoids building a stub
scanner for phase 3.

Dedupe is on payload identity rather than a time window. The payload is an
absolute state assertion so re-applying it is harmless, but the device hears
each advert several times at the ~187 ms interval a phone actually uses. This
also retires the old 12 s command cooldown, which existed only because the
payload used to be a toggle and could swallow a genuine second press.

The arm-bit handling in app.cpp is a deliberate temporary shim so the device
stays usable while the app is built; Task 10 replaces it."
```

---

# PHASE 3 — The app

## Task 4: Scaffold `class_app`

**Files:**
- Create: `/Users/andy/nordic/ncs/v3.2.4/class_app/` (Flutter project)

**Interfaces:**
- Consumes: nothing.
- Produces: a buildable Flutter project with `ble_peripheral` and Bluetooth entitlements. Tasks 5–6 build inside it.

- [ ] **Step 1: Create the project and add the dependency**

```bash
cd /Users/andy/nordic/ncs/v3.2.4
flutter create --org ie.alcsystems --platforms=ios,android,macos --project-name class_app class_app
cd class_app
flutter pub add ble_peripheral
git init && git add -A && git commit -m "Scaffold the CLASS control app"
```

- [ ] **Step 2: Add the Bluetooth permissions**

macOS needs a sandbox entitlement; iOS needs a usage string. Without these the radio silently never powers on.

```bash
cd /Users/andy/nordic/ncs/v3.2.4/class_app
for f in macos/Runner/DebugProfile.entitlements macos/Runner/Release.entitlements; do
  /usr/libexec/PlistBuddy -c "Add :com.apple.security.device.bluetooth bool true" "$f" 2>/dev/null \
    || /usr/libexec/PlistBuddy -c "Set :com.apple.security.device.bluetooth true" "$f"
done
/usr/libexec/PlistBuddy -c "Add :NSBluetoothAlwaysUsageDescription string 'Sends CLASS device commands over Bluetooth.'" ios/Runner/Info.plist 2>/dev/null
```

- [ ] **Step 3: Generate the Dart tables**

Now that `lib/protocol/` can exist, re-run the generator so it emits the Dart half:

```bash
mkdir -p /Users/andy/nordic/ncs/v3.2.4/class_app/lib/protocol
cd /Users/andy/nordic/ncs/v3.2.4/class_mfs_1
python3 tools/gen_protocol_tables.py
```

Expected: two `wrote ...` lines, the second pointing into `class_app`.

- [ ] **Step 4: Verify it builds**

```bash
cd /Users/andy/nordic/ncs/v3.2.4/class_app && flutter build macos --debug
```

Expected: `✓ Built build/macos/Build/Products/Debug/class_app.app`.

- [ ] **Step 5: Commit**

```bash
cd /Users/andy/nordic/ncs/v3.2.4/class_app
git add -A
git commit -m "Add ble_peripheral, Bluetooth entitlements and the generated tables

Entitlements are not optional: without them the radio silently never powers on,
which presents as an app that appears to work and a device that never hears it.

The encoding tables are generated from tools/gen_protocol_tables.py in the
class_mfs_1 repo so both sides cannot drift."
```

---

## Task 5: Protocol and advertiser

**Files:**
- Create: `class_app/lib/protocol/mfs_protocol.dart`
- Create: `class_app/lib/services/advertiser.dart`
- Create: `class_app/test/protocol_test.dart`

**Interfaces:**
- Consumes: `kThresholdTable`, `kCooldownTable` from `lib/protocol/tables.dart`.
- Produces: `Mfs1Payload`, `String buildUuid(Mfs1Payload)`, `Advertiser.send(String uuid)`. Task 6 consumes both.

- [ ] **Step 1: Write the failing test**

Create `class_app/test/protocol_test.dart`:

```dart
import 'package:flutter_test/flutter_test.dart';
import 'package:class_app/protocol/mfs_protocol.dart';
import 'package:class_app/protocol/tables.dart';

void main() {
  test('table anchors match the firmware', () {
    expect(kThresholdTable[0], 4000);
    expect(kThresholdTable[143], 302);
    expect(kThresholdTable[255], 40);
    expect(kCooldownTable[0], 0);
    expect(kCooldownTable[128], 60);
    expect(kCooldownTable[255], 3600);
  });

  test('payload bytes are laid out as the spec says', () {
    final bytes = buildPayloadBytes(const Mfs1Payload(
      dayCode: 0x11111111,
      armActive: true,
      activations: 3,
      cooldownByte: 128,
      sensitivityByte: 143,
    ));
    expect(bytes.length, 16);
    expect(bytes[0], 0x43); // 'C'
    expect(bytes[1], 0x4C); // 'L'
    expect(bytes[2], 0x01); // device type LE
    expect(bytes[3], 0x00);
    expect(bytes[4], 0x01); // version
    expect(bytes.sublist(5, 9), [0x11, 0x11, 0x11, 0x11]);
    expect(bytes[9], 0x01);
    expect(bytes[10], 3);
  });

  test('the two shared bytes pack both fields', () {
    final bytes = buildPayloadBytes(const Mfs1Payload(
      dayCode: 0,
      armActive: true,
      activations: 1,
      cooldownByte: 0,
      sensitivityByte: 0,
      delayCode: 119,               // 1 hour
      mode: Mfs1Mode.reportOnly,    // 0b10
    ));
    // Byte 4: version in the low six bits, mode in the top two.
    expect(bytes[4], 0x01 | (2 << 6));
    // Byte 9: arm bit set, delay code above it.
    expect(bytes[9], 0x01 | (119 << 1));
  });

  test('delay encoding is contiguous at both seams', () {
    expect(kDelayTable[0], 0);
    expect(kDelayTable[59], 59);
    expect(kDelayTable[60], 60);        // 1 min follows 59 s
    expect(kDelayTable[118], 3540);
    expect(kDelayTable[119], 3600);     // 1 h follows 59 min
    expect(kDelayTable[127], 32400);    // 9 h
    expect(formatDelay(0), 'none');
    expect(formatDelay(60), '1 min');
    expect(formatDelay(127), '9 h');
    expect(bytes[11], 128);
    expect(bytes[12], 143);
  });

  test('the UUID string is the on-air bytes REVERSED', () {
    // Bluetooth transmits a 128-bit UUID least-significant byte first. Proven on
    // hardware: string 0F0E...00 goes on air as 00 01 ... 0F. Get this backwards
    // and the device rejects every payload on the magic check.
    final uuid = buildUuidFromBytes(
        List<int>.generate(16, (i) => i)); // on air 00..0F
    expect(uuid.toUpperCase(), '0F0E0D0C-0B0A-0908-0706-050403020100');
  });
}
```

- [ ] **Step 2: Run to verify it fails**

```bash
cd /Users/andy/nordic/ncs/v3.2.4/class_app && flutter test
```

Expected: FAIL — `Target of URI doesn't exist: 'package:class_app/protocol/mfs_protocol.dart'`.

- [ ] **Step 3: Implement the protocol**

Create `class_app/lib/protocol/mfs_protocol.dart`:

```dart
import 'tables.dart';

/// Wire format — see class_mfs_1/docs/superpowers/specs/2026-09-12-app-control-design.md
/// section 4. Byte positions below are ON-AIR order.
const int kUuidBytes = 16;
const int kMagic0 = 0x43; // 'C'
const int kMagic1 = 0x4C; // 'L'
const int kDeviceTypeMfs1 = 0x0001;
const int kProtocolVersion = 0x01;

/// The settings-test code. Configures only — the device refuses to arm on it.
const int kTestDayCode = 0x00000000;

/// The ten hard-coded day codes for this phase.
const List<int> kDayCodes = <int>[
  0x11111111, 0x22222222, 0x33333333, 0x44444444, 0x55555555,
  0x66666666, 0x77777777, 0x88888888, 0x99999999, 0xAAAAAAAA,
];

/// What the device does when the activation count is reached.
///
/// Report and reportAndTrigger make the device ADVERTISE, which is an exception
/// to the standing rule that it never does. Offer them sparingly in the UI and
/// say what they cost — advertising forfeits covertness.
enum Mfs1Mode { triggerOnly, reportAndTrigger, reportOnly }

class Mfs1Payload {
  const Mfs1Payload({
    required this.dayCode,
    required this.armActive,
    required this.activations,
    required this.cooldownByte,
    required this.sensitivityByte,
    this.delayCode = 0,
    this.mode = Mfs1Mode.triggerOnly,
  });

  final int dayCode;
  final bool armActive;
  final int activations;   // 1..16
  final int cooldownByte;  // 0 = none
  final int sensitivityByte;
  final int delayCode;     // 0..127, see kDelayTable
  final Mfs1Mode mode;

  int get thresholdLsb => kThresholdTable[sensitivityByte];
  int get cooldownSeconds => kCooldownTable[cooldownByte];
  int get delaySeconds => kDelayTable[delayCode];
  double get thresholdMilligravity => thresholdLsb * 0.25;
}

/// Formats a delay code the way the encoding is defined, not as raw seconds:
/// the operator picked "5 minutes", so show "5 min".
String formatDelay(int code) {
  if (code == 0) return 'none';
  if (code < 60) return '$code s';
  if (code < 119) return '${code - 59} min';
  return '${code - 118} h';
}

/// Builds the 16 on-air bytes, least-significant first.
List<int> buildPayloadBytes(Mfs1Payload p) {
  final bytes = List<int>.filled(kUuidBytes, 0);
  bytes[0] = kMagic0;
  bytes[1] = kMagic1;
  bytes[2] = kDeviceTypeMfs1 & 0xFF;
  bytes[3] = (kDeviceTypeMfs1 >> 8) & 0xFF;
  // Byte 4 is shared: low six bits version, top two mode.
  bytes[4] = (kProtocolVersion & 0x3F) | ((p.mode.index & 0x03) << 6);
  bytes[5] = p.dayCode & 0xFF;
  bytes[6] = (p.dayCode >> 8) & 0xFF;
  bytes[7] = (p.dayCode >> 16) & 0xFF;
  bytes[8] = (p.dayCode >> 24) & 0xFF;
  // Byte 9 is shared: bit 0 arm state, bits 1-7 delay code.
  bytes[9] = (p.armActive ? 0x01 : 0x00) | ((p.delayCode & 0x7F) << 1);
  bytes[10] = p.activations;
  bytes[11] = p.cooldownByte;
  bytes[12] = p.sensitivityByte;
  // Bytes 13-15 stay zero. They are reserved for other MFS variants.
  return bytes;
}

/// Formats on-air bytes as a UUID string.
///
/// Bluetooth transmits a 128-bit UUID least-significant byte first, so the
/// string is the on-air sequence REVERSED. Getting this backwards makes the
/// device reject every payload on its magic check, with no error anywhere.
String buildUuidFromBytes(List<int> onAir) {
  final hex = onAir.reversed
      .map((b) => b.toRadixString(16).padLeft(2, '0'))
      .join()
      .toUpperCase();
  return '${hex.substring(0, 8)}-${hex.substring(8, 12)}-'
      '${hex.substring(12, 16)}-${hex.substring(16, 20)}-${hex.substring(20)}';
}

String buildUuid(Mfs1Payload p) => buildUuidFromBytes(buildPayloadBytes(p));
```

- [ ] **Step 4: Run to verify it passes**

```bash
flutter test
```

Expected: `All tests passed!`

- [ ] **Step 5: Implement the advertiser**

Create `class_app/lib/services/advertiser.dart`:

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
}
```

- [ ] **Step 6: Commit**

```bash
cd /Users/andy/nordic/ncs/v3.2.4/class_app
git add lib/protocol lib/services test
git commit -m "Add payload construction and the advertiser

The UUID string is the on-air bytes reversed, which a test pins explicitly:
Bluetooth sends a 128-bit UUID least-significant byte first, and getting it
backwards makes the device reject every payload on its magic check with no error
anywhere to explain it.

The 30 s advertising window is measured, not chosen. A phone advertises at
~187 ms and the interval is not ours to set, so each 100 ms device wake detects
with probability ~53%; thirty seconds spans six wakes for ~99%."
```

---

## Task 6: The MFS_1 control screen

**Files:**
- Create: `class_app/lib/devices/mfs1/mfs1_settings.dart`
- Create: `class_app/lib/devices/mfs1/mfs1_screen.dart`
- Modify: `class_app/lib/main.dart`

**Interfaces:**
- Consumes: `Mfs1Payload`, `buildUuid()`, `Advertiser` from Task 5.
- Produces: the running app. Nothing later consumes it.

- [ ] **Step 1: Write the settings model**

Create `class_app/lib/devices/mfs1/mfs1_settings.dart`:

```dart
import 'package:flutter/foundation.dart';

import '../../protocol/mfs_protocol.dart';

class Mfs1Settings extends ChangeNotifier {
  int activations = 1;
  int cooldownByte = 0;
  int sensitivityByte = 143; // the firmware's present default, ~75 mg
  int delayCode = 0;         // no delay - the present behaviour
  Mfs1Mode mode = Mfs1Mode.triggerOnly;
  bool armActive = false;

  /// Codes this app believes it has spent.
  ///
  /// A GUESS, not device state. The device is radio-silent and is the sole
  /// arbiter; this list exists only so the engineer is not asked to remember.
  /// It can diverge - hence [resetUsedCodes].
  final Set<int> usedCodes = <int>{};

  List<int> get availableCodes =>
      kDayCodes.where((int c) => !usedCodes.contains(c)).toList();

  void consume(int code) {
    usedCodes.add(code);
    // The device releases all ten once all ten are spent; mirror that.
    if (usedCodes.length >= kDayCodes.length) usedCodes.clear();
    notifyListeners();
  }

  void resetUsedCodes() {
    usedCodes.clear();
    notifyListeners();
  }

  void update({
    int? activations,
    int? cooldownByte,
    int? sensitivityByte,
    int? delayCode,
    Mfs1Mode? mode,
  }) {
    if (activations != null) this.activations = activations;
    if (cooldownByte != null) this.cooldownByte = cooldownByte;
    if (sensitivityByte != null) this.sensitivityByte = sensitivityByte;
    if (delayCode != null) this.delayCode = delayCode;
    if (mode != null) this.mode = mode;
    notifyListeners();
  }

  Mfs1Payload payload(int dayCode, bool arm) => Mfs1Payload(
        dayCode: dayCode,
        armActive: arm,
        activations: activations,
        cooldownByte: cooldownByte,
        sensitivityByte: sensitivityByte,
        delayCode: delayCode,
        mode: mode,
      );
}
```

- [ ] **Step 2: Write the screen**

Create `class_app/lib/devices/mfs1/mfs1_screen.dart`:

```dart
import 'package:flutter/material.dart';

import '../../protocol/mfs_protocol.dart';
import '../../services/advertiser.dart';
import 'mfs1_settings.dart';

class Mfs1Screen extends StatefulWidget {
  const Mfs1Screen({super.key});

  @override
  State<Mfs1Screen> createState() => _Mfs1ScreenState();
}

class _Mfs1ScreenState extends State<Mfs1Screen> {
  final Mfs1Settings _settings = Mfs1Settings();
  final Advertiser _advertiser = Advertiser();

  @override
  void initState() {
    super.initState();
    _settings.addListener(_refresh);
    _advertiser.addListener(_refresh);
    _advertiser.initialise();
  }

  @override
  void dispose() {
    _settings.removeListener(_refresh);
    _advertiser.removeListener(_refresh);
    _advertiser.dispose();
    super.dispose();
  }

  void _refresh() => setState(() {});

  /// Sends the current settings.
  ///
  /// While DEACTIVATED the test code goes out, so tuning costs no day codes.
  /// Changing the arm state prompts for a real one.
  Future<void> _send() async {
    int code = kTestDayCode;

    if (_settings.armActive) {
      final int? chosen = await _promptForCode();
      if (chosen == null) return;
      code = chosen;
    }

    await _advertiser.send(buildUuid(_settings.payload(code, _settings.armActive)));
    if (code != kTestDayCode) _settings.consume(code);
  }

  Future<int?> _promptForCode() {
    return showDialog<int>(
      context: context,
      builder: (BuildContext c) => SimpleDialog(
        title: const Text('Day code'),
        children: <Widget>[
          for (final int code in _settings.availableCodes)
            SimpleDialogOption(
              onPressed: () => Navigator.pop(c, code),
              child: Text('0x${code.toRadixString(16).toUpperCase().padLeft(8, '0')}'),
            ),
          SimpleDialogOption(
            onPressed: () => Navigator.pop(c),
            child: const Text('Cancel'),
          ),
        ],
      ),
    );
  }

  @override
  Widget build(BuildContext context) {
    final bool tuning = !_settings.armActive;

    return Scaffold(
      appBar: AppBar(title: const Text('MFS_1')),
      body: ListView(
        padding: const EdgeInsets.all(16),
        children: <Widget>[
          SwitchListTile(
            title: const Text('Armed'),
            subtitle: Text(tuning
                ? 'Deactivated — settings send with the test code'
                : 'Sending will prompt for a day code'),
            value: _settings.armActive,
            onChanged: (bool v) => setState(() => _settings.armActive = v),
          ),
          const Divider(),

          const Text('Activations before triggering'),
          Wrap(
            spacing: 6,
            children: <Widget>[
              for (int n = 1; n <= 16; n++)
                ChoiceChip(
                  label: Text('$n'),
                  selected: _settings.activations == n,
                  onSelected: (_) => _settings.update(activations: n),
                ),
            ],
          ),
          const SizedBox(height: 16),

          // Only meaningful with more than one activation - with N = 1 every
          // activation triggers, so there is nothing to blank between.
          if (_settings.activations > 1) ...<Widget>[
            Text('Cooldown between activations — '
                '${kCooldownTable[_settings.cooldownByte]} s'),
            Slider(
              value: _settings.cooldownByte.toDouble(),
              max: 255,
              divisions: 255,
              onChanged: (double v) =>
                  _settings.update(cooldownByte: v.round()),
            ),
            const SizedBox(height: 16),
          ],

          Text('Delay before triggering — '
              '${formatDelay(_settings.delayCode)}'),
          Slider(
            value: _settings.delayCode.toDouble(),
            max: 127,
            divisions: 127,
            onChanged: (double v) => _settings.update(delayCode: v.round()),
          ),
          if (_settings.delayCode > 0)
            const Padding(
              padding: EdgeInsets.only(bottom: 8),
              child: Text(
                'The device stays awake and scans continuously while a trigger '
                'is pending, so deactivating cancels it promptly.',
                style: TextStyle(fontSize: 11),
              ),
            ),
          const SizedBox(height: 16),

          const Text('Operating mode'),
          SegmentedButton<Mfs1Mode>(
            segments: const <ButtonSegment<Mfs1Mode>>[
              ButtonSegment<Mfs1Mode>(
                  value: Mfs1Mode.triggerOnly, label: Text('Trigger')),
              ButtonSegment<Mfs1Mode>(
                  value: Mfs1Mode.reportAndTrigger, label: Text('Report+Trig')),
              ButtonSegment<Mfs1Mode>(
                  value: Mfs1Mode.reportOnly, label: Text('Report')),
            ],
            selected: <Mfs1Mode>{_settings.mode},
            onSelectionChanged: (Set<Mfs1Mode> v) =>
                _settings.update(mode: v.first),
          ),
          // The warning is not decoration. The device is scan-only for its whole
          // life by design, and these modes are documented exceptions to that.
          if (_settings.mode != Mfs1Mode.triggerOnly)
            const Padding(
              padding: EdgeInsets.only(top: 8),
              child: Text(
                'This device will TRANSMIT when triggered. Advertising forfeits '
                'covertness — use only when necessary.',
                style: TextStyle(fontSize: 11, fontWeight: FontWeight.bold),
              ),
            ),
          const SizedBox(height: 16),

          Text('Sensitivity — '
              '${(kThresholdTable[_settings.sensitivityByte] * 0.25).toStringAsFixed(1)} mg'),
          const Text('less sensitive  →  more sensitive',
              style: TextStyle(fontSize: 11)),
          Slider(
            value: _settings.sensitivityByte.toDouble(),
            max: 255,
            divisions: 255,
            onChanged: (double v) =>
                _settings.update(sensitivityByte: v.round()),
          ),
          const SizedBox(height: 24),

          FilledButton(
            onPressed: _advertiser.ready && !_advertiser.advertising ? _send : null,
            child: Text(_advertiser.advertising
                ? 'Advertising… ${_advertiser.secondsLeft} s'
                : 'Send'),
          ),
          if (_advertiser.advertising)
            const Padding(
              padding: EdgeInsets.only(top: 8),
              child: Text(
                'Watch the device LED. Nothing is ever acknowledged — the device '
                'is radio-silent by design.',
                style: TextStyle(fontSize: 12),
              ),
            ),
          if (!_advertiser.ready)
            const Padding(
              padding: EdgeInsets.only(top: 8),
              child: Text('Bluetooth is off.'),
            ),
          TextButton(
            onPressed: _settings.resetUsedCodes,
            child: Text('Reset used codes '
                '(${_settings.availableCodes.length}/${kDayCodes.length} left)'),
          ),
        ],
      ),
    );
  }
}
```

Replace `class_app/lib/main.dart` with:

```dart
import 'package:flutter/material.dart';

import 'devices/mfs1/mfs1_screen.dart';

void main() => runApp(const ClassApp());

class ClassApp extends StatelessWidget {
  const ClassApp({super.key});

  @override
  Widget build(BuildContext context) => MaterialApp(
        title: 'CLASS',
        theme: ThemeData(useMaterial3: true, colorSchemeSeed: Colors.teal),
        home: const Mfs1Screen(),
      );
}
```

Add the tables import to `mfs1_screen.dart`:

```dart
import '../../protocol/tables.dart';
```

- [ ] **Step 3: Build and run against the real device**

MFS_1 must be powered with Task 3's firmware, and RTT open.

```bash
cd /Users/andy/nordic/ncs/v3.2.4/class_app
flutter test
flutter run -d macos
```

Move the sliders, press Send, and watch RTT. Expected, within 30 seconds:

```
<inf> scanner: Payload: code 0x00000000, arm INACTIVE, activations 1, cooldown 0 s, threshold 302 LSB.
```

**Verify the values in RTT match the app's readouts exactly.** That comparison is the whole point of this phase: a mismatch here is a protocol bug caught at the cheapest possible moment.

- [ ] **Step 4: Measure the iPhone advertising interval**

This closes the last open item in the spec (§9.2). With the phone connected and Developer Mode on:

```bash
flutter run -d <iphone-device-id>
```

Press Send and note the RTT timestamps of consecutive `Payload:` lines — dedupe suppresses repeats, so temporarily comment out the dedupe check in `command_scanner.cpp` to see every advert. If the interval differs materially from 187 ms, update `kAdvertiseWindow` and the spec's detection table.

- [ ] **Step 5: Commit**

```bash
cd /Users/andy/nordic/ncs/v3.2.4/class_app
git add -A
git commit -m "Add the MFS_1 control screen

Send behaviour follows the arm toggle invisibly: deactivated it sends the test
code so tuning costs no day codes, and arming prompts for a real one which
travels with the final settings in the same payload. There is therefore no
window in which the device is armed with settings the engineer did not watch
being tested.

The cooldown slider is hidden at one activation, where there is nothing to blank
between. The used-code list is labelled as a guess and has a manual reset: the
device is the sole arbiter and the app can never confirm a send landed."
```

---

# PHASE 4 — Device behaviour

## Task 7: Settings persistence

**Files:**
- Create: `src/settings.hpp`, `src/settings.cpp`
- Create: `tests/test_settings.cpp`
- Modify: `CMakeLists.txt`, `Makefile`, `tests/test_main.cpp`

**Interfaces:**
- Consumes: `protocol::Payload`, `SensitivityToThresholdLsb()`, `CooldownToSeconds()`.
- Produces: `Settings` with `Load()`, `ApplyFrom(const protocol::Payload&) -> bool`, `Activations()`, `ThresholdLsb()`, `CooldownSeconds()`. Tasks 9–10 consume it.

- [ ] **Step 1: Write the failing test**

Create `tests/test_settings.cpp`. The NVS calls are behind `Load()`/`save()`, so the host test exercises the pure conversion and change-detection logic:

```cpp
#include <cassert>
#include <cstdio>

#include "settings.hpp"

void run_settings_tests()
{
  using namespace alc;

  Settings settings;

  // Defaults match the firmware's historical Kconfig values, so a device that
  // has never been configured behaves exactly as it did before this feature.
  assert(settings.Activations() == 1);
  assert(settings.ThresholdLsb() == 302);
  assert(settings.CooldownSeconds() == 0);

  // No delay, Trigger-only. A device that has never been configured must not
  // acquire a delay or start advertising.
  assert(settings.DelaySeconds() == 0);
  assert(settings.OperatingMode() == protocol::Mode::TriggerOnly);

  protocol::Payload payload;
  payload.activations     = 3;
  payload.cooldownByte    = 128;
  payload.sensitivityByte = 255;
  payload.delayCode       = 119;                        // 1 hour
  payload.mode            = protocol::Mode::ReportOnly;

  assert(settings.ApplyFrom(payload));   // changed
  assert(settings.Activations() == 3);
  assert(settings.CooldownSeconds() == 60);
  assert(settings.ThresholdLsb() == 40);
  assert(settings.DelaySeconds() == 3600);
  assert(settings.OperatingMode() == protocol::Mode::ReportOnly);

  // A change to the delay ALONE must still be detected. Missing a field in the
  // comparison would leave the device running an old delay while the app
  // showed the new one.
  payload.delayCode = 127;
  assert(settings.ApplyFrom(payload));
  assert(settings.DelaySeconds() == 32400);

  // ApplyFrom reports whether anything actually changed. NVS is rated 10,000
  // cycles per word line and the device hears the same settings repeatedly, so
  // writing unconditionally would burn endurance for nothing.
  assert(!settings.ApplyFrom(payload));  // unchanged

  printf("settings: OK\n");
}
```

- [ ] **Step 2: Run to verify it fails**

```bash
make test
```

Expected: FAIL — `settings.hpp: No such file or directory`.

- [ ] **Step 3: Implement**

Create `src/settings.hpp`:

```cpp
#pragma once

#include <cstdint>

#include "mfs_protocol.hpp"

namespace alc
{

  /**
   * @brief The three engineer-settable parameters, NVS-backed.
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
       * @brief Adopt the parameters from a payload and persist if they changed.
       *
       * @return True if anything changed. **Only write NVS on change** - RRAM is
       *         rated 10,000 cycles per word line and the device hears the same
       *         settings repeatedly.
       */
      bool ApplyFrom(const protocol::Payload& payload);

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

Create `src/settings.cpp`:

```cpp
#include "settings.hpp"

#if defined(__ZEPHYR__)
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

#if defined(__ZEPHYR__)
    constexpr const char* M_SETTINGS_KEY { "mfs/params" };
#endif
  }

  Settings::Settings()
      : m_activations(M_DEFAULT_ACTIVATIONS)
      , m_cooldown_byte(M_DEFAULT_COOLDOWN_BYTE)
      , m_sensitivity_byte(M_DEFAULT_SENSITIVITY_BYTE)
      , m_delay_code(M_DEFAULT_DELAY_CODE)
      , m_mode(M_DEFAULT_MODE)
  {}

  bool Settings::ApplyFrom(const protocol::Payload& payload)
  {
    if (payload.activations == m_activations && payload.cooldownByte == m_cooldown_byte
        && payload.sensitivityByte == m_sensitivity_byte && payload.delayCode == m_delay_code && payload.mode == m_mode) {
      return false;
    }

    m_activations      = payload.activations;
    m_cooldown_byte    = payload.cooldownByte;
    m_sensitivity_byte = payload.sensitivityByte;
    m_delay_code       = payload.delayCode;
    m_mode             = payload.mode;
    save();
    return true;
  }

#if defined(__ZEPHYR__)

  int Settings::Load()
  {
    int result { settings_subsys_init() };
    if (result < 0) {
      LOG_ERR("settings_subsys_init failed: %d!", result);
      return result;
    }
    return settings_load_subtree("mfs");
  }

  int Settings::save()
  {
    uint8_t record[5] { m_activations, m_cooldown_byte, m_sensitivity_byte, m_delay_code, static_cast<uint8_t>(m_mode) };
    int result { settings_save_one(M_SETTINGS_KEY, record, sizeof(record)) };

    if (result < 0) { LOG_ERR("Failed to persist settings: %d!", result); }
    return result;
  }

#else

  // Host build: the conversion and change-detection logic is what the tests
  // exercise; persistence is a Zephyr concern.
  int Settings::Load() { return 0; }
  int Settings::save() { return 0; }

#endif

}
```

Add `src/settings.cpp` to both `CMakeLists.txt` (`target_sources`) and `Makefile` (`SRCS`), and `run_settings_tests();` to `tests/test_main.cpp` with its forward declaration.

- [ ] **Step 4: Run to verify it passes**

```bash
make test
```

Expected: `settings: OK` and `ALL TESTS PASSED`.

- [ ] **Step 5: Commit**

```bash
/Users/andy/nrfenv/bin/clang-format -i src/settings.cpp src/settings.hpp
git add src/settings.cpp src/settings.hpp tests/ Makefile CMakeLists.txt
git commit -m "Add NVS-backed settings

Stores the raw wire bytes rather than converted values, so the generated table
stays the single place a byte becomes a physical quantity.

ApplyFrom() reports whether anything actually changed and only writes on change.
RRAM is rated 10,000 cycles per word line and the device hears the same settings
repeatedly, so writing unconditionally would burn endurance for nothing.

Defaults reproduce the previous Kconfig values, so a device that has never been
configured behaves exactly as it did before this feature."
```

---

## Task 8: Day codes

**Files:**
- Create: `src/day_codes.hpp`, `src/day_codes.cpp`
- Create: `tests/test_day_codes.cpp`
- Modify: `CMakeLists.txt`, `Makefile`, `tests/test_main.cpp`

**Interfaces:**
- Consumes: nothing from earlier tasks.
- Produces: `DayCodes` with `Load()`, `IsTestCode(uint32_t)`, `Consume(uint32_t) -> bool`. Task 10 consumes it.

- [ ] **Step 1: Write the failing test**

Create `tests/test_day_codes.cpp`:

```cpp
#include <cassert>
#include <cstdio>

#include "day_codes.hpp"

void run_day_code_tests()
{
  using namespace alc;

  DayCodes codes;

  assert(codes.IsTestCode(0x00000000));
  assert(!codes.IsTestCode(0x11111111));

  // A real code works exactly once.
  assert(codes.Consume(0x11111111));
  assert(!codes.Consume(0x11111111));

  // A code that is not in the table is refused.
  assert(!codes.Consume(0x12345678));

  // The test code is never consumed and never valid as a code to spend.
  assert(!codes.Consume(0x00000000));

  // Spend the remaining nine; the tenth releases all ten for the next cycle.
  const uint32_t rest[] { 0x22222222, 0x33333333, 0x44444444, 0x55555555,
                          0x66666666, 0x77777777, 0x88888888, 0x99999999, 0xAAAAAAAA };
  for (uint32_t code : rest) { assert(codes.Consume(code)); }

  // All ten spent -> released. The first one works again.
  assert(codes.Consume(0x11111111));

  printf("day codes: OK\n");
}
```

- [ ] **Step 2: Run to verify it fails**

```bash
make test
```

Expected: FAIL — `day_codes.hpp: No such file or directory`.

- [ ] **Step 3: Implement**

Create `src/day_codes.hpp`:

```cpp
#pragma once

#include <cstdint>

namespace alc
{

  /**
   * @brief The day-code table and its used-mask.
   *
   * This phase uses ten hard-coded constants. Derivation from the TAN seed is
   * deferred - see docs/tan-scheme.md. The shape is kept deliberately close to
   * what the real scheme needs: a validate-then-consume call, a persisted mask,
   * and a device that is the sole arbiter.
   *
   * When all ten are spent the mask clears and all ten become available again.
   * That is a TESTING convenience and must not survive into production, where a
   * spent code stays spent.
   */
  class DayCodes
  {
    public:
      static constexpr uint8_t M_CODE_COUNT { 10 };
      static constexpr uint32_t M_TEST_CODE { 0x00000000 };

      DayCodes();

      /** @brief Load the used-mask from NVS. */
      int Load();

      /** @brief Whether this is the settings-test code. */
      bool IsTestCode(uint32_t code) const { return code == M_TEST_CODE; }

      /**
       * @brief Validate a code and, if good, mark it used and persist.
       *
       * @return True if the code was valid and unused. False for an unknown
       *         code, an already-used code, or the test code - which is never
       *         spendable.
       */
      bool Consume(uint32_t code);

      /** @brief How many codes remain unused. Diagnostics only. */
      uint8_t RemainingCount() const;

    private:
      int save();

      uint16_t m_used_mask;
  };

}
```

Create `src/day_codes.cpp`:

```cpp
#include "day_codes.hpp"

#if defined(__ZEPHYR__)
#include <zephyr/logging/log.h>
#include <zephyr/settings/settings.h>
LOG_MODULE_REGISTER(day_codes, LOG_LEVEL_INF);
#endif

namespace alc
{

  namespace
  {
    // Hard-coded for this phase. Replaced by TAN derivation - see
    // docs/tan-scheme.md. Order defines the bit position in the used-mask.
    constexpr uint32_t M_CODES[DayCodes::M_CODE_COUNT] {
      0x11111111, 0x22222222, 0x33333333, 0x44444444, 0x55555555,
      0x66666666, 0x77777777, 0x88888888, 0x99999999, 0xAAAAAAAA,
    };

    constexpr uint16_t M_ALL_USED { 0x03FF }; // ten bits

#if defined(__ZEPHYR__)
    constexpr const char* M_SETTINGS_KEY { "mfs/codes" };
#endif
  }

  DayCodes::DayCodes()
      : m_used_mask(0)
  {}

  bool DayCodes::Consume(uint32_t code)
  {
    // The test code configures and nothing else. It is never spendable, so it
    // can never exhaust the cycle or be mistaken for a real code.
    if (code == M_TEST_CODE) { return false; }

    for (uint8_t index = 0; index < M_CODE_COUNT; index++) {
      if (M_CODES[index] != code) { continue; }

      uint16_t bit { static_cast<uint16_t>(1U << index) };
      if ((m_used_mask & bit) != 0) { return false; }

      m_used_mask = static_cast<uint16_t>(m_used_mask | bit);

      // Testing convenience: once all ten are spent, release all ten. In
      // production a spent code stays spent - see docs/tan-scheme.md.
      if (m_used_mask == M_ALL_USED) { m_used_mask = 0; }

      save();
      return true;
    }

    return false;
  }

  uint8_t DayCodes::RemainingCount() const
  {
    uint8_t remaining { 0 };

    for (uint8_t index = 0; index < M_CODE_COUNT; index++) {
      if ((m_used_mask & (1U << index)) == 0) { remaining++; }
    }
    return remaining;
  }

#if defined(__ZEPHYR__)

  int DayCodes::Load() { return settings_load_subtree("mfs"); }

  int DayCodes::save()
  {
    int result { settings_save_one(M_SETTINGS_KEY, &m_used_mask, sizeof(m_used_mask)) };

    if (result < 0) { LOG_ERR("Failed to persist the day-code mask: %d!", result); }
    return result;
  }

#else

  int DayCodes::Load() { return 0; }
  int DayCodes::save() { return 0; }

#endif

}
```

Add `src/day_codes.cpp` to `CMakeLists.txt` and `Makefile`, and `run_day_code_tests();` to `tests/test_main.cpp`.

- [ ] **Step 4: Run to verify it passes**

```bash
make test
```

Expected: `day codes: OK` and `ALL TESTS PASSED`.

- [ ] **Step 5: Commit**

```bash
/Users/andy/nrfenv/bin/clang-format -i src/day_codes.cpp src/day_codes.hpp
git add src/day_codes.cpp src/day_codes.hpp tests/ Makefile CMakeLists.txt
git commit -m "Add the day-code table and its NVS used-mask

Ten hard-coded codes for this phase, with the shape kept close to what the real
TAN scheme needs: validate-then-consume, a persisted mask, and a device that is
the sole arbiter of what has been spent.

The test code is never spendable, so it cannot exhaust the cycle or be mistaken
for a real code. Releasing all ten once all ten are spent is a testing
convenience and is commented as such - in production a spent code stays spent."
```

---

## Task 9: The detection engine

**Files:**
- Modify: `src/app.hpp` (members), `src/app.cpp` (counting, cooldown, `updateOutputState()`)

**Interfaces:**
- Consumes: `Settings::Activations()`, `Settings::CooldownSeconds()`, `Npm2100` timer and GPIO API.
- Produces: `App::IsOutputActive()` semantics unchanged; `m_detection_met` latched internally.

- [ ] **Step 1: Add the members**

In `src/app.hpp`, add to the private section:

```cpp
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

- [ ] **Step 2: Add the cooldown helpers to app.cpp**

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
    if (result == 0) { result = m_pmic.TimerSetDurationMs(seconds * MSEC_PER_SEC); }
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
```

Declare both in `src/app.hpp` beside the other private methods.

> **Note on GPIO0.** The probe proved `GpioUsage::InterruptHi` asserts P1.06 high on expiry, which matches the overlay's `GPIO_ACTIVE_HIGH`. Polling `TimerIsExpired()` over I²C from the existing 100 ms loop is used here rather than a GPIO interrupt, because the loop already runs at that cadence and polling adds no new interrupt path. The GPIO route stays available if the loop ever slows.

- [ ] **Step 3: Rewrite updateOutputState()**

Replace the body of `App::updateOutputState()` in `src/app.cpp`, keeping the existing banner comment block above it:

```cpp
    bool awake { gpio_pin_get_dt(&s_adxl_int1) > 0 };

    if (m_ignore_stale_trigger && !awake) {
      m_ignore_stale_trigger = false;
      LOG_INF("ADXL cleared after arming - device is now live.");
    }

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

    // The trigger's own AWAKE running to completion is what clears the output.
    if (m_detection_met && !awake) { m_detection_met = false; }

    m_output_active = (m_arm_state == ArmState::Active) && m_detection_met;

    // Driven here, in the same breath as the condition is derived. A consumer at
    // the derivation point cannot be forgotten by a later edit to the loop.
    m_output_switch.Set(m_output_active);
```

Keep the existing stuck-AWAKE watchdog block that follows, unchanged.

- [ ] **Step 4: Call serviceCooldown() from the main loop**

Add `serviceCooldown();` immediately before the `updateOutputState();` call in the main loop.

- [ ] **Step 5: Build and bench-test**

```bash
west build -b nrf54l15dk/nrf54l05/cpuapp -p always
west flash --dev-id 853003346 --recover
```

With RTT open, use the app to set **activations = 3, cooldown = 8 s**, then arm. Tap the device three times with pauses. Expected:

```
<inf> app: Activation 1 of 3.
<inf> app: Cooldown started: 8 s.
<inf> app: Cooldown elapsed - detection re-armed.
<inf> app: Activation 2 of 3.
...
<inf> app: Activation 3 of 3.
<inf> app: Output ASSERTED.
```

**Verify that tapping during the cooldown does not increment the count** — that is the whole point of the blanking window.

- [ ] **Step 6: Commit**

```bash
/Users/andy/nrfenv/bin/clang-format -i src/app.cpp src/app.hpp
git add src/app.cpp src/app.hpp
git commit -m "Add activation counting and the cooldown blanking window

Counts RISING edges of AWAKE, not levels. AWAKE stays asserted for the whole
inactivity period, so counting the level would add one activation per loop tick.

Blanking never applies after the triggering activation - standing the ADXL down
at that moment would cut short the assertion that IS the output's 5 s duration.
Between counted activations the part is stood down for the window rather than
left running, because a continuous disturbance would otherwise hold AWAKE right
through the blanking period and the re-arm would inherit a stale level, which is
the bug 0a50910 fixed for the arming path.

The count does not expire: it clears only on trigger or deactivation, so the
device accumulates evidence of tampering over unlimited time."
```

---

## Task 9a: The trigger delay and its interlock — SAFETY CRITICAL

**A trigger that fires after the engineer deactivated the device is the worst
failure this product has.** With a delay of up to nine hours, that window is now
enormous. This task adds the delay and the doubled guard together — they must not
be separated, because the delay without the interlock is the hazard.

**Files:**
- Modify: `src/output_switch.hpp`, `src/output_switch.cpp` (add the interlock)
- Modify: `src/app.hpp`, `src/app.cpp` (delay timer, predicate, cancellation)

**Interfaces:**
- Consumes: `protocol::DelayToSeconds()`, `Payload::delayCode` from Task 1.
- Produces: `OutputSwitch::SetInterlock(InterlockFn, void*)`; `App::delayPermitsFiring()`. Task 10 installs the interlock.

- [ ] **Step 1: Add the interlock to OutputSwitch**

`OutputSwitch` must **not** learn what a delay is — coupling a general containment
to one feature destroys its value. It gains a predicate it consults instead.

In `src/output_switch.hpp`, add to the public section:

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
```

and to the private section:

```cpp
      InterlockFn m_interlock;
      void* m_interlock_context;
```

- [ ] **Step 2: Enforce it in Set()**

In `src/output_switch.cpp`, initialise both members to `nullptr` in the
constructor, add the setter, and insert the check in `Set()` immediately after the
`IsUsable()` guard and **before** `driveBoth(true)`:

```cpp
  void OutputSwitch::SetInterlock(InterlockFn interlock, void* context)
  {
    m_interlock         = interlock;
    m_interlock_context = context;
  }
```

```cpp
    // SECOND LAYER. The caller should already have declined to ask, so being
    // refused here means the first layer failed - a bug, not a routine
    // condition. Latch faulty and say so loudly rather than quietly declining.
    if (m_interlock != nullptr && !m_interlock(m_interlock_context)) {
      driveBoth(false);
      enterFaultState("interlock refused the assertion - a caller bypassed the derivation point", -EPERM);
      return -EPERM;
    }
```

- [ ] **Step 3: Add the delay state to App**

In `src/app.hpp`, add `#include <zephyr/kernel.h>` and to the private section:

```cpp
      // The delay's two independent witnesses. Deliberately different in kind so
      // that either being wrong still BLOCKS firing - see delayPermitsFiring().
      bool m_delay_pending;
      struct k_timer m_delay_timer;

      // Held for the whole delay so the SoC cannot enter a deeper state. Battery
      // life is explicitly not a factor while a trigger is pending.
      bool m_delay_pm_lock_held;

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

- [ ] **Step 4: Implement them in app.cpp**

```cpp
  bool App::delayPermitsFiring() const
  {
    // BOTH must agree. Not one, not either - both.
    return !m_delay_pending && k_timer_remaining_ticks(const_cast<struct k_timer*>(&m_delay_timer)) == 0;
  }

  bool App::interlockThunk(void* context) { return static_cast<const App*>(context)->delayPermitsFiring(); }

  void App::beginDelay()
  {
    uint16_t seconds { protocol::DelayToSeconds(m_settings.DelayCode()) };

    if (seconds == 0) { return; }

    // GRTC, not the PMIC timer. At +/-10% over temperature the PMIC would put a
    // 9-hour delay anywhere inside a 108-minute window.
    k_timer_start(&m_delay_timer, K_SECONDS(seconds), K_NO_WAIT);
    m_delay_pending = true;

    // Stay awake for the duration and scan harder. The deactivate path is the
    // most important thing the device does while a trigger is pending, and at
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
```

Initialise the timer in `App::Run()` before the main loop, and install the
interlock:

```cpp
    k_timer_init(&m_delay_timer, nullptr, nullptr);
    m_output_switch.SetInterlock(&App::interlockThunk, this);
```

Add `#include <zephyr/pm/policy.h>` to `src/app.cpp`.

- [ ] **Step 5: Fold the delay into the derivation point**

In `updateOutputState()`, replace the trigger-latch block from Task 9:

```cpp
      if (m_activation_count >= m_settings.Activations()) {
        m_activation_count = 0;
        if (protocol::DelayToSeconds(m_settings.DelayCode()) > 0) {
          beginDelay();          // m_detection_met waits for the timer
        } else {
          m_detection_met = true;
        }
      } else {
        beginCooldown();
      }
```

and service the delay's expiry alongside it:

```cpp
    // The delay elapsed and was not cancelled - commit the trigger.
    if (m_delay_pending && k_timer_remaining_ticks(&m_delay_timer) == 0) {
      cancelDelay();             // clears the flag and releases the PM lock
      m_detection_met = true;
    }
```

Then make the delay part of the condition itself — **layer one**:

```cpp
    m_output_active = (m_arm_state == ArmState::Active) && m_detection_met && delayPermitsFiring();
```

- [ ] **Step 6: Cancel on deactivation**

In `setArmState()`, on the transition to `Inactive`, **before** anything else:

```cpp
    if (state == ArmState::Inactive) {
      // UNCONDITIONAL. A pending trigger must not outlive disarming.
      cancelDelay();
      m_detection_met    = false;
      m_activation_count = 0;
    }
```

Note the pending delay is deliberately **not** persisted, so a reset also loses
the trigger. That is the fail-safe direction.

- [ ] **Step 7: Add fast-scan support to CommandScanner**

In `src/command_scanner.hpp` add `int SetFastScan(bool fast);`, and implement it in
`src/command_scanner.cpp` by restarting the scan with `window == interval` when
fast, and the configured pair otherwise:

```cpp
  int CommandScanner::SetFastScan(bool fast)
  {
    if (fast == m_fast) { return 0; }

    const struct bt_le_scan_param scanParam {
      .type     = BT_LE_SCAN_TYPE_PASSIVE,
      .options  = BT_LE_SCAN_OPT_NONE,
      .interval = fast ? M_SCAN_WINDOW_UNITS : M_SCAN_INTERVAL_UNITS,
      .window   = M_SCAN_WINDOW_UNITS,
    };

    bt_le_scan_stop();
    int result { bt_le_scan_start(&scanParam, &scanRecvCallback) };
    if (result < 0) {
      LOG_ERR("Failed to change scan cadence: %d!", result);
      return result;
    }

    m_fast = fast;
    LOG_INF("Scan cadence now %s.", fast ? "CONTINUOUS (trigger pending)" : "duty-cycled");
    return 0;
  }
```

Add `bool m_fast;` to its private section, initialised false.

- [ ] **Step 8: Bench-verify the abort path — this is the point of the task**

```bash
west build -b nrf54l15dk/nrf54l05/cpuapp -p always
west flash --dev-id 853003346 --recover
```

With RTT open and the app to hand:

1. Set **activations = 1, delay = 30 s**, arm the device.
2. Trigger it. Expect `TRIGGER PENDING: firing in 30 s. Deactivating cancels it.`
   and `Scan cadence now CONTINUOUS (trigger pending).`
3. **Wait the full 30 s.** Confirm the fire output asserts and LED B lights.
4. Repeat, but **deactivate at ~15 s**. Confirm:
   - the arm state goes Inactive,
   - **the output NEVER asserts**, then or later,
   - the scan returns to duty-cycled.
5. Repeat, and **reset the board mid-delay**. Confirm it comes up Inactive with no
   pending trigger.
6. Set delay = 0 and confirm firing is still immediate — the default path must be
   unchanged.

Step 4 is the one that matters. If the output asserts there, stop and fix it
before going further.

- [ ] **Step 9: Commit**

```bash
/Users/andy/nrfenv/bin/clang-format -i src/app.cpp src/app.hpp src/output_switch.cpp src/output_switch.hpp src/command_scanner.cpp src/command_scanner.hpp
git add src/app.cpp src/app.hpp src/output_switch.cpp src/output_switch.hpp src/command_scanner.cpp src/command_scanner.hpp
git commit -m "Add the trigger delay and its doubled interlock

A trigger firing after the engineer deactivated the device is the worst failure
this product has, and a delay of up to nine hours makes that window enormous. The
delay and the guard land together on purpose: the delay without the interlock is
the hazard.

Two witnesses must both agree no delay is pending before the output can assert,
and they are deliberately different in kind - a software flag and the kernel
timer. A flag left set with a dead timer blocks firing; a running timer with a
cleared flag also blocks firing. Both failure directions are safe, which is the
whole reason for using two.

OutputSwitch is NOT taught what a delay is - that would couple a general
containment to one feature. It gains an interlock predicate it consults instead,
giving two independent layers rather than the same test written twice. Being
refused at the second layer means the first failed, so it latches faulty and says
so rather than quietly declining.

Deactivating cancels unconditionally, and the pending state is deliberately not
persisted so a reset loses the trigger too - the fail-safe direction.

The SoC holds a PM lock and scans continuously for the whole delay. Battery life
is explicitly not a factor while a trigger is pending, and at the normal 6 s
cadence an abort would take ~30 s to be heard with confidence."
```

---

## Task 10: Payload handling and arm transitions

**Files:**
- Modify: `src/app.hpp` (add `Settings` and `DayCodes` members), `src/app.cpp` (replace the Task 3 shim)

**Interfaces:**
- Consumes: `Settings`, `DayCodes`, `protocol::Payload`.
- Produces: the complete behaviour. Nothing later consumes it.

- [ ] **Step 1: Add the members**

In `src/app.hpp`, add `#include "day_codes.hpp"` and `#include "settings.hpp"`, and to the private section:

```cpp
      Settings m_settings;
      DayCodes m_day_codes;
```

Initialise both in the constructor's initialiser list, after `m_scanner()`.

- [ ] **Step 2: Replace the Task 3 shim**

Replace the phase-2 shim in `src/app.cpp` with:

```cpp
    protocol::Payload payload;
    if (m_scanner.TakePendingPayload(payload)) { handlePayload(payload); }
```

- [ ] **Step 3: Implement handlePayload()**

Add to `src/app.cpp`, and declare it in `src/app.hpp`:

```cpp
  void App::handlePayload(const protocol::Payload& payload)
  {
    bool wantActive { payload.armActive };
    bool isTestCode { m_day_codes.IsTestCode(payload.dayCode) };

    // The test code configures only, and only while deactivated. The parser has
    // already refused a test code asking to arm; this is the second line.
    if (isTestCode) {
      if (m_arm_state != ArmState::Inactive) {
        LOG_WRN("Test code ignored: the device is armed.");
        return;
      }

      if (m_settings.ApplyFrom(payload)) {
        LOG_INF("Settings applied: %u activations, %u s cooldown, %u LSB.", m_settings.Activations(), m_settings.CooldownSeconds(),
                m_settings.ThresholdLsb());

        // Re-arm the engine so the new threshold takes effect immediately -
        // tuning is the whole purpose of this path.
        m_activation_count = 0;
        m_detection_met    = false;
        enableAccelerometer();
      }
      return;
    }

    // From here a real code is required. Note the ORDER: settings are applied
    // and the part configured BEFORE the code is consumed, so a failed arm
    // never burns a code.
    if (wantActive) {
      if (m_arm_state == ArmState::Active) { return; }

      m_settings.ApplyFrom(payload);

      // setArmState() performs the standby -> configure -> confirm-AWAKE-clear
      // sequence that makes arming edge-triggered.
      setArmState(ArmState::Active);
      if (m_arm_state != ArmState::Active) {
        LOG_ERR("Arming refused - the day code is NOT consumed and can be resent.");
        return;
      }

      if (!m_day_codes.Consume(payload.dayCode)) {
        // The code was bad. Undo the arm rather than leave the device armed on
        // an unauthenticated payload.
        LOG_WRN("Day code rejected - reverting to Inactive.");
        setArmState(ArmState::Inactive);
        return;
      }

      LOG_INF("Armed. %u day codes remain.", m_day_codes.RemainingCount());
      return;
    }

    if (m_arm_state == ArmState::Inactive) { return; }

    if (!m_day_codes.Consume(payload.dayCode)) {
      LOG_WRN("Day code rejected - staying armed.");
      return;
    }

    setArmState(ArmState::Inactive);
    m_activation_count = 0;
    m_detection_met    = false;
    LOG_INF("Deactivated. %u day codes remain.", m_day_codes.RemainingCount());
  }
```

> **Ordering note.** Validating the code *before* arming would burn it on a part that then fails to configure. Arming first and reverting on a bad code keeps both properties: a failed configure costs no code, and a bad code never leaves the device armed.

> **Mode note.** `Settings::OperatingMode()` is stored and honoured for
> `TriggerOnly` from this task. **`ReportOnly` and `ReportAndTrigger` must log a
> warning and behave as `TriggerOnly` until Task 12 lands** — the report payload
> is not specified yet, and silently doing nothing in `ReportOnly` would make a
> configured device look armed while being inert. Add to `handlePayload()`:
>
> ```cpp
>     if (payload.mode != protocol::Mode::TriggerOnly) {
>       LOG_WRN("Mode %u requested but reporting is not implemented - behaving as Trigger only!", static_cast<unsigned>(payload.mode));
>     }
> ```

- [ ] **Step 4: Load persisted state at boot**

In `App::Run()`, after `initPmic()` succeeds:

```cpp
    result = m_settings.Load();
    if (result < 0) { LOG_WRN("Settings not loaded (%d) - using defaults.", result); }

    result = m_day_codes.Load();
    if (result < 0) { LOG_WRN("Day-code mask not loaded (%d) - all codes available.", result); }
```

Replace the remaining `CONFIG_MFS_ADXL_THRESHOLD` uses in `enableAccelerometer()` and the stuck-AWAKE watchdog with `m_settings.ThresholdLsb()`.

- [ ] **Step 5: Build and bench-test the whole flow**

```bash
west build -b nrf54l15dk/nrf54l05/cpuapp -p always
west flash --dev-id 853003346 --recover
```

Walk the spec's §8.2 workflow end to end with the app:

1. Device boots Inactive, LED A on.
2. Adjust sliders, Send (test code) — settings log, no arm change.
3. Toggle to Armed, Send with a real code — device arms, LED A off, code count drops.
4. Trigger it — LED B and the fire output assert for ~5 s.
5. Toggle to Deactivated, Send with another code — device disarms.
6. **Resend a spent code** — expect `Day code rejected`.
7. **Power-cycle and resend a spent code** — still rejected, proving NVS persistence.

- [ ] **Step 6: Commit**

```bash
/Users/andy/nrfenv/bin/clang-format -i src/app.cpp src/app.hpp
git add src/app.cpp src/app.hpp
git commit -m "Wire day codes and settings into the arm transitions

Order is load-bearing. Settings are applied and the accelerometer configured
before the day code is consumed, so a part that fails to configure never burns a
code - the engineer simply sends it again. A bad code arriving after a
successful arm reverts the device rather than leaving it armed on an
unauthenticated payload.

The test code path is gated twice: the parser refuses a test code asking to arm,
and this refuses a test code while armed. Applying settings on that path re-arms
the engine so a new threshold takes effect immediately, which is the whole point
of tuning."
```

---

## Task 11: Correct the documents the design invalidates

Spec §10 lists these. Leaving them wrong is worse than leaving them absent — they are the documents a future reader trusts.

**Files:**
- Modify: `docs/v1-scope.md` §1.0, §1.1, §6
- Modify: `CLAUDE.md`

- [ ] **Step 1: Fix `docs/v1-scope.md`**

- **§1.0** — LED B is no longer the output mirror. Replace the `ledB = IsOutputActive();` worked example with `m_output_switch.Set(IsOutputActive());` and note that LED B is now a tuning indicator driven by `m_detection_met`.
- **§1.1** — remove the single toggle, the deferred configuration and the deferred BLE payload from the deferred list.
- **§6** — record that arm-state persistence is answered: NVS this phase, with nPM2100 SCRATCHA as the intended refinement because it survives a brownout but clears on battery removal, which is exactly the fail-safe §6 wanted and NVS cannot give.

- [ ] **Step 2: Fix `CLAUDE.md`**

- Replace "**The counterpart must advertise at 20–50 ms.**" with the measured finding: a phone advertises at ~187 ms, the interval is not ours to set, and the app compensates with a 30 s advertising window for ~99 % detection across six wakes.
- Replace the Thingy:53 toggle-tool section with a note that it is **retired**, superseded by `../class_app`.
- Replace the opening "this project is a **fresh skeleton**" paragraph — stale since 2026-08-17 — with the current state.
- Add `../class_app` to the workspace-context tree.
- Add a build-commands note: **always flash this board with `--recover`.**

- [ ] **Step 3: Commit**

```bash
git add docs/v1-scope.md CLAUDE.md
git commit -m "Correct the documents the app-control work invalidates

LED B is no longer the output mirror, so v1-scope section 1.0's worked example
now points at OutputSwitch - the example future consumers copy has to be the
real thing rather than a stand-in.

CLAUDE.md's claim that the counterpart must advertise at 20-50 ms is disproved:
a phone advertises at ~187 ms and the interval is not ours to set. It also still
opened by calling this repo a fresh skeleton, stale since 2026-08-17."
```

---

## Task 12: Report modes — BLOCKED, do not start

**This task cannot begin until the report payload is specified** (spec §6.5.2).
It is listed so the gap is visible rather than forgotten, and so Tasks 1–11 can
ship a complete, useful Trigger-only device without it.

**What is blocking.** The hub needs to know *which* sensor fired, so the report
carries a device identifier. **A stable identifier in a repeated broadcast is a
tracking beacon** for anyone listening — which is precisely what the scan-only
architecture exists to avoid. Choosing that identifier is a covertness decision,
not a formatting one, and guessing it would be the worst possible way to resolve
it.

Questions that must be answered first:

1. **What identifies the sensor to the hub?** The `HAH-MMYY-xxxx` serial is
   readable by anyone in range and is stable for the life of the device. A
   rotating or derived identifier costs the hub a lookup but does not follow the
   device around.
2. **How long is the burst, and at what interval?** `docs/power-budget.md` §8.7.1
   requires it be **bounded**. The hub's own scan duty cycle sets the floor, the
   same arithmetic as §3 of the design spec but in the opposite direction.
3. **Is the report authenticated?** An unauthenticated trigger report can be
   forged by anyone, which turns the hub into a false-alarm generator. The TAN
   seed is available; whether a truncated HMAC fits the payload is the question.
4. **What does the hub do with a report it cannot attribute?** Silently dropping
   it and loudly logging it are very different products.

**When unblocked, the work is roughly:** a `Reporter` class owning a bounded
advertising burst; a call site in `updateOutputState()` immediately before
`m_output_switch.Set(true)` for `ReportAndTrigger`, and in place of it for
`ReportOnly`; removal of the Task 10 warning shim; and the `CLAUDE.md` rewording
from Task 11 extended with the burst's actual bounds.

---

## Self-Review

**Spec coverage.** §1 scope → Tasks 1, 7. §2 decisions → all. §3 spike → Tasks 5, 6. §4 wire format → Tasks 1–3, 5. §5 encodings → Task 1. §6.1 units → Tasks 3, 7, 8, 9. §6.2 invariant → Task 9. §6.3 engine → Task 9. §6.4 activation order → Task 10. §6.5 test code → Tasks 2, 10. §6.6 silent failures → Task 2. §7 persistence → Tasks 7, 8. §8 app → Tasks 4–6. §9.1 PMIC — already done. §9.2 iPhone interval → Task 6 step 4. §9.3 fire pins — already done. §10 documents → Task 11.

**Type consistency.** `protocol::Payload` fields (`dayCode`, `armActive`, `activations`, `cooldownByte`, `sensitivityByte`) are identical across Tasks 2, 3, 7, 10 and mirrored in Dart in Task 5. `TakePendingPayload` is used with the same signature in Tasks 3 and 10. `Settings::ApplyFrom` returns `bool` consistently in Tasks 7 and 10. `DayCodes::Consume` returns `bool` in Tasks 8 and 10.

**Amendment coverage (2026-09-13).** Spec §4.3 modes → Tasks 1, 2, 5, 6, 7, 10 (gated), 12 (blocked). §5.1 delay encoding → Tasks 1, 2, 5. §5.1.1 GRTC not PMIC → Task 9a. §5.1.2 stay awake + fast scan → Task 9a steps 4, 7. §6.5.1 interlock → Task 9a steps 1, 2, 5, 6. §6.5.2 report ordering → Task 12.

**Known gaps, deliberately deferred.**

1. The `OutputSwitch` fault-latch paths are not exercised by any test — they need
   GPIO failure injection, which the workspace has no mock for. Hardware
   verification covers the working path only. Worth a mock seam if the class grows.
2. **The delay interlock has no automated test either**, for the same reason: both
   witnesses are Zephyr primitives. Task 9a step 8 is a scripted bench procedure
   instead, and **step 4 of it — deactivating mid-delay — is the single most
   important verification in this plan.**
3. Task 12 is blocked on a covertness decision and is not schedulable.
