"""Verify image exports using Pillow and an independent Poppler render."""

import argparse
import hashlib
import json
import math
from pathlib import Path
import subprocess

from PIL import Image, ImageChops, ImageStat
from pypdf import PdfReader

from test_tools import poppler_tool


def check(condition, message):
    if not condition:
        raise RuntimeError(message)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("directory", type=Path)
    args = parser.parse_args()
    directory = args.directory.resolve()
    output = directory / "independent-image-export.json"
    check(not output.exists(), "Preserve previous independent export results")
    suite = json.loads((directory / "selftest.json").read_text("utf-8-sig"))
    process = json.loads((directory / "process-result.json").read_text("utf-8-sig"))
    check(
        process["completed"] and process["report_valid"] and process["exit_code"] == 0,
        "Valid passing run required",
    )
    cases = [case for case in suite["tests"] if case["name"].startswith("M4E")]
    check(
        len(cases) == 3 and all(case["status"] == "PASS" for case in cases),
        "Three export cases must pass",
    )
    manifest = json.loads((directory / "m4-export-images.json").read_text("utf-8"))
    result = {
        "executable_sha256": process["exe_sha256"],
        "PNG": [],
        "scope": "Pillow pixels/dpi, independent Poppler geometry/content. Not native OS image viewer or physical printing.",
    }
    source = PdfReader(directory / "m4-export-reference.pdf")
    check(len(source.pages) == 4, "Export reference PDF has four pages")
    for index, row in enumerate(manifest["PNG"]):
        path = directory / "m4-export-png" / row["path"]
        page = source.pages[index]
        unit = float(page.get("/UserUnit", 1))
        width, height = (
            float(page.cropbox.width) * unit,
            float(page.cropbox.height) * unit,
        )
        if int(page.get("/Rotate", 0)) % 180:
            width, height = height, width
        physical_pixels = (
            math.floor(width * row["dpi"] / 72 + 0.5),
            math.floor(height * row["dpi"] / 72 + 0.5),
        )
        # The pinned pdftoppm uses raw coordinate-space dpi and MediaBox by
        # default. Request CropBox and scale dpi by this page's UserUnit. Do
        # not resize its output bitmap to match the product after rendering.
        prefix = directory / f"m4-export-poppler-crop-{index + 1}"
        subprocess.run(
            [
                str(poppler_tool("pdftoppm")),
                "-f",
                str(index + 1),
                "-l",
                str(index + 1),
                "-cropbox",
                "-r",
                str(row["dpi"] * unit),
                "-png",
                "-singlefile",
                str(directory / "m4-export-reference.pdf"),
                str(prefix),
            ],
            check=True,
            capture_output=True,
            creationflags=subprocess.CREATE_NO_WINDOW,
        )
        with Image.open(path) as image, Image.open(
            directory / row["reference"]
        ) as reference, Image.open(prefix.with_suffix(".png")) as external:
            image.load()
            check(
                image.format == "PNG"
                and image.size == reference.size == (row["width"], row["height"]),
                "PNG size/format changed",
            )
            check(
                image.size == physical_pixels,
                "Independent CropBox/UserUnit/rotation/dpi dimensions disagree",
            )
            maximum = max(
                high
                for low, high in ImageChops.difference(
                    image.convert("RGB"), reference.convert("RGB")
                ).getextrema()
            )
            check(maximum == 0, "PNG is not lossless relative to the actual render")
            check(
                all(
                    abs(value - row["dpi"]) < 0.1
                    for value in image.info.get("dpi", (0, 0))
                ),
                "PNG dpi is wrong",
            )
            # Poppler uses ceil while the Qt renderer uses nearest integer. The
            # expected one-pixel border difference is recorded explicitly.
            check(
                abs(image.width - external.width) <= 1
                and abs(image.height - external.height) <= 1,
                "Independent page orientation/physical dimensions disagree",
            )
            gray = external.convert("L")
            check(gray.getextrema()[0] < 200, "Independent page content is blank")
            result["PNG"].append(
                {
                    "page": row["page"],
                    "sha256": hashlib.sha256(path.read_bytes()).hexdigest(),
                    "size": image.size,
                    "dpi": image.info["dpi"],
                    "max_pixel_difference": maximum,
                    "Poppler_size": external.size,
                    "physical_pixels_from_PDF": physical_pixels,
                    "UserUnit": unit,
                    "Poppler_requested_dpi": row["dpi"] * unit,
                    "status": "PASS",
                }
            )
    jpeg = manifest["JPEG"]
    with Image.open(directory / jpeg["path"]) as image, Image.open(
        directory / jpeg["reference"]
    ) as reference:
        image.load()
        check(
            image.format == "JPEG" and image.size == reference.size,
            "JPEG size/format changed",
        )
        mean = (
            sum(
                ImageStat.Stat(
                    ImageChops.difference(
                        image.convert("RGB"), reference.convert("RGB")
                    )
                ).mean
            )
            / 3
        )
        check(
            mean <= 3,
            "Fixed digital JPEG exceeds the predeclared mean difference limit",
        )
        check(
            all(
                abs(value - jpeg["dpi"]) < 1 for value in image.info.get("dpi", (0, 0))
            ),
            "JPEG dpi is wrong",
        )
        result["JPEG"] = {
            "size": image.size,
            "dpi": image.info["dpi"],
            "mean_absolute_channel_difference": mean,
            "limit": 3,
            "status": "PASS",
        }
    check(
        len(list((directory / "m4-export-ui").glob("*.png"))) == 2,
        "UI selected range did not produce exactly two images",
    )
    result["UI_range"] = [2, 4]
    result["status"] = "PASS"
    output.write_text(
        json.dumps(result, ensure_ascii=False, indent=2) + "\n", encoding="utf-8"
    )
    print(
        "PASS: lossless PNG pixels/dpi, JPEG quality/dpi, Poppler page geometry/content and selected range"
    )


if __name__ == "__main__":
    main()
