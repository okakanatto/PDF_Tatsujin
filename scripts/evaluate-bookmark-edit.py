"""Inspect saved outline pointers/counts, Unicode, destinations and unchanged pages."""

import argparse
import hashlib
import json
from pathlib import Path

from PIL import ImageChops
from pypdf import PdfReader
from pypdf.generic import IndirectObject
import pypdfium2 as pdfium


def check(value, message):
    if not value:
        raise RuntimeError(message)


def identity(value):
    return (
        (value.idnum, value.generation) if isinstance(value, IndirectObject) else None
    )


def canonical(value):
    if isinstance(value, IndirectObject):
        return identity(value)
    if isinstance(value, dict):
        return {key: canonical(part) for key, part in value.items()}
    if isinstance(value, list):
        return [canonical(part) for part in value]
    return value


def outlines(reader):
    root_ref = reader.trailer["/Root"].get("/Outlines")
    if root_ref is None:
        return []
    rows, seen = [], set()

    def walk(parent_ref, level):
        parent = parent_ref.get_object()
        current = parent.get("/First")
        previous = None
        visible = 0
        while current is not None:
            ref = identity(current)
            check(
                ref is not None and ref not in seen,
                "Invalid, duplicate or cyclic outline reference",
            )
            seen.add(ref)
            item = current.get_object()
            check(
                identity(item.get("/Parent")) == identity(parent_ref),
                "Incorrect parent pointer",
            )
            check(
                identity(item.get("/Prev")) == identity(previous),
                "Incorrect backward sibling pointer",
            )
            row = {
                "reference": ref,
                "title": str(item["/Title"]),
                "level": level,
                "count": int(item.get("/Count", 0)),
                "object": item,
            }
            rows.append(row)
            descendants = walk(current, level + 1) if "/First" in item else 0
            check(
                abs(row["count"]) == descendants,
                "Outline Count does not match visible descendants",
            )
            visible += 1 + (descendants if row["count"] > 0 else 0)
            previous, current = current, item.get("/Next")
        check(
            identity(parent.get("/Last")) == identity(previous),
            "Incorrect last sibling pointer",
        )
        return visible

    visible = walk(root_ref, 0)
    check(
        int(root_ref.get_object().get("/Count", 0)) == visible,
        "Root visible Count is wrong",
    )
    return rows


def preserved_pages(before_path, after_path):
    before, after = PdfReader(before_path), PdfReader(after_path)
    check(len(before.pages) == len(after.pages), "Page count changed")
    for old, new in zip(before.pages, after.pages):
        for key in (
            "/MediaBox",
            "/CropBox",
            "/Rotate",
            "/UserUnit",
            "/Resources",
            "/Annots",
        ):
            check(
                canonical(old.get(key)) == canonical(new.get(key)),
                f"Page entry changed: {key}",
            )
        a, b = old.get_contents(), new.get_contents()
        if a is None:
            check(b is None, "Originally blank page acquired a body stream")
        else:
            old_bytes = a.get_data()
            check(
                old_bytes and b is not None and old_bytes == b.get_data(),
                "Nonempty original body was changed or flattened",
            )
        check(old.extract_text() == new.extract_text(), "Original text changed")
    fields = lambda reader: {
        name: str(field.get("/V", ""))
        for name, field in (reader.get_fields() or {}).items()
    }
    check(fields(before) == fields(after), "Form values changed")
    old_pdf, new_pdf = pdfium.PdfDocument(str(before_path)), pdfium.PdfDocument(
        str(after_path)
    )
    try:
        for i in range(len(old_pdf)):
            a, b = old_pdf[i], new_pdf[i]
            first, second = a.render(scale=0.6, draw_annots=True), b.render(
                scale=0.6, draw_annots=True
            )
            check(
                ImageChops.difference(
                    first.to_pil().convert("RGB"), second.to_pil().convert("RGB")
                ).getbbox()
                is None,
                "Bookmark-only edit changed visible pages",
            )
            text_a, text_b = a.get_textpage(), b.get_textpage()
            check(
                text_a.get_text_range() == text_b.get_text_range(),
                "Independent body copy/search text changed",
            )
            text_a.close()
            text_b.close()
            first.close()
            second.close()
            a.close()
            b.close()
    finally:
        old_pdf.close()
        new_pdf.close()
    return {
        "pages": len(before.pages),
        "visible_difference": 0,
        "body_annotations_forms": "PASS",
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("directory", type=Path)
    directory = parser.parse_args().directory.resolve()
    target = directory / "independent-bookmark-edit.json"
    check(not target.exists(), "Preserve earlier results")
    process = json.loads((directory / "process-result.json").read_text("utf-8-sig"))
    suite = json.loads((directory / "selftest.json").read_text("utf-8-sig"))
    cases = [row for row in suite["tests"] if row["name"].startswith("M4B")]
    check(
        process["completed"]
        and process["report_valid"]
        and process["exit_code"] == 0
        and len(cases) == 4
        and all(row["status"] == "PASS" for row in cases),
        "Four bookmark edit workflows must pass",
    )
    before = outlines(PdfReader(directory / "bookmarks-before.pdf"))
    after = outlines(PdfReader(directory / "bookmarks-edited.pdf"))
    lookup = {tuple(row["reference"]): row["object"] for row in after}
    for row in before:
        check(tuple(row["reference"]) in lookup, "Original outline item lost")
        for key in ("/Dest", "/A", "/C", "/F", "/TatsujinTestProperty"):
            check(
                canonical(row["object"].get(key))
                == canonical(lookup[tuple(row["reference"])].get(key)),
                f"Existing action/destination/property changed: {key}",
            )
    check(
        after[0]["title"] == "日本語の見出し"
        and after[0]["count"] < 0
        and after[1]["title"] == "追加した子"
        and after[1]["level"] == 1,
        "Unicode hierarchy/collapsed state wrong",
    )
    retargeted = outlines(PdfReader(directory / "bookmarks-retargeted.pdf"))
    first = retargeted[0]["object"]
    check(
        "/A" not in first and str(first["/Dest"][1]) == "/Fit",
        "Explicit retarget did not replace old action with Fit",
    )
    check(
        not outlines(PdfReader(directory / "bookmarks-empty.pdf")),
        "Saved removal left visible outlines",
    )
    pages = {}
    for before_name, after_name in (
        ("bookmarks-before.pdf", "bookmarks-edited.pdf"),
        ("bookmarks-before.pdf", "bookmarks-retargeted.pdf"),
        ("bookmarks-before.pdf", "bookmarks-empty.pdf"),
        ("bookmarks-form-before.pdf", "bookmarks-form.pdf"),
        ("bookmarks-ocr-before.pdf", "bookmarks-ocr.pdf"),
    ):
        pages[after_name] = preserved_pages(
            directory / before_name, directory / after_name
        )
    check(
        str(PdfReader(directory / "bookmarks-form.pdf").get_fields()["name"]["/V"])
        == "髙橋 香織",
        "Japanese form value lost",
    )
    toc = {}
    deepest = outlines(PdfReader(directory / "bookmarks-depth32.pdf"))
    check(
        len(deepest) == 32 and deepest[-1]["level"] == 31,
        "32-level boundary was truncated or corrupt",
    )
    for name in (
        "bookmarks-edited.pdf",
        "bookmarks-retargeted.pdf",
        "bookmarks-ui.pdf",
        "bookmarks-ui-reedited.pdf",
    ):
        document = pdfium.PdfDocument(str(directory / name))
        rows = []
        try:
            for item in document.get_toc(max_depth=32):
                destination = item.get_dest()
                rows.append(
                    {
                        "title": item.get_title(),
                        "level": item.level,
                        "count": item.get_count(),
                        "page": destination.get_index() if destination else None,
                    }
                )
        finally:
            document.close()
        toc[name] = rows
    check(
        any(
            row["title"] == "追加した子" and row["page"] == 2
            for row in toc["bookmarks-edited.pdf"]
        ),
        "Independent PDFium cannot use added destination",
    )
    check(
        [(row["title"], row["level"], row["page"]) for row in toc["bookmarks-ui.pdf"]]
        == [("第二節", 0, 1), ("第一章", 0, 0), ("付録", 1, 3)],
        "Actual dialog hierarchy/order/destinations wrong",
    )
    check(
        toc["bookmarks-ui-reedited.pdf"][0]["title"] == "保存後の章名変更"
        and toc["bookmarks-ui-reedited.pdf"][0]["page"] == 2,
        "Saved dialog reedit is wrong",
    )
    result = {
        "status": "PASS",
        "executable_sha256": process["exe_sha256"],
        "saved_pdf_sha256": hashlib.sha256(
            (directory / "bookmarks-edited.pdf").read_bytes()
        ).hexdigest(),
        "original_outline_items": len(before),
        "saved_outline_items": len(after),
        "structure_counts_properties_destinations": "PASS",
        "pages": pages,
        "PDFium_outlines": toc,
        "scope": "Independent pypdf pointers/counts/Unicode/actions and PDFium TOC/destinations/body text/pixels. Not native Reader GUI.",
    }
    target.write_text(
        json.dumps(result, ensure_ascii=False, indent=2) + "\n", encoding="utf-8"
    )
    print(json.dumps(result, ensure_ascii=False))


if __name__ == "__main__":
    main()
