# MFS event reports to the Network Manager — design

**Owner:** Andy (brainstormed 2026-11-24). **Status:** approved by the owner 2026-11-24.
**Repos:** `class_mfs_1` (this design's home), `class_hub`, a new Network Manager
stand-in, `class_app`, and `tools/fire_fob` in this repo.

Calls marked **(builder's call)** were made during the write-up, not by the owner;
check them while reading.

## 1. Intent

Production MFS devices tell the back office what happened to them: armed, disarmed,
fired (and why), and battery low. The information goes MFS → hub → Network Manager
(NM) → MQTT, and later into **ATAK** for display. A firing report goes out
**immediately before the fire lines assert**, because the firing usually destroys the
device: it is the last thing the device ever says.

This replaces the FIRE command's receipt confirmation (confirmId events 0x01/0x02 to
the phone and fob) and its 10 s countdown. A single notification path now covers every
firing, whatever caused it.

## 2. Goals, non-goals, anti-goals

**Goals**
- A minimal, encrypted, random-looking **event report**: battery voltage (x.y V) and
  one event — *low battery, activated, deactivated, fired from command, fired from
  motion, fired from sound level*.
- Fire reports go out **before** the fire lines assert, for every firing cause.
- **FIRE fires at once**: no 10 s countdown, no receipt confirmation.
- The hub **relays reports unread**. It holds no keys.
- The NM (a stand-in for now) decrypts and publishes JSON on **`CLASS/<hub serial>`**
  with MFS serial, date, time, and event.
- The app gets a **second settings page** with four switches: low battery, activate,
  deactivate, and fire reports.

**Non-goals**
- The ATAK feed (later; the JSON is shaped so it maps easily — §7.3).
- The production NM (HSM, login, attestation, slot assignment — `docs/tan-scheme.md` §5).
  The stand-in here is replaced by it later without touching the MFS or hub.
- Acknowledgement or retry of reports. They are one-way broadcasts; **speed matters
  more than reliability** (owner).
- MFS_3 and its MEMS microphone. MFS_1 has no microphone (the OPT3001 is light), so
  it never sends *fired from sound level*. The code is reserved now so MFS_3 needs no
  protocol change.

**Anti-goals — failure even if it works**
- **A hub that can read or forge reports.** It relays opaque bytes only.
- **A fixed identifier on air.** Anyone with a scanner could follow a device around;
  covertness is the rest of the protocol's whole point.
- **A report that delays or prevents a fire beyond §5.3's bound.** If a report cannot
  be built, the device fires anyway and logs it.
- **CCM nonce reuse** under the report key — including across a reflash (§4.3).
- **A report that becomes a second reader of the arm state.** The fire report is
  decided where the output is decided (§5.3); the invariants in `CLAUDE.md` stand.

## 3. Approaches considered (owner chose A)

| | Approach | Trade-off |
|---|---|---|
| **A** | **Fixed report key per device**, derived one-way from the device secret; a never-reused report counter drives the rotating ID and nonce | Works whatever the device clock's state — a low-battery report after a battery change goes out before any sync. Costs one persisted counter on the device and one tracked counter at the NM |
| B | Reuse the slot-0 day key | Almost no new key code, but no report until the clock is synced, and the NM must try two days around 04:00 UTC |
| C | Device ID in clear, body encrypted | Simplest NM lookup, but a fixed identifier on air — rejected (anti-goal) |

Also decided with the owner: the hub relays unread and the NM decrypts (rather than a
hub holding read-only keys); a separate NM stand-in service rather than decoding in the
hub bridge.

## 4. The report on air (protocol)

### 4.1 Derivation

Same primitives and conventions as commands (`docs/tan-scheme.md` §3: HMAC-SHA256,
big-endian inputs, AES-128-CCM with a 4-byte tag). New labels **0x08–0x0A**
(0x00/0x01 retired, 0x02–0x05 in use, 0x06 the old confirmId, 0x07 the boot key check):

```
reportKey(id, g)  = HMAC( deviceSecret, id(4) ‖ g(2) ‖ 0x08 )
reportEncKey      = HMAC( reportKey, 0x09 )[0..15]
reportId(r)       = HMAC( reportKey, r(4) ‖ 0x0A )[0..3]
nonce(r)          = id(4) ‖ g(2) ‖ 0xFF(1) ‖ r(4)                    (11 bytes)
aad(r)            = reportId(r) ‖ reportVersion(1)                     (5 bytes)
report(r)         = reportId(r) ‖ AES-128-CCM(reportEncKey, nonce, aad, plaintext(8), tag 4)
```

- `g` is the **report generation** (§4.3). `r` is the 32-bit **report counter**.
- `0xFF` sits where a command nonce carries its slot (0–7), so a report nonce can never
  equal a command nonce even under a shared key. **(builder's call)**
- `reportVersion` = **0x01**, never on air, as `protocolVersion` is for commands.
- On the device, `reportKey` is an HMAC under the KMU-held `deviceSecret` (as day keys
  are); `reportKey` and `reportEncKey` live in RAM only, like day keys.

On air it is **one 128-bit service UUID, 16 bytes**, the same AD type and byte order as a
command (`CommandAdvertiser`), non-connectable, non-resolvable private address, no name.

### 4.2 Plaintext (8 bytes)

| Byte | Field | Values |
|---|---|---|
| 0 | event | `0x01` low battery · `0x02` activated · `0x03` deactivated · `0x10` fired from command · `0x11` fired from motion · `0x12` fired from sound level (MFS_3). Anything else: NM logs and drops |
| 1 | battery | V<sub>BAT</sub> in **tenths of a volt** (`29` = 2.9 V); `0xFF` = not measured |
| 2–7 | reserved | zero; the NM ignores them |

Minimal by design (owner). Spare bytes leave room for a later field (e.g. a deactivation
reason) without a format change.

### 4.3 The counter, the generation, and nonce safety

**Report counter `r`.** Never reused under one `(reportKey)`. The device persists a
**reservation floor** in NVS (new record `report/v1`) in blocks: at boot it reserves
`[floor, floor + 16)` by saving `floor + 16` first, then uses those values from RAM,
and reserves the next block when four remain — outside the fire path. A reset skips at
most one block's unused values, which is harmless. **(builder's call:** block size 16,
refill at 4.)

**Report generation `g`.** A reflash erases NVS, so `r` restarts at 0 — under a fixed
key that would reuse nonces. The fix: every reflash **bumps `g`**, which changes
`reportKey`. `tools/flash_device.sh` reads `g` from `secrets/<serial>.conf`
(`CONFIG_MFS_REPORT_GENERATION`, starting at 1), increments it, writes it back
**before** building, and passes it in the derived conf with the serial and device ID.
`g` is not secret. The NM reads the same file, so it always knows each device's current
`g`. **(builder's call** — the alternative, keeping `r` somewhere a recover cannot
erase, does not exist on this chip.)

- **`secrets/<serial>.conf` is the master copy of `g`.** Flashing, the fleet credentials
  and the NM stand-in all live on the same Mac mini, so there is one file and no copy to
  drift. The credentials spreadsheet does not carry `g`.
- **When the script bumps it:** on every run that goes on to the nrfutil batch, before the
  build, so the image and the file always agree. `--build` and `--dry-run` do not bump it.
  A failed batch has still bumped it — the chip may hold the old image or none — so
  **the NM accepts reports under `g` and `g − 1`** and drops `g − 1` once it has heard
  one under `g`. **(builder's call)**

**At the NM.** Per device: the current `g` and a floor; it accepts `r` in
`[floor, floor + 1024)` and sets `floor = r + 1`. A new `g` starts at floor 0.
Duplicates from a burst, and replays, fall below the floor and are dropped silently.
The floor persists in the NM stand-in's state file.

- **Why 1024, not the commands' 64:** unlike day keys there is no daily reset to bring
  the two sides back into step, and the device uses up counter values the NM never hears
  — up to 15 per reset (block reservation) and one per unheard report. A 1024 window
  covers ~60 resets or ~1000 unheard reports. The cost is a precomputed table of 1024
  four-byte IDs per device. A false ID match (~1024 / 2³² per device per UUID) is caught
  by the CCM tag. **(builder's call)**
- **If a device ever drifts past the window**, its reports are lost until the next reflash
  (new `g`). The NM logs a daily warning for any device with no report for 7 days, so the
  loss is visible. **(builder's call)**

## 5. MFS firmware (`class_mfs_1`)

### 5.1 When a report is sent

| Event | Sent when | Not sent when |
|---|---|---|
| **activated** | the arming sequence reaches **Active** | an Arm is merely accepted (Active follows 10 s later); a failed arming |
| **deactivated** | the device leaves **Active** for Inactive — a Disarm command, or a fail-safe disarm (switch fault, scanner loss) | after a fire (the fire report implies it); a cancelled or failed arming (it never reported activated) |
| **fired from command** | an armed device fires on FIRE | a disarmed FIRE rehearsal |
| **fired from motion** | an armed device's detection engine fires the output (undelayed or after its delay) | disarmed simulated firing (the red LED test mode) |
| **fired from sound level** | never on MFS_1 | — |
| **low battery** | V<sub>BAT</sub> is below **2.7 V** while the device is **Active** — including a low reading taken while disarmed, sent when the device next becomes Active (§5.4) | while disarmed or Arming; a failed arming battery test (the device stays disarmed); already reported since the battery last recovered |

**A disarmed device sends nothing** (owner, 2026-11-24) — Inactive or Arming, no report
of any kind. The one report about becoming disarmed, *deactivated*, is sent as the device
leaves Active. The rest of the table is **(builder's call)** within that rule: the
deactivated-only-from-Active rule matches the trial status report
(`2026-11-16-status-report-design.md`).

Reports **do not need a valid clock**, so they work before a sync. Each is gated by its
switch (§5.5); a switched-off event is simply not sent.

### 5.2 FIRE without the countdown

- **Armed + FIRE → fire now**: raise the fire supply, send the fire report, assert (§5.3),
  hold for `M_DELAYED_TRIGGER_HOLD_MS`, then latch Inactive as any one-shot does.
- **Disarmed + FIRE → rehearsal now**: red LED for the same hold, **no report**; the fire pins and supply are untouched.
- **Arming + FIRE** stays ignored (`DecideCommand()`).
- **Removed:** the 10 s countdown (`FireSequence::M_FIRE_COUNTDOWN_MS`), the
  "unstoppable by Disarm" carve-out and the FIRE scanner-loss exemption — with no
  countdown there is nothing left for them to protect — and the confirmId receipt
  burst (`M_CONFIRM_EVENT_FIRE_*`, `access::ConfirmId()`, label 0x06 retired). The
  fire-command amendment (`2026-09-15-fire-command-amendment.md`) §3–§4 is superseded
  by this section; the FIRE payload itself is unchanged.
- **Always-fail-safe returns in full**: there is no longer any exception to it.

### 5.3 Firing order and timing — speed first

1. The fire decision is made **in `App::updateOutputState()`**, as today — the single
   place the output is derived. The fire report is emitted from there, on the output's
   rising edge, never from a second reader.
2. Build the report from the **cached battery reading** (§5.4) — no ADC read in the fire
   path.
3. Start a **100 ms burst at the fastest legal interval (20 ms)** — about five adverts.
   Non-fire reports use the same burst. **(builder's call:** 20 ms for every report;
   events are rare, so the cost is negligible.)
4. Assert the fire lines **100 ms after the burst starts**. Where the fire supply still
   has to be raised (FIRE, undelayed motion), the burst **overlaps the existing 100 ms
   supply settle**, so they add nothing. An armed delayed trigger, whose supply is already
   up, fires **up to 100 ms later** than today.
   - **Not by blocking.** The wait is a short *pending-fire* stage inside
     `updateOutputState()`: the tick that decides to fire starts the burst; the next tick
     (the loop runs every 100 ms) asserts. `m_output_switch.Set()` is still called only
     there, so the single-derivation invariant holds. A **Disarm or any fault in that
     window cancels the fire** — always-fail-safe, with no exception (§5.2). The supply
     settle keeps its existing blocking 100 ms, as today. **(builder's call)**
5. If the report cannot be built (crypto fault, no reserved counter), **fire anyway**
   with no extra wait, and log `Fire report not sent (%d) - firing anyway!`.

Bound: **no fire is ever more than 100 ms later than today**, and FIRE goes from 10 s to
about 0.1 s.

### 5.4 Battery

- Read V<sub>BAT</sub> (`Npm2100::AdcReadVbat`) at boot, after every arming battery test,
  and **every 6 h** while running; cache the last value for reports.
  **(builder's call:** 6 h; negligible against the ~103 µA budget.)
- **Low battery** when the reading is below **2.7 V**. Sent once, and only while Active
  (a low reading taken while disarmed waits until the device is next Active); re-armed
  when a reading is ≥ 2.8 V (100 mV hysteresis) or at boot. A failed arming battery
  test sends nothing — the device stays disarmed, and the engineer standing at it sees
  the orange slow flash. **(builder's call:** 2.7 V is early
  warning for a CR123A, whose voltage stays near 3.0 V for most of its life and then
  falls; the owner may want a different figure.)
- Every report carries the cached voltage, rounded to tenths.

### 5.5 The four switches (Settings command)

- **Settings byte 6, bits 0–3** — today the "per-variant extension, MFS_1 must ignore"
  (`2026-09-14-command-types-amendment.md` §2.1): bit 0 low battery, bit 1 activated,
  bit 2 deactivated, bit 3 fired. A set bit **suppresses** that report, so **zero means
  all on** — the default, and what any command that leaves the byte zero means.
  Bits 4–7 and byte 7 stay reserved and ignored. **(builder's call:** suppress-sense,
  so a missing field can never silently switch reports off.)
- Applied only by a Settings command while Inactive, and persisted with the other
  parameters (`params/v1` grows a field, versioned as the settings store does today),
  like every other setting. The app never stores them (owner rule 2026-09-14).
- **`protocolVersion` → 0x05.** FIRE's meaning changed and Settings gained a field, so an
  old app's or fob's commands fail authentication instead of acting on old assumptions.
  Vectors regenerate (`tools/gen_access_vectors.py`), as on every version change.

### 5.6 Host tests

Report derivation and encryption against new Python known-answer vectors (also used by
the NM); counter reservation (block, refill, reset skip); the report policy table in
§5.1 as pure logic; low-battery threshold and hysteresis; FIRE immediate in
`FireSequence`/`DecideCommand()`; Settings byte 6 encode/decode. `make test` and
`sh tools/test_flash_device.sh` (the generation bump) must pass.

## 6. Hub (`class_hub`)

- The DK listener additionally forwards **every non-connectable advert whose data is
  exactly one AD structure: a Complete List of 128-bit Service UUIDs holding one UUID**
  (the MFS report shape — `CommandAdvertiser` sends nothing else), as a new line, e.g.
  `MFSUUID <32 hex> rssi <n> rec <r> age <ms|-1>`. It cannot tell a report from a
  command or a stranger's advert of the same shape, and does not try: the NM sorts them.
  The tight shape filter keeps strangers' traffic rare; such adverts are public
  broadcasts, so storing and relaying them to the local broker is acceptable.
  **(builder's call)**
- **Identical UUIDs are de-duplicated** at the hub (a burst sends ~5 copies; a phone
  repeats a command for 30 s) — e.g. drop a UUID seen in the last 60 s.
  **(builder's call)**
- **Reports must survive a bridge outage**: a **new, single, anonymous store-and-forward
  queue** beside the per-device report slots (which are keyed by device ID and cannot
  hold these), oldest first, ACKed like the existing records, dropping the oldest when
  full. The builder sizes it. **(builder's call)**
- The bridge publishes each one to the **local** broker, e.g.
  `alc/hub/<hub serial>/uuid` with `{"uuid", "rssi", "utc", "utc_known"}`. `utc` is
  when the hub heard it: the bridge's NTP-disciplined clock minus the record's `age`.
  A record that lost its age across a hub reset (`age -1`) carries the forwarding time
  with `utc_known: false`, and the NM passes that on (§7.2). **(builder's call)**
- The daily and status trial reports (00007) are untouched.

## 7. Network Manager stand-in

### 7.1 What it is

A small Python service, separate from the hub bridge, standing in for the NM until the
real one exists. **(builder's call:** a new sibling repo `class_nm`, running on the Mac
mini beside the bridge under launchd, like the bridge.)

- **Keys:** reads `deviceSecret`, device ID, serial and `g` for each device from the
  fleet credentials (`secrets/<serial>.conf`). Those files already live on the Mac mini,
  where devices are flashed; the stand-in adds a long-running reader of them, not a new
  copy. It is the only place outside the device that can read reports; treat the host as
  a secrets host, and never log a key or a derived key.
- **Input:** subscribes to `alc/hub/+/uuid` on the local broker.
- **Matching:** for each UUID, look up `reportId` in a precomputed table of each
  device's window `[floor, floor + 64)` under its current `g`; on a hit, decrypt and
  verify. A miss (a phone command, noise, a duplicate) is dropped silently.
- **State:** per-device floors in a local JSON file, saved before publishing.

### 7.2 Output — `CLASS/<hub serial>`

On the CLASS broker (the existing TLS `[class]` configuration in `class_hub`'s
`broker.toml`), QoS 1, not retained:

```json
{"serial": "MFS1-1126-00004", "date": "2026-11-24", "time": "14:05:31",
 "event": "fired from motion", "battery_v": 2.9,
 "hub": "HUBA-1126-00001", "rssi": -78}
```

- `event` is exactly one of: `low battery`, `activated`, `deactivated`,
  `fired from command`, `fired from motion`, `fired from sound level`.
- `date` and `time` are **UTC**, from the bridge's reception time — not the device clock,
  which may be unsynced. **(builder's call:** UTC, per the project rule that time is UTC
  everywhere; ATAK also works in UTC.)
- `battery_v` is null when the device sent `0xFF`.
- `hub`, `rssi` and `time_known` (false when the hub lost the hearing time
  across a reset, §6) are additions beyond the owner's list. **(builder's call)**

### 7.3 Towards ATAK

Not built now. The message carries what a Cursor-on-Target event needs, apart from a
position, which belongs to the device's installation record at the NM.

## 8. App (`class_app`) and fob

- **Protocol 0x05** and regenerated vectors.
- **Second settings page** after the existing Settings page, reached the same way
  (only through a confirmed disarm), with four switches — Low battery, Activate,
  Deactivate, Fire — **all on by default**, and covered by Restore defaults. Sent in the
  same Settings command as the rest (§5.5). Not stored by the app.
- **FIRE:** no countdown screen and no confirm scan — remove `confirm_listener.dart`
  and its UI; the page reports that the FIRE was sent.
- **`tools/fire_fob`:** fire-and-forget — drop the confirm scan and its LEDs; protocol
  0x05.

## 9. Documents to update

`CLAUDE.md` (the "never advertises" rule gains production reports as a documented,
switchable exception; the FIRE countdown and exception paragraphs go; new labels),
`docs/tan-scheme.md` (§3 derivations, label list), `docs/v1-scope.md`, the fire-command
amendment (superseded note), the command-types amendment §2.1 (byte 6),
`docs/power-budget.md` (reports and the 6 h battery read — negligible), and the bench
checklist (a new section: each event end to end MFS → hub → NM → CLASS broker, a
reflash and generation bump, a bridge outage).

## 10. Order of work

1. Protocol: derivations, plaintext, vectors (Python), shared by everything below.
2. MFS firmware (§5), with host tests.
3. `class_hub` raw UUID forwarding (§6).
4. NM stand-in (§7).
5. `class_app` and the fob (§8).

Each step is usable on its own bench: after 2, reports are visible with
`tools/uuid_observer`; after 4, end to end.

## 11. Rolling out to the units in the field

Protocol 0x05 stops the current app and fob working with any unit not reflashed, and
every reflash erases NVS and bumps `g`. **(builder's call:)**

- **Bench first:** one unit (00001) end to end before any other.
- **00004, 00006** and the fob: after the bench pass, reflashed together with the new app,
  so the demo set never mixes versions.
- **00007 stays out** until its daily-report trial ends (it stays on its trial tag, as
  `CLAUDE.md` already says).
- **00009 stays out** while it is the only unit holding the motion-detection-loss state
  (`docs/investigations/2026-11-19-motion-detection-loss.md`): reflashing it erases the
  evidence.
