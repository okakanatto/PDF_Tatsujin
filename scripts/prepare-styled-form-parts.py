"""Freeze styled shared Form invocations before product editing."""

import argparse
import hashlib
import json
from pathlib import Path

from pypdf import PdfReader, PdfWriter
from pypdf.generic import (
    DictionaryObject,
    FloatObject,
    NameObject,
    NumberObject,
    RectangleObject,
)


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=False)
    root = Path(__file__).resolve().parents[1]
    base = root / "fixtures/existing-image-edit/forms"
    styled = root / "fixtures/existing-text-edit/styled-parts-v1"
    original = json.loads((base / "criteria.json").read_text(encoding="utf-8"))
    previous = json.loads((styled / "criteria.json").read_text(encoding="utf-8"))
    jp_path = styled / "absolute.pdf"
    assert sha(jp_path) == previous["files"][jp_path.name]
    jp_source = PdfReader(jp_path)
    jp_font = jp_source.pages[0]["/Resources"]["/Font"].raw_get("/StyledJP")
    # Preserve the frozen Japanese subset and its exact character codes.
    data = jp_source.pages[0].get_contents().get_data()
    begin = data.index(b"1 0 0 1 60.25 610.125 Tm")
    jp_part = data[begin : data.index(b"ET", begin)]
    jp_part = jp_part.replace(b"60.25 610.125", b"5 73").replace(
        b"/StyledJP 16 Tf", b"/FormJP 12 Tf"
    )
    files = {}
    variants = {
        "shared": "shared-forms.pdf",
        "rotated": "shared-forms.pdf",
        "inherited": "shared-forms.pdf",
        "bbox-refused": "shared-forms.pdf",
        "cycle-refused": "cyclic-form.pdf",
        "group-refused": "group-form.pdf",
        "clip-refused": "clipped-form.pdf",
    }
    for name, source_name in variants.items():
        source = base / source_name
        assert sha(source) == original["files"][source_name]
        writer = PdfWriter()
        writer.clone_document_from_reader(PdfReader(source))
        page = writer.pages[0]
        outer_ref = page["/Resources"]["/XObject"].raw_get("/TatsujinFixtureOuter")
        outer = outer_ref.get_object()
        inner = outer["/Resources"]["/XObject"]["/Inner"].get_object()
        fonts = DictionaryObject(
            {NameObject("/FormPlain"): page["/Resources"]["/Font"].raw_get("/F1")}
        )
        fonts[NameObject("/FormBold")] = writer._add_object(
            DictionaryObject(
                {
                    NameObject("/Type"): NameObject("/Font"),
                    NameObject("/Subtype"): NameObject("/Type1"),
                    NameObject("/BaseFont"): NameObject("/Helvetica-Bold"),
                    NameObject("/Encoding"): NameObject("/WinAnsiEncoding"),
                }
            )
        )
        fonts[NameObject("/FormJP")] = jp_font.clone(writer)
        inner[NameObject("/Resources")][NameObject("/Font")] = fonts
        inner[NameObject("/BBox")] = RectangleObject([0, 0, 215, 125])
        outer[NameObject("/BBox")] = RectangleObject([0, 0, 240, 150])
        inner.set_data(
            inner.get_data() + b"\nBT 0 Tr 0 Tc 0 Tw 100 Tz 0 Ts\n"
            b"/FormPlain 10 Tf 1 0 0 1 5 95 Tm (Intro ) Tj\n"
            b"/FormBold 12 Tf 1 0 0 1 60 95 Tm [(Styled ) 25 (phrase)] TJ\n"
            b"/FormPlain 10 Tf 1 0 0 1 150 95 Tm (Suffix intact) Tj\n"
            + jp_part
            + b"\nET\nBT 1 0 0 1 5 8 Tm (FOLLOWING STYLE STATE) Tj ET\n"
        )
        if name == "inherited":
            parent_resources = outer["/Resources"].get_object()
            parent_resources[NameObject("/Font")] = fonts
            parent_resources["/XObject"][NameObject("/Shared")] = inner["/Resources"][
                "/XObject"
            ].raw_get("/Shared")
            del inner["/Resources"]
        if name == "bbox-refused":
            inner[NameObject("/BBox")] = RectangleObject([0, 0, 215, 96])
        # The same original Form resource is also used on the second page.
        second = writer.pages[1]
        resources = DictionaryObject(second["/Resources"].get_object())
        xobjects = DictionaryObject(resources.get("/XObject", DictionaryObject()))
        xobjects[NameObject("/StyledOuter")] = outer_ref
        resources[NameObject("/XObject")] = xobjects
        second[NameObject("/Resources")] = resources
        from pypdf.generic import DecodedStreamObject

        stream = DecodedStreamObject()
        stream.set_data(
            second.get_contents().get_data()
            + b"\nq 1 0 0 1 80 540 cm /StyledOuter Do Q\n"
        )
        second[NameObject("/Contents")] = writer._add_object(stream)
        if name == "rotated":
            page[NameObject("/Rotate")] = NumberObject(90)
            page[NameObject("/CropBox")] = RectangleObject([20, 25, 590, 770])
            page[NameObject("/UserUnit")] = FloatObject(1.25)
        path = args.output / (name + ".pdf")
        with path.open("xb") as handle:
            writer.write(handle)
        files[path.name] = sha(path)
    fixed = dict(
        files=files,
        source_inputs={
            str(path.relative_to(root)).replace("\\", "/"): sha(path)
            for path in (base / "criteria.json", styled / "criteria.json", jp_path)
        },
        positives=["shared", "rotated", "inherited"],
        selected_invocation=0,
        selected_depth=2,
        replacements=previous["replacements"],
        remove=previous["remove"],
        reedited=previous["reedited"],
        refused=["cycle-refused.pdf", "group-refused.pdf", "clip-refused.pdf"],
        bbox_refused="bbox-refused.pdf",
        overflow="W" * 24,
        outside_pixels=0,
        raster_dpi=300,
        boundary_pixels=1,
        position="Every unselected real character and explicit space exactly unchanged",
        resources="Original and all unselected Form bytes, fonts, images and attributes unchanged; only selected call path cloned",
        scope="Synthetic CC0 shared nested Forms and OFL Japanese subset; fixed before product operations; prior inputs and thresholds unchanged",
    )
    criterion = args.output / "criteria.json"
    criterion.write_text(
        json.dumps(fixed, ensure_ascii=False, indent=2), encoding="utf-8"
    )
    print(json.dumps(dict(criterion_sha256=sha(criterion), files=files)))


if __name__ == "__main__":
    main()
