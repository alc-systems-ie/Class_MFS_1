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

- [ ] Boot log shows `Crypto self-test passed: 3 command vectors and the time-sync vector match.`
- [ ] Then `Device 0x........ ready. Clock INVALID until a provisioner time sync.`
- [ ] If it fails, record the line. Suspect CRACEN and the 4-byte tag or the 11-byte nonce (`PSA_ERROR_NOT_SUPPORTED` = -134). Do not weaken the test.
- [ ] Once it passes, mark spec §9 item 5 RESOLVED (with the log line) and delete the "PSA self-test on target" row from `docs/tan-scheme.md` §11.
- [ ] Rebuild **without** `-DEXTRA_CONF_FILE` and flash. Expect `Credentials missing or malformed`, and the device boots Inactive. Reflash the credentials build.

## 2. Boot and access state (Task 9)

- [ ] First boot: `No access state stored - first boot, no day floor.`
- [ ] `Passive scan started: 100 ms window every 6000 ms.`
- [ ] Temporarily enable `CONFIG_THREAD_ANALYZER=y` (and `CONFIG_THREAD_ANALYZER_AUTO=y` or a manual call point), flash, provision, send a command and trigger the device, then record the reported high-water marks for the main thread and the Bluetooth RX thread. Revert the Kconfig afterwards.

## 3. Provisioning and commands, end to end (Task 13)

- [ ] Device picker → clock icon → **Send time sync**. Within 30 s: `Clock set by provisioner: unix ..., day ..., HH:MM UTC`, and HH:MM matches the Mac's UTC time.
- [ ] Send the time sync again: nothing is logged (the clock is valid).
- [ ] MFS_1 screen, Armed off, move sliders, **Send**. The `Command slot 1 n 0:` line shows exactly the app's values, the current UTC minute, and **one blink** on LED A.
- [ ] **Send** again without changes: `n 1` (new bytes each time).
- [ ] Reset the board (`nrfutil device reset --serial-number 853003346`). Expect `Access state restored: day floor <today>` and the clock invalid. A **Send** logs nothing. Provision, **Send**: accepted, and `n` continues upward.
- [ ] Leave the board powered past 04:00 UTC with no commands, reset it, and confirm the restored floor is the new day (the daily rollover persisted without commands).

## 4. Detection engine (Task 14)

- [ ] Tune (Armed off) with activations 3 and cooldown 8 s (byte 66), **Send** (one blink). Handle the device: LED B lights about 5 s per detection.
- [ ] Armed on, **Send**: **rapid flash**. Tap three times with pauses: `Activation 1 of 3`, `Cooldown started: 8 s`, `Cooldown elapsed - detection re-armed`, … `Output ASSERTED`.
- [ ] Tapping during the cooldown does **not** increment the count.
- [ ] Power-cycle: the boot `Settings:` line shows the values sent (NVS).

## 5. Delay and interlock (Task 15) — STOP if 5.3 fails

1. [ ] Tune activations 1, delay 30 s; arm (rapid flash); trigger. Expect `TRIGGER PENDING: firing in 30 s` and `Scan cadence now CONTINUOUS`.
2. [ ] Wait the full 30 s: `Output ASSERTED`, LED B and the fire output for **about 5 s**, then `Output cleared`, `Trigger complete - latched Inactive.`, and LED A lights.
3. [ ] **Repeat, and disarm at about 15 s** (Armed off, **Send**): **double blink**, `Disarmed with a trigger PENDING`, the output **never** asserts, and the scan returns to duty-cycled. **This is the most important check on the list.**
4. [ ] Repeat and **reset mid-delay**: comes up Inactive, clock invalid, no trigger.
5. [ ] Delay 0: firing is immediate.

## 6. Armed path, one-shot, LED A (Task 16) — STOP if 6.2–6.4 fail

1. [ ] Armed off, Tune: one blink. Report+Trig and Report are disabled in the app's mode selector as of the final fix wave (Task 18 item 3) - confirm they cannot be selected and the note under the selector is visible. To exercise the firmware's refusal directly, send a hand-crafted slot-0 command with the mode bits set to `10` (Report): expect **one blink** (SettingsApplied, not ModeChanged - the mode is refused) and `Mode 2 refused - reporting is not implemented yet; device stays Trigger only.`. Power-cycle and confirm the boot `Settings:` line still reports mode 0.
2. [ ] Armed on (activations 1, delay 0), **Send**: rapid flash. **While still armed**, change the sensitivity and **Send** again: **no flash, nothing changes**, `Armed: command slot 1 n ... ignored`. The Network Manager button is disabled while the Armed toggle is on.
3. [ ] Trigger: output for about 5 s, then `Trigger complete - latched Inactive.`. Trigger again: **the output never asserts**.
4. [ ] Arm with delay 60 s, trigger, and within the minute set Armed off **with activations changed to 7**, **Send**: **double blink**, no fire, and `Applied:` still shows the **old** activation count. Armed off, **Send** again: one blink, and now 7.
5. [ ] **Tune-then-arm (the fixed hazard):** Tune with activations 1 and delay 60 s, handle the device, then arm within 60 s. The output must **not** assert a minute later.
6. [ ] Build with `CONFIG_MFS_DEBUG_LED=n` in prj.conf, flash, provision, and arm: the rapid flash still plays, LED A is **dark** afterwards, and it stays dark after a one-blink Tune. Restore `CONFIG_MFS_DEBUG_LED=y`.
7. [ ] Arm Refused (optional, needs the ADXL367 disconnected): three long pulses, device Inactive.

## 7. iPhone advertising interval (Task 13, spec §9 item 2)

- [ ] Temporarily raise `CONFIG_BT_RX_STACK_SIZE` to 4096 in `prj.conf` for this step - the added `LOG_INF()` below runs on the Bluetooth RX thread in immediate log mode, and the normal 2048-byte stack is sized on the assumption that this thread never logs (see the comment on `s_dropped` in `src/command_scanner.cpp`). Revert afterwards.
- [ ] Temporarily add `LOG_INF("UUID seen at %lld ms.", k_uptime_get());` at the top of `parseAdStructure()`'s UUID branch in `src/command_scanner.cpp`, flash, `flutter run -d <iphone>`, and **Send** once. Record the interval between timestamps. **Revert both changes and reflash.**
- [ ] If the interval is materially different from 187 ms, update `kAdvertiseWindow` in `class_app/lib/services/advertiser.dart` and spec §3's detection table, and `CLAUDE.md`'s interval note.

## 8. Recovery notes

- A corrupt `access/v1` record makes the firmware refuse commands on every boot (`Stored access state is invalid`). The only recovery is a wired erase of the settings partition, followed by re-provisioning.
- If the scanner is lost during a pending delay, the trigger **still fires** (owner decision 2026-09-13) and logs `Trigger firing although the scanner was not running during the delay`.
- A nearby advertiser flooding random 128-bit UUIDs is an RF-jamming-class attack on the 8-entry candidate queue, not just noise: none of the garbage counts towards the lockout (§6.4 of `docs/tan-scheme.md`), but if it arrives faster than `serviceCandidates()` can drain it, genuine candidates get dropped. Run a flood test (an advertiser cycling random 128-bit UUIDs at a high rate) and check for `Candidate queue full - N adverts dropped!` in the log; confirm a genuine command still lands once the flood stops.
