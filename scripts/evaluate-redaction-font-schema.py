"""Inspect standard-font private data, text, geometry and refusal independently."""

import argparse
import importlib.util
import json
from pathlib import Path

import pypdfium2 as pdfium
from pypdf import PdfReader
from pypdf.generic import ContentStream, StreamObject
from test_tools import poppler_tool

spec = importlib.util.spec_from_file_location(
    "partial", Path(__file__).with_name("evaluate-redaction-partial.py")
)
partial = importlib.util.module_from_spec(spec)
spec.loader.exec_module(partial)
PIN = "91765aeee26617872f16ba46f4b9cadba61468279ef157a141d8e9891630ad63"
PINS = {
    "standard-font-families-v2": "708cfc9929203151427b7d09a9914cfe294b9112b13fab024abbd5fb5e58ed9d",
    "font-schema-direct-followup-v1": "e7569c49f5917663c07232ee1cd542ccebbbc719fd545fb9841428ad1b87361d",
    "font-schema-direct-v1": "8c40a3c704d4581ef751ddc6217295f970c92cd189bd39277ec8bb9e0c7d4f2c",
    "font-schema-v2": PIN,
    "font-schema-followup-v1": "a2a73f6c9a8d2a087bc511509ae7b83f2eb81a267ad5dc42eb944d38440e02c6",
}


def inspect(source, target, case, poppler):
    content, boxes = partial.text_and_boxes(target, case["keep"])
    assert case["remove"] not in content
    reader = PdfReader(target)
    if case.get("appearance_terms"):
        appearances = reader.pages[0]["/Annots"][0].get_object()["/AP"]
        for state, term in zip(
            ("/N", "/R", "/D"), case["appearance_terms"], strict=True
        ):
            operations = ContentStream(appearances[state], reader).operations
            strings = [
                str(operands[0])
                for operands, operator in operations
                if operator == b"Tj"
            ]
            assert term in strings, (state, term, strings)
    extracted = reader.pages[0].extract_text()
    assert case["remove"] not in extracted
    assert all(term in extracted for term in case["keep"])
    marker = case["private_marker"]
    if marker:
        assert (
            marker.encode() not in target.read_bytes()
        ), "Private marker retained in file"
        for value in partial.objects(reader):
            assert marker not in str(value), "Private dictionary value retained"
            if isinstance(value, StreamObject):
                assert (
                    marker.encode() not in value.get_data()
                ), "Private stream retained"
        for value in partial.objects(reader):
            if isinstance(value, dict) and value.get("/Subtype") == "/Type1":
                assert "/PrivateAuditPayload" not in value
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
    assert maximum <= case["max_position_error_pt"], maximum
    assert changed == [0, 0], changed
    second_changed = []
    if case["second_page_terms"]:
        assert len(reader.pages) == 2
        with pdfium.PdfDocument(target) as document:
            second_text = document[1].get_textpage().get_text_range()
            assert all(term in second_text for term in case["second_page_terms"])
        assert all(
            term in reader.pages[1].extract_text() for term in case["second_page_terms"]
        )
        second_changed = [
            partial.raster_unchanged_outside(
                source, target, [-10, -10, 0, 0], engine, 1
            )
            for engine in (None, poppler)
        ]
        assert second_changed == [0, 0], second_changed
    return dict(
        max_position_error_pt=maximum,
        outside_changed_pixels=changed,
        unselected_page_changed_pixels=second_changed,
        private_data_absent=True,
    )


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--fixtures", type=Path, required=True)
    parser.add_argument("--run", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--probe-layout", action="store_true")
    args = parser.parse_args()
    assert not args.output.exists()
    raw = (args.fixtures / "criteria.json").read_bytes()
    pin = PINS[args.fixtures.name]
    assert partial.sha(raw) == pin
    poppler = poppler_tool("pdftoppm")
    rows = []
    for case in json.loads(raw)["cases"]:
        source = args.fixtures / case["file"]
        assert partial.sha(source.read_bytes()) == case["sha256"]
        target = (
            args.run / Path(case["file"]).stem / "candidate.pdf"
            if args.probe_layout
            else args.run / ("schema-" + case["file"])
        )
        record = dict(
            file=case["file"], status="FAIL", expected_refusal=case["refused"]
        )
        try:
            if case["refused"]:
                assert not target.exists(), "Unsafe declaration was accepted"
                record.update(status="PASS", refused_output_absent=True)
            else:
                record.update(inspect(source, target, case, poppler), status="PASS")
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
        success_outputs=sum(
            r["status"] == "PASS" and not r["expected_refusal"] for r in rows
        ),
        refusal_absence_checks=sum(
            r["status"] == "PASS" and r["expected_refusal"] for r in rows
        ),
        scope="Private data absent; unselected same phrase and rendered pages preserved",
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
