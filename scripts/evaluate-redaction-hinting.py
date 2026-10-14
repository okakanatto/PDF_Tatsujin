"""Independent hinted-font audit and source-based PDFium/Poppler pixel checks."""

import argparse
import ctypes
import importlib.util
import io
import json
import math
import re
import subprocess
import traceback
from collections import Counter, defaultdict
from pathlib import Path

import numpy as np
import pypdfium2 as pdfium
import pypdfium2.raw as raw
from fontTools.ttLib import TTFont
from fontTools.pens.recordingPen import DecomposingRecordingPen
from PIL import Image
from pypdf import PdfReader
from pypdf.generic import ContentStream, IndirectObject, StreamObject
from test_tools import poppler_tool

spec = importlib.util.spec_from_file_location(
    "partial", Path(__file__).with_name("evaluate-redaction-partial.py")
)
partial = importlib.util.module_from_spec(spec)
spec.loader.exec_module(partial)
PIN = "9704e6be7527959c40f97e3e1cb097798777f9edb17d855ea17430f30631ff27"
PREP = bytes.fromhex("b801ff85b0048d")


def all_objects(reader):
    """Inspect ordinary and compressed xref entries, including unused objects."""
    entries = {
        (number, generation)
        for generation, numbers in reader.xref.items()
        for number in numbers
        if number
    }
    entries.update((number, 0) for number in reader.xref_objStm)
    for number, generation in sorted(entries):
        yield reader.get_object(IndirectObject(number, generation, reader))


def validate_producer(run, names):
    process = json.loads((run / "process-result.json").read_bytes())
    assert (
        process["completed"] and process["report_valid"] and process["exit_code"] == 0
    )
    executable = Path(process["executable"])
    assert partial.sha(executable.read_bytes()) == process["exe_sha256"]
    tests = json.loads((run / "selftest.json").read_bytes())["tests"]
    matches = {row["name"]: row for row in tests if row["name"] in names}
    assert set(matches) == set(names) and all(
        row["status"] == "PASS" for row in matches.values()
    )
    assert process["test_count"] == len(tests)
    assert all(row["status"] == "PASS" for row in tests)
    return process, matches


def validate_case_outcomes(details, cases):
    expected = {row["file"]: row["refused"] for row in cases}
    actual = {row["file"]: row["refused"] for row in details["cases"]}
    assert len(actual) == len(details["cases"]) and actual == expected
    assert details["refused"] == sum(expected.values())


def object_key(value):
    ref = (
        value
        if isinstance(value, IndirectObject)
        else getattr(value, "indirect_reference", None)
    )
    return (ref.idnum, ref.generation) if ref else ("direct", id(value))


def font_record(value):
    font = value.get_object()
    if "/ToUnicode" not in font:
        return None
    child = font["/DescendantFonts"][0] if "/DescendantFonts" in font else font
    child = child.get_object()
    descriptor = child.get("/FontDescriptor")
    if descriptor is None or "/FontFile2" not in descriptor.get_object():
        return None
    data = descriptor.get_object()["/FontFile2"].get_data()
    sfnt = TTFont(io.BytesIO(data))
    mappings = {}
    cmap = sfnt["cmap"].tables[0].cmap
    gid_map = child.get("/CIDToGIDMap")
    for body in re.findall(
        rb"beginbfchar(.*?)endbfchar", font["/ToUnicode"].get_data(), re.S
    ):
        for code, cp in re.findall(rb"<([0-9a-fA-F]+)>\s*<([0-9a-fA-F]+)>", body):
            cid = int(code, 16)
            character = bytes.fromhex(cp.decode()).decode("utf-16-be")
            assert len(character) == 1, "Closed single-Unicode fixture scope"
            if "/DescendantFonts" in font:
                gid = (
                    cid
                    if gid_map is None or str(gid_map) == "/Identity"
                    else int.from_bytes(
                        gid_map.get_object().get_data()[cid * 2 : cid * 2 + 2], "big"
                    )
                )
            else:
                name = cmap.get(cid)
                if name is None:
                    continue
                gid = sfnt.getGlyphID(name)
            assert cid not in mappings and gid < len(sfnt.getGlyphOrder())
            mappings[cid] = (character, gid)
    return dict(
        identity=object_key(value),
        data=data,
        sfnt=sfnt,
        mappings=mappings,
        code_bytes=2 if "/DescendantFonts" in font else 1,
        used_glyphs=[],
    )


def resource_inventory(reader):
    """Retain each page/Form/AP binding and actual text use independently."""
    bindings, usage = {}, defaultdict(str)

    def decode(value, record):
        encoded = value.original_bytes
        size = record["code_bytes"]
        assert len(encoded) % size == 0
        codes = [
            int.from_bytes(encoded[i : i + size], "big")
            for i in range(0, len(encoded), size)
        ]
        assert all(code in record["mappings"] for code in codes)
        record["used_glyphs"].extend(record["mappings"][code] for code in codes)
        return "".join(record["mappings"][code][0] for code in codes)

    def inspect(stream, resources, path, active):
        resources = resources.get_object()
        local = {}
        for alias, value in resources.get("/Font", {}).items():
            record = font_record(value)
            if record:
                key = path + "/font" + str(alias)
                bindings[key] = record
                local[str(alias)] = key
        current, stack, form_index = None, [], 0
        for args, operator in ContentStream(stream, reader).operations:
            if operator == b"q":
                stack.append(current)
            elif operator == b"Q":
                assert stack, path
                current = stack.pop()
            elif operator == b"Tf":
                current = local.get(str(args[0]))
            elif operator in (b"Tj", b"TJ", b"'", b'"') and current:
                strings = args[0] if operator == b"TJ" else [args[-1]]
                for value in strings:
                    if hasattr(value, "original_bytes"):
                        usage[current] += decode(value, bindings[current])
            elif operator == b"Do":
                alias = str(args[0])
                child = resources["/XObject"][args[0]].get_object()
                if child.get("/Subtype") != "/Form":
                    continue
                identity = object_key(child)
                assert identity not in active, "Cyclic Form in accepted audit input"
                inspect(
                    child,
                    child.get("/Resources", resources),
                    path + "/form/" + str(form_index),
                    active | {identity},
                )
                form_index += 1

    def appearance(value, resources, path, active):
        value = value.get_object()
        if isinstance(value, StreamObject):
            inspect(value, value.get("/Resources", resources), path, active)
        else:
            for state, child in value.items():
                appearance(child, resources, path + str(state), active)

    for index, page in enumerate(reader.pages):
        resources = page["/Resources"]
        inspect(page.get_contents(), resources, f"page/{index}", set())
        for number, annotation in enumerate(page.get("/Annots", [])):
            for kind, value in annotation.get_object().get("/AP", {}).items():
                appearance(value, resources, f"page/{index}/AP/{number}{kind}", set())
    return bindings, usage


def glyph_signature(record, gid):
    font = record["sfnt"]
    name = font.getGlyphOrder()[gid]
    glyph = font["glyf"][name]
    pen = DecomposingRecordingPen(font.getGlyphSet())
    font.getGlyphSet()[name].draw(pen)
    coordinates, endpoints, flags = glyph.getCoordinates(font["glyf"])
    return (
        repr(pen.value),
        bytes(glyph.program.getBytecode()) if hasattr(glyph, "program") else b"",
        font["hmtx"][name],
        tuple(tuple(point) for point in coordinates),
        tuple(endpoints),
        bytes(flags),
    )


def glyph_closure(record, roots):
    font, active, complete = record["sfnt"], set(), set()

    def visit(gid):
        assert gid not in active, "Cyclic glyph in independent audit"
        if gid in complete:
            return
        active.add(gid)
        glyph = font["glyf"][font.getGlyphOrder()[gid]]
        if glyph.isComposite():
            for component in glyph.components:
                visit(font.getGlyphID(component.glyphName))
        active.remove(gid)
        complete.add(gid)

    for gid in roots:
        visit(gid)
    return complete


def audit_resource_glyphs(before, after, case):
    source, source_usage = resource_inventory(before)
    target, target_usage = resource_inventory(after)
    expected = {
        key: text.replace(case["remove"], "") if key.startswith("page/0/") else text
        for key, text in source_usage.items()
    }
    expected = {key: text for key, text in expected.items() if text}
    assert set(target_usage) == set(expected), "Embedded-font use bindings changed"
    groups = defaultdict(list)
    expected_uses = {}
    inspected = 0
    for key, actual in target_usage.items():
        assert Counter(actual) == Counter(expected[key]), ("Font usage changed", key)
        original, retained = source[key], target[key]
        deleted = set()
        if key.startswith("page/0/"):
            for match in re.finditer(re.escape(case["remove"]), source_usage[key]):
                deleted.update(range(match.start(), match.end()))
        expected_uses[key] = [
            entry
            for index, entry in enumerate(original["used_glyphs"])
            if index not in deleted
        ]
        assert Counter(
            (character, glyph_signature(original, gid))
            for character, gid in expected_uses[key]
        ) == Counter(
            (character, glyph_signature(retained, gid))
            for character, gid in retained["used_glyphs"]
        ), (
            "Resource/GID text-use instructions or outlines changed",
            key,
        )
        original_characters = defaultdict(set)
        for character, gid in original["mappings"].values():
            original_characters[character].add(gid)
        for character, gid in retained["mappings"].values():
            assert character in original_characters, (
                "New font-map character",
                key,
                character,
            )
            assert any(
                glyph_signature(retained, gid) == glyph_signature(original, source_gid)
                for source_gid in original_characters[character]
            ), (
                "Resource glyph outline, metrics or instructions changed",
                key,
                character,
                gid,
            )
            inspected += 1
        groups[retained["identity"]].append(key)
    known_payloads = set()
    for identity, keys in groups.items():
        retained = target[keys[0]]
        originals = [source[key] for key in keys]
        assert len({partial.sha(record["data"]) for record in originals}) == 1, (
            "Unrelated source fonts merged",
            keys,
        )
        original = originals[0]
        allowed = set("".join(expected[key] for key in keys))
        mapped = {character for character, _ in retained["mappings"].values()}
        assert mapped == allowed, (
            "Unused or missing mapped glyph",
            keys,
            mapped,
            allowed,
        )
        source_roots = {gid for key in keys for _, gid in expected_uses[key]}
        target_roots = {gid for _, gid in retained["mappings"].values()}
        old_closure, new_closure = glyph_closure(original, source_roots), glyph_closure(
            retained, target_roots
        )
        assert set(range(len(retained["sfnt"].getGlyphOrder()))) == new_closure | {0}, (
            "Unmapped non-component glyph remains",
            keys,
        )
        if 0 not in new_closure:
            notdef = retained["sfnt"]["glyf"][retained["sfnt"].getGlyphOrder()[0]]
            assert notdef.numberOfContours == 0
            assert not hasattr(notdef, "program") or not notdef.program.getBytecode()
        assert Counter(
            glyph_signature(original, gid) for gid in old_closure
        ) == Counter(glyph_signature(retained, gid) for gid in new_closure), (
            "Retained glyph/component closure differs",
            keys,
        )
        known_payloads.add(partial.sha(retained["data"]))
    descriptors = {
        partial.sha(value["/FontFile2"].get_data())
        for value in all_objects(after)
        if isinstance(value, dict) and "/FontFile2" in value
    }
    assert descriptors == known_payloads, "Unbound embedded font data remains"
    return dict(
        resource_bindings=len(target_usage),
        audited_map_entries=inspected,
        font_closures=len(groups),
        retained_point_topology_exact=True,
    )


def audit_private_streams(before, after, case, target):
    original = {partial.sha(before.pages[0].get_contents().get_data())}
    for value in all_objects(before):
        if isinstance(value, dict):
            for key in ("/ToUnicode", "/FontFile2"):
                if key in value:
                    original.add(partial.sha(value[key].get_data()))
    for value in all_objects(after):
        if isinstance(value, StreamObject):
            data = value.get_data()
            assert (
                partial.sha(data) not in original
            ), "Original font, map or target stream remains"
            if case.get("private_marker"):
                assert case["private_marker"].encode() not in data
    if case.get("private_marker"):
        assert case["private_marker"].encode() not in target.read_bytes()


def check_fonts(reader):
    count = hinted = 0
    for value in all_objects(reader):
        if not isinstance(value, dict) or "/FontFile2" not in value:
            continue
        data = value["/FontFile2"].get_data()
        font = TTFont(io.BytesIO(data))
        assert set(font.keys()) <= {
            "GlyphOrder",
            "head",
            "hhea",
            "maxp",
            "hmtx",
            "loca",
            "glyf",
            "cmap",
            "name",
            "post",
            "prep",
        }
        assert "prep" not in font or font.getTableData("prep") == PREP
        instructions = [
            bytes(font["glyf"][name].program.getBytecode())
            for name in font.getGlyphOrder()
            if hasattr(font["glyf"][name], "program")
        ]
        longest = max(map(len, instructions), default=0)
        assert font["maxp"].maxStackElements == int(bool(longest or "prep" in font))
        assert font["maxp"].maxSizeOfInstructions == longest
        assert font["maxp"].maxZones == 1
        assert all(
            getattr(font["maxp"], field) == 0
            for field in (
                "maxTwilightPoints",
                "maxStorage",
                "maxFunctionDefs",
                "maxInstructionDefs",
            )
        )
        expected = font.getTableData("maxp")
        font["maxp"].recalc(font)
        assert font.getTableData("maxp") == expected
        hinted += sum(bool(value) for value in instructions)
        count += 1
    assert count and hinted
    return dict(fonts=count, hinted_glyphs=hinted)


def pixels(source, target, dpi, page_index, rectangle, engine):
    with pdfium.PdfDocument(source) as before, pdfium.PdfDocument(target) as after:
        page = before[page_index]
        if engine:

            def render(path):
                run = subprocess.run(
                    [
                        str(engine),
                        "-cropbox",
                        "-singlefile",
                        "-png",
                        "-r",
                        str(dpi),
                        "-f",
                        str(page_index + 1),
                        "-l",
                        str(page_index + 1),
                        str(path),
                    ],
                    capture_output=True,
                    timeout=60,
                    check=True,
                )
                return np.array(Image.open(io.BytesIO(run.stdout)).convert("RGB"))

            a, b = render(source), render(target)
        else:
            a = np.array(page.render(scale=dpi / 72).to_pil().convert("RGB"))
            b = np.array(
                after[page_index].render(scale=dpi / 72).to_pil().convert("RGB")
            )
        assert a.shape == b.shape
        changed = np.any(a != b, axis=2)
        if rectangle:
            x, y, width, height = rectangle
            h, w, _ = a.shape
            corners = []
            for px, py in (
                (x, y),
                (x + width, y),
                (x, y + height),
                (x + width, y + height),
            ):
                out_x, out_y = ctypes.c_int(), ctypes.c_int()
                assert raw.FPDF_PageToDevice(
                    page.raw,
                    0,
                    0,
                    w * 200,
                    h * 200,
                    0,
                    px,
                    py,
                    ctypes.byref(out_x),
                    ctypes.byref(out_y),
                )
                corners.append((out_x.value / 200, out_y.value / 200))
            x0 = max(0, math.floor(min(p[0] for p in corners)) - 1)
            x1 = min(w, math.ceil(max(p[0] for p in corners)) + 1)
            y0 = max(0, math.floor(min(p[1] for p in corners)) - 1)
            y1 = min(h, math.ceil(max(p[1] for p in corners)) + 1)
            changed[y0:y1, x0:x1] = False
        return int(np.count_nonzero(changed))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--fixtures", type=Path, required=True)
    parser.add_argument("--run", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    assert not args.output.exists(), "Preserve prior independent reports"
    criteria = (args.fixtures / "criteria.json").read_bytes()
    assert partial.sha(criteria) == PIN
    fixed = json.loads(criteria)
    global_bytes = (
        args.fixtures.parent / "hinting-global-v1/criteria.json"
    ).read_bytes()
    assert (
        partial.sha(global_bytes)
        == "107fd6c0880449634310db823e4afe69e9937b0557ce28c321db8d94ef91170d"
    )
    process, producer_tests = validate_producer(
        args.run, {"M5RH01_hinting", "M5RH02_hinting_UI"}
    )
    validate_case_outcomes(
        producer_tests["M5RH01_hinting"]["details"]["main"], fixed["cases"]
    )
    validate_case_outcomes(
        producer_tests["M5RH01_hinting"]["details"]["global_only"],
        json.loads(global_bytes)["cases"],
    )
    engine = poppler_tool("pdftoppm")
    results = []
    for case in fixed["cases"]:
        row = dict(file=case["file"], status="FAIL")
        try:
            source = args.fixtures / case["file"]
            assert partial.sha(source.read_bytes()) == case["sha256"]
            target = args.run / ("hinting-" + case["file"])
            if case["refused"]:
                assert not target.exists()
                row.update(status="PASS", rejected_output_absent=True)
                results.append(row)
                continue
            before, after = PdfReader(source), PdfReader(target)
            assert len(after.pages) == len(before.pages)
            maximum = 0
            for index in range(len(before.pages)):
                terms = case["keep"] if index == 0 else case["second_page_terms"]
                with pdfium.PdfDocument(target) as document:
                    page = document[index]
                    text = page.get_textpage()
                    content = text.get_text_range()
                    assert case["remove"] not in content
                    assert case["remove"] not in after.pages[index].extract_text()
                    for term in terms:
                        assert (
                            content.count(term) == 1
                            and term in after.pages[index].extract_text()
                        )
                        start = content.index(term)
                        positions = [
                            list(text.get_charbox(i))
                            for i in range(start, start + len(term))
                        ]
                        boxes = (
                            case["boxes"]
                            if len(before.pages) == 1
                            else case["boxes"][str(index)]
                        )
                        assert len(positions) == len(boxes[term])
                        maximum = max(
                            maximum,
                            max(
                                abs(a - b)
                                for actual, expected in zip(positions, boxes[term])
                                for a, b in zip(actual, expected)
                            ),
                        )
            assert maximum <= fixed["position_error_pt"]
            resource_audit = audit_resource_glyphs(before, after, case)
            audit_private_streams(before, after, case, target)
            raster = {}
            for dpi in fixed["dpi"]:
                for label, renderer in (("PDFium", None), ("Poppler", engine)):
                    differences = [
                        pixels(
                            source,
                            target,
                            dpi,
                            index,
                            case["rect"] if index == 0 else None,
                            renderer,
                        )
                        for index in range(len(before.pages))
                    ]
                    assert all(value == 0 for value in differences), (
                        dpi,
                        label,
                        differences,
                    )
                    raster[f"{label}-{dpi}"] = differences
            observable = {}
            if case["file"] == "single.pdf":
                base = Path(__file__).resolve().parents[1] / case["source_file"]
                assert partial.sha(base.read_bytes()) == case["source_sha256"]
                for dpi in (72, 96):
                    for label, renderer in (("PDFium", None), ("Poppler", engine)):
                        observable[f"{label}-{dpi}"] = pixels(
                            source, base, dpi, 0, None, renderer
                        )
                assert any(
                    observable.values()
                ), "Fixture hints have no observable rendering effect"
            row.update(
                status="PASS",
                max_position_error_pt=maximum,
                raster=raster,
                hinting_observable_changed_pixels=observable,
                original_font_map_content_absent=True,
                kept_outline_and_instructions_exact=True,
                **resource_audit,
                **check_fonts(after),
            )
        except Exception as error:
            row["error"] = repr(error)
            row["traceback"] = traceback.format_exc()
        results.append(row)
    for case in json.loads(global_bytes)["cases"]:
        source = args.fixtures.parent / "hinting-global-v1" / case["file"]
        assert partial.sha(source.read_bytes()) == case["sha256"] and case["refused"]
        absent = not (args.run / ("hinting-global-" + case["file"])).exists()
        results.append(
            dict(
                file="global/" + case["file"],
                status="PASS" if absent else "FAIL",
                rejected_output_absent=absent,
            )
        )
    ui_source = args.run / "hinting-ui-before.pdf"
    assert ui_source.exists(), "Producer UI baseline missing"
    if ui_source.exists():
        ui_target = args.run / "hinting-ui.pdf"
        case = next(row for row in fixed["cases"] if row["file"] == "shared-pages.pdf")
        ui_pixels = {}
        for dpi in fixed["dpi"]:
            for label, renderer in (("PDFium", None), ("Poppler", engine)):
                differences = [
                    pixels(
                        ui_source,
                        ui_target,
                        dpi,
                        page,
                        case["rect"] if page == 0 else None,
                        renderer,
                    )
                    for page in (0, 1)
                ]
                assert differences == [0, 0], (label, dpi, differences)
                ui_pixels[f"{label}-{dpi}"] = differences
        results.append(
            dict(
                file="hinting-ui.pdf",
                status="PASS",
                raster=ui_pixels,
                unsaved_signature_pixels_preserved=True,
            )
        )
    report = dict(
        criteria_sha256=PIN,
        evaluator_sha256=partial.sha(Path(__file__).read_bytes()),
        resource_audit_schema=2,
        producer_executable_sha256=process["exe_sha256"],
        producer_completed=True,
        results=results,
        passed=sum(row["status"] == "PASS" for row in results),
        failed=sum(row["status"] != "PASS" for row in results),
        engines=["PDFium", "Poppler", "pypdf", "fontTools"],
        general_hinting="unexecuted",
        native_external_GUI="unexecuted",
    )
    args.output.write_text(
        json.dumps(report, ensure_ascii=False, indent=2) + "\n", "utf-8"
    )
    print(json.dumps(report, ensure_ascii=True))
    return int(report["failed"] > 0)


if __name__ == "__main__":
    raise SystemExit(main())
