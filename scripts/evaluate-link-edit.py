"""Independently verify saved Link annotations, PDFium targets and body retention."""

import argparse
import ctypes
import json
from pathlib import Path

from PIL import ImageChops
from pypdf import PdfReader
import pypdfium2 as pdfium
from pypdfium2 import raw

from pypdf.generic import IndirectObject


def check(value, message):
    if not value:
        raise RuntimeError(message)


def canonical(value):
    if isinstance(value, IndirectObject):
        return (value.idnum, value.generation)
    if isinstance(value, dict):
        return {key: canonical(part) for key, part in value.items()}
    if isinstance(value, list):
        return [canonical(part) for part in value]
    return value


def links(page):
    return [
        (i, ref.get_object())
        for i, ref in enumerate(page.get("/Annots", []))
        if ref.get_object().get("/Subtype") == "/Link"
    ]


def other_annotations(page):
    return [
        canonical(ref.get_object())
        for ref in page.get("/Annots", [])
        if ref.get_object().get("/Subtype") != "/Link"
    ]


def pdf_rectangle(page, visible):
    # Independent formulas, not a translation of the application's QTransform.
    left, bottom, right, top = map(float, page.cropbox)
    unit = float(page.get("/UserUnit", 1))
    rotation = int(page.get("/Rotate", 0)) % 360

    def point(x, y):
        if rotation == 0:
            return left + x / unit, top - y / unit
        if rotation == 90:
            return left + y / unit, bottom + x / unit
        if rotation == 180:
            return right - x / unit, bottom + y / unit
        if rotation == 270:
            return right - y / unit, top - x / unit
        raise RuntimeError("Unexpected rotation")

    x, y, width, height = visible
    corners = [point(x, y), point(x + width, y + height)]
    return [
        min(p[0] for p in corners),
        min(p[1] for p in corners),
        max(p[0] for p in corners),
        max(p[1] for p in corners),
    ]


def preserved(before_path, after_path):
    a, b = PdfReader(before_path), PdfReader(after_path)
    check(len(a.pages) == len(b.pages), "Page count changed")
    for old, new in zip(a.pages, b.pages):
        for key in ("/MediaBox", "/CropBox", "/Rotate", "/UserUnit", "/Resources"):
            check(
                canonical(old.get(key)) == canonical(new.get(key)),
                "Page entry changed: " + key,
            )
        first, second = old.get_contents(), new.get_contents()
        check(
            (first is None and second is None)
            or (
                first is not None
                and second is not None
                and first.get_data() == second.get_data()
            ),
            "Original body stream changed/flattened",
        )
        check(
            old.extract_text() == new.extract_text(), "Original searchable text changed"
        )
        check(
            other_annotations(old) == other_annotations(new),
            "Unrelated annotation changed",
        )
    fields = lambda reader: {
        name: str(field.get("/V", ""))
        for name, field in (reader.get_fields() or {}).items()
    }
    check(fields(a) == fields(b), "Form values changed")
    for key in ("/Outlines", "/Names", "/Dests", "/AcroForm"):
        check(
            canonical(a.trailer["/Root"].get(key))
            == canonical(b.trailer["/Root"].get(key)),
            "Catalog entry changed: " + key,
        )
    with pdfium.PdfDocument(str(before_path)) as first, pdfium.PdfDocument(
        str(after_path)
    ) as second:
        for i in range(len(first)):
            old, new = first[i], second[i]
            old_image, new_image = old.render(scale=0.6, draw_annots=False), new.render(
                scale=0.6, draw_annots=False
            )
            check(
                ImageChops.difference(
                    old_image.to_pil().convert("RGB"), new_image.to_pil().convert("RGB")
                ).getbbox()
                is None,
                "Independent body render changed",
            )
            old_text, new_text = old.get_textpage(), new.get_textpage()
            check(
                old_text.get_text_range() == new_text.get_text_range(),
                "Independent body copy/search changed",
            )
            old_text.close()
            new_text.close()
            old_image.close()
            new_image.close()
            old.close()
            new.close()
    return {"pages": len(a.pages), "body_pixel_difference": 0, "form_values": fields(b)}


def inspect_pdfium(path):
    reader = PdfReader(path)
    rows = []
    with pdfium.PdfDocument(str(path)) as document:
        for number in range(len(document)):
            page = document[number]
            offset = ctypes.c_int(0)
            handle = raw.FPDF_LINK()
            found = []
            while raw.FPDFLink_Enumerate(
                page, ctypes.byref(offset), ctypes.byref(handle)
            ):
                rectangle = raw.FS_RECTF()
                check(
                    raw.FPDFLink_GetAnnotRect(handle, ctypes.byref(rectangle)),
                    "PDFium rejected link rectangle",
                )
                dest = raw.FPDFLink_GetDest(document, handle)
                action = raw.FPDFLink_GetAction(handle)
                uri = None
                if action and raw.FPDFAction_GetType(action) == 3:
                    length = raw.FPDFAction_GetURIPath(document, action, None, 0)
                    buffer = ctypes.create_string_buffer(length)
                    raw.FPDFAction_GetURIPath(document, action, buffer, length)
                    uri = buffer.value.decode("utf-8")
                found.append(
                    {
                        "rectangle": [
                            rectangle.left,
                            rectangle.bottom,
                            rectangle.right,
                            rectangle.top,
                        ],
                        "page": (
                            raw.FPDFDest_GetDestPageIndex(document, dest)
                            if dest
                            else None
                        ),
                        "url": uri,
                    }
                )
            annotations = links(reader.pages[number])
            check(
                len(found) == len(annotations),
                "PDFium did not expose every Link annotation",
            )
            for entry, (_, annotation) in zip(found, annotations):
                expected = [
                    ctypes.c_float(float(value)).value for value in annotation["/Rect"]
                ]
                check(
                    max(abs(a - b) for a, b in zip(entry["rectangle"], expected))
                    <= 1e-6,
                    "Independent PDFium rectangle mismatch after specified float conversion",
                )
                if "/Dest" in annotation and isinstance(annotation["/Dest"], list):
                    page_ref = annotation["/Dest"][0]
                    expected_page = next(
                        i
                        for i, p in enumerate(reader.pages)
                        if canonical(p.indirect_reference) == canonical(page_ref)
                    )
                    check(
                        entry["page"] == expected_page,
                        "Independent PDFium page destination mismatch",
                    )
                if "/A" in annotation and annotation["/A"].get("/S") == "/URI":
                    check(
                        entry["url"] == str(annotation["/A"]["/URI"]),
                        "Independent PDFium URI mismatch",
                    )
            rows.append({"physical_page": number + 1, "links": found})
            page.close()
    return rows


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--run", required=True, type=Path)
    args = parser.parse_args()
    run = args.run
    result_path = run / "link-edit-verification.json"
    check(not result_path.exists(), "Preserve earlier verification")
    report = {
        "status": "FAIL",
        "scope": "Independent pypdf structure, PDFium link/target API and body pixels; not native external viewer GUI",
    }
    try:
        checks = []
        for before, after in (
            ("links-before.pdf", "links-edited.pdf"),
            ("links-before.pdf", "links-reedited.pdf"),
            ("links-existing-before.pdf", "links-existing-edited.pdf"),
            ("links-direct-before.pdf", "links-direct-edited.pdf"),
            ("links-form-before.pdf", "links-form-edited.pdf"),
            ("links-scan-ocr.pdf", "links-scan-reedited.pdf"),
        ):
            checks.append(
                {
                    "before": before,
                    "after": after,
                    **preserved(run / before, run / after),
                }
            )
        reader = PdfReader(run / "links-edited.pdf")
        check(
            {int(p.get("/Rotate", 0)) % 360 for p in reader.pages} == {0, 90, 180, 270},
            "All four rotations required",
        )
        errors = []
        for number, page in enumerate(reader.pages):
            items = links(page)
            check(len(items) == (2 if number == 0 else 1), "Created link count wrong")
            annotation = items[0][1]
            check(
                str(annotation["/Contents"]) == f"日本語のリンク {number + 1}",
                "Unicode description lost",
            )
            expected = pdf_rectangle(page, (15, 25, 90, 35))
            error = max(
                abs(float(a) - b) for a, b in zip(annotation["/Rect"], expected)
            )
            check(
                error <= 1e-6,
                "Saved PDF rectangle exceeds predeclared 1e-6pt criterion",
            )
            errors.append(error)
            check(
                annotation["/Dest"][1] == "/Fit",
                "Page target does not use requested Fit",
            )
        uri = reader.pages[0]["/Annots"][-1].get_object()["/A"]["/URI"]
        check(
            str(uri)
            == "https://example.com/%E8%B3%87%E6%96%99?q=%E6%97%A5%E6%9C%AC%E8%AA%9E#section",
            "URL is not portable encoded URI",
        )
        after = PdfReader(run / "links-reedited.pdf")
        box = links(after.pages[0])[0][1]["/Rect"]
        expected = pdf_rectangle(after.pages[0], (35, 55, 110, 45))
        check(
            max(abs(float(a) - b) for a, b in zip(box, expected)) <= 1e-6,
            "Reedited rectangle mismatch",
        )
        old, new = PdfReader(run / "links-existing-before.pdf"), PdfReader(
            run / "links-existing-edited.pdf"
        )
        for number, (a, b) in enumerate(zip(old.pages, new.pages)):
            for (_, first), (_, second) in zip(links(a), links(b)):
                for key in (
                    "/A",
                    "/Dest",
                    "/AP",
                    "/Border",
                    "/C",
                    "/F",
                    "/TatsujinTestProperty",
                ):
                    check(
                        canonical(first.get(key)) == canonical(second.get(key)),
                        "Existing link property lost: " + key,
                    )
                if "/QuadPoints" in first:
                    ax, ay, ar, at = map(float, first["/Rect"])
                    bx, by, br, bt = map(float, second["/Rect"])
                    expected = [
                        (
                            (bx + (float(v) - ax) / (ar - ax) * (br - bx))
                            if i % 2 == 0
                            else (by + (float(v) - ay) / (at - ay) * (bt - by))
                        )
                        for i, v in enumerate(first["/QuadPoints"])
                    ]
                    check(
                        max(
                            abs(float(a) - b)
                            for a, b in zip(second["/QuadPoints"], expected)
                        )
                        <= 1e-6,
                        "QuadPoints translation/scaling mismatch",
                    )
        ui = PdfReader(run / "links-ui.pdf")
        reedit = PdfReader(run / "links-ui-reedited.pdf")
        check(
            str(links(ui.pages[0])[0][1]["/Contents"]) == "日本語のページリンク",
            "UI Unicode save failed",
        )
        check(
            str(links(reedit.pages[0])[0][1]["/Contents"]) == "保存後にリンクを再編集",
            "UI saved edit lost",
        )
        external = {
            name: inspect_pdfium(run / name)
            for name in (
                "links-edited.pdf",
                "links-reedited.pdf",
                "links-ui.pdf",
                "links-ui-reedited.pdf",
                "links-scan-reedited.pdf",
            )
        }
        check(
            external["links-ui.pdf"][0]["links"][0]["page"] == 1
            and external["links-ui-reedited.pdf"][0]["links"][0]["page"] == 3,
            "UI page targets do not match actual navigation test",
        )
        scan = PdfReader(run / "links-scan-reedited.pdf")
        check(
            "市民公園" in scan.pages[0].extract_text()
            and "coastal" in scan.pages[1].extract_text().lower(),
            "Fixed OCR terms lost",
        )
        report.update(
            status="PASS",
            preserved=checks,
            saved_rectangle_max_error_pt=max(errors),
            encoded_uri=str(uri),
            external_links=external,
            quad_points_preserved=True,
        )
    except Exception as error:
        report["error"] = str(error)
    result_path.write_text(
        json.dumps(report, ensure_ascii=False, indent=2), encoding="utf-8"
    )
    print(
        json.dumps(
            {
                "status": report["status"],
                "error": report.get("error"),
                "output": str(result_path),
            },
            ensure_ascii=False,
        )
    )
    if report["status"] != "PASS":
        raise SystemExit(1)


if __name__ == "__main__":
    main()
