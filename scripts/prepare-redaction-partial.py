"""Freeze additional partial-text redaction cases before running the product."""

import argparse
import hashlib
import io
import json
import re
import zlib
from pathlib import Path

import pypdfium2 as pdfium
from fontTools.ttLib import TTFont as FontProgram
from pypdf import PdfReader, PdfWriter
from pypdf.generic import (
    ArrayObject,
    ByteStringObject,
    ContentStream,
    DecodedStreamObject,
    DictionaryObject,
    FloatObject,
    NameObject,
    NumberObject,
)
from reportlab.pdfbase import pdfmetrics
from reportlab.pdfbase.ttfonts import TTFont
from reportlab.pdfgen import canvas


def sha(data):
    return hashlib.sha256(data).hexdigest()


def boxes(path, phrase):
    with pdfium.PdfDocument(path) as pdf:
        page = pdf[0]
        text = page.get_textpage()
        content = text.get_text_range()
        assert content.count(phrase) == 1, (path, content, phrase)
        start = content.index(phrase)
        return [list(text.get_charbox(i)) for i in range(start, start + len(phrase))]


def stream(data, **entries):
    value = DecodedStreamObject()
    value.set_data(data)
    for key, entry in entries.items():
        value[NameObject("/" + key)] = entry
    return value


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=False)
    root = Path(__file__).resolve().parents[1]
    pdfmetrics.registerFont(
        TTFont("PartialJP", str(root / "assets/fonts/NotoSansJP.ttf"))
    )
    rows = []
    names = [
        "standard",
        "visible",
        "invisible",
        "TJ-spacing",
        "identity-H",
        "form",
        "nested-form",
        "scan-OCR",
    ]
    names += [f"rotation-{angle}" for angle in (0, 90, 180, 270)]
    for name in names:
        raw = io.BytesIO()
        pdf = canvas.Canvas(raw, pagesize=(612, 792), invariant=True)
        standard = name == "standard"
        remove = "SECRET9" if standard else "秘匿機密亀鶴"
        left = "LEFT" if standard else "公開"
        right = "RIGHT" if standard else "文字保持"
        text = pdf.beginText(60, 650)
        text.setFont("Helvetica" if standard else "PartialJP", 22)
        if name in ("invisible", "scan-OCR"):
            text.setTextRenderMode(3)
        if name == "TJ-spacing":
            text.setCharSpace(1.25)
            text.setWordSpace(3)
            text.setHorizScale(85)
        text.textOut(left + " " + remove + " " + right)
        pdf.drawText(text)
        pdf.setFont("Helvetica", 16)
        pdf.drawString(60, 550, "KEEP_FOLLOWING_LINE")
        pdf.save()
        writer = PdfWriter(clone_from=PdfReader(io.BytesIO(raw.getvalue())))
        page = writer.pages[0]
        content = ContentStream(page.get_contents(), writer)
        if name == "TJ-spacing":
            for i, (operands, op) in enumerate(content.operations):
                if op == b"Tj" and len(operands[0].original_bytes) > 12:
                    data = operands[0].original_bytes
                    content.operations[i] = (
                        [
                            ArrayObject(
                                [
                                    ByteStringObject(data[:3]),
                                    NumberObject(125),
                                    ByteStringObject(data[3:]),
                                ]
                            )
                        ],
                        b"TJ",
                    )
        if name == "identity-H":
            fonts = page["/Resources"]["/Font"]
            for font_name, reference in list(fonts.items()):
                font = reference.get_object()
                if "/ToUnicode" not in font:
                    continue
                program = FontProgram(
                    io.BytesIO(font["/FontDescriptor"]["/FontFile2"].get_data())
                )
                mappings = re.findall(
                    rb"<([0-9A-Fa-f]{2})>\s*<([0-9A-Fa-f]{4})>",
                    font["/ToUnicode"].get_data(),
                )
                cmap = b"/CIDInit /ProcSet findresource begin 12 dict begin begincmap\n/CMapName /Partial def /CMapType 2 def\n1 begincodespacerange <0000> <FFFF> endcodespacerange\n"
                cmap += b"".join(
                    b"1 beginbfchar <00" + code + b"> <" + value + b"> endbfchar\n"
                    for code, value in mappings
                )
                cmap += (
                    b"endcmap CMapName currentdict /CMap defineresource pop end end\n"
                )
                mapping = bytearray(512)
                for code, _ in mappings:
                    old = int(code, 16)
                    glyph_name = program["cmap"].tables[0].cmap.get(old)
                    if glyph_name:
                        mapping[old * 2 : old * 2 + 2] = program.getGlyphID(
                            glyph_name
                        ).to_bytes(2, "big")
                cid = DictionaryObject(
                    {
                        NameObject("/Type"): NameObject("/Font"),
                        NameObject("/Subtype"): NameObject("/CIDFontType2"),
                        NameObject("/BaseFont"): font["/BaseFont"],
                        NameObject("/CIDSystemInfo"): DictionaryObject(
                            {
                                NameObject("/Registry"): ByteStringObject(b"Adobe"),
                                NameObject("/Ordering"): ByteStringObject(b"Identity"),
                                NameObject("/Supplement"): NumberObject(0),
                            }
                        ),
                        NameObject("/FontDescriptor"): font.raw_get("/FontDescriptor"),
                        NameObject("/W"): ArrayObject(
                            [font["/FirstChar"], font["/Widths"]]
                        ),
                        NameObject("/CIDToGIDMap"): writer._add_object(
                            stream(bytes(mapping))
                        ),
                    }
                )
                fonts[font_name] = writer._add_object(
                    DictionaryObject(
                        {
                            NameObject("/Type"): NameObject("/Font"),
                            NameObject("/Subtype"): NameObject("/Type0"),
                            NameObject("/BaseFont"): font["/BaseFont"],
                            NameObject("/Encoding"): NameObject("/Identity-H"),
                            NameObject("/DescendantFonts"): ArrayObject(
                                [writer._add_object(cid)]
                            ),
                            NameObject("/ToUnicode"): writer._add_object(stream(cmap)),
                        }
                    )
                )
                current = None
                for operands, op in content.operations:
                    if op == b"Tf":
                        current = operands[0]
                    if op == b"Tj" and current == font_name:
                        operands[0] = ByteStringObject(
                            b"".join(
                                bytes([0, code]) for code in operands[0].original_bytes
                            )
                        )
        page[NameObject("/Contents")] = writer._add_object(stream(content.get_data()))
        if name in ("form", "nested-form", "scan-OCR"):
            resources = page["/Resources"]
            form = stream(
                content.get_data(),
                Type=NameObject("/XObject"),
                Subtype=NameObject("/Form"),
                BBox=ArrayObject(
                    [
                        NumberObject(0),
                        NumberObject(0),
                        NumberObject(612),
                        NumberObject(792),
                    ]
                ),
                Resources=resources,
            )
            if name == "nested-form":
                inner = writer._add_object(form)
                form = stream(
                    b"/Inner Do\n",
                    Type=NameObject("/XObject"),
                    Subtype=NameObject("/Form"),
                    BBox=form["/BBox"],
                    Resources=DictionaryObject(
                        {
                            NameObject("/XObject"): DictionaryObject(
                                {NameObject("/Inner"): inner}
                            )
                        }
                    ),
                )
            objects = DictionaryObject(
                {NameObject("/TextForm"): writer._add_object(form)}
            )
            page[NameObject("/Resources")] = DictionaryObject(
                {NameObject("/XObject"): objects}
            )
            commands = b"/TextForm Do\n"
            if name == "scan-OCR":
                # A genuine bitmap and an already present invisible OCR layer.
                visible = content.get_data().replace(b"3 Tr", b"0 Tr")
                visual_writer = PdfWriter(
                    clone_from=PdfReader(io.BytesIO(raw.getvalue()))
                )
                visual_writer.pages[0][NameObject("/Contents")] = (
                    visual_writer._add_object(stream(visible))
                )
                visual = io.BytesIO()
                visual_writer.write(visual)
                with pdfium.PdfDocument(visual.getvalue()) as view:
                    image = view[0].render(scale=2).to_pil().convert("RGB")
                    pixels = image.tobytes()
                    image_stream = stream(
                        pixels,
                        Type=NameObject("/XObject"),
                        Subtype=NameObject("/Image"),
                        Width=NumberObject(image.width),
                        Height=NumberObject(image.height),
                        BitsPerComponent=NumberObject(8),
                        ColorSpace=NameObject("/DeviceRGB"),
                    )
                    objects[NameObject("/Scan")] = writer._add_object(
                        image_stream.flate_encode()
                    )
                commands = b"q 612 0 0 792 0 0 cm /Scan Do Q\n" + commands
            page[NameObject("/Contents")] = writer._add_object(stream(commands))
        if name.startswith("rotation-"):
            page[NameObject("/Rotate")] = NumberObject(int(name.split("-")[1]))
            page[NameObject("/CropBox")] = ArrayObject(
                [
                    NumberObject(12),
                    NumberObject(18),
                    NumberObject(600),
                    NumberObject(780),
                ]
            )
            page[NameObject("/UserUnit")] = FloatObject(1.5)
        path = args.output / (name + ".pdf")
        writer.write(path)
        target = boxes(path, remove)
        # The gaps contain spaces; include complete target glyphs, never widen
        # the criterion in response to a product result.
        x0, y0 = min(b[0] for b in target) - 1, min(b[1] for b in target) - 1
        x1, y1 = max(b[2] for b in target) + 1, max(b[3] for b in target) + 1
        rows.append(
            {
                "file": path.name,
                "sha256": sha(path.read_bytes()),
                "remove": remove,
                "keep": [left, right, "KEEP_FOLLOWING_LINE"],
                "rect": [x0, y0, x1 - x0, y1 - y0],
                "boxes": {
                    term: boxes(path, term)
                    for term in (left, right, "KEEP_FOLLOWING_LINE")
                },
                "max_position_error_pt": 0.5,
            }
        )
    criteria = {
        "version": 1,
        "cases": rows,
        "all_source_fonts_and_maps_absent": True,
        "removed_unicode_not_in_rebuilt_fonts": "秘匿機密亀鶴",
        "native_GUI": "未実行",
    }
    (args.output / "criteria.json").write_text(
        json.dumps(criteria, ensure_ascii=False, indent=2) + "\n", encoding="utf-8"
    )
    print(
        json.dumps(
            {
                "cases": len(rows),
                "criteria_sha256": sha((args.output / "criteria.json").read_bytes()),
            }
        )
    )


if __name__ == "__main__":
    main()
