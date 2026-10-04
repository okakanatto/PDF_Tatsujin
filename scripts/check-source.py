"""Portable source checks; never substitutes for the Windows acceptance suite."""

import ast
import hashlib
import json
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[1]
GROUND_TRUTH_SHA256 = "7a8878927ec067c80f8b5b42640203beb7d7a99daec17a790d7f5e661e4b15e7"


def sha256(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    truth = ROOT / "fixtures/ground-truth.json"
    if sha256(truth) != GROUND_TRUTH_SHA256:
        raise RuntimeError(
            "Frozen ground truth changed; do not retune acceptance inputs."
        )
    entries = json.loads((ROOT / "fixtures/manifest.json").read_text(encoding="utf-8"))
    for entry in entries:
        if sha256(ROOT / "fixtures" / entry["file"]) != entry["sha256"]:
            raise RuntimeError(f"Fixture hash mismatch: {entry['file']}")
    for path in (ROOT / "scripts").glob("*.py"):
        ast.parse(path.read_text(encoding="utf-8-sig"), filename=str(path))
    lock = json.loads((ROOT / "dependency-lock.json").read_text(encoding="utf-8"))
    head = subprocess.check_output(
        ["git", "-C", str(ROOT / "vendor/PDF4QT"), "rev-parse", "HEAD"], text=True
    ).strip()
    if head != lock["pdf4qt"]["commit"]:
        raise RuntimeError("PDF4QT checkout does not match dependency-lock.json")
    print(
        f"PASS: {len(entries)} frozen fixtures, ground truth, Python syntax, PDF4QT pin"
    )


if __name__ == "__main__":
    main()
