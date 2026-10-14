"""Embed unaltered, public-domain book-scan pages as image-only diagnostic PDFs.

Ground truth is frozen before OCR. These old type/ruby/vertical inputs are not
substitutes for D03's fixed horizontal-print acceptance corpus.
"""

import hashlib
import json
from pathlib import Path
from PIL import Image
from reportlab.pdfgen import canvas

root = Path(__file__).resolve().parents[1] / "fixtures/real-scans"
manifest = root / "manifest.json"
if manifest.exists():
    raise RuntimeError("Preserve frozen real-scan inputs and truth")

entries = [
    {
        "id": "R01",
        "language": "jpn",
        "image": "rashomon-page6.jpg",
        "pdf": "R01.pdf",
        "source": "https://commons.wikimedia.org/wiki/File:Rashomon.djvu",
        "image_url": "https://thumb.wikimedia.org/wikipedia/commons/thumb/2/2a/Rashomon.djvu/page6-1920px-Rashomon.djvu.jpg",
        "origin": "1917 Rashomon, Ryunosuke Akutagawa; National Diet Library scan, DjVu page 6",
        "reuse": "Public domain original and mechanical scan; Commons PD-scan/PD-old-95-expired/PDM 1.0",
        "direction": "vertical, old orthography, ruby, two-page spread",
        "truth_scope": "Main text only; ruby is not transcribed. Raw CER is diagnostic and includes ruby-related mismatches; not an M1 threshold judgment.",
        "search_terms": ["羅生門", "京都", "地震", "佛像"],
        "truth": "羅生門\n或日の暮方の事である。一人の下人が、羅生門の下で雨やみを待つてゐた。\n廣い門の下には、この男の外に誰もゐない。唯、所々丹塗の剝げた、大きな圓柱に、蟋蟀が一匹とまつてゐる。羅生門が、朱雀大路にある以上は、この男の外にも、雨やみをする市女笠や揉烏帽子が、もう二三人はありさうなものである。それが、この男の外には誰もゐない。\n何故かと云ふと、この二三年、京都には、地震とか辻風とか火事とか饑饉とか云ふ災がつゞいて起つた。そこで洛中のさびれ方は一通りでない。舊記によると、佛像や佛具を打碎いて、その丹がついたり、金銀の箔がついたりした木を、路ばたにつみ重ねて、薪の料に賣つてゐたと云ふ事である。洛中が\n1",
    },
    {
        "id": "R02",
        "language": "eng",
        "image": "yellow-wall-paper-page13.jpg",
        "pdf": "R02.pdf",
        "source": "https://commons.wikimedia.org/wiki/File:The_Yellow_Wall_Paper.djvu",
        "image_url": "https://thumb.wikimedia.org/wikipedia/commons/thumb/a/a6/The_Yellow_Wall_Paper.djvu/page13-1920px-The_Yellow_Wall_Paper.djvu.jpg",
        "origin": "1901 The Yellow Wall Paper, Charlotte Perkins Stetson/Gilman; Google Books/Internet Archive scan, DjVu page 13",
        "reuse": "Public domain original; Commons PD-old-80-expired/PDM 1.0",
        "direction": "horizontal, old serif print, slight skew, italics",
        "truth_scope": "Full visible page including heading/page number and printed line-end hyphen. Transcribed from the scan before OCR.",
        "search_terms": ["phosphates", "Personally", "congenial", "opposition"],
        "truth": "THE YELLOW WALL PAPER\nalso of high standing, and he says the same thing.\nSo I take phosphates or phosphites, — whichever it is, — and tonics, and journeys, and air, and exercise, and am absolutely forbidden to “work” until I am well again.\nPersonally I disagree with their ideas.\nPersonally I believe that congenial work, with excitement and change, would do me good.\nBut what is one to do?\nI did write for a while in spite of them; but it does exhaust me a good deal — having to be so sly about it, or else meet with heavy opposition.\nI sometimes fancy that in my con-\ndition if I had less opposition and more\n3",
    },
]
for entry in entries:
    source = root / entry["image"]
    image = Image.open(source)
    dimensions = tuple(value * 72 / 300 for value in image.size)
    target = root / entry["pdf"]
    if target.exists():
        raise RuntimeError("Do not overwrite real-scan PDF")
    document = canvas.Canvas(
        str(target), pagesize=dimensions, pageCompression=1, invariant=1
    )
    document.drawImage(str(source), 0, 0, width=dimensions[0], height=dimensions[1])
    document.showPage()
    document.save()
    entry["source_sha256"] = hashlib.sha256(source.read_bytes()).hexdigest()
    entry["pdf_sha256"] = hashlib.sha256(target.read_bytes()).hexdigest()
    entry["pixels"] = image.size
    entry["embedded_dpi"] = 300
manifest.write_text(
    json.dumps(entries, ensure_ascii=False, indent=2) + "\n", encoding="utf-8"
)
print("Created two real-scan diagnostic PDFs with frozen provenance and truth")
