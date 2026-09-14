# Scan Reliability and Send/Stop Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Commands are heard reliably from any measured counterpart: scan period off the 187.5 ms lock, continuous scanning during Arming, and an app Send/Stop pair replacing the Disarm pre-empt.

**Architecture:** A pure `DesiredFastScan(ArmState, bool armedDelayPending)` arbiter decides the scanner cadence, applied in one place in `App`; the engine's trigger-pending request becomes an input. Kconfig default period changes to 5906 ms. The Flutter pages gain a Stop button driven by the existing `Advertiser`.

**Tech Stack:** C++20 / Zephyr NCS v3.2.4 (nRF54L05), host tests (`make test`), Flutter (`flutter test`).

**Spec:** `docs/superpowers/specs/2026-09-14-scan-reliability-amendment.md` (binding).

## Global Constraints

- House style `~/.claude/CLAUDE.md`; clang-format `/Users/andy/nrfenv/bin/clang-format -i` on touched C++.
- `CONFIG_MFS_SCAN_PERIOD_MS` default **5906**; window unchanged at 100 ms.
- Scanner cadence: `fast = (state == ArmState::Arming) || armedDelayPending`, computed by one pure function and applied in one `App` place; PM lock only for the armed trigger delay (unchanged); existing scanner failure logging and `serviceScanHealth()` retry unchanged.
- App pages that send (Arm page, Settings page, provisioner): Send + red Stop. Idle: Send enabled per existing rules, Stop disabled. After Send pressed: Send disabled; Stop enabled while advertising or starting. Stop or 30 s end: advertising stops, Stop disabled, Send enabled. The "Send Disarm (replaces advert, N s)" pre-empt is removed. Disarm prompt unchanged (only for a started Disarm).
- Sequence numbers: never reused; every Send reserves a new `n` (unchanged).
- Never flash, never touch the J-Link/RTT; never commit `credentials.conf` or `class_app/lib/services/bench_credentials.dart`.
- Commit trailer:
  ```
  Co-Authored-By: Claude Opus 5 (1M context) <noreply@anthropic.com>
  Claude-Session: https://claude.ai/code/session_01UySHgbzqJcwKqpRntC1PtN
  ```

---

### Task 1: Firmware — cadence arbiter, continuous scan during Arming, scan period

**Files:** Create `src/scan_policy.hpp` (header-only pure function), `tests/test_scan_policy.cpp`; modify `src/app.hpp`, `src/app.cpp`, `Kconfig`, `tests/test_main.cpp` (register), `Makefile` if a source is added.

**Interfaces:**
- Produces: `namespace alc { inline bool DesiredFastScan(ArmState state, bool armedDelayPending); }` (include `arming_sequence.hpp` for `ArmState`).
- Consumes: `ArmingSequence::State()`, `DetectionEngine::DelayPendingArmed()`, `App::SetTriggerPendingScan(bool)` (DetectionHardware), `CommandScanner::SetFastScan(bool)`, `IsScanning()`.

- [ ] **Step 1: Failing tests** `tests/test_scan_policy.cpp`: all six combinations — Inactive/false → false; Inactive/true → true (an armed delay cannot be pending while Inactive in practice, but the arbiter is total); Arming/false → true; Arming/true → true; Active/false → false; Active/true → true.
- [ ] **Step 2:** `make test` fails. Implement `DesiredFastScan`. `make test` passes.
- [ ] **Step 3: App wiring.** `App::SetTriggerPendingScan(fast)` keeps the PM lock get/put exactly as now, records `m_trigger_pending_scan = fast`, and calls a new private `applyScanCadence()` returning the scanner result (so the engine's DelayFastScanFailed / DelayScanRestoreFailed reporting still works). `applyScanCadence()` computes `DesiredFastScan(m_arming.State(), m_trigger_pending_scan)` and calls `m_scanner.SetFastScan(desired)` (a no-op when unchanged). Call `applyScanCadence()` (a) inside `SetTriggerPendingScan`, (b) immediately after a successful `BeginArming`, (c) in the main loop once per iteration after `m_arming.Service()` and after command handling, so every exit from Arming (Active, cancel, failure) restores duty-cycled scanning within one loop tick. Log nothing new on success (SetFastScan already logs cadence changes). Do not call `applyScanCadence()` from any `ArmingActions` implementation (the re-entrancy contract).
- [ ] **Step 4: Kconfig.** `MFS_SCAN_PERIOD_MS` default 5906; update its help: why not 6000 (32 × 187.5 ms lock with macOS advertisers), the rule from the amendment §2, and the ~1.6 % budget effect. Check `prj.conf` does not override it.
- [ ] **Step 5:** `make test` green; firmware builds with zero warnings from touched files (`west build -b nrf54l15dk/nrf54l05/cpuapp -d build-task1 -p always -- -DEXTRA_CONF_FILE=credentials.conf`; confirm `CONFIG_MFS_SCAN_PERIOD_MS=5906` in `build-task1/class_mfs_1/zephyr/.config`; delete the dir). clang-format; commit `Scan off the 187.5 ms lock and continuously while arming`.

---

### Task 2: App — Send and Stop

**Files (in `/Users/andy/nordic/ncs/v3.2.4/class_app`):** modify `lib/devices/mfs1/mfs1_screen.dart`, `lib/devices/mfs1/mfs1_settings_screen.dart`, `lib/devices/provisioner/provisioner_screen.dart`, `test/mfs1_flow_test.dart`; `lib/services/advertiser.dart` only if Stop during a pending start needs support (then `test/advertiser_test.dart`).

- [ ] **Step 1: Failing widget tests** (Arm page, and one Settings page case):
  1. Idle: Send enabled, Stop disabled (find a `FilledButton` 'Send' and a red button 'Stop').
  2. Tap Send (Armed): Send disabled; Stop enabled once advertising started; exactly one UUID started.
  3. Tap Stop: advertiser stopped (fake records stop), Send enabled, Stop disabled; no new UUID started.
  4. After Stop, flip to Disarmed, Send: a second UUID starts and the disarm prompt shows.
  5. While advertising with the switch at Disarmed, Send stays disabled (the pre-empt is gone); no widget text contains 'replaces advert'.
  6. Countdown end (drive the fake/advertiser to end the window): Stop disabled, Send enabled.
  7. Settings page: Send disables, Stop enables, Stop re-enables Send; the Network Manager send button follows the same Send rule.
- [ ] **Step 2:** `flutter test -j 1` fails.
- [ ] **Step 3: Implement.** A red `FilledButton` (e.g. `style: FilledButton.styleFrom(backgroundColor: Theme.of(context).colorScheme.error)`) labelled 'Stop' beside Send on each page; `onPressed` enabled iff `advertiser.advertising || _sending`, calls `advertiser.stop()`. Send enabled iff the page's existing ready/rules hold and `!advertiser.advertising && !_sending`. Send keeps showing 'Advertising… N s' while advertising. Remove the pre-empt logic and label. Stop during a pending start: make it abort cleanly (if `Advertiser.stop()` while a start is pending does not already resolve `send()` to false without an error message, add that, with an advertiser test). Arm page legend: replace the cancel sentence with "To cancel arming or disarm straight after arming: press Stop, set Disarmed, press Send." and add "Stop ends the advert once LED A has confirmed." Keep all other legend text.
- [ ] **Step 4:** `flutter test -j 1` all pass; `flutter analyze` clean. Commit (stage only changed files): `Send and Stop on every sending page; remove the Disarm pre-empt`.

---

### Task 3: Docs, bench log, observer tool

**Files:** `docs/power-budget.md`, project `CLAUDE.md`, `docs/superpowers/specs/2026-09-12-app-control-design.md` (§3 and §8 banners/text), `docs/superpowers/specs/2026-09-14-arming-sequence-amendment.md` (§3 pointer), `docs/superpowers/specs/2026-09-14-command-types-amendment.md` (§3 pointer: pre-empt removed), `docs/superpowers/plans/2026-09-13-bench-checklist.md`, `tools/uuid_observer/` (add `README.md`; the source files already exist uncommitted — commit them), `docs/tan-scheme.md` only if it states the 6 s cadence as a number that matters.

- [ ] **Step 1: Power budget and CLAUDE.md.** Change the scan period to 5906 ms wherever it is a configuration value or the settled decision (table row, `CONFIG_MFS_SCAN_PERIOD_MS=5906`, duty cycle 100/5906 = 1.693 %), recompute the directly affected figures (scan RX term and total average current) with the document's own formulas and state the new battery-life figure; keep historical analysis that argues about "6 s-class" cadence where the argument is unaffected, adding "(5.906 s since 2026-09-14, see scan-reliability amendment)" at the decision point. In `CLAUDE.md` update the Settled decisions table (Scan: 100 ms passive every 5.906 s) and the counterpart-interval bullet with the measured Mac 187.5 ms / iPhone ~35 ms and the no-near-multiple rule.
- [ ] **Step 2: Specs.** Banners/pointers per the Files list; describe Send/Stop and the removal of the pre-empt; describe continuous scanning during Arming.
- [ ] **Step 3: Bench checklist.** In §9 bench log add a dated entry for today's findings (Mac 187.5 ms lock-in with 6000 ms, the unheard time sync and Disarms, iPhone 35 ms every-scan catches, the 1.7 s replay of the previous advert, `CONFIG_MFS_SCAN_DIAG` and the observer tool). Replace §7 (iPhone interval) with the measured result and mark it done. Add `## 5d. Scan reliability and Send/Stop (plan 2026-09-14)`: (1) after flashing, confirm `Passive scan started: 100 ms window every 5906 ms.`; (2) Mac: 10 Sends, count those heard at the first or second scan (expect nearly all); (3) Arm then, within the 10 s, Stop → Disarmed → Send: `Scan cadence now CONTINUOUS` on arming, cancel heard within ~1 s, `Scan cadence now duty-cycled` afterwards; (4) arming completes: cadence returns to duty-cycled; (5) Stop ends the advert (observer shows the UUID stop); (6) optional with the observer: note the previous payload replay at the start of a Send.
- [ ] **Step 4: Observer tool.** `tools/uuid_observer/README.md`: purpose, board (`nrf54l15dk/nrf54l15/cpuapp`), build/flash commands (`west build -b nrf54l15dk/nrf54l15/cpuapp -d build-observer tools/uuid_observer`, flash with the DK's own `--dev-id`), console on VCOM1 at 115200, the capture recipe (bounded Python reader), and how to compute intervals. Commit `tools/uuid_observer` (CMakeLists.txt, prj.conf, src/main.cpp, README.md).
- [ ] **Step 5:** Commits: `Docs: scan period 5906 ms, continuous scan while arming, Send/Stop` and `Add the bench UUID observer tool`.
