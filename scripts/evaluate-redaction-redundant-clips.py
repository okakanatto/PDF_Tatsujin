"""Independently inspect redundant clip removal and unchanged visible content."""

import argparse
import importlib.util
import json
from pathlib import Path

from pypdf import PdfReader
from pypdf.generic import ContentStream, DecodedStreamObject, StreamObject
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
paths = module("evaluate-redaction-paths.py", "path_checks")
custom = module("evaluate-redaction-custom-cmap.py", "custom_checks")
PINS = {
    "redundant-clips-v1": "e3d31bbc74fdf0c28830fb746b1b6d29e4c058eca08f0d8940766cf401bd0c46",
    "redundant-clips-followup-v1": "81489b5b301af21191ab4103e3425a417fe2da379232e47faf56d48b848d8bc0",
}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--fixtures", type=Path, required=True)
    parser.add_argument("--run", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--output-prefix", choices=("clip", "path"), default="clip")
    args = parser.parse_args()
    assert not args.output.exists()
    raw = (args.fixtures / "criteria.json").read_bytes()
    PIN = PINS[args.fixtures.name]
    assert partial.sha(raw) == PIN
    fixed = json.loads(raw)
    engine = poppler_tool("pdftoppm")
    results = []
    for case in fixed["cases"]:
        if case["refused"]:
            continue
        row = {"file": case["file"], "status": "FAIL"}
        try:
            source = args.fixtures / case["file"]
            target = args.run / (args.output_prefix + "-" + case["file"])
            assert partial.sha(source.read_bytes()) == case["sha256"]
            before, after = PdfReader(source), PdfReader(target)
            assert len(before.pages) == len(after.pages)
            error = 0
            for page_index, terms in enumerate(
                [case["keep"], case["second_page_terms"]]
            ):
                if not terms:
                    continue
                text, boxes = shared.text_boxes(target, page_index, terms)
                external = after.pages[page_index].extract_text()
                if page_index == 0:
                    assert case["remove"] not in text and case["remove"] not in external
                for term, values in boxes.items():
                    assert term in external
                    reference = case["boxes"][str(page_index)][term]
                    assert len(values) == len(reference)
                    error = max(
                        error,
                        max(
                            abs(a - b)
                            for left, right in zip(values, reference)
                            for a, b in zip(left, right)
                        ),
                    )
                for key in ("/MediaBox", "/CropBox", "/Rotate", "/UserUnit"):
                    assert before.pages[page_index].get(key) == after.pages[
                        page_index
                    ].get(key)
            assert error <= case["max_position_error_pt"]
            source_streams = {partial.sha(before.pages[0].get_contents().get_data())}
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
                            source_streams.add(partial.sha(obj.get_object().get_data()))
            for value in partial.objects(after):
                if isinstance(value, StreamObject):
                    assert partial.sha(value.get_data()) not in source_streams
            clipped = DecodedStreamObject()
            clipped.set_data(case["removed_clip_commands"].encode())
            removed = paths.commands(clipped, after)
            assert removed
            selected_streams = [after.pages[0].get_contents()]
            for value in partial.objects(after):
                if isinstance(value, StreamObject) and value.get("/Subtype") == "/Form":
                    selected_streams.append(value)
            retained = set()
            for content in selected_streams:
                operations = ContentStream(content, after).operations
                assert all(op not in (b"W", b"W*") for operands, op in operations)
                retained |= paths.commands(content, after)
            assert removed.isdisjoint(
                retained
            ), "Redundant source clip coordinates remain"
            if case.get("source_encoding"):
                old_outlines = custom.outlines(before, case["source_encoding"])
                new_outlines = custom.outlines(after)
            else:
                old_outlines = partial.font_characters(before)
                new_outlines = partial.font_characters(after)
            assert new_outlines
            for character, outline in new_outlines.items():
                assert old_outlines[character] == outline
            changed = [
                partial.raster_unchanged_outside(source, target, case["rect"], renderer)
                for renderer in (None, engine)
            ]
            assert changed == [0, 0], changed
            second = []
            if len(after.pages) == 2:
                second = [
                    shared.untouched_page(source, target, renderer)
                    for renderer in (None, engine)
                ]
                assert second == [0, 0]
                assert b"W" in after.pages[1].get_contents().get_data()
            row.update(
                status="PASS",
                max_position_error_pt=error,
                outside_changed_pixels=changed,
                second_page_changed_pixels=second,
                redundant_clip_commands_absent=True,
                source_fonts_maps_content_absent=True,
                surviving_outlines_exact=True,
            )
        except FileNotFoundError as error:
            row.update(status="未実行", error=str(error))
        except Exception as error:
            row["error"] = str(error)
        results.append(row)
    report = {
        "criteria_sha256": PIN,
        "results": results,
        "passed": sum(row["status"] == "PASS" for row in results),
        "failed": sum(row["status"] == "FAIL" for row in results),
        "unexecuted": sum(row["status"] == "未実行" for row in results),
        "native_external_GUI": "未実行",
    }
    args.output.write_text(
        json.dumps(report, ensure_ascii=False, indent=2) + "\n", encoding="utf-8"
    )
    print(json.dumps(report, ensure_ascii=False))
    return int(report["failed"] or report["unexecuted"])


if __name__ == "__main__":
    raise SystemExit(main())
