"""Freeze synthetic shared-image edit inputs before running product operations."""

import argparse
import hashlib
import json
from pathlib import Path

from PIL import Image, ImageDraw
from pypdf import PdfReader, PdfWriter
from pypdf.generic import DecodedStreamObject, DictionaryObject, NameObject


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    args.output.mkdir(parents=True, exist_ok=False)
    foundation = root / "fixtures/redaction-copy/foundation/unsafe-source.pdf"
    writer = PdfWriter()
    writer.clone_document_from_reader(PdfReader(foundation))
    objects = writer.pages[0]["/Resources"]["/XObject"]
    image_name = next(
        key
        for key, value in objects.items()
        if value.get_object().get("/Subtype") == "/Image"
    )
    placements = {0: (360, 250, 120, 60), 1: (60, 420, 120, 60)}
    for index, (x, y, width, height) in placements.items():
        page = writer.pages[index]
        resources = DictionaryObject(page["/Resources"].get_object())
        resources[NameObject("/XObject")] = objects
        page[NameObject("/Resources")] = resources
        stream = DecodedStreamObject()
        stream.set_data(
            page.get_contents().get_data()
            + f"\nq {width} 0 0 {height} {x} {y} cm {image_name} Do Q\n".encode("ascii")
        )
        page[NameObject("/Contents")] = writer._add_object(stream)
    digital = root / "fixtures/D01.pdf"
    writer.add_page(PdfReader(digital).pages[0])
    with (args.output / "shared-images.pdf").open("xb") as file:
        writer.write(file)
    png = Image.new("RGBA", (160, 80), (30, 100, 210, 255))
    draw = ImageDraw.Draw(png)
    draw.rectangle((15, 15, 60, 65), fill=(220, 130, 40, 128))
    png.save(args.output / "replacement.png")
    jpeg = Image.new("RGB", (120, 60), (180, 45, 90))
    ImageDraw.Draw(jpeg).rectangle((25, 10, 95, 50), fill=(20, 130, 90))
    jpeg.save(args.output / "replacement.jpg", quality=95)
    files = {
        name: hashlib.sha256((args.output / name).read_bytes()).hexdigest()
        for name in ("shared-images.pdf", "replacement.png", "replacement.jpg")
    }
    result = dict(
        files=files,
        source_inputs={
            foundation.relative_to(root)
            .as_posix(): hashlib.sha256(foundation.read_bytes())
            .hexdigest(),
            digital.relative_to(root)
            .as_posix(): hashlib.sha256(digital.read_bytes())
            .hexdigest(),
        },
        source_images=[
            dict(page=0, occurrence=0, raw_rectangle=[60, 250, 240, 120]),
            dict(page=0, occurrence=1, raw_rectangle=list(placements[0])),
            dict(page=1, occurrence=0, raw_rectangle=list(placements[1])),
        ],
        move_target=[90, 190, 180, 90],
        rotated_target=[200, 400, 150, 75],
        keep_text=["KEEP_VISIBLE_9df67", "KEEP_SECOND_PAGE_9df67"],
        keep_fields={
            "keep-field": "KEEP_FORM_9df67",
            "secret-field": "SECRET_FORM_9df67",
        },
        required=[
            "Only selected drawing occurrence changes",
            "Unselected image occurrences preserve transforms and pixels",
            "Original text, forms and page geometry preserved",
            "Original snapshot and bytes unchanged",
            "Undo/Redo and PDF reopen/reedit",
            "Unsupported OCR, forms, inline images, clipping and protected cases refuse without mutation",
        ],
        scope="Synthetic CC0 graphics and text; appended frozen D01 text retains embedded font notices. Criteria frozen before execution, not general image-edit acceptance.",
    )
    (args.output / "criteria.json").write_text(
        json.dumps(result, ensure_ascii=False, indent=2), encoding="utf-8"
    )
    print(json.dumps(result, ensure_ascii=False, indent=2))


if __name__ == "__main__":
    main()
