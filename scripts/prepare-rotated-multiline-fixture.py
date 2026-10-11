"""Freeze rotated/cropped/UserUnit multiline input before its first operation."""

import argparse
import hashlib
import json
from pathlib import Path
from pypdf import PdfReader, PdfWriter
from pypdf.generic import NameObject, NumberObject, RectangleObject


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    base = root / "fixtures/existing-text-edit/multiline"
    plan = json.loads((base / "criteria.json").read_text(encoding="utf-8"))
    source = base / "uniform.pdf"
    assert (
        hashlib.sha256(source.read_bytes()).hexdigest() == plan["files"]["uniform.pdf"]
    )
    args.output.mkdir(exist_ok=False, parents=True)
    writer = PdfWriter()
    writer.clone_document_from_reader(PdfReader(source))
    page = writer.pages[0]
    page[NameObject("/Rotate")] = NumberObject(90)
    page[NameObject("/UserUnit")] = NumberObject(2)
    page[NameObject("/CropBox")] = RectangleObject([20, 40, 590, 760])
    path = args.output / "body-text.pdf"
    with path.open("xb") as target:
        writer.write(target)
    record = dict(
        file=path.name,
        sha256=hashlib.sha256(path.read_bytes()).hexdigest(),
        source_sha256=plan["files"]["uniform.pdf"],
        selected=plan["selected"],
        geometry=[100, 250, 300, 80],
        required="90-degree rotation, CropBox and UserUnit=2 multiline geometry; all unselected glyphs/images/forms intact",
        scope="Separate rotated positive case fixed before first product operation; existing criteria unchanged",
    )
    (args.output / "criteria.json").write_text(
        json.dumps(record, ensure_ascii=False, indent=2), encoding="utf-8"
    )
    print(json.dumps(record, ensure_ascii=False))


if __name__ == "__main__":
    main()
