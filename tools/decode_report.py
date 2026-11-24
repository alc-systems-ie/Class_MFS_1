#!/usr/bin/env python3
"""Decodes MFS event reports relayed by the hub - a bench stand-in for the Network Manager's decrypt step.

    python3 tools/decode_report.py <serial>                 # every UUID in ../class_hub/logs/uuids.jsonl
    python3 tools/decode_report.py <serial> <uuid hex> ...  # just these UUIDs
    python3 tools/decode_report.py --self-test              # the known-answer vectors, no secrets

Reads the device ID, secret and report generation from secrets/<serial>.conf (owner-only, never
committed) and prints only the decoded event: never the secret or any key derived from it.

A report is reportId(4) | AES-128-CCM ciphertext(8) | tag(4) under a key fixed per device and report
generation (docs/superpowers/specs/2026-11-24-event-reports-design.md section 4). The decoder tries
the conf's generation and the few before it (a failed flash still burns one), and counters from 0
(every flash erases the device's counter). A UUID that matches nothing is not this device's report.
"""
import argparse
import datetime
import json
import pathlib
import sys

from cryptography.exceptions import InvalidTag
from cryptography.hazmat.primitives.ciphers.aead import AESCCM

import gen_access_vectors as scheme

M_HERE = pathlib.Path(__file__).resolve().parent
M_REPO = M_HERE.parent
M_DEFAULT_LOG = M_REPO.parent / "class_hub" / "logs" / "uuids.jsonl"
M_UUID_BYTES = 16
M_REPORT_ID_BYTES = 4
M_GENERATION_LOOKBACK = 8
M_COUNTER_SEARCH = 4096
M_BATTERY_NOT_MEASURED = 0xFF
M_TENTHS_PER_VOLT = 10.0

M_EVENTS = {
    0x01: "low battery",
    0x02: "activated",
    0x03: "deactivated",
    0x10: "fired from command",
    0x11: "fired from motion",
    0x12: "fired from sound level",
}


def load_device(serial: str) -> tuple[int, bytes, int]:
    """(device ID, secret, report generation) from secrets/<serial>.conf. The secret never leaves this process."""
    path = M_REPO / "secrets" / f"{serial}.conf"
    values = {}
    for line in path.read_text().splitlines():
        line = line.strip()
        if not line.startswith("CONFIG_") or "=" not in line:
            continue
        name, value = line.split("=", 1)
        values[name] = value.strip().strip('"')
    missing = [name for name in ("CONFIG_MFS_DEVICE_ID", "CONFIG_MFS_DEVICE_SECRET", "CONFIG_MFS_REPORT_GENERATION")
               if not values.get(name)]
    if missing:
        sys.exit(f"{path.name} does not set {', '.join(missing)}.")
    device_id = int(values["CONFIG_MFS_DEVICE_ID"], 16)
    secret = bytes.fromhex(values["CONFIG_MFS_DEVICE_SECRET"])
    generation = int(values["CONFIG_MFS_REPORT_GENERATION"])
    if generation == 0:
        sys.exit(f"{serial} is at report generation 0: reports are disabled on that build.")
    return device_id, secret, generation


class ReportDecoder:
    """Precomputes the report IDs for the generations and counters searched, then decodes UUIDs against them."""

    def __init__(self, device_id: int, secret: bytes, newest_generation: int):
        self.device_id = device_id
        self.keys = {}
        self.index = {}
        oldest = max(1, newest_generation - M_GENERATION_LOOKBACK)
        for generation in range(newest_generation, oldest - 1, -1):
            report_key = scheme.report_key(secret, device_id, generation)
            self.keys[generation] = scheme.report_enc_key(report_key)
            for counter in range(M_COUNTER_SEARCH):
                self.index.setdefault(scheme.report_id(report_key, counter), (generation, counter))

    def decode(self, uuid: bytes) -> dict:
        report_id = uuid[:M_REPORT_ID_BYTES]
        match = self.index.get(report_id)
        if match is None:
            return {"error": "not a report from this device (no generation/counter matches its report ID)"}
        generation, counter = match
        aad = report_id + bytes([scheme.REPORT_VERSION])
        nonce = scheme.report_nonce(self.device_id, generation, counter)
        try:
            plaintext = AESCCM(self.keys[generation], tag_length=scheme.TAG_BYTES).decrypt(nonce, uuid[M_REPORT_ID_BYTES:], aad)
        except InvalidTag:
            return {"generation": generation, "counter": counter, "error": "report ID matched but the tag failed"}
        event = M_EVENTS.get(plaintext[0], f"unknown event 0x{plaintext[0]:02X}")
        battery = None if plaintext[1] == M_BATTERY_NOT_MEASURED else plaintext[1] / M_TENTHS_PER_VOLT
        return {"generation": generation, "counter": counter, "event": event, "battery_v": battery}


def parse_uuid(text: str) -> bytes:
    raw = bytes.fromhex(text.replace("-", "").strip())
    if len(raw) != M_UUID_BYTES:
        raise ValueError(f"{text!r} is not 16 bytes")
    return raw


def describe(result: dict) -> str:
    if "event" in result:
        battery = "not measured" if result["battery_v"] is None else f"{result['battery_v']:.1f} V"
        return f"{result['event']}, battery {battery} (generation {result['generation']}, counter {result['counter']})"
    return result["error"]


def from_log(path: pathlib.Path) -> list[tuple[str, dict]]:
    records = [json.loads(line) for line in path.read_text().splitlines() if line.strip()]
    return [(record["uuid"], record) for record in records]


def self_test() -> int:
    failures = 0
    for name, generation, counter, plaintext in scheme.REPORT_CASES:
        if counter >= M_COUNTER_SEARCH:
            continue
        decoder = ReportDecoder(scheme.DEVICE_ID, scheme.SECRET, generation)
        sealed = scheme.seal_report(scheme.SECRET, scheme.DEVICE_ID, generation, counter, plaintext)
        result = decoder.decode(sealed)
        expected_event = M_EVENTS[plaintext[0]]
        ok = result.get("event") == expected_event and result.get("counter") == counter and result.get("generation") == generation
        failures += not ok
        print(f"{'PASS' if ok else 'FAIL'} {name}: {describe(result)}")
    tampered = bytearray(scheme.seal_report(scheme.SECRET, scheme.DEVICE_ID, 1, 0, scheme.REPORT_CASES[0][3]))
    tampered[-1] ^= 0x01
    result = ReportDecoder(scheme.DEVICE_ID, scheme.SECRET, 1).decode(bytes(tampered))
    ok = result.get("error") == "report ID matched but the tag failed"
    failures += not ok
    print(f"{'PASS' if ok else 'FAIL'} tampered tag: {describe(result)}")
    older = scheme.seal_report(scheme.SECRET, scheme.DEVICE_ID, 3, 5, scheme.REPORT_CASES[0][3])
    result = ReportDecoder(scheme.DEVICE_ID, scheme.SECRET, 5).decode(older)
    ok = result.get("generation") == 3 and result.get("counter") == 5
    failures += not ok
    print(f"{'PASS' if ok else 'FAIL'} older generation: {describe(result)}")
    result = ReportDecoder(scheme.DEVICE_ID, bytes(32), 1).decode(scheme.seal_report(scheme.SECRET, scheme.DEVICE_ID, 1, 0, scheme.REPORT_CASES[0][3]))
    ok = "event" not in result
    failures += not ok
    print(f"{'PASS' if ok else 'FAIL'} another device's report: {describe(result)}")
    return 1 if failures else 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("serial", nargs="?", help="device serial, e.g. MFS1-1126-00008")
    parser.add_argument("uuids", nargs="*", help="relayed UUIDs as hex; default: every entry in the hub's uuids.jsonl")
    parser.add_argument("--log", type=pathlib.Path, default=M_DEFAULT_LOG, help="the hub bridge's uuids.jsonl")
    parser.add_argument("--self-test", action="store_true", help="decode the known-answer vectors (no secrets)")
    args = parser.parse_args()
    if args.self_test:
        return self_test()
    if not args.serial:
        parser.error("a serial is required (or --self-test)")
    device_id, secret, generation = load_device(args.serial)
    decoder = ReportDecoder(device_id, secret, generation)
    del secret
    if args.uuids:
        entries = [(text, {}) for text in args.uuids]
    else:
        entries = from_log(args.log)
    for text, record in entries:
        try:
            result = decoder.decode(parse_uuid(text))
        except ValueError as error:
            print(f"{text}: {error}")
            continue
        heard = ""
        if record.get("utc_known") and "utc" in record:
            heard = datetime.datetime.fromtimestamp(record["utc"], datetime.timezone.utc).strftime("%Y-%m-%d %H:%M:%S UTC ")
        rssi = f" rssi {record['rssi']}" if "rssi" in record else ""
        print(f"{heard}{args.serial}: {describe(result)}{rssi}  [{text}]")
    return 0


if __name__ == "__main__":
    sys.exit(main())
