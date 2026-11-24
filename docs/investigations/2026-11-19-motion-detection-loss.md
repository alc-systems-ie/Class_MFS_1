# Motion detection lost on 00004, 00006, 00009 - investigation (open)

2026-11-19, bench. Status: **cause not found; OPT3001 exonerated; 00004 soaking.**

## Symptom

MFS1-1126-00004, 00006 and 00009 stopped responding to movement: no red simulated-firing
LED while disarmed, no fire output while armed. Everything else worked - time sync, Disarm,
Arm (normal red flash, no warning), Settings and FIRE were all accepted. The fault survived
power cycles (00009 re-checked after a battery pull on 11-19: still no detection).

All three carry an OPT3001; the owner's first suspicion was its soldering.

## Shared history

- Flashed locked, back to back, 2026-11-16 17:30-17:33 UTC, from 284ebfc (FEM GPIO-only fix),
  clean tree, no extra conf. 00001 got the identical image at 14:23 and still detects.
- Targets of the Thingy:53 fire-fob demo (`tools/fire_fob`, slot 7) on 11-16. The fob only
  recognises the LIVE confirmation (event 0x01), and the demo gave green triples, so the
  three were **armed and received live FIREs**. Two had e-matches fitted; 00009's came loose
  and did not fire, yet 00009 fails too.
- About three days of running and power cycles afterwards.

## What was tested (11-19)

| Test | Result |
|------|--------|
| 00006 reflashed `--unlocked`, RTT | ADXL367 healthy (probe, config read-back, ~1 g, INT1 idle high); detects disarmed and armed; fires |
| 00006: phone FIRE rehearsal (slot 3), then arm + move | Rehearsal normal (10 s, red 5 s); fires on movement |
| 00004 (failing, locked): sync, Disarm, move | Orange triple, no red |
| 00004: Settings at defaults, move | Orange triple, no red - **stored settings ruled out** |
| 00004: Arm, move | Normal arming (no warning), no fire on movement |
| Sensor supply measured | 3.3 V on OPT3001 and ADXL - they are on **VOUT, not LSOUT** (app.cpp comment is wrong); a reboot never power-cycles them |
| 00004 reflashed `--unlocked` | Detects at once |
| 00004 power-cycled (unlocked) | Detects - debug-domain state ruled out |
| 00004 reflashed **locked** (same image as 11-16), power-cycled | Detects - **debug lock ruled out** |
| ADXL367 joints | Owner: firm |
| 00009 battery pull, sync, Disarm, move | Still no red - fault survives a cold boot |
| 00004 unlocked: fob FIRE rehearsal (slot 7) | Normal; still detects |
| 00004 armed: fob **live** FIRE (confirmed in 1.4 s), Disarm, move; then power cycle | Still detects |

**Exonerated:** the OPT3001 and the board hardware (the same boards detect after a reflash),
stored settings, the debug lock, RAM/debug state, a single rehearsal or live FIRE.

**Not yet reproduced or explained:** what the reflash clears that a power cycle does not. The
only stored records are `params/v1` (settings - ruled out) and `access/v1` (day floor and eight
slot counters), and no code path from either to detection was found. Untested: days of uptime
after a live fire, repeated presses, a fire into a real e-match load.

## Update 11-24

- **00004 still detects** (owner, 2026-11-24), about 4.9 days after its 11-19 12:14 UTC reflash
  and well past the 36.4 h tick-wrap mark (twice over). Uptime alone did not reproduce the fault
  on this image, which weakens the tick-wrap hypothesis. It does not rule out uptime combined
  with the 11-16 history (live FIREs, then days running).

## State at close (11-19)

- **00004**: unlocked + `CONFIG_LOG_BACKEND_RTT_MODE_OVERWRITE=y`, soaking. When it stops
  detecting: do NOT power-cycle; put it on the pogo pins, read the RTT buffer without reset,
  dump the storage partition (0x74000), and read GPIO P1 (0x500D8200) IN / PIN_CNF[3] (INT1,
  P1.03) while moving it. Bench only - reflash locked before it leaves the bench.
- **00006**: reflashed locked at close (`tools/flash_device.sh`, protection-get All), same image as 11-16.
- **00009**: untouched since 11-16, still failing - the only unit holding the original state.
  Locked, so reading it would erase it.
- **00001**, others: owner monitoring.

## Side findings

- `tools/fire_fob` derives only the live confirmation (0x01): a rehearsal always ends in blue
  for 45 s then a long red, although the MFS confirmed it. Should also accept 0x02.
- `protocol::CommandTypeName()` logs FIRE as "Reserved" (cosmetic).
- Reflashing mid-day resets the device's slot counters; the phone's slot-3 counter then sits
  beyond the 16-number window (rapid orange) until 04:00 UTC. Use another
  `BENCH_ENGINEER_SLOT` or wait.
- Fresh `class_app` builds were SIGKILLed (exit -9) with 72 % memory free - Bitdefender
  suspected.
