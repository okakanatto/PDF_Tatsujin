"""Execute real vertical OCR on frozen crop/rotation/UserUnit input."""

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess

from PIL import ImageChops
import pypdfium2 as pdfium
from pypdf import PdfReader

ROOT = Path(__file__).resolve().parents[1]
CRITERIA_SHA = "4e087d6138ce25f08c7f5cc70b0b2ee2b85a7c6da69bab370d2eb74ed9417ac8"


def check(value, reason):
    if not value:
        raise AssertionError(reason)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--app-directory", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()
    args.output.mkdir(exist_ok=False)
    criterion = ROOT / "fixtures/vertical-ocr-geometry/criteria.json"
    check(
        hashlib.sha256(criterion.read_bytes()).hexdigest() == CRITERIA_SHA,
        "Frozen geometry criterion",
    )
    fixed = json.loads(criterion.read_text("utf-8"))
    source = criterion.parent / fixed["source"]
    check(
        hashlib.sha256(source.read_bytes()).hexdigest() == fixed["source_sha256"],
        "Frozen geometry source",
    )
    exe = args.app_directory.resolve() / "PDFTatsujin.exe"
    target = args.output.resolve() / "vertical-geometry.pdf"
    options = args.output.resolve() / "options.json"
    options.write_text(
        json.dumps(dict(pages=str(fixed["page"]), language="jpn_vert")), "utf-8"
    )
    env = os.environ.copy()
    env["PATH"] = str(
        Path(env.get("SystemRoot", env.get("SYSTEMROOT", "C:/Windows"))) / "System32"
    )
    env["QT_QPA_PLATFORM"] = "offscreen"
    env.pop("TATSU_ASSETS", None)
    env.pop("QT_PLUGIN_PATH", None)
    process = subprocess.run(
        [
            str(exe),
            "--ocr-worker",
            str(source),
            str(target),
            str(options),
            str(args.output.resolve() / "worker-report.json"),
        ],
        env=env,
        capture_output=True,
        timeout=180,
    )
    (args.output / "stderr.txt").write_bytes(process.stderr)
    (args.output / "stdout.txt").write_bytes(process.stdout)
    result = dict(
        exit=process.returncode,
        executable_sha256=hashlib.sha256(exe.read_bytes()).hexdigest(),
        criteria_sha256=CRITERIA_SHA,
        status="FAIL",
    )
    try:
        check(process.returncode == 0, "Real geometry OCR process")
        reader = PdfReader(target, strict=True)
        page = reader.pages[fixed["page"] - 1]
        check(
            page.rotation == fixed["rotation"]
            and float(page.get("/UserUnit")) == fixed["UserUnit"]
            and list(map(float, page.cropbox)) == fixed["cropbox"],
            "Geometry metadata preserved",
        )
        expected = "".join(fixed["expected_lines_right_to_left"])
        check(
            re.sub(r"\s+", "", page.extract_text()) == expected,
            "pypdf exact text and order",
        )
        with pdfium.PdfDocument(source) as before, pdfium.PdfDocument(target) as after:
            p = after[fixed["page"] - 1]
            text = p.get_textpage()
            copied = text.get_text_range()
            check(
                re.sub(r"\s+", "", copied) == expected,
                "PDFium exact text and column order",
            )
            check(
                all(line in copied for line in fixed["expected_lines_right_to_left"]),
                "Contiguous column copy",
            )
            searches = []
            for item in fixed["search_terms"]:
                finder = text.search(item["term"])
                hit = finder.get_next()
                check(hit is not None, "Fixed rotated search term")
                finder.close()
                boxes = [
                    text.get_charbox(i)
                    for i in range(hit[0], hit[0] + hit[1])
                    if not text.get_text_range(i, 1).isspace()
                ]
                left, bottom = min(b[0] for b in boxes), min(b[1] for b in boxes)
                right, top = max(b[2] for b in boxes), max(b[3] for b in boxes)
                unit = fixed["UserUnit"]
                actual = [
                    (left - fixed["cropbox"][0]) * unit,
                    (fixed["cropbox"][3] - top) * unit,
                    (right - left) * unit,
                    (top - bottom) * unit,
                ]
                error = (
                    max(abs(a - b) for a, b in zip(actual, item["qt_bounds_pt"]))
                    * 25.4
                    / 72
                )
                check(
                    error <= fixed["maximum_bounds_error_mm"],
                    "Unchanged 2mm physical search criterion",
                )
                searches.append(dict(term=item["term"], maximum_bounds_error_mm=error))
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
            result.update(
                status="PASS",
                copied=copied,
                searches=searches,
                visible_pixel_difference=0,
                geometry_metadata_preserved=True,
            )
    except Exception as error:
        result["error"] = str(error)
    check(
        hashlib.sha256(source.read_bytes()).hexdigest() == fixed["source_sha256"],
        "Source bytes retained",
    )
    with (args.output / "result.json").open("x", encoding="utf-8") as out:
        json.dump(result, out, ensure_ascii=False, indent=2)
        out.write("\n")
    print(json.dumps(result, ensure_ascii=False, indent=2))
    raise SystemExit(result["status"] != "PASS")


if __name__ == "__main__":
    main()
