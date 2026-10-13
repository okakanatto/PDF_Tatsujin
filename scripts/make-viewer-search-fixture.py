"""Generate the additional, frozen viewer-search input; never overwrite it.

The literal occurrences and page order are specified before running the app.
This does not regenerate any M1 OCR inputs or change their ground truth.
"""

import hashlib
import json
from pathlib import Path

from reportlab.pdfbase import pdfmetrics
from reportlab.pdfbase.ttfonts import TTFont
from reportlab.pdfgen.canvas import Canvas

ROOT = Path(__file__).resolve().parents[1]
target = ROOT / "fixtures/viewer-search.pdf"
manifest = ROOT / "fixtures/viewer-search-manifest.json"
assert not target.exists() and not manifest.exists(), "Preserve frozen input"
pdfmetrics.registerFont(
    TTFont("Fixture", str(ROOT / "fixtures/NotoSansJP-fixture.ttf"))
)
pages = [
    (
        (595, 842),
        [
            (60, 720, "ALPHA opens this paragraph."),
            (60, 450, "Middle context: alpha is the second occurrence."),
            (60, 390, "交通費を申請します。"),
            (60, 260, "交通費の領収書を確認します。"),
        ],
    ),
    (
        (595, 842),
        [
            (60, 740, "Before the destination"),
            (60, 530, "Travel notes: alpha closes this section."),
            (60, 470, "交通費は毎月精算します。"),
            (60, 380, "transport expense and document review."),
        ],
    ),
    (
        (842, 595),
        [
            (60, 470, "A wider page for reading context"),
            (60, 330, "Keep the start of this sentence in view when finding alpha."),
            (60, 190, "交通費の最終確認です。"),
        ],
    ),
]
pdf = Canvas(str(target), invariant=1, pageCompression=1)
for size, lines in pages:
    pdf.setPageSize(size)
    pdf.setFont("Fixture", 16)
    for x, y, text in lines:
        pdf.drawString(x, y, text)
    pdf.showPage()
pdf.save()
manifest.write_text(
    json.dumps(
        {
            "file": target.name,
            "sha256": hashlib.sha256(target.read_bytes()).hexdigest(),
            "origin": "Synthetic text authored for viewer tests; MIT. Font: Noto Sans JP, OFL.",
            "expected": {
                "alpha": {"count": 4, "pages": [1, 1, 2, 3]},
                "交通費": {"count": 4, "pages": [1, 1, 2, 3]},
                "expense": {"count": 1, "pages": [2]},
                "no-such-text-92817": {"count": 0, "pages": []},
            },
        },
        ensure_ascii=False,
        indent=2,
    )
    + "\n",
    encoding="utf-8",
)
print(target.name, "and expected counts frozen before app testing")
