"""Inspect shared nested-image edits with independent PDF engines."""

import argparse
import hashlib
import importlib.util
import json
import math
from pathlib import Path
import pypdfium2 as pdfium
from pypdf import PdfReader
from pypdf.generic import ContentStream
from PIL import Image, ImageChops, ImageDraw


def module(name):
    spec = importlib.util.spec_from_file_location(
        name, Path(__file__).with_name(name + ".py")
    )
    result = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(result)
    return result


def drawn_images(path):
    result = []
    with pdfium.PdfDocument(path) as document:
        for index in range(len(document)):
            page = document[index]
            rows = []

            def walk(form=None, parent=pdfium.PdfMatrix()):
                for obj in page.get_objects(max_depth=1, form=form):
                    matrix = obj.get_matrix().multiply(parent)
                    if obj.type == pdfium.raw.FPDF_PAGEOBJ_FORM:
                        walk(obj, matrix)
                    elif obj.type == pdfium.raw.FPDF_PAGEOBJ_IMAGE:
                        points = [
                            matrix.on_point(x, y)
                            for x, y in ((0, 0), (1, 0), (0, 1), (1, 1))
                        ]
                        box = [
                            min(p[0] for p in points),
                            min(p[1] for p in points),
                            max(p[0] for p in points) - min(p[0] for p in points),
                            max(p[1] for p in points) - min(p[1] for p in points),
                        ]
                        rows.append(
                            dict(
                                bounds=box,
                                pixels=hashlib.sha256(
                                    obj.get_bitmap().to_pil().convert("RGB").tobytes()
                                ).hexdigest(),
                            )
                        )

            walk()
            result.append(rows)
            page.close()
    return result


def actual_paths(page, reader):
    result = []

    def walk(contents, resources, path=()):
        for operands, operator in ContentStream(contents, reader).operations:
            if operator != b"Do":
                continue
            name = operands[0]
            obj = resources["/XObject"][name].get_object()
            if obj.get("/Subtype") == "/Image":
                result.append(path + (str(name),))
            elif obj.get("/Subtype") == "/Form":
                walk(obj, obj.get("/Resources", resources), path + (str(name),))

    walk(page.get_contents(), page["/Resources"].get_object())
    return result


def close(first, last):
    # PDFium exposes pageobject matrices as 32-bit floats, as in direct-image inspection.
    return all(math.isclose(a, b, rel_tol=0, abs_tol=2e-5) for a, b in zip(first, last))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--fixtures", required=True, type=Path)
    parser.add_argument("--run", required=True, type=Path)
    parser.add_argument("--poppler", required=True, type=Path)
    parser.add_argument("--with-ui", action="store_true")
    args = parser.parse_args()
    output = args.run / "independent-form-images.json"
    assert not output.exists()
    base = args.fixtures / "existing-image-edit/forms"
    fixed = json.loads((base / "positive-criteria.json").read_text(encoding="utf-8"))
    source = base / "shared-forms.pdf"
    images = module("evaluate-existing-images")
    helper = module("evaluate-redaction-probe")
    render = module("evaluate-redaction-copy")
    assert helper.digest(source) == fixed["files"][source.name]
    original = PdfReader(source)
    before = images.page_data(source)
    old = drawn_images(source)
    assert len(old[0]) == 4
    assert close(old[0][2]["bounds"], fixed["first_pdf_bounds"]) and close(
        old[0][3]["bounds"], fixed["second_pdf_bounds"]
    )
    directory = args.run / "independent-form-poppler"
    directory.mkdir(exist_ok=False)
    pictures = [
        render.poppler(args.poppler, source, directory / f"source-{i}", page=i + 1)[0]
        for i in range(len(original.pages))
    ]
    replacement = Image.open(base / fixed["replacement"]).convert("RGBA")
    assert helper.digest(base / fixed["replacement"]) == fixed["replacement_sha256"]
    unit = float(original.pages[0].get("/UserUnit", 1))
    x, y, w, h = fixed["target_physical"]
    crop = original.pages[0].cropbox
    target = [
        float(crop.left) + x / unit,
        float(crop.top) - (y + h) / unit,
        w / unit,
        h / unit,
    ]
    rows = []
    cases = [
        "moved",
        "restored",
        "replaced",
        "history",
        "reedited",
        "removed",
        "bbox-edge",
    ]
    if args.with_ui:
        cases.append("ui")
    for name in cases:
        path = args.run / f"existing-form-image-{name}.pdf"
        reader = PdfReader(path)
        after = images.page_data(path)
        actual = drawn_images(path)
        removed = name == "removed"
        changed = name in ("replaced", "history", "reedited")
        expected = (
            fixed["first_pdf_bounds"] if name in ("restored", "reedited") else target
        )
        if name == "ui":
            ui = json.loads((base / "ui-criteria.json").read_text(encoding="utf-8"))
            ux, uy, uw, uh = ui["physical_rectangle"]
            expected = [
                float(crop.left) + ux / unit,
                float(crop.top) - (uy + uh) / unit,
                uw / unit,
                uh / unit,
            ]
        if name == "bbox-edge":
            expected = json.loads(
                (base / "bbox-criteria.json").read_text(encoding="utf-8")
            )["edge_pdf_bounds"]
        checks = dict(
            all_text_and_positions_identical=all(
                a["text"] == b["text"] and a["character_boxes"] == b["character_boxes"]
                for a, b in zip(before, after)
            ),
            form_values_identical={
                k: v.get("/V") for k, v in original.get_fields().items()
            }
            == {k: v.get("/V") for k, v in reader.get_fields().items()},
            widgets_and_appearances_identical=images.widgets(original)
            == images.widgets(reader),
            page_geometry_identical=helper.geometry(original)
            == helper.geometry(reader),
            unselected_direct_and_shared_sibling_images_identical=actual[0][:2]
            == old[0][:2]
            and actual[0][-1] == old[0][3],
            other_pages_images_identical=actual[1:] == old[1:],
            only_one_nested_draw_removed=(
                len(actual[0]) == 3 if removed else len(actual[0]) == 4
            ),
        )
        if not removed:
            checks["selected_raw_bounds"] = close(actual[0][2]["bounds"], expected)
            if changed:
                paths = actual_paths(reader.pages[0], reader)
                selected = reader.pages[0].images[list(paths[2])].image.convert("RGBA")
                checks["replacement_all_RGBA_bytes_exact"] = (
                    selected.size == replacement.size
                    and selected.tobytes() == replacement.tobytes()
                )
            else:
                checks["selected_original_bitmap_retained"] = (
                    actual[0][2]["pixels"] == old[0][2]["pixels"]
                )
        outside = True
        for i, picture in enumerate(pictures):
            actual_picture = render.poppler(
                args.poppler, path, directory / f"{name}-{i}", page=i + 1
            )[0]
            assert actual_picture.size == picture.size
            mask = Image.new("L", picture.size, 255)
            if i == 0 and name != "restored":
                draw = ImageDraw.Draw(mask)
                for box in [fixed["first_pdf_bounds"]] + (
                    [] if removed else [expected]
                ):
                    draw.rectangle(
                        images.display_box(original.pages[0], box, picture.size), fill=0
                    )
            outside &= (
                ImageChops.multiply(
                    ImageChops.difference(picture, actual_picture), mask.convert("RGB")
                ).getbbox()
                is None
            )
        checks["poppler_outside_changed_image_bounds_identical"] = outside
        rows.append(
            dict(
                file=path.name,
                status="PASS" if all(checks.values()) else "FAIL",
                checks=checks,
                sha256=helper.digest(path),
            )
        )
    result = dict(
        status="PASS" if all(r["status"] == "PASS" for r in rows) else "FAIL",
        cases=rows,
        scope="Frozen nested shared Forms and UserUnit=2; PDFium/pypdf/Poppler. One pixel at changed image boundaries, complete page pixel equality when restored. Not arbitrary Form or native acceptance.",
    )
    output.write_text(
        json.dumps(result, ensure_ascii=False, indent=2), encoding="utf-8"
    )
    print(json.dumps(result, ensure_ascii=False, indent=2))
    if result["status"] != "PASS":
        raise SystemExit(1)


if __name__ == "__main__":
    main()
