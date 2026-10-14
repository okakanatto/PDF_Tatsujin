"""Freeze standard-font private-data conditions before implementation trials."""

import argparse
import copy
import hashlib
import json
from pathlib import Path

import pypdfium2 as pdfium
from pypdf import PdfReader, PdfWriter
from pypdf.generic import (
    ArrayObject,
    DictionaryObject,
    NameObject,
    NumberObject,
    TextStringObject,
)
from prepare_redaction_helpers import stream

ROOT = Path(__file__).resolve().parents[1]
BASE = ROOT / "fixtures/redaction-copy/partial-text/visible.pdf"
MARKER = "FONT_PRIVATE_SECRET_20261012"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, required=True)
    out = parser.parse_args().output
    out.mkdir(exist_ok=False)
    assert (
        hashlib.sha256(BASE.read_bytes()).hexdigest()
        == "f9bacb0ca4b4b19f5bc100e68bce571dbb474b55665ad6dbfb8097e7c9f2fd39"
    )
    rows = []
    names = (
        "plain-control",
        "private-string",
        "private-direct",
        "private-stream",
        "shared-page",
        "shared-form",
        "duplicate-stream",
        "to-unicode-refused",
        "encoding-dictionary-refused",
        "descriptor-refused",
        "outside-reference-refused",
    )
    for name in names:
        writer = PdfWriter(clone_from=PdfReader(BASE))
        page = writer.pages[0]
        resources = page["/Resources"]
        font = resources["/Font"]["/F1"]
        if name in ("private-stream", "duplicate-stream"):
            private = writer._add_object(stream(MARKER.encode()))
        else:
            private = TextStringObject(MARKER)
        if name != "plain-control":
            font[NameObject("/PrivateAuditPayload")] = private
        if name == "private-direct":
            resources["/Font"][NameObject("/F1")] = copy.copy(font)
        if name == "to-unicode-refused":
            mapping = b"\n".join(
                f"<{code:02X}> <{code:04X}>".encode() for code in range(32, 127)
            )
            font[NameObject("/ToUnicode")] = writer._add_object(
                stream(
                    b"/CIDInit /ProcSet findresource begin 12 dict begin begincmap\n"
                    b"/CIDSystemInfo << /Registry (Adobe) /Ordering (UCS) /Supplement 0 >> def\n"
                    b"/CMapName /PrivateMapping def /CMapType 2 def\n"
                    b"1 begincodespacerange <00> <FF> endcodespacerange\n"
                    b"95 beginbfchar\n" + mapping + b"\nendbfchar endcmap\n"
                    b"CMapName currentdict /CMap defineresource pop end end\n"
                )
            )
        if name == "encoding-dictionary-refused":
            font[NameObject("/Encoding")] = writer._add_object(
                DictionaryObject(
                    {
                        NameObject("/Type"): NameObject("/Encoding"),
                        NameObject("/BaseEncoding"): NameObject("/WinAnsiEncoding"),
                        NameObject("/PrivateAuditPayload"): private,
                    }
                )
            )
        if name == "descriptor-refused":
            font[NameObject("/FontDescriptor")] = writer._add_object(
                DictionaryObject(
                    {
                        NameObject("/Type"): NameObject("/FontDescriptor"),
                        NameObject("/FontName"): NameObject("/Helvetica"),
                        NameObject("/Flags"): NumberObject(32),
                        NameObject("/FontBBox"): ArrayObject(
                            [NumberObject(v) for v in (-166, -225, 1000, 931)]
                        ),
                        NameObject("/ItalicAngle"): NumberObject(0),
                        NameObject("/Ascent"): NumberObject(718),
                        NameObject("/Descent"): NumberObject(-207),
                        NameObject("/CapHeight"): NumberObject(718),
                        NameObject("/StemV"): NumberObject(88),
                        NameObject("/PrivateAuditPayload"): private,
                    }
                )
            )
        if name == "outside-reference-refused":
            private = writer._add_object(
                DictionaryObject({NameObject("/Secret"): private})
            )
            font[NameObject("/PrivateAuditPayload")] = private
            writer._root_object[NameObject("/PrivateAuditCopy")] = private
        extra = b"BT /F1 18 Tf 1 0 0 1 60 430 Tm (PUBLIC_LEFT SECRET_ALIAS PUBLIC_FORM) Tj ET\n"
        if name == "shared-form":
            form = writer._add_object(
                stream(
                    b"BT /F1 12 Tf 1 0 0 1 60 330 Tm (KEEP_SHARED_FORM) Tj ET\n",
                    Type=NameObject("/XObject"),
                    Subtype=NameObject("/Form"),
                    BBox=ArrayObject([NumberObject(v) for v in (0, 0, 612, 792)]),
                    Resources=DictionaryObject(
                        {NameObject("/Font"): resources["/Font"]}
                    ),
                )
            )
            resources[NameObject("/XObject")] = DictionaryObject(
                {NameObject("/Keep"): form}
            )
            extra += b"/Keep Do\n"
        page[NameObject("/Contents")] = writer._add_object(
            stream(page.get_contents().get_data() + extra)
        )
        second = name in ("shared-page", "duplicate-stream")
        if second:
            # A distinct Page object is required; adding the same writer page
            # twice produces duplicate Kids references and an invalid page tree.
            writer.add_page(PdfReader(BASE).pages[0])
            other = writer.pages[1]
            other[NameObject("/Contents")] = page.raw_get("/Contents")
            other[NameObject("/Resources")] = copy.copy(resources)
            other["/Resources"][NameObject("/Font")] = copy.copy(resources["/Font"])
            if name == "duplicate-stream":
                copied_font = copy.copy(font)
                copied_font[NameObject("/PrivateAuditPayload")] = writer._add_object(
                    stream(MARKER.encode())
                )
                other["/Resources"]["/Font"][NameObject("/F1")] = writer._add_object(
                    copied_font
                )
        path = out / (name + ".pdf")
        writer.write(path)
        keep = [
            "公開",
            "秘匿機密亀鶴",
            "文字保持",
            "KEEP_FOLLOWING_LINE",
            "PUBLIC_LEFT",
            "PUBLIC_FORM",
        ]
        if name == "shared-form":
            keep.append("KEEP_SHARED_FORM")
        with pdfium.PdfDocument(path) as pdf:
            text = pdf[0].get_textpage()
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
                refused=name.endswith("refused"),
                remove="SECRET_ALIAS",
                keep=keep,
                second_page_terms=keep + ["SECRET_ALIAS"] if second else [],
                rect=[x0, y0, x1 - x0, y1 - y0],
                boxes=boxes,
                max_position_error_pt=0.5,
                private_marker=MARKER if name != "plain-control" else "",
            )
        )
    criteria = out / "criteria.json"
    criteria.write_text(
        json.dumps(
            dict(cases=rows, raster_dpi=300, outside_changed_pixels=0, boundary_px=1),
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
