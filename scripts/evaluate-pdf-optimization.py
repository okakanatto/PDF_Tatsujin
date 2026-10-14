"""Compare reachable PDF meaning, stream data, editing identity and pixels."""

import argparse
import json
from pathlib import Path

from PIL import ImageChops
from pypdf import PdfReader
from pypdf.generic import IndirectObject, StreamObject
import pypdfium2 as pdfium


def check(value, message):
    if not value:
        raise RuntimeError(message)


def identity(value):
    return value.idnum, value.generation


def compare_graph(before, after):
    visited, identities, reverse = set(), {}, {}
    stream_count = 0

    def compare(a, b, path):
        nonlocal stream_count
        if isinstance(a, IndirectObject) or isinstance(b, IndirectObject):
            check(
                isinstance(a, IndirectObject) and isinstance(b, IndirectObject),
                "Reference/value changed: " + path,
            )
            first, second = a.get_object(), b.get_object()
            pair = (identity(a), identity(b))
            if pair in visited:
                return
            visited.add(pair)
            if not isinstance(first, StreamObject):
                check(
                    identities.get(identity(a), identity(b)) == identity(b),
                    "One editing object split: " + path,
                )
                check(
                    reverse.get(identity(b), identity(a)) == identity(a),
                    "Distinct editing objects merged: " + path,
                )
                identities[identity(a)], reverse[identity(b)] = identity(b), identity(a)
            compare(first, second, path)
            return
        if isinstance(a, StreamObject):
            check(
                isinstance(b, StreamObject), "Stream converted to other type: " + path
            )
            check(
                a.get_data() == b.get_data(), "Decoded stream content changed: " + path
            )
            stream_count += 1
            keys = set(a) - {"/Length"}
            check(
                keys == set(b) - {"/Length"}, "Stream dictionary keys changed: " + path
            )
            for key in keys:
                compare(a.raw_get(key), b.raw_get(key), path + key)
        elif isinstance(a, dict):
            check(
                isinstance(b, dict) and set(a) == set(b),
                "Dictionary structure changed: " + path,
            )
            for key in a:
                compare(a.raw_get(key), b.raw_get(key), path + key)
        elif isinstance(a, list):
            check(isinstance(b, list) and len(a) == len(b), "Array changed: " + path)
            for i, (first, second) in enumerate(zip(a, b)):
                compare(first, second, path + f"[{i}]")
        else:
            check(a == b, "PDF value changed: " + path)

    for key in ("/Root", "/Info", "/ID"):
        check(
            (key in before.trailer) == (key in after.trailer),
            "Trailer key presence changed",
        )
        if key in before.trailer:
            compare(before.trailer.raw_get(key), after.trailer.raw_get(key), key)
    return {
        "reachable_streams_checked": stream_count,
        "distinct_non_stream_identities": len(identities),
    }


def compare_pages(before_path, after_path):
    with pdfium.PdfDocument(str(before_path)) as a, pdfium.PdfDocument(
        str(after_path)
    ) as b:
        check(len(a) == len(b), "Page count changed")
        count = len(a)
        for number in range(len(a)):
            old, new = a[number], b[number]
            old_image, new_image = old.render(scale=0.8, draw_annots=True), new.render(
                scale=0.8, draw_annots=True
            )
            check(
                ImageChops.difference(
                    old_image.to_pil().convert("RGB"), new_image.to_pil().convert("RGB")
                ).getbbox()
                is None,
                "Independent visible image changed",
            )
            old_text, new_text = old.get_textpage(), new.get_textpage()
            check(
                old_text.get_text_range() == new_text.get_text_range(),
                "Independent extracted body/OCR text changed",
            )
            old_text.close()
            new_text.close()
            old_image.close()
            new_image.close()
            old.close()
            new.close()
    return {"pages": count, "pixel_difference": 0}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("directory", type=Path)
    args = parser.parse_args()
    run = args.directory
    target = run / "independent-pdf-optimization.json"
    check(not target.exists(), "Preserve earlier results")
    result = {
        "status": "FAIL",
        "scope": "Independent pypdf reachable graph/decoded streams, PDFium rendering/text; not native UI acceptance",
    }
    try:
        rows = []
        for before, after in (
            ("optimization-before.pdf", "optimization-after.pdf"),
            ("optimization-form-before.pdf", "optimization-form-after.pdf"),
            ("optimization-mixed-before.pdf", "optimization-mixed-after.pdf"),
            ("optimization-ocr-before.pdf", "optimization-ocr-after.pdf"),
        ):
            a, b = PdfReader(run / before), PdfReader(run / after)
            check(
                (run / after).stat().st_size < (run / before).stat().st_size,
                "Actual saved bytes did not decrease",
            )
            graph = compare_graph(a, b)
            rows.append(
                {
                    "before": before,
                    "after": after,
                    "before_bytes": (run / before).stat().st_size,
                    "after_bytes": (run / after).stat().st_size,
                    **graph,
                    **compare_pages(run / before, run / after),
                }
            )
        reader = PdfReader(run / "optimization-reedited.pdf")
        links = [
            ref.get_object()
            for ref in reader.pages[0]["/Annots"]
            if ref.get_object().get("/Subtype") == "/Link"
        ]
        check(
            [str(link["/Contents"]) for link in links]
            == ["片方だけ再編集", "同一だが別のリンク"],
            "Shared link data collapsed editing identity",
        )
        form = PdfReader(run / "optimization-form-after.pdf")
        check(
            str(form.get_fields()["name"]["/V"]) == "髙橋 香織",
            "Japanese form value lost",
        )
        result.update(
            status="PASS", comparisons=rows, independently_editable_links=True
        )
    except Exception as error:
        result["error"] = str(error)
    target.write_text(
        json.dumps(result, ensure_ascii=False, indent=2), encoding="utf-8"
    )
    print(
        json.dumps(
            {
                "status": result["status"],
                "error": result.get("error"),
                "output": str(target),
            },
            ensure_ascii=False,
        )
    )
    if result["status"] != "PASS":
        raise SystemExit(1)


if __name__ == "__main__":
    main()
