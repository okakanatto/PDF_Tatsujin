"""Freeze twelve standard Latin faces, leading/middle removal and private data."""

import argparse
import hashlib
import json
from pathlib import Path
import pypdfium2 as pdfium
from pypdf import PdfReader, PdfWriter
from pypdf.generic import DictionaryObject, NameObject, TextStringObject
from prepare_redaction_helpers import stream

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, required=True)
    out = parser.parse_args().output
    out.mkdir(exist_ok=False)
    base = ROOT / "fixtures/redaction-copy/partial-text/visible.pdf"
    assert (
        hashlib.sha256(base.read_bytes()).hexdigest()
        == "f9bacb0ca4b4b19f5bc100e68bce571dbb474b55665ad6dbfb8097e7c9f2fd39"
    )
    rows = []
    families = (
        "Helvetica",
        "Helvetica-Bold",
        "Helvetica-Oblique",
        "Helvetica-BoldOblique",
        "Times-Roman",
        "Times-Bold",
        "Times-Italic",
        "Times-BoldItalic",
        "Courier",
        "Courier-Bold",
        "Courier-Oblique",
        "Courier-BoldOblique",
    )
    for family in families:
        for leading in (False, True):
            writer = PdfWriter(clone_from=PdfReader(base))
            page = writer.pages[0]
            font = writer._add_object(
                DictionaryObject(
                    {
                        NameObject("/Type"): NameObject("/Font"),
                        NameObject("/Subtype"): NameObject("/Type1"),
                        NameObject("/BaseFont"): NameObject("/" + family),
                        NameObject("/Encoding"): NameObject("/WinAnsiEncoding"),
                        NameObject("/PrivateAuditPayload"): TextStringObject(
                            "FONT_FAMILY_SECRET_20261012"
                        ),
                    }
                )
            )
            page["/Resources"]["/Font"][NameObject("/Family")] = font
            extra = (
                b"BT /Family 17.25 Tf 1 0 0 1 60.25 430.125 Tm ("
                + (b"" if leading else b"PUBLIC_LEFT ")
                + b"SECRET_ALIAS PUBLIC_FORM) Tj ET\n"
            )
            page[NameObject("/Contents")] = writer._add_object(
                stream(page.get_contents().get_data() + extra)
            )
            name = family + ("-leading" if leading else "-middle")
            path = out / (name + ".pdf")
            writer.write(path)
            keep = [
                "公開",
                "秘匿機密亀鶴",
                "文字保持",
                "KEEP_FOLLOWING_LINE",
                "PUBLIC_FORM",
            ] + ([] if leading else ["PUBLIC_LEFT"])
            with pdfium.PdfDocument(path) as document:
                text = document[0].get_textpage()
                content = text.get_text_range()
                boxes = {}
                for term in keep + ["SECRET_ALIAS"]:
                    assert content.count(term) == 1, (name, term, content)
                    at = content.index(term)
                    boxes[term] = [
                        list(text.get_charbox(i)) for i in range(at, at + len(term))
                    ]
            selected = boxes.pop("SECRET_ALIAS")
            x0, y0 = min(b[0] for b in selected) - 1, min(b[1] for b in selected) - 1
            x1, y1 = max(b[2] for b in selected) + 1, max(b[3] for b in selected) + 1
            rows.append(
                dict(
                    file=path.name,
                    sha256=hashlib.sha256(path.read_bytes()).hexdigest(),
                    refused=False,
                    remove="SECRET_ALIAS",
                    keep=keep,
                    second_page_terms=[],
                    rect=[x0, y0, x1 - x0, y1 - y0],
                    boxes=boxes,
                    max_position_error_pt=0.5,
                    private_marker="FONT_FAMILY_SECRET_20261012",
                    family=family,
                    leading=leading,
                )
            )
    criteria = out / "criteria.json"
    criteria.write_text(
        json.dumps(
            dict(
                cases=rows,
                raster_dpi=300,
                outside_changed_pixels=0,
                boundary_px=1,
                scope="Twelve Latin standard Type1 faces; leading/middle, fractional origin and size; preserve text/paint and remove private data",
            ),
            ensure_ascii=False,
            indent=2,
        )
        + "\n",
        encoding="utf-8",
    )
    print(
        json.dumps(
            dict(
                cases=len(rows),
                criteria_sha256=hashlib.sha256(criteria.read_bytes()).hexdigest(),
            )
        )
    )


if __name__ == "__main__":
    main()
