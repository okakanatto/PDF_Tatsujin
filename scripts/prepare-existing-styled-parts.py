"""Freeze independent-position styled text parts before product operations."""

import argparse
import hashlib
import io
import json
from pathlib import Path

from reportlab.pdfbase import pdfmetrics
from reportlab.pdfbase.ttfonts import TTFont
from reportlab.pdfgen import canvas
from pypdf import PdfReader, PdfWriter
from pypdf.generic import (
    DecodedStreamObject,
    DictionaryObject,
    FloatObject,
    NameObject,
    NumberObject,
    RectangleObject,
)


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=False)
    root = Path(__file__).resolve().parents[1]
    source = root / "fixtures/existing-text-edit/visible/body-text.pdf"
    font = root / "fixtures/NotoSansJP-fixture.ttf"
    japanese = "編集します日本語本文"
    pdfmetrics.registerFont(TTFont("StyledFixture", str(font)))
    buffer = io.BytesIO()
    c = canvas.Canvas(buffer, pagesize=(612, 792), pageCompression=0, invariant=1)
    c.setFont("StyledFixture", 16)
    c.drawString(60.25, 610.125, japanese)
    c.save()
    jp_page = PdfReader(buffer).pages[0]
    jp_fonts = jp_page["/Resources"]["/Font"].get_object()
    jp_name = next(
        k for k, v in jp_fonts.items() if v.get_object().get("/Subtype") == "/TrueType"
    )
    jp_content = jp_page.get_contents().get_data()
    begin = jp_content.index(b"BT 1 0 0 1 60.25 610.125 Tm")
    jp_block = jp_content[begin + 3 : jp_content.index(b"ET", begin)]
    jp_block = jp_block.replace(jp_name.encode(), b"/StyledJP").replace(b" T*", b"")
    files = {}
    for name in ("absolute", "rotated", "relative-refused", "spacing-refused"):
        writer = PdfWriter()
        writer.clone_document_from_reader(PdfReader(source))
        page = writer.pages[0]
        resources = DictionaryObject(page["/Resources"].get_object())
        fonts = DictionaryObject(resources["/Font"].get_object())
        helvetica = next(
            k
            for k, v in fonts.items()
            if v.get_object().get("/BaseFont") == "/Helvetica"
        )
        fonts[NameObject("/StyledBold")] = writer._add_object(
            DictionaryObject(
                {
                    NameObject("/Type"): NameObject("/Font"),
                    NameObject("/Subtype"): NameObject("/Type1"),
                    NameObject("/BaseFont"): NameObject("/Helvetica-Bold"),
                    NameObject("/Encoding"): NameObject("/WinAnsiEncoding"),
                }
            )
        )
        fonts[NameObject("/StyledJP")] = jp_fonts[jp_name].clone(writer)
        resources[NameObject("/Font")] = fonts
        page[NameObject("/Resources")] = resources
        block = (
            b"BT 0 Tr 0 Tc 0 Tw 100 Tz 0 Ts\n"
            + helvetica.encode()
            + b" 16 Tf 1 0 0 1 60.25 660.125 Tm (Intro ) Tj\n"
            b"/StyledBold 18 Tf 1 0 0 1 125.25 660.125 Tm [(Styled ) 25 (phrase)] TJ\n"
            + helvetica.encode()
            + b" 16 Tf 1 0 0 1 290.25 660.125 Tm (Suffix intact) Tj\n"
            + jp_block
            + b"\nET\n"
            b"BT 1 0 0 1 60.25 570.125 Tm (FOLLOWING STYLE STATE) Tj ET\n"
        )
        if name == "relative-refused":
            block = block.replace(b"1 0 0 1 125.25 660.125 Tm", b"")
        if name == "spacing-refused":
            block = block.replace(b"0 Tc", b"1 Tc", 1)
        stream = DecodedStreamObject()
        stream.set_data(page.get_contents().get_data() + b"\n" + block)
        page[NameObject("/Contents")] = writer._add_object(stream)
        if name == "rotated":
            page[NameObject("/Rotate")] = NumberObject(90)
            page[NameObject("/CropBox")] = RectangleObject([20, 25, 590, 770])
            page[NameObject("/UserUnit")] = FloatObject(1.25)
        path = args.output / (name + ".pdf")
        with path.open("xb") as handle:
            writer.write(handle)
        files[path.name] = digest(path)
    fixed = dict(
        files=files,
        source_inputs={
            str(p.relative_to(root)).replace("\\", "/"): digest(p)
            for p in (source, font)
        },
        replacements={
            "Intro ": "Start ",
            "Styled phrase": "Bold text",
            "Suffix intact": "End intact",
            japanese: "日本語本文編集します",
        },
        reedited="Bold edit",
        remove="Styled phrase",
        following="FOLLOWING STYLE STATE",
        refusals={
            "relative-refused.pdf": "Styled phrase",
            "spacing-refused.pdf": "Styled phrase",
        },
        position="Unselected character boxes exactly unchanged",
        outside_pixels=0,
        raster_dpi=300,
        boundary_pixels=1,
        scope="Synthetic CC0 text; Noto fixture OFL-1.1. Fresh absolute-position cases fixed before implementation; prior mixed-font paragraph refusal retained.",
    )
    path = args.output / "criteria.json"
    path.write_text(json.dumps(fixed, ensure_ascii=False, indent=2), encoding="utf-8")
    print(json.dumps(dict(criterion_sha256=digest(path), files=files)))


if __name__ == "__main__":
    main()
