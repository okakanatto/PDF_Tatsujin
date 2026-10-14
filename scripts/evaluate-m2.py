"""Independent semantic and rendering checks for M2 saved PDFs.

Expected values and standard annotation types are fixed by the C++ test inputs.
This is not a native Reader/Firefox UI or physical printer test.
"""

import argparse
import hashlib
import json
from pathlib import Path
import subprocess

from PIL import Image, ImageChops
from pypdf import PdfReader
import pypdfium2 as pdfium
from test_tools import poppler_tool


def check(condition, message):
    if not condition:
        raise RuntimeError(message)


def field_values(reader):
    fields = reader.get_fields() or {}
    return {name: str(field.get("/V", "")) for name, field in fields.items()}


def verify_widgets(reader):
    fields = reader.get_fields() or {}
    rows = []
    for page_index, page in enumerate(reader.pages):
        for reference in page.get("/Annots", []):
            widget = reference.get_object()
            if widget.get("/Subtype") != "/Widget":
                continue
            field = widget
            seen = set()
            while "/T" not in field and "/Parent" in field:
                parent = field["/Parent"]
                check(id(parent) not in seen, "cyclic widget parent")
                seen.add(id(parent))
                field = parent
            name = str(field.get("/T", ""))
            check(name in fields, f"widget missing from canonical field tree: {name}")
            check(
                str(field.get("/V", "")) == str(fields[name].get("/V", "")),
                f"widget/canonical value mismatch: {name}",
            )
            check("/N" in widget.get("/AP", {}), f"missing appearance: {name}")
            rows.append(
                {
                    "page": page_index + 1,
                    "name": name,
                    "value": str(field.get("/V", "")),
                }
            )
    check(rows, "no interactive widgets")
    return rows


def render_pdfium(path, page=0, annotations=True):
    document = pdfium.PdfDocument(path)
    document.init_forms()
    image = (
        document[page]
        .render(scale=1.5, draw_annots=annotations)
        .to_pil()
        .convert("RGB")
    )
    return image


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("evidence", type=Path)
    args = parser.parse_args()
    output = args.evidence.resolve()
    check(not (output / "independent-m2.json").exists(), "preserve existing evidence")
    root = Path(__file__).resolve().parents[1]
    result = {
        "engines": {
            "pdfium": str(pdfium.PDFIUM_INFO),
            "poppler": subprocess.run(
                [str(poppler_tool("pdftoppm")), "-v"], capture_output=True, text=True
            ).stderr.strip(),
        },
        "native_Reader_GUI": "未実行",
        "physical_print": "未実行",
        "files": {},
    }
    expected = {
        "name": "髙橋 香織",
        "notes": "東京都\n申請内容の追記",
        "agree": "/Off",
        "choice": "/B",
        "combo": "Red",
        "list": "West",
    }
    for name in (
        "form-input.pdf",
        "form-reedited.pdf",
        "m2-combined.pdf",
        "m2-combined-reedited.pdf",
    ):
        reader = PdfReader(output / name, strict=True)
        values = field_values(reader)
        if name == "form-input.pdf":
            check(
                values == expected,
                "saved logical form values do not match frozen input",
            )
        elif name == "form-reedited.pdf":
            check(
                values == dict(expected, name="再編集した氏名"),
                "reedited logical values",
            )
        else:
            check(values.get("name") == "髙橋 香織", "combined Japanese form value")
            text = pdfium.PdfDocument(output / name)[0].get_textpage().get_text_range()
            check("図書館" in text, "combined saved OCR independently searchable")
            check(len(reader.pages) == 2, "combined page order/count")
        result["files"][name] = {"values": values, "widgets": verify_widgets(reader)}
    for name in ("pages-merged.pdf", "pages-form-extracted.pdf"):
        path = output / name
        if path.exists():
            reader = PdfReader(path, strict=True)
            check(
                len(reader.get_fields() or {}) == 6,
                "merge/extract lost canonical fields",
            )
            result["files"][name] = {"widgets": verify_widgets(reader)}
    reader = PdfReader(output / "annotations.pdf", strict=True)
    own = [
        reference.get_object()
        for reference in reader.pages[0]["/Annots"]
        if "/Tatsujin" in reference.get_object()
    ]
    check(
        [str(item["/Subtype"]) for item in own]
        == ["/Text", "/Square", "/Line", "/Line", "/Highlight"],
        "standard annotation types",
    )
    check(
        str(own[0]["/Contents"]) == "日本語の確認コメント", "Japanese comment contents"
    )
    check(
        list(map(str, own[3]["/LE"])) == ["/None", "/ClosedArrow"],
        "arrow standard ending",
    )
    check(len(own[4]["/QuadPoints"]) == 16, "two real highlight quadrilaterals")
    check(
        all(int(item["/F"]) & 4 and "/N" in item["/AP"] for item in own),
        "annotation appearance/print flags",
    )
    original = render_pdfium(root / "fixtures/D01.pdf")
    saved = render_pdfium(output / "annotations.pdf")
    # Point strictly inside the frozen rectangle, away from its border.
    interior = (
        round(160 * 1.5),
        round((float(reader.pages[0].mediabox.height) - 325) * 1.5),
    )
    check(
        saved.getpixel(interior) == original.getpixel(interior),
        "PDFium rectangle interior must remain transparent",
    )
    check(
        ImageChops.difference(original, saved).getbbox() is not None,
        "PDFium actually draws saved annotations",
    )
    saved.save(output / "annotations-pdfium.png")
    result["files"]["annotations.pdf"] = {
        "standard_types": 5,
        "Japanese_contents": True,
        "arrow_ending": True,
        "highlight_quads": 2,
        "visible_difference": True,
    }
    for name in (
        "writing.pdf",
        "form-input.pdf",
        "annotations.pdf",
        "annotations-ui.pdf",
        "m2-combined.pdf",
    ):
        path = output / name
        target = output / (path.stem + "-poppler")
        process = subprocess.run(
            [
                str(poppler_tool("pdftoppm")),
                "-png",
                "-r",
                "108",
                "-f",
                "1",
                "-l",
                "1",
                "-singlefile",
                str(path),
                str(target),
            ],
            capture_output=True,
            text=True,
            encoding="utf-8",
            errors="replace",
        )
        check(
            process.returncode == 0 and target.with_suffix(".png").is_file(),
            f"Poppler render {name}",
        )
        result["files"].setdefault(name, {})["poppler_rendered"] = True
        result["files"][name]["poppler_diagnostics"] = process.stderr.strip()
        result["files"][name]["sha256"] = hashlib.sha256(path.read_bytes()).hexdigest()
    result["status"] = "PASS"
    (output / "independent-m2.json").write_text(
        json.dumps(result, ensure_ascii=False, indent=2) + "\n", encoding="utf-8"
    )
    print(json.dumps(result, ensure_ascii=False, indent=2))


if __name__ == "__main__":
    main()
