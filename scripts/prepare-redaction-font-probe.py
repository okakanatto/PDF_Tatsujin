"""Freeze Japanese subset-font redaction inputs before diagnostic execution."""

import argparse
import hashlib
import json
from pathlib import Path

from reportlab.pdfbase import pdfmetrics
from reportlab.pdfbase.ttfonts import TTFont
from reportlab.pdfgen import canvas


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=False)
    root = Path(__file__).resolve().parents[1]
    font = root / "assets/fonts/NotoSansJP.ttf"
    pdfmetrics.registerFont(TTFont("SyntheticJapanese", str(font)))
    rows = []
    for name, mode in (("visible", 0), ("invisible-OCR", 3)):
        path = args.output / (name + ".pdf")
        pdf = canvas.Canvas(str(path), pagesize=(612, 792))
        pdf.setFont("Helvetica", 16)
        pdf.drawString(60, 730, "KEEP_OUTSIDE_FONT_PROBE")
        text = pdf.beginText(60, 650)
        text.setFont("SyntheticJapanese", 22)
        text.setTextRenderMode(mode)
        text.textOut("秘匿機密亀鶴")
        pdf.drawText(text)
        pdf.showPage()
        pdf.save()
        rows.append(
            dict(
                file=path.name,
                sha256=hashlib.sha256(path.read_bytes()).hexdigest(),
                remove="秘匿機密亀鶴",
                keep="KEEP_OUTSIDE_FONT_PROBE",
                mode=mode,
            )
        )
    (args.output / "plan.json").write_text(
        json.dumps([dict(page=0, rect=[48, 620, 420, 65])]), encoding="utf-8"
    )
    (args.output / "criteria.json").write_text(
        json.dumps(
            dict(
                cases=rows,
                font_sha256=hashlib.sha256(font.read_bytes()).hexdigest(),
                required=[
                    "target text absent from visible and invisible text and recoverable subset mappings",
                    "outside text preserved",
                    "source unchanged or unsupported operation rejected without publication",
                ],
                scope="Synthetic font side-channel evaluation; not general security acceptance",
            ),
            ensure_ascii=False,
            indent=2,
        ),
        encoding="utf-8",
    )


if __name__ == "__main__":
    main()
