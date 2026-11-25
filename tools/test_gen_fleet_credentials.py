#!/usr/bin/env python3
"""Host test for tools/gen_fleet_credentials.py. Plain Python, never touches secrets/.

    uv run --with segno --with openpyxl python3 tools/test_gen_fleet_credentials.py

(openpyxl is needed to import the script; segno is optional: the QR file checks are skipped without it.)
"""
import pathlib
import re
import stat
import subprocess
import sys
import tempfile

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import gen_fleet_credentials as g  # noqa: E402

# Copied from class_nm/registry/qr.py and registry/models.py (DEVICE_SERIAL_PATTERN); keep in step.
SERIAL = r"[A-Z0-9]{4}-[0-9]{4}-[0-9A-F]{5}"
LABEL = re.compile(rf"CLASS1:D:({SERIAL}):([0-9A-F]{{8}})", re.ASCII)
SECRET = re.compile(rf"CLASS1:S:({SERIAL}):([0-9A-F]{{8}}):([0-9A-F]{{64}}):([0-9A-F]{{64}}):([0-9]{{1,5}})", re.ASCII)

failures = 0


def check(name, ok):
    global failures
    print(("PASS " if ok else "FAIL ") + name)
    failures += 0 if ok else 1


sequence = iter([0, 5, 0xFF, 0x100, 0x101])
check("newDeviceId skips reserved and used", g.newDeviceId({0x100}, lambda: next(sequence)) == 0x101)

serial = g.serial_of("MFS1", "1126", 1)
device_id, secret, provision = "FA4E0001", "A" * 64, "B" * 64
match = LABEL.fullmatch(g.labelQrText(serial, device_id))
check("label text matches the NM format", bool(match) and match.groups() == (serial, device_id))
match = SECRET.fullmatch(g.secretQrText(serial, device_id, secret, provision))
check("secret text matches the NM format, generation 0", bool(match) and match.groups() == (serial, device_id, secret, provision, "0"))

with tempfile.TemporaryDirectory() as tmp:
    out = pathlib.Path(tmp) / "out"
    result = subprocess.run([sys.executable, str(pathlib.Path(g.__file__)), "--model", "MFS1", "--mmyy", "1126", "--count", "3",
                             "--out-dir", str(out)], capture_output=True, text=True)
    check("generator runs", result.returncode == 0)
    ids = [int(m, 16) for m in re.findall(r"ID ([0-9A-F]{8})", result.stdout)]
    check("three IDs, none reserved", len(ids) == 3 and all(i > 0xFF for i in ids))
    again = subprocess.run([sys.executable, str(pathlib.Path(g.__file__)), "--model", "MFS1", "--mmyy", "1126", "--count", "1",
                            "--out-dir", str(out)], capture_output=True, text=True)
    check("pre-existing --out-dir is refused", again.returncode != 0)
    try:
        import segno  # noqa: F401
    except ImportError:
        print("SKIP QR file checks (segno not installed)")
    else:
        pngs = sorted((out / "MFS1-1126-qr").glob("*.png"))
        check("six non-empty PNGs, all 0600", len(pngs) == 6 and all(p.stat().st_size > 0 and stat.S_IMODE(p.stat().st_mode) == 0o600 for p in pngs))

sys.exit(1 if failures else 0)
