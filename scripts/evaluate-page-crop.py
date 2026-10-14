"""Verify crop-only edits preserve PDF semantics and independent rendering."""

import argparse
import hashlib
import json
from pathlib import Path
import subprocess

from PIL import Image
from pypdf import PdfReader
import pypdfium2 as pdfium

from test_tools import poppler_tool


def check(value, message):
    if not value:
        raise RuntimeError(message)


def annotations(page):
    result = []
    for reference in page.get("/Annots", []):
        value = reference.get_object()
        result.append(
            {
                "type": str(value.get("/Subtype")),
                "rectangle": list(value.get("/Rect", [])),
                "contents": str(value.get("/Contents", "")),
                "metadata": (
                    bytes(value["/Tatsujin"].original_bytes).hex()
                    if "/Tatsujin" in value
                    else None
                ),
            }
        )
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("directory", type=Path)
    args = parser.parse_args()
    directory = args.directory.resolve()
    output = directory / "independent-page-crop.json"
    check(not output.exists(), "Preserve earlier crop verification")
    process = json.loads((directory / "process-result.json").read_text("utf-8-sig"))
    suite = json.loads((directory / "selftest.json").read_text("utf-8-sig"))
    cases = [row for row in suite["tests"] if row["name"].startswith("M4C")]
    check(
        process["completed"]
        and process["report_valid"]
        and process["exit_code"] == 0
        and len(cases) == 3
        and all(row["status"] == "PASS" for row in cases),
        "Three crop cases must pass",
    )
    result = {"status": "PASS", "executable_sha256": process["exe_sha256"], "files": {}}
    for before_name, after_name, margins in [
        ("crop-before.pdf", "crop-after.pdf", (4, 1, 2, 3)),
        ("crop-form-before.pdf", "crop-form.pdf", (1, 1, 1, 1)),
    ]:
        before = PdfReader(directory / before_name)
        after = PdfReader(directory / after_name)
        check(len(before.pages) == len(after.pages), "No crop page deleted")
        old_fields, new_fields = before.get_fields() or {}, after.get_fields() or {}
        old_values = {
            name: str(value.get("/V", "")) for name, value in old_fields.items()
        }
        new_values = {
            name: str(value.get("/V", "")) for name, value in new_fields.items()
        }
        check(old_values == new_values, "Canonical form fields and values preserved")
        if old_values:
            check(
                new_values.get("name") == "髙橋 香織", "Japanese form value preserved"
            )
        rows = []
        rendered = pdfium.PdfDocument(str(directory / after_name))
        try:
            for index, (old, page) in enumerate(zip(before.pages, after.pages)):
                check(list(old.mediabox) == list(page.mediabox), "MediaBox preserved")
                check(
                    old.get("/Rotate", 0) == page.get("/Rotate", 0)
                    and old.get("/UserUnit", 1) == page.get("/UserUnit", 1),
                    "Rotation/UserUnit preserved",
                )
                old_contents, contents = old.get_contents(), page.get_contents()
                old_bytes = old_contents.get_data() if old_contents is not None else b""
                content_bytes = contents.get_data() if contents is not None else b""
                check(
                    old_bytes and old_bytes == content_bytes,
                    "Nonempty page content stream preserved, no flattening",
                )
                check(
                    old.extract_text() == page.extract_text(), "Original text retained"
                )
                check(
                    annotations(old) == annotations(page),
                    "Annotation coordinates/metadata preserved",
                )
                unit = float(page.get("/UserUnit", 1))
                factor = 72 / 25.4 / unit
                left, top, right, bottom = (value * factor for value in margins)
                angle = int(page.get("/Rotate", 0)) % 360
                x0, y0, x1, y1 = (float(value) for value in old.cropbox)
                expected = {
                    0: (x0 + left, y0 + bottom, x1 - right, y1 - top),
                    90: (x0 + top, y0 + left, x1 - bottom, y1 - right),
                    180: (x0 + right, y0 + top, x1 - left, y1 - bottom),
                    270: (x0 + bottom, y0 + right, x1 - top, y1 - left),
                }[angle]
                error = max(abs(float(a) - b) for a, b in zip(page.cropbox, expected))
                check(error <= 0.000001, "Physical rotated margins are wrong")
                pdf_page = rendered[index]
                bitmap = pdf_page.render(scale=1, draw_annots=True)
                try:
                    image = bitmap.to_pil()
                    check(
                        image.convert("L").getextrema()[0] < 200,
                        "Independent PDFium crop/signature is blank",
                    )
                    image.save(directory / f"{after_name[:-4]}-pdfium-{index + 1}.png")
                finally:
                    bitmap.close()
                    pdf_page.close()
                prefix = directory / f"{after_name[:-4]}-poppler-{index + 1}"
                subprocess.run(
                    [
                        str(poppler_tool("pdftoppm")),
                        "-cropbox",
                        "-r",
                        str(72 * unit),
                        "-f",
                        str(index + 1),
                        "-l",
                        str(index + 1),
                        "-singlefile",
                        "-png",
                        str(directory / after_name),
                        str(prefix),
                    ],
                    check=True,
                    capture_output=True,
                    creationflags=subprocess.CREATE_NO_WINDOW,
                )
                with Image.open(prefix.with_suffix(".png")) as image:
                    check(
                        image.convert("L").getextrema()[0] < 200,
                        "Independent Poppler crop/signature is blank",
                    )
                    dimensions = [
                        float(page.cropbox.width) * unit,
                        float(page.cropbox.height) * unit,
                    ]
                    if angle % 180:
                        dimensions.reverse()
                    check(
                        all(
                            abs(value - actual) <= 1
                            for value, actual in zip(dimensions, image.size)
                        ),
                        "Independent visible physical size is wrong",
                    )
                rows.append(
                    {
                        "page": index + 1,
                        "rotation": angle,
                        "UserUnit": unit,
                        "CropBox": list(page.cropbox),
                        "max_error_pt": error,
                        "content_sha256": hashlib.sha256(content_bytes).hexdigest(),
                        "content_bytes": len(content_bytes),
                        "annotations_preserved": len(annotations(page)),
                        "PDFium_Poppler": "PASS",
                        "status": "PASS",
                    }
                )
        finally:
            rendered.close()
        result["files"][after_name] = {
            "pages": rows,
            "form_values": new_values,
            "status": "PASS",
        }
    result["scope"] = (
        "Independent pypdf dictionaries/content/text/forms, PDFium and Poppler rendering. Not native Reader GUI or physical printing. Cropping is not redaction."
    )
    output.write_text(
        json.dumps(result, ensure_ascii=False, indent=2) + "\n", encoding="utf-8"
    )
    print(
        "PASS: physical crop boxes, text/content/annotations/forms preserved, PDFium and Poppler render"
    )


if __name__ == "__main__":
    main()
