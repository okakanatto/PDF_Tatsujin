"""Independently verify comparison page pairs, PDF semantics and pixel bounds."""

import argparse
import hashlib
import json
from pathlib import Path

from PIL import ImageChops
from pypdf import PdfReader
import pypdfium2 as pdfium


FLAGS = (
    "geometryChanged",
    "textChanged",
    "pixelsChanged",
    "formsChanged",
    "annotationsChanged",
)


def check(condition, message):
    if not condition:
        raise RuntimeError(message)


def render(document, reader, index):
    page = document[index]
    bitmap = page.render(
        scale=float(reader.pages[index].get("/UserUnit", 1)), draw_annots=True
    )
    image = bitmap.to_pil().convert("RGB").copy()
    textpage = page.get_textpage()
    text = textpage.get_text_range()
    textpage.close()
    bitmap.close()
    page.close()
    return image, text


def inspect(
    run,
    name,
    before,
    after,
    expected_pairs,
    changed_indices,
    expected_flags=None,
    properties=None,
):
    report_path = run / f"comparison-{name}.json"
    report = json.loads(report_path.read_bytes())
    check(
        report["format"] == "PDFTatsujin-comparison" and report["version"] == 1,
        name + ": format",
    )
    check(
        report["dpi"] == 72 and report["coordinateTolerancePoints"] == 1e-6,
        name + ": fixed settings",
    )
    rows = report["pages"]
    check(
        [(r["leftPage"], r["rightPage"]) for r in rows] == expected_pairs,
        name + ": exact page correspondence",
    )
    changed = [
        i
        for i, r in enumerate(rows)
        if not r["leftPage"] or not r["rightPage"] or any(r[f] for f in FLAGS)
    ]
    check(
        changed == changed_indices and report["changedPages"] == len(changed_indices),
        name + ": fixed changed pages",
    )
    check(
        report["sameInComparedScope"]
        == (not changed and not report["propertyChanges"]),
        name + ": equality scope",
    )
    if properties is not None:
        check(
            report["propertyChanges"] == properties,
            name + ": fixed document property changes",
        )
    if expected_flags is not None:
        check(
            {f for f in FLAGS if rows[0][f]} == set(expected_flags),
            name + ": exact change kinds",
        )
    first, second = PdfReader(run / before), PdfReader(run / after)
    hashes = {
        n: hashlib.sha256((run / n).read_bytes()).hexdigest() for n in (before, after)
    }
    check(
        report["leftPages"] == len(first.pages)
        and report["rightPages"] == len(second.pages),
        name + ": independent page counts",
    )
    observations = []
    with pdfium.PdfDocument(str(run / before)) as left, pdfium.PdfDocument(
        str(run / after)
    ) as right:
        left.init_forms()
        right.init_forms()
        for row in rows:
            if not row["leftPage"] or not row["rightPage"]:
                observations.append(
                    dict(
                        left=row["leftPage"],
                        right=row["rightPage"],
                        added_or_deleted=True,
                    )
                )
                continue
            a, at = render(left, first, row["leftPage"] - 1)
            b, bt = render(right, second, row["rightPage"] - 1)
            equal_size = a.size == b.size
            box = (
                ImageChops.difference(a, b).getbbox()
                if equal_size
                else (0, 0, max(a.width, b.width), max(a.height, b.height))
            )
            check(
                bool(box) == row["pixelsChanged"],
                name + ": independent visible change differs",
            )
            for key in ("leftPixelSHA256", "rightPixelSHA256"):
                check(
                    len(row[key]) == 64
                    and all(c in "0123456789abcdef" for c in row[key]),
                    name + ": pixel digest",
                )
            error = None
            if box and equal_size:
                x, y, width, height = row["pixelBounds"]
                error = max(
                    abs(a - b) for a, b in zip(box, (x, y, x + width, y + height))
                )
                check(
                    error <= 2,
                    f"{name}: independent difference bounds {box} vs {row['pixelBounds']}, error {error}px",
                )
            if name == "body":
                check(
                    "確認前" in at and "確認後" in bt, "Independent Japanese body text"
                )
            if name == "ocr":
                term = "市民公園" if row["leftPage"] == 1 else "coastal"
                check(at == bt and term in at, "Independent bilingual OCR preserved")
            observations.append(
                dict(
                    left=row["leftPage"],
                    right=row["rightPage"],
                    pixels_changed=bool(box),
                    PDFium_bounds=box,
                    application_bounds=row["pixelBounds"],
                    maximum_edge_error_px=error,
                    text_equal=at == bt,
                )
            )
    check(
        all(
            hashlib.sha256((run / n).read_bytes()).hexdigest() == value
            for n, value in hashes.items()
        ),
        name + ": input PDF changed",
    )
    return dict(
        case=name,
        before=before,
        after=after,
        input_SHA256=hashes,
        changed_pages=changed_indices,
        properties=report["propertyChanges"],
        pages=observations,
    )


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("directory", type=Path)
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    run = args.directory.resolve()
    output = (
        args.output.resolve() if args.output else run / "independent-comparison.json"
    )
    check(not output.exists(), "Preserve earlier independent evidence")
    process = json.loads((run / "process-result.json").read_text("utf-8-sig"))
    suite = json.loads((run / "selftest.json").read_bytes())
    tests = [r for r in suite["tests"] if r["name"].startswith("M5C")]
    check(
        process["completed"]
        and process["report_valid"]
        and len(tests) == 4
        and all(r["status"] == "PASS" for r in tests),
        "Require completed four comparison execution tests",
    )
    result = dict(status="FAIL", cases=[])
    paired = [(i, i) for i in range(1, 5)]
    base = "comparison-same-before.pdf"
    try:
        cases = [
            ("same", base, "comparison-same-after.pdf", paired, [], [], []),
            (
                "body",
                "comparison-body-before.pdf",
                "comparison-body-after.pdf",
                [(1, 1)],
                [0],
                ["textChanged", "pixelsChanged"],
                None,
            ),
            (
                "signature",
                "comparison-signature-before.pdf",
                "comparison-signature-after.pdf",
                paired,
                [0],
                ["pixelsChanged", "annotationsChanged"],
                None,
            ),
            (
                "comment",
                "comparison-comment-before.pdf",
                "comparison-comment-after.pdf",
                paired,
                [0],
                ["annotationsChanged"],
                [],
            ),
            (
                "form",
                "comparison-form-before.pdf",
                "comparison-form-after.pdf",
                [(1, 1)],
                [0],
                ["pixelsChanged", "formsChanged"],
                None,
            ),
            ("title", base, "comparison-title-after.pdf", paired, [], [], ["title"]),
            (
                "bookmark",
                base,
                "comparison-bookmark-after.pdf",
                paired,
                [],
                [],
                ["bookmarks", "modifiedDate", "producer"],
            ),
            (
                "link",
                "comparison-link-before.pdf",
                "comparison-link-after.pdf",
                paired,
                [0],
                ["annotationsChanged"],
                [],
            ),
            (
                "image",
                base,
                "comparison-image-after.pdf",
                paired,
                [0],
                ["pixelsChanged", "annotationsChanged"],
                None,
            ),
            (
                "vector",
                base,
                "comparison-vector-after.pdf",
                paired,
                [0],
                ["pixelsChanged", "annotationsChanged"],
                None,
            ),
            (
                "inserted",
                base,
                "comparison-inserted.pdf",
                [(1, 1), (2, 2), (0, 3), (3, 4), (4, 5)],
                [2],
                None,
                None,
            ),
            (
                "deleted",
                base,
                "comparison-deleted.pdf",
                [(1, 1), (2, 0), (3, 2), (4, 3)],
                [1],
                None,
                None,
            ),
            (
                "numbered",
                base,
                "comparison-inserted.pdf",
                [(1, 1), (2, 2), (3, 3), (4, 4), (0, 5)],
                [2, 3, 4],
                None,
                None,
            ),
            (
                "ocr",
                "comparison-ocr-before.pdf",
                "comparison-ocr-after.pdf",
                [(1, 1), (2, 2)],
                [0],
                ["pixelsChanged", "annotationsChanged"],
                None,
            ),
        ]
        for key in ("Rotate", "CropBox", "UserUnit"):
            cases.append(
                (
                    f"geometry-{key}",
                    base,
                    f"comparison-geometry-{key}.pdf",
                    paired,
                    [0],
                    ["geometryChanged", "pixelsChanged"],
                    [],
                )
            )
        for case in cases:
            result["cases"].append(inspect(run, *case))
        form = PdfReader(run / "comparison-form-after.pdf").get_fields()
        check(form["name"]["/V"] == "髙橋 香織", "Independent Japanese form value")
        for phase, expected in (("before", "確認前"), ("after", "確認後")):
            page = PdfReader(run / f"comparison-comment-{phase}.pdf").pages[0]
            check(
                any(
                    ref.get_object().get("/Contents") == expected
                    for ref in page["/Annots"]
                ),
                "Independent invisible comment content",
            )
        title = PdfReader(run / "comparison-title-after.pdf")
        check(
            title.metadata.title == "比較対象の新しい題名", "Independent document title"
        )
        bookmark = PdfReader(run / "comparison-bookmark-after.pdf")
        check(
            bookmark.outline[0].title == "比較用しおり"
            and bookmark.get_destination_page_number(bookmark.outline[0]) == 2,
            "Independent bookmark destination/title",
        )
        destinations = []
        for phase in ("before", "after"):
            document = PdfReader(run / f"comparison-link-{phase}.pdf")
            link = [
                ref.get_object()
                for ref in document.pages[0]["/Annots"]
                if ref.get_object().get("/Subtype") == "/Link"
            ][-1]
            dest = link.get("/Dest", link.get("/A", {}).get("/D"))
            destinations.append(
                next(
                    i
                    for i, page in enumerate(document.pages)
                    if page.indirect_reference == dest[0]
                )
            )
        check(destinations == [1, 2], "Independent changed internal link destination")
        result.update(
            status="PASS",
            fixed_bbox_tolerance_px=2,
            input_semantics="Japanese form/comment/title/bookmark/link independently checked",
            scope="Independent pypdf structure and PDFium rendering at physical 72dpi with form appearances. Not native Reader, OS DPI, IME or physical printing.",
        )
    except Exception as error:
        result["error"] = str(error)
        raise
    finally:
        output.write_text(
            json.dumps(result, ensure_ascii=False, indent=2) + "\n", encoding="utf-8"
        )
    print(
        "PASS: 17 exact comparison cases, independent pixels/bounds, PDF semantics and unchanged inputs"
    )


if __name__ == "__main__":
    main()
