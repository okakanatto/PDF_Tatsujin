"""Freeze synthetic existing-body text edits before running them."""

import argparse
import hashlib
import io
import json
from pathlib import Path
from reportlab.pdfgen import canvas
from reportlab.pdfbase import pdfmetrics
from reportlab.pdfbase.ttfonts import TTFont
from pypdf import PdfReader, PdfWriter
from pypdf.generic import DecodedStreamObject, DictionaryObject, NameObject


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--visible", action="store_true")
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    args.output.mkdir(exist_ok=False, parents=True)
    font = root / "fixtures/NotoSansJP-fixture.ttf"
    pdfmetrics.registerFont(TTFont("BodyFixture", str(font)))
    buffer = io.BytesIO()
    c = canvas.Canvas(buffer, pagesize=(612, 792), pageCompression=0, invariant=1)
    c.setFont("BodyFixture", 16)
    c.drawString(60, 500, "日本語の本文を編集します")
    c.save()
    japanese = PdfReader(buffer).pages[0]
    resources = japanese["/Resources"].get_object()
    jp = next(
        key
        for key, value in resources["/Font"].items()
        if value.get_object().get("/Subtype") == "/TrueType"
    )
    # ReportLab's generated string bytes and subset are kept; remove its trailing line advance.
    data = japanese.get_contents().get_data()
    start = data.index(b"BT 1 0 0 1 60 500 Tm")
    japanese_block = data[start : data.index(b"ET", start) + 2].replace(b" T*", b"")
    writer = PdfWriter()
    source = root / "fixtures/existing-image-edit/shared-images.pdf"
    writer.clone_document_from_reader(PdfReader(source))
    page = writer.pages[0]
    page_resources = DictionaryObject(page["/Resources"].get_object())
    fonts = DictionaryObject(page_resources["/Font"].get_object())
    fonts[NameObject(jp)] = resources["/Font"][jp].clone(writer)
    page_resources[NameObject("/Font")] = fonts
    page[NameObject("/Resources")] = page_resources
    content = page.get_contents().get_data() + b"\n"
    english = b"BT /F1 16 Tf 1 0 0 1 60 700 Tm (Original body line) Tj ET\n"
    # Discover the preserved Helvetica resource rather than inventing a name.
    helvetica = next(
        key
        for key, value in fonts.items()
        if value.get_object().get("/BaseFont") == "/Helvetica"
    )
    english = english.replace(b"/F1", helvetica.encode("ascii"))
    if args.visible:
        english = english.replace(b"BT ", b"BT 0 Tr ", 1)
        japanese_block = japanese_block.replace(b"BT ", b"BT 0 Tr ", 1)
    stream = DecodedStreamObject()
    stream.set_data(content + english + japanese_block + b"\n")
    page[NameObject("/Contents")] = writer._add_object(stream)
    path = args.output / "body-text.pdf"
    with path.open("xb") as file:
        writer.write(file)
    result = dict(
        file=path.name,
        sha256=hashlib.sha256(path.read_bytes()).hexdigest(),
        source_inputs={
            source.relative_to(root)
            .as_posix(): hashlib.sha256(source.read_bytes())
            .hexdigest(),
            font.relative_to(root)
            .as_posix(): hashlib.sha256(font.read_bytes())
            .hexdigest(),
        },
        replacements={
            "Original body line": "Edited body line",
            "日本語の本文を編集します": "本文の日本語を編集します",
        },
        move_physical_rectangle=[160, 120, 300, 30],
        keep_fields={
            "keep-field": "KEEP_FORM_9df67",
            "secret-field": "SECRET_FORM_9df67",
        },
        required=[
            "Source immutable",
            "Only selected text block changes",
            "Unselected text and character positions unchanged",
            "Forms/images/page geometry preserved",
            "Undo/Redo, save/reopen/reedit",
            "Unsupported text and characters refuse atomically",
        ],
        scope="Synthetic CC0 text/graphics; embedded Noto font OFL-1.1; fixed before product operations",
        visible_text=args.visible,
        initial_input_retained="The initial source inherits Tr=3 and is retained as an invisible-text refusal. Visible positive input explicitly sets Tr=0 before any positive operation.",
    )
    (args.output / "criteria.json").write_text(
        json.dumps(result, ensure_ascii=False, indent=2), encoding="utf-8"
    )
    print(json.dumps(result, ensure_ascii=False))


if __name__ == "__main__":
    main()
