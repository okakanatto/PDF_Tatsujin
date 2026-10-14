"""Independently inspect fixed body-text edits, without claiming general acceptance."""

import argparse
import importlib.util
import json
from collections import Counter
from pathlib import Path
from PIL import Image, ImageChops, ImageDraw
from pypdf import PdfReader


def module(name):
    path = Path(__file__).with_name(name)
    spec = importlib.util.spec_from_file_location("tatsu_" + path.stem, path)
    result = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(result)
    return result


def spans(row, selected):
    text = row["text"]
    boxes = row["character_boxes"]
    assert len(text) == len(boxes), "Explicit character/box correspondence"
    exclusions = set()
    bounds = []
    for value in selected:
        start = text.index(value)
        assert text.find(value, start + 1) < 0, "Unique selected source text"
        positions = range(start, start + len(value))
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
    unchanged = [
        (c, boxes[i])
        for i, c in enumerate(text)
        if i not in exclusions and c not in "\r\n"
    ]
    return unchanged, bounds


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--fixtures", type=Path, required=True)
    parser.add_argument("--run", type=Path, required=True)
    parser.add_argument("--poppler", type=Path, required=True)
    parser.add_argument("--with-ui", action="store_true")
    parser.add_argument("--tag", default="")
    args = parser.parse_args()
    if len(args.tag) > 20 or any(
        c not in "abcdefghijklmnopqrstuvwxyz0123456789-" for c in args.tag
    ):
        raise ValueError("Short inspection tag")
    suffix = "-" + args.tag if args.tag else ""
    output = args.run / ("independent-existing-text" + suffix + ".json")
    assert not output.exists(), "Preserve prior inspection"
    images = module("evaluate-existing-images.py")
    render = module("evaluate-redaction-copy.py")
    helper = module("evaluate-redaction-probe.py")
    base = args.fixtures / "existing-text-edit/visible"
    plan = json.loads((base / "criteria.json").read_text(encoding="utf-8"))
    source = base / plan["file"]
    assert helper.digest(source) == plan["sha256"], "Frozen body source"
    directory = args.run / ("independent-text-poppler" + suffix)
    directory.mkdir(exist_ok=False)
    english = "Original body line"
    japanese = "日本語の本文を編集します"
    cases = [
        ("english", {english: plan["replacements"][english]}),
        ("japanese", {japanese: plan["replacements"][japanese]}),
        ("restored-english", {}),
        ("restored-japanese", {}),
        ("moved", {english: english}),
        ("removed", {english: ""}),
        ("rotated", {english: english}),
    ]
    if args.with_ui:
        cases += [
            ("ui", plan["replacements"]),
            ("ui-removed", {english: ""}),
            ("ui-moved", {english: english}),
        ]
    references = {}

    def reference(path):
        if path not in references:
            reader = PdfReader(path)
            data = images.page_data(path)
            pictures = [
                render.poppler(
                    args.poppler,
                    path,
                    directory / ("source-" + helper.digest(path)[:12] + "-" + str(i)),
                    page=i + 1,
                )[0]
                for i in range(len(reader.pages))
            ]
            references[path] = (reader, data, pictures)
        return references[path]

    rows = []
    for name, replacements in cases:
        original, before, pictures = reference(
            args.run / "existing-text-ui-source.pdf"
            if name == "ui"
            else (
                args.fixtures / "existing-text-edit/rotated/body-text.pdf"
                if name == "rotated"
                else source
            )
        )
        path = args.run / ("existing-text-" + name + ".pdf")
        reader = PdfReader(path)
        after = images.page_data(path)
        old = list(replacements)
        new = [v for v in replacements.values() if v]
        before_unselected, old_bounds = spans(before[0], old)
        after_unselected, new_bounds = spans(after[0], new)
        expected = before[0]["text"]
        for first, last in replacements.items():
            expected = expected.replace(first, last)
        checks = dict(
            selected_phrases_exact_and_page_character_counts_equal=Counter(
                expected.replace("\r", "").replace("\n", "")
            )
            == Counter(after[0]["text"].replace("\r", "").replace("\n", ""))
            and all(value in after[0]["text"] for value in new),
            unselected_characters_and_positions_identical=sorted(before_unselected)
            == sorted(after_unselected),
            other_page_text_and_character_positions_identical=all(
                a["text"] == b["text"] and a["character_boxes"] == b["character_boxes"]
                for a, b in zip(before[1:], after[1:])
            ),
            all_images_and_positions_identical=all(
                a["images"] == b["images"] for a, b in zip(before, after)
            ),
            page_geometry_identical=helper.geometry(original)
            == helper.geometry(reader),
            widgets_geometry_and_appearance_identical=images.widgets(original)
            == images.widgets(reader),
            form_values_identical={
                k: v.get("/V") for k, v in (original.get_fields() or {}).items()
            }
            == {k: v.get("/V") for k, v in (reader.get_fields() or {}).items()},
            pypdf_sees_expected_text=all(
                v in (reader.pages[0].extract_text() or "") for v in new
            ),
        )
        outside = True
        for index, picture in enumerate(pictures):
            actual, _ = render.poppler(
                args.poppler,
                path,
                directory / (name + "-" + str(index)),
                page=index + 1,
            )
            assert actual.size == picture.size, "Rendered geometry"
            mask = Image.new("L", picture.size, 255)
            if index == 0:
                draw = ImageDraw.Draw(mask)
                for rect in old_bounds + new_bounds:
                    draw.rectangle(
                        images.display_box(original.pages[0], rect, picture.size),
                        fill=0,
                    )
            outside &= (
                ImageChops.multiply(
                    ImageChops.difference(picture, actual), mask.convert("RGB")
                ).getbbox()
                is None
            )
        checks["poppler_outside_selected_glyph_bounds_exact"] = outside
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
        scope="Fixed synthetic horizontal blocks; PDFium, pypdf and Poppler; one antialias pixel at selected glyph bounds. Not general body-text or native viewer acceptance.",
    )
    output.write_text(
        json.dumps(result, ensure_ascii=False, indent=2), encoding="utf-8"
    )
    print(json.dumps(result, ensure_ascii=False, indent=2))
    if result["status"] != "PASS":
        raise SystemExit(1)


if __name__ == "__main__":
    main()
