"""Freeze one-line flowing mixed-font inputs before product editing."""

import argparse
import hashlib
import json
import re
from pathlib import Path

from pypdf import PdfReader, PdfWriter
from pypdf.generic import DecodedStreamObject, NameObject, RectangleObject


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def flow(data, form=False):
    begin = data.index(b"BT 0 Tr 0 Tc 0 Tw 100 Tz 0 Ts")
    end = data.index(b"ET", begin)
    block = data[begin:end]
    matrices = list(re.finditer(rb"1 0 0 1 [0-9.]+ [0-9.]+ Tm", block))
    assert len(matrices) == 4
    for match in reversed(matrices[1:]):
        block = block[: match.start()] + block[match.end() :]
    if not form:
        block = block.replace(b"60.25 660.125", b"60.25 610.125")
    result = data[:begin] + block + data[end:]
    if not form:
        result = result.replace(b"60.25 570.125", b"60.25 430.125")
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=False)
    root = Path(__file__).resolve().parents[1]
    page_base = root / "fixtures/existing-text-edit/styled-parts-v1"
    form_base = root / "fixtures/existing-text-edit/styled-form-parts-v1"
    previous = json.loads((page_base / "criteria.json").read_text(encoding="utf-8"))
    forms = json.loads((form_base / "criteria.json").read_text(encoding="utf-8"))
    variants = {
        "page": (False, "absolute"),
        "rotated": (False, "rotated"),
        "shared": (True, "shared"),
        "inherited": (True, "inherited"),
        "position-refused": (False, "absolute"),
        "spacing-refused": (False, "absolute"),
        "relative-refused": (False, "absolute"),
        "bbox-refused": (True, "bbox-refused"),
        "cycle-refused": (True, "cycle-refused"),
        "group-refused": (True, "group-refused"),
        "clip-refused": (True, "clip-refused"),
    }
    files = {}
    inputs = {}
    for name, (is_form, source_name) in variants.items():
        base, conditions = (form_base, forms) if is_form else (page_base, previous)
        source = base / (source_name + ".pdf")
        assert sha(source) == conditions["files"][source.name]
        inputs[str(source.relative_to(root)).replace("\\", "/")] = sha(source)
        writer = PdfWriter()
        writer.clone_document_from_reader(PdfReader(source))
        page = writer.pages[0]
        if is_form:
            outer = page["/Resources"]["/XObject"]["/TatsujinFixtureOuter"]
            inner = outer["/Resources"]["/XObject"]["/Inner"]
            inner.set_data(flow(inner.get_data(), True))
            inner[NameObject("/BBox")] = RectangleObject(
                [0, 0, 310, 96 if name == "bbox-refused" else 125]
            )
            outer[NameObject("/BBox")] = RectangleObject([0, 0, 330, 150])
            for index, item in enumerate(writer.pages[:2]):
                data = item.get_contents().get_data()
                if index == 0:
                    data = data.replace(b"80 540 cm", b"80 480 cm").replace(
                        b"340 540 cm", b"80 300 cm"
                    )
                else:
                    data = data.replace(b"80 540 cm", b"80 480 cm")
                stream = DecodedStreamObject()
                stream.set_data(data)
                item[NameObject("/Contents")] = writer._add_object(stream)
        else:
            data = page.get_contents().get_data()
            if name != "position-refused":
                data = flow(data)
            begin = data.index(b"BT 0 Tr 0 Tc 0 Tw 100 Tz 0 Ts")
            if name == "spacing-refused":
                data = data[:begin] + data[begin:].replace(b"0 Tc", b"1 Tc", 1)
            if name == "relative-refused":
                data = data[:begin] + data[begin:].replace(
                    b"(Intro ) Tj", b"(Intro ) Tj 0 -20 Td", 1
                )
            stream = DecodedStreamObject()
            stream.set_data(data)
            page[NameObject("/Contents")] = writer._add_object(stream)
        path = args.output / (name + ".pdf")
        with path.open("xb") as handle:
            writer.write(handle)
        files[path.name] = sha(path)
    fixed = dict(
        files=files,
        source_inputs=inputs,
        positives={"page": 0, "rotated": 0, "shared": 2, "inherited": 2},
        replacements=previous["replacements"],
        remove=previous["remove"],
        reedited=previous["reedited"],
        following=previous["following"],
        refused=["cycle-refused", "group-refused", "clip-refused"],
        restricted=[
            "position-refused",
            "relative-refused",
            "spacing-refused",
            "bbox-refused",
        ],
        page_overflow="W" * 200,
        form_overflow="W" * 24,
        output_cases=[
            "intro",
            "bold",
            "suffix",
            "japanese",
            "removed",
            "saved",
            "reedited",
        ],
        second_call_cases=["shared", "inherited"],
        ui_cases=["replaced", "removed"],
        arithmetic_tolerance_pt=1e-8,
        outside_pixels=0,
        raster_dpi=300,
        boundary_pixels=1,
        coordinates="Every real character box exactly equals independent source-and-criteria reference; only the selected BT/ET suffix moves",
        scope="Synthetic CC0 single-line flowing parts; OFL Japanese subset; fixed before product operations. Prior contracts and inputs unchanged.",
    )
    criterion = args.output / "criteria.json"
    criterion.write_text(
        json.dumps(fixed, ensure_ascii=False, indent=2), encoding="utf-8"
    )
    print(json.dumps(dict(criterion_sha256=sha(criterion), files=files)))


if __name__ == "__main__":
    main()
