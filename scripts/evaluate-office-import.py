"""Independent checks against the DOCX expectations fixed before conversion."""

import argparse
import hashlib
import json
from pathlib import Path
import subprocess

from PIL import Image
import pypdfium2 as pdfium
from pypdf import PdfReader

ROOT = Path(__file__).resolve().parents[1]
CRITERIA_SHA = "18a8ddead69890390cf52cf9fca782563cd23c4d30dfdbed58e9bb6800ac3785"


def check(value, why):
    if not value:
        raise AssertionError(why)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("output", type=Path)
    parser.add_argument("--poppler", type=Path, required=True)
    parser.add_argument("--attempt", default="r1")
    args = parser.parse_args()
    fixed_path = ROOT / "fixtures/office-import/criteria.json"
    check(
        hashlib.sha256(fixed_path.read_bytes()).hexdigest() == CRITERIA_SHA,
        "criteria SHA",
    )
    fixed = json.loads(fixed_path.read_text(encoding="utf-8"))
    render = args.output / ("office-independent-render-" + args.attempt)
    render.mkdir(exist_ok=False)
    results = []
    cases = [
        ("simple", "simple.docx"),
        ("pages-images", "pages-images.docx"),
        ("signed", "simple.docx"),
        ("reedited", "simple.docx"),
        ("ui", "pages-images.docx"),
    ]
    for name, source in cases:
        path = args.output / ("office-" + name + ".pdf")
        row = {"case": name, "sha256": hashlib.sha256(path.read_bytes()).hexdigest()}
        try:
            reader = PdfReader(path, strict=True)
            expected = fixed["expected_pages"][source]
            check(len(reader.pages) == len(expected), "page count")
            with pdfium.PdfDocument(path) as document:
                check(len(document) == len(expected), "PDFium page count")
                for index, required in enumerate(expected):
                    pypdf_text = reader.pages[index].extract_text()
                    page = document[index]
                    try:
                        textpage = page.get_textpage()
                        try:
                            text = textpage.get_text_range()
                        finally:
                            textpage.close()
                        for phrase in required:
                            check(phrase in text, "PDFium searchable text: " + phrase)
                            check(
                                phrase in pypdf_text, "pypdf searchable text: " + phrase
                            )
                        size = page.get_size()
                        check(
                            all(
                                abs(a - b) <= 0.5
                                for a, b in zip(size, fixed["page_size_pt"])
                            ),
                            "PDFium A4 size",
                        )
                        image = page.render(scale=1).to_pil()
                        check(
                            image.getextrema() != ((255, 255),) * 3, "nonblank render"
                        )
                        image.save(render / f"{name}-{index + 1}-pdfium.png")
                    finally:
                        page.close()
            if source == "pages-images.docx":
                with pdfium.PdfDocument(path) as image_document:
                    image_page = image_document[0]
                    try:
                        objects = list(
                            image_page.get_objects(
                                filter=[pdfium.raw.FPDF_PAGEOBJ_IMAGE]
                            )
                        )
                        check(len(objects) == 1, "PDFium one raster")
                        left, _, right, _ = objects[0].get_bounds()
                        check(
                            abs(right - left - fixed["image_physical_width_pt"]) <= 0.5,
                            "DOCX image physical width",
                        )
                    finally:
                        image_page.close()
                images = list(reader.pages[0].images)
                check(len(images) == 1, "one DOCX raster")
                image = images[0].image.convert("RGB")
                check(list(image.size) == fixed["image_size"], "image pixel dimensions")
                check(
                    hashlib.sha256(image.tobytes()).hexdigest()
                    == fixed["image_rgb_sha256"],
                    "lossless source image bytes",
                )
            completed = subprocess.run(
                [
                    str(args.poppler),
                    "-r",
                    "72",
                    "-png",
                    str(path),
                    str(render / (name + "-poppler")),
                ],
                capture_output=True,
                timeout=60,
                check=True,
            )
            check(
                len(list(render.glob(name + "-poppler-*.png"))) == len(expected),
                "Poppler page renders",
            )
            row.update(
                status="PASS",
                poppler_stderr=completed.stderr.decode("utf-8", errors="replace"),
            )
        except Exception as error:
            row.update(status="FAIL", error=str(error))
        results.append(row)
    report = {
        "criteria_sha256": CRITERIA_SHA,
        "cases": results,
        "failures": sum(row["status"] != "PASS" for row in results),
    }
    destination = args.output / ("office-independent-" + args.attempt + ".json")
    with destination.open("x", encoding="utf-8") as file:
        json.dump(report, file, ensure_ascii=False, indent=2)
    print(json.dumps(report, ensure_ascii=False, indent=2))
    return bool(report["failures"])


if __name__ == "__main__":
    raise SystemExit(main())
