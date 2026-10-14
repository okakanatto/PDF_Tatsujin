"""Freeze transformed vector and shared-form selection boundary cases."""

import argparse
import copy
import hashlib
import json
from pathlib import Path

from pypdf import PdfReader, PdfWriter
from pypdf.generic import ArrayObject, FloatObject, NameObject, NumberObject
from prepare_redaction_helpers import stream


def sha(data):
    return hashlib.sha256(data).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=False)
    root = Path(__file__).resolve().parents[1]
    folder = root / "fixtures/redaction-copy/vector-paths-v1"
    original = next(
        r
        for r in json.loads((folder / "criteria.json").read_text(encoding="utf-8"))[
            "cases"
        ]
        if r["file"] == "form.pdf"
    )
    rows = []
    for angle in (0, 90, 180, 270):
        source = folder / "form.pdf"
        assert sha(source.read_bytes()) == original["sha256"]
        writer = PdfWriter(clone_from=PdfReader(source))
        page = writer.pages[0]
        page[NameObject("/Rotate")] = NumberObject(angle)
        page[NameObject("/CropBox")] = ArrayObject(
            list(map(NumberObject, (12, 18, 600, 780)))
        )
        page[NameObject("/UserUnit")] = FloatObject(1.5)
        path = args.output / f"rotation-{angle}.pdf"
        writer.write(path)
        row = dict(original)
        row.update(
            file=path.name,
            sha256=sha(path.read_bytes()),
            folder=args.output.name,
            refused=False,
        )
        rows.append(row)
    shared = root / "fixtures/redaction-copy/shared-fonts-v1"
    original = next(
        r
        for r in json.loads((shared / "criteria.json").read_text(encoding="utf-8"))[
            "cases"
        ]
        if r["file"] == "shared-target-form-refused.pdf"
    )
    for name in ("copied-form-refused", "different-selections-refused"):
        source = shared / original["file"]
        assert sha(source.read_bytes()) == original["sha256"]
        writer = PdfWriter(clone_from=PdfReader(source))
        if name == "copied-form-refused":
            form = writer.pages[1]["/Resources"]["/XObject"]["/Shared"]
            writer.pages[1][NameObject("/Resources")] = copy.copy(
                writer.pages[1]["/Resources"]
            )
            writer.pages[1]["/Resources"][NameObject("/XObject")] = copy.copy(
                writer.pages[1]["/Resources"]["/XObject"]
            )
            writer.pages[1]["/Resources"]["/XObject"][NameObject("/Shared")] = (
                writer._add_object(copy.copy(form))
            )
        path = args.output / (name + ".pdf")
        writer.write(path)
        row = dict(original)
        row.update(
            file=path.name, sha256=sha(path.read_bytes()), folder=args.output.name
        )
        row["plan"] = [{"page": 0, "rect": row["rect"]}]
        if name == "different-selections-refused":
            row["plan"].append({"page": 1, "rect": [300, 700, 30, 30]})
        rows.append(row)
    destination = args.output / "criteria.json"
    destination.write_text(
        json.dumps(
            {"version": 1, "cases": rows, "native_GUI": "未実行"},
            ensure_ascii=False,
            indent=2,
        )
        + "\n",
        encoding="utf-8",
    )
    print(
        json.dumps(
            {"cases": len(rows), "criteria_sha256": sha(destination.read_bytes())}
        )
    )


if __name__ == "__main__":
    main()
