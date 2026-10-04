"""Repair D07's input appearance without changing its field values or expectations."""

from pathlib import Path
import hashlib, io, json
from reportlab.pdfgen import canvas
from reportlab.pdfbase import pdfmetrics
from reportlab.pdfbase.ttfonts import TTFont
from pypdf import PdfReader, PdfWriter
from pypdf.generic import (
    NameObject,
    DictionaryObject,
    DecodedStreamObject,
    ArrayObject,
    NumberObject,
)

ROOT = Path(__file__).resolve().parents[1]
FX = ROOT / "fixtures"
path = FX / "D07.pdf"
original = path.read_bytes()
before = hashlib.sha256(original).hexdigest()
archive = ROOT / "evidence/D07-original-invalid-appearance.pdf"
if not archive.exists():
    archive.write_bytes(original)
writer = PdfWriter(clone_from=path)
values = {k: str(v.get("/V")) for k, v in writer.get_fields().items()}
pdfmetrics.registerFont(TTFont("FixtureNoto", FX / "NotoSansJP-fixture.ttf"))
buf = io.BytesIO()
c = canvas.Canvas(buf, pagesize=(220, 25))
c.setFillColorRGB(0.8, 0.84, 1)
c.rect(0, 0, 220, 25, fill=1, stroke=1)
c.setFillColorRGB(0, 0, 0)
c.setFont("FixtureNoto", 12)
c.drawString(4, 7, "山田 太郎")
c.showPage()
c.save()
ap = PdfReader(buf).pages[0]
stream = DecodedStreamObject()
stream.set_data(ap.get_contents().get_data())
stream[NameObject("/Type")] = NameObject("/XObject")
stream[NameObject("/Subtype")] = NameObject("/Form")
stream[NameObject("/BBox")] = ArrayObject([NumberObject(x) for x in [0, 0, 220, 25]])
stream[NameObject("/Resources")] = ap["/Resources"].clone(writer)
field = writer._root_object["/AcroForm"]["/Fields"][0].get_object()
field[NameObject("/AP")] = DictionaryObject(
    {NameObject("/N"): writer._add_object(stream)}
)
writer.write(path)
assert {k: str(v.get("/V")) for k, v in PdfReader(path).get_fields().items()} == values
after = hashlib.sha256(path.read_bytes()).hexdigest()
manifest = json.loads((FX / "manifest.json").read_text(encoding="utf-8"))
for item in manifest:
    if item["file"] == "D07.pdf":
        item.update(
            sha256=after,
            appearance_correction="scripts/correct-form-fixture.py; field values unchanged; previous input archived in evidence",
        )
(FX / "manifest.json").write_text(
    json.dumps(manifest, ensure_ascii=False, indent=2), encoding="utf-8"
)
(ROOT / "evidence/fixture-correction.json").write_text(
    json.dumps(
        {
            "file": "D07.pdf",
            "before_sha256": before,
            "after_sha256": after,
            "values_unchanged": values,
            "reason": "The initial 220x100 appearance was scaled into a 220x25 widget; replaced with correctly sized 220x25 appearance. OCR truth/search terms unchanged.",
        },
        ensure_ascii=False,
        indent=2,
    ),
    encoding="utf-8",
)
