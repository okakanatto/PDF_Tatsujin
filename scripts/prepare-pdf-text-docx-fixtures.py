"""Freeze supplementary editable Word text inputs before implementation."""

import hashlib
import json
from pathlib import Path

from pypdf import PdfReader, PdfWriter
from pypdf.constants import UserAccessPermissions
from reportlab.pdfbase import pdfmetrics
from reportlab.pdfbase.ttfonts import TTFont
from reportlab.pdfgen import canvas

ROOT = Path(__file__).resolve().parents[1]
OUTPUT = ROOT / "fixtures/pdf-text-docx"
PAGES = [
    [
        "本文をWordへ取り出します。",
        "English text remains editable.",
        "00123 & <文字>",
        "2行目：順序と改行を確認。",
    ],
    ['Second page / quotes: "hello".', "日本語と英語の本文を保持。", "Last line: 0007"],
]


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    OUTPUT.mkdir(exist_ok=False)
    font = ROOT / "fixtures/NotoSansJP-fixture.ttf"
    assert (
        sha(font) == "0ef6019ca22fb4206a64a5c4ce326855afd2bc945cb42324816ec84d471433e3"
    )
    pdfmetrics.registerFont(TTFont("TatsujinWordFixture", str(font)))
    source = OUTPUT / "two-pages.pdf"
    document = canvas.Canvas(str(source), pagesize=(595.276, 841.89), invariant=True)
    for lines in PAGES:
        document.setFont("TatsujinWordFixture", 14)
        for index, line in enumerate(lines):
            document.drawString(50, 760 - index * 30, line)
        document.showPage()
    document.save()
    scan = OUTPUT / "image-only.pdf"
    writer = PdfWriter()
    writer.add_page(PdfReader(ROOT / "fixtures/D03.pdf").pages[0])
    with scan.open("xb") as target:
        writer.write(target)
    restricted = OUTPUT / "copy-restricted.pdf"
    writer = PdfWriter(clone_from=source)
    writer.encrypt(
        "",
        "fixture-owner",
        algorithm="AES-256",
        permissions_flag=UserAccessPermissions.PRINT,
    )
    with restricted.open("xb") as target:
        writer.write(target)
    result = dict(
        purpose="Supplementary PDF text to editable DOCX; no existing acceptance changes",
        expected_pages=PAGES,
        source="two-pages.pdf",
        source_sha256=sha(source),
        image_only="image-only.pdf",
        image_only_sha256=sha(scan),
        copy_restricted="copy-restricted.pdf",
        copy_restricted_sha256=sha(restricted),
        generator_font_sha256=sha(font),
        criteria=dict(
            all_text_exact=True,
            explicit_page_breaks=1,
            editable_paragraphs=True,
            existing_destination_unchanged=True,
            original_document_state_unchanged=True,
        ),
        license="CC0-1.0 original synthetic content; embedded Noto font retains SIL OFL 1.1",
    )
    (OUTPUT / "criteria.json").write_text(
        json.dumps(result, ensure_ascii=False, indent=2) + "\n", "utf-8"
    )
    print("Fixed supplementary criterion SHA-256: " + sha(OUTPUT / "criteria.json"))


if __name__ == "__main__":
    main()
