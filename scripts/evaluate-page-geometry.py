"""Check colored vector positions against PDF coordinates and Poppler."""

import argparse
import json
from pathlib import Path
import subprocess

from PIL import Image, ImageChops
from pypdf import PdfReader

from test_tools import poppler_tool


def marker_box(image, color):
    masks = [
        channel.point(
            lambda value, expected=expected: 255 if abs(value - expected) <= 24 else 0
        )
        for channel, expected in zip(image.convert("RGB").split(), color)
    ]
    return ImageChops.multiply(
        ImageChops.multiply(masks[0], masks[1]), masks[2]
    ).getbbox()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("directory", type=Path)
    args = parser.parse_args()
    directory = args.directory.resolve()
    output = directory / "independent-page-geometry.json"
    if output.exists():
        raise RuntimeError("Preserve earlier geometry results")
    process = json.loads((directory / "process-result.json").read_text("utf-8-sig"))
    suite = json.loads((directory / "selftest.json").read_text("utf-8-sig"))
    cases = [row for row in suite["tests"] if row["name"].startswith("Geometry")]
    valid = (
        process["completed"] and process["report_valid"] and process["exit_code"] == 0
    )
    valid = valid and len(cases) == 2 and all(row["status"] == "PASS" for row in cases)
    source = PdfReader(directory / "geometry-markers.pdf")
    manifest = json.loads((directory / "geometry-markers.json").read_text("utf-8"))
    rows = []
    for row, page in zip(manifest, source.pages):
        number = row["page"]
        unit = float(page.get("/UserUnit", 1))
        scale = row["scale"]
        crop = tuple(float(value) for value in page.cropbox)
        angle = int(page.get("/Rotate", 0)) % 360

        def expected_point(x, y):
            x0, y0, x1, y1 = crop
            point = {
                0: (x - x0, y1 - y),
                90: (y - y0, x - x0),
                180: (x1 - x, y - y0),
                270: (y1 - y, x1 - x),
            }[angle]
            return tuple(value * unit * scale for value in point)

        prefix = directory / f"geometry-poppler-{number}"
        subprocess.run(
            [
                str(poppler_tool("pdftoppm")),
                "-f",
                str(number),
                "-l",
                str(number),
                "-cropbox",
                "-r",
                str(72 * scale * unit),
                "-png",
                "-singlefile",
                str(directory / "geometry-markers.pdf"),
                str(prefix),
            ],
            check=True,
            capture_output=True,
            creationflags=subprocess.CREATE_NO_WINDOW,
        )
        with Image.open(
            directory / f"geometry-render-{number}.png"
        ) as render, Image.open(
            directory / f"geometry-preview-{number}.png"
        ) as preview, Image.open(
            prefix.with_suffix(".png")
        ) as external:
            preview_equal = (
                render.size == preview.size
                and ImageChops.difference(
                    render.convert("RGB"), preview.convert("RGB")
                ).getbbox()
                is None
            )
            for marker in row["markers"]:
                x, y, w, h = marker["PDF_rectangle"]
                points = [expected_point(a, b) for a in (x, x + w) for b in (y, y + h)]
                expected = [
                    min(p[0] for p in points),
                    min(p[1] for p in points),
                    max(p[0] for p in points),
                    max(p[1] for p in points),
                ]
                actual = marker_box(render, marker["color"])
                reference = marker_box(external, marker["color"])
                error = (
                    max(abs(a - b) for a, b in zip(actual, expected))
                    if actual
                    else None
                )
                reference_error = (
                    max(abs(a - b) for a, b in zip(reference, expected))
                    if reference
                    else None
                )
                difference = (
                    max(abs(a - b) for a, b in zip(actual, reference))
                    if actual and reference
                    else None
                )
                passed = preview_equal and all(
                    value is not None and value <= 2
                    for value in (error, reference_error, difference)
                )
                rows.append(
                    {
                        "page": number,
                        "rotation": angle,
                        "UserUnit": unit,
                        "color": marker["color"],
                        "PDF_rectangle": marker["PDF_rectangle"],
                        "expected_pixels": expected,
                        "Qt_pixels": actual,
                        "Poppler_pixels": reference,
                        "Qt_geometry_error_px": error,
                        "Poppler_geometry_error_px": reference_error,
                        "cross_engine_error_px": difference,
                        "limit_px": 2,
                        "preview_pixels_equal": preview_equal,
                        "status": "PASS" if passed else "FAIL",
                    }
                )
    passed = (
        valid
        and len(source.pages) == len(manifest) == 4
        and len(rows) == 16
        and all(row["status"] == "PASS" for row in rows)
    )
    result = {
        "status": "PASS" if passed else "FAIL",
        "executable_sha256": process["exe_sha256"],
        "product_geometry_cases_passed": valid,
        "markers": rows,
        "scope": "Synthetic colored vectors, PDF physical coordinates, preview pixels, independent Poppler; no native OS viewer acceptance",
    }
    output.write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
    if not passed:
        raise RuntimeError(
            "Page geometry failed; diagnostics preserved in independent-page-geometry.json"
        )
    print(
        "PASS: 16 colored vectors, physical PDF axes and independent Poppler positions"
    )


if __name__ == "__main__":
    main()
