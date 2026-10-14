"""Freeze shared nested Form text before product operations."""

import argparse
import hashlib
import json
from pathlib import Path
from pypdf import PdfReader, PdfWriter
from pypdf.generic import DictionaryObject, NameObject


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    base = root / "fixtures/existing-image-edit/forms"
    old = json.loads((base / "criteria.json").read_text(encoding="utf-8"))
    args.output.mkdir(exist_ok=False, parents=True)
    files = {}
    for name in (
        "shared-forms.pdf",
        "cyclic-form.pdf",
        "group-form.pdf",
        "clipped-form.pdf",
    ):
        source = base / name
        assert hashlib.sha256(source.read_bytes()).hexdigest() == old["files"][name]
        writer = PdfWriter()
        writer.clone_document_from_reader(PdfReader(source))
        page = writer.pages[0]
        outer = page["/Resources"]["/XObject"]["/TatsujinFixtureOuter"].get_object()
        inner = outer["/Resources"]["/XObject"]["/Inner"].get_object()
        resources = DictionaryObject(inner["/Resources"].get_object())
        resources[NameObject("/Font")] = DictionaryObject(
            {NameObject("/FormFont"): page["/Resources"]["/Font"].raw_get("/F1")}
        )
        inner[NameObject("/Resources")] = resources
        inner.set_data(
            inner.get_data()
            + b"\nBT /FormFont 12 Tf 0 Tr 0 Tc 0 Tw 100 Tz 0 Ts 15 TL 1 0 0 1 5 80 Tm (Form first line) Tj T* (Form second line) Tj ET\n"
        )
        target = args.output / name
        with target.open("xb") as stream:
            writer.write(stream)
        files[name] = hashlib.sha256(target.read_bytes()).hexdigest()
    fixed = dict(
        files=files,
        source_files=old["files"],
        selected="Form first line\nForm second line",
        selected_invocation=0,
        depth=2,
        english="Edited first row\nEdited next row",
        japanese="本文を編集\n署名を保持",
        reedited="本文を更新\n画像を保持",
        three_lines="本文を編集\n署名を保持\n画像を保持",
        explicit_family="Meiryo UI",
        leading_ratio=1.25,
        refused=["cyclic-form.pdf", "group-form.pdf", "clipped-form.pdf"],
        scope="Additional synthetic CC0 group-text contract fixed before implementation and product operations; original criteria and thresholds unchanged",
    )
    (args.output / "criteria.json").write_text(
        json.dumps(fixed, ensure_ascii=False, indent=2), encoding="utf-8"
    )
    print(json.dumps(fixed, ensure_ascii=False, indent=2))


if __name__ == "__main__":
    main()
