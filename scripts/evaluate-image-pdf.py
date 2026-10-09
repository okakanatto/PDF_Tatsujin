"""Inspect M4 image-created PDFs with pypdf, PDFium and Poppler.

Synthetic reference pixels are decoded and saved before embedding. No fixed M1
fixtures, ground truth or acceptance thresholds are changed by this evaluator.
"""

import argparse
import hashlib
import json
from pathlib import Path
import subprocess

from PIL import Image, ImageChops, ImageOps
import pypdfium2 as pdfium
from pypdf import PdfReader
from pypdf.generic import ContentStream

from test_tools import poppler_tool


def check(condition, message):
    if not condition:
        raise RuntimeError(message)


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def compare_images(first, second):
    first, second = first.convert("RGBA"), second.convert("RGBA")
    check(first.size == second.size, "Extracted image dimensions changed")
    difference = ImageChops.difference(first, second)
    return max(high for _, high in difference.getextrema())


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("directory", type=Path)
    args = parser.parse_args()
    directory = args.directory.resolve()
    output = directory / "independent-image-pdf.json"
    check(not output.exists(), "Preserve previous independent results")
    suite = json.loads((directory / "selftest.json").read_text("utf-8-sig"))
    process = json.loads((directory / "process-result.json").read_text("utf-8-sig"))
    check(
        process["completed"] and process["report_valid"] and process["exit_code"] == 0,
        "Run must have a valid passing report",
    )
    cases = [test for test in suite["tests"] if test["name"].startswith("M4I")]
    check(
        len(cases) == 5 and all(test["status"] == "PASS" for test in cases),
        "All five M4 image cases must pass before evaluating the complete workflow",
    )
    references = json.loads((directory / "m4-image-references.json").read_text("utf-8"))
    result = {
        "executable_sha256": process["exe_sha256"],
        "engines": {
            "pdfium": str(pdfium.PDFIUM_INFO),
            "poppler": str(poppler_tool("pdftoppm")),
            "structure_and_images": "pypdf",
        },
        "scope": "Synthetic PNG/JPEG; decoded pixels, EXIF, geometry, independent renders and derived bilingual OCR. Not native Reader GUI, OS clipboard or physical printing.",
        "files": {},
    }
    with Image.open(directory / "m4-two-frames.png") as animated:
        check(
            animated.n_frames == 2 and animated.is_animated,
            "Rejected APNG fixture must contain two actual frames",
        )
        animated.seek(1)
        animated.load()
    result["rejected_APNG_fixture"] = {
        "frames": 2,
        "second_frame_loaded_independently": True,
    }
    for name, physical in (("m4-images-a4.pdf", False), ("m4-images-150dpi.pdf", True)):
        path = directory / name
        document = PdfReader(path)
        rendered = pdfium.PdfDocument(path)
        check(
            len(document.pages) == len(references["references"]) == 4,
            "Page count/order changed",
        )
        rows = []
        for index, (page, reference) in enumerate(
            zip(document.pages, references["references"])
        ):
            source = Path(references["inputs_directory"]) / reference["input"]
            check(sha(source) == reference["input_sha256"], "Source image changed")
            expected = Image.open(directory / reference["reference"]).convert("RGBA")
            with Image.open(source) as original:
                oriented = ImageOps.exif_transpose(original)
                check(oriented.size == expected.size, "EXIF rotation is incorrect")
            images = list(page.images)
            check(len(images) == 1, "Expected one full image per page")
            pixels = compare_images(images[0].image, expected)
            check(
                pixels == 0,
                "Embedded decoded pixels or alpha differ from the reference",
            )
            width, height = float(page.mediabox.width), float(page.mediabox.height)
            if physical:
                target = (expected.width * 72 / 150, expected.height * 72 / 150)
                margin = 0
            else:
                target = (
                    (297 * 72 / 25.4, 210 * 72 / 25.4)
                    if expected.width > expected.height
                    else (210 * 72 / 25.4, 297 * 72 / 25.4)
                )
                margin = 10 * 72 / 25.4
            check(
                abs(width - target[0]) < 0.01 and abs(height - target[1]) < 0.01,
                "Physical page size/direction changed",
            )
            operations = ContentStream(page.get_contents(), document).operations
            transforms = [
                list(map(float, operands))
                for operands, operator in operations
                if operator == b"cm"
            ]
            check(len(transforms) == 1, "Image placement must have one transform")
            w, b, c, h, x, y = transforms[0]
            check(
                b == c == 0 and w > 0 and h > 0, "Image is unexpectedly skewed/flipped"
            )
            check(
                abs(w / h - expected.width / expected.height) < 1e-8,
                "Aspect ratio changed",
            )
            check(
                x >= margin - 0.01
                and y >= margin - 0.01
                and x + w <= width - margin + 0.01
                and y + h <= height - margin + 0.01,
                "Image is cropped or exceeds margins",
            )
            if physical:
                check(
                    abs(x) < 0.01
                    and abs(y) < 0.01
                    and abs(w - width) < 0.01
                    and abs(h - height) < 0.01,
                    "Image-size mode leaves an unexpected border",
                )
            else:
                check(
                    min(abs(x - margin), abs(y - margin)) < 0.01,
                    "A4 fit did not use the available area",
                )
            bitmap = rendered[index].render(scale=1).to_pil().convert("RGB")
            for u, v in ((0.25, 0.25), (0.75, 0.25), (0.25, 0.75), (0.75, 0.75)):
                rgba = expected.getpixel(
                    (int(expected.width * u), int(expected.height * v))
                )
                color = tuple(
                    round(channel * rgba[3] / 255 + 255 - rgba[3])
                    for channel in rgba[:3]
                )
                actual = bitmap.getpixel(
                    (
                        min(bitmap.width - 1, round(x + w * u)),
                        min(bitmap.height - 1, round(height - y - h + h * v)),
                    )
                )
                check(
                    max(abs(a - b) for a, b in zip(color, actual)) <= 2,
                    "Independent rendered quadrant/orientation/alpha differs",
                )
            bitmap.save(directory / f"{path.stem}-pdfium-{index + 1}.png")
            rows.append(
                {
                    "page": index + 1,
                    "decoded_pixel_max_difference": pixels,
                    "width_pixels": expected.width,
                    "height_pixels": expected.height,
                    "page_points": [width, height],
                    "placement": transforms[0],
                    "status": "PASS",
                }
            )
        result["files"][name] = {"sha256": sha(path), "pages": rows}
        rendered.close()
    before = pdfium.PdfDocument(directory / "m4-scan-before.pdf")
    after = pdfium.PdfDocument(directory / "m4-scan-ocr-signed.pdf")
    check(len(before) == len(after) == 2, "Derived OCR document pages changed")
    ocr = []
    for index, term in enumerate(("市民公園", "coastal")):
        text = after[index].get_textpage().get_text_range()
        check(
            term.casefold() in text.casefold(),
            "Independent OCR search/copy text is missing",
        )
        visible_difference = compare_images(
            before[index].render(scale=1.5).to_pil(),
            after[index].render(scale=1.5).to_pil(),
        )
        check(visible_difference == 0, "OCR changed the visible image or signature")
        ocr.append(
            {
                "page": index + 1,
                "term": term,
                "text_characters": len(text),
                "visible_max_difference": visible_difference,
                "status": "PASS",
            }
        )
    structure = PdfReader(directory / "m4-scan-ocr-signed.pdf")
    annotations = structure.pages[0].get("/Annots", [])
    check(len(annotations) == 1, "Placed signature was lost/duplicated")
    annotation = annotations[0].get_object()
    check(
        annotation.get("/AP") and annotation.get("/Tatsujin") and annotation["/F"] & 4,
        "Signature appearance, reediting or print flag missing",
    )
    result["derived_OCR"] = ocr
    before.close()
    after.close()
    for name in (
        "m4-images-a4.pdf",
        "m4-images-150dpi.pdf",
        "m4-window-signed.pdf",
        "m4-scan-ocr-signed.pdf",
    ):
        prefix = directory / (Path(name).stem + "-poppler")
        subprocess.run(
            [
                str(poppler_tool("pdftoppm")),
                "-r",
                "72",
                "-png",
                str(directory / name),
                str(prefix),
            ],
            check=True,
            capture_output=True,
            creationflags=subprocess.CREATE_NO_WINDOW,
        )
        expected_pages = len(PdfReader(directory / name).pages)
        renders = list(directory.glob(prefix.name + "-*.png"))
        check(len(renders) == expected_pages, "Poppler did not render every page")
        for image in renders:
            with Image.open(image) as rendered:
                rendered.load()
                check(
                    rendered.width > 0 and rendered.height > 0, "Empty Poppler render"
                )
    result["status"] = "PASS"
    output.write_text(
        json.dumps(result, ensure_ascii=False, indent=2) + "\n", encoding="utf-8"
    )
    print(
        "PASS: 8 exact embedded images, geometry/EXIF/alpha, independent bilingual OCR and all Poppler pages"
    )


if __name__ == "__main__":
    main()
