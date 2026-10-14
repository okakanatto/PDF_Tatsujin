"""Check frozen vertical text, search bounds and unchanged pixels independently."""

import argparse
import hashlib
import json
from pathlib import Path
import re

from PIL import ImageChops
import pypdfium2 as pdfium
from pypdf import PdfReader
from vertical_ocr_fixtures import PROFILES, frozen_vertical, physical_upright_box


def check(value, reason):
    if not value:
        raise AssertionError(reason)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("output", type=Path)
    parser.add_argument("--attempt", default="r1")
    parser.add_argument("--profile", choices=PROFILES, default="standard")
    args = parser.parse_args()
    fixed, source, digest = frozen_vertical(args.profile)
    index = fixed["page"] - 1
    expected = "".join(fixed["expected_lines_right_to_left"])
    rows = []
    for name in ("vertical-ui-saved.pdf", "vertical-ui-reedited.pdf"):
        path = args.output / name
        row = dict(file=name)
        try:
            reader = PdfReader(path, strict=True)
            if "cropbox" in fixed:
                saved_page = reader.pages[index]
                check(
                    list(map(float, saved_page.cropbox)) == fixed["cropbox"]
                    and saved_page.rotation == fixed["rotation"]
                    and float(saved_page.get("/UserUnit", 1)) == fixed["UserUnit"],
                    "Original CropBox, rotation and UserUnit preserved",
                )
            copied = reader.pages[index].extract_text()
            check(re.sub(r"\s+", "", copied) == expected, "pypdf text and column order")
            with pdfium.PdfDocument(source) as before, pdfium.PdfDocument(
                path
            ) as after:
                check(len(before) == len(after), "Page count")
                page = after[index]
                textpage = page.get_textpage()
                actual = textpage.get_text_range()
                check(
                    re.sub(r"\s+", "", actual) == expected,
                    "PDFium exact text and column order",
                )
                for line in fixed["expected_lines_right_to_left"]:
                    check(
                        line in actual,
                        "Contiguous column copy, not one newline per character",
                    )
                searches = []
                for item in fixed["search_terms"]:
                    finder = textpage.search(item["term"])
                    match = finder.get_next()
                    check(
                        match is not None and finder.get_next() is None,
                        "One fixed search match",
                    )
                    finder.close()
                    boxes = [
                        textpage.get_charbox(i)
                        for i in range(match[0], match[0] + match[1])
                        if not textpage.get_text_range(i, 1).isspace()
                    ]
                    left, bottom = min(b[0] for b in boxes), min(b[1] for b in boxes)
                    right, top = max(b[2] for b in boxes), max(b[3] for b in boxes)
                    actual_box = physical_upright_box(
                        fixed, (left, bottom, right, top), page.get_height()
                    )
                    error = (
                        max(
                            abs(a - b) for a, b in zip(actual_box, item["qt_bounds_pt"])
                        )
                        * 25.4
                        / 72
                    )
                    check(
                        error <= fixed["maximum_bounds_error_mm"],
                        "Unchanged 2mm search criterion",
                    )
                    searches.append(
                        dict(term=item["term"], maximum_bounds_error_mm=error)
                    )
                for number in range(len(before)):
                    a = (
                        before[number]
                        .render(scale=1, draw_annots=False)
                        .to_pil()
                        .convert("RGB")
                    )
                    b = (
                        after[number]
                        .render(scale=1, draw_annots=False)
                        .to_pil()
                        .convert("RGB")
                    )
                    check(
                        ImageChops.difference(a, b).getbbox() is None,
                        "Every original visible page unchanged",
                    )
                row.update(
                    status="PASS",
                    searches=searches,
                    copy=actual,
                    sha256=hashlib.sha256(path.read_bytes()).hexdigest(),
                    visible_pixel_difference=0,
                )
        except Exception as error:
            row.update(status="FAIL", error=str(error))
        rows.append(row)
    result = dict(
        criteria_sha256=digest,
        cases=rows,
        failures=sum(row["status"] != "PASS" for row in rows),
    )
    target = args.output / ("vertical-independent-" + args.attempt + ".json")
    with target.open("x", encoding="utf-8") as out:
        json.dump(result, out, ensure_ascii=False, indent=2)
        out.write("\n")
    print(json.dumps(result, ensure_ascii=False, indent=2))
    raise SystemExit(bool(result["failures"]))


if __name__ == "__main__":
    main()
