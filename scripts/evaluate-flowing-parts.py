"""Inspect flowing parts against independently encoded fixed-source references."""

import argparse
from collections import Counter
import json
import re
from pathlib import Path

import numpy as np
import pypdfium2 as pdfium
from PIL import Image, ImageChops, ImageDraw
from pypdf import PdfReader, PdfWriter
from pypdf.generic import ByteStringObject, ContentStream, DictionaryObject, NameObject

from importlib.util import module_from_spec, spec_from_file_location


def module(name):
    spec = spec_from_file_location(name, Path(__file__).with_name(name + ".py"))
    result = module_from_spec(spec)
    spec.loader.exec_module(result)
    return result


styled = module("evaluate-existing-styled-parts")
form_text = module("evaluate-styled-form-parts")


def reference(
    source,
    destination,
    depth,
    call,
    selected_show,
    replacement=None,
    isolated=False,
    selected_line=0,
):
    """Use source bytes and frozen text only; isolated paint retains all line advances."""
    from pypdf._font import Font

    writer = PdfWriter()
    writer.clone_document_from_reader(PdfReader(source))
    page = writer.pages[0]
    calls = 0

    def encode(font, text):
        face = Font.from_font_resource(font.get_object())
        codes = {}
        for code in range(256):
            raw = bytes([code])
            encoded = (
                raw.decode(face.encoding, "surrogatepass")
                if isinstance(face.encoding, str)
                else face.encoding.get(code, chr(code))
            )
            decoded = "".join(face.character_map.get(c, c) for c in encoded)
            if len(decoded) == 1:
                codes.setdefault(decoded, raw)
        assert all(c in codes for c in text), "Original font must encode normative text"
        return b"".join(codes[c] for c in text)

    def walk(content, resources, current_depth=0):
        nonlocal calls
        invocation = calls if current_depth == 2 else 0
        if current_depth == 2:
            calls += 1
        assert current_depth <= 8
        resources = DictionaryObject(resources.get_object())
        xobjects = DictionaryObject(
            resources.get("/XObject", DictionaryObject()).get_object()
        )
        stream = ContentStream(content, writer)
        operations = []
        line = False
        physical_line = 0
        first = True
        show = 0
        changed = False
        font = None
        for operands, operator in stream.operations:
            if operator == b"BT":
                line, first, show = False, True, 0
                physical_line = 0
            if operator in (b"T*", b"Td", b"TD"):
                physical_line += 1
            if operator == b"Tf":
                font = resources["/Font"][operands[0]]
            if operator in (b"Tj", b"TJ"):
                if first:
                    line = operator == b"Tj" and str(operands[0]) == "Intro "
                    first = False
                target_line = (
                    line
                    and physical_line == selected_line
                    and current_depth == depth
                    and invocation == call
                )
                if target_line:
                    changed = True
                    if show == selected_show and replacement is not None:
                        operands, operator = [
                            ByteStringObject(encode(font, replacement))
                        ], b"Tj"
                    operations.append((operands, operator))
                elif not isolated:
                    operations.append((operands, operator))
                show += 1
                continue
            if operator == b"Do":
                obj = xobjects[operands[0]].get_object()
                if obj.get("/Subtype") == "/Form":
                    child, effective, selected = walk(
                        obj, obj.get("/Resources", resources), current_depth + 1
                    )
                    if selected:
                        child.update(
                            {
                                k: v
                                for k, v in obj.items()
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
                        alias = NameObject("/IndependentFlowLine")
                        assert alias not in xobjects
                        xobjects[alias] = writer._add_object(child)
                        operations.append(([alias], b"Do"))
                        changed = True
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
        return stream, resources, changed

    content, resources, changed = walk(page.get_contents(), page["/Resources"])
    assert changed and (calls == 2 if depth else calls == 0)
    page[NameObject("/Contents")] = writer._add_object(content)
    page[NameObject("/Resources")] = resources
    if isolated:
        page.pop("/Annots", None)
    assert not destination.exists(), "Preserve all previous references"
    with destination.open("xb") as handle:
        writer.write(handle)


def characters(row):
    return sorted(zip(row["text"], row["character_boxes"]))


def outside_line(row, phrases, call, selected_show=None, new=None):
    excluded = set()
    for index, phrase in enumerate(phrases):
        occurrence = call
        if index == selected_show:
            phrase, occurrence = new, 0
        if not phrase:
            continue
        start = -len(phrase)
        for _ in range(occurrence + 1):
            start = row["text"].index(phrase, start + len(phrase))
        excluded.update(range(start, start + len(phrase)))
    return sorted(
        (c, row["character_boxes"][i])
        for i, c in enumerate(row["text"])
        if i not in excluded
    )


def outside_reference(row, isolated):
    """Subtract one physical line, even when another line has identical text."""
    whole = Counter(
        (c, tuple(box)) for c, box in zip(row["text"], row["character_boxes"])
    )
    line = Counter(
        (c, tuple(box)) for c, box in zip(isolated["text"], isolated["character_boxes"])
    )
    assert not line - whole, "Source-only isolated glyphs must belong to the full page"
    return sorted((whole - line).items())


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--fixtures", required=True, type=Path)
    parser.add_argument("--run", required=True, type=Path)
    parser.add_argument("--poppler", required=True, type=Path)
    parser.add_argument("--attempt", default="")
    parser.add_argument("--profile", choices=("parts", "lines"), default="parts")
    parser.add_argument("--reuse-run", type=Path)
    args = parser.parse_args()
    if args.poppler.is_dir():
        args.poppler /= "pdftoppm.exe"
    assert args.poppler.is_file()
    assert re.fullmatch(r"[A-Za-z0-9_-]{0,32}", args.attempt)
    suffix = "-" + args.attempt if args.attempt else ""
    family = "flowing-" + args.profile
    prefix = "flowing-lines-" if args.profile == "lines" else "flowing-"
    output = args.run / ("independent-" + family + suffix + ".json")
    assert not output.exists()
    process = json.loads(
        (args.run / "process-result.json").read_text(encoding="utf-8-sig")
    )
    assert (
        process["completed"] and process["exit_code"] == 0
    ), "Completed successful producer required"
    base = args.fixtures / ("existing-text-edit/" + family + "-v1")
    assert styled.sha(base / "criteria.json") == (
        "7d30fb24b74b61ffd35558f513d68a87d2b21a7871a86eb69161ccb97a629bd7"
        if args.profile == "lines"
        else "1ac4f62edf2325101e0c8d0230a339518d6ae449eafd74fec2f5351fea968818"
    )
    fixed = json.loads((base / "criteria.json").read_text(encoding="utf-8"))
    prior_cases = {}
    if args.reuse_run:
        prior = json.loads(
            (args.reuse_run / ("independent-" + family + ".json")).read_text(
                encoding="utf-8"
            )
        )
        prior_process = json.loads(
            (args.reuse_run / "process-result.json").read_text(encoding="utf-8-sig")
        )
        assert prior_process["completed"] and prior_process["exit_code"] == 0
        assert prior["criteria_sha256"] == styled.sha(base / "criteria.json")
        prior_cases = {row["file"]: row for row in prior["cases"]}
    images = module("evaluate-existing-images")
    forms = module("evaluate-form-images")
    render = module("evaluate-redaction-copy")
    helper = module("evaluate-redaction-probe")
    directory = args.run / ("independent-" + family + "-pictures" + suffix)
    directory.mkdir(exist_ok=False)
    cases = []
    phrases = list(fixed["replacements"])
    show_offset = fixed.get("show_offset", 0)
    line_number = fixed.get("selected_line", 0)
    for name, depth in fixed["positives"].items():
        for show, new in enumerate(fixed["replacements"].values()):
            slug = ("intro", "bold", "suffix", "japanese")[show]
            cases.append(
                (
                    f"{prefix}{name}-{slug}",
                    base / (name + ".pdf"),
                    depth,
                    0,
                    show + show_offset,
                    new,
                    line_number,
                    phrases,
                )
            )
        for slug, new in (
            ("removed", ""),
            ("saved", "Bold text"),
            ("reedited", fixed["reedited"]),
        ):
            cases.append(
                (
                    f"{prefix}{name}-{slug}",
                    base / (name + ".pdf"),
                    depth,
                    0,
                    1 + show_offset,
                    new,
                    line_number,
                    phrases,
                )
            )
        if name in fixed["second_call_cases"]:
            cases.append(
                (
                    f"{prefix}{name}-second",
                    base / (name + ".pdf"),
                    depth,
                    1,
                    1 + show_offset,
                    "Bold text",
                    line_number,
                    phrases,
                )
            )
        if args.profile == "lines":
            first_phrases = [
                "Intro ",
                "Styled phrase",
                "Suffix intact",
                "編集します日本語本文",
            ]
            last_phrases = [
                "Third ",
                "Bold third",
                "Suffix third",
                "本文編集します日本語",
            ]
            cases.extend(
                [
                    (
                        f"{prefix}{name}-first",
                        base / (name + ".pdf"),
                        depth,
                        0,
                        0,
                        "First ",
                        0,
                        first_phrases,
                    ),
                    (
                        f"{prefix}{name}-last",
                        base / (name + ".pdf"),
                        depth,
                        0,
                        9,
                        "Last text",
                        2,
                        last_phrases,
                    ),
                ]
            )
    cases += [
        (
            prefix + "ui-replaced",
            args.run / (prefix + "ui-source.pdf"),
            2,
            1,
            1 + show_offset,
            "Bold text",
            line_number,
            phrases,
        ),
        (
            prefix + "ui-removed",
            args.run / (prefix + "ui-source.pdf"),
            2,
            1,
            1 + show_offset,
            "",
            line_number,
            phrases,
        ),
    ]
    cache = {}
    rows = []
    for name, source, depth, call, show, new, selected_line, line_phrases in cases:
        normative = directory / (name + "-reference.pdf")
        old_ink, new_ink = [
            directory / (name + "-" + s + "-ink.pdf") for s in ("old", "new")
        ]
        reference(
            source, normative, depth, call, show, new, selected_line=selected_line
        )
        reference(
            source,
            old_ink,
            depth,
            call,
            show,
            isolated=True,
            selected_line=selected_line,
        )
        reference(
            source,
            new_ink,
            depth,
            call,
            show,
            new,
            isolated=True,
            selected_line=selected_line,
        )
        if source not in cache:
            if source.parent == base:
                assert styled.sha(source) == fixed["files"][source.name]
            original = PdfReader(source)
            before = images.page_data(source)
            real_before, generated_before = styled.real_characters(source, before[0])
            originals = {}
            with pdfium.PdfDocument(source) as pdf:
                originals["pdfium"] = [
                    pdf[p]
                    .render(scale=fixed["raster_dpi"] / 72)
                    .to_pil()
                    .convert("RGB")
                    for p in range(len(pdf))
                ]
            originals["poppler"] = [
                render.poppler(
                    args.poppler,
                    source,
                    directory / (source.stem + f"-source-{p}"),
                    page=p + 1,
                    dpi=fixed["raster_dpi"],
                )[0]
                for p in range(len(original.pages))
            ]
            cache[source] = original, before, real_before, generated_before, originals
        original, before, real_before, generated_before, originals = cache[source]
        path = args.run / (name + ".pdf")
        reader = PdfReader(path)
        after = images.page_data(path)
        expected = images.page_data(normative)
        real_after, generated_after = styled.real_characters(path, after[0])
        real_expected, _ = styled.real_characters(normative, expected[0])
        if args.profile == "lines":
            old_row = images.page_data(old_ink)[0]
            new_row = images.page_data(new_ink)[0]
            old_real, _ = styled.real_characters(old_ink, old_row)
            new_real, _ = styled.real_characters(new_ink, new_row)
            outside_exact = outside_reference(
                real_before, old_real
            ) == outside_reference(real_after, new_real)
        else:
            outside_exact = outside_line(
                real_before, line_phrases, call
            ) == outside_line(real_after, line_phrases, call, show % 4, new)
        if depth:
            content_exact = form_text.tree(
                original, original.pages[0], call, show
            ) == form_text.tree(reader, reader.pages[0], call, show)
        else:
            index = styled.selected_index(original, line_phrases[show % 4])
            content_exact = styled.operations(original, index) == styled.operations(
                reader, index
            )
        checks = dict(
            all_real_coordinates_exact_to_reference=characters(real_after)
            == characters(real_expected),
            unselected_blocks_calls_coordinates_exact=outside_exact,
            replacement_searchable=not new or new in after[0]["text"],
            pypdf_extracts_replacement=not new or new in reader.pages[0].extract_text(),
            commands_original_fonts_exact=content_exact,
            original_resources_exact=all(
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
            images_positions_pixels_exact=forms.drawn_images(source)
            == forms.drawn_images(path),
            geometry_exact=helper.geometry(original) == helper.geometry(reader),
            widgets_exact=images.widgets(original) == images.widgets(reader),
        )
        metrics = dict(
            generated_before=generated_before,
            generated_after=generated_after,
            reference="Source + frozen text only; whole flowing line isolated with all advances; 300dpi/1px/0 unchanged",
        )
        pixel_checks = [
            engine + check
            for engine in ("pdfium", "poppler")
            for check in ("_full_reference_pixels_exact", "_outside_pixels_zero")
        ]
        prior_row = prior_cases.get(path.name)
        prior_directory = (
            args.reuse_run / ("independent-" + family + "-pictures")
            if args.reuse_run
            else None
        )
        reuse = (
            prior_row is not None
            and prior_row["sha256"] == styled.sha(path)
            and all(prior_row["checks"][key] for key in pixel_checks)
            and all(
                styled.sha(reference_path)
                == styled.sha(prior_directory / reference_path.name)
                for reference_path in (normative, old_ink, new_ink)
            )
        )
        if reuse:
            checks.update({key: prior_row["checks"][key] for key in pixel_checks})
            metrics.update(
                {
                    engine: prior_row["metrics"][engine]
                    for engine in ("pdfium", "poppler")
                }
            )
            metrics["raster_reused_from"] = str(args.reuse_run)
        with pdfium.PdfDocument(path) as pdf, pdfium.PdfDocument(normative) as ref:
            for engine, pictures in originals.items():
                if reuse:
                    continue
                ink_bounds = []
                for ink_path in (old_ink, new_ink):
                    if engine == "pdfium":
                        with pdfium.PdfDocument(ink_path) as ink:
                            painted = (
                                ink[0]
                                .render(scale=fixed["raster_dpi"] / 72)
                                .to_pil()
                                .convert("RGB")
                            )
                    else:
                        painted = render.poppler(
                            args.poppler,
                            ink_path,
                            directory / (ink_path.stem + "-paint"),
                            page=1,
                            dpi=fixed["raster_dpi"],
                        )[0]
                    ink_bounds.append(
                        ImageChops.difference(
                            painted, Image.new("RGB", painted.size, "white")
                        ).getbbox()
                    )
                outside = exact = 0
                for p, picture in enumerate(pictures):
                    if engine == "pdfium":
                        actual = (
                            pdf[p]
                            .render(scale=fixed["raster_dpi"] / 72)
                            .to_pil()
                            .convert("RGB")
                        )
                        expected_picture = (
                            ref[p]
                            .render(scale=fixed["raster_dpi"] / 72)
                            .to_pil()
                            .convert("RGB")
                        )
                    else:
                        actual = render.poppler(
                            args.poppler,
                            path,
                            directory / (name + f"-{p}"),
                            page=p + 1,
                            dpi=fixed["raster_dpi"],
                        )[0]
                        expected_picture = render.poppler(
                            args.poppler,
                            normative,
                            directory / (name + f"-reference-{p}"),
                            page=p + 1,
                            dpi=fixed["raster_dpi"],
                        )[0]
                    assert picture.size == actual.size == expected_picture.size
                    exact += int(
                        np.count_nonzero(
                            np.asarray(
                                ImageChops.difference(actual, expected_picture)
                            ).any(axis=2)
                        )
                    )
                    mask = Image.new("L", picture.size, 255)
                    if p == 0:
                        draw = ImageDraw.Draw(mask)
                        for bounds in ink_bounds:
                            if bounds:
                                x0, y0, x1, y1 = bounds
                                b = fixed["boundary_pixels"]
                                draw.rectangle(
                                    (x0 - b, y0 - b, x1 - 1 + b, y1 - 1 + b), fill=0
                                )
                    delta = ImageChops.multiply(
                        ImageChops.difference(picture, actual), mask.convert("RGB")
                    )
                    outside += int(np.count_nonzero(np.asarray(delta).any(axis=2)))
                checks[engine + "_full_reference_pixels_exact"] = exact == 0
                checks[engine + "_outside_pixels_zero"] = (
                    outside == fixed["outside_pixels"]
                )
                metrics[engine] = dict(
                    full_reference_difference_pixels=exact,
                    outside_pixels=outside,
                    ink_bounds=ink_bounds,
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
        criteria_sha256=styled.sha(base / "criteria.json"),
        profile=args.profile,
        scope="Fixed flowing profile; all glyph coordinates and full-page pixels exact to independent reference; native and general editing unexecuted",
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
