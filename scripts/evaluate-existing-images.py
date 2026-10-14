"""Inspect fixed existing-image edits with PDFium, pypdf and Poppler."""

import argparse
import hashlib
import importlib.util
import json
import math
from pathlib import Path

from PIL import Image, ImageChops, ImageDraw
from pypdf import PdfReader
import pypdfium2 as pdfium


def module(name):
    path = Path(__file__).with_name(name)
    spec = importlib.util.spec_from_file_location("tatsu_" + path.stem, path)
    result = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(result)
    return result


def page_data(path):
    rows = []
    with pdfium.PdfDocument(path) as document:
        for index in range(len(document)):
            page = document[index]
            text = page.get_textpage()
            rows.append(
                dict(
                    text=text.get_text_range(),
                    character_boxes=[
                        list(text.get_charbox(i)) for i in range(text.count_chars())
                    ],
                    images=[
                        dict(
                            bounds=list(obj.get_bounds()),
                            pixel_sha256=hashlib.sha256(
                                obj.get_bitmap().to_pil().convert("RGB").tobytes()
                            ).hexdigest(),
                        )
                        for obj in page.get_objects(
                            filter=[pdfium.raw.FPDF_PAGEOBJ_IMAGE]
                        )
                    ],
                )
            )
            text.close()
            page.close()
    return rows


def display_box(page, rectangle, size):
    x, y, width, height = rectangle
    crop = page.cropbox
    left, bottom, right, top = (
        float(crop.left),
        float(crop.bottom),
        float(crop.right),
        float(crop.top),
    )
    rotation = int(page.get("/Rotate", 0)) % 360

    def point(px, py):
        return {
            0: (px - left, top - py),
            90: (py - bottom, px - left),
            180: (right - px, py - bottom),
            270: (top - py, right - px),
        }[rotation]

    points = [
        point(px, py)
        for px, py in ((x, y), (x + width, y), (x, y + height), (x + width, y + height))
    ]
    w, h = (
        (float(crop.height), float(crop.width))
        if rotation in (90, 270)
        else (float(crop.width), float(crop.height))
    )
    return (
        math.floor(min(p[0] for p in points) * size[0] / w) - 1,
        math.floor(min(p[1] for p in points) * size[1] / h) - 1,
        math.ceil(max(p[0] for p in points) * size[0] / w) + 1,
        math.ceil(max(p[1] for p in points) * size[1] / h) + 1,
    )


def widgets(reader):
    """Compare saved widget geometry and decoded appearances, not object IDs."""
    rows = []
    for page in reader.pages:
        for reference in page.get("/Annots", []):
            annotation = reference.get_object()
            if annotation.get("/Subtype") != "/Widget":
                continue

            def appearance(value):
                value = value.get_object()
                if hasattr(value, "get_data"):
                    return hashlib.sha256(value.get_data()).hexdigest()
                return {str(k): appearance(v) for k, v in value.items()}

            rows.append(
                dict(
                    rectangle=list(annotation.get("/Rect", [])),
                    state=str(annotation.get("/AS", "")),
                    appearance=(
                        appearance(annotation["/AP"]) if "/AP" in annotation else None
                    ),
                )
            )
    return rows


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--fixtures", required=True, type=Path)
    parser.add_argument("--run", required=True, type=Path)
    parser.add_argument("--poppler", required=True, type=Path)
    parser.add_argument("--tag", default="")
    parser.add_argument("--with-ui", action="store_true")
    args = parser.parse_args()
    if len(args.tag) > 20 or any(
        c not in "abcdefghijklmnopqrstuvwxyz0123456789-" for c in args.tag
    ):
        raise ValueError("Use a short inspection tag")
    suffix = "-" + args.tag if args.tag else ""
    output = args.run / ("independent-existing-images" + suffix + ".json")
    if output.exists():
        raise FileExistsError("Preserve prior inspection")
    base = args.fixtures / "existing-image-edit"
    criteria = json.loads((base / "criteria.json").read_text(encoding="utf-8"))
    helper = module("evaluate-redaction-probe.py")
    render = module("evaluate-redaction-copy.py")
    for name, expected in criteria["files"].items():
        assert helper.digest(base / name) == expected, "Frozen image input changed"
    source = base / "shared-images.pdf"
    original = PdfReader(source)
    before = page_data(source)
    for fixed in criteria["source_images"]:
        x, y, width, height = fixed["raw_rectangle"]
        actual = before[fixed["page"]]["images"][fixed["occurrence"]]["bounds"]
        assert (
            max(abs(a - b) for a, b in zip(actual, [x, y, x + width, y + height]))
            < 1e-7
        ), "Source painted image bounds differ from frozen plan"
    directory = args.run / ("independent-image-poppler" + suffix)
    directory.mkdir(exist_ok=False)
    original_renders = [
        render.poppler(
            args.poppler, source, directory / ("source-" + str(i)), page=i + 1
        )[0]
        for i in range(len(original.pages))
    ]
    cases = [
        ("moved", 0, criteria["move_target"], "move"),
        ("restored", 0, criteria["source_images"][0]["raw_rectangle"], "restore"),
        ("replaced-png", 0, criteria["source_images"][0]["raw_rectangle"], "replace"),
        ("replaced-jpg", 0, criteria["source_images"][0]["raw_rectangle"], "replace"),
        ("removed", 0, None, "remove"),
        ("rotated", 1, criteria["rotated_target"], "move"),
    ]
    if args.with_ui:
        ui = json.loads(
            (base / "ui-positive-criteria.json").read_text(encoding="utf-8")
        )
        cases.extend(
            [
                ("ui", 0, ui["raw_pdf_rectangle"], "move"),
                (
                    "ui-replaced-png",
                    0,
                    criteria["source_images"][0]["raw_rectangle"],
                    "replace",
                ),
                ("ui-removed", 0, None, "remove"),
            ]
        )
    rows = []
    for name, page_index, target, action in cases:
        if name == "ui":
            reference = args.run / "existing-image-ui-source.pdf"
            original = PdfReader(reference)
            before = page_data(reference)
            original_renders = [
                render.poppler(
                    args.poppler,
                    reference,
                    directory / ("ui-source-" + str(i)),
                    page=i + 1,
                )[0]
                for i in range(len(original.pages))
            ]
        elif name == "ui-replaced-png":
            original = PdfReader(source)
            before = page_data(source)
            original_renders = [
                Image.open(directory / ("source-" + str(i) + ".png")).convert("RGB")
                for i in range(len(original.pages))
            ]
        path = args.run / ("existing-image-" + name + ".pdf")
        reader = PdfReader(path)
        after = page_data(path)
        checks = dict(
            geometry_preserved=helper.geometry(reader) == helper.geometry(original),
            all_text_and_character_boxes_identical=all(
                a["text"] == b["text"] and a["character_boxes"] == b["character_boxes"]
                for a, b in zip(before, after)
            ),
            form_values_defaults_and_widgets_preserved=all(
                (reader.get_fields() or {}).get(key, {}).get("/V") == value
                and (reader.get_fields() or {}).get(key, {}).get("/DV") == value
                for key, value in criteria["keep_fields"].items()
            )
            and widgets(reader) == widgets(original),
            unselected_page_images_preserved=all(
                before[i]["images"] == after[i]["images"]
                for i in range(len(before))
                if i != page_index
            ),
        )
        expected_count = len(before[page_index]["images"]) - (
            1 if action == "remove" else 0
        )
        checks["only_selected_drawing_changed"] = (
            len(after[page_index]["images"]) == expected_count
        )
        untouched = (
            after[page_index]["images"][0 if action == "remove" else 1 :]
            if page_index == 0
            else []
        )
        if page_index == 0:
            checks["second_shared_image_unchanged"] = (
                untouched == before[0]["images"][1:]
            )
        if target:
            x, y, width, height = target
            actual = after[page_index]["images"][0]["bounds"]
            checks["target_raw_pdf_bounds"] = (
                max(abs(a - b) for a, b in zip(actual, (x, y, x + width, y + height)))
                < 1e-7
            )
        if action == "replace":
            expected = Image.open(
                base / ("replacement." + name.split("-")[-1])
            ).convert("RGBA")
            actual = reader.pages[0].images["/TatsujinImage1"].image.convert("RGBA")
            checks["replacement_pixels_and_alpha_exact"] = (
                actual.size == expected.size and actual.tobytes() == expected.tobytes()
            )
        elif action != "remove":
            checks["original_selected_bitmap_unchanged"] = (
                after[page_index]["images"][0]["pixel_sha256"]
                == before[page_index]["images"][0]["pixel_sha256"]
            )
        outside_ok = True
        for index, image in enumerate(original_renders):
            actual, _ = render.poppler(
                args.poppler,
                path,
                directory / (name + "-" + str(index)),
                page=index + 1,
            )
            assert image.size == actual.size, "Rendered page geometry differs"
            mask = Image.new("L", image.size, 255)
            if index == page_index and action != "restore":
                draw = ImageDraw.Draw(mask)
                old = next(
                    row["raw_rectangle"]
                    for row in criteria["source_images"]
                    if row["page"] == page_index and row["occurrence"] == 0
                )
                draw.rectangle(
                    display_box(original.pages[index], old, image.size), fill=0
                )
                if target:
                    draw.rectangle(
                        display_box(original.pages[index], target, image.size), fill=0
                    )
            outside_ok &= (
                ImageChops.multiply(
                    ImageChops.difference(image, actual), mask.convert("RGB")
                ).getbbox()
                is None
            )
        checks["poppler_pixels_outside_changed_image_bounds_identical"] = outside_ok
        rows.append(
            dict(
                file=path.name,
                status="PASS" if all(checks.values()) else "FAIL",
                checks=checks,
                sha256=helper.digest(path),
            )
        )
    result = dict(
        status="PASS" if all(row["status"] == "PASS" for row in rows) else "FAIL",
        cases=rows,
        scope="Fixed synthetic existing-image edits; independent engines and one antialias pixel at edited boundaries. Not native viewer or general image-edit acceptance.",
    )
    output.write_text(
        json.dumps(result, ensure_ascii=False, indent=2), encoding="utf-8"
    )
    print(json.dumps(result, ensure_ascii=False, indent=2))
    if result["status"] != "PASS":
        raise SystemExit(1)


if __name__ == "__main__":
    main()
