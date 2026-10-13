"""Compare an OCR PDF with a signature-moved PDF saved through the native UI.

This verifies artifact preservation. It does not prove native UI execution.
"""

import argparse
from contextlib import closing
import hashlib
import json
from pathlib import Path

from PIL import ImageChops
from pypdf import PdfReader
import pypdfium2 as pdfium


def stamp(path):
    annotations = [
        ref.get_object()
        for page in PdfReader(path).pages
        for ref in page.get("/Annots", [])
        if "/Tatsujin" in ref.get_object()
    ]
    if len(annotations) != 1:
        raise ValueError("Expected exactly one editable signature")
    annotation = annotations[0]
    return {
        "metadata": json.loads(annotation["/Tatsujin"].original_bytes),
        "contents": str(annotation["/Contents"]),
        "rect": [float(v) for v in annotation["/Rect"]],
        "print": bool(int(annotation["/F"]) & 4),
        "appearance": bool(annotation["/AP"]["/N"].get_data()),
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", type=Path)
    parser.add_argument("saved", type=Path)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()
    if args.output.exists():
        raise ValueError("Preserve earlier evidence; use a new output path")
    before, after = stamp(args.source), stamp(args.saved)
    checks = [
        {
            "id": "editable_signature_moved_and_retained",
            "status": (
                "PASS"
                if before["metadata"]["text"] == after["metadata"]["text"]
                and before["metadata"]["size"] == after["metadata"]["size"]
                and before["contents"] == after["contents"]
                and before["rect"] != after["rect"]
                and after["print"]
                and after["appearance"]
                else "FAIL"
            ),
        }
    ]
    with closing(pdfium.PdfDocument(args.source)) as original, closing(
        pdfium.PdfDocument(args.saved)
    ) as saved:
        checks.append(
            {
                "id": "page_count",
                "status": "PASS" if len(original) == len(saved) else "FAIL",
            }
        )
        for index in range(min(len(original), len(saved))):
            with closing(original[index]) as a, closing(saved[index]) as b:
                with closing(a.get_textpage()) as ta, closing(b.get_textpage()) as tb:
                    text_equal = ta.get_text_range() == tb.get_text_range()
                with closing(a.render(scale=1, draw_annots=False)) as ra, closing(
                    b.render(scale=1, draw_annots=False)
                ) as rb:
                    ia, ib = ra.to_pil().convert("RGB"), rb.to_pil().convert("RGB")
                images_equal = (
                    ia.size == ib.size
                    and ImageChops.difference(ia, ib).getbbox() is None
                )
            checks.append(
                {
                    "id": f"OCR_body_page_{index+1}",
                    "status": "PASS" if text_equal and images_equal else "FAIL",
                    "text_equal": text_equal,
                    "visible_pixels_equal": images_equal,
                    "scope": "Exclude intentionally moved signature annotation",
                }
            )
    result = {
        "status": "PASS" if all(c["status"] == "PASS" for c in checks) else "FAIL",
        "engine": str(pdfium.PDFIUM_INFO),
        "files": [
            {
                "file": path.name,
                "sha256": hashlib.sha256(path.read_bytes()).hexdigest(),
                "bytes": path.stat().st_size,
            }
            for path in [args.source, args.saved]
        ],
        "before_signature": before,
        "after_signature": after,
        "checks": checks,
    }
    args.output.write_text(
        json.dumps(result, ensure_ascii=False, indent=2) + "\n",
        encoding="utf-8",
        newline="\n",
    )
    print(json.dumps({"status": result["status"], "checks": len(checks)}))
    return int(result["status"] != "PASS")


if __name__ == "__main__":
    raise SystemExit(main())
