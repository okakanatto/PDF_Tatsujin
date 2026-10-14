"""Independent vector command removal and outside raster/text checks."""

import argparse
import importlib.util
import json
from pathlib import Path

from pypdf import PdfReader
from pypdf.generic import ContentStream, DecodedStreamObject, StreamObject
from test_tools import poppler_tool

spec = importlib.util.spec_from_file_location(
    "partial_checks", Path(__file__).with_name("evaluate-redaction-partial.py")
)
partial = importlib.util.module_from_spec(spec)
spec.loader.exec_module(partial)
PINS = {
    "vector-paths-v1": "027f0ab58bf461ce4f47762055244e6ddbe4e3141cfeffed5e60aa7483fbc869",
    "shared-followup-v2": "d4e635ed3ec17063fece57722f81dc84d9378919ad39835c730e8387dc848c5b",
}


def commands(stream, reader):
    return {
        (op, tuple(round(float(v), 7) for v in operands))
        for operands, op in ContentStream(stream, reader).operations
        if op in (b"m", b"l", b"c", b"v", b"y", b"re")
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--fixtures", type=Path, required=True)
    parser.add_argument("--run", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    assert not args.output.exists()
    raw = (args.fixtures / "criteria.json").read_bytes()
    pin = PINS[args.fixtures.name]
    assert partial.sha(raw) == pin
    criteria = json.loads(raw)
    poppler = poppler_tool("pdftoppm")
    results = []
    for case in criteria["cases"]:
        if case["refused"]:
            continue
        row = {"file": case["file"], "status": "FAIL"}
        try:
            source = args.fixtures / case["file"]
            target = args.run / ("path-" + case["file"])
            assert partial.sha(source.read_bytes()) == case["sha256"]
            before, after = PdfReader(source), PdfReader(target)
            text, positions = partial.text_and_boxes(target, case["keep"])
            assert case["remove"] not in text
            assert case["remove"] not in after.pages[0].extract_text()
            error = max(
                abs(a - b)
                for term, boxes in positions.items()
                for x, y in zip(boxes, case["boxes"][term])
                for a, b in zip(x, y)
            )
            assert error <= case["max_position_error_pt"]
            frozen = DecodedStreamObject()
            frozen.set_data(case["removed_path_commands"].encode())
            removed = commands(frozen, after)
            assert removed
            retained = commands(after.pages[0].get_contents(), after)
            for value in partial.objects(after):
                if isinstance(value, StreamObject) and value.get("/Subtype") == "/Form":
                    retained |= commands(value, after)
            assert removed.isdisjoint(
                retained
            ), "Original selected path construction remains"
            original_outlines, new_outlines = partial.font_characters(
                before
            ), partial.font_characters(after)
            for char, outline in new_outlines.items():
                assert outline == original_outlines[char]
            changed = [
                partial.raster_unchanged_outside(source, target, case["rect"], engine)
                for engine in (None, poppler)
            ]
            assert changed == [0, 0], changed
            row.update(
                status="PASS",
                max_position_error_pt=error,
                source_path_commands_absent=True,
                outside_changed_pixels=changed,
                surviving_outlines_exact=True,
            )
        except FileNotFoundError as error:
            row.update(status="未実行", error=str(error))
        except Exception as error:
            row["error"] = str(error)
        results.append(row)
    report = {
        "criteria_sha256": pin,
        "results": results,
        "passed": sum(r["status"] == "PASS" for r in results),
        "failed": sum(r["status"] == "FAIL" for r in results),
        "unexecuted": sum(r["status"] == "未実行" for r in results),
        "engines": ["PDFium", "pypdf", "fontTools", "Poppler"],
        "native_external_GUI": "未実行",
    }
    args.output.write_text(
        json.dumps(report, ensure_ascii=False, indent=2) + "\n", encoding="utf-8"
    )
    print(json.dumps(report, ensure_ascii=False))
    return int(report["failed"] or report["unexecuted"])


if __name__ == "__main__":
    raise SystemExit(main())
