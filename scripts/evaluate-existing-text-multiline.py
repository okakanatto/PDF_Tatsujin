"""Independently inspect frozen uniform two-line body edits."""

import argparse
import io
import json
from collections import Counter
from pathlib import Path
from fontTools.ttLib import TTFont
from PIL import Image, ImageChops, ImageDraw
from pypdf import PdfReader
from importlib.util import module_from_spec, spec_from_file_location


def module(name):
    spec = spec_from_file_location(name, Path(__file__).with_name(name + ".py"))
    result = module_from_spec(spec)
    spec.loader.exec_module(result)
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--fixtures", required=True, type=Path)
    parser.add_argument("--run", required=True, type=Path)
    parser.add_argument("--poppler", required=True, type=Path)
    parser.add_argument("--rotated", action="store_true")
    args = parser.parse_args()
    suffix = "-rotated" if args.rotated else ""
    output = args.run / ("independent-existing-text-multiline" + suffix + ".json")
    assert not output.exists(), "Preserve prior inspection"
    base = args.fixtures / (
        "existing-text-edit/multiline-rotated"
        if args.rotated
        else "existing-text-edit/multiline"
    )
    plan = json.loads((base / "criteria.json").read_text(encoding="utf-8"))
    images = module("evaluate-existing-images")
    helper = module("evaluate-redaction-probe")
    render = module("evaluate-redaction-copy")
    text = module("evaluate-existing-text")
    source = base / (plan["file"] if args.rotated else "uniform.pdf")
    assert helper.digest(source) == (
        plan["sha256"] if args.rotated else plan["files"]["uniform.pdf"]
    )
    original = PdfReader(source)
    before = images.page_data(source)
    old = plan["selected"].split("\n")
    unchanged, old_bounds = text.spans(before[0], old)
    directory = args.run / ("independent-multiline-poppler" + suffix)
    directory.mkdir(exist_ok=False)
    pictures = [
        render.poppler(args.poppler, source, directory / f"source-{i}", page=i + 1)[0]
        for i in range(len(original.pages))
    ]
    rows = []
    cases = (
        [("rotated", plan["selected"], False)]
        if args.rotated
        else [
            ("english", plan["replacement"], False),
            ("japanese", plan["japanese"], True),
            ("reedited", plan["reedited"], True),
            ("moved", plan["selected"], False),
            ("removed", "", False),
            ("ui", plan["japanese"], True),
        ]
    )
    for name, value, explicit in cases:
        path = args.run / ("existing-text-multiline-" + name + ".pdf")
        reader = PdfReader(path)
        after = images.page_data(path)
        new = value.split("\n") if value else []
        retained, new_bounds = text.spans(after[0], new)
        expected = before[0]["text"]
        for index, line in enumerate(old):
            expected = expected.replace(line, new[index] if new else "")
        checks = dict(
            exact_page_character_counts=Counter(
                expected.replace("\r", "").replace("\n", "")
            )
            == Counter(after[0]["text"].replace("\r", "").replace("\n", "")),
            both_replacement_lines_searchable=all(
                line in after[0]["text"] for line in new
            ),
            both_lines_extractable_in_pypdf=all(
                line in reader.pages[0].extract_text() for line in new
            ),
            unselected_characters_and_positions_identical=sorted(unchanged)
            == sorted(retained),
            other_pages_identical=before[1:] == after[1:],
            all_images_identical=all(
                a["images"] == b["images"] for a, b in zip(before, after)
            ),
            form_values_identical={
                k: v.get("/V") for k, v in original.get_fields().items()
            }
            == {k: v.get("/V") for k, v in reader.get_fields().items()},
            widgets_and_appearances_identical=images.widgets(original)
            == images.widgets(reader),
            page_geometry_identical=helper.geometry(original)
            == helper.geometry(reader),
        )
        if explicit:
            embedded = []
            for font in reader.pages[0]["/Resources"]["/Font"].get_object().values():
                font = font.get_object()
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
            checks["explicit_family_embedded_as_small_Unicode_subset"] = bool(
                embedded
            ) and all(embedded)
        outside = True
        for i, picture in enumerate(pictures):
            actual = render.poppler(
                args.poppler, path, directory / f"{name}-{i}", page=i + 1
            )[0]
            assert actual.size == picture.size
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
        checks["poppler_outside_selected_line_glyphs_identical"] = outside
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
        scope="Frozen uniform two-line body; PDFium, pypdf, fontTools and Poppler, one antialias pixel around selected line glyphs. General paragraphs, native viewer and IME not evaluated.",
    )
    output.write_text(
        json.dumps(result, ensure_ascii=False, indent=2), encoding="utf-8"
    )
    print(json.dumps(result, ensure_ascii=False, indent=2))
    if result["status"] != "PASS":
        raise SystemExit(1)


if __name__ == "__main__":
    main()
