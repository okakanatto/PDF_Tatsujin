"""Freeze a wrapping contract using a known constant-width source font."""

import argparse
import hashlib
import json
from pathlib import Path
from pypdf import PdfReader, PdfWriter
from pypdf.generic import DictionaryObject, NameObject, DecodedStreamObject


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    source = root / "fixtures/existing-text-edit/forms/shared-forms.pdf"
    original_sha = "9537f53f54d3961465a270938411db625589b6a6904a1a00ce8ccba5b4020ddd"
    assert hashlib.sha256(source.read_bytes()).hexdigest() == original_sha
    args.output.mkdir(parents=True, exist_ok=False)
    files = {}
    for name, rotate in (("uniform.pdf", False), ("rotated.pdf", True)):
        writer = PdfWriter()
        writer.clone_document_from_reader(PdfReader(source))
        page = writer.pages[0]
        fonts = DictionaryObject(page["/Resources"]["/Font"].get_object())
        fonts[NameObject("/WrapCourier")] = writer._add_object(
            DictionaryObject(
                {
                    NameObject("/Type"): NameObject("/Font"),
                    NameObject("/Subtype"): NameObject("/Type1"),
                    NameObject("/BaseFont"): NameObject("/Courier"),
                    NameObject("/Encoding"): NameObject("/WinAnsiEncoding"),
                }
            )
        )
        page["/Resources"][NameObject("/Font")] = fonts
        stream = DecodedStreamObject()
        stream.set_data(
            page.get_contents().get_data()
            + b"\nBT /WrapCourier 12 Tf 0 Tr 0 Tc 0 Tw 100 Tz 0 Ts 15 TL 1 0 0 1 60 500 Tm (Wrap first row) Tj T* (Wrap second row) Tj ET\n"
        )
        page[NameObject("/Contents")] = writer._add_object(stream)
        if rotate:
            page.rotate(90)
        target = args.output / name
        with target.open("xb") as output:
            writer.write(output)
        files[name] = hashlib.sha256(target.read_bytes()).hexdigest()
    fixed = dict(
        files=files,
        source_sha256=original_sha,
        selected="Wrap first row\nWrap second row",
        english="Alpha beta gamma delta",
        english_wrapped="Alpha beta\ngamma delta",
        english_width_pt=200,
        long_word="ABCDEFGHIJKLMN",
        long_word_wrapped="ABCDEFGHIJKLM\nN",
        long_word_width_pt=187.2,
        japanese="日本語の本文を編集",
        japanese_wrapped="日本語の本文\nを編集",
        reedited="日本語の本文を変更",
        reedited_wrapped="日本語の本文\nを変更",
        narrow_wrapped="日本語の\n本文を編\n集",
        narrow_width_pt=96,
        japanese_width_pt=144,
        family="Meiryo UI",
        leading_ratio=1.25,
        scope="New fixed wrapping contract; original expected data and thresholds unchanged",
    )
    (args.output / "criteria.json").write_text(
        json.dumps(fixed, ensure_ascii=False, indent=2), encoding="utf-8"
    )
    print(json.dumps(fixed, ensure_ascii=False, indent=2))


if __name__ == "__main__":
    main()
