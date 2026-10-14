"""Fix editable-table expectations before implementing extraction."""

import hashlib
import json
from pathlib import Path

from pypdf import PdfReader, PdfWriter
from pypdf.generic import FloatObject, NameObject
from reportlab.pdfbase import pdfmetrics
from reportlab.pdfbase.ttfonts import TTFont
from reportlab.pdfgen.canvas import Canvas

ROOT = Path(__file__).resolve().parents[1]
DIRECTORY = ROOT / "fixtures/table-extraction"
FONT = ROOT / "assets/fonts/TatsujinSansJP-Regular.ttf"


def main():
    DIRECTORY.mkdir(exist_ok=False)
    pdfmetrics.registerFont(TTFont("TableFixture", str(FONT)))
    cells = [
        ["品目", "番号", "備考"],
        ["項目一", "0012", "=1+2"],
        ["二行\n改行", "", "English text"],
        ["項目二", "30", "合計"],
    ]
    path = DIRECTORY / "table.pdf"
    canvas = Canvas(str(path), pagesize=(400, 300), invariant=1, pageCompression=1)
    canvas.setFont("TableFixture", 11)
    for row, values in enumerate(cells):
        for column, value in enumerate(values):
            for line, text in enumerate(value.split("\n")):
                canvas.drawString(48 + column * 100, 218 - row * 35 - line * 12, text)
    for x in (40, 140, 240, 340):
        canvas.line(x, 90, x, 230)
    for y in (90, 125, 160, 195, 230):
        canvas.line(40, y, 340, y)
    canvas.showPage()
    canvas.save()
    reader = PdfReader(path)
    writer = PdfWriter()
    page = writer.add_page(reader.pages[0])
    page.cropbox.lower_left = (10, 20)
    page.cropbox.upper_right = (390, 290)
    page.rotate(90)
    page[NameObject("/UserUnit")] = FloatObject(1.5)
    writer.write(DIRECTORY / "rotated-crop-unit.pdf")
    canvas = Canvas(str(DIRECTORY / "image-only.pdf"), pagesize=(400, 300), invariant=1)
    canvas.drawImage(
        str(ROOT / "fixtures/office-import/sheets-slides/fixture.png"),
        40,
        90,
        width=300,
        height=140,
    )
    canvas.showPage()
    canvas.save()
    criterion = {
        "provenance": "Synthetic text/grid CC0-1.0; embedded modified Noto font retains OFL notices.",
        "font": {
            "file": FONT.relative_to(ROOT).as_posix(),
            "sha256": hashlib.sha256(FONT.read_bytes()).hexdigest(),
        },
        "expected_cells": cells,
        "nonempty_cell_type": "string; formula-like text must never become a formula",
        "blank_cell": "empty string or absent value; position retained",
        "line_rule": "Trim leading/trailing positioning whitespace per line; keep internal spaces and line breaks",
        "cases": [
            {"file": "table.pdf", "region_pt": [40, 70, 300, 140]},
            {
                "file": "rotated-crop-unit.pdf",
                "region_pt": [45, 90, 450, 210],
                "page_rotation": 90,
                "UserUnit": 1.5,
            },
        ],
        "row_boundaries": [0, 0.25, 0.5, 0.75, 1],
        "column_boundaries": [0, 1 / 3, 2 / 3, 1],
        "refused": "image-only.pdf; boundary crossing; invalid grid; existing output; cancelled output",
        "files": {
            p.name: hashlib.sha256(p.read_bytes()).hexdigest()
            for p in sorted(DIRECTORY.glob("*.pdf"))
        },
    }
    with (DIRECTORY / "criteria.json").open("x", encoding="utf-8") as file:
        json.dump(criterion, file, ensure_ascii=False, indent=2)
    print(hashlib.sha256((DIRECTORY / "criteria.json").read_bytes()).hexdigest())


if __name__ == "__main__":
    main()
