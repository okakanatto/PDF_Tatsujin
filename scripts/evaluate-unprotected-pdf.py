"""Verify an owner-authorized copy with independent PDF parsers and renderers."""

import argparse
import hashlib
import importlib.util
import json
from pathlib import Path

from PIL import ImageChops
from pypdf import PdfReader
import pypdfium2 as pdfium

spec = importlib.util.spec_from_file_location(
    "encryption_inspection", Path(__file__).with_name("evaluate-encrypted-pdf.py")
)
inspection = importlib.util.module_from_spec(spec)
spec.loader.exec_module(inspection)
check = inspection.check


def inspect_pair(before, after, password, *, render_password=None):
    source, copy = PdfReader(before), PdfReader(after)
    check(source.is_encrypted, "Input is not encrypted")
    check(source.decrypt(password).name == "OWNER_PASSWORD", "Independent owner role")
    check(
        not copy.is_encrypted and "/Encrypt" not in copy.trailer, "Encryption persists"
    )
    graph = inspection.graph(source, copy, upgraded_pdf_version=False)
    with pdfium.PdfDocument(
        str(before), password=password if render_password is None else render_password
    ) as original:
        with pdfium.PdfDocument(str(after)) as restored:
            check(len(original) == len(restored), "Page count differs")
            for index in range(len(original)):
                first, second = original[index], restored[index]
                check(first.get_size() == second.get_size(), "Page geometry differs")
                a, b = first.render(scale=0.8, draw_annots=True), second.render(
                    scale=0.8, draw_annots=True
                )
                check(
                    ImageChops.difference(
                        a.to_pil().convert("RGB"), b.to_pil().convert("RGB")
                    ).getbbox()
                    is None,
                    "Pixels differ",
                )
                x, y = first.get_textpage(), second.get_textpage()
                check(x.get_text_range() == y.get_text_range(), "Body/OCR text differs")
                for resource in (x, y, a, b, first, second):
                    resource.close()
    return dict(
        input=before.name,
        output=after.name,
        pages=len(copy.pages),
        revision=source.trailer["/Encrypt"]["/R"],
        encryption_removed=True,
        source_render_authentication="owner" if render_password is None else "user",
        pixel_difference=0,
        extracted_text_exact=True,
        **graph,
    )


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--run", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()
    check(not args.output.exists(), "Use new independent evidence output")
    root = Path(__file__).resolve().parents[1]
    run = args.run
    result = dict(status="FAIL", files=[])
    try:
        for kind in ("rich", "ocr"):
            result["files"].append(
                inspect_pair(
                    run / f"unprotected-{kind}-input.pdf",
                    run / f"unprotected-{kind}-copy.pdf",
                    "変更-Owner-2026",
                )
            )
        manifest = json.loads((root / "fixtures/protection/manifest.json").read_bytes())
        for row in manifest["cases"]:
            before = root / "fixtures/protection" / row["file"]
            check(
                hashlib.sha256(before.read_bytes()).hexdigest() == row["sha256"],
                "Fixture changed",
            )
            result["files"].append(
                inspect_pair(
                    before,
                    run / ("unprotected-compat-" + row["file"]),
                    row["owner_password"],
                    # This PDFium rejects an empty owner password. Its user
                    # password still opens the same source pixels. pypdf above
                    # independently verifies the actual empty-owner role.
                    render_password=(
                        row["user_password"] if not row["owner_password"] else None
                    ),
                )
            )
        edited = PdfReader(run / "unprotected-rich-edited.pdf")
        check(not edited.is_encrypted, "Edited copy became encrypted")
        check(
            edited.get_fields()["name"]["/V"] == "解除後の入力", "Reedited form missing"
        )
        overlays = [
            json.loads(annotation.get_object()["/Tatsujin"].original_bytes)
            for annotation in edited.pages[0]["/Annots"]
            if "/Tatsujin" in annotation.get_object()
        ]
        check(
            any(row.get("text") == "解除後の署名" for row in overlays),
            "Reedited signature missing",
        )
        with pdfium.PdfDocument(str(run / "unprotected-ocr-copy.pdf")) as document:
            values = []
            for index in range(2):
                page = document[index]
                text = page.get_textpage()
                values.append(text.get_text_range())
                text.close()
                page.close()
            check(
                "市民公園" in values[0] and "coastal" in values[1].lower(),
                "Fixed bilingual terms lost",
            )
        result.update(status="PASS", independent_input_cases=6, reediting=True)
    except Exception as error:
        result["error"] = str(error)
    args.output.write_text(
        json.dumps(result, ensure_ascii=False, indent=2), encoding="utf-8"
    )
    print(json.dumps(dict(status=result["status"], error=result.get("error"))))
    if result["status"] != "PASS":
        raise SystemExit(1)


if __name__ == "__main__":
    main()
