"""Freeze a following block that inherits the selected body's font state."""

import argparse, hashlib, json
from pathlib import Path
from pypdf import PdfReader, PdfWriter
from pypdf.generic import DecodedStreamObject, NameObject


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    base = root / "fixtures/existing-text-edit"
    styles = json.loads((base / "font-styles.json").read_text(encoding="utf-8"))
    source = base / styles["source"]
    assert hashlib.sha256(source.read_bytes()).hexdigest() == styles["source_sha256"]
    args.output.mkdir(exist_ok=False, parents=True)
    writer = PdfWriter()
    writer.clone_document_from_reader(PdfReader(source))
    page = writer.pages[0]
    data = page.get_contents().get_data()
    needle = b"(Original body line) Tj ET"
    assert data.count(needle) == 1
    data = data.replace(
        needle, needle + b"\nBT 1 0 0 1 60 665 Tm (AFTER FONT STATE) Tj ET", 1
    )
    stream = DecodedStreamObject()
    stream.set_data(data)
    page[NameObject("/Contents")] = writer._add_object(stream)
    path = args.output / "body-text.pdf"
    with path.open("xb") as f:
        writer.write(f)
    record = dict(
        file=path.name,
        sha256=hashlib.sha256(path.read_bytes()).hexdigest(),
        source_sha256=styles["source_sha256"],
        selected=styles["selected_text"],
        replacement=styles["replacement"],
        families=styles["families"],
        following_text="AFTER FONT STATE",
        unsupported_scalar=1114111,
        required="Following block has no Tf and must preserve original glyphs, font and positions after explicit family replacement",
        scope="Additional state-leakage input fixed before first font operation; original cases unchanged",
    )
    (args.output / "criteria.json").write_text(
        json.dumps(record, ensure_ascii=False, indent=2), encoding="utf-8"
    )
    print(json.dumps(record, ensure_ascii=False))


if __name__ == "__main__":
    main()
