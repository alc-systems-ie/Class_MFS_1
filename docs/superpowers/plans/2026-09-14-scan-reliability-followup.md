# Scan Reliability Follow-up (owner decisions) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development. Steps use checkbox (`- [ ]`) syntax.

**Goal:** Apply the owner's decisions of 2026-09-14: never arm without a running scanner; send only while the app is in the foreground; scan period 5876 ms, chosen and checked by a simulation script that replaces the unsound closed-form rule.

**Spec:** `docs/superpowers/specs/2026-09-14-scan-reliability-amendment.md` (to be amended by Task 2 with §6 "Owner decisions"), `docs/superpowers/specs/2026-09-14-arming-sequence-amendment.md`.

## Global Constraints

- House style `~/.claude/CLAUDE.md`; clang-format `/Users/andy/nrfenv/bin/clang-format -i`.
- **No scanner, no arming.** An Arm is refused when the scanner is not running, both when the Arm is accepted (no Arming state entered) and at the end of the exit delay before the armed restart (fail safe). A refusal raises the warning (LED B interim pattern) with a reason naming the scanner, gives no LED A acknowledgement, leaves the device Inactive with pins isolated and the test running. The check lives in the host-tested `ArmingSequence` via `ArmingActions`.
- **Scan period 5876 ms** (`CONFIG_MFS_SCAN_PERIOD_MS` default 5876 = 9401 BLE units = 5875.625 ms); window 100 ms unchanged; range 1000–10240 unchanged.
- **Period check = simulation**, `tools/scan_phase_check.py`: fraction of start phases for which N consecutive scans (period P, window W) all miss an advertiser at interval I, adverts as instants; defaults W = 100 ms, N = 5 (30 s command), P in BLE units × 0.625 ms. The closed-form `d × N ≥ I − W` rule is withdrawn everywhere.
- **Foreground-only sending (app):** Send is disabled unless the app lifecycle is `resumed`; when the app becomes `hidden`, `paused` or `detached` any advert (or pending start) is stopped at once and the page shows "Advertising stopped - keep the app open while sending." `inactive` (visible but unfocused, e.g. a macOS window losing focus or an iOS system sheet) does not stop an advert.
- Never flash, never touch the J-Link/RTT; never commit `credentials.conf` or `class_app/lib/services/bench_credentials.dart`; never stage the iOS build artefacts in class_app (`ios/Runner.xcodeproj/project.pbxproj`, `ios/Runner.xcworkspace/contents.xcworkspacedata`, `ios/Podfile.lock`).
- Commit trailer:
  ```
  Co-Authored-By: Claude Opus 5 (1M context) <noreply@anthropic.com>
  Claude-Session: https://claude.ai/code/session_01UySHgbzqJcwKqpRntC1PtN
  ```

---

### Task 1: Firmware — no scanner, no arming

**Files:** `src/arming_sequence.hpp`, `src/arming_sequence.cpp`, `tests/test_arming_sequence.cpp`, `src/app.hpp`, `src/app.cpp`.

**Interfaces:** `ArmingActions` gains `virtual bool ScannerRunning() const = 0;`; `ArmingStep` gains `ScannerCheck`. `BeginArming(nowMs)` returns false and raises `SignalWarning(ScannerCheck, -ENODEV)` (and records the failure for `TakeFailure`) when Inactive but `!ScannerRunning()`, leaving state Inactive and calling nothing else. `Service()` at the deadline checks `ScannerRunning()` first; if false → the existing fail-safe path (disable pins, Inactive, `RestartDetection(false)`, warning `ScannerCheck`) and no restart/enable.

- [ ] **Step 1: Failing tests:** (a) BeginArming with scanner down → false, state Inactive, call log contains only the warning (ScannerCheck, -ENODEV), TakeFailure reports ScannerCheck; (b) scanner up at BeginArming, down at the deadline → no RestartDetection(true)/EnablePins; DisablePins → RestartDetection(false) → warning ScannerCheck; never Active; (c) scanner up throughout → unchanged sequence (existing tests still pass); (d) the re-entrancy/session-token tests still pass.
- [ ] **Step 2:** implement; App implements `ScannerRunning()` as `m_scanner.IsScanning()` (must not call into the sequence); `armingStepReason` gains a scanner reason ("scanner not running - a disarm could not be heard"); applyCommand: an Arm refused by BeginArming logs `LOG_ERR("Arming refused: scanner not running - the device could not hear a disarm!")` and plays no LED A pattern (the warning is raised by the sequence).
- [ ] **Step 3:** make test green; firmware build zero warnings (delete build dir); clang-format; commit `Refuse arming when the scanner is not running`.

### Task 2: Scan period 5876 ms and the simulation script

**Files:** `Kconfig`, `tools/scan_phase_check.py` (new), `docs/superpowers/specs/2026-09-14-scan-reliability-amendment.md`, `CLAUDE.md`, `docs/power-budget.md`, `docs/v1-scope.md`, `docs/superpowers/plans/2026-09-13-bench-checklist.md`, `tools/uuid_observer/README.md` (pointer to the script).

- [ ] **Step 1: Script.** `tools/scan_phase_check.py` (python3, stdlib only): args `--period-units` (default 9401), `--window-ms` (100), `--scans` (5), `--steps` (2000), positional intervals in ms (default: 35 100 152.5 187.5 211.25 318.75 417.5 546.25 760 852.5 1022.5 1285); prints per interval the miss fraction and a verdict (`exempt` if I ≤ W, `PASS` if miss == 0, else `FAIL x%`); `--sweep MIN_UNITS MAX_UNITS` ranks periods by worst miss over the given intervals ≤ an optional `--max-interval-ms`. Include a docstring explaining the model and its limits (instantaneous adverts, no advDelay jitter, a command lasts N scans, intervals ≥ ~5 × W cannot be covered by any period). Self-check in the report: 6000 ms vs 187.5 → ~47 % miss; 5905.625 vs 211.25 → ~35 %; 5875.625 vs 152.5/187.5/211.25/318.75 → 0 %.
- [ ] **Step 2: Kconfig** default 5876; help: why (script results for measured Mac 187.5 ms and Apple intervals up to 318.75 ms), slow intervals (≳ 5 × W) cannot be covered by any period — foreground fast advertising in the app is the guarantee; run the script before changing the period. Remove the closed-form rule text.
- [ ] **Step 3: Docs.** Amendment §2: replace the closed-form rule and its table with the simulation method and a results table for 6000 / 5905.625 / 5875.625 ms against the default intervals (from the script output); remove the "OPEN — 211.25 ms fails" item (now passes) and add §6 "Owner decisions 2026-09-14" (no scanner no arming; foreground-only sending; 5876 ms + script). CLAUDE.md: settled table 5.876 s, the rule bullet → "check with tools/scan_phase_check.py", add the two owner rules to Access rules / tool platform. Power budget: recompute with its formulas for 100/5876 (show arithmetic). v1-scope: default note 5876. Bench checklist: boot expectation `every 5876 ms`; §5d Android item uses the script; add §5e: (1) with the scanner forced down is not bench-inducible → mark "host-tested only"; (2) app foreground: Send an Arm, switch away from the app → advert stops (observer), message shown; macOS: clicking another window (inactive) does not stop it.
- [ ] **Step 4:** make test + firmware build (confirm `CONFIG_MFS_SCAN_PERIOD_MS=5876`); commits `Scan period 5876 ms, checked by a phase simulation` (Kconfig + script + docs).

### Task 3: App — foreground-only sending

**Files (class_app):** `lib/services/advertiser.dart` or a small `lib/services/foreground_guard.dart`, the three sending pages, tests.

- [ ] **Step 1: Failing tests:** lifecycle `hidden`/`paused` while advertising → advertiser stop called, message "Advertising stopped - keep the app open while sending." shown, Send disabled until `resumed`; `inactive` while advertising → no stop; lifecycle not resumed → Send disabled; a pending start interrupted by `paused` → no advert, no "Not sent" error, no disarm prompt.
- [ ] **Step 2:** implement with `WidgetsBindingObserver`/`AppLifecycleListener` in one shared place (prefer a single app-level guard that calls `advertiser.stop()` and exposes `isForeground` to the pages) rather than duplicating per page.
- [ ] **Step 3:** `flutter test -j 1`, `flutter analyze`; commit (stage only changed files) `Send only while the app is in the foreground`.

---

### Task 4: Firmware — always fail safe on scanner loss while armed

**Owner rule 2026-09-14: always fail safe.** Supersedes the 2026-09-13 "prioritise fire" ruling (fire anyway when the scanner is lost during a pending armed delay).

**Files:** `src/app.hpp`, `src/app.cpp`, `src/detection_engine.hpp`, `src/detection_engine.cpp`, `tests/test_detection_engine.cpp`, `docs/superpowers/specs/2026-09-12-app-control-design.md` (~571), `docs/superpowers/specs/2026-09-14-arming-sequence-amendment.md` (§4 warning sources), `docs/superpowers/plans/2026-09-13-bench-checklist.md` (~93), `CLAUDE.md` (general rule).

- [ ] **Step 1:** In `App::Run()` after `serviceScanHealth()` and command handling, if `m_arming.State() == ArmState::Active && !m_scanner.IsScanning()` → set a pending flag; handle it at the same point as the switch-fault disarm: `signalWarning("scanner not running while armed - a disarm could not be heard", -ENODEV)` then `disarmDevice()` (pins isolated first, Inactive, any pending trigger cancelled), log `LOG_ERR("Scanner not running while armed - disarmed (fail safe)!")`. No LED A. Once per event (the disarm ends it).
- [ ] **Step 2:** Remove "prioritise fire": the engine must no longer fire an armed delay that ran without a scanner. Since App disarms as soon as the scanner is down while Active, keep the engine's scan-lost tracking only as a defensive guard: if an armed delay expires with `m_delay_scan_lost`, the engine does NOT set DetectionMet (no fire) and reports a renamed event `DelayExpiredScanLostSuppressed` → App `LOG_ERR("Trigger suppressed: the scanner was not running during the delay (fail safe)!")`. Update the event docs, the "Andy's ruling" comments (→ "Owner rule 2026-09-14: always fail safe"), and the host test that asserted firing despite scan loss so it asserts no fire.
- [ ] **Step 3:** Docs: design spec ~571 and checklist ~93 → fail safe (trigger cancelled, disarmed, warning); arming-sequence amendment §4 adds warning source "scanner not running while armed"; CLAUDE.md adds under the arm invariant: "**Always fail safe** (owner rule 2026-09-14): any fault the device depends on — fire switch, arming step, command scanner, while arming or armed, including a pending trigger delay — isolates the pins, disarms, cancels any pending trigger and raises the warning. Never keep firing capability through a fault."
- [ ] **Step 4:** make test green; firmware build zero warnings; clang-format; commit `Always fail safe: disarm when the scanner is lost while armed, never fire a delay that ran deaf`.
