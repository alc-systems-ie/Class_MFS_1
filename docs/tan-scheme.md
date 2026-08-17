# MFS_1 TAN Scheme

Project CLASS — Multi-Function Sensor 1. Recorded 2026-08-17.

How a service engineer obtains and uses one-time codes, and how those codes are
bound to a single day and a single device. Power and timekeeping constraints that
this scheme depends on are in `docs/power-budget.md` §8.

## 1. Requirement being met

An engineer is issued a **paper TAN sheet for one device, for one day** — 10
codes. If the sheet is lost, only that device and only that day are compromised.
The codes are worthless the following day whether or not they were used.

## 2. The central idea

**The day is not a label attached to a TAN. It is an input to the number itself.**

Every TAN is the output of a keyed hash over (device seed, day, slot). A code
valid on day 228 is arithmetically unrelated to any code valid on day 229. The
device never asks "which day is this TAN for?" — it asks "is this one of my ten
numbers for today?"

Three properties follow at no cost:

- **Nothing about the date is transmitted, so nothing about the date can be
  forged.** There is no date field to tamper with. Compare a scheme that sends
  `(date, TAN)` and validates the TAN against the sent date — a holder of a lost
  day-228 sheet simply claims to be on day 228, and expiry is defeated.
- **Codes are device-specific**, because the seed is per-device. A sheet for one
  MFS_1 is useless on any other.
- **Nothing is stored.** The device holds a 32-byte seed and recomputes the day's
  ten codes on demand. There is no TAN table to size, exhaust or read out.

## 3. Derivation

```
TAN(day, slot)        = truncate( HMAC-SHA256( seed, day ‖ slot ‖ 0x00 ) )
sessionKey(day, slot) =            HMAC-SHA256( seed, day ‖ slot ‖ 0x01 )
```

- `seed` — 256-bit, unique per device, provisioned at manufacture (§7).
- `day` — `uint16` day index, big-endian (§4).
- `slot` — `uint8`, 0–9, the position on the sheet.
- The trailing label byte gives **domain separation** so the transmitted code and
  the session key are independent outputs of the same key.

Truncation to six decimal digits, HOTP-style:

```
value = be32(mac[0..3]) & 0x7FFFFFFF
tan   = value % 1000000
```

Only the six digits ever go over the air. The other 27 bytes of the HMAC never
leave either party, which is what makes `sessionKey` usable as a shared secret
(§6.5).

Constants, in house style:

```cpp
namespace alc::tan
{
  constexpr uint8_t  M_TANS_PER_DAY            { 10 };
  constexpr uint32_t M_TAN_MODULUS             { 1000000 };    // Six decimal digits.
  constexpr uint32_t M_DAY_SECONDS             { 86400 };
  constexpr uint32_t M_DAY_BOUNDARY_OFFSET_SECS{ 4 * 3600 };   // 04:00 UTC — see section 4.
  constexpr uint32_t M_TAN_EPOCH_UNIX          { 1767225600 }; // 2026-01-01T00:00:00Z.
  constexpr uint8_t  M_LABEL_TAN               { 0x00 };
  constexpr uint8_t  M_LABEL_SESSION_KEY       { 0x01 };
}
```

### 3.1 Slot collisions are harmless

Ten values drawn from 10<sup>6</sup> carry a ~0.0045% chance that two slots on the
same day yield the same six digits. The effect is only that one code matches two
slots; consume the lowest-numbered match. No special handling is needed.

## 4. The day index — UTC, boundary at 04:00

```cpp
uint16_t dayIndex(uint32_t unixSeconds)
{
  return static_cast<uint16_t>((unixSeconds - M_TAN_EPOCH_UNIX - M_DAY_BOUNDARY_OFFSET_SECS) / M_DAY_SECONDS);
}
```

`uint16` covers 179 years from the epoch. Guard against times before
`M_TAN_EPOCH_UNIX + M_DAY_BOUNDARY_OFFSET_SECS`, which would underflow.

**Everything is UTC on the device. Never local time.** This is the trap worth
stating explicitly: Ireland runs GMT in winter and IST in summer, so a device
computing "04:00 local" and a back office computing the same would disagree by an
hour twice a year, and every sheet issued across a DST transition would be wrong.
The device has no timezone database and must not acquire one.

04:00 UTC lands in the small hours across both target markets — 04:00/05:00 in
Ireland, 05:00/06:00 in Germany. Clock error at the day boundary therefore falls
where it can neither reject a working engineer nor extend a lost sheet into a
working day. This is the whole reason the boundary is not midnight; see
`docs/power-budget.md` §8.5.

## 5. How the engineer obtains a sheet

1. Engineer (or scheduler) requests a sheet from the ALC back office tool,
   supplying **device serial** and **date**.
2. The tool looks the device's seed up by serial in the provisioning database.
3. It computes `TAN(day, 0..9)` for that date's day index.
4. It renders a sheet and logs the issue — who, which device, which date, when.

Illustrative sheet layout (**codes below are made up, not computed**):

```
+---------------------------------------------+
|  PROJECT CLASS - MFS_1 SERVICE TAN SHEET    |
|                                             |
|  Device serial : MFS-0826-0043              |
|  Valid date    : Mon 17 Aug 2026            |
|  Day index     : 228                        |
|  Valid from    : 04:00 UTC  17 Aug 2026     |
|  Valid until   : 04:00 UTC  18 Aug 2026     |
|                                             |
|    1   418 902        6   115 774           |
|    2   770 331        7   629 048           |
|    3   052 916        8   340 187           |
|    4   963 425        9   886 512           |
|    5   287 640       10   471 209           |
|                                             |
|  Single use each. Destroy after 04:00 UTC   |
|  18 Aug 2026 - the codes are then dead.     |
+---------------------------------------------+
```

The serial prefix `MFS-` is provisional — a device-type prefix must be assigned
alongside the `0x??` device type, in line with `DMA-` for Drawer Master.

Printing the day index and both UTC bounds is deliberate: when a code is rejected,
support needs to distinguish "wrong sheet" from "device clock is out" without
guessing.

### 5.1 One sheet per device — the operational cost is real

Codes are device-specific by design, so an engineer visiting twenty devices needs
twenty sheets. The alternative — one fleet-wide sheet per day — means a single
lost sheet compromises **every** device for that day, which discards the main
property being bought. **Keep sheets per-device.**

Mitigation is a tooling one: the back office should print a **route booklet**, one
page per scheduled device for the visit date, rather than making the engineer
request sheets one at a time.

### 5.2 The engineer must know which device they are at

The device is covert and **never advertises** (§6.1), so it cannot announce its
serial — a covert sensor that identifies itself over the air is not covert. The
engineer therefore identifies the device from the work order or a physical label,
not from a scan. This is a deliberate consequence of the architecture, and it
means sheet-to-device pairing is an operational discipline, not something the
radio can resolve.

## 6. The exchange

> **Settled.** MFS_1 is a *scanner*, not an advertiser
> (`docs/power-budget.md` §3, §6), so the engineer's tool advertises and the device
> listens. The tool platform is **Android**
> (`docs/power-budget.md` §8.7.4), which can carry arbitrary manufacturer-specific
> advertising data. The device **never advertises at any point in its life**.
>
> The TAN advertising payload is nonetheless held to **12 bytes** — version (1) +
> 6-digit TAN (3) = 4 bytes, well inside it — so that an iOS tool remains possible
> later without a protocol change. See `docs/power-budget.md` §8.7.4 for why 12.

### 6.1 Flow

1. **Device listens.** 100 ms passive scan every 6 s, as budgeted. Passive: no
   scan requests, so the device emits nothing.
2. **Tool advertises** at a 20–50 ms interval, carrying **one** candidate TAN in a
   manufacturer-specific AD field. At 20 ms the device catches it within one
   scan window with near-certainty (`docs/power-budget.md` §6), so worst-case
   latency is one 6 s cycle.
3. **Device validates** against today's unconsumed slots (§6.2). Ten HMAC-SHA256
   operations at worst, hardware-accelerated on CRACEN.
4. **On success**, the device marks the slot consumed, persists 4 bytes, derives
   `sessionKey`, and **connects outward as central** to the tool's advertised
   connectable peripheral.
5. **Session** proceeds keyed on `sessionKey` (§6.5).
6. **On failure**, the device does nothing observable — no response, no
   advertisement, no error. It increments a failure counter (§6.6).

Step 4 is worth dwelling on: because the device initiates, **it never advertises
at any point in its life.** It is invisible on the air until a correct code for the
current day is presented. Silence on failure means an attacker probing with wrong
codes gets no confirmation that a device is even present.

### 6.2 Validation

Persistent state, the whole of it:

```cpp
struct __packed TanState
{
  uint16_t dayIndex;      // Day the consumed mask applies to.
  uint16_t consumedMask;  // Bit n set = slot n consumed; 10 bits used.
};                        // 4 bytes.
```

```
1. Guard: is the clock valid at all? If not -> recovery path, power-budget.md §8.7.
2. today = dayIndex(now)
3. if (today < state.dayIndex) today = state.dayIndex;   // Monotonic: never rewind.
4. if (today != state.dayIndex) { state.dayIndex = today; state.consumedMask = 0; persist(); }
5. for slot in 0..9:
     if (!(state.consumedMask & BIT(slot)) && candidate == computeTan(today, slot))
       -> match
6. On match:  state.consumedMask |= BIT(slot); persist(); derive sessionKey; connect.
7. On no match: failure counter++, backoff. Emit nothing.
```

Step 3 enforces the monotonic-day rule from `docs/power-budget.md` §8.6: a clock
that has been reset or has drifted backwards can stall the date but never rewind
it below a day the device has already seen. Without it, cutting power becomes a way
to revive an old sheet.

Step 4 is where expiry actually happens, and note what it does *not* do: it keeps
no record of which codes were used on previous days. Past codes are rejected on
date grounds by step 5's use of `today`, so per-day history would be dead weight.
This is why the state is 4 bytes and not a 457-byte bitmap.

### 6.3 Single use — what exactly consumes a code

Each code works **once**, and dies at whichever of two independent limits arrives
first:

| Mechanism | Kills | When |
|---|---|---|
| Consumption | that one code | immediately on successful use |
| Date expiry | all ten, used or not | 04:00 UTC next day |

A sheet is therefore **10 sessions for the day, not 10 attempts at one session**.

**Failed attempts do not consume.** A mis-keyed digit matches no slot, so it costs
a retry and nothing else. Only a match sets a bit.

**Consume at validation, not at session end.** Step 6 of §6.2 sets the bit as soon
as the code matches, before the outward connection is attempted. The consequence
is accepted deliberately: a session that drops — out of range, tool crash, engineer
interrupted — has burned that code, and the engineer moves to the next one. Ten
slots exist to absorb exactly this.

The alternative, committing only once a session completes, is unsafe. The code has
already been broadcast in clear (§6.5), so leaving it unconsumed after a failed
session leaves it replayable by anyone who was listening.

**Persist before acting.** The 4-byte `TanState` write must complete before the
device connects outward. Reversed, a power interruption mid-session leaves the code
unconsumed and therefore replayable.

This ordering also narrows the power-loss exposure in `docs/power-budget.md` §8.6:
because the consumed mask is persisted alongside the day index, consumption
survives a reset. A device resuming on a stale day revives only the codes that were
**never used** from that sheet — 7 of 10 if the engineer got through three, not all
ten.

### 6.4 Any slot, any order

The device tries all unconsumed slots, so the engineer uses whichever code is
next unused on the sheet. No slot number is transmitted. Sending one would save
nine hash operations — worth nothing on CRACEN — while adding a field that can be
wrong.

Ten codes exist so that a day's work can survive retries, multiple sessions and
mis-keyed digits without a return visit.

### 6.5 The TAN authenticates entry; it must also key the session

A TAN alone is a doorbell. The six digits travel in clear in an advertisement, so
anyone in radio range during the visit can read the code being used — harmless in
itself, since it is consumed on first use, but it means the *session* that follows
needs its own protection or it can be hijacked.

`sessionKey(day, slot)` solves this without another secret. Both sides can compute
it, and it never crosses the air: an eavesdropper who captures the six transmitted
digits learns nothing about the remaining 27 bytes of that HMAC output.

Recommended reuse: keep `alc_drawer_master`'s ALC GATT auth v2 challenge/response
machinery and substitute `sessionKey` for the static device secret as the HMAC key.
"Without authentication" then means *without a static shared secret as the entry
gate* — the handshake stays, but its key is one-time and day-bounded, which is
strictly stronger than a fixed per-device secret.

**Operational rule:** the tool advertises **one** TAN at a time and stops as soon
as the device connects. Advertising several, or continuing to broadcast after
success, leaks unconsumed codes for no benefit.

### 6.6 Guess resistance

Ten valid codes in 10<sup>6</sup> gives **1 in 100,000 per attempt**. Three
independent factors compound on top:

- **The scan cadence caps the attempt rate.** An unauthenticated peer cannot drive
  attempts faster than the device listens — one per 6 s at best, so ~14,400 per
  day against a 1-in-100,000 space.
- **A retry counter with exponential backoff.** `alc_drawer_master`'s auth payload
  already carries a `tries` field to model on.
- **Silence on failure** (§6.1) denies the attacker any signal, including whether
  a device is there at all.

## 7. Seed provisioning and the assumption everything rests on

The seed is 256 bits of randomness, unique per device, generated at manufacture
and recorded against the serial in the provisioning database.

**On the device, the seed belongs in the nRF54L05 KMU** (Key Management Unit), not
in a build-time Kconfig string. `alc_drawer_master` carries its secret as
`CONFIG_ALC_DEVICE_SECRET` via `credentials.conf`, which is convenient for bench
work but leaves the key in the image. KMU keeps it out of reach of application code
and of a debugger.

**The whole scheme rests on the seed staying secret.** Two consequences that must
not be deferred:

- **The debug port must be locked in production.** A readable seed means an
  attacker generates every sheet for every day, and TAN expiry becomes decoration.
- **The provisioning database is now high-value.** It holds every device's seed,
  and therefore every sheet ever issuable. It needs the handling of a key store,
  not of a parts list.

### 7.1 The provisioning key is separate from the TAN seed

Two credentials are flashed at manufacture, and they must be **distinct**:

| Credential | Confers | Stored |
|---|---|---|
| TAN seed | Ability to generate any day's sheet — entry to the device | KMU, per device |
| Provisioning key | Ability to set the device clock, and nothing else | KMU, per device |

Separation of duties matters here because a clock-setting capability is an attack
on the TAN scheme: set the date to day N and a lost day-N sheet works again. Keeping
the keys distinct means a compromised provisioning key yields denial of service
(clock pushed forward, current sheets rejected) but not entry, since generating a
sheet for the new date still requires the seed.

The monotonic and bounded-jump rules that make this hold are in
`docs/power-budget.md` §8.7.3. **Provision a key, not a BLE address** — see §8.7.2
for why an address is not an authenticator.

Separately, note the house-style warning on MCUboot signing keys: the NCS default
keys are public, and a device that trusts them will accept anyone's firmware —
which is another route to the seed. Unique production signing keys are a
prerequisite, not a later hardening step.

## 8. Open items

| Item | Detail |
|---|---|
| Link direction | §6 assumes tool advertises / device connects outward. Confirm. |
| Serial prefix and device type | `MFS-` and `0x??` provisional; assign alongside the hub device manager. |
| Battery-change recovery | `docs/power-budget.md` §8.7 — window length, whether a credential is required, whether past-dated sheets are an alternative route in. |
| TAN length | Six digits assumed. Eight digits would give 1 in 10<sup>7</sup> per attempt at the cost of engineer keying effort. |
| Sheet destruction | Whether return-and-destroy is an auditable step or left to the engineer. |
