"""Inspect explicit body-font edits with independent PDF engines."""

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
    parser.add_argument("--fixtures", type=Path, required=True)
    parser.add_argument("--run", type=Path, required=True)
    parser.add_argument("--poppler", type=Path, required=True)
    args = parser.parse_args()
    output = args.run / "independent-existing-text-fonts.json"
    assert not output.exists(), "Keep prior inspection"
    text = module("evaluate-existing-text")
    images = module("evaluate-existing-images")
    render = module("evaluate-redaction-copy")
    helper = module("evaluate-redaction-probe")
    base = args.fixtures / "existing-text-edit/font-state"
    plan = json.loads((base / "criteria.json").read_text(encoding="utf-8"))
    source = base / plan["file"]
    assert helper.digest(source) == plan["sha256"], "Frozen font-state source"
    before = images.page_data(source)
    original = PdfReader(source)
    directory = args.run / "independent-font-poppler"
    directory.mkdir(exist_ok=False)
    pictures = [
        render.poppler(args.poppler, source, directory / f"source-{i}", page=i + 1)[0]
        for i in range(len(original.pages))
    ]
    old_chars, old_bounds = text.spans(before[0], [plan["selected"]])
    original_fonts = original.pages[0]["/Resources"]["/Font"].get_object()
    rows = []
    for name, family, replacement in [
        ("noto", "Noto Sans JP", plan["replacement"]),
        ("meiryo", "Meiryo UI", plan["replacement"]),
        ("reedited-noto", "Noto Sans JP", "テストします。本文の編集を"),
        ("reedited-meiryo", "Meiryo UI", "テストします。本文の編集を"),
        ("ui", "Meiryo UI", plan["replacement"]),
    ]:
        path = args.run / ("existing-text-font-" + name + ".pdf")
        reader = PdfReader(path)
        after = images.page_data(path)
        new_chars, new_bounds = text.spans(after[0], [replacement])
        fonts = reader.pages[0]["/Resources"]["/Font"].get_object()
        added = [v.get_object() for k, v in fonts.items() if k not in original_fonts]
        assert len(added) == 1, "Exactly one explicit new font"
        new_font = added[0]
        descendant = new_font["/DescendantFonts"][0].get_object()
        descriptor = descendant["/FontDescriptor"].get_object()
        program = descriptor["/FontFile2"].get_object().get_data()
        with TTFont(io.BytesIO(program)) as embedded:
            families = sorted(
                {
                    n.toUnicode()
                    for n in embedded["name"].names
                    if n.nameID in (1, 4, 6, 16)
                }
            )
            glyph_count = embedded["maxp"].numGlyphs
            embedding_flags = embedded["OS/2"].fsType
        checks = dict(
            expected_phrase_searchable_in_pdfium=replacement in after[0]["text"],
            expected_phrase_extractable_in_pypdf=replacement
            in reader.pages[0].extract_text(),
            exact_page_character_counts=Counter(
                before[0]["text"]
                .replace(plan["selected"], replacement)
                .replace("\r", "")
                .replace("\n", "")
            )
            == Counter(after[0]["text"].replace("\r", "").replace("\n", "")),
            all_unselected_characters_and_positions_identical=sorted(old_chars)
            == sorted(new_chars),
            other_pages_text_and_positions_identical=before[1:] == after[1:],
            all_images_identical=all(
                a["images"] == b["images"] for a, b in zip(before, after)
            ),
            widgets_and_appearances_identical=images.widgets(original)
            == images.widgets(reader),
            form_values_identical={
                k: v.get("/V") for k, v in original.get_fields().items()
            }
            == {k: v.get("/V") for k, v in reader.get_fields().items()},
            page_geometry_identical=helper.geometry(original)
            == helper.geometry(reader),
            actual_embedded_font_family=family in families,
            small_embedded_glyph_subset=0 < glyph_count <= len(set(replacement)) + 4
            and len(program) < 100000,
            editable_subset_embedding_allowed=(embedding_flags & 0xE in (0, 8))
            and not embedding_flags & 0x300,
            unicode_map_present=bool(new_font["/ToUnicode"].get_object().get_data()),
            original_font_resources_retained=set(original_fonts) <= set(fonts),
        )
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
        checks["poppler_pixels_outside_selected_glyphs_identical"] = outside
        rows.append(
            dict(
                file=path.name,
                status="PASS" if all(checks.values()) else "FAIL",
                checks=checks,
                embedded_names=families,
                embedded_program_bytes=len(program),
                embedded_glyphs=glyph_count,
                sha256=helper.digest(path),
            )
        )
    result = dict(
        status="PASS" if all(r["status"] == "PASS" for r in rows) else "FAIL",
        cases=rows,
        scope="Fixed one-line font replacements and reedit; PDFium, pypdf, fontTools and Poppler. Following no-Tf block included. One antialias pixel around selected glyphs. Native viewer/IME and general text editing not evaluated.",
    )
    output.write_text(
        json.dumps(result, ensure_ascii=False, indent=2), encoding="utf-8"
    )
    print(json.dumps(result, ensure_ascii=False, indent=2))
    if result["status"] != "PASS":
        raise SystemExit(1)


if __name__ == "__main__":
    main()
