# Three-LED Indication Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace the two-LED (LED A / LED B) scheme with the red / orange / green scheme agreed in the spec, in every build.

**Architecture:** Two pure, host-tested pieces decide every LED level: `LedSequencer` plays one overlay pattern at a time (with a priority rule), and a new `led_indicator` module turns the device state into the base levels of all three LEDs. `App` only gathers inputs and writes pins: the main loop writes all three pins while no overlay plays, and a single 10 ms timer writes all three while one does — one writer per pin at a time, as today.

**Tech Stack:** C++20, Zephyr / NCS v3.2.4, host tests with plain `g++` (`make test`).

**Spec:** `docs/superpowers/specs/2026-11-12-three-led-indication-design.md` — read it first; this plan does not restate its reasoning.

## Global Constraints

- ALC house style (`~/.claude/CLAUDE.md`) and project `CLAUDE.md`; run `/Users/andi/nrfenv/bin/clang-format -i` on every source file touched.
- Host tests: `make test` must end `ALL TESTS PASSED` after every task.
- Firmware build (from the repo root, in Claude's shell): `nrfutil toolchain-manager launch --ncs-version v3.2.4 -- west build -b nrf54l15dk/nrf54l05/cpuapp -p always -- -DEXTRA_CONF_FILE=credentials.conf` must succeed with no new warnings after every task that touches `src/`, `Kconfig`, `prj.conf` or the overlay.
- Never commit `credentials.conf`. Do not flash; flashing and the bench checks are the owner's.
- Pins: LED1 **red P2.02**, LED2 **orange P2.03**, LED3 **green P2.04**, all `GPIO_ACTIVE_HIGH` (red assumed; bench polarity check).
- Timings: green slow flash **500 ms on / 2500 ms off**; red flash = today's Armed (`{60, 65}`, 3000 ms); orange steady **3000 ms**; orange triple **3 × 150 ms on, 200 ms apart, 1000 ms pattern**; `LedSequencer::M_FRAME_GAP_MS` stays **300 ms**.
- Presentation only: never read INT1 / AWAKE; read state through `m_arming.State()`, `m_engine.DetectionMet()`, `m_fire.RehearsalFired()`, `m_clock.IsValid()`. Nothing touches the arm boolean, the output derivation or the fire path.
- Failed authentication, replay or malformed payload: no LED, unchanged.
- Every LED behaviour is identical in bench and production builds; only `CONFIG_MFS_BATTERY_TEST` changes green.

## Review Focus

1. **A lower-priority overlay arriving while a higher one plays** (a Disarm's orange triple during a 3 s orange steady warning) — the steady must keep playing, not be cut short. Pinned by `LedSequencer` test in Task 1.
2. **Simulated red while Arming or Active** (a rehearsal FIRE still running when an Arm is accepted) — must be masked: Arming shows only red steady, Active shows nothing. Pinned in Task 2.
3. **An Arm accepted while an orange triple still plays** — the exit delay's steady red must show at once, not after the triple. Covered by keeping the `Stop()` in the Arm path (Task 3) and a bench check (Task 5).
4. **A warning raised before `initLeds()`** (boot pin check) — must still play once the pins exist. Keep the `m_leds_initialised` start-later mechanism (Task 3).
5. **Fault latch never cleared by a rejected command** — only an Accepted verdict or an applied time sync clears it. Placement fixed in Task 4; bench check in Task 5.

---

### Task 1: Sequencer patterns for three colours

**Files:**
- Modify: `src/led_sequencer.hpp`, `src/led_sequencer.cpp`
- Modify: `src/app.cpp` (call sites only, mechanical rename — see Step 5)
- Test: `tests/test_led_sequencer.cpp`
- Also commit: `docs/superpowers/specs/2026-11-12-three-led-indication-design.md`, this plan

**Interfaces:**
- Produces:
  - `enum class LedPattern : uint8_t { None, RedFlash, OrangeTriple, OrangeSteady };` (Armed, Disarmed, DisarmedDelayCancelled, Warning, SettingsApplied, ModeChanged removed)
  - `enum class LedColour : uint8_t { None, Red, Orange, Green };`
  - `LedColour ColourOf(LedPattern pattern);` — free function, `alc` namespace, declared in `led_sequencer.hpp`.
  - `bool LedSequencer::Start(LedPattern pattern, int64_t nowMs);` — now returns whether it started. Refuses (returns false, leaves the current pattern) when the current pattern is still active and has a **higher** priority. Priority, highest first: OrangeSteady, RedFlash, OrangeTriple. Equal or higher priority replaces (restart from the lead-in).
  - `Stop()`, `Current()`, `IsActive()`, `Level()` unchanged.

- [ ] **Step 1: Rewrite `tests/test_led_sequencer.cpp`** keeping the framing assertions, now on the new names:
  - `RedFlash`: the existing Armed assertions (gap dark, on at `GAP`, off at `GAP+60`, on at `GAP+125`, dark at `GAP+3000`, inactive at `GAP+3000+GAP`).
  - `OrangeSteady`: dark at `GAP-1`; lit at `GAP`, `GAP+1500`, `GAP+2999`; dark at `GAP+3000`; inactive at `GAP+3000+GAP`.
  - `OrangeTriple`: lit at `GAP+0`, `GAP+149`, `GAP+350`, `GAP+700`, `GAP+849`; dark at `GAP+150`, `GAP+349`, `GAP+500`, `GAP+850`, `GAP+999`; active at `GAP+1000+GAP-1`; inactive at `GAP+1000+GAP`.
  - `ColourOf`: RedFlash→Red, OrangeTriple→Orange, OrangeSteady→Orange, None→None.
  - Priority: `Start(OrangeSteady, T)` then `assert(!Start(OrangeTriple, T+500))` and `Current() == OrangeSteady`; `Start(OrangeTriple, T)` then `assert(Start(OrangeSteady, T+100))`; `Start(OrangeTriple, T)` then `assert(Start(OrangeTriple, T+100))` restarts (dark at `T+100+GAP-1`, lit at `T+100+GAP`); once a pattern is inactive any pattern starts: `Start(OrangeSteady, T)`, `assert(Start(OrangeTriple, T+GAP+3000+GAP))`.
- [ ] **Step 2: Run `make test`** — expected: compile failure on the new names.
- [ ] **Step 3: Implement** in `led_sequencer.{hpp,cpp}`: steps `M_RED_FLASH {60,65}` / 3000 ms, `M_ORANGE_TRIPLE {150,200}` / 1000 ms, `M_ORANGE_STEADY {3000,0}` / 3000 ms; a private `priorityOf()` in the anonymous namespace. Rewrite the enum's Doxygen to describe the three-colour scheme and point at the spec.
- [ ] **Step 4: Run `make test`** — expected `ALL TESTS PASSED`.
- [ ] **Step 5: Keep the firmware building** with a mechanical rename in `src/app.cpp` (behaviour fixed in Task 3): `Armed`→`RedFlash`; `Disarmed`, `DisarmedDelayCancelled`, `SettingsApplied`, `ModeChanged`→`OrangeTriple`; `Warning`→`OrangeSteady`. `playLedPattern()` ignores `Start()`'s return for now (`(void)`). Run the firmware build — expected success.
- [ ] **Step 6: clang-format, commit** the spec, this plan, `src/led_sequencer.*`, `src/app.cpp`, the test: `LED sequencer: red flash, orange triple and orange steady, with overlay priority`.

### Task 2: Base levels from device state (pure)

**Files:**
- Create: `src/led_indicator.hpp`, `src/led_indicator.cpp`
- Create: `tests/test_led_indicator.cpp`
- Modify: `Makefile` (`HOST_SRCS` += `src/led_indicator.cpp`), `tests/test_main.cpp` (`run_led_indicator_tests()`)

**Interfaces:**
- Consumes: `ArmState` (`src/arming_sequence.hpp`), `LedPattern`, `ColourOf()` (Task 1).
- Produces (namespace `alc`):
  - `struct LedLevels { bool red; bool orange; bool green; };` with a defaulted `operator==`.
  - `struct LedInputs { ArmState state; bool clockValid; bool faultLatched; bool simulatedFiring; };`
  - `LedLevels BaseLevels(const LedInputs& inputs, int64_t nowMs);`
  - `LedLevels OverlayLevels(LedPattern pattern, bool on);` — only `ColourOf(pattern)`'s LED, lit when `on`; the other two off (this is what turns green off under an overlay).
  - `constexpr uint32_t M_GREEN_FLASH_ON_MS { 500 }; constexpr uint32_t M_GREEN_FLASH_PERIOD_MS { 3000 };`

Rules for `BaseLevels`, first match wins: Active → all off. Arming → red only. Inactive and `faultLatched` → orange only. Inactive and `simulatedFiring` → red only. Inactive, `!clockValid` → green on. Inactive, `clockValid` → green on when `nowMs % 3000 < 500`, else all off. (`simulatedFiring` outside Inactive is ignored — the mask.)

- [ ] **Step 1: Write `tests/test_led_indicator.cpp`** asserting, with `nowMs` 0 unless stated:
  - Inactive, clock invalid → `{false,false,true}`; also at `nowMs` 1000 (steady).
  - Inactive, clock valid → green at 0 and 499, off at 500 and 2999, on again at 3000 and 3499.
  - Inactive, fault latched (clock valid or not, simulatedFiring true or false) → `{false,true,false}`.
  - Inactive, simulatedFiring, clock invalid → `{true,false,false}`.
  - Arming with simulatedFiring true and false → `{true,false,false}`.
  - Active with simulatedFiring, faultLatched, clock valid → `{false,false,false}`.
  - `OverlayLevels(RedFlash,true) == {true,false,false}`, `OverlayLevels(OrangeTriple,true) == {false,true,false}`, `OverlayLevels(OrangeSteady,false) == {false,false,false}`, `OverlayLevels(None,true) == {false,false,false}`.
- [ ] **Step 2: Run `make test`** — expected: link/compile failure.
- [ ] **Step 3: Implement** `led_indicator.{hpp,cpp}` per the rules above, Doxygen on each function citing spec §3.1–3.3. Add it to `Makefile` and `test_main.cpp`.
- [ ] **Step 4: Run `make test`** — expected `ALL TESTS PASSED`.
- [ ] **Step 5: clang-format, commit**: `LED indicator: base levels for red, orange and green from device state`.

### Task 3: App drives three LEDs

**Files:**
- Modify: `boards/nrf54l15dk_nrf54l05_cpuapp.overlay`
- Modify: `src/app.hpp`, `src/app.cpp`
- Modify: `CMakeLists.txt` if sources are listed explicitly (add `src/led_indicator.cpp`)
- Modify: `Kconfig`, `prj.conf`

**Interfaces:**
- Consumes: Task 1 sequencer, Task 2 `BaseLevels`, `OverlayLevels`, `LedInputs`, `LedLevels`.
- Produces (used by Task 4): `void App::playLedPattern(LedPattern pattern);` (same name, now priority-aware), `bool m_fault_display_latched;` member (initialised false; set/cleared in Task 4), `int App::applyLeds(const LedLevels& levels);`.

- [ ] **Step 1: Overlay.** Add under `zephyr,user`: `led-red-gpios = <&gpio2 2 GPIO_ACTIVE_HIGH>;`. Keep `&led0` (P2.04, green) and `&led1` (P2.03, orange). Rewrite the header comment and the `&led0`/`&led1` comment: LED1 red P2.02, LED2 orange P2.03, LED3 green P2.04; delete the "P2.02 OPEN" note — nPM2100 GPIO_1 is P2.01.
- [ ] **Step 2: App pins.** Replace `s_led_a`/`s_led_b` with `s_led_red` (`DT_PATH(zephyr_user), led_red_gpios`), `s_led_orange` (`led1`), `s_led_green` (`led0`). `initLeds()` checks and configures all three `GPIO_OUTPUT_INACTIVE`. Replace `applyLedA`/`applyLedB` with `applyLeds(const LedLevels&)` writing all three, returning the first error.
- [ ] **Step 3: One overlay, one timer.** Remove `m_warning_sequencer`, `m_warning_timer`, `warningTimerHandler`. `ledTimerHandler` writes all three pins from `OverlayLevels(m_led_sequencer.Current(), m_led_sequencer.Level(now))` and stops itself when inactive. `playLedPattern()`: stop timer → `Start()`; if refused, restart the timer if the current pattern is still active and return; else start the timer only when `m_leds_initialised`. `signalWarning()` keeps its log line and calls `playLedPattern(LedPattern::OrangeSteady)`. In `Run()`, after `m_leds_initialised = true`, start the timer if `m_led_sequencer.IsActive(now)` (replaces the warning-timer start).
- [ ] **Step 4: Main loop derivation.** Replace the ledA / ledB block with: `LedInputs inputs { m_arming.State(), m_clock.IsValid(), m_fault_display_latched, m_engine.DetectionMet() || m_fire.RehearsalFired() }`; `overlayActive = m_led_sequencer.IsActive(now)`; `levels = overlayActive ? OverlayLevels(Current(), Level(now)) : BaseLevels(inputs, now)`. Battery-test build: `levels.green` is replaced by the existing liveness blink (`blinkOnTicks > 0`, which is 0 when `CONFIG_MFS_BLINK_MS=0`) whenever no overlay plays. Write with `applyLeds(levels)` only when `!overlayActive`. The output transition log becomes `"Output %s. Arm %s, red %s, orange %s, green %s."`.
- [ ] **Step 5: Detection log.** Rename `CONFIG_MFS_DEBUG_LED` to `CONFIG_MFS_DEBUG_DETECTION_LOG` (default y, bench only, gates the "Detection met / cleared" transition log and nothing else); its log text says "red on/off" for the simulated-firing signal. Rewrite the Kconfig help; update `CONFIG_MFS_BATTERY_TEST` / `CONFIG_MFS_BLINK_MS` help ("green" not "LED A"); update the `prj.conf` LED comments and the renamed symbol.
- [ ] **Step 6: Arm path.** Keep the `k_timer_stop` + `m_led_sequencer.Stop()` in `ArmAction::Arm` so the exit delay's red steady shows at once (Review Focus 3); update its comment.
- [ ] **Step 7: Build** — expected success, no new warnings; `make test` still passes.
- [ ] **Step 8: clang-format, commit**: `Three LEDs: red P2.02, orange P2.03, green P2.04 driven from the LED indicator`.

### Task 4: Events — confirmations, fault latch, time set

**Files:**
- Modify: `src/app.cpp`, `src/app.hpp`

**Interfaces:**
- Consumes: `playLedPattern()`, `m_fault_display_latched` (Task 3).

- [ ] **Step 1: Confirmations.** In `applyCommand()`: Disarm → `OrangeTriple` (drop the delay-cancelled distinction, keep its `LOG_WRN`); Tune → `OrangeTriple` (drop the mode distinction; `previousMode` goes if now unused); `ReplayArmed` → `RedFlash`; arming completed (`armedThisTick && Active`) → `RedFlash`. Fire: no overlay, unchanged.
- [ ] **Step 2: Time set.** In `handleTimeSyncCandidate()`, after the `Clock set by provisioner` log: clear `m_fault_display_latched`, `playLedPattern(LedPattern::OrangeTriple)`.
- [ ] **Step 3: Clear the latch** in `handleCommandCandidate()` immediately after the `Accepted` check, before the command log (any accepted command, including ignored and replayed ones). Rejected verdicts return earlier and never reach it.
- [ ] **Step 4: Set the latch** (`m_fault_display_latched = true;` plus `LOG_INF("Device disarmed itself - orange until the next accepted command.")` via a private `void latchFaultDisplay()`) at exactly three places in `Run()`: after `ServiceScannerHealth()` returns true; in the switch-fault disarm (`m_switch_fault_pending` while Active); in the `TakeTriggerComplete()` disarm. Check (read `updateOutputState()` around `app.cpp:771` and `FireSequence`) that a live FIRE's end disarms through the `TakeTriggerComplete()` path; if it does not, add the call at the place it does disarm and note it in the commit. Not at boot, not on arming-step failures at the deadline (spec §3.1, §3.2).
- [ ] **Step 5: Comments.** Remove every remaining "LED A" / "LED B" mention in `src/` (`grep -n "LED [AB]" src/`  → no hits), including `adxl367.hpp`'s class comment.
- [ ] **Step 6: Build and `make test`** — both succeed.
- [ ] **Step 7: clang-format, commit**: `LEDs: orange triple confirmations, red flash on arming, orange latch when the device disarms itself`.

### Task 5: Documentation and bench checklist

**Files:**
- Modify: `CLAUDE.md`, `docs/v1-scope.md`, `docs/todo.md`, `docs/superpowers/specs/2026-09-12-app-control-design.md` (§6.7), the arming-sequence, disarmed-test-mode and FIRE amendments wherever they name LED A / LED B, `docs/superpowers/plans/2026-09-13-bench-checklist.md`

- [ ] **Step 1: `CLAUDE.md`.** Pin map rows: LED3 green P2.04, LED2 orange P2.03, LED1 red P2.02. Delete "OPEN — P2.02". Rewrite the invariant/access-rule sentences naming LED A / LED B to the new colours (warning = orange steady; acknowledgement = orange triple; Armed replay = red flash; simulated firing = red), pointing at the spec.
- [ ] **Step 2: Older specs.** In each older document, add one line at its LED section: "Superseded by `2026-11-12-three-led-indication-design.md`." Do not rewrite their history. Update `docs/v1-scope.md`'s LED section to summarise the new scheme.
- [ ] **Step 3: `docs/todo.md`.** Add the spec §7 items: orange reason codes, variable exit delay, app display-status option, fault message.
- [ ] **Step 4: Bench checklist.** New section: red polarity (dark when armed); boot → green steady; time sync → orange triple then green 500/2500 flash; Settings and Disarm → orange triple; Arm during a triple → red steady at once, 10 s, red flash, dark; armed Arm → red flash; armed trigger → orange steady until the next command; refused Arm (scanner down) → orange 3 s then green; disarmed test trigger and rehearsal FIRE → red for the would-be window, green off; rehearsal FIRE then Arm → no red during Arming/Active; failed-auth command → no LED change, latch kept.
- [ ] **Step 5: `grep -rn "LED A\|LED B" CLAUDE.md docs/v1-scope.md src/`** — only hits that deliberately say "formerly".
- [ ] **Step 6: Commit**: `Docs: three-LED indication`.
