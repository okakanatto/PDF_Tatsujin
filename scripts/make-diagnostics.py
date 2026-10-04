"""Additional diagnostic inputs; never included in baseline CER."""

from pathlib import Path
import json, hashlib
from PIL import Image, ImageDraw, ImageFont
from reportlab.pdfgen import canvas
from reportlab.lib.utils import ImageReader

ROOT = Path(__file__).resolve().parents[1]
FX = ROOT / "fixtures"
base = Image.open(FX / "scan-3.png").convert("RGB")
font = ImageFont.truetype(str(FX / "NotoSansJP-fixture.ttf"), 46)
pages = []
pages.append(("low-100dpi", base.resize((827, 1169), Image.Resampling.LANCZOS), 100))
pages.append(("skew-3-degrees", base.rotate(3, expand=False, fillcolor="white"), 300))
vertical = Image.new("RGB", base.size, "white")
draw = ImageDraw.Draw(vertical)
for col, text in enumerate(
    ["縦書き文章の診断です。", "基準精度とは分けます。", "日本語の読み順を確認。"]
):
    for row, char in enumerate(text):
        draw.text((2150 - col * 90, 220 + row * 65), char, font=font, fill="black")
pages.append(("vertical", vertical, 300))
columns = Image.new("RGB", base.size, "white")
draw = ImageDraw.Draw(columns)
for col in range(2):
    for row in range(14):
        draw.text(
            (150 + col * 1180, 220 + row * 80),
            f"{col+1}段目の資料、項目{row+1}。",
            font=font,
            fill="black",
        )
pages.append(("two-columns", columns, 300))
table = Image.new("RGB", base.size, "white")
draw = ImageDraw.Draw(table)
for row in range(8):
    for col in range(3):
        x, y = 180 + col * 650, 300 + row * 130
        draw.rectangle((x, y, x + 650, y + 130), outline="black", width=3)
        draw.text(
            (x + 20, y + 20),
            (
                ["項目", "数量", "確認"][col]
                if row == 0
                else [f"資料{row}", str(row * 12), "済"][col]
            ),
            font=font,
            fill="black",
        )
pages.append(("table", table, 300))
out = FX / "D11-diagnostics.pdf"
c = canvas.Canvas(str(out), pagesize=(595.276, 841.89))
for _, img, _ in pages:
    c.drawImage(ImageReader(img), 0, 0, 595.276, 841.89)
    c.showPage()
c.save()
path = FX / "manifest.json"
manifest = json.loads(path.read_text(encoding="utf-8"))
manifest = [m for m in manifest if m["id"] != "D11"]
manifest.append(
    {
        "id": "D11",
        "file": out.name,
        "sha256": hashlib.sha256(out.read_bytes()).hexdigest(),
        "pages": 5,
        "origin": "scripts/make-diagnostics.py",
        "license": "synthetic CC0 / embedded Noto OFL",
        "language": "jpn",
        "diagnostic_only": True,
        "conditions": [
            {"page": i + 1, "kind": name, "dpi": dpi}
            for i, (name, _, dpi) in enumerate(pages)
        ],
        "real_scanner": "未準備・未実行",
    }
)
path.write_text(json.dumps(manifest, ensure_ascii=False, indent=2), encoding="utf-8")
print(out.name)
