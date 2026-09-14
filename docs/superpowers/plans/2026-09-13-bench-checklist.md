# MFS_1 App Control — Bench Checklist

Hardware verification deferred during subagent execution of
`docs/superpowers/plans/2026-09-12-app-control.md` (2026-09-13). No agent flashed the
board or touched the J-Link. Work through this in order. Record the result against
each item, and fix before moving on if an item marked **STOP** fails.

**Setup.** Close JLinkRTTViewer before flashing, and reopen it afterwards.
`nrfutil device device-info --serial-number 853003346` must report an nRF54L.

```bash
cd /Users/andy/nordic/ncs/v3.2.4/class_mfs_1
west build -b nrf54l15dk/nrf54l05/cpuapp -p always -- -DEXTRA_CONF_FILE=credentials.conf
west flash --dev-id 853003346 --recover
```

RTT: device `nRF54L05_M33`, SWD, 4000 kHz. App: `cd ../class_app && flutter run -d macos`
(the Mac clock must be network-synced).

## 1. Crypto self-test (Task 8) — STOP if it fails

- [x] Boot log shows `Crypto self-test passed: 3 command vectors and the time-sync vector match.`
- [x] Then `Device 0x........ ready. Clock INVALID until a provisioner time sync.`
- [x] If it fails, record the line. Suspect CRACEN and the 4-byte tag or the 11-byte nonce (`PSA_ERROR_NOT_SUPPORTED` = -134). Do not weaken the test.
- [x] Once it passes, mark spec §9 item 5 RESOLVED (with the log line) and delete the "PSA self-test on target" row from `docs/tan-scheme.md` §11.
- [x] Rebuild **without** `-DEXTRA_CONF_FILE` and flash. Expect `Credentials missing or malformed`, and the device boots Inactive. Reflash the credentials build.

## 2. Boot and access state (Task 9)

- [x] First boot: `No access state stored - first boot, no day floor.`
- [x] `Passive scan started: 100 ms window every 5906 ms.`
- [x] Temporarily enable `CONFIG_THREAD_ANALYZER=y` (and `CONFIG_THREAD_ANALYZER_AUTO=y` or a manual call point), flash, provision, send a command and trigger the device, then record the reported high-water marks for the main thread and the Bluetooth RX thread. Revert the Kconfig afterwards.

## 3. Provisioning and commands, end to end (Task 13)

- [x] Device picker → clock icon → **Send time sync**. Within 30 s: `Clock set by provisioner: unix ..., day ..., HH:MM UTC`, and HH:MM matches the Mac's UTC time.
- [x] Send the time sync again: nothing is logged (the clock is valid).
- [x] MFS_1 screen, Armed off, move sliders, **Send**. The `Command slot 1 n 0:` line shows exactly the app's values, the current UTC minute, and **one blink** on LED A.
- [x] **Send** again without changes: `n 1` (new bytes each time).
- [x] Reset the board (`nrfutil device reset --serial-number 853003346`). Expect `Access state restored: day floor <today>` and the clock invalid. A **Send** logs nothing. Provision, **Send**: accepted, and `n` continues upward.
- [x] Leave the board powered past 04:00 UTC with no commands, reset it, and confirm the restored floor is the new day (the daily rollover persisted without commands).

## 4. Detection engine (Task 14)

- [x] Tune (Armed off) with activations 3 and cooldown 8 s (byte 66), **Send** (one blink). Handle the device: LED B lights about 5 s per detection.
- [x] Armed on, **Send**: **rapid flash**. Tap three times with pauses: `Activation 1 of 3`, `Cooldown started: 8 s`, `Cooldown elapsed - detection re-armed`, … `Output ASSERTED`.
- [x] Tapping during the cooldown does **not** increment the count.
- [x] Power-cycle: the boot `Settings:` line shows the values sent (NVS).

## 5. Delay and interlock (Task 15) — STOP if 5.3 fails

1. [ ] Disarm if not already; Settings page, activations 1, delay 30 s, **Send** (single blink); Arm page, Send Armed (rapid flash); trigger. Expect `TRIGGER PENDING: firing in 30 s` and `Scan cadence now CONTINUOUS`.
2. [ ] Wait the full 30 s: `Output ASSERTED`, LED B and the fire output for **about 5 s**, then `Output cleared`, `Trigger complete - latched Inactive.`, and LED A lights.
3. [ ] **Repeat, and at about 15 s, Arm page Send Disarmed**: **double blink**, `Disarmed with a trigger PENDING`, the output **never** asserts, and the scan returns to duty-cycled. **This is the most important check on the list.**
4. [ ] Repeat and **reset mid-delay**: comes up Inactive, clock invalid, no trigger.
5. [ ] Settings page, delay 0, **Send** (single blink); Arm page, Send Armed; firing is immediate.

## 6. Armed path, one-shot, LED A (Task 16) — STOP if 6.2–6.4 fail

1. [ ] Disarm if not already; Settings page, **Send**: one blink. Report+Trig and Report are disabled in the app's mode selector as of the final fix wave (Task 18 item 3) - confirm they cannot be selected and the note under the selector is visible. To exercise the firmware's refusal directly, send a hand-crafted slot-0 command with the mode bits set to `10` (Report): expect **one blink** (SettingsApplied, not ModeChanged - the mode is refused) and `Mode 2 refused - reporting is not implemented yet; device stays Trigger only.`. Power-cycle and confirm the boot `Settings:` line still reports mode 0.
2. [ ] Arm page, Send Armed (activations 1, delay 0 already stored): rapid flash. **While still armed**, go out of range (or cover the device) so the Disarm is not heard, Arm page, Send Disarmed, then press **Disarmed — open settings** anyway; come back in range (uncover the device), Settings page, **Send**: expect **rapid flash** (Armed replay), RTT `Armed: Settings from slot 1 n X changes nothing - replaying Armed.`, and the device still armed (tap: fires). Back on the Arm page, Send Armed again: **rapid flash replay**, `Armed: Arm from slot 1 n ... changes nothing - replaying Armed.`.
3. [ ] Trigger: output for about 5 s, then `Trigger complete - latched Inactive.`. Trigger again: **the output never asserts**.
4. [ ] Arm with delay 60 s, trigger, and within the minute, Arm page Send Disarmed: **double blink**, `Disarmed with a trigger PENDING`, no fire. Settings page, change activations to 7, **Send**: one blink, and `Applied:` now shows 7.
5. [ ] **Tune-then-arm (the fixed hazard):** Settings page, activations 1 and delay 60 s, **Send** (single blink); handle the device, then Arm page Send Armed within 60 s. The output must **not** assert a minute later.
6. [ ] Build with `CONFIG_MFS_DEBUG_LED=n` in prj.conf, flash, provision, and Arm page Send Armed: the rapid flash still plays, LED A is **dark** afterwards. Disarm (slow flash), then Settings page **Send** (one blink) and confirm LED A stays dark afterwards. Restore `CONFIG_MFS_DEBUG_LED=y`.
7. [ ] Arming failure (optional, needs the ADXL367 disconnected): after the 10 s exit delay, **three long pulses on LED B**, no LED A flash, device Inactive - see §5c item 7.

## 7. iPhone advertising interval (Task 13, spec §9 item 2) — DONE 2026-09-14

- [x] Measured with a second nRF54L15 DK running `tools/uuid_observer` (a
  continuous passive scanner logging every UUID with a timestamp) instead of the
  in-firmware `LOG_INF()` patch originally planned below: **~35 ms, steady for
  30 s, ~800 adverts per Send** (iOS 26.6.1, foreground, release build, slot 3).
  Materially different from the Mac's 187.5 ms.
- [x] Against the iPhone, MFS_1's `CONFIG_MFS_SCAN_DIAG` counters showed 2–3
  adverts caught in every 100 ms scan window and every command heard at the
  first scan — no `kAdvertiseWindow` or detection-table change is needed on the
  iPhone side. The Mac's 187.5 ms interval was the actual problem: it is close
  to an integer multiple of the old 6000 ms scan period (see the 2026-09-14
  bench log entry below), fixed by moving the default to 5906 ms
  (`docs/superpowers/specs/2026-09-14-scan-reliability-amendment.md`).
- [x] `CLAUDE.md`'s interval note updated with both measured values and the
  no-near-multiple rule.

Original recipe (superseded by the observer tool above; kept for reference):

- Temporarily raise `CONFIG_BT_RX_STACK_SIZE` to 4096 in `prj.conf` for this step - the added `LOG_INF()` below runs on the Bluetooth RX thread in immediate log mode, and the normal 2048-byte stack is sized on the assumption that this thread never logs (see the comment on `s_dropped` in `src/command_scanner.cpp`). Revert afterwards.
- Temporarily add `LOG_INF("UUID seen at %lld ms.", k_uptime_get());` at the top of `parseAdStructure()`'s UUID branch in `src/command_scanner.cpp`, flash, `flutter run -d <iphone>`, and **Send** once. Record the interval between timestamps. **Revert both changes and reflash.**

## 8. Recovery notes

- A corrupt `access/v1` record makes the firmware refuse commands on every boot (`Stored access state is invalid`). The only recovery is a wired erase of the settings partition, followed by re-provisioning.
- If the scanner is lost during a pending delay, the trigger **still fires** (owner decision 2026-09-13) and logs `Trigger firing although the scanner was not running during the delay`.
- A nearby advertiser flooding random 128-bit UUIDs is an RF-jamming-class attack on the 8-entry candidate queue, not just noise: none of the garbage counts towards the lockout (§6.4 of `docs/tan-scheme.md`), but if it arrives faster than `serviceCandidates()` can drain it, genuine candidates get dropped. Run a flood test (an advertiser cycling random 128-bit UUIDs at a high rate) and check for `Candidate queue full - N adverts dropped!` in the log; confirm a genuine command still lands once the flood stops.

## 5a. Command types (plan 2026-09-14) — do this first on the new build

1. [ ] Flash with `--recover`, provision the clock.
2. [ ] Inactive, Arm page Send Armed → rapid flash; RTT `Command slot 1 n X: Arm, minute M.` and `Applied:` shows the **stored** settings (not app defaults).
3. [ ] Armed, Send Armed again → rapid flash replay; RTT `Armed: Arm from slot 1 n X changes nothing - replaying Armed.`; still armed (tap: fires).
4. [ ] Armed, Send Disarmed → slow flash; the app prompt appears with the fault-finding text; press Disarmed — open settings.
5. [ ] Settings page: sliders at defaults; Send → single blink; LED B simulates at those settings.
6. [ ] Restore defaults resets the sliders; nothing is sent until Send.
7. [ ] Back → Arm page shows Armed. Send → rapid flash; RTT `Applied:` shows the settings from step 5.
8. [ ] Inactive, Send Disarmed → slow flash replay, LED B stops if tuning.
9. [ ] Prompt "Not seen" stays on the Arm page, and Settings is unreachable without a confirmed disarm.
10. [ ] Turn Bluetooth off on the Mac: Send is disabled and the Arm page shows "Bluetooth is off." (the advertising-error path — "Not sent: …", no countdown, no dialog — is covered by host tests in `class_app/test/advertiser_test.dart`, not exercised here).
11. [ ] Old-app regression: none needed (nothing deployed); a `0x02` command from an old app build is silent, but its rotating ID still matches (the ID does not depend on the protocol version), so each one counts as an authentication failure toward the 20-failure lockout — make sure no old app build is on a bench phone.

## 5b. Disarmed test mode (plan 2026-09-14)

RTT expectations below are the engine's log texts as implemented — see
`App::OnDetectionEvent` and the Detection met/cleared and Output lines in
`App::Run()` (`src/app.cpp`).

1. [ ] Flash **without** `--recover` (keeps `access/v1` and the stored settings, so
   the app's slot stays in sync with the device's sequence window - a `--recover`
   flash erases them, forcing a slot change or a wait for the 04:00 rollover per
   the bench log below), provision the clock. With no command sent, tap the device
   three times: LED B lights on the third tap (`Detection met (test) - LED B
   on.`), at the settings already stored from boot - no Settings command is needed
   to start the test. **This is the new-image check**: the old image held the
   ADXL in standby while Inactive and would not react to any tap here, so LED B
   lighting on the third tap with nothing sent proves the new image is running.
2. [ ] Disarm (Arm page, Send Disarmed): slow flash, then the test restarts from
   zero. Count the restarts: the next tap after the disarm logs `Activation 1 of
   N.`, not a continuation of whatever was counted before.
   - (a) [ ] Disarm while shaking the device continuously: the loop-mode
     bootstrap waits for AWAKE to clear before returning, so LED A's slow flash
     can arrive late - up to about 8 s at the default 5 s inactivity period. Do
     not resend before about 10 s on the strength of a missing acknowledgement.
3. [ ] Settings page: 1 activation, 30 s delay, Send (single blink). Tap once: LED
   B lights only after about 30 s; RTT shows `TEST trigger pending: LED B in 30
   s.` then `TEST delay elapsed - LED B on.` - never `TRIGGER PENDING`. No
   `Scan cadence now CONTINUOUS` or fast-scan line is logged - continuous scan and
   the PM lock are armed-only, so a long test delay costs no extra battery.
4. [ ] Long cooldown: Settings page, activations 2, cooldown 600 s (10 min),
   Send. Tap once (`Activation 1 of 2.`, `Cooldown started: 600 s.`), then Disarm.
   The next tap logs `Activation 1 of 2.` immediately - no waiting out the
   cooldown.
5. [ ] **SAFETY, arm mid-test** (activations 3, cooldown 8 s, delay 0):
   - (a) [ ] Tap twice (`Activation 1 of 3.`, `Activation 2 of 3.`), Arm page Send
     Armed (rapid flash), tap once: no fire, RTT `Activation 1 of 3.` - the armed
     session started from zero regardless of the count in progress.
   - (b) [ ] Tap once (cooldown now running), Arm immediately, tap once:
     `Activation 1 of 3.`, no fire.
   - (c) [ ] Settings page: delay 30 s, activations 1, Send; tap once
     (`TEST trigger pending: LED B in 30 s.`); Arm page Send Armed within the 30
     s; leave the device untouched past 30 s: no fire - the pending test delay
     never reaches the armed output.
   - (d) [ ] Shake the device continuously while sending Arm: no fire until
     motion stops and a fresh, complete edge finishes the count (`Activation 1 of
     3.` logs only once AWAKE has cleared and reasserted after arming).
   - (e) [ ] Arm while LED B is lit (a live test detection, or held by a pending
     test delay): no fire; the next count starts at `Activation 1 of 3.`, not a
     continuation of the test in progress.
6. [ ] Armed trigger fires once (`Output ASSERTED`, fire GPIOs and LED B for
   about 5 s), then `Output cleared` and `Trigger complete - latched Inactive.
   Re-arming needs an engineer command.`. The test resumes on its own with no
   further command: the next completed tap after that logs `Activation 1 of N.`
   and LED B lights on the next completed count.
7. [ ] Measure device current while disarmed and idle (the ADXL367 now runs
   continuously when disarmed) and compare it with the armed idle current -
   confirm the two are close, since the part is configured identically and runs
   the same loop-mode engine in both states.
8. [ ] Cold-cell boot: after a battery insert, confirm the boot log shows the
   accelerometer configured at boot - no configure-failure or retry line
   (`Accelerometer would not configure`, `Detection test could not start -
   retrying the accelerometer every N ms!`, `Failed to reconfigure the ADXL -
   retrying every N ms!`) - before the first tap.

## 5c. Arming sequence and fire-pin isolation (plan 2026-09-14)

RTT expectations below are the exact strings logged by `src/app.cpp` and
`src/output_switch.cpp` as implemented — see `App::applyCommand()`,
`App::disarmDevice()`, `App::logArmingFailure()`, `App::Run()` and
`OutputSwitch::Init()`/`Enable()`/`Disable()`.

**A voltmeter cannot tell the pin configurations apart.** A driven-low line and a
pulled-low (disconnected) line both read 0 V, and with 10 kΩ on a MOSFET gate the
RC decay is microseconds, far too fast for a meter to see. Infer the pin
configuration from the RTT lines quoted below. Use the meter only to confirm that a
fire line **is low / is not high**.

1. [ ] **Boot.** RTT shows `Fire output initialised: both pins read low at boot
   and are isolated (no driver).` That line is the evidence the pins are
   disconnected; the meter only confirms both fire lines read low.
   On a failed check, expect `Fire output failed its boot check (%d) - pins
   isolated and latched faulty; booting on, but the device will NOT arm!` and
   `WARNING (light TBC): fire pins failed the boot check (%d)!`, preceded by the
   switch's own reason - `Fire gate %s read high at boot with no driver! Output is
   SAFE - the other MOSFET blocks - but redundancy is LOST!` or `BOTH FIRE GATES
   READ HIGH AT BOOT WITH NO DRIVER - THE DEVICE MAY BE FIRING!`, then `Fire output
   LATCHED FAULTY and will not assert again: %s (%d)!`. LED B plays three long
   pulses shortly after boot.
   - **Bare nRF54L15 DK:** the DK has **no pull-downs on P2.05 / P2.09**. A plain
     input with no pull floats, so the boot check may read high and latch the
     switch faulty. That is **expected on a bare DK**, not a firmware fault - fit
     10 kΩ pull-downs to GND on both pins to exercise items 2-7, which all need a
     switch that passed its boot check.
2. [ ] **Arm.** Send Armed. RTT: `Arming: fire pins isolated, arming in 10 s.`
   Immediately after: **nothing on LED A or LED B for 10 s**. Tap the device
   during the window and confirm LED B stays dark (suppressed while Arming).
   The meter shows both fire lines low throughout. After 10 s, RTT shows `Fire
   pins enabled: outputs, de-energised and verified.` then `Arm state: Active -
   fire pins enabled (uptime ... ms).`, and LED A plays the rapid flash (Armed).
   The `Fire pins enabled` line - not the meter, which still reads 0 V - is what
   confirms `Enable()` attached a driver. If it ends `(NO READ-BACK AVAILABLE -
   unverified)`, record that: the gates are then driven without read-back.
3. [ ] **Disarm during the 10 s.** From the same phone: Send Armed, then, while
   the Arm is still advertising, set the switch to Disarmed and Send. The app
   lets a Disarm replace an Arm that is still advertising (a new sequence
   number, never a reused one). RTT: `Arming cancelled by slot N n M.` then
   `Arm state: Inactive - arming cancelled (uptime ... ms).` LED A plays the
   ordinary **slow flash** (Disarmed, not the double blink - no armed delay was
   pending). The device never reaches Active, no `Fire pins enabled` line is
   logged, and the meter shows both fire lines low throughout.
   - **Timing near the deadline.** The armed restart at 10 s reconfigures the
     accelerometer and waits for AWAKE to clear, so it can block the main loop
     for up to about 8 s while the device is being disturbed. A Disarm heard
     during that block is acted on just after Active: RTT shows `Arm state:
     Active - fire pins enabled (uptime ... ms).` followed at once by `Arm state:
     Inactive (uptime ... ms).` That is correct behaviour, not a failed cancel.
     Judge it from those RTT lines, not LED A.
4. [ ] **Arm or Settings during the 10 s.** The arming phone cannot do this step:
   the app keeps Arm waiting for its advertising window, and the Settings page is
   unreachable without a confirmed disarm. Use **a second phone on another slot**:
   phone 1 Sends Armed, then phone 2 sends an Arm (or a Settings command) within
   the delay. If no second phone is available, mark this step skipped - it is
   covered on the host by `tests/test_arm_policy.cpp` and
   `tests/test_arming_sequence.cpp`. RTT: `Arming: Arm from slot N n M ignored -
   only a disarm is accepted while arming.` (or `Arming: Settings from slot N n M
   ignored - only a disarm is accepted while arming.`). No LED, no clock trim, no
   settings applied, and the original arming still completes at its original 10 s
   deadline.
5. [ ] **Disarm while Active.** With the device armed (RTT `Fire pins enabled`
   seen), Send Disarmed. RTT: `Arm state: Inactive (uptime ... ms).` and no `Fire
   pin disable failed` line; LED A slow flash. The absence of a disable failure is
   what shows both pins were driven low and disconnected - the meter only confirms
   both fire lines read low. A failed disable logs `Fire output LATCHED FAULTY and
   will not assert again: fire pin disable failed (%d)!`, `Fire pin disable failed
   (%d) - fire pins may NOT be isolated!` and `WARNING (light TBC): fire pins could
   not be isolated (%d)!`, and LED B plays three long pulses.
6. [ ] **Armed trigger.** Trigger the device while armed: the fire lines
   assert (meter and/or the existing fire-output check), then RTT `Trigger
   complete - latched Inactive. Re-arming needs an engineer command.` and
   `Arm state: Inactive (uptime ... ms).` Meter: both fire lines low
   immediately afterwards.
7. [ ] **Failure path - LED B warning.** Only if a failure can be induced safely:
   for example, disconnect the ADXL367 or hold it unresponsive so the armed
   restart fails. Otherwise skip this step and note why.
   Send Armed and wait out the 10 s exit delay. Expect one of two pairs of lines.
   A failed restart logs `Arming failed at the detection restart (%d) - device
   Inactive, fire pins disabled, no acknowledgement!` with `WARNING (light TBC):
   arming failed - detection would not restart armed (%d)!`.
   A failed enable logs `Arming failed at the fire pin enable (%d) - device
   Inactive, fire pins disabled, no acknowledgement!` with `WARNING (light TBC):
   arming failed - fire pins would not enable (%d)!`.
   **LED B plays three long pulses** (700 ms on / 300 ms off for 3 s) and **LED A
   plays no rapid flash** - no acknowledgement of any kind.
   The device ends Inactive, the meter shows both fire lines low, and the test
   resumes disarmed.

## 5d. Scan reliability and Send/Stop (plan 2026-09-14)

1. [ ] After flashing, confirm the boot log shows `Passive scan started: 100 ms
   window every 5906 ms.`
2. [ ] Mac: 10 Sends to the device. Count how many are heard at the first or
   second scan (RTT `Command slot ... n ...` or LED A's pattern). Expect nearly
   all - 5906 ms is no longer a near-integer-multiple of the Mac's 187.5 ms
   advertising interval.
3. [ ] Arm, then within the 10 s exit delay: press **Stop**, set Disarmed, **Send**.
   Expect `Continuous scan: arming exit delay.` and `Scan cadence now
   CONTINUOUS.` logged when the Arm was accepted, the cancel heard within about
   a second of the Disarm advertising, and `Scan cadence now duty-cycled.` once
   arming is cancelled.
4. [ ] Let an arming complete instead of cancelling it: once Active, confirm the
   cadence has returned to duty-cycled (`Scan cadence now duty-cycled.`) -
   continuous scan covers the exit delay only, not the armed state itself.
5. [ ] Press **Stop** during an in-flight Send: the app's advert stops at once
   rather than running the full 30 s; with the observer tool
   (`tools/uuid_observer`) running, confirm the UUID stops appearing.
6. [ ] Optional, with the observer running: watch for the previous payload
   being re-broadcast for a second or two at the start of the next Send (see
   the 2026-09-14 bench log entry below) - harmless, documented in
   `docs/superpowers/specs/2026-09-14-scan-reliability-amendment.md` §5.

## 9. Bench log

### 2026-09-14 — first bench session (J-Link 853003346, nRF54L05, device 0xFBACBE88)

**Passed:** §1 (after the heap fix below), §2, §3, §4. The item 3.6 04:00 rollover
was not run (it needs the board left powered overnight). Also seen early from §6:
the one-shot latch, and LED A's single blink once framed.

**Fixed on the bench:**

- **Crypto self-test failed: `HMAC key import failed: -141`.** `-141` is
  `PSA_ERROR_INSUFFICIENT_MEMORY`. PSA allocates imported key buffers from the mbedtls
  heap, which an observer-only build does not get implicitly: Drawer Master gets it
  through Bluetooth SMP. Fixed with `CONFIG_MBEDTLS_ENABLE_HEAP=y`, a 2 KB heap
  (commit `af3657f`).
- **The "settings applied" blink was invisible.** A bench build lights LED A steadily
  while Inactive, and a bare 200 ms on phase changed nothing. Every pattern is now
  framed by 300 ms of dark before and after (commit `6973c78`).

**Observations:**

- **Detection is marginal against the 30 s window.** The first time sync landed with
  only 3 s left. After a later reset the time sync needed **two** Sends. The design
  expects about 99 % per 30 s Send at a 187 ms advertising interval (spec §3), so the
  real rate looks lower. Measure it (§7) before changing `kAdvertiseWindow` or the scan
  cadence.
- **After a reset, commands are silently ignored until a time sync** — correct by
  design, but the app cannot say so. Engineer procedure: *after any reset or battery
  change, provision first.* Candidate for the screen's help text.
- **App UX (deferred, Andy):** Send always advertises for the full 30 s even when the
  device accepted in the first scan. A sliding-bar countdown until the next Send would
  help. Not to implement yet.

**DECISION NEEDED — unheard Sends can exhaust the sequence window.** Every Send reserves
a new sequence number whether or not the device hears it. The device accepts only the
next 16 per slot (`AccessControl::M_WINDOW`). More than 16 unheard Sends to one device
on one day — out of range, clock invalid after a reset, or jammed — push the app's
counter past the window, and that slot is locked out **until the 04:00 UTC key
rollover**. There is no recovery short of waiting, or using another slot. Options:

- **A.** The app refuses a new Send to the same device while the previous one is still
  advertising, or until a cool-off period has passed.
- **B.** The app counts unconfirmed Sends per device and day, warns as the count nears
  16, and refuses at 16.
- **C.** Widen the device's window (e.g. 32 or 64). This costs RAM (4 bytes × slots ×
  window for the ID table) and HMACs at each rollover, and it proportionally increases
  what a captured-ID attacker can reach.

Also flagged in `docs/tan-scheme.md` §11.


**§5 halted — Arm overwrites settings (08:03–08:25 UTC).** After re-provisioning,
two Arm Sends produced no RTT line at all (app store at `n 14`, device window from
`n 10`, clocks agree) — **not heard**, cause still open: the app starts its 30 s
countdown even if `startAdvertising` failed, so check the advert with a phone scanner
next time. A later Arm with the app's default sliders was accepted (`n 14`) and
**overwrote the device's 3 activations / 8 s cooldown with 1 / 0 s**; a following Arm
with the real settings was correctly ignored as armed (`n 15`). Arming or disarming is
therefore only safe with knowledge of the settings. **Design changed:** Arm, Disarm and
Settings become separate command types, settings only behind a confirmed disarm in the
app — `docs/superpowers/specs/2026-09-14-command-types-amendment.md`. §5 and §6 resume
on the new build.

**DECIDED (Option A, 2026-09-14) — device settings are never stored in the app.** Useful, but a lost phone
would carry every sensor's sensitivity, activations, cooldown and delay with no expiry.
Options in the amendment §4; the owner chose A. Also flagged
in `docs/tan-scheme.md` §11.

**Command-types build flashed (10:37 UTC).** First Arm Send after re-provisioning was
silent: `west flash --recover` erased `access/v1`, so the device expected slot 1 `n` 0–15
for day 256 while the app's store was at `n 26` — every Send `NotForUs`. **Never reset
the app counter to recover** (nonce reuse under the same day key). Workaround: bench app
switched to engineer slot 2 (local, uncommitted `class_app/lib/main.dart`); slot 1
recovers at the 04:00 UTC rollover. **Rule: after any `--recover` flash, change slot or
wait for the rollover.** This is a second route into the sequence-window DECISION NEEDED
above — a device-side erase, which app-side options A/B cannot help and C only partly.
§5a.2 then passed: `Command slot 2 n 0: Arm, minute 642.`, `Applied:` with the stored
(default) settings.

### 2026-09-14 — scan reliability bench session (second nRF54L15 DK, J-Link 1057733814, running `tools/uuid_observer`; MFS_1 built with `CONFIG_MFS_SCAN_DIAG=y`)

**Root cause found for unreliable commands from the Mac.** The Mac advertises at
a steady **187.5 ms** (~155 adverts per 30 s Send). With
`CONFIG_MFS_SCAN_PERIOD_MS` still at its old default of 6000 ms, 6000 ms is
exactly 32 × 187.5 ms, so every 100 ms scan window landed at the same phase of
the Mac's advertising cycle: MFS_1's `CONFIG_MFS_SCAN_DIAG` counters showed 0 or
1 adverts caught per scan, and whole commands were missed — a time sync missed
by all 6 of its scans, four consecutive Disarms (n 18–21) unheard, and Disarm
n 24 unheard.

**iPhone measured for the first time** (iOS 26.6.1, foreground, release build,
slot 3): a steady **~35 ms** advertising interval for the full 30 s, ~800
adverts per Send. MFS_1 caught 2–3 adverts in every 100 ms scan window, every
command was heard at the first scan, and a cancel sent during the 10 s exit
delay worked. This closes §7 above and the "iPhone re-measure pending" note in
`CLAUDE.md`.

**iOS re-broadcasts the previous payload.** At the start of the next Send, the
observer logged the previous advert's UUID (the time-sync payload) repeating
for about 1.7 s before the new payload appeared — a minute after the previous
advert had ended. Documented as an acceptable, explicable behaviour, not a bug:
`docs/superpowers/specs/2026-09-14-scan-reliability-amendment.md` §5.

**Fix:** `CONFIG_MFS_SCAN_PERIOD_MS` default moved to **5906 ms** (31.5 ×
187.5 ms, so consecutive scans sample opposite halves of the Mac's cycle,
instead of 32 × 187.5 ms landing on the same phase every time), and the
scanner now runs continuously for the whole Arming exit delay (§3, §5d above),
not only for an armed trigger pending. Full derivation:
`docs/superpowers/specs/2026-09-14-scan-reliability-amendment.md`.
