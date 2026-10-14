"""Independent custom-CMap text, source-resource, outline and pixel inspection."""

import argparse
import importlib.util
import io
import json
import re
from pathlib import Path

from fontTools.pens.recordingPen import DecomposingRecordingPen
from fontTools.ttLib import TTFont
from pypdf import PdfReader
from pypdf.generic import StreamObject
from test_tools import poppler_tool


def module(filename, name):
    specification = importlib.util.spec_from_file_location(
        name, Path(__file__).with_name(filename)
    )
    result = importlib.util.module_from_spec(specification)
    specification.loader.exec_module(result)
    return result


partial = module("evaluate-redaction-partial.py", "partial_checks")
shared = module("evaluate-redaction-shared.py", "shared_checks")
PINS = {
    "custom-cmap-v1": "044024c5bf7ec5a52802e6ba0d0265237aefee83471147207c61a882f7d4a36f",
    "custom-cmap-followup-v1": "463ab0757312ee1ceb00605452bf6369e71f25c1acdd1b15ea8eb1fcfbcb720c",
}


def outlines(reader, source_encoding=None):
    result = {}
    for value in partial.objects(reader):
        if not isinstance(value, dict) or value.get("/Subtype") != "/Type0":
            continue
        child = value["/DescendantFonts"][0].get_object()
        program = TTFont(io.BytesIO(child["/FontDescriptor"]["/FontFile2"].get_data()))
        glyphs = program.getGlyphSet()
        mapping = child["/CIDToGIDMap"].get_data()
        unicode = value["/ToUnicode"].get_data()
        pairs = []
        for body in re.findall(rb"beginbfchar(.*?)endbfchar", unicode, re.S):
            pairs += re.findall(rb"<([0-9a-fA-F]+)>\s*<([0-9a-fA-F]+)>", body)
        for body in re.findall(rb"beginbfrange(.*?)endbfrange", unicode, re.S):
            start, end, array, scalar = re.match(
                rb"\s*<([0-9a-fA-F]+)>\s*<([0-9a-fA-F]+)>\s*(?:\[(.*?)\]|<([0-9a-fA-F]+)>)",
                body,
                re.S,
            ).groups()
            values = (
                re.findall(rb"<([0-9a-fA-F]+)>", array)
                if array is not None
                else [
                    f"{int(scalar, 16) + index:04x}".encode()
                    for index in range(int(end, 16) - int(start, 16) + 1)
                ]
            )
            assert len(values) == int(end, 16) - int(start, 16) + 1
            pairs += [
                (f"{int(start, 16) + index:04x}".encode(), cp)
                for index, cp in enumerate(values)
            ]
        for code, cp in pairs:
            character = bytes.fromhex(cp.decode()).decode("utf-16-be")
            if character == "\0":
                continue
            cid = (
                source_encoding[str(int(code, 16))]
                if source_encoding
                else int(code, 16)
            )
            gid = int.from_bytes(mapping[cid * 2 : cid * 2 + 2], "big")
            if source_encoding and not gid:
                continue  # Unused source mappings to .notdef do not survive.
            assert gid and gid < len(program.getGlyphOrder()), (code, cid, gid)
            name = program.getGlyphOrder()[gid]
            pen = DecomposingRecordingPen(glyphs)
            glyphs[name].draw(pen)
            if character in result:
                assert result[character] == pen.value, "Conflicting Unicode outlines"
            result[character] = pen.value
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--fixtures", type=Path, required=True)
    parser.add_argument("--run", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    assert not args.output.exists(), "Preserve earlier results"
    raw = (args.fixtures / "criteria.json").read_bytes()
    PIN = PINS[args.fixtures.name]
    assert partial.sha(raw) == PIN
    fixed = json.loads(raw)
    poppler = poppler_tool("pdftoppm")
    results = []
    for case in fixed["cases"]:
        if case["refused"]:
            continue
        record = {"file": case["file"], "status": "FAIL"}
        try:
            source = args.fixtures / case["file"]
            assert partial.sha(source.read_bytes()) == case["sha256"]
            target = args.run / ("path-" + case["file"])
            before, after = PdfReader(source), PdfReader(target)
            assert len(before.pages) == len(after.pages)
            maximum = 0
            for page_index, terms in enumerate(
                [case["keep"], case["second_page_terms"]]
            ):
                if not terms:
                    continue
                content, positions = shared.text_boxes(target, page_index, terms)
                assert case["remove"] not in content
                external = after.pages[page_index].extract_text()
                assert case["remove"] not in external
                for term in terms:
                    assert term in external, (term, external)
                    baseline = case["boxes"][str(page_index)][term]
                    assert len(baseline) == len(positions[term])
                    maximum = max(
                        maximum,
                        max(
                            abs(a - b)
                            for left, right in zip(baseline, positions[term])
                            for a, b in zip(left, right)
                        ),
                    )
                for key in ("/MediaBox", "/CropBox", "/Rotate", "/UserUnit"):
                    assert before.pages[page_index].get(key) == after.pages[
                        page_index
                    ].get(key)
            assert maximum <= case["max_position_error_pt"], maximum
            original = {partial.sha(before.pages[0].get_contents().get_data())}
            for value in partial.objects(before):
                if isinstance(value, dict):
                    for key in (
                        "/ToUnicode",
                        "/FontFile2",
                        "/CIDToGIDMap",
                        "/Encoding",
                    ):
                        obj = value.get(key)
                        if obj is not None and isinstance(
                            obj.get_object(), StreamObject
                        ):
                            original.add(partial.sha(obj.get_object().get_data()))
            for value in partial.objects(after):
                if isinstance(value, StreamObject):
                    assert (
                        partial.sha(value.get_data()) not in original
                    ), "Source stream remains"
                if isinstance(value, dict) and value.get("/Subtype") == "/Type0":
                    assert value["/Encoding"] == "/Identity-H"
            source_outlines = outlines(before, case["source_encoding"])
            kept_outlines = outlines(after)
            assert kept_outlines
            for character, outline in kept_outlines.items():
                assert outline == source_outlines[character], (
                    character,
                    "changed outline",
                )
            for character in case["remove"]:
                assert character not in kept_outlines, (
                    character,
                    "removed Unicode retained",
                )
            changed = [
                partial.raster_unchanged_outside(source, target, case["rect"], engine)
                for engine in (None, poppler)
            ]
            assert changed == [0, 0], changed
            other = []
            if len(after.pages) > 1:
                other = [
                    shared.untouched_page(source, target, engine)
                    for engine in (None, poppler)
                ]
                assert other == [0, 0], other
            record.update(
                status="PASS",
                max_position_error_pt=maximum,
                outside_changed_pixels=changed,
                second_page_changed_pixels=other,
                source_fonts_encoding_maps_content_absent=True,
                surviving_outlines_exact=True,
                removed_unicode_absent=True,
            )
        except FileNotFoundError as error:
            record.update(status="未実行", error=str(error))
        except Exception as error:
            record["error"] = str(error)
        results.append(record)
    report = {
        "criteria_sha256": PIN,
        "results": results,
        "passed": sum(row["status"] == "PASS" for row in results),
        "failed": sum(row["status"] == "FAIL" for row in results),
        "unexecuted": sum(row["status"] == "未実行" for row in results),
        "engines": ["PDFium", "pypdf", "fontTools", "Poppler"],
        "native_GUI": "未実行",
    }
    args.output.write_text(
        json.dumps(report, ensure_ascii=False, indent=2) + "\n", encoding="utf-8"
    )
    print(json.dumps(report, ensure_ascii=False))
    return int(report["failed"] or report["unexecuted"])


if __name__ == "__main__":
    raise SystemExit(main())
