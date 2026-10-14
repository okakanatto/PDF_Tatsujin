"""Inspect pre-fixed wrapped body text without trusting the editor's layout."""

import argparse
import json
import re
from collections import Counter
from pathlib import Path
from PIL import Image, ImageChops, ImageDraw
from pypdf import PdfReader
from importlib.util import module_from_spec, spec_from_file_location
import io
from fontTools.ttLib import TTFont


def module(name):
    spec = spec_from_file_location(name, Path(__file__).with_name(name + ".py"))
    result = module_from_spec(spec)
    spec.loader.exec_module(result)
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--fixtures", type=Path, required=True)
    parser.add_argument("--run", type=Path, required=True)
    parser.add_argument("--poppler", type=Path, required=True)
    parser.add_argument("--with-ui", action="store_true")
    parser.add_argument("--attempt", default="")
    args = parser.parse_args()
    assert not args.attempt or re.fullmatch(r"[A-Za-z0-9_-]{1,64}", args.attempt)
    suffix = "-" + args.attempt if args.attempt else ""
    output = args.run / ("independent-existing-text-wrap" + suffix + ".json")
    assert not output.exists()
    base = args.fixtures / "existing-text-edit/wrapped"
    fixed = json.loads((base / "criteria.json").read_text(encoding="utf-8"))
    forms = module("evaluate-form-text")
    form_images = module("evaluate-form-images")
    images = module("evaluate-existing-images")
    helper = module("evaluate-redaction-probe")
    render = module("evaluate-redaction-copy")
    source = base / "uniform.pdf"
    assert helper.digest(source) == fixed["files"][source.name]
    cases = [
        ("english", fixed["english_wrapped"], fixed["english_width_pt"], False),
        ("long_word", fixed["long_word_wrapped"], fixed["long_word_width_pt"], False),
        ("japanese", fixed["japanese_wrapped"], fixed["japanese_width_pt"], True),
        ("reedited", fixed["reedited_wrapped"], fixed["japanese_width_pt"], True),
        ("narrow", fixed["narrow_wrapped"], fixed["narrow_width_pt"], True),
        ("form", fixed["japanese_wrapped"], fixed["japanese_width_pt"], True),
    ]
    if args.with_ui:
        cases.append(
            ("ui", fixed["japanese_wrapped"], fixed["japanese_width_pt"], True)
        )
    directory = args.run / ("independent-wrap-poppler" + suffix)
    directory.mkdir(exist_ok=False)
    rows = []
    for name, value, width, explicit in cases:
        original_path = (
            args.fixtures / "existing-text-edit/forms/shared-forms.pdf"
            if name == "form"
            else source
        )
        old_text = (
            "Form first line\nForm second line" if name == "form" else fixed["selected"]
        )
        if name == "form":
            assert helper.digest(original_path) == fixed["source_sha256"]
        original = PdfReader(original_path)
        before = images.page_data(original_path)
        unchanged, old_bounds = forms.first_spans(before[0], old_text.split("\n"))
        path = args.run / f"existing-text-wrapped-{name}.pdf"
        reader = PdfReader(path)
        after = images.page_data(path)
        new_lines = value.split("\n")
        retained, new_bounds = forms.first_spans(after[0], new_lines)
        line_positions = []
        cursor = 0
        for line in new_lines:
            cursor = after[0]["text"].index(line, cursor)
            line_positions.append(cursor)
            cursor += len(line)
        expected = before[0]["text"]
        for line in old_text.split("\n"):
            expected = expected.replace(line, "", 1)
        expected += value
        unit = float(original.pages[0].get("/UserUnit", 1))
        actual_width = (
            max(b[0] + b[2] for b in new_bounds) - min(b[0] for b in new_bounds)
        ) * unit
        checks = dict(
            exact_page_character_counts=Counter(
                expected.replace("\r", "").replace("\n", "")
            )
            == Counter(after[0]["text"].replace("\r", "").replace("\n", "")),
            frozen_wrapped_lines_searchable=all(
                line in after[0]["text"] for line in new_lines
            ),
            frozen_wrapped_lines_extractable_in_pypdf=all(
                line in reader.pages[0].extract_text() for line in new_lines
            ),
            wrapped_line_order=line_positions == sorted(line_positions),
            physical_width_within_frozen_bound=actual_width <= width + 0.5,
            unselected_characters_and_positions_identical=sorted(unchanged)
            == sorted(retained),
            all_nested_image_pixels_and_positions_identical=form_images.drawn_images(
                original_path
            )
            == form_images.drawn_images(path),
            other_pages_identical=before[1:] == after[1:],
            form_values_identical={
                k: v.get("/V") for k, v in original.get_fields().items()
            }
            == {k: v.get("/V") for k, v in reader.get_fields().items()},
            widget_appearances_identical=images.widgets(original)
            == images.widgets(reader),
            page_geometry_identical=helper.geometry(original)
            == helper.geometry(reader),
        )
        if explicit:
            embedded = []
            for font in forms.active_fonts(reader.pages[0], reader):
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
            checks["explicit_Unicode_subset_embedded"] = bool(embedded) and all(
                embedded
            )
        outside = True
        for i in range(len(original.pages)):
            picture = render.poppler(
                args.poppler,
                original_path,
                directory / f"{name}-source-{i}",
                page=i + 1,
            )[0]
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
                physical_width=dict(actual=actual_width, bound=width, tolerance_pt=0.5),
                sha256=helper.digest(path),
            )
        )
    result = dict(
        status="PASS" if all(row["status"] == "PASS" for row in rows) else "FAIL",
        cases=rows,
        scope="Fixed English and Japanese wrapping at unchanged font size; PDFium, pypdf, fontTools and Poppler. Native viewers, complex paragraphs and IME not evaluated.",
    )
    output.write_text(
        json.dumps(result, ensure_ascii=False, indent=2), encoding="utf-8"
    )
    print(json.dumps(result, ensure_ascii=False, indent=2))
    if result["status"] != "PASS":
        raise SystemExit(1)


if __name__ == "__main__":
    main()
