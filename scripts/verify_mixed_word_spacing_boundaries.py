"""Independent source and actual-runtime checks for the additive RAW boundaries."""

import argparse
import copy
import datetime
import hashlib
import importlib.util
import json
import logging
from pathlib import Path
import traceback


ROOT = Path(__file__).resolve().parents[1]
MAIN_SHA = "3cc1a3172a9fc60b5814f8efc1cecc164b6d95c26f8d0c8f79c5bc50995d2b57"
CRITERIA_SHA = "2d9fc95ed0e62ac20dcf9181604055f0b3b580e3479379617af970bfeded2104"


def digest(data):
    return hashlib.sha256(data).hexdigest()


def info(path):
    path = Path(path).resolve()
    data = path.read_bytes()
    return dict(path=str(path), bytes=len(data), sha256=digest(data))


def load_main():
    path = ROOT / "scripts/verify_mixed_word_spacing.py"
    assert digest(path.read_bytes()) == MAIN_SHA, "fixed independent parser changed"
    spec = importlib.util.spec_from_file_location("fixed_main_auditor", path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def within(path):
    path = Path(path).resolve()
    path.relative_to(ROOT)
    return path


def full_selection(glyph, rect):
    x, y, width, height = rect
    right, top = x + width, y + height
    box = glyph["bounds_pt"]
    if box is None:
        assert glyph["unicode"] == " ", "unexpected glyph without outline"
        origin, baseline = glyph["origin_pt"]
        end = origin + glyph["advance_pt"]
        return (
            x <= min(origin, end) <= max(origin, end) <= right and y <= baseline <= top
        )
    covered = x <= box[0] <= box[2] <= right and y <= box[1] <= box[3] <= top
    disjoint = box[2] <= x or right <= box[0] or box[3] <= y or top <= box[1]
    assert covered or disjoint, "boundary partly touches another actual outline"
    return covered


def alias_case(v, fixed, main, base, row, records):
    """Derive expected geometry only from frozen source fields, never output."""
    case = copy.deepcopy(base)
    case.update(
        rect=row["rect"],
        source_sfnt_sha256=row["source_sfnt_sha256"],
        encoding_sha256=row["encoding_sha256"],
        tounicode_sha256=row["tounicode_sha256"],
    )
    glyph = next(g for g in main["source_glyphs"] if g["gid"] == row["alias_gid"])
    bounds = glyph["true_bezier_bounds"]
    units = next(iter(records.values()))["sfnt"]["head"].unitsPerEm
    condition = case["font_bindings"][0]
    for show in row["alias_shows"]:
        entries = []
        size, scale = show["font_size_pt"], show["horizontal_scale"]
        advance = (1000 * size / 1000 + show["tc"]) * scale
        assert abs(advance - show["per_glyph_advance_pt"]) < 1e-12
        for index, code in enumerate(show["code_hex"]):
            origin = [show["origin_pt"][0] + index * advance, show["origin_pt"][1]]
            box = [
                origin[0] + bounds[0] * size / units * scale,
                origin[1] + bounds[1] * size / units,
                origin[0] + bounds[2] * size / units * scale,
                origin[1] + bounds[3] * size / units,
            ]
            entries.append(
                dict(
                    code_hex=code,
                    cid=row["alias_cid"],
                    gid=row["alias_gid"],
                    unicode=row["alias_unicode"],
                    width=1000.0,
                    origin_pt=origin,
                    advance_pt=advance,
                    raw_code_word_spacing=0,
                    outline_box_pt=box,
                    removed=False,
                )
            )
        condition["streams"].append(
            dict(
                text=show["unicode"],
                operator="Tj",
                origin_pt=show["origin_pt"],
                end_origin_pt=[
                    show["origin_pt"][0] + len(entries) * advance,
                    show["origin_pt"][1],
                ],
                effective_tw=show["tw"],
                effective_tc=show["tc"],
                entries=entries,
            )
        )
    condition["text"] = "".join(op["text"] for op in condition["streams"])
    for code in row["alias_codes"]:
        route = dict(code_hex=code, byte_count=2, value=int(code, 16))
        case["encoding_route"].append(
            dict(route, cid=row["alias_cid"], gid=row["alias_gid"])
        )
        case["unicode_route"].append(dict(route, unicode=row["alias_unicode"]))
    return case


def source_review(args, v):
    path = within(args.criteria)
    assert digest(path.read_bytes()) == args.criteria_sha256 == CRITERIA_SHA
    fixed = json.loads(path.read_text(encoding="utf-8"))
    main_path = within(ROOT / fixed["main_43_criteria"]["path"])
    assert digest(main_path.read_bytes()) == fixed["main_43_criteria"]["sha256"]
    assert (
        fixed["position_error_pt"],
        fixed["boundary_pixels"],
        fixed["outside_pixel_difference"],
    ) == (0.5, 1, 0)
    assert fixed["dpi"] == [72, 96, 300]
    main = json.loads(main_path.read_text(encoding="utf-8"))
    base = next(c for c in main["cases"] if c["file"] == "single-positive.pdf")
    rows, derived = [], {}
    for row in fixed["cases"]:
        source = within(ROOT / row["source"])
        assert digest(source.read_bytes()) == row["sha256"]
        reader = v.PdfReader(source)
        records, uses, operations = v.inspect_pdf(reader)
        assert len(reader.pages) == 1 and len(records) == len(uses) == 1
        font = next(iter(records.values()))
        assert font["sha256"] == main["original_source"]["sfnt_sha256"]
        v.audit_frozen_glyphs(main, records)
        instructions = v.ContentStream(
            reader.pages[0].get_contents(), reader
        ).operations
        assert not any(
            op in (b"W", b"W*", b"cm", b"gs") for operands, op in instructions
        ), "unevaluated clipping/transform/opacity"
        report = dict(name=row["name"], source=info(source), status="PASS_SOURCE")
        if "alias_codes" not in row:
            v.audit_case(base, records, uses, operations)
            selected, indices = [], {}
            for glyph in next(iter(uses.values())):
                ordinal = glyph["show_ordinal"]
                index = indices.get(ordinal, 0)
                indices[ordinal] = index + 1
                if full_selection(glyph, row["rect"]):
                    selected.append([ordinal, index])
            assert selected == row["source_selected_glyphs"]
            report.update(selected=selected, exact_source_reuse=True)
        else:
            case = alias_case(v, fixed, main, base, row, records)
            v.audit_case(case, records, uses, operations)
            expected = v.retained_source(case, uses)
            keys = {g["code_hex"] for glyphs in expected.values() for g in glyphs}
            assert keys == set(row["exact_final_used_keys"])
            assert len(keys) == row["used_codekey_count"]
            assert len(font["spaces"]) == row["source_codespace_count"]
            assert len(font["entries"]) == row["source_mapping_rows"]
            assert len({int.from_bytes(k, "big") for k in font["entries"]}) == len(
                font["entries"]
            ), "equal numeric different lengths"
            assert (
                digest(reader.pages[0].get_contents().get_data())
                == row["content_sha256"]
            )
            crop = [float(x) for x in reader.pages[0].cropbox]
            alias_glyphs = [
                g
                for glyphs in uses.values()
                for g in glyphs
                if g["code_hex"] in row["alias_codes"]
            ]
            assert len(alias_glyphs) == len(row["alias_codes"])
            for glyph in alias_glyphs:
                box = glyph["bounds_pt"]
                assert (
                    box
                    and crop[0] <= box[0] <= box[2] <= crop[2]
                    and crop[1] <= box[1] <= box[3] <= crop[3]
                )
                assert box[3] < row["rect"][1] and not glyph["tw_applies"]
            for op in case["font_bindings"][0]["streams"]:
                assert (
                    op["entries"]
                    and not op["entries"][0]["removed"]
                    and not op["entries"][-1]["removed"]
                )
            derived[row["name"]] = case
            report.update(
                final_used_keys=len(keys),
                mapping_rows=len(font["entries"]),
                visible_aliases=len(alias_glyphs),
                alias_shows=len(row["alias_shows"]),
                no_clipping_or_numeric_collision=True,
            )
        rows.append(report)
    assert len(rows) == 4
    direct = fixed["direct_builder"]
    assert len(set(direct["valid_keys"])) == direct["valid_count"] == 256
    assert len(set(direct["invalid_keys"])) == direct["invalid_count"] == 257
    assert set(direct["valid_keys"]) < set(direct["invalid_keys"])
    return (
        fixed,
        derived,
        dict(
            status="PASS_SOURCE_BOUNDARIES_4",
            source_only=True,
            source_inspections=4,
            runtime_passes=0,
            cases=rows,
            chronology=fixed["chronology"],
            direct_builder_scope="source/API route counts only; runtime unexecuted",
        ),
    )


def qt_geometry(v, path, measured):
    records, uses, operations = v.inspect_pdf(v.PdfReader(path))
    expected = []
    for op in operations:
        if op["binding"]:
            expected.extend(
                (g["unicode"], g["origin_pt"])
                for g in uses[op["binding"]]
                if g["show_ordinal"] == op["show_ordinal"]
            )
        else:
            for data in op["strings_hex"]:
                decoded = bytes.fromhex(data).decode("ascii")
                expected.extend((c, None) for c in decoded)
    assert measured["raw_unicode"] == "".join(c for c, point in expected)
    assert len(measured["glyphs"]) == len(expected)
    errors = []
    for actual, (character, point) in zip(measured["glyphs"], expected):
        assert actual["unicode"] == character
        if point:
            error = max(abs(a - b) for a, b in zip(actual["origin_pt"], point))
            assert error <= 0.5
            errors.append(error)
    return dict(
        Type0_glyphs=len(errors),
        max_origin_error_pt=max(errors, default=0),
        standard_ASCII="literal characters checked; standard origins covered by raster, not source parser",
    )


def runtime_review(args, v, fixed, derived):
    run = within(args.run)
    result_path = run / "boundary-results.json"
    native = json.loads(result_path.read_text(encoding="utf-8-sig"))
    proof_path = within(args.module_proof)
    proof = json.loads(proof_path.read_text(encoding="utf-8-sig"))
    executable = within(args.producer_executable)
    core = within(args.core_library)
    assert (
        native["status"] == "PASS"
        and native["native_pass"] == 6
        and native["native_fail"] == 0
    )
    assert native["fixture_criteria_sha256"] == CRITERIA_SHA
    assert v.same_path(native["executable"]["path"], executable) and native[
        "executable"
    ]["sha256"] == digest(executable.read_bytes())
    assert proof["completed"] and proof["process_id"] == native["pid"]
    assert v.same_path(proof["process_executable"], executable)
    observed = proof["loaded_modules"]
    for path in (executable, core):
        matches = [m for m in observed if v.same_path(m["path"], path)]
        assert len(matches) == 1 and matches[0]["sha256"] == digest(path.read_bytes())
        assert matches[0]["process_id"] == native["pid"]
    actual_core = [
        m for m in native["loaded_modules"] if m["name"].lower() == core.name.lower()
    ]
    assert len(actual_core) == 1 and v.same_path(actual_core[0]["path"], core)
    assert actual_core[0]["sha256"] == digest(core.read_bytes())
    assert native["widgets_module_state"] == "UNLOADED"
    assert not any(
        "pdf4qtlibwidgets" in m["name"].lower() for m in native["loaded_modules"]
    )
    before = [info(p) for p in (executable, core, result_path, proof_path)]
    tests = {r["name"]: r for r in native["cases"]}
    assert len(tests) == 6 and all(r["status"] == "PASS" for r in tests.values())
    rows = []
    engine = None
    from test_tools import poppler_tool

    engine = poppler_tool("pdftoppm")
    for row in fixed["cases"]:
        test = tests[row["name"]]
        source = within(ROOT / row["source"])
        target = within(run / row["output"])
        assert test["source_sha256"] == row["sha256"] == digest(source.read_bytes())
        assert test["source_document_and_history_unchanged"]
        assert test["refused"] is row["refused"]
        report = dict(
            name=row["name"],
            status="PASS",
            source_Qt=qt_geometry(v, source, test["source_geometry"]),
        )
        if row["refused"]:
            assert test["output_absent"] and not target.exists()
            assert row["message_contains"] in test["error"]
            report.update(output_absent=True, original_history_unchanged=True)
        else:
            assert digest(target.read_bytes()) == test["output_sha256"]
            case = derived[row["name"]]
            report["structure"] = v.output_contract(source, target, case)
            records, uses, operations = v.inspect_pdf(v.PdfReader(target))
            font = next(iter(records.values()))
            routes = {
                r["code_hex"]: (r["unicode"], r["cid"]) for r in test["raw_routes"]
            }
            assert set(routes) == set(row["exact_final_used_keys"])
            assert routes == {
                k.hex(): (cp, cid) for k, (cp, cid, gid) in font["entries"].items()
            }
            report["target_Qt"] = qt_geometry(v, target, test["target_geometry"])
            raster = {}
            for dpi in fixed["dpi"]:
                for name, renderer in (("PDFium", None), ("Poppler", engine)):
                    delta = v.raster_compare(
                        source, target, dpi, 0, row["rect"], renderer, args.engine_temp
                    )
                    assert delta == 0, (name, dpi, delta)
                    raster[f"{name}-{dpi}"] = delta
            report["raster"] = raster
            import pypdfium2 as pdfium

            with pdfium.PdfDocument(target) as document:
                text = document[0].get_textpage().get_text_range()
                assert row["remove"] not in text and all(
                    term in text for term in row["keep"]
                )
                assert text.count(row["alias_unicode"]) >= len(row["alias_codes"])
            report["output"] = info(target)
        assert digest(source.read_bytes()) == row["sha256"]
        rows.append(report)
    direct = fixed["direct_builder"]
    for name, oversized in (("builder-256", False), ("builder-257", True)):
        test = tests[name]
        assert test["source_unchanged"] and test["refused"] is oversized
        assert test["route_count"] == (257 if oversized else 256)
        if oversized:
            assert test["builder_storage_unchanged"] and "TrueType" in test["error"]
        else:
            assert test["own_decoder_roundtrip"]
            routes = {
                r["code_hex"]: (r["cid"], r["unicode"]) for r in test["raw_routes"]
            }
            assert set(routes) == set(direct["valid_keys"])
            assert set(routes.values()) == {(1, direct["glyph"]["unicode"])}
        rows.append(
            dict(
                name=name,
                status="PASS_NATIVE_API_METADATA",
                scope="in-memory builder/own decoder; no direct font witness raster/security qualification",
            )
        )
    assert before == [info(p) for p in (executable, core, result_path, proof_path)]
    assert not list(args.engine_temp.rglob("*")), "owned engine TEMP residue"
    return dict(
        status="PASS_SCOPED_BOUNDARIES_6",
        source_only=False,
        runtime_passes=6,
        failed=0,
        cases=rows,
        producer_bindings=before,
        module_proof=proof,
        Widgets="UNLOADED/UNQUALIFIED by this Core-only boundary executable",
        nativeGUI_OSclipboard="UNEXECUTED",
    )


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--criteria", type=Path, required=True)
    parser.add_argument("--criteria-sha256", required=True)
    parser.add_argument("--run", type=Path)
    parser.add_argument("--producer-executable", type=Path)
    parser.add_argument("--core-library", type=Path)
    parser.add_argument("--module-proof", type=Path)
    parser.add_argument("--engine-temp", type=Path)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    logging.getLogger("fontTools").setLevel(logging.ERROR)
    try:
        v = load_main()
        fixed, derived, source = source_review(args, v)
        report = runtime_review(args, v, fixed, derived) if args.run else source
        if args.run:
            report["source_review"] = source
    except Exception as error:
        report = dict(
            status="FAIL",
            error=repr(error),
            traceback=traceback.format_exc(),
            runtime_passes=0,
        )
    report.update(
        run_time_utc=datetime.datetime.now(datetime.timezone.utc).isoformat(),
        examiner=info(__file__),
        main_examiner_sha256=MAIN_SHA,
        criterion_sha256=args.criteria_sha256,
        general_M1_M3_M5_acceptance="withheld",
        source_review_first_attempt="Earlier one-off source metadata proof used int(raw-bytes,16) instead of int.from_bytes; failed before writes, corrected here. Input/truth unchanged; tool error retained.",
    )
    output = within(args.output)
    with output.open("x", encoding="utf-8", newline="\n") as stream:
        json.dump(report, stream, ensure_ascii=False, indent=2)
        stream.write("\n")
    print(
        json.dumps(
            {
                k: report[k]
                for k in ("status", "source_inspections", "runtime_passes", "failed")
                if k in report
            }
        )
    )
    return 1 if report["status"] == "FAIL" else 0


if __name__ == "__main__":
    raise SystemExit(main())
