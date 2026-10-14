"""Independently inspect sanitized font headers and exact surviving rendering."""

import argparse
import importlib.util
import io
import json
import struct
from pathlib import Path

from fontTools.ttLib import TTFont
from pypdf import PdfReader
from pypdf.generic import StreamObject
from test_tools import poppler_tool

spec = importlib.util.spec_from_file_location(
    "partial_checks", Path(__file__).with_name("evaluate-redaction-partial.py")
)
partial = importlib.util.module_from_spec(spec)
spec.loader.exec_module(partial)
PINS = {
    "font-tables-v1": "7a8d6e747eba9816486ec202aac858acc1bdbf686c9c0931808fc9f725981cdd",
    "empty-glyph-v1": "229109449cedde155678515bd2a6bb628d0602203541289e0e40b271f1ccf5d1",
}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--fixtures", required=True, type=Path)
    parser.add_argument("--run", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--probe-layout", action="store_true")
    args = parser.parse_args()
    if args.output.exists():
        raise FileExistsError("Preserve prior independent report")
    criteria = (args.fixtures / "criteria.json").read_bytes()
    pin = PINS[args.fixtures.name]
    assert partial.sha(criteria) == pin
    results = []
    engine = poppler_tool("pdftoppm")
    refused = []
    for case in json.loads(criteria)["cases"]:
        if case["refused"]:
            assert not (args.run / ("path-" + case["file"])).exists()
            refused.append(case["file"])
            continue
        row = dict(file=case["file"], status="FAIL")
        try:
            source = args.fixtures / case["file"]
            target = args.run / (
                Path(case["file"]).stem + "/candidate.pdf"
                if args.probe_layout
                else "path-" + case["file"]
            )
            assert partial.sha(source.read_bytes()) == case["sha256"]
            before, after = PdfReader(source), PdfReader(target)
            contents, boxes = partial.text_and_boxes(target, case["keep"])
            assert case["remove"] not in contents
            assert case["remove"] not in after.pages[0].extract_text()
            assert all(term in after.pages[0].extract_text() for term in case["keep"])
            error = max(
                abs(a - b)
                for term, values in boxes.items()
                for actual, expected in zip(values, case["boxes"][term])
                for a, b in zip(actual, expected)
            )
            assert all(
                len(values) == len(case["boxes"][term])
                for term, values in boxes.items()
            )
            assert error <= case["max_position_error_pt"]
            old = partial.font_characters(before)
            new = partial.font_characters(after)
            assert new and all(
                old[character] == outline for character, outline in new.items()
            )
            assert not (set(case["remove"]) & set(new))
            programs = 0
            source_streams = {
                partial.sha(value.get_data())
                for value in partial.objects(before)
                if isinstance(value, StreamObject)
            }
            for value in partial.objects(after):
                if isinstance(value, StreamObject):
                    data = value.get_data()
                    assert all(
                        marker.encode() not in data
                        for marker in case["private_payloads"]
                    )
                    assert partial.sha(data) not in source_streams
                if isinstance(value, dict) and "/FontFile2" in value:
                    data = value["/FontFile2"].get_data()
                    tables = {}
                    for index in range(struct.unpack_from(">H", data, 4)[0]):
                        tag, _, offset, length = struct.unpack_from(
                            ">4sIII", data, 12 + 16 * index
                        )
                        tables[tag] = data[offset : offset + length]
                    font = TTFont(
                        io.BytesIO(data), recalcBBoxes=False, recalcTimestamp=False
                    )
                    assert font["head"].fontRevision == 1.0
                    # fontTools normalizes low timestamp values on decompile.
                    # Inspect the actual bytes to verify zero original dates.
                    assert tables[b"head"][20:36] == b"\0" * 16
                    assert all(
                        getattr(font["hhea"], "reserved" + str(i)) == 0
                        for i in range(4)
                    )
                    assert all(
                        getattr(font["maxp"], key) == 0
                        for key in (
                            "maxTwilightPoints",
                            "maxStorage",
                            "maxFunctionDefs",
                            "maxInstructionDefs",
                            "maxSizeOfInstructions",
                        )
                    )
                    # The retained closed prep profile consumes one stack slot.
                    # Derive the required resource from executable bytes rather
                    # than copying the original private maxp summary.
                    prep = tables.get(b"prep", b"")
                    assert prep in (b"", bytes.fromhex("b801ff85b0048d"))
                    assert font["maxp"].maxStackElements == int(bool(prep))
                    assert font["maxp"].maxZones == 1
                    assert (
                        font["hhea"].numberOfHMetrics
                        == font["maxp"].numGlyphs
                        == len(font.getGlyphOrder())
                    )
                    expected = font.getTableData("maxp")
                    expected_bounds = struct.unpack_from(">hhhh", tables[b"head"], 36)
                    font["maxp"].recalc(font)
                    assert (
                        font.getTableData("maxp") == expected
                    ), "maxp differs from independent fontTools recount"
                    assert (
                        tuple(
                            getattr(font["head"], key)
                            for key in ("xMin", "yMin", "xMax", "yMax")
                        )
                        == expected_bounds
                    )
                    font["hhea"].recalc(font)
                    # Header statistics must equal the independently recomputed glyph set.
                    raw = font.getTableData("hhea")
                    assert raw[10:18] == tables[b"hhea"][10:18]
                    programs += 1
            assert programs > 0
            pixels = [
                partial.raster_unchanged_outside(source, target, case["rect"], tool)
                for tool in (None, engine)
            ]
            assert pixels == [0, 0]
            row.update(
                status="PASS",
                private_payloads_absent=True,
                independently_recounted_headers=True,
                outside_changed_pixels=pixels,
                max_position_error_pt=error,
                surviving_outlines_exact=True,
                sha256=partial.sha(target.read_bytes()),
            )
        except FileNotFoundError as error:
            row.update(status="未実行", error=str(error))
        except Exception as error:
            row["error"] = str(error)
        results.append(row)
    report = dict(
        criteria_sha256=pin,
        producer="diagnostic probe" if args.probe_layout else "application selftest",
        results=results,
        refused_outputs_absent=refused,
        passed=sum(row["status"] == "PASS" for row in results),
        failed=sum(row["status"] == "FAIL" for row in results),
        unexecuted=sum(row["status"] == "未実行" for row in results),
        native_external_GUI="未実行",
    )
    args.output.write_text(
        json.dumps(report, ensure_ascii=False, indent=2) + "\n", encoding="utf-8"
    )
    print(json.dumps(report, ensure_ascii=False))
    return int(report["failed"] != 0 or report["unexecuted"] != 0)


if __name__ == "__main__":
    raise SystemExit(main())
