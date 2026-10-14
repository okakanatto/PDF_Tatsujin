import argparse
import copy
import hashlib
import json
from pathlib import Path

import pypdfium2 as pdfium
from pypdf import PdfReader, PdfWriter
from pypdf.generic import ArrayObject, DictionaryObject, NameObject, NumberObject
import sys

root = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(root / "scripts"))
from prepare_redaction_helpers import stream

parser = argparse.ArgumentParser(
    description="Freeze font binding conditions without overwriting prior inputs"
)
parser.add_argument("--output", type=Path, required=True)
out = parser.parse_args().output
out.mkdir(exist_ok=False)
base = root / "fixtures/redaction-copy/partial-text/visible.pdf"
assert (
    hashlib.sha256(base.read_bytes()).hexdigest()
    == "f9bacb0ca4b4b19f5bc100e68bce571dbb474b55665ad6dbfb8097e7c9f2fd39"
)
rows = []
for name in (
    "same-direct",
    "shadow-direct-refused",
    "shadow-reference-refused",
    "nested-shadow-refused",
):
    writer = PdfWriter(clone_from=PdfReader(base))
    page = writer.pages[0]
    resources = page["/Resources"]
    font = copy.copy(resources["/Font"]["/F1"])
    resources["/Font"][NameObject("/F1")] = font
    local_font = copy.copy(font)
    if name != "same-direct":
        local_font[NameObject("/BaseFont")] = NameObject("/Courier")
    if name == "shadow-reference-refused":
        local_font = writer._add_object(local_font)
    bounds = ArrayObject([NumberObject(v) for v in (0, 0, 612, 792)])
    form = writer._add_object(
        stream(
            b"BT 1 0 0 1 60 430 Tm (SECRET_ALIAS PUBLIC_FORM) Tj ET\n",
            Type=NameObject("/XObject"),
            Subtype=NameObject("/Form"),
            BBox=bounds,
            Resources=DictionaryObject(
                {NameObject("/Font"): DictionaryObject({NameObject("/F1"): local_font})}
            ),
        )
    )
    if name == "nested-shadow-refused":
        form = writer._add_object(
            stream(
                b"/Inner Do\n",
                Type=NameObject("/XObject"),
                Subtype=NameObject("/Form"),
                BBox=bounds,
                Resources=DictionaryObject(
                    {
                        NameObject("/Font"): DictionaryObject(
                            {NameObject("/F1"): copy.copy(font)}
                        ),
                        NameObject("/XObject"): DictionaryObject(
                            {NameObject("/Inner"): form}
                        ),
                    }
                ),
            )
        )
    resources[NameObject("/XObject")] = DictionaryObject({NameObject("/Target"): form})
    page[NameObject("/Contents")] = writer._add_object(
        stream(page.get_contents().get_data() + b"BT /F1 18 Tf ET\n/Target Do\n")
    )
    path = out / (name + ".pdf")
    writer.write(path)
    terms = ["公開", "秘匿機密亀鶴", "文字保持", "KEEP_FOLLOWING_LINE", "PUBLIC_FORM"]
    with pdfium.PdfDocument(path) as pdf:
        text = pdf[0].get_textpage()
        content = text.get_text_range()
        boxes = {}
        for term in terms + ["SECRET_ALIAS"]:
            assert content.count(term) == 1, (name, term, content)
            at = content.index(term)
            boxes[term] = [list(text.get_charbox(i)) for i in range(at, at + len(term))]
    selected = boxes.pop("SECRET_ALIAS")
    x0, y0 = min(b[0] for b in selected) - 1, min(b[1] for b in selected) - 1
    x1, y1 = max(b[2] for b in selected) + 1, max(b[3] for b in selected) + 1
    rows.append(
        dict(
            file=path.name,
            sha256=hashlib.sha256(path.read_bytes()).hexdigest(),
            refused=name != "same-direct",
            remove="SECRET_ALIAS",
            keep=terms,
            rect=[x0, y0, x1 - x0, y1 - y0],
            boxes=boxes,
            max_position_error_pt=0.5,
        )
    )
path = out / "criteria.json"
path.write_text(
    json.dumps(
        dict(
            cases=rows,
            raster_dpi=300,
            outside_changed_pixels=0,
            boundary_px=1,
            scope="Track actual inherited font declaration; refuse resource shadow mismatch",
        ),
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
