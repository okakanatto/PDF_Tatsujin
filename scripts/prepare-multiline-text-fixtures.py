"""Freeze uniform two-line body blocks before product operations."""

import argparse
import hashlib
import json
from pathlib import Path

from pypdf import PdfReader, PdfWriter
from pypdf.generic import DecodedStreamObject, NameObject


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    base = root / "fixtures/existing-text-edit"
    source = base / "font-state/body-text.pdf"
    assert (
        hashlib.sha256(source.read_bytes()).hexdigest()
        == "878c862cbf57ad38409c5841e47c60d0399c1e9e90a7624bafeb3baadc582693"
    )
    args.output.mkdir(exist_ok=False, parents=True)
    files = {}
    for name, mixed in (("uniform.pdf", False), ("mixed-fonts.pdf", True)):
        writer = PdfWriter()
        writer.clone_document_from_reader(PdfReader(source))
        page = writer.pages[0]
        fonts = page["/Resources"]["/Font"].get_object()
        helvetica = next(
            k
            for k, v in fonts.items()
            if v.get_object().get("/BaseFont") == "/Helvetica"
        )
        command = (
            b"\nBT 0 Tr "
            + helvetica.encode("ascii")
            + b" 16 Tf 22 TL 1 0 0 1 60 380 Tm (First body line) Tj T* "
        )
        if mixed:
            command += helvetica.encode("ascii") + b" 18 Tf "
        command += b"(Second body line) Tj ET\nBT 1 0 0 1 60 310 Tm (AFTER MULTILINE STATE) Tj ET\n"
        stream = DecodedStreamObject()
        stream.set_data(page.get_contents().get_data() + command)
        page[NameObject("/Contents")] = writer._add_object(stream)
        path = args.output / name
        with path.open("xb") as target:
            writer.write(target)
        files[name] = hashlib.sha256(path.read_bytes()).hexdigest()
    plan = dict(
        files=files,
        source_sha256=hashlib.sha256(source.read_bytes()).hexdigest(),
        selected="First body line\nSecond body line",
        replacement="Edited first line\nEdited second line",
        japanese="本文の一行目を編集します。\n本文の二行目を編集します。",
        reedited="第一行を再編集しました。\n第二行を再編集しました。",
        original_lines=2,
        leading_pt=22,
        explicit_family="Meiryo UI",
        following="AFTER MULTILINE STATE",
        geometry=[120, 300, 320, 80],
        refused="Changed line count and mixed font state; original immutable",
        scope="Additional synthetic CC0 input fixed before first multiline product operation; original criteria and thresholds unchanged",
    )
    (args.output / "criteria.json").write_text(
        json.dumps(plan, ensure_ascii=False, indent=2), encoding="utf-8"
    )
    print(json.dumps(plan, ensure_ascii=False, indent=2))


if __name__ == "__main__":
    main()
