"""Inspect styled-part edits using PDFium, pypdf, and both 300dpi renderers."""

import argparse
import hashlib
import importlib.util
import json
import re
from collections import Counter
from pathlib import Path

import pypdfium2 as pdfium
import numpy as np
from PIL import Image, ImageChops, ImageDraw
from pypdf import PdfReader
from pypdf.generic import ContentStream


def module(name):
    spec = importlib.util.spec_from_file_location(
        name, Path(__file__).with_name(name + ".py")
    )
    result = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(result)
    return result


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def real_characters(path, row):
    """Separate PDFium's generated layout whitespace from PDF text characters."""
    with pdfium.PdfDocument(path) as document:
        page = document[0]
        text = page.get_textpage()
        generated = [
            i
            for i in range(text.count_chars())
            if pdfium.raw.FPDFText_IsGenerated(text, i)
        ]
        assert all(
            row["text"][i].isspace() for i in generated
        ), "Generated entries must only be layout whitespace"
        generated_set = set(generated)
        actual = dict(row)
        actual["text"] = "".join(
            c for i, c in enumerate(row["text"]) if i not in generated_set
        )
        actual["character_boxes"] = [
            b for i, b in enumerate(row["character_boxes"]) if i not in generated_set
        ]
        diagnostic = [(i, row["text"][i], row["character_boxes"][i]) for i in generated]
        text.close()
        page.close()
    return actual, diagnostic


def semantic(value):
    value = value.get_object() if hasattr(value, "get_object") else value
    if hasattr(value, "get_data"):
        return {
            "bytes": hashlib.sha256(value.get_data()).hexdigest(),
            "attributes": {
                str(k): semantic(v)
                for k, v in value.items()
                if str(k) not in ("/Length", "/Filter", "/DecodeParms")
            },
        }
    if isinstance(value, dict):
        return {str(k): semantic(v) for k, v in value.items()}
    if isinstance(value, list):
        return [semantic(v) for v in value]
    if isinstance(value, bytes):
        return value.hex()
    return str(value)


def operations(reader, occurrence):
    result = []
    index = 0
    for values, operator in ContentStream(
        reader.pages[0].get_contents(), reader
    ).operations:
        if operator in (b"Tj", b"TJ"):
            selected = index == occurrence
            index += 1
            if selected:
                continue
        result.append((operator.decode("ascii"), semantic(values)))
    return result


def selected_index(reader, phrase):
    # Decode source show operations individually with pypdf's own text engine,
    # preserving the source font-state commands and positioning. No product metadata.
    content = ContentStream(reader.pages[0].get_contents(), reader)
    from pypdf._font import Font

    fonts = {}
    for name, value in reader.pages[0]["/Resources"]["/Font"].get_object().items():
        fonts[name] = Font.from_font_resource(value.get_object())
    # TJ can split into several text events. Use the explicit unique source show
    # occurrence given by source Unicode and the frozen sequence below instead.
    source_parts = []
    font_name = None
    for operands, operator in content.operations:
        if operator == b"Tf":
            font_name = operands[0]
        if operator not in (b"Tj", b"TJ"):
            continue
        font = fonts[font_name]
        pieces = (
            [operands[0]]
            if operator == b"Tj"
            else [v for v in operands[0] if isinstance(v, (str, bytes))]
        )
        decoded = ""
        for piece in pieces:
            raw = (
                piece.original_bytes
                if hasattr(piece, "original_bytes")
                else bytes(piece)
            )
            encoded = (
                raw.decode(font.encoding, "surrogatepass")
                if isinstance(font.encoding, str)
                else "".join(font.encoding.get(c, chr(c)) for c in raw)
            )
            decoded += "".join(font.character_map.get(c, c) for c in encoded)
        source_parts.append(decoded)
    assert source_parts.count(phrase) == 1, "Unique selected show"
    return source_parts.index(phrase)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--fixtures", required=True, type=Path)
    parser.add_argument("--run", required=True, type=Path)
    parser.add_argument("--poppler", required=True, type=Path)
    parser.add_argument("--attempt", default="")
    args = parser.parse_args()
    if args.poppler.is_dir():
        args.poppler = args.poppler / "pdftoppm.exe"
    assert args.poppler.is_file(), "Poppler executable required"
    assert re.fullmatch(r"[A-Za-z0-9_-]{0,32}", args.attempt), "Safe inspection attempt"
    suffix = "-" + args.attempt if args.attempt else ""
    output = args.run / ("independent-styled-parts" + suffix + ".json")
    assert not output.exists(), "Preserve prior independent result"
    images = module("evaluate-existing-images")
    text = module("evaluate-existing-text")
    render = module("evaluate-redaction-copy")
    helper = module("evaluate-redaction-probe")
    base = args.fixtures / "existing-text-edit/styled-parts-v1"
    plan = json.loads((base / "criteria.json").read_text(encoding="utf-8"))
    assert (
        sha(base / "criteria.json")
        == "76781b4b06150f72fcc94d5cf9b693cd4c2485e0a58e0ba78549df5348e1aab9"
    )
    directory = args.run / ("independent-styled-pictures" + suffix)
    directory.mkdir(exist_ok=False)
    cases = []
    for source_name in ("absolute", "rotated"):
        for old, new in plan["replacements"].items():
            slug = (
                "intro"
                if old.startswith("Intro")
                else (
                    "bold"
                    if old.startswith("Styled")
                    else "suffix" if old.startswith("Suffix") else "japanese"
                )
            )
            cases.append(
                (
                    f"styled-{source_name}-{slug}",
                    base / (source_name + ".pdf"),
                    old,
                    new,
                )
            )
        for slug, new in (
            ("removed", ""),
            ("saved", "Bold text"),
            ("reedited", plan["reedited"]),
        ):
            cases.append(
                (
                    f"styled-{source_name}-{slug}",
                    base / (source_name + ".pdf"),
                    plan["remove"],
                    new,
                )
            )
    cases.append(
        ("styled-ui", args.run / "styled-ui-source.pdf", plan["remove"], "Bold text")
    )
    cache = {}
    rows = []
    for name, source, old, new in cases:
        if source not in cache:
            if source.parent == base:
                assert sha(source) == plan["files"][source.name]
            original = PdfReader(source)
            before = images.page_data(source)
            pictures = []
            with pdfium.PdfDocument(source) as pdf:
                for page in range(len(pdf)):
                    pictures.append(
                        pdf[page]
                        .render(scale=plan["raster_dpi"] / 72)
                        .to_pil()
                        .convert("RGB")
                    )
            poppler = [
                render.poppler(
                    args.poppler,
                    source,
                    directory / (source.stem + f"-source-{p}"),
                    page=p + 1,
                    dpi=plan["raster_dpi"],
                )[0]
                for p in range(len(original.pages))
            ]
            cache[source] = original, before, pictures, poppler
        original, before, pictures, poppler = cache[source]
        path = args.run / (name + ".pdf")
        reader = PdfReader(path)
        after = images.page_data(path)
        real_before, generated_before = real_characters(source, before[0])
        real_after, generated_after = real_characters(path, after[0])
        unchanged, old_bounds = text.spans(real_before, [old])
        retained, new_bounds = text.spans(real_after, [new] if new else [])
        occurrence = selected_index(original, old)
        expected = real_before["text"].replace(old, new, 1)
        checks = dict(
            exact_real_character_counts=Counter(
                expected.replace("\r", "").replace("\n", "")
            )
            == Counter(real_after["text"].replace("\r", "").replace("\n", "")),
            selected_searchable=not new or new in after[0]["text"],
            pypdf_extracts_replacement=not new or new in reader.pages[0].extract_text(),
            unselected_characters_positions_exact=sorted(unchanged) == sorted(retained),
            unselected_content_commands_exact=operations(original, occurrence)
            == operations(reader, occurrence),
            font_resources_exact=semantic(original.pages[0]["/Resources"]["/Font"])
            == semantic(reader.pages[0]["/Resources"]["/Font"]),
            other_pages_exact=before[1:] == after[1:],
            images_exact=all(a["images"] == b["images"] for a, b in zip(before, after)),
            geometry_exact=helper.geometry(original) == helper.geometry(reader),
            widgets_appearance_exact=images.widgets(original) == images.widgets(reader),
            forms_exact={
                k: v.get("/V") for k, v in (original.get_fields() or {}).items()
            }
            == {k: v.get("/V") for k, v in (reader.get_fields() or {}).items()},
        )
        metrics = dict(
            pdfium_generated_layout_before=generated_before,
            pdfium_generated_layout_after=generated_after,
            generated_layout_is_not_pdf_text="FPDFText_IsGenerated; only whitespace; all real glyphs, including explicit spaces, compared exactly",
        )
        with pdfium.PdfDocument(path) as pdf:
            for engine, originals in (("pdfium", pictures), ("poppler", poppler)):
                differences = 0
                for p, picture in enumerate(originals):
                    actual = (
                        pdf[p]
                        .render(scale=plan["raster_dpi"] / 72)
                        .to_pil()
                        .convert("RGB")
                        if engine == "pdfium"
                        else render.poppler(
                            args.poppler,
                            path,
                            directory / (name + f"-{p}"),
                            page=p + 1,
                            dpi=plan["raster_dpi"],
                        )[0]
                    )
                    assert actual.size == picture.size
                    mask = Image.new("L", picture.size, 255)
                    if p == 0:
                        draw = ImageDraw.Draw(mask)
                        for rectangle in old_bounds + new_bounds:
                            draw.rectangle(
                                images.display_box(
                                    original.pages[0], rectangle, picture.size
                                ),
                                fill=0,
                            )
                    delta = ImageChops.multiply(
                        ImageChops.difference(picture, actual), mask.convert("RGB")
                    )
                    differences += int(np.count_nonzero(np.asarray(delta).any(axis=2)))
                checks[engine + "_outside_pixels_zero"] = (
                    differences == plan["outside_pixels"]
                )
                metrics[engine + "_outside_pixels"] = differences
        rows.append(
            dict(
                file=path.name,
                status="PASS" if all(checks.values()) else "FAIL",
                checks=checks,
                metrics=metrics,
                sha256=sha(path),
            )
        )
        print(json.dumps(dict(file=path.name, status=rows[-1]["status"])), flush=True)
    result = dict(
        status="PASS" if all(row["status"] == "PASS" for row in rows) else "FAIL",
        cases=rows,
        scope="Independent-position styled parts; exact positions/content/resources and 300dpi both-engine outside pixels. Native shipping evaluation unexecuted.",
    )
    output.write_text(
        json.dumps(result, ensure_ascii=False, indent=2), encoding="utf-8"
    )
    print(
        json.dumps(
            dict(
                status=result["status"],
                cases=len(rows),
                failures=[r for r in rows if r["status"] != "PASS"],
            ),
            ensure_ascii=True,
        )
    )
    if result["status"] != "PASS":
        raise SystemExit(1)


if __name__ == "__main__":
    main()
