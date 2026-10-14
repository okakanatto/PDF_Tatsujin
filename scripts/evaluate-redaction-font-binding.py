"""Independently check font-state binding without retiring unrelated fonts."""

import argparse
import importlib.util
import json
from pathlib import Path

from pypdf import PdfReader
from test_tools import poppler_tool

spec = importlib.util.spec_from_file_location(
    "partial", Path(__file__).with_name("evaluate-redaction-partial.py")
)
partial = importlib.util.module_from_spec(spec)
spec.loader.exec_module(partial)
PIN = "b167c53f03b5939aae9206d8bf0c38b205790413698e87c93462732b4c07f62b"
PINS = {
    "font-binding-v1": PIN,
    "font-binding-followup-v1": "7f585559474ea31e06f94c1b1f18c40a575e0b11e7e9dd5741d473d3bc3c9720",
}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--fixtures", type=Path, required=True)
    parser.add_argument("--run", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--probe-layout", action="store_true")
    parser.add_argument("--inspect-refused-candidates", action="store_true")
    args = parser.parse_args()
    assert not args.output.exists()
    raw = (args.fixtures / "criteria.json").read_bytes()
    pin = PINS[args.fixtures.name]
    assert partial.sha(raw) == pin
    poppler = poppler_tool("pdftoppm")
    rows = []
    for case in json.loads(raw)["cases"]:
        target = (
            args.run / Path(case["file"]).stem / "candidate.pdf"
            if args.probe_layout
            else args.run / ("binding-" + case["file"])
        )
        if case["refused"] and not (
            args.inspect_refused_candidates and target.exists()
        ):
            continue
        record = dict(
            file=case["file"], status="FAIL", expected_refusal=case["refused"]
        )
        try:
            source = args.fixtures / case["file"]
            assert partial.sha(source.read_bytes()) == case["sha256"]
            content, boxes = partial.text_and_boxes(target, case["keep"])
            assert case["remove"] not in content
            extracted = PdfReader(target).pages[0].extract_text()
            assert case["remove"] not in extracted
            assert all(term in extracted for term in case["keep"])
            maximum = max(
                abs(a - b)
                for term in case["keep"]
                for old, new in zip(case["boxes"][term], boxes[term], strict=True)
                for a, b in zip(old, new, strict=True)
            )
            changed = [
                partial.raster_unchanged_outside(source, target, case["rect"], engine)
                for engine in (None, poppler)
            ]
            record.update(max_position_error_pt=maximum, outside_changed_pixels=changed)
            assert maximum <= case["max_position_error_pt"], maximum
            assert changed == [0, 0], changed
            assert not case[
                "refused"
            ], "Candidate accepted despite frozen refusal boundary"
            record["status"] = "PASS"
        except FileNotFoundError as error:
            record.update(status="未実行", error=str(error))
        except Exception as error:
            record["error"] = str(error)
        rows.append(record)
    report = dict(
        criteria_sha256=pin,
        results=rows,
        passed=sum(r["status"] == "PASS" for r in rows),
        failed=sum(r["status"] == "FAIL" for r in rows),
        unexecuted=sum(r["status"] == "未実行" for r in rows),
        scope="Same direct declaration control; shadowed declarations require refusal",
        engines=["PDFium", "pypdf", "Poppler"],
        native_GUI="未実行",
    )
    args.output.write_text(
        json.dumps(report, ensure_ascii=False, indent=2) + "\n", encoding="utf-8"
    )
    print(json.dumps(report, ensure_ascii=True))
    return int(report["failed"] or report["unexecuted"])


if __name__ == "__main__":
    raise SystemExit(main())
