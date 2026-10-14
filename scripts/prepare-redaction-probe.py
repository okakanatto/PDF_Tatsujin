"""Create synthetic, explicitly unsafe fixtures for redaction feasibility tests."""

import argparse
import hashlib
import io
import json
from pathlib import Path

from PIL import Image, ImageDraw
from pypdf import PdfReader, PdfWriter
from pypdf.generic import (
    ArrayObject,
    DecodedStreamObject,
    DictionaryObject,
    FloatObject,
    NameObject,
    NumberObject,
    TextStringObject,
)
from reportlab.lib.utils import ImageReader
from reportlab.pdfgen import canvas


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=False)
    image = Image.new("RGB", (240, 120), (222, 174, 95))
    draw = ImageDraw.Draw(image)
    draw.rectangle((40, 25, 160, 85), fill=(30, 180, 210))
    draw.text((42, 48), "SECRET_IMAGE_9df67", fill=(0, 0, 0))
    image.save(args.output / "secret-image.png")
    buffer = io.BytesIO()
    pdf = canvas.Canvas(buffer, pagesize=(612, 792), pageCompression=0)
    pdf.setFont("Helvetica", 16)
    pdf.drawString(60, 730, "KEEP_VISIBLE_9df67")
    pdf.drawString(60, 650, "SECRET_TEXT_9df67")
    text = pdf.beginText(60, 570)
    text.setFont("Helvetica", 14)
    text.setTextRenderMode(3)
    text.textOut("SECRET_HIDDEN_9df67")
    pdf.drawText(text)
    pdf.drawImage(ImageReader(image), 60, 250, width=240, height=120)
    pdf.acroForm.textfield(
        name="keep-field", value="KEEP_FORM_9df67", x=60, y=420, width=220, height=25
    )
    pdf.acroForm.textfield(
        name="secret-field",
        value="SECRET_FORM_9df67",
        x=60,
        y=465,
        width=220,
        height=25,
    )
    pdf.bookmarkPage("first")
    pdf.addOutlineEntry("KEEP_OUTLINE_9df67", "first")
    pdf.showPage()
    pdf.setFont("Helvetica", 16)
    pdf.drawString(60, 650, "KEEP_SECOND_PAGE_9df67")
    pdf.showPage()
    pdf.save()
    writer = PdfWriter()
    writer.clone_document_from_reader(PdfReader(buffer))
    writer.add_metadata({"/Title": "SECRET_META_9df67"})
    metadata = DecodedStreamObject()
    metadata.set_data(
        b'<x:xmpmeta xmlns:x="adobe:ns:meta/">SECRET_XMP_9df67</x:xmpmeta>'
    )
    metadata[NameObject("/Type")] = NameObject("/Metadata")
    metadata[NameObject("/Subtype")] = NameObject("/XML")
    writer._root_object[NameObject("/Metadata")] = writer._add_object(metadata)
    writer.add_attachment("synthetic-secret.txt", b"SECRET_ATTACH_9df67")
    writer._add_object(
        DictionaryObject(
            {NameObject("/OrphanSecret"): TextStringObject("SECRET_ORPHAN_9df67")}
        )
    )
    writer.pages[0][NameObject("/UserUnit")] = FloatObject(2)
    writer.pages[1][NameObject("/UserUnit")] = FloatObject(1.5)
    writer.pages[1][NameObject("/MediaBox")] = ArrayObject(
        [NumberObject(v) for v in (-40, -25, 572, 767)]
    )
    writer.pages[1][NameObject("/CropBox")] = ArrayObject(
        [NumberObject(v) for v in (-20, -5, 552, 747)]
    )
    writer.pages[1][NameObject("/Rotate")] = NumberObject(90)
    source = args.output / "unsafe-source.pdf"
    writer.write(source)
    regions = [
        {"page": 0, "rect": [48, 620, 420, 65], "target": "visible text"},
        {"page": 0, "rect": [48, 540, 420, 65], "target": "invisible OCR text"},
        {"page": 0, "rect": [100, 280, 80, 40], "target": "image interior"},
        {
            "page": 0,
            "rect": [48, 458, 240, 40],
            "target": "form widget value/default/appearance",
        },
    ]
    (args.output / "plan.json").write_text(
        json.dumps(regions, indent=2), encoding="utf-8"
    )
    criteria = {
        "scope": "Frozen before execution; synthetic fixtures only, no product acceptance declaration",
        "source_sha256": hashlib.sha256(source.read_bytes()).hexdigest(),
        "required": [
            "selected visible and invisible text removed",
            "selected image pixels absent from extracted images",
            "selected field value/default/appearance removed",
            "unselected searchable text and form remain usable",
            "MediaBox/CropBox/Rotate/UserUnit preserved",
            "metadata/attachments/unreachable payload absent",
            "source unchanged",
        ],
        "not_acceptable": [
            "opaque overlay only",
            "rasterizing ordinary saves",
            "outlining all remaining text",
            "dropping unaffected forms or annotations",
            "claiming execution is security acceptance",
        ],
    }
    (args.output / "criteria.json").write_text(
        json.dumps(criteria, indent=2), encoding="utf-8"
    )
    print(
        json.dumps(
            {"source_sha256": criteria["source_sha256"], "bytes": source.stat().st_size}
        )
    )


if __name__ == "__main__":
    main()
