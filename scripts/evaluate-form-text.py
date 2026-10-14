"""Inspect one shared Form text invocation using independent PDF engines."""

import argparse
import io
import json
from collections import Counter
from pathlib import Path
from fontTools.ttLib import TTFont
from PIL import Image, ImageChops, ImageDraw
from pypdf import PdfReader
from pypdf.generic import ContentStream
from importlib.util import module_from_spec, spec_from_file_location


def module(name):
    spec = spec_from_file_location(name, Path(__file__).with_name(name + ".py"))
    result = module_from_spec(spec)
    spec.loader.exec_module(result)
    return result


def first_spans(row, selected):
    """Exclude only the first call; preserve all repeated sibling characters."""
    text, boxes = row["text"], row["character_boxes"]
    assert len(text) == len(boxes)
    exclusions, bounds = set(), []
    cursor = 0
    for value in selected:
        start = text.index(value, cursor)
        cursor = start + len(value)
        positions = range(start, cursor)
        exclusions.update(positions)
        painted = [boxes[i] for i in positions if not text[i].isspace()]
        bounds.append(
            [
                min(b[0] for b in painted),
                min(b[1] for b in painted),
                max(b[2] for b in painted) - min(b[0] for b in painted),
                max(b[3] for b in painted) - min(b[1] for b in painted),
            ]
        )
    retained = [
        (c, boxes[i])
        for i, c in enumerate(text)
        if i not in exclusions and c not in "\r\n"
    ]
    return retained, bounds


def active_fonts(page, reader):
    """Visit fonts actually used by content in each active Form invocation."""
    result = []

    def walk(contents, resources, depth=0):
        assert depth <= 8
        for operands, operator in ContentStream(contents, reader).operations:
            if operator == b"Tf":
                result.append(resources["/Font"][operands[0]].get_object())
            elif operator == b"Do":
                obj = resources["/XObject"][operands[0]].get_object()
                if obj.get("/Subtype") == "/Form":
                    walk(obj, obj.get("/Resources", resources), depth + 1)

    walk(page.get_contents(), page["/Resources"].get_object())
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--fixtures", required=True, type=Path)
    parser.add_argument("--run", required=True, type=Path)
    parser.add_argument("--poppler", required=True, type=Path)
    parser.add_argument("--with-ui", action="store_true")
    args = parser.parse_args()
    output = args.run / "independent-form-text.json"
    assert not output.exists(), "Preserve previous inspection"
    base = args.fixtures / "existing-text-edit/forms"
    fixed = json.loads((base / "criteria.json").read_text(encoding="utf-8"))
    geometry = json.loads((base / "geometry-criteria.json").read_text(encoding="utf-8"))
    helper = module("evaluate-redaction-probe")
    images = module("evaluate-existing-images")
    forms = module("evaluate-form-images")
    render = module("evaluate-redaction-copy")
    source = base / "shared-forms.pdf"
    assert helper.digest(source) == fixed["files"][source.name]
    original = PdfReader(source)
    before = images.page_data(source)
    old_images = forms.drawn_images(source)
    old_lines = fixed["selected"].split("\n")
    assert all(before[0]["text"].count(line) == 2 for line in old_lines)
    unchanged, old_bounds = first_spans(before[0], old_lines)
    directory = args.run / "independent-form-text-poppler"
    directory.mkdir(exist_ok=False)
    pictures = [
        render.poppler(args.poppler, source, directory / f"source-{i}", page=i + 1)[0]
        for i in range(len(original.pages))
    ]
    cases = [
        ("english", fixed["english"], False),
        ("japanese", fixed["japanese"], True),
        ("reedited", fixed["reedited"], True),
        ("lines", fixed["three_lines"], True),
        ("moved", fixed["selected"], False),
        ("removed", "", False),
    ]
    if args.with_ui:
        cases.append(("ui", fixed["japanese"], True))
    rows = []
    for name, value, explicit in cases:
        path = args.run / f"existing-form-text-{name}.pdf"
        reader = PdfReader(path)
        after = images.page_data(path)
        new_lines = value.split("\n") if value else []
        retained, new_bounds = first_spans(after[0], new_lines)
        expected = before[0]["text"]
        for line in old_lines:
            expected = expected.replace(line, "", 1)
        expected += value
        checks = dict(
            exact_character_counts=Counter(expected.replace("\r", "").replace("\n", ""))
            == Counter(after[0]["text"].replace("\r", "").replace("\n", "")),
            replacement_lines_searchable=all(
                line in after[0]["text"] for line in new_lines
            ),
            replacement_lines_extractable_in_pypdf=all(
                line in reader.pages[0].extract_text() for line in new_lines
            ),
            unselected_call_characters_and_positions_identical=sorted(unchanged)
            == sorted(retained),
            other_pages_identical=before[1:] == after[1:],
            all_nested_image_pixels_and_positions_identical=old_images
            == forms.drawn_images(path),
            form_values_identical={
                k: v.get("/V") for k, v in original.get_fields().items()
            }
            == {k: v.get("/V") for k, v in reader.get_fields().items()},
            widgets_and_appearances_identical=images.widgets(original)
            == images.widgets(reader),
            page_geometry_identical=helper.geometry(original)
            == helper.geometry(reader),
            replacement_line_order=[after[0]["text"].index(line) for line in new_lines]
            == sorted(after[0]["text"].index(line) for line in new_lines),
        )
        metrics = {}
        if name == "moved":
            page = original.pages[0]
            unit = float(page.get("/UserUnit", 1))
            x, y = min(b[0] for b in new_bounds), min(b[1] for b in new_bounds)
            right = max(b[0] + b[2] for b in new_bounds)
            top = max(b[1] + b[3] for b in new_bounds)
            actual = [
                (x - float(page.cropbox.left)) * unit,
                (float(page.cropbox.top) - top) * unit,
                (right - x) * unit,
                (top - y) * unit,
            ]
            checks["physical_geometry_within_half_point"] = all(
                abs(a - b) <= 0.5 for a, b in zip(actual, geometry["geometry"])
            )
            metrics["physical_geometry"] = dict(
                actual=actual, expected=geometry["geometry"], tolerance_pt=0.5
            )
        if explicit:
            embedded = []
            for font in active_fonts(reader.pages[0], reader):
                if font.get("/BaseFont") != "/MeiryoUI":
                    continue
                program = (
                    font["/DescendantFonts"][0]
                    .get_object()["/FontDescriptor"]["/FontFile2"]
                    .get_object()
                    .get_data()
                )
                with TTFont(io.BytesIO(program)) as face:
                    families = {
                        n.toUnicode() for n in face["name"].names if n.nameID in (1, 16)
                    }
                    glyphs = face["maxp"].numGlyphs
                embedded.append(
                    "Meiryo UI" in families
                    and 0 < glyphs <= 64
                    and len(program) < 100000
                    and bool(font["/ToUnicode"].get_object().get_data())
                )
            checks["explicit_family_embedded_small_Unicode_subset"] = bool(
                embedded
            ) and all(embedded)
        outside = True
        for i, picture in enumerate(pictures):
            actual = render.poppler(
                args.poppler, path, directory / f"{name}-{i}", page=i + 1
            )[0]
            assert picture.size == actual.size
            mask = Image.new("L", picture.size, 255)
            if i == 0:
                draw = ImageDraw.Draw(mask)
                for bounds in old_bounds + new_bounds:
                    draw.rectangle(
                        images.display_box(original.pages[0], bounds, picture.size),
                        fill=0,
                    )
            outside &= (
                ImageChops.multiply(
                    ImageChops.difference(picture, actual), mask.convert("RGB")
                ).getbbox()
                is None
            )
        checks["poppler_outside_selected_glyphs_identical"] = outside
        rows.append(
            dict(
                file=path.name,
                status="PASS" if all(checks.values()) else "FAIL",
                checks=checks,
                metrics=metrics,
                sha256=helper.digest(path),
            )
        )
    result = dict(
        status="PASS" if all(r["status"] == "PASS" for r in rows) else "FAIL",
        cases=rows,
        scope="Frozen shared nested Form text calls; PDFium, pypdf, fontTools and Poppler. Native viewers and IME not evaluated.",
    )
    output.write_text(
        json.dumps(result, ensure_ascii=False, indent=2), encoding="utf-8"
    )
    print(json.dumps(result, ensure_ascii=False, indent=2))
    if result["status"] != "PASS":
        raise SystemExit(1)


if __name__ == "__main__":
    main()
