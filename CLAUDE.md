# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Current state of this repository

v1 app control is implemented and host-tested. `src/app.{hpp,cpp}` holds the state
machine (boot sequence, PMIC/ADXL/LED bring-up, the detection engine, the delay
interlock and the command path); one class per peripheral; `boards/` overlays for
the nRF54L15 DK target; all Kconfig in `prj.conf` plus `credentials.conf` (gitignored,
templated by `credentials.conf.template`) for the device secret and provisioning
key. Units for every on-air and NVS field are in
`docs/superpowers/specs/2026-09-12-app-control-design.md` §6.1. Run the host suite
with `make test` (see **Host-side unit tests** below) before trusting a change to
`AccessControl`, `DeviceClock`, `arm_policy.hpp` or `command_scanner.hpp`. Hardware
verification not yet covered by host tests is tracked in
`docs/superpowers/plans/2026-09-13-bench-checklist.md`.

## Settled design decisions

**Project CLASS** — Covert Local Alarm Sensor System. MFS_1 is the Multi-Function
Sensor 1. Firmware is a simplified `../alc_drawer_master` with the HMAC
challenge/response authentication replaced by **day keys** issued per engineer,
per device, per day (`docs/tan-scheme.md`, rewritten 2026-09-13).

Hardware is the `alc_drawer_master` board **minus the FEM**: **nRF54L05 + nPM2100
+ ADXL367**, board target `nrf54l15dk/nrf54l05/cpuapp`.

| Decision | Value |
|----------|-------|
| Sleep architecture | **System ON idle + RTC wake** — *not* nPM2100 Hibernate |
| Scan | **100 ms passive every 5.876 s** (1.702% RX duty cycle) |
| ADXL367 | Continuous measurement mode, 100 Hz ODR |
| nRF21540 FEM | **Not fitted** — costs 3 dB TX (+7 dBm native vs +10 dBm) |
| Access | **Day keys** — AES-128-CCM commands, rotating IDs, 8 slots, window 16. No paper TANs; protocol version 0x03 with an explicit command type |
| Day boundary | **04:00 UTC**, no multi-day validity window |
| Timekeeping | **LFXO** 32.768 kHz (fitted), GRTC-sourced. **No external RTC** (BOM, reaffirmed 2026-09-13). Invalid on every boot until provisioner sync |
| Battery | CR123A, ~2.4 year expected life at ~70 µA average |

Full derivation, component figures with citations, and the reasoning behind each
choice: **`docs/power-budget.md`**. Read it before changing the duty cycle, the
sleep mode, or the FEM configuration — each of those was decided against a
specific numeric constraint recorded there.

**v1 is deliberately minimal** (`docs/v1-scope.md`): cold start, provisioner time
sync, ADXL367 waking the SoC via INT1, an Active/Inactive arm state defaulting to
Inactive, engineer commands, and the LEDs. **No BLE connection at all in v1** — a
command is one 16-byte encrypted service UUID, so there is no GATT, no central role
and no transmission. Everything else from Drawer Master is deferred.

**Hardware hazard — ADXL367 INT2 must never be driven** (`docs/v1-scope.md` §2).
Drawer Master routes INT2 → nPM2100 SHPHLD for its Hibernate wake; MFS_1 does not
use that path, and leaving it driven is dangerous twice over: a 5 s assertion then
release **is** the nPM2100 ship-mode gesture and powers the device off, and SHPHLD
has a **1.9 V absolute maximum** so an ADXL on a 3 V rail would damage the PMIC.
Unmap INT2 in the ADXL config, set `shiphold-longpress = "disable"`, and keep the
ADXL on the 1.8 V LSOUT rail. Also force LDOSW to Ultra-Low Power — in `Auto` it
would sit in High Power forever now that the device never hibernates.

**ARCHITECTURAL INVARIANT — the arm boolean is definitive** (`docs/v1-scope.md`
§1.0). The device tracks `m_arming.State()` (`ArmingSequence`,
`src/arming_sequence.hpp`) — `Inactive`, `Arming` or `Active`; the accelerometer
is only ever **ANDed** with it. In the product the output switches a voltage, so
firing while deactivated is dangerous.

- `App::updateOutputState()` is the **only** place the two are combined.
- `App::IsOutputActive()` is the **only** sanctioned read.
- Anything added later — voltage switch, alarm report, event counter, BLE
  notification — calls `IsOutputActive()` and **never** reads INT1, the AWAKE bit
  or `Adxl367::ReadAwake()` directly, and never re-derives the condition.
- `m_output_switch.Set(m_output_active);`, called from inside
  `updateOutputState()` itself, is the worked example future consumers copy. LED B
  (`ledB = m_detection_met;`) is a bench-only detection indicator, not an output
  consumer, and is gated out of production builds.
- Armed, the device's only path out is a disarm command or a one-shot trigger —
  see `DecideCommand()` (`src/arm_policy.hpp`), restated under Access rules below.
  While **Arming**, the only command that acts is Disarm, which cancels it — see
  below.
- **Disarmed differs from armed in exactly two ways** — parameters change only
  while disarmed, and a trigger shows on LED B instead of the fire GPIOs.
  Detection, cooldown and delay run identically; Disarm restarts a test from
  zero; arming always starts a fresh session
  (`docs/superpowers/specs/2026-09-14-disarmed-test-mode-amendment.md`).
- **Fire pins are isolated unless armed** — `GPIO_DISCONNECTED` with external
  pull-downs; disarm disables them first; arming waits 10 s, restarts detection,
  enables the pins last, and any step failure fails safe to the warning
  (`docs/superpowers/specs/2026-09-14-arming-sequence-amendment.md`).
  `App::signalWarning()` is the one warning path: it logs `WARNING (light TBC)`
  and, **until the dedicated warning light is chosen, plays three long pulses on
  LED B in every build** (production included), overriding the bench detection
  level while it plays. Sources: an arming-step failure, the boot pin check, a
  fire-pin disable failure, a fire switch `Set()` failure while Active (once
  per Active session), and the scanner not running while Active. An arming failure shows **no** LED A acknowledgement.
  **A fire switch fault while Active fails safe to disarmed** — the main loop runs
  the ordinary disarm beside the trigger latch and logs `Fire switch fault while
  armed - disarmed (fail safe).`, with no LED A acknowledgement (amendment §4.1).
  `Set(false)` **latches and isolates** on a failed clear write *or* a clear
  read-back that is not low, logging once (both gates = the `BOTH FIRE GATES
  FAILED TO CLEAR` emergency, one = lost redundancy).
  `OutputSwitch::Disable()` **latches the switch faulty on any failure**,
  including a disconnect-only failure that leaves both gates safely low — so a
  device that will not arm after a "Fire output LATCHED FAULTY" log is expected,
  not a further symptom to chase. `GPIO_DISCONNECTED` on the fire pins currently
  behaves cleanly only because they sit on `gpio2`, which has **no GPIOTE
  instance** on this SoC; re-check disconnect/reconfigure behaviour from
  scratch if the fire pins ever move to `gpio0` or `gpio1`.
- **Always fail safe** (owner rule 2026-09-14): any fault the device depends on — fire switch, arming step, command scanner, while arming or armed, including a pending trigger delay — isolates the pins, disarms, cancels any pending trigger and raises the warning. Never keep firing capability through a fault.
  For the scanner: `App::Run()` disarms the moment `IsScanning()` is false while
  Active (`Scanner not running while armed - disarmed (fail safe)!`), before the
  output is derived; the detection engine additionally never fires an armed delay
  that ran without a scanner (`DelayExpiredScanLostSuppressed`). This supersedes
  the 2026-09-13 "prioritise fire" ruling.

Related: **arming is edge-triggered** (§1.0.1). AWAKE is a level, not a latch, so a
naive `armed && triggered` fires the instant the device is armed on motion that
predates arming — and an engineer handling the device to arm it *is* motion, so
that is the common case, not an edge case. The device therefore **reconfigures the
ADXL367 afresh on every arm — and on every test restart — so there is no stale
level to inherit**. The order is load-bearing and now also carries the fire-pin
isolation (`docs/superpowers/specs/2026-09-14-arming-sequence-amendment.md`):
**arm** is accepted → **Arming**; nothing for 10 s (`M_EXIT_DELAY_MS`, LED A dark,
LED B suppressed); then, synchronously, restart detection armed (configure, confirm
AWAKE clear) → enable the fire pins **last** → boolean Active only on every step's
success, LED A plays Armed. **Disarm**, from any state: disable the fire pins
**first** → boolean Inactive (cancel any arming) → re-derive the output (GPIOs
already isolated) → restart the test. Arming is **refused** — fails safe to the
warning, disable pins, Inactive, test resumed disarmed — if any step fails,
including the part not configuring.

**ADXL367 loop mode has a mandatory initialization routine** (`docs/v1-scope.md`
§3.1). Referenced mode holds an internal reference that is only valid once the
engine has cycled; configure the real thresholds up front and it never cycles, so
gravity reads as permanent motion and **AWAKE never clears**. The datasheet's
routine forces one cycle with a sub-noise activity threshold and an above-1 g
inactivity threshold, both timers zero, then installs the real values at step 9.
AUTOSLEEP (`POWER_CTL = 0x07`) is not optional. Four bring-up attempts were lost
to inventing a sequence instead of using the published one — **the real datasheet
is at `../../datasheets/adi/ADXL367_Datasheet.pdf`; the markdown summary in
`v3.1.0/alc_mailbox_monitor/` omits the routine entirely.** (It was previously
cited as `../alc_help_at_hand/docs/adxl367.pdf`, which does not resolve from this
workspace — `alc_help_at_hand` lives under `v3.1.0`.)

Constraints from that analysis that are easy to violate by accident:

- **Hibernate is wrong at this cadence.** It saves ~4 µA of sleep but forces a
  cold boot costing ~42 µA averaged over 6 s. The 5 s / 1 s-scan / Hibernate
  design originally proposed budgets at 815 µA — about 74 days on a CR123A.
- **Hibernate_PT cannot power the ADXL367** — it force-disables LDOSW and resets
  the PMIC registers. Only plain Hibernate can hold LSOUT up in ULP mode.
- **Never enable an nRF21540 LNA for scanning** in any future FEM-equipped
  variant — +5 mA at 1.702% duty is +85.1 µA, more than doubling the whole budget.
- **The counterpart's advertising interval is not ours to set.** Measured
  (2026-09-14, `docs/superpowers/specs/2026-09-14-scan-reliability-amendment.md`
  §1): **187.5 ms, steady**, from a Mac (`CoreBluetooth`, foreground); **~35 ms,
  steady for 30 s**, from an iPhone (iOS 26.6.1, foreground, release build).
  Both are well outside the 20–50 ms this project originally assumed, and
  iOS/macOS do not expose the interval as a setting. The app compensates with a
  30 s advertising window rather than a fast interval. **Check any period or
  newly measured interval with `tools/scan_phase_check.py`** — a phase-coverage
  simulation, not a closed-form rule; a prior closed-form rule (d × N ≥ I − W)
  was proved unsound (off by one, and invalid once the drift exceeds the scan
  window) and is withdrawn. `CONFIG_MFS_SCAN_PERIOD_MS` at 6000 ms was exactly
  32 × 187.5 ms, so whole commands were missed; the default is now **5876 ms**
  (5875.625 ms real, 9401 BLE units), which the script reports as a clean pass
  (zero misses) against the Mac's 187.5 ms and every Apple interval up to
  318.75 ms, including the 211.25 ms Apple recommends. Intervals from about
  5 × the 100 ms window upward cannot be fully covered by any period at this
  scan count — the owner decisions below (foreground-only sending; no scanner,
  no arming) are what actually bound that case, not the period. The production
  Android phone's interval is still to be measured and checked with the script.

Full access design — derivation, key issue, wire format, acceptance, time, threat
review: **`docs/tan-scheme.md`**. The wire format and firmware units are in
`docs/superpowers/specs/2026-09-12-app-control-design.md`.

Access rules that follow from the threat model (an engineer's phone holds day keys
for its assigned devices; a lost phone must compromise those devices until the
next 04:00 UTC only — `docs/power-budget.md` §8.1):

- **Never widen key validity to a multi-day window.** It would give a lost phone
  three days of life and let tomorrow's key work today. Drift is handled by the
  04:00 day boundary and bounded trim, not by a window.
- **Never accept the date from the presenter.** The day is an input to the key, and
  the device derives it from its own clock. That makes the clock a security component.
- **Every code must expire at the day boundary.** A sequence number nested under a
  day key is fine; a bare HOTP/iTAN counter, which never expires unused, is not.
- **Never reuse a sequence number.** It is the CCM nonce: the app saves `n + 1`
  before advertising, and two phones never share a slot.
- **The day index never moves backwards.** Persisted on rollover as a floor; syncs
  below it are refused; trims may not cross below it.
- **The clock is invalid on every boot** until an authenticated provisioner sync.
  Never resume the day from NVS — that would revive a past day's keys.
- **UTC only on the device, never local time.** Ireland's GMT/IST switch would put
  the device and the back office a day apart across every DST transition. The
  device has no timezone database and must not acquire one.
- **Only slot 0 (the Network Manager) may change the operating mode.**
- **Arm, Disarm and Settings are separate command types**
  (`docs/superpowers/specs/2026-09-14-command-types-amendment.md`). Arm and Disarm
  carry no settings, so an engineer can arm or disarm without knowing the device's
  tuning; settings are applied only by a Settings command while Inactive. **Armed,
  the only state change is disarm** — Arm or Settings to an armed device changes
  nothing and replays the Armed pattern on LED A. **While Arming (the 10 s exit
  delay), only Disarm acts** — it cancels the arming (§2 disarm order); Arm and
  Settings are ignored outright, logged, with no LED and no clock trim
  (`docs/superpowers/specs/2026-09-14-arming-sequence-amendment.md` §3.1).
  **Triggers are one-shot** and latch the device Inactive, so disarm and trigger
  are the only two ways out of the armed state. `DecideCommand()`
  (`src/arm_policy.hpp`) is the single place this is decided — never add a second
  path in `App`.
- **The app never stores device settings** (owner decision 2026-09-14, amendment
  §4) — a lost phone must not become a map of every sensor's tuning.
- **No scanner, no arming** (owner decision 2026-09-14, amendment §6). An Arm is
  refused when the scanner is not running, both when the Arm is accepted (no
  Arming state entered) and, fail-safe, at the end of the exit delay before the
  armed restart — a device that could not hear a Disarm must never reach Active.
  The refusal raises the warning pattern (LED B) naming the scanner, gives no LED
  A acknowledgement, and leaves the device Inactive with pins isolated and the
  detection test running. Checked in the host-tested `ArmingSequence` via
  `ArmingActions::ScannerRunning()` — never a second check in `App`.
- **Persist before acting**, and **failures emit nothing** — no advert, no LED. LED A
  acknowledges only *accepted* commands, never a failed authentication.
- **The device never advertises to solicit contact.** It scans. Report modes are
  documented exceptions (design spec §4.3). Advertising forfeits covertness.
- **A corrupt `access/v1` record refuses commands every boot** (`access_store.cpp`
  logs "Stored access state is invalid"). There is no in-field recovery — the fix
  is a wired erase of the settings partition followed by re-provisioning, never an
  attempt to parse around the corruption.

Hardware and end-to-end verification this workspace's agents are barred from
running (no flashing, no J-Link, no RTT) is tracked as a single ordered checklist:
**`docs/superpowers/plans/2026-09-13-bench-checklist.md`**. Work through it on the
bench before trusting any claim that a change "works" beyond the host test suite.

Battery-change recovery is by **trusted-provisioner time sync**
(`docs/tan-scheme.md` §7.2): a provisioning key distinct from the device secret is
flashed at manufacture, and its holder can set an *invalid* clock and nothing else.
Load-bearing rules: provision a *key*, never a BLE address (addresses are spoofable
and RPAs rotate); refuse syncs while the clock is valid; never below the persisted
floor; never more than 400 days past it.

**LFXO is the timebase and its accuracy is a security parameter** — the day
boundary it defines is what makes a lost day key expire. The crystal is fitted,
and `CONFIG_CLOCK_CONTROL_NRF_K32SRC_XTAL` / `CONFIG_NRF_GRTC_TIMER_SOURCE_LFXO`
are already the defaults for this target, so no Kconfig work is needed.

Crystal is an **Abracon ABS06N-32.768kHz-9-T, CL = 9 pF**, so `&lfxo` is set to
`load-capacitance-femtofarad = <9000>` explicitly in the overlay. It had been
silently inheriting the DK's **17000 fF** — an 8 pF over-capacitance that pulls the
oscillator ~45–50 ppm slow, roughly 25 min/year (`docs/power-budget.md` §8.5.3).
`alc_drawer_master` still carries that inherited value.

**A mispulled LFXO never announces itself** — BLE tolerates 500 ppm, so links work
fine at 100 ppm and nothing logs an error; only accumulated drift shows it. There
is also no LFRC calibration driver on nRF54L (`nrf_clock_calibration.c` is
nRF52-era), so the crystal is the only accurate option.

**OPEN — trim `&lfxo` by measurement.** 9000 fF is nominal from the part number,
not measured; board stray shifts the optimum, and note `&hfxo` sits at 14000 fF
against a nominally 8 pF crystal on this same board.

**Tool platform: Android in production, one Flutter app** (`docs/power-budget.md`
§8.7.4). **Every payload is exactly one 128-bit service UUID — 16 bytes** — so the
same protocol works from iOS (`CBPeripheralManager` cannot send manufacturer data at
all). There is **no fixed prefix**: bytes 0–3 are a rotating ID only a key holder can
produce.

**Foreground-only sending** (owner decision 2026-09-14, amendment §6): Send is
disabled unless the app lifecycle is `resumed`; when the app becomes `hidden`,
`paused` or `detached`, any advert (or a pending start) is stopped at once and the
page shows "Advertising stopped - keep the app open while sending." `inactive`
(visible but unfocused — a macOS window losing focus, or an iOS system sheet) does
**not** stop an advert. This is what actually bounds the slow-advertiser case the
scan period alone cannot cover (see the Scan section above).

**Malformed, replayed and out-of-range payloads are tested on the host** (plan
Tasks 3–5), against
`AccessControl` and `DeviceClock` with vectors from `tools/gen_access_vectors.py`,
rather than injected from a dongle as `docs/power-budget.md` §8.7.5 originally
planned — every rule lives in pure logic, so the host tests reach paths a
well-behaved advertiser never would.

## Workspace context

This is a Zephyr/nRF Connect SDK **application inside an existing west workspace**, not a standalone repo:

```
/Users/andy/nordic/ncs/v3.2.4/     <- west topdir (.west/config, manifest = ncs-serial-modem/west.yml)
├── zephyr/  nrf/  nrfxlib/  modules/  bootloader/   <- SDK trees (NCS v3.2.4)
├── class_mfs_1/                   <- THIS repo (its own git repo, tracked separately)
├── class_app/                     <- Flutter provisioner/engineer app, its own git repo
├── class_templates/npm2100/       <- host-tested driver-class template
└── alc_flush_master/  alc_drawer_master/  alc_hub/ ... <- sibling ALC applications
```

Each application directory is its own git repository; the SDK trees are managed by west at the topdir. `git status` here only ever shows this app's files.

## Build commands

Board target is the nRF54L15 DK running the nRF54L05 target. **Every normal build
needs the bench credentials** (the device secret and provisioning key), kept out of
`prj.conf` in gitignored `credentials.conf`:

```sh
west build -b nrf54l15dk/nrf54l05/cpuapp -p always -- -DEXTRA_CONF_FILE=credentials.conf
west flash --recover
```

**Always flash this board with `--recover`** — `west flash` alone has reported
success while the old image kept running (see **West flash needs `--recover`** in
session memory).

Useful variations used in this workspace:

```sh
west build -b <board> -p always                                  # pristine rebuild, no credentials — boots Inactive, access control unavailable
west build -t menuconfig                                         # inspect resolved Kconfig
```

`compile_commands.json` is generated into `build/` — symlink or copy it to the project root so clangd (configured by `.clangd`) resolves includes.

## Host-side unit tests

The established pattern for driver classes in this workspace is a plain `g++` test runner with a mock transport, not Twister — see `../class_templates/npm2100/`:

```sh
make test          # compiles driver + tests/test_*.cpp into ./test_runner and runs it
make clean
```

Its flags (`-std=c++20 -Wall -Wextra -Wpedantic -Werror`) are the reference for any host-test target added here. There is no single-test filter in that harness; a single case is run by compiling only the relevant `tests/test_*.cpp`.

## Bench tool — `tools/toggle_dongle` (retired)

**Retired 2026-09-13.** MFS_1 no longer accepts this tool's payload — commands are
encrypted under day keys (`docs/tan-scheme.md`), not an unauthenticated toggle byte.
Superseded by `../class_app`. The directory is kept, not deleted, for its
Thingy:53 build notes, three of which stay valid for any future Thingy:53 work:

- **An nRF5340 app with `CONFIG_BT=y` is only half a Bluetooth build**, and the
  Thingy:53 defaults the other half to *nothing*. The radio is on the network
  core; the board's own `Kconfig.sysbuild` sets `SECURE_BOOT_NETCORE=y`, which
  sets `NRF_DEFAULT_EMPTY=y`, so `NETCORE_EMPTY` wins the `NETCORE` choice. The
  build then **succeeds**, `bt_enable()` fails at boot, and on a tool with no
  console the button simply does nothing. The fix is a `Kconfig.sysbuild` in the
  application directory:

  ```
  source "share/sysbuild/Kconfig"

  config NRF_DEFAULT_IPC_RADIO
  	default y

  config NETCORE_IPC_RADIO_BT_HCI_IPC
  	default y
  ```

  These are *defaults*, not assignments, so the same file is inert on single-core
  targets — `SUPPORT_NETCORE` is unset there and the choice never appears. One
  file serves every board. `nrf/sysbuild/netcore.cmake` then applies ipc_radio's
  `overlay-bt_hci_ipc.conf` itself, so the netcore image needs no config from us.
  Pattern copied from `nrf/samples/bluetooth/peripheral_uart`.

- **Never bind a demo button to a board's `mcuboot-button0`.** On the Thingy:53
  that alias is `button1` (P1.13); holding it through a reset enters MCUboot
  serial recovery. The single enclosure pushbutton is `button0` (P1.14), already
  aliased `sw0`. Nordic's own Thingy:53 applications treat `button0` as *the* user
  button. (On the nRF52840 Dongle the two are unavoidably the same button — it has
  only one — which is harmless, but it is why the distinction is worth keeping
  where a board offers a choice.)

- **On a board with one RGB LED, the alias picks the colour.** Thingy:53 `led0` is
  **red** (P1.08), `led1` green (P1.06), `led2` blue (P1.07). Red reads as a fault
  on a demo, and blue is `mcuboot-led0` and blinks during DFU. Remap the alias in
  the board overlay rather than changing application code:

  ```dts
  / { aliases { led0 = &green_led; }; };
  ```

## READ THIS BEFORE TOUCHING HARDWARE

**`../alc_drawer_master/docs/superpowers/handoff_02072026.md` §5, "Lessons learned".**
It is the accumulated hardware knowledge for *this exact board* and it is not
obvious from the code. Copying a driver from a sibling is not enough — read how
the sibling **calls** it, and read its hand-off notes. Two faults in this project
cost hours because that was skipped:

- **Lesson 5 — the nPM2100 boot monitor resets the host ~9 s after boot unless
  firmware calls `TimerStop()` early.** It is sticky and survives a reflash.
  `App::initPmic()` does this now. Presented as LEDs blinking on a ~6 s cycle, an
  arm state that would not stick, and RTT going silent after boot.
- **Lesson 7 — keep `CONFIG_LOG_MODE_IMMEDIATE=y` when debugging a hang or reset.**
  Deferred logging hides the hang point. Also: `west build -- -DCONFIG_X` does not
  reliably reach the app under sysbuild; set Kconfig in `prj.conf`.

Also relevant and already handled here: lesson 2 (latched ADXL INT2 reads as
`PowerOffButton` on SHPHLD — we leave INT2 unmapped *and* call
`DisablePowerOffButton(true)`), and lesson 3, which independently identifies
**ADXL autosleep/AWAKE mode** as the correct approach over raw latched activity —
which is the design in `docs/v1-scope.md` §3.1.

Lesson 1 (anti-bricking during Hibernate) does not apply to MFS_1: the device
never hibernates, so it is always awake and flashable.

## Reference projects

When adding structure, mirror the layout of the nearest sibling rather than inventing one. `../alc_flush_master/` and `../alc_drawer_master/` are the most complete examples (`src/main.cpp` + `src/app.{hpp,cpp}` holding the state machine, one class per peripheral, `boards/<board>.overlay`, all Kconfig in `prj.conf`, project-specific `CLAUDE.md` documenting hardware and register-level design decisions). Their `CLAUDE.md` files are worth reading for the house patterns in practice.

## Style

**`.clang-format` is authoritative for layout — run it on every file you touch:**

```sh
/Users/andy/nrfenv/bin/clang-format -i src/<file>
```

Imported from `../alc_drawer_master/.clang-format` with two deliberate
divergences, so trivial guard clauses collapse to one line **while keeping their
braces**:

```yaml
AllowShortIfStatementsOnASingleLine: WithoutElse   # was: false
AllowShortBlocksOnASingleLine: Always              # new
```

giving `if (result < 0) { return result; }`. It stays conservative — two
statements, an `else` branch, a loop body, or a line over 150 columns all expand
normally. `AllowShortBlocksOnASingleLine` is the setting doing the work; the
short-if setting alone has no effect when braces are present.

**Braces are never removed.** `RemoveBracesLLVM` is deliberately unset: it is
documented as experimental, and brace-less control flow is the `goto fail;` shape.

Note `ReflowComments: false` — reindenting a Doxygen block moves the opening
`/**` but leaves the ` * ` continuation lines behind, so they need a hand pass
after a large reformat.

All code follows the ALC house style in `~/.claude/CLAUDE.md` (C++20, `.cpp`/`.hpp`, `alc` namespace, `m_`/`s_`/`M_` prefixes, PascalCase public methods vs snake_case SDK calls, RTT logging). That file is authoritative; do not restate or contradict it here. Project-specific architecture, register maps and design decisions belong in this file as they are established.
