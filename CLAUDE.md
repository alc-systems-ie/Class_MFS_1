# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Current state of this repository

This project is a **fresh skeleton**. It currently contains only:

- `README.md` — title only (`# Class_MFS_1`)
- `src/main.cpp` — empty file
- `.clangd` — clangd tuning for the arm-none-eabi flags emitted into `compile_commands.json`
- `docs/power-budget.md` — energy budget and sleep architecture (see below)
- `docs/tan-scheme.md` — TAN derivation, engineer sheet issue, BLE exchange
- `docs/v1-scope.md` — what the first build does, and the config/preset roadmap

There is **no `CMakeLists.txt`, `prj.conf`, `Kconfig`, `boards/` overlay or `sysbuild.conf` yet**, so the app cannot be built until those are added. Nothing under `src/` is implemented. The hardware and duty cycle are settled (below); the firmware is not.

## Settled design decisions

**Project CLASS** — Covert Local Alarm Sensor System. MFS_1 is the Multi-Function
Sensor 1. Firmware is a simplified `../alc_drawer_master` with the HMAC
challenge/response authentication replaced by one-time TANs (bank-TAN style).

Hardware is the `alc_drawer_master` board **minus the FEM**: **nRF54L05 + nPM2100
+ ADXL367**, board target `nrf54l15dk/nrf54l05/cpuapp`.

| Decision | Value |
|----------|-------|
| Sleep architecture | **System ON idle + RTC wake** — *not* nPM2100 Hibernate |
| Scan | **100 ms passive every 6 s** (1.667% RX duty cycle) |
| ADXL367 | Continuous measurement mode, 100 Hz ODR |
| nRF21540 FEM | **Not fitted** — costs 3 dB TX (+7 dBm native vs +10 dBm) |
| TANs | **Day-indexed**, 10/day, expire at day end — 4 bytes of state, horizon unbounded |
| Day boundary | **04:00 local**, no multi-day validity window |
| Timekeeping | **LFXO** 32.768 kHz (fitted), GRTC-sourced. No external RTC |
| Battery | CR123A, ~2.4 year expected life at ~69 µA average |

Full derivation, component figures with citations, and the reasoning behind each
choice: **`docs/power-budget.md`**. Read it before changing the duty cycle, the
sleep mode, or the FEM configuration — each of those was decided against a
specific numeric constraint recorded there.

**v1 is deliberately minimal** (`docs/v1-scope.md`): cold start, provisioner time
sync, ADXL367 waking the SoC via INT1, an Active/Inactive arm state defaulting to
Inactive, an engineer toggle, and two debug LEDs. **No BLE connection at all in v1**
— TAN (3) + version (1) + toggle (1) = 5 bytes fits the advertising payload, so
there is no GATT, no central role and no transmission. Everything else from Drawer
Master is deferred.

**Hardware hazard — ADXL367 INT2 must never be driven** (`docs/v1-scope.md` §2).
Drawer Master routes INT2 → nPM2100 SHPHLD for its Hibernate wake; MFS_1 does not
use that path, and leaving it driven is dangerous twice over: a 5 s assertion then
release **is** the nPM2100 ship-mode gesture and powers the device off, and SHPHLD
has a **1.9 V absolute maximum** so an ADXL on a 3 V rail would damage the PMIC.
Unmap INT2 in the ADXL config, set `shiphold-longpress = "disable"`, and keep the
ADXL on the 1.8 V LSOUT rail. Also force LDOSW to Ultra-Low Power — in `Auto` it
would sit in High Power forever now that the device never hibernates.

**ARCHITECTURAL INVARIANT — the arm boolean is definitive** (`docs/v1-scope.md`
§1.0). The device tracks `m_arm_state`; the accelerometer is only ever **ANDed**
with it. In the product the output switches a voltage, so firing while deactivated
is dangerous.

- `App::updateOutputState()` is the **only** place the two are combined.
- `App::IsOutputActive()` is the **only** sanctioned read.
- Anything added later — voltage switch, alarm report, event counter, BLE
  notification — calls `IsOutputActive()` and **never** reads INT1, the AWAKE bit
  or `Adxl367::ReadAwake()` directly, and never re-derives the condition.
- LED B is written as `ledB = IsOutputActive();` deliberately, as the example for
  future consumers to copy.

Related: **arming is edge-triggered** (§1.0.1). AWAKE is a level, not a latch, so a
naive `armed && triggered` fires the instant the device is armed on motion that
predates arming — and an engineer handling the device to arm it *is* motion, so
that is the common case, not an edge case.

**ADXL367 loop mode has a mandatory initialization routine** (`docs/v1-scope.md`
§3.1). Referenced mode holds an internal reference that is only valid once the
engine has cycled; configure the real thresholds up front and it never cycles, so
gravity reads as permanent motion and **AWAKE never clears**. The datasheet's
routine forces one cycle with a sub-noise activity threshold and an above-1 g
inactivity threshold, both timers zero, then installs the real values at step 9.
AUTOSLEEP (`POWER_CTL = 0x07`) is not optional. Four bring-up attempts were lost
to inventing a sequence instead of using the published one — **the real datasheet
is at `../alc_help_at_hand/docs/adxl367.pdf`; the markdown summary in
`v3.1.0/alc_mailbox_monitor/` omits the routine entirely.**

Constraints from that analysis that are easy to violate by accident:

- **Hibernate is wrong at this cadence.** It saves ~4 µA of sleep but forces a
  cold boot costing ~42 µA averaged over 6 s. The 5 s / 1 s-scan / Hibernate
  design originally proposed budgets at 815 µA — about 74 days on a CR123A.
- **Hibernate_PT cannot power the ADXL367** — it force-disables LDOSW and resets
  the PMIC registers. Only plain Hibernate can hold LSOUT up in ULP mode.
- **Never enable an nRF21540 LNA for scanning** in any future FEM-equipped
  variant — +5 mA at 1.667% duty is +83 µA, more than doubling the whole budget.
- **The counterpart must advertise at 20–50 ms.** A 100 ms passive window catches a
  20 ms advertiser with certainty; at 152.5 ms detection drops to ~65% per wake.

Full TAN design — derivation, sheet issue, the BLE exchange, seed provisioning:
**`docs/tan-scheme.md`**.

TAN-specific rules that follow from the threat model (engineer gets a paper sheet
for one day; a lost sheet must compromise that day only — `docs/power-budget.md`
§8.1):

- **Never widen TAN validity to a multi-day window.** It would give a lost sheet
  three days of life and let tomorrow's sheet work today. Drift is handled by the
  04:00 day boundary, not by a window.
- **Never accept the date from the presenter.** Validating a TAN against a
  peer-supplied date defeats expiry completely. The device's own clock is the sole
  arbiter, which makes it a security component.
- **The day index must be monotonic** — persisted on rollover, `max(persisted,
  synced)` on boot, never moved backwards.
- **Counter-indexed (iTAN/HOTP) TANs are ruled out** — they never expire unused.
- **UTC only on the device, never local time.** Ireland's GMT/IST switch would put
  the device and the back office a day apart across every DST transition. The
  device has no timezone database and must not acquire one.
- **The device never advertises.** It scans, and connects outward as central once a
  valid TAN arrives. Advertising at any point forfeits covertness. Failures emit
  nothing at all — not even an error.

Battery-change recovery is by **trusted-provisioner time sync**
(`docs/power-budget.md` §8.7): a provisioning key distinct from the TAN seed is
flashed at manufacture, and its holder can set the clock and nothing else. Three
rules there are load-bearing — provision a *key*, never a BLE address (addresses are
spoofable and RPAs rotate); never accept a time earlier than the persisted floor;
and bound forward jumps.

**LFXO is the timebase and its accuracy is a security parameter** — the day
boundary it defines is what makes a lost TAN sheet expire. The crystal is fitted,
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

**Tool platform: Android** (`docs/power-budget.md` §8.7.4). It can advertise
arbitrary manufacturer data, so the device stays scan-only and never advertises.
**Keep both advertising payloads within 12 bytes** — TAN is 4, time sync is 12 —
so an iOS tool stays possible later without a protocol change (iOS
`CBPeripheralManager` cannot send manufacturer data at all; a payload must be
smuggled into a 128-bit service UUID).

**Bench development uses an nRF52840 dongle or spare nRF54L15-DK as the
advertiser, not a phone** (§8.7.5) — scriptable, and it can inject the malformed,
replayed and out-of-range payloads needed to test the monotonic-floor and
bounded-jump rules.

## Workspace context

This is a Zephyr/nRF Connect SDK **application inside an existing west workspace**, not a standalone repo:

```
/Users/andy/nordic/ncs/v3.2.4/     <- west topdir (.west/config, manifest = ncs-serial-modem/west.yml)
├── zephyr/  nrf/  nrfxlib/  modules/  bootloader/   <- SDK trees (NCS v3.2.4)
├── class_mfs_1/                   <- THIS repo (its own git repo, tracked separately)
├── class_templates/npm2100/       <- host-tested driver-class template
└── alc_flush_master/  alc_drawer_master/  alc_hub/ ... <- sibling ALC applications
```

Each application directory is its own git repository; the SDK trees are managed by west at the topdir. `git status` here only ever shows this app's files.

## Build commands

Run west from this directory once a `CMakeLists.txt` and `prj.conf` exist. Board target must be chosen for the actual hardware — the two in use across sibling projects are:

```sh
# Thingy:53 / nRF5340 (dual core — flash both).
west build -b thingy53/nrf5340/cpuapp
west flash --recover

# nRF54L15 DK running the nRF54L05 target.
west build -b nrf54l15dk/nrf54l05/cpuapp -p always
west flash --recover
```

Useful variations used in this workspace:

```sh
west build -b <board> -p always                                  # pristine rebuild
west build -b <board> -- -DEXTRA_CONF_FILE=credentials.conf      # extra Kconfig fragment (secrets kept out of prj.conf)
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
