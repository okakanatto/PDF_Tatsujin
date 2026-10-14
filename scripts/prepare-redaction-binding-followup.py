import argparse
import copy
import hashlib
import json
from pathlib import Path
import sys

import pypdfium2 as pdfium
from pypdf import PdfReader, PdfWriter
from pypdf.generic import NameObject, NumberObject

root = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(root / "scripts"))
base = root / "fixtures/redaction-copy/font-binding-v1"
fixed = json.loads((base / "criteria.json").read_text(encoding="utf-8"))
source = next(c for c in fixed["cases"] if c["file"] == "same-direct.pdf")
assert (
    hashlib.sha256((base / source["file"]).read_bytes()).hexdigest() == source["sha256"]
)
parser = argparse.ArgumentParser(
    description="Freeze font binding conditions without overwriting prior inputs"
)
parser.add_argument("--output", type=Path, required=True)
out = parser.parse_args().output
out.mkdir(exist_ok=False)
rows = []
for name in ("middle-span", "following-Tj", "rotation-90", "fractional-origin-size"):
    writer = PdfWriter(clone_from=PdfReader(base / source["file"]))
    page = writer.pages[0]
    form = page["/Resources"]["/XObject"]["/Target"]
    data = form.get_data()
    keep = list(source["keep"])
    if name == "middle-span":
        data = data.replace(
            b"(SECRET_ALIAS PUBLIC_FORM)", b"(BEFORE_WORD SECRET_ALIAS PUBLIC_FORM)"
        )
        keep.append("BEFORE_WORD")
    elif name == "following-Tj":
        data = data.replace(b"Tj ET", b"Tj ( AFTER_TEXT) Tj ET")
        keep.append("AFTER_TEXT")
    elif name == "rotation-90":
        page[NameObject("/Rotate")] = NumberObject(90)
    else:
        data = data.replace(b"60 430 Tm", b"61.1234 432.875 Tm")
        page["/Contents"].set_data(
            page.get_contents().get_data().replace(b"/F1 18 Tf", b"/F1 17.25 Tf")
        )
    form.set_data(data)
    path = out / (name + ".pdf")
    writer.write(path)
    boxes = {}
    with pdfium.PdfDocument(path) as pdf:
        text = pdf[0].get_textpage()
        content = text.get_text_range()
        for term in keep + ["SECRET_ALIAS"]:
            assert content.count(term) == 1, (name, term, content)
            start = content.index(term)
            boxes[term] = [
                list(text.get_charbox(i)) for i in range(start, start + len(term))
            ]
    target = boxes.pop("SECRET_ALIAS")
    x0, y0 = min(b[0] for b in target) - 1, min(b[1] for b in target) - 1
    x1, y1 = max(b[2] for b in target) + 1, max(b[3] for b in target) + 1
    rows.append(
        dict(
            file=path.name,
            sha256=hashlib.sha256(path.read_bytes()).hexdigest(),
            remove="SECRET_ALIAS",
            keep=keep,
            refused=False,
            rect=[x0, y0, x1 - x0, y1 - y0],
            boxes=boxes,
            max_position_error_pt=0.5,
        )
    )
path = out / "criteria.json"
path.write_text(
    json.dumps(
        dict(cases=rows, raster_dpi=300, outside_changed_pixels=0, boundary_px=1),
        ensure_ascii=False,
        indent=2,
    )
    + "\n",
    encoding="utf-8",
)
print(
    json.dumps(
        dict(
            cases=len(rows),
            criteria_sha256=hashlib.sha256(path.read_bytes()).hexdigest(),
        )
    )
)
