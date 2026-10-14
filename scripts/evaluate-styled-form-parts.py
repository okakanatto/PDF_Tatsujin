"""Independently inspect the selected styled shared Form branch and all other calls."""

import argparse
import json
import re
from collections import Counter
from pathlib import Path

import numpy as np
import pypdfium2 as pdfium
from PIL import Image, ImageChops, ImageDraw
from pypdf import PdfReader, PdfWriter
from pypdf.generic import ContentStream, DictionaryObject, NameObject, ByteStringObject

from importlib.util import module_from_spec, spec_from_file_location


def module(name):
    spec = spec_from_file_location(name, Path(__file__).with_name(name + ".py"))
    result = module_from_spec(spec)
    spec.loader.exec_module(result)
    return result


styled = module("evaluate-existing-styled-parts")


def reference(
    source, destination, selected_call, selected_show, replacement=None, isolated=False
):
    """Build a normative edit/paint reference only from the fixed source and condition.

    Never read product output to select a mask or derive expected glyph positions.
    The isolated reference keeps text/font/matrix state and Form BBox clipping,
    removing all other painting. Renderer-specific hinted ink is thus measured
    with the same frozen 1px boundary instead of PDFium bounds for Poppler.
    """
    from pypdf._font import Font

    writer = PdfWriter()
    writer.clone_document_from_reader(PdfReader(source))
    page = writer.pages[0]
    calls = 0

    def encode(font, value):
        face = Font.from_font_resource(font.get_object())
        codes = {}
        for code in range(256):
            raw = bytes([code])
            decoded = (
                raw.decode(face.encoding, "surrogatepass")
                if isinstance(face.encoding, str)
                else face.encoding.get(code, chr(code))
            )
            unicode = "".join(face.character_map.get(c, c) for c in decoded)
            if len(unicode) == 1:
                codes.setdefault(unicode, raw)
        assert all(
            c in codes for c in value
        ), "Reference only uses characters representable by original font"
        return b"".join(codes[c] for c in value)

    def walk(content, resources, depth=0):
        nonlocal calls
        invocation = calls if depth == 2 else -1
        if depth == 2:
            calls += 1
        assert depth <= 8
        resources = DictionaryObject(resources.get_object())
        xobjects = DictionaryObject(
            resources.get("/XObject", DictionaryObject()).get_object()
        )
        stream = ContentStream(content, writer)
        operations = []
        shows = 0
        font = None
        selected = False
        for operands, operator in stream.operations:
            if operator == b"Tf":
                font = resources["/Font"][operands[0]]
            if operator in (b"Tj", b"TJ"):
                target = invocation == selected_call and shows == selected_show
                shows += 1
                if target:
                    selected = True
                    if replacement is not None:
                        operands, operator = [
                            ByteStringObject(encode(font, replacement))
                        ], b"Tj"
                    operations.append((operands, operator))
                elif not isolated:
                    operations.append((operands, operator))
                continue
            if operator == b"Do":
                obj = xobjects[operands[0]].get_object()
                if obj.get("/Subtype") == "/Form":
                    child, effective, changed = walk(
                        obj, obj.get("/Resources", resources), depth + 1
                    )
                    if changed:
                        attributes = dict(obj)
                        child.update(
                            {
                                k: v
                                for k, v in attributes.items()
                                if str(k)
                                not in (
                                    "/Length",
                                    "/Filter",
                                    "/DecodeParms",
                                    "/Resources",
                                )
                            }
                        )
                        child[NameObject("/Resources")] = effective
                        alias = NameObject("/IndependentSelectedForm")
                        assert alias not in xobjects
                        xobjects[alias] = writer._add_object(child)
                        operations.append(([alias], b"Do"))
                        selected = True
                    elif not isolated:
                        operations.append((operands, operator))
                elif not isolated:
                    operations.append((operands, operator))
                continue
            if isolated and operator in (
                b"S",
                b"s",
                b"f",
                b"F",
                b"f*",
                b"B",
                b"B*",
                b"b",
                b"b*",
                b"sh",
            ):
                operations.append(([], b"n"))
            else:
                operations.append((operands, operator))
        stream.operations = operations
        resources[NameObject("/XObject")] = xobjects
        return stream, resources, selected

    content, resources, selected = walk(page.get_contents(), page["/Resources"])
    assert selected and calls == 2
    page[NameObject("/Contents")] = writer._add_object(content)
    page[NameObject("/Resources")] = resources
    if isolated:
        page.pop("/Annots", None)
    assert not destination.exists()
    with destination.open("xb") as handle:
        writer.write(handle)


def first_spans(row, phrase, invocation):
    """Exclude only one physical invocation; retain every repeated sibling character."""
    boxes = row["character_boxes"]
    start = -len(phrase)
    for _ in range(invocation + 1):
        start = row["text"].index(phrase, start + len(phrase))
    end = start + len(phrase)
    ink = [boxes[i] for i in range(start, end) if not row["text"][i].isspace()]
    # PDFium boxes are [left,bottom,right,top], unlike this evaluator's masks.
    bounds = [
        [
            min(b[0] for b in ink),
            min(b[1] for b in ink),
            max(b[2] for b in ink) - min(b[0] for b in ink),
            max(b[3] for b in ink) - min(b[1] for b in ink),
        ]
    ]
    retained = [
        (c, boxes[i])
        for i, c in enumerate(row["text"])
        if not start <= i < end and c not in "\r\n"
    ]
    return retained, bounds


def tree(reader, page, selected_call, selected_show):
    """Expand executed Forms; ignore just the selected show, never sibling invocations."""
    calls = 0
    selected_count = 0

    def walk(content, resources, depth=0):
        nonlocal calls, selected_count
        assert depth <= 8
        invocation = calls if depth == 2 else -1
        if depth == 2:
            calls += 1
        shows = 0
        result = []
        for operands, operator in ContentStream(content, reader).operations:
            if operator in (b"Tj", b"TJ"):
                skip = invocation == selected_call and shows == selected_show
                shows += 1
                if skip:
                    selected_count += 1
                    result.append(("selected_show",))
                    continue
            if operator == b"Do":
                obj = resources["/XObject"][operands[0]].get_object()
                if obj.get("/Subtype") == "/Form":
                    effective = obj.get("/Resources", resources).get_object()
                    attrs = {
                        str(k): styled.semantic(v)
                        for k, v in obj.items()
                        if str(k)
                        not in ("/Resources", "/Length", "/Filter", "/DecodeParms")
                    }
                    result.append(
                        (
                            "Form",
                            attrs,
                            styled.semantic(effective.get("/Font", {})),
                            walk(obj, effective, depth + 1),
                        )
                    )
                    continue
                result.append(("Image", styled.semantic(obj)))
                continue
            result.append((operator.decode("ascii"), styled.semantic(operands)))
        return result

    value = walk(page.get_contents(), page["/Resources"].get_object())
    assert (
        selected_count == 1 and calls == 2
    ), "Exactly one selected nested show and two calls"
    return value


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--fixtures", required=True, type=Path)
    parser.add_argument("--run", required=True, type=Path)
    parser.add_argument("--poppler", required=True, type=Path)
    parser.add_argument("--attempt", default="")
    args = parser.parse_args()
    if args.poppler.is_dir():
        args.poppler /= "pdftoppm.exe"
    assert args.poppler.is_file()
    assert re.fullmatch(r"[A-Za-z0-9_-]{0,32}", args.attempt)
    suffix = "-" + args.attempt if args.attempt else ""
    output = args.run / ("independent-styled-forms" + suffix + ".json")
    assert not output.exists(), "Preserve previous inspection"
    base = args.fixtures / "existing-text-edit/styled-form-parts-v1"
    assert (
        styled.sha(base / "criteria.json")
        == "d7ae8793fb2d2e3e27f452885c75a005a30ab7a55ab2b18d452358302c6969e9"
    )
    fixed = json.loads((base / "criteria.json").read_text(encoding="utf-8"))
    images = module("evaluate-existing-images")
    forms = module("evaluate-form-images")
    render = module("evaluate-redaction-copy")
    helper = module("evaluate-redaction-probe")
    directory = args.run / ("independent-styled-form-pictures" + suffix)
    directory.mkdir(exist_ok=False)
    cases = []
    for name in fixed["positives"]:
        for index, (old, new) in enumerate(fixed["replacements"].items()):
            slug = ("intro", "bold", "suffix", "japanese")[index]
            cases.append(
                (
                    f"styled-form-{name}-{slug}",
                    base / (name + ".pdf"),
                    old,
                    new,
                    0,
                    index,
                )
            )
        for slug, new, call in (
            ("removed", "", 0),
            ("saved", "Bold text", 0),
            ("reedited", fixed["reedited"], 0),
            ("second", "Bold text", 1),
        ):
            cases.append(
                (
                    f"styled-form-{name}-{slug}",
                    base / (name + ".pdf"),
                    fixed["remove"],
                    new,
                    call,
                    1,
                )
            )
    cases += [
        (
            "styled-form-ui",
            args.run / "styled-form-ui-source.pdf",
            fixed["remove"],
            "Bold text",
            1,
            1,
        ),
        (
            "styled-form-ui-recovered",
            args.run / "styled-form-ui-source.pdf",
            fixed["remove"],
            fixed["reedited"],
            1,
            1,
        ),
    ]
    cache = {}
    rows = []
    for name, source, old, new, call, show in cases:
        normative = directory / (name + "-reference.pdf")
        old_ink = directory / (name + "-old-ink.pdf")
        new_ink = directory / (name + "-new-ink.pdf")
        reference(source, normative, call, show, new)
        reference(source, old_ink, call, show, isolated=True)
        reference(source, new_ink, call, show, new, isolated=True)
        poppler_ink = [
            render.poppler(
                args.poppler,
                p,
                directory / (p.stem + "-paint"),
                page=1,
                dpi=fixed["raster_dpi"],
            )[0]
            for p in (old_ink, new_ink)
        ]
        ink_bounds = [
            ImageChops.difference(
                picture.convert("RGB"), Image.new("RGB", picture.size, "white")
            ).getbbox()
            for picture in poppler_ink
        ]
        if source not in cache:
            if source.parent == base:
                assert styled.sha(source) == fixed["files"][source.name]
            original = PdfReader(source)
            before = images.page_data(source)
            real_before, generated_before = styled.real_characters(source, before[0])
            pictures = []
            with pdfium.PdfDocument(source) as pdf:
                for p in range(len(pdf)):
                    pictures.append(
                        pdf[p]
                        .render(scale=fixed["raster_dpi"] / 72)
                        .to_pil()
                        .convert("RGB")
                    )
            poppler = [
                render.poppler(
                    args.poppler,
                    source,
                    directory / (source.stem + f"-source-{p}"),
                    page=p + 1,
                    dpi=fixed["raster_dpi"],
                )[0]
                for p in range(len(original.pages))
            ]
            cache[source] = (
                original,
                before,
                real_before,
                generated_before,
                pictures,
                poppler,
            )
        original, before, real_before, generated_before, pictures, poppler = cache[
            source
        ]
        path = args.run / (name + ".pdf")
        reader = PdfReader(path)
        after = images.page_data(path)
        real_after, generated_after = styled.real_characters(path, after[0])
        unchanged, old_bounds = first_spans(real_before, old, call)
        if new:
            retained, new_bounds = first_spans(real_after, new, 0)
        else:
            retained = [
                (c, real_after["character_boxes"][i])
                for i, c in enumerate(real_after["text"])
                if c not in "\r\n"
            ]
            new_bounds = []
        expected = real_before["text"].replace(old, new, 1)
        checks = dict(
            exact_real_character_counts=Counter(
                expected.replace("\r", "").replace("\n", "")
            )
            == Counter(real_after["text"].replace("\r", "").replace("\n", "")),
            replacement_searchable=not new or new in after[0]["text"],
            pypdf_extracts_replacement=not new or new in reader.pages[0].extract_text(),
            unselected_characters_positions_exact=sorted(unchanged) == sorted(retained),
            expanded_content_fonts_attributes_exact=tree(
                original, original.pages[0], call, show
            )
            == tree(reader, reader.pages[0], call, show),
            all_original_page_resources_exact=all(
                (
                    styled.semantic(v)
                    == styled.semantic(reader.pages[0]["/Resources"][key])
                    if key != "/XObject"
                    else all(
                        styled.semantic(obj)
                        == styled.semantic(reader.pages[0]["/Resources"][key][k])
                        for k, obj in v.get_object().items()
                    )
                )
                for key, v in original.pages[0]["/Resources"].get_object().items()
            ),
            other_pages_exact=before[1:] == after[1:],
            other_page_content_resources_exact=all(
                styled.semantic(a.get_contents()) == styled.semantic(b.get_contents())
                and styled.semantic(a["/Resources"]) == styled.semantic(b["/Resources"])
                for a, b in zip(original.pages[1:], reader.pages[1:])
            ),
            nested_image_pixels_positions_exact=forms.drawn_images(source)
            == forms.drawn_images(path),
            geometry_exact=helper.geometry(original) == helper.geometry(reader),
            widgets_appearance_exact=images.widgets(original) == images.widgets(reader),
            form_values_exact={
                k: v.get("/V") for k, v in (original.get_fields() or {}).items()
            }
            == {k: v.get("/V") for k, v in (reader.get_fields() or {}).items()},
        )
        metrics = dict(
            pdfium_generated_layout_before=generated_before,
            pdfium_generated_layout_after=generated_after,
            poppler_reference_ink_bounds=ink_bounds,
            mask_reference="Fixed source + frozen replacement; independently encoded and cloned by pypdf; hinted paint bounds with unchanged 1px boundary",
        )
        with pdfium.PdfDocument(path) as pdf, pdfium.PdfDocument(
            normative
        ) as reference_pdf:
            for engine, originals in (("pdfium", pictures), ("poppler", poppler)):
                differences = 0
                reference_differences = 0
                for p, picture in enumerate(originals):
                    actual = (
                        pdf[p]
                        .render(scale=fixed["raster_dpi"] / 72)
                        .to_pil()
                        .convert("RGB")
                        if engine == "pdfium"
                        else render.poppler(
                            args.poppler,
                            path,
                            directory / (name + f"-{p}"),
                            page=p + 1,
                            dpi=fixed["raster_dpi"],
                        )[0]
                    )
                    assert picture.size == actual.size
                    expected_picture = (
                        reference_pdf[p]
                        .render(scale=fixed["raster_dpi"] / 72)
                        .to_pil()
                        .convert("RGB")
                        if engine == "pdfium"
                        else render.poppler(
                            args.poppler,
                            normative,
                            directory / (name + f"-reference-{p}"),
                            page=p + 1,
                            dpi=fixed["raster_dpi"],
                        )[0]
                    )
                    assert expected_picture.size == actual.size
                    reference_differences += int(
                        np.count_nonzero(
                            np.asarray(
                                ImageChops.difference(expected_picture, actual)
                            ).any(axis=2)
                        )
                    )
                    mask = Image.new("L", picture.size, 255)
                    if p == 0:
                        draw = ImageDraw.Draw(mask)
                        if engine == "poppler":
                            for bounds in ink_bounds:
                                if bounds:
                                    x0, y0, x1, y1 = bounds
                                    boundary = fixed["boundary_pixels"]
                                    draw.rectangle(
                                        (
                                            x0 - boundary,
                                            y0 - boundary,
                                            x1 - 1 + boundary,
                                            y1 - 1 + boundary,
                                        ),
                                        fill=0,
                                    )
                        else:
                            for bounds in old_bounds + new_bounds:
                                draw.rectangle(
                                    images.display_box(
                                        original.pages[0], bounds, picture.size
                                    ),
                                    fill=0,
                                )
                    delta = ImageChops.multiply(
                        ImageChops.difference(picture, actual), mask.convert("RGB")
                    )
                    differences += int(np.count_nonzero(np.asarray(delta).any(axis=2)))
                checks[engine + "_outside_pixels_zero"] = (
                    differences == fixed["outside_pixels"]
                )
                metrics[engine + "_outside_pixels"] = differences
                checks[engine + "_full_page_reference_exact"] = (
                    reference_differences == 0
                )
                metrics[engine + "_full_page_reference_difference_pixels"] = (
                    reference_differences
                )
        row = dict(
            file=path.name,
            status="PASS" if all(checks.values()) else "FAIL",
            checks=checks,
            metrics=metrics,
            sha256=styled.sha(path),
        )
        rows.append(row)
        print(json.dumps(dict(file=path.name, status=row["status"])), flush=True)
    result = dict(
        status="PASS" if all(r["status"] == "PASS" for r in rows) else "FAIL",
        cases=rows,
        scope="Fixed styled shared Form profile; all real unselected coordinates exact and 300dpi both-engine outside pixels zero; native/shipping unexecuted",
    )
    output.write_text(
        json.dumps(result, ensure_ascii=False, indent=2), encoding="utf-8"
    )
    print(
        json.dumps(
            dict(
                status=result["status"],
                count=len(rows),
                failures=[r["file"] for r in rows if r["status"] != "PASS"],
            )
        )
    )
    if result["status"] != "PASS":
        raise SystemExit(1)


if __name__ == "__main__":
    main()
