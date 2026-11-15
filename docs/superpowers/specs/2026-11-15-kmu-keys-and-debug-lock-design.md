# MFS_1 keys in the KMU, fleet debug lock — design

**Owner:** Andy (brainstormed 2026-11-15). **Repo:** `class_mfs_1`.
**Builds on:** `docs/tan-scheme.md` §8 (secrets and provisioning), the KMU spike of 2026-11-15
(`../kmu_hmac_spike/`, key-test DK J-Link 1057755043).

## 1. Intent

Protect the ten fleet devices (`MFS1-1126-00001` … `0000A`) **(owner)**. Today each device's
`deviceSecret` and `provisionKey` are Kconfig strings compiled into its image from
`secrets/<serial>.conf`; anyone with the image, or a J-Link on the device, has every day key for
that device, forever. After this change the two keys live in the nRF54L05 **KMU** and the debug
port is **locked** **(owner)**, so a held device yields neither the keys nor the use of them.

**Spike result (2026-11-15):** a 256-bit HMAC-SHA256 key in KMU slots (Raw scheme) reproduced
the `docs/tan-scheme.md` dayKey vector byte for byte on NCS v3.2.4, although Nordic's key table
lists HMAC at 128 bits only. **No protocol change:** class_app, the tooling and the vectors stay
as they are.

## 2. Goals, non-goals, anti-goals

**Goals**
- Both keys provisioned into the KMU per device; neither in any image, build directory or
  `.config` for a KMU build.
- Fleet devices debug-locked (AP-Protect and Secure AP-Protect) **(owner: "lock them")**.
- One command per device does it all (owner: option 1, an explicit sequence in
  `tools/flash_device.sh`).
- Host tests keep running the access code with raw keys.

**Non-goals**
- **Erase protection (`UICR.ERASEPROTECT`) stays off (owner).** A locked device stays
  recoverable by ERASEALL, which also wipes its keys.
- Authenticated debug; hub report keys (a separate design: the hub must hold per-device
  *report* keys, never MFS secrets); MCUboot (MFS_1 has none); class_app changes.
- Migrating MFS1-1126-00007 while the six-month daily-report trial runs: it stays on its
  test build until the trial ends **(my call)**.

**Anti-goals — failures even if it works**
- A device locked with wrong or missing keys and no way to tell.
- A secret left in an image, a build directory, `.config`, a log or git.
- A fleet image that can boot with the debug port open.
- Two code paths where a fleet build could silently fall back to Kconfig keys.

## 3. Constraints and facts

- **KMU:** **250 slots** (0–249) × 128 bits on the nRF54L05 (`KMU_KEYSLOTNUM 250`,
  `nrf54l05_application_peripherals.h`; Nordic's KMU overview numbers a 256-slot map, whose
  250–255 are reserved and absent here); application slots **0–179**; 180–249 reserved
  (IKG seed, bootloader keys, protected-RAM invalidation data at 248–249). A 256-bit key takes two consecutive slots. HMAC keys are **Raw scheme
  only**: copied to CPU RAM while in use — so the lock is what protects them from a debugger.
- **KMU slots are write-only:** a key can only be checked by using it.
- **ERASEALL** (`nrfutil device recover`, `erase --all`, `west flash --recover`) wipes RRAM, UICR
  **and the KMU**. It is also the only way to unlock a locked device.
- **nRF54L lock needs firmware and UICR:** firmware without the lock option *reopens* the debug
  port at boot (TAMPC). The fleet image must set `CONFIG_NRF_APPROTECT_LOCK=y` and
  `CONFIG_NRF_SECURE_APPROTECT_LOCK=y`, and the UICR is set with `nrfutil device protection-set
  All`. **Consequence:** once the fleet image boots, RTT is closed — no RTT-based check after it.
- **Plain `west flash` over existing firmware failed to boot on this board** (bench checklist
  §5j.1, unresolved); `--recover` cannot be used after provisioning (it wipes the KMU). Any
  sequence must program only onto an **erased** chip.
- **class_app's bench credentials** are generated from `secrets/<serial>.conf`
  (`tools/gen_bench_credentials.py`), so those files remain the owner's key source.
- `docs/tan-scheme.md` §8.1: the two keys must differ; the firmware also refuses all-zero keys.

## 4. Approaches considered

**Provisioning route** (owner picked 1):
1. **Explicit sequence in `flash_device.sh`** — erase, provision, flash, lock, each step checked.
   **Chosen.**
2. Nordic's `keyfile.json` in the build directory, provisioned by `west flash --recover` — leaves
   a key file in `build/`, easy to reuse for the wrong serial.
3. A provisioning image that writes the keys itself — puts keys in an image again. Rejected.

**Pre-lock check** (the owner's option 1 included "check before lock"; see §1's consequence):
- (a) **End-to-end check after lock, with the app (recommended):** the sequence is erase →
  provision → flash the locked fleet image → set UICR lock; then the owner does the normal
  clock sync (proves `provisionKey`) and one command (proves `deviceSecret`). A failure is
  recoverable: re-run the script (erase-all wipes and redoes). Programs only onto an erased chip.
- (b) RTT key-check value before lock, via a small unlocked "probe" image: needs a second flash
  over the probe — the §5j.1 failure mode — before the fleet image. Rejected for that risk.
- Either way the firmware runs a **boot self-check** (§5.3) and refuses commands, with the
  warning, if its keys are absent, all-zero or identical — so a bad provisioning shows on the
  device's LEDs, not only in the app.

## 5. Design

### 5.1 Key references in the crypto seam

- `crypto::HmacSha256` gains a key-reference overload: `struct KeyRef` holding either raw bytes
  (host tests, the boot self-test vectors, day keys) or a KMU slot (target). Day keys stay raw
  (they are derived per command and short-lived); only `deviceSecret` and `provisionKey` become
  KMU references.
- Target: a KMU reference maps to
  `PSA_KEY_HANDLE_FROM_CRACEN_KMU_SLOT(CRACEN_KMU_KEY_USAGE_SCHEME_RAW, slot)` and is used by
  `psa_mac_compute()` directly (no import, no copy in app RAM). Host (`tests/crypto_openssl.cpp`):
  a test KMU table maps slots to bytes so the same access code runs on the host.
- `AccessControl` and `OpenTimeSync` take a `KeyRef` instead of `const uint8_t*` for the secret
  and the provisioning key; nothing else in the protocol changes.

### 5.2 Slots

| Key | Slots | Scheme | Persistence |
|---|---|---|---|
| `deviceSecret` (256-bit HMAC-SHA256, usage SIGN) | **100–101** | RAW | DEFAULT |
| `provisionKey` (256-bit HMAC-SHA256, usage SIGN) | **102–103** | RAW | DEFAULT |

No export or copy usage flags. The slot numbers are firmware constants and tool constants,
defined once each and cross-checked by a test (§7).

### 5.3 Boot self-check (replaces the Kconfig parse and the zero/equal checks)

At boot, after `crypto::Init()`: compute `HMAC(key, "MFS-KCV" ‖ 0x07)` with each KMU key and
with an imported all-zero 256-bit key. Refuse to enable commands and syncs (existing
`initAccess()` failure path: `LOG_ERR`, warning, commands ignored) if either KMU key is missing
(PSA error), the two results are equal (keys identical), or either equals the all-zero result.
The label and domain byte `0x07` are new and distinct from every protocol message (dayKey 0x02,
encKey 0x03, rotatingId 0x04, timeSync 0x05, confirmId 0x06). Nothing derived from a key is
logged.

### 5.4 Build: no secrets in the image

- New Kconfig `MFS_KEYS_IN_KMU` (default **y**). With it, `MFS_DEVICE_SECRET` and
  `MFS_PROVISION_KEY` are not used by the firmware, and **the build fails** if either is
  non-empty (a fleet build must not carry them). The Kconfig strings remain only for host-side
  tools and are removed from firmware use entirely **(my call: no Kconfig fallback in firmware —
  anti-goal "two code paths")**.
- `prj.conf` sets `CONFIG_NRF_APPROTECT_LOCK=y` and `CONFIG_NRF_SECURE_APPROTECT_LOCK=y` for
  every build. A bench build that needs RTT uses a separate, explicit overlay
  (`debug-unlocked.conf`) that turns both off and logs `DEBUG PORT UNLOCKED - not for the
  field!` at boot **(my call)**; `flash_device.sh` refuses it for a fleet serial unless
  `--unlocked` is given.
- `flash_device.sh` builds with a **derived conf** holding only `CONFIG_ALC_DEVICE_SERIAL` and
  `CONFIG_MFS_DEVICE_ID` (taken from `secrets/<serial>.conf`), written to a temp file and
  deleted after the build. `secrets/<serial>.conf` keeps the secrets for class_app and the key
  file.

### 5.5 `flash_device.sh <serial>` sequence

1. Validate the serial; load `secrets/<serial>.conf`; refuse all-zero or equal keys (host-side
   duplicate of §5.3).
2. Build (derived conf, §5.4); check the image carries the serial; check the built `.config` and
   `zephyr.hex` contain neither secret (grep for the hex strings).
3. Write the key file (`generate_psa_key_attributes.py`, slots per §5.2) to a `mktemp` file,
   `chmod 600`.
4. `nrfutil device recover --serial-number <J-Link>` (unlocks if locked; wipes KMU and NVS).
5. `nrfutil device x-provision-keys --serial-number <J-Link> --key-file <temp>`; delete the
   temp file whether it succeeds or not (`trap`).
6. Program the image with `chip_erase_mode=ERASE_RANGES_TOUCHED_BY_FIRMWARE` — not "no
   further erase": `recover` may leave recovery firmware in the application region, which
   a program with no erase would not clear. The KMU sits in the configuration regions at
   `0x00FFxxxx` and the script refuses any image that reaches them, so the ranges erased
   are never the KMU's.
7. `nrfutil device protection-set All` (UICR; the firmware locks at boot as well).
8. Print the end-to-end check: "Sync the clock from the app, then send one Disarm; if either
   fails, re-run this script."
- `--build` builds only (steps 1–2). `--keep-nvs` is withdrawn: a reflash always erases, so the
  counter-mismatch note in `CLAUDE.md` applies to every flash.
- The J-Link defaults to the MFS bench probe 853003346 (as today); `MFS_DEV_ID` overrides.

### 5.6 Docs

`CLAUDE.md` (keys in the KMU; locked fleet; how to unlock = re-run the script; RTT only with the
unlocked overlay), `docs/tan-scheme.md` §8 (KMU done; slots; Raw-scheme caveat),
`credentials.conf.template`, `tools/gen_fleet_credentials.py` help text, and a bench-checklist
section for the first fleet device.

### 5.6a Existing keys are kept (owner)

The fleet's current keys are **not rotated**: every device and every build of them has stayed
with the owner (owner, 2026-11-15). Migration is a reflash through `flash_device.sh` with the
existing `secrets/<serial>.conf`; class_app's bench credentials and engineers' day keys are
unchanged. Old `build/` directories holding Kconfig keys are deleted after migration **(my
call)**.

### 5.7 Bench and diagnostic builds (my call, builder check)

- **Every MFS device goes through `flash_device.sh`**, the bench unit included: the bench
  identity in `credentials.conf` moves to `secrets/MFS1-0926-00001.conf` (same format), and
  `credentials.conf` is retired from firmware builds (class_app's generator still reads the
  `secrets/` files). A DK is flashed the same way with its own `secrets/<serial>.conf`.
- Bench and diagnostic builds (`MFS_SCAN_DIAG`, `MFS_DIAG_BOOST_PASSTHROUGH`, anything needing
  RTT) are provisioned into the KMU exactly like the fleet and run with `debug-unlocked.conf`
  (`flash_device.sh <serial> --unlocked [--extra-conf <file>]`). There is no build that runs
  without KMU keys and accepts commands.
- `CLAUDE.md`'s "normal build" becomes `tools/flash_device.sh <serial> [--unlocked]`; a bare
  `west build` still builds (no secrets, locked) but the device refuses commands until
  provisioned — the existing "Credentials missing" path.

### 5.8 The trial device and the branch (my call, builder check)

- MFS1-1126-00007 keeps its current test image. The commit it runs is **tagged**
  (`trial-00007-2026-11-14`) so a trial fix can still be built from the tag, Kconfig keys and
  all, on that branch. This design lands on a new branch from `main` and never carries the
  Kconfig path forward. After the trial, 00007 is reflashed through `flash_device.sh` like the
  others.

### 5.9 First run on real nRF54L05 silicon (my call, builder check)

The spike ran on an nRF54L15 DK. The MFS boards are nRF54L05. The plan's hardware steps are:
key-test DK (nRF54L15) for the script end to end; then **one fleet device as an explicit test
step** (KMU 256-bit HMAC on L05 silicon, lock state via `nrfutil device protection-get`,
`recover` restores it), with the app sync and one command; only then the rest. Every step is
recoverable by ERASEALL.

## 6. What is left to the builder

- `KeyRef`'s exact shape and the host KMU table; whether `crypto_psa.cpp` keeps a volatile
  import path for raw keys (it must, for day keys and the self-test).
- Exact `nrfutil` program command for step 6 (`nrfutil device program --firmware <hex>
  --options chip_erase_mode=ERASE_NONE` or `west flash` without erase) — whichever programs an
  erased nRF54L05 without erasing the KMU.
- Whether `protection-set All` also covers Secure AP-Protect on this SoC, or needs
  `SecureRegions` too (verify on the key-test DK).
- Log wording.

## 7. Verification

- Host: access tests pass through `KeyRef` with the host KMU table; the self-check refuses
  missing, equal and zero keys; slot constants match between firmware and tool (a test reads
  both).
- Key-test DK (1057755043), before any fleet device: run the full sequence; confirm it boots,
  the debug port is locked (`nrfutil device protection-get`), RTT is unavailable, and
  `nrfutil device recover` unlocks it (keys gone: the self-check then refuses).
- First fleet device: sync and one command from the app; then the rest of the fleet.
