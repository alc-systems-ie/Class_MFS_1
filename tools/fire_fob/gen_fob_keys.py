#!/usr/bin/env python3
"""Generates the Thingy:53 fire fob's key header for one UTC key day. BENCH / DEMO ONLY.

Reads each device's gitignored secrets/<serial>.conf, derives that device's day key
for the fob's slot (docs/tan-scheme.md), and writes secrets/fire_fob_keys.h with ONLY
what the fob needs: the AES-CCM encryption key, the rotating IDs for
sequence numbers 0..N-1, a known-answer seal per device (the
fob's boot self-test against this reference implementation), the key day and the
UTC time of generation (the fob's clock). The device secrets never leave this Mac.

Everything in the header expires at the next 04:00 UTC day boundary. Generate, build
and flash in one go (tools/fire_fob/README.md): the fob's clock is the generation
time plus its uptime, and the MFS rejects a command more than 10 min from its clock.

    python3 tools/fire_fob/gen_fob_keys.py MFS1-1126-00004 MFS1-1126-00006 MFS1-1126-00009

    python3 tools/fire_fob/gen_fob_keys.py --fake --out <dir>   # compile check only: all-zero keys
"""
import argparse
import hashlib
import hmac
import os
import pathlib
import re
import struct
import time

from cryptography.hazmat.primitives.ciphers.aead import AESCCM

# docs/tan-scheme.md and tools/gen_access_vectors.py - the scheme's constants.
LABEL_DAY_KEY = 0x02
LABEL_ENC_KEY = 0x03
LABEL_ROTATING_ID = 0x04
PROTOCOL_VERSION = 0x05
FAKE_MARKER = "// FAKE FOB KEYS - all-zero, compile check only - never flash"
TAG_BYTES = 4
EPOCH_UNIX = 1767225600           # 2026-01-01 00:00 UTC (src/device_clock.hpp)
DAY_BOUNDARY_SECS = 4 * 3600      # the key day starts at 04:00 UTC
SECONDS_PER_DAY = 86400
# FIRE plaintext with minute 0 (fire-command amendment section 2.2): 'O' type-00 'M' 'G'.
FIRE_PLAINTEXT_MINUTE_0 = bytes([0x4F, 0x00, 0x4D, 0x47, 0x00, 0x00, 0x00, 0x00])

ROOT = pathlib.Path(__file__).resolve().parents[2]
SERIAL_PATTERN = re.compile(r"^MFS1-\d{4}-[0-9A-F]{5}$")


def hmac256(key: bytes, message: bytes) -> bytes:
    return hmac.new(key, message, hashlib.sha256).digest()


def conf_value(text: str, name: str) -> str:
    match = re.search(rf"^{name}=\"?(?:0x)?([0-9A-Fa-f]+)\"?\s*$", text, re.MULTILINE)
    if not match:
        raise SystemExit(f"{name} missing or not hex in the secrets file.")
    return match.group(1)


def c_bytes(data: bytes) -> str:
    return "{ " + ", ".join(f"0x{b:02X}" for b in data) + " }"


def fake_entries(serials: list, count: int) -> list:
    """All-zero keys and IDs: the right shape for a compile check, authenticating nothing."""
    return [(serial, 0, bytes(16), [bytes(4)] * count, bytes(16)) for serial in serials]


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("serials", nargs="*", help="MFS serials, fired in this order")
    parser.add_argument("--slot", type=int, default=7, help="engineer slot 1-7 (default 7); never shared with a phone")
    parser.add_argument("--count", type=int, default=100, help="sequence numbers per device (presses per day)")
    parser.add_argument("--fake", action="store_true", help="write an all-zero-key header for a compile check; reads no credentials")
    parser.add_argument("--out", type=pathlib.Path, help="directory for the header (required with --fake; never the repo's secrets/)")
    args = parser.parse_args()

    if not 1 <= args.slot <= 7:
        raise SystemExit("--slot must be 1-7: slot 0 is the Network Manager's and never issued.")
    if args.fake:
        if args.out is None:
            raise SystemExit("--fake needs --out <dir>.")
        secrets_dir = ROOT / "secrets"
        # samefile folds case and symlinks on a case-insensitive filesystem; resolve() covers a secrets/ that does not exist yet.
        if args.out.resolve() == secrets_dir.resolve() or (
                args.out.exists() and secrets_dir.exists() and os.path.samefile(args.out, secrets_dir)):
            raise SystemExit("--fake refuses to write into the repo's secrets/ directory.")
        existing = args.out / "fire_fob_keys.h"
        if existing.exists() and existing.read_text().split("\n", 1)[0] != FAKE_MARKER:
            raise SystemExit(f"{existing} exists and is not a fake header; --fake will not overwrite it.")
        if not args.serials:
            args.serials = ["MFS1-0000-00000"] * 3
    elif args.out is not None:
        raise SystemExit("--out is only for --fake.")
    elif not args.serials:
        raise SystemExit("Give at least one MFS serial.")
    now = int(time.time())
    if now < EPOCH_UNIX + DAY_BOUNDARY_SECS:
        raise SystemExit("System clock is before the scheme epoch.")
    day = (now - EPOCH_UNIX - DAY_BOUNDARY_SECS) // SECONDS_PER_DAY
    expires = EPOCH_UNIX + DAY_BOUNDARY_SECS + (day + 1) * SECONDS_PER_DAY

    entries = fake_entries(args.serials, args.count) if args.fake else []
    for serial in [] if args.fake else args.serials:
        if not SERIAL_PATTERN.match(serial):
            raise SystemExit(f"{serial} is not an MFS serial (MFS1-MMYY-NNNNN).")
        conf = (ROOT / "secrets" / f"{serial}.conf").read_text()
        device_id = int(conf_value(conf, "CONFIG_MFS_DEVICE_ID"), 16)
        secret = bytes.fromhex(conf_value(conf, "CONFIG_MFS_DEVICE_SECRET"))
        if len(secret) != 32 or secret == bytes(32):
            raise SystemExit(f"{serial}: the device secret must be 32 non-zero bytes.")
        day_key = hmac256(secret, struct.pack(">IHB", device_id, day, args.slot) + bytes([LABEL_DAY_KEY]))
        enc_key = hmac256(day_key, bytes([LABEL_ENC_KEY]))[:16]
        rotating = [hmac256(day_key, struct.pack(">I", n) + bytes([LABEL_ROTATING_ID]))[:4] for n in range(args.count)]
        nonce = struct.pack(">IHBI", device_id, day, args.slot, 0)
        sealed = AESCCM(enc_key, tag_length=TAG_BYTES).encrypt(nonce, FIRE_PLAINTEXT_MINUTE_0,
                                                                rotating[0] + bytes([PROTOCOL_VERSION]))
        entries.append((serial, device_id, enc_key, rotating, rotating[0] + sealed))

    lines = ([FAKE_MARKER] if args.fake else []) + [
        "// GENERATED by tools/fire_fob/gen_fob_keys.py - DO NOT EDIT. SECRET: never commit.",
        f"// Key day {day}, slot {args.slot}; expires {time.strftime('%Y-%m-%d %H:%M UTC', time.gmtime(expires))}.",
        "#pragma once",
        "",
        "#include <cstdint>",
        "",
        "namespace alc::fob::keys",
        "{",
        f"  constexpr uint32_t M_GENERATED_UNIX {{ {now} }};",
        f"  constexpr uint16_t M_DAY {{ {day} }};",
        f"  constexpr uint8_t M_SLOT {{ {args.slot} }};",
        f"  constexpr uint8_t M_DEVICE_COUNT {{ {len(entries)} }};",
        f"  constexpr uint16_t M_SEQUENCE_COUNT {{ {args.count} }};",
        "",
        "  struct Device",
        "  {",
        "      const char* serial;",
        "      uint32_t deviceId;",
        "      uint8_t encKey[16];",
        "      uint8_t rotatingIds[M_SEQUENCE_COUNT][4];",
        "      // FIRE, n 0, minute 0, sealed by the Python reference: the fob's boot self-test.",
        "      uint8_t testSealed[16];",
        "  };",
        "",
        "  constexpr Device M_DEVICES[M_DEVICE_COUNT] {",
    ]
    for serial, device_id, enc_key, rotating, test in entries:
        lines += [
            f'    {{ "{serial}", 0x{device_id:08X}, {c_bytes(enc_key)},',
            "      { " + ", ".join(c_bytes(r) for r in rotating) + " },",
            f"      {c_bytes(test)} }},",
        ]
    lines += ["  };", "}", ""]

    out_dir = args.out if args.fake else ROOT / "secrets"
    out_dir.mkdir(parents=True, exist_ok=True)
    out = out_dir / "fire_fob_keys.h"
    out.write_text("\n".join(lines))
    out.chmod(0o600)
    if args.fake:
        print(f"Wrote {out}: ALL-ZERO keys, compile check only - never flash.")
        return
    print(f"Wrote {out.relative_to(ROOT)}: slot {args.slot}, key day {day}, {len(entries)} devices "
          f"({', '.join(e[0] for e in entries)}), {args.count} presses each.")
    print(f"Keys expire {time.strftime('%Y-%m-%d %H:%M UTC', time.gmtime(expires))}. Build and flash the fob now: "
          "its clock starts from this moment.")


if __name__ == "__main__":
    main()
