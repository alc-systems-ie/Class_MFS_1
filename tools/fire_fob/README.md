# Fire fob (Thingy:53) — BENCH / DEMO ONLY

One button press sends **FIRE** to each listed MFS in turn, **10 s apart**, as engineer
**slot 7**. It is **fire and forget** (protocol 0x05): the MFS sends no receipt, so the fob does
not listen, and a green flash means *sent*, not *received*.

| Thingy:53 LED | Meaning |
|---|---|
| One long green at boot | Ready |
| Blue | A FIRE run is in progress (presses ignored) |
| One green flash | One unit's FIRE has been sent (advertised for 30 s) |
| One long red | One unit's FIRE could not be sent |
| Triple red | Refused: keys expired (04:00 UTC), second boot on the same keys, or self-test failed — see RTT |

**An MFS acts on a FIRE about 100 ms after hearing it — there is no 10 s countdown.** A
disarmed MFS rehearses (red LED). **An ARMED MFS FIRES ITS OUTPUTS at once and nothing can
stop it.** Run the demo against DISARMED units, unless a live fire is intended, or with
nothing on the fire outputs.

## Prepare (every demo day)

Wire the Thingy:53's 10-pin debug connector to a J-Link (never the hub DK, 1057733814),
power switch SW1 on, then:

```sh
FOB_DEV_ID=<J-Link> tools/fire_fob/flash_fob.sh MFS1-1126-00004 MFS1-1126-00006 MFS1-1126-00009
```

It derives that day's slot-7 keys from the gitignored `secrets/<serial>.conf` files into
the gitignored `secrets/fire_fob_keys.h` (encryption keys and rotating IDs
only — no device secret leaves the Mac), builds, and flashes.

## Rules it keeps (docs/tan-scheme.md)

- **Keys last one UTC key day** — they expire at **04:00 UTC**; re-run the script that day.
- **Every FIRE carries the UTC minute** and the MFS rejects one more than 10 min from its
  clock. The fob's clock is *key-generation time + uptime*, so generate, build and flash
  together, and **do not power-cycle the fob**: after any second boot on the same keys it
  refuses (triple red) until re-run.
- **Slot 7 must not be used by any phone** for these units today (a shared slot reuses
  nonces). Each sequence number is saved to flash before its FIRE is advertised, so a
  reset never reuses one; 100 presses per unit per day.
- The MFS clock must be synced (as for any command), and the order of firing is kept by
  the 10 s stagger (an MFS hears a command 0–7.3 s after it starts).
- Compile check without real keys: `python3 tools/fire_fob/gen_fob_keys.py --fake --out <dir>`
  writes an all-zero header there (never flash it); build with `-DFOB_KEYS_DIR=<dir>`.
- Radio: nRF21540 at POUTB, **+10 dBm** (GPIO mode), within the Irish EIRP allocation.
