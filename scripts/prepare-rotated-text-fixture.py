import hashlib, json
from pathlib import Path
from pypdf import PdfReader, PdfWriter
from pypdf.generic import NameObject, NumberObject, RectangleObject

source = Path("fixtures/existing-text-edit/visible/body-text.pdf")
out = Path("fixtures/existing-text-edit/rotated")
out.mkdir(exist_ok=False)
writer = PdfWriter()
writer.clone_document_from_reader(PdfReader(source))
writer.pages[0][NameObject("/Rotate")] = NumberObject(90)
writer.pages[0][NameObject("/CropBox")] = RectangleObject([20, 40, 590, 760])
path = out / "body-text.pdf"
with path.open("xb") as f:
    writer.write(f)
result = dict(
    file=path.name,
    sha256=hashlib.sha256(path.read_bytes()).hexdigest(),
    source_sha256=hashlib.sha256(source.read_bytes()).hexdigest(),
    text="Original body line",
    move_physical_rectangle=[100, 200, 250, 40],
    rotation=90,
    crop_box=[20, 40, 590, 760],
    user_unit=2,
    required="Selected body glyphs map to the exact physical target; other text, images, forms and page geometry retained",
    scope="Additional rotated positive fixed before any product operation; original cases unchanged",
)
(out / "criteria.json").write_text(
    json.dumps(result, ensure_ascii=False, indent=2), encoding="utf-8"
)
print(json.dumps(result))
