"""Freeze crop/rotation/UserUnit derivatives without changing existing inputs."""

import hashlib
import json
from pathlib import Path

from pypdf import PdfReader, PdfWriter
from pypdf.generic import FloatObject, NameObject

ROOT = Path(__file__).resolve().parents[1]


def main():
    criterion = ROOT / "fixtures/vertical-ocr-criteria.json"
    assert (
        hashlib.sha256(criterion.read_bytes()).hexdigest()
        == "516522131307b548f3b46e8d8a274b398d732857455806c8df2484ee40f3e604"
    )
    fixed = json.loads(criterion.read_text("utf-8"))
    source = ROOT / "fixtures" / fixed["source"]
    assert hashlib.sha256(source.read_bytes()).hexdigest() == fixed["source_sha256"]
    output = ROOT / "fixtures/vertical-ocr-geometry"
    output.mkdir(exist_ok=False)
    writer = PdfWriter(clone_from=source)
    page = writer.pages[fixed["page"] - 1]
    upper = float(page.mediabox.top)
    page.cropbox.lower_left = (30, 20)
    page.cropbox.upper_right = (580, 820)
    page.rotate(90)
    page[NameObject("/UserUnit")] = FloatObject(1.5)
    target = output / "crop-rotate-unit.pdf"
    writer.write(target)
    searches = []
    for item in fixed["search_terms"]:
        x, y, width, height = item["qt_bounds_pt"]
        searches.append(
            dict(
                term=item["term"],
                qt_bounds_pt=[
                    (x - 30) * 1.5,
                    (y - (upper - 820)) * 1.5,
                    width * 1.5,
                    height * 1.5,
                ],
            )
        )
    result = dict(
        source=target.name,
        source_sha256=hashlib.sha256(target.read_bytes()).hexdigest(),
        original_source=fixed["source"],
        original_source_sha256=fixed["source_sha256"],
        page=fixed["page"],
        cropbox=[30, 20, 580, 820],
        rotation=90,
        UserUnit=1.5,
        expected_lines_right_to_left=fixed["expected_lines_right_to_left"],
        search_terms=searches,
        maximum_bounds_error_mm=2,
        comparison="Whitespace only; physical upright CropBox points; original Rotate/UserUnit remain",
        provenance="CC0 derivative of existing frozen synthetic D11; original input/truth unchanged",
    )
    with (output / "criteria.json").open("x", encoding="utf-8") as out:
        json.dump(result, out, ensure_ascii=False, indent=2)
        out.write("\n")
    print(hashlib.sha256((output / "criteria.json").read_bytes()).hexdigest())


if __name__ == "__main__":
    main()
