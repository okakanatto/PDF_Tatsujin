"""Freeze a supplementary vertical criterion without regenerating D11."""

import hashlib
import json
from pathlib import Path

from PIL import Image, ImageDraw, ImageFont

ROOT = Path(__file__).resolve().parents[1]


def main():
    source = ROOT / "fixtures/D11-diagnostics.pdf"
    manifest = json.loads((ROOT / "fixtures/manifest.json").read_text("utf-8"))
    original = next(row for row in manifest if row["id"] == "D11")
    digest = hashlib.sha256(source.read_bytes()).hexdigest()
    assert digest == original["sha256"]
    image = Image.open(ROOT / "fixtures/scan-3.png")
    draw = ImageDraw.Draw(Image.new("RGB", image.size))
    font = ImageFont.truetype(str(ROOT / "fixtures/NotoSansJP-fixture.ttf"), 46)
    lines = [
        "縦書き文章の診断です。",
        "基準精度とは分けます。",
        "日本語の読み順を確認。",
    ]
    searches = []
    for column, (line, term) in enumerate(zip(lines, ["縦書き", "基準精度", "読み順"])):
        start = line.index(term)
        boxes = [
            draw.textbbox((2150 - column * 90, 220 + row * 65), line[row], font=font)
            for row in range(start, start + len(term))
        ]
        left = min(box[0] for box in boxes)
        top = min(box[1] for box in boxes)
        right = max(box[2] for box in boxes)
        bottom = max(box[3] for box in boxes)
        sx, sy = 595.276 / image.width, 841.89 / image.height
        searches.append(
            dict(
                term=term,
                qt_bounds_pt=[
                    left * sx,
                    top * sy,
                    (right - left) * sx,
                    (bottom - top) * sy,
                ],
            )
        )
    result = dict(
        source=source.name,
        source_sha256=digest,
        page=3,
        provenance="Existing synthetic D11 CC0; embedded Noto retains OFL. Original bytes and truth unchanged.",
        expected_lines_right_to_left=lines,
        comparison="Remove whitespace only; keep all other characters and column order",
        CER_max=0.02,
        search_terms=searches,
        maximum_bounds_error_mm=2,
        visible_pixel_difference_max=0,
        real_scan="R01 is diagnostic only; retain original real-scans/manifest.json",
        fixture_font_sha256=hashlib.sha256(
            (ROOT / "fixtures/NotoSansJP-fixture.ttf").read_bytes()
        ).hexdigest(),
    )
    with (ROOT / "fixtures/vertical-ocr-criteria.json").open(
        "x", encoding="utf-8"
    ) as file:
        json.dump(result, file, ensure_ascii=False, indent=2)
        file.write("\n")
    print(
        hashlib.sha256(
            (ROOT / "fixtures/vertical-ocr-criteria.json").read_bytes()
        ).hexdigest()
    )


if __name__ == "__main__":
    main()
