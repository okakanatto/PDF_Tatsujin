"""Freeze an unrotated unit derivative without replacing the rotated criterion."""

import hashlib
import json
from pathlib import Path

from pypdf import PdfWriter
from pypdf.generic import NameObject, NumberObject

from vertical_ocr_fixtures import frozen_vertical

ROOT = Path(__file__).resolve().parents[1]


def main():
    fixed, source, _ = frozen_vertical("rotated")
    output = ROOT / "fixtures/vertical-ocr-unit"
    output.mkdir(exist_ok=False)
    writer = PdfWriter(clone_from=source)
    writer.pages[fixed["page"] - 1][NameObject("/Rotate")] = NumberObject(0)
    target = output / "crop-unit.pdf"
    writer.write(target)
    fixed.update(
        source=target.name,
        source_sha256=hashlib.sha256(target.read_bytes()).hexdigest(),
        rotation=0,
        provenance="CC0 derivative; only Rotate set0; original rotation success criterion retained separately",
        comparison="Unrotated physical CropBox points; UserUnit1.5 and existing fixed truth/search2mm preserved",
    )
    criterion = output / "criteria.json"
    criterion.write_bytes(
        (json.dumps(fixed, ensure_ascii=False, indent=2) + "\n")
        .replace("\n", "\r\n")
        .encode("utf-8")
    )
    print(hashlib.sha256(criterion.read_bytes()).hexdigest())


if __name__ == "__main__":
    main()
