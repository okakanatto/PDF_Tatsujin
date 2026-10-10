"""Freeze Japanese subset-font redaction inputs before diagnostic execution."""

import argparse
import hashlib
import json
from pathlib import Path

import pypdfium2 as pdfium
from reportlab.pdfbase import pdfmetrics
from reportlab.pdfbase.ttfonts import TTFont
from reportlab.pdfgen import canvas
from pypdf import PdfReader, PdfWriter
from pypdf.generic import NameObject


def phrase_boxes(path, phrase):
    with pdfium.PdfDocument(path) as document:
        page = document[0]
        text = page.get_textpage()
        try:
            contents = text.get_text_range()
            assert contents.count(phrase) == 1, "Unique source phrase required"
            start = contents.index(phrase)
            return [
                list(text.get_charbox(i)) for i in range(start, start + len(phrase))
            ]
        finally:
            text.close()
            page.close()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--with-sharing", action="store_true")
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=False)
    root = Path(__file__).resolve().parents[1]
    font = root / "assets/fonts/NotoSansJP.ttf"
    pdfmetrics.registerFont(TTFont("SyntheticJapanese", str(font)))
    rows = []
    cases = [("visible", 0), ("invisible-OCR", 3)]
    if args.with_sharing:
        cases.append(("surviving-subset-text", 0))
        cases.append(("following-font-switch", 0))
    for name, mode in cases:
        path = args.output / (name + ".pdf")
        pdf = canvas.Canvas(str(path), pagesize=(612, 792))
        pdf.setFont("Helvetica", 16)
        pdf.drawString(60, 730, "KEEP_OUTSIDE_FONT_PROBE")
        text = pdf.beginText(60, 650)
        text.setFont("SyntheticJapanese", 22)
        text.setTextRenderMode(mode)
        text.textOut("秘匿機密亀鶴")
        if name == "following-font-switch":
            text.setFont("Helvetica", 16)
            text.textOut("KEEP_FOLLOW_FONT_PROBE")
        pdf.drawText(text)
        if name == "surviving-subset-text":
            pdf.setFont("SyntheticJapanese", 16)
            pdf.drawString(60, 700, "公開文字を保持")
        pdf.showPage()
        pdf.save()
        row = dict(
            file=path.name,
            sha256=hashlib.sha256(path.read_bytes()).hexdigest(),
            remove="秘匿機密亀鶴",
            keep="KEEP_OUTSIDE_FONT_PROBE",
            mode=mode,
            keep_additional=(
                "公開文字を保持" if name == "surviving-subset-text" else ""
            ),
            expect_rejection=name == "surviving-subset-text",
        )
        if name == "following-font-switch":
            # Fix the rectangle before execution; it ends before the first kept glyph.
            rect = [48, 620, 145, 65]
            target_boxes = phrase_boxes(path, row["remove"])
            kept_boxes = phrase_boxes(path, "KEEP_FOLLOW_FONT_PROBE")
            left, bottom, width, height = rect
            right, top = left + width, bottom + height
            assert all(
                left <= box[0] <= box[2] <= right and bottom <= box[1] <= box[3] <= top
                for box in target_boxes
            ), "Fixed rectangle must contain every target glyph"
            assert all(box[0] > right for box in kept_boxes), "Following glyphs outside"
            plan = "following-font-switch-plan.json"
            (args.output / plan).write_text(
                json.dumps([dict(page=0, rect=rect)]), encoding="utf-8"
            )
            row.update(
                plan=plan,
                keep_additional="KEEP_FOLLOW_FONT_PROBE",
                preserve_position_phrase="KEEP_FOLLOW_FONT_PROBE",
                preserved_character_boxes=kept_boxes,
            )
        rows.append(row)
    if args.with_sharing:
        writer = PdfWriter()
        writer.clone_document_from_reader(PdfReader(args.output / "visible.pdf"))
        for reference in writer.pages[0]["/Resources"]["/Font"].values():
            value = reference.get_object()
            if "/ToUnicode" in value:
                value[NameObject("/BaseFont")] = NameObject(
                    "/SyntheticUntaggedJapanese"
                )
                value["/FontDescriptor"][NameObject("/FontName")] = NameObject(
                    "/SyntheticUntaggedJapanese"
                )
        path = args.output / "untagged-font.pdf"
        writer.write(path)
        rows.append(
            dict(
                file=path.name,
                sha256=hashlib.sha256(path.read_bytes()).hexdigest(),
                remove="秘匿機密亀鶴",
                keep="KEEP_OUTSIDE_FONT_PROBE",
                mode=0,
                expect_rejection=False,
                keep_additional="",
            )
        )
        for name, key in (
            ("shared-ToUnicode", "/ToUnicode"),
            ("shared-font-program", "/FontFile2"),
        ):
            writer = PdfWriter()
            writer.clone_document_from_reader(PdfReader(args.output / "visible.pdf"))
            font_object = next(
                reference.get_object()
                for reference in writer.pages[0]["/Resources"]["/Font"].values()
                if "/ToUnicode" in reference.get_object()
            )
            owner = (
                font_object if key == "/ToUnicode" else font_object["/FontDescriptor"]
            )
            writer._root_object[NameObject("/TatsujinTestFontAlias")] = owner.raw_get(
                key
            )
            path = args.output / (name + ".pdf")
            writer.write(path)
            rows.append(
                dict(
                    file=path.name,
                    sha256=hashlib.sha256(path.read_bytes()).hexdigest(),
                    remove="秘匿機密亀鶴",
                    keep="KEEP_OUTSIDE_FONT_PROBE",
                    mode=0,
                    expect_rejection=True,
                    keep_additional="",
                )
            )
    (args.output / "plan.json").write_text(
        json.dumps([dict(page=0, rect=[48, 620, 420, 65])]), encoding="utf-8"
    )
    (args.output / "criteria.json").write_text(
        json.dumps(
            dict(
                cases=rows,
                font_sha256=hashlib.sha256(font.read_bytes()).hexdigest(),
                required=[
                    "target text absent from visible and invisible text and recoverable subset mappings",
                    "outside text preserved",
                    "source unchanged or unsupported operation rejected without publication",
                ],
                scope="Synthetic font side-channel evaluation; not general security acceptance",
            ),
            ensure_ascii=False,
            indent=2,
        ),
        encoding="utf-8",
    )


if __name__ == "__main__":
    main()
