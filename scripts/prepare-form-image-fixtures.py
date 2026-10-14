"""Freeze shared nested Form image inputs before editing them."""

import argparse
import hashlib
import json
from pathlib import Path
from pypdf import PdfReader, PdfWriter
from pypdf.generic import (
    ArrayObject,
    DecodedStreamObject,
    DictionaryObject,
    NameObject,
    NumberObject,
)


def numbers(values):
    return ArrayObject([NumberObject(v) for v in values])


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    base = root / "fixtures/existing-image-edit"
    fixed = json.loads((base / "criteria.json").read_text(encoding="utf-8"))
    source = base / "shared-images.pdf"
    assert (
        hashlib.sha256(source.read_bytes()).hexdigest() == fixed["files"][source.name]
    )
    args.output.mkdir(exist_ok=False, parents=True)
    files = {}
    for name in (
        "shared-forms.pdf",
        "cyclic-form.pdf",
        "group-form.pdf",
        "clipped-form.pdf",
    ):
        writer = PdfWriter()
        writer.clone_document_from_reader(PdfReader(source))
        page = writer.pages[0]
        resources = DictionaryObject(page["/Resources"].get_object())
        xobjects = DictionaryObject(resources["/XObject"].get_object())
        image = next(
            v for v in xobjects.values() if v.get_object().get("/Subtype") == "/Image"
        )
        inner = DecodedStreamObject()
        inner.update(
            {
                NameObject("/Type"): NameObject("/XObject"),
                NameObject("/Subtype"): NameObject("/Form"),
                NameObject("/FormType"): NumberObject(1),
                NameObject("/BBox"): numbers([0, 0, 100, 100]),
                NameObject("/Matrix"): numbers([1, 0, 0, 1, 0, 0]),
            }
        )
        inner_xobjects = DictionaryObject({NameObject("/Shared"): image})
        inner[NameObject("/Resources")] = DictionaryObject(
            {NameObject("/XObject"): inner_xobjects}
        )
        inner.set_data(b"q 60 0 0 40 20 30 cm /Shared Do Q\n")
        inner_ref = writer._add_object(inner)
        outer = DecodedStreamObject()
        outer.update(
            {
                NameObject("/Type"): NameObject("/XObject"),
                NameObject("/Subtype"): NameObject("/Form"),
                NameObject("/FormType"): NumberObject(1),
                NameObject("/BBox"): numbers([0, 0, 200, 120]),
                NameObject("/Resources"): DictionaryObject(
                    {
                        NameObject("/XObject"): DictionaryObject(
                            {NameObject("/Inner"): inner_ref}
                        )
                    }
                ),
            }
        )
        outer.set_data(b"q 1 0 0 1 10 10 cm /Inner Do Q\n")
        outer_ref = writer._add_object(outer)
        if name == "cyclic-form.pdf":
            inner_xobjects[NameObject("/Cycle")] = outer_ref
            inner.set_data(inner.get_data() + b"/Cycle Do\n")
        elif name == "group-form.pdf":
            inner[NameObject("/Group")] = DictionaryObject(
                {NameObject("/S"): NameObject("/Transparency")}
            )
        elif name == "clipped-form.pdf":
            inner.set_data(b"0 0 100 100 re W n\n" + inner.get_data())
        xobjects[NameObject("/TatsujinFixtureOuter")] = outer_ref
        resources[NameObject("/XObject")] = xobjects
        page[NameObject("/Resources")] = resources
        stream = DecodedStreamObject()
        stream.set_data(
            page.get_contents().get_data()
            + b"\nq 1 0 0 1 80 540 cm /TatsujinFixtureOuter Do Q\nq 1 0 0 1 340 540 cm /TatsujinFixtureOuter Do Q\n"
        )
        page[NameObject("/Contents")] = writer._add_object(stream)
        path = args.output / name
        with path.open("xb") as target:
            writer.write(target)
        files[name] = hashlib.sha256(path.read_bytes()).hexdigest()
    plan = dict(
        files=files,
        source_sha256=fixed["files"][source.name],
        first_pdf_bounds=[110, 580, 60, 40],
        second_pdf_bounds=[370, 580, 60, 40],
        first_physical=[110, 172, 60, 40],
        target_physical=[115, 177, 48, 32],
        replacement="../replacement.png",
        replacement_sha256=fixed["files"]["replacement.png"],
        required=[
            "Only first nested image invocation changes",
            "Other shared Forms/images, glyphs, forms and original immutable",
            "Undo/Redo/save/reopen/reedit",
            "BBox overflow, cycle, explicit clipping and transparency group refuse atomically",
        ],
        scope="Synthetic CC0 graphics; shared existing image and Noto fixture resources; fixed before first nested-image edit; original acceptance unchanged",
    )
    (args.output / "criteria.json").write_text(
        json.dumps(plan, ensure_ascii=False, indent=2), encoding="utf-8"
    )
    print(json.dumps(plan, ensure_ascii=False, indent=2))


if __name__ == "__main__":
    main()
