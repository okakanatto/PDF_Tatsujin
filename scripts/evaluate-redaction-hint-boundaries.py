"""Inspect fixed PUSHB/PUSHW limits and each retained font-resource closure."""

import argparse
import importlib.util
import json
import traceback
from pathlib import Path

import pypdfium2 as pdfium
from pypdf import PdfReader

spec = importlib.util.spec_from_file_location(
    "hinting", Path(__file__).with_name("evaluate-redaction-hinting.py")
)
hinting = importlib.util.module_from_spec(spec)
spec.loader.exec_module(hinting)
PIN = "1fcd232da7b7ae1af28430c8fdb7a0e92318c72895637a258f66c874dd3e4941"


def inspect_boundary_source(reader, case):
    bindings, _ = hinting.resource_inventory(reader)
    records = {record["identity"]: record for record in bindings.values()}
    observed = set()
    for criterion in case["source_font_glyphs"]:
        matches = [
            record
            for record in records.values()
            if hinting.partial.sha(record["data"]) == criterion["font_input_sha256"]
        ]
        assert matches, "Frozen boundary font payload missing"
        for record in matches:
            font = record["sfnt"]
            gid = criterion["original_gid"]
            assert (criterion["unicode_char"], gid) in record["mappings"].values()
            glyph = font["glyf"][font.getGlyphOrder()[gid]]
            assert len(glyph.coordinates) == criterion["expanded_points"]
            program = (
                bytes(glyph.program.getBytecode()) if hasattr(glyph, "program") else b""
            )
            assert program.hex() == criterion["expected_program_hex"]
            assert len(program) == case["program_bytes"]
            if not case["refused"]:
                # Independent fontTools bytecode decoding also rejects truncated operands.
                assert glyph.program.getAssembly() if program else True
            observed.add((criterion["font_input_sha256"], gid))
    return dict(
        boundary_source_glyphs=len(observed),
        point_operand=case["point_operand"],
        program_bytes=case["program_bytes"],
    )


def inspect_saved_boundary(source, target, case, fixed, engine):
    before, after = PdfReader(source), PdfReader(target)
    assert len(after.pages) == len(before.pages)
    maximum = 0
    with pdfium.PdfDocument(target) as document:
        for index in range(len(before.pages)):
            terms = case["keep"] if index == 0 else case["second_page_terms"]
            text = document[index].get_textpage()
            content = text.get_text_range()
            assert case["remove"] not in content
            assert case["remove"] not in after.pages[index].extract_text()
            for term in terms:
                assert (
                    content.count(term) == 1
                    and term in after.pages[index].extract_text()
                )
                start = content.index(term)
                actual = [
                    list(text.get_charbox(i)) for i in range(start, start + len(term))
                ]
                boxes = (
                    case["boxes"]
                    if len(before.pages) == 1
                    else case["boxes"][str(index)]
                )
                assert len(actual) == len(boxes[term])
                maximum = max(
                    maximum,
                    max(
                        abs(a - b)
                        for position, expected in zip(actual, boxes[term])
                        for a, b in zip(position, expected)
                    ),
                )
            text.close()
    assert maximum <= fixed["position_error_pt"]
    resources = hinting.audit_resource_glyphs(before, after, case)
    hinting.audit_private_streams(before, after, case, target)
    raster = {}
    for dpi in fixed["dpi"]:
        for label, renderer in (("PDFium", None), ("Poppler", engine)):
            differences = [
                hinting.pixels(
                    source,
                    target,
                    dpi,
                    index,
                    case["rect"] if index == 0 else None,
                    renderer,
                )
                for index in range(len(before.pages))
            ]
            assert all(value == 0 for value in differences), (label, dpi, differences)
            raster[f"{label}-{dpi}"] = differences
    return dict(
        max_position_error_pt=maximum,
        raster=raster,
        original_font_map_content_absent=True,
        kept_outline_and_instructions_exact=True,
        **resources,
        **hinting.check_fonts(after),
    )


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--fixtures", type=Path, required=True)
    parser.add_argument("--run", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    assert not args.output.exists(), "Preserve prior independent reports"
    data = (args.fixtures / "criteria.json").read_bytes()
    assert hinting.partial.sha(data) == PIN
    fixed = json.loads(data)
    assert fixed["position_error_pt"] == 0.5
    assert fixed["dpi"] == [72, 96, 300]
    assert (
        fixed["outside_pixel_difference"] == 0 and fixed["rectangle_boundary_px"] == 1
    )
    process, tests = hinting.validate_producer(
        args.run, {"M5RH03_hint_boundaries", "M5RH04_hint_boundaries_UI"}
    )
    hinting.validate_case_outcomes(
        tests["M5RH03_hint_boundaries"]["details"], fixed["cases"]
    )
    engine = hinting.poppler_tool("pdftoppm")
    results = []
    for case in fixed["cases"]:
        row = dict(file=case["file"], status="FAIL")
        try:
            source = args.fixtures / case["file"]
            assert hinting.partial.sha(source.read_bytes()) == case["sha256"]
            target = args.run / ("hint-boundary-" + case["file"])
            # Rejection is supported by the exact completed producer, not absence alone.
            if case["refused"]:
                assert not target.exists()
                row.update(status="PASS", rejected_output_absent=True)
            else:
                boundary = inspect_boundary_source(PdfReader(source), case)
                verified = inspect_saved_boundary(source, target, case, fixed, engine)
                row.update(status="PASS", **boundary, **verified)
        except Exception as error:
            row["error"] = repr(error)
            row["traceback"] = traceback.format_exc()
        results.append(row)
    ui_source, ui_target = (
        args.run / "hint-boundary-ui-before.pdf",
        args.run / "hint-boundary-ui.pdf",
    )
    assert (
        ui_source.exists() and ui_target.exists()
    ), "Boundary UI producer output missing"
    ui_case = next(
        row for row in fixed["cases"] if row["file"] == "shared-pages-word-256.pdf"
    )
    raster = {}
    for dpi in fixed["dpi"]:
        for label, renderer in (("PDFium", None), ("Poppler", engine)):
            differences = [
                hinting.pixels(
                    ui_source,
                    ui_target,
                    dpi,
                    page,
                    ui_case["rect"] if page == 0 else None,
                    renderer,
                )
                for page in (0, 1)
            ]
            assert differences == [0, 0], (label, dpi, differences)
            raster[f"{label}-{dpi}"] = differences
    results.append(
        dict(
            file="hint-boundary-ui.pdf",
            status="PASS",
            raster=raster,
            unsaved_signature_pixels_preserved=True,
        )
    )
    report = dict(
        criteria_sha256=PIN,
        evaluator_sha256=hinting.partial.sha(Path(__file__).read_bytes()),
        shared_resource_evaluator_sha256=hinting.partial.sha(
            Path(hinting.__file__).read_bytes()
        ),
        resource_audit_schema=2,
        producer_executable_sha256=process["exe_sha256"],
        producer_completed=True,
        results=results,
        passed=sum(row["status"] == "PASS" for row in results),
        failed=sum(row["status"] != "PASS" for row in results),
        engines=["PDFium", "Poppler", "pypdf", "fontTools"],
        general_hinting="unexecuted",
        CVT="unexecuted",
        native_external_GUI="unexecuted",
    )
    args.output.write_text(
        json.dumps(report, ensure_ascii=False, indent=2) + "\n", "utf-8"
    )
    print(json.dumps(report, ensure_ascii=True))
    return int(report["failed"] > 0)


if __name__ == "__main__":
    raise SystemExit(main())
