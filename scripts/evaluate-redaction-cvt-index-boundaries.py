"""Independent frozen CVT index-boundary and dev40 metadata audits.

Only --review-dev40-metadata runs without PDF dependencies. Boundary source-only
inspection never renders or claims product refusal/acceptance. Existing evaluators
and their criteria are immutable dependencies, checked before importing them.
"""

import argparse
import datetime
import gzip
import hashlib
import importlib.util
import json
import re
import struct
import subprocess
import traceback
from collections import Counter, defaultdict
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
OLD_EVALUATORS = {
    "evaluate-redaction-cvt.py": "19391459934f94c7da499a2d8f1cb6de26a60d9fdf595343da2cce43f77f7312",
    "evaluate-redaction-hinting.py": "53434799af0e6066e4a3f619902863089bf441497eb10b6461c1903c78dca5cf",
    "evaluate-redaction-hint-boundaries.py": "8ec629354226bd5af9e3c0e06cb8d405456eb3ad86d6367bbfc00551c3db7016",
}
# Populated only after the independent pre-product fixture review.
CRITERIA_PIN = "d8243815eeec4a579639502d3d66533ba876bec483fdc6e3edc813d83453c596"


def sha(path):
    digest = hashlib.sha256()
    with Path(path).open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def digest(data):
    return hashlib.sha256(data).hexdigest()


def load(path):
    return json.loads(Path(path).read_bytes())


def metadata_review():
    """Read actual metadata and current bytes; do not decompress/restore a run."""
    checks = []

    def verified(path, expected=None, kind="metadata"):
        path = Path(path)
        actual = sha(path)
        assert expected is None or actual == expected, (str(path), actual, expected)
        checks.append(dict(path=str(path), sha256=actual, kind=kind))
        return actual

    aggregate = load("docs/testing/M5_CVT_REFERENCES_RESULTS.json")
    verified("docs/testing/M5_CVT_REFERENCES_RESULTS.json")
    optional = load("evidence/dev40-integration-review-unexecuted.json")
    verified("evidence/dev40-integration-review-unexecuted.json")
    assert optional["status"] == "UNEXECUTED_OPTIONAL_ADDITIONAL_REVIEW"
    assert optional["extra_review_counted_as_PASS"] is False
    proof = load("evidence/dev40-independent-proof.json")
    verified("evidence/dev40-independent-proof.json")
    assert proof["status"] == "PASS" and proof["general_M5_acceptance"] == "withheld"
    verified(proof["executable"], proof["executable_sha256"], "producer-executable")
    run = proof["run"]
    process = load(run + "/process-result.json")
    tests = load(run + "/selftest.json")["tests"]
    verified(run + "/process-result.json", proof["process_sha256"], "producer-process")
    verified(run + "/selftest.json", proof["selftest_sha256"], "producer-report")
    assert (
        process["completed"] and process["report_valid"] and process["exit_code"] == 0
    )
    assert process["exe_sha256"] == proof["executable_sha256"]
    assert process["test_count"] == len(tests) == 256
    assert len({row["name"] for row in tests}) == 256
    assert all(row["status"] == "PASS" for row in tests)
    assert aggregate["independent"] == proof["checks"]
    reports = {}
    for row in proof["checks"]:
        verified(row["path"], row["sha256"], "required-report")
        report = load(row["path"])
        reports[row["path"]] = report
        assert report.get("status", "PASS") == "PASS" and report.get("failed", 0) == 0
        count = report.get(
            "passed", report.get("count", report.get("limited_PASS_status_fields"))
        )
        assert count == row["passed"]
    for name, script in (
        ("cvt", "evaluate-redaction-cvt.py"),
        ("hinting", "evaluate-redaction-hinting.py"),
        ("boundaries", "evaluate-redaction-hint-boundaries.py"),
    ):
        report = reports["evidence/dev40-independent-" + name + "-final.json"]
        verified(ROOT / "scripts" / script, report["evaluator_sha256"], "evaluator")
        if "shared_resource_evaluator_sha256" in report:
            verified(
                ROOT / "scripts/evaluate-redaction-hinting.py",
                report["shared_resource_evaluator_sha256"],
                "resource-evaluator",
            )
        # Earlier report schemas lack source_only; the completed producer binding
        # and actual result fields are their explicit runtime evidence.
        assert report.get("source_only", False) is False
        assert report["producer_completed"] is True
        assert report["producer_executable_sha256"] == proof["executable_sha256"]
        assert report["passed"] == len(report["results"])
        assert all(row["status"] == "PASS" for row in report["results"])
    verified(
        aggregate["criteria"]["path"],
        aggregate["criteria"]["sha256"],
        "frozen-criteria",
    )
    verified(
        "fixtures/redaction-copy/hint-boundaries-v1/criteria.json",
        "1fcd232da7b7ae1af28430c8fdb7a0e92318c72895637a258f66c874dd3e4941",
        "frozen-criteria",
    )
    index = load("dist/dev40-validation-recovery-r1-20261012/index.json")
    verified("dist/dev40-validation-recovery-r1-20261012/index.json")
    verified(index["plan"], index["plan_sha256"], "immutable-plan")
    plan = json.loads(gzip.decompress(Path(index["plan"]).read_bytes()))
    assert plan["validation_run"] == run
    assert plan["bindings"]["executable_sha256"] == proof["executable_sha256"]
    verified(
        plan["bindings"]["independent_proof"],
        plan["bindings"]["independent_proof_sha256"],
        "bound-proof",
    )
    for path, expected in plan["bindings"]["independent_checks"].items():
        verified(path, expected, "bound-required-report")
    entries, group = plan["entries"], plan["groups"][0]
    assert len(entries) == len(group["paths"]) == 1383
    assert set(entries) == set(group["paths"])
    assert (
        sum(row["bytes"] for row in entries.values())
        == group["original_bytes"]
        == 207440786
    )
    archive = load(plan["store"] + "/proof-validation.json")
    verified(plan["store"] + "/proof-validation.json")
    assert (
        archive["status"] == "PASS"
        and archive["disk_readback_every_original_sha"] is True
    )
    assert archive["plan_sha256"] == index["plan_sha256"] and archive["files"] == 1383
    verified(group["container"], archive["container_sha256"], "archive-container")
    assert (
        Path(group["container"]).stat().st_size
        == archive["container_bytes"]
        == 62990528
    )
    sample = archive["disk_materialization"]
    entry = entries[sample["original"]]
    assert entry["sha256"] == sample["sha256"]
    assert Path(sample["path"]).stat().st_size == entry["bytes"]
    verified(sample["path"], entry["sha256"], "existing-restored-sample")
    removed = load(plan["store"] + "/removed-validation.json")
    verified(plan["store"] + "/removed-validation.json")
    assert (
        removed["status"] == "PASS" and removed["plan_sha256"] == index["plan_sha256"]
    )
    assert all(not Path(path).exists() for path in entries)
    for row in plan["excluded_literal_fault_records"]:
        verified(row["path"], row["sha256"], "protected-fault-record")
    m5 = reports["evidence/dev40-M5-independent-summary-final.json"]
    assert len(m5["groups"]) == 19 and sum(row["passed"] for row in m5["groups"]) == 124
    assert sum(row["mode"] == "reused_identical_bytes" for row in m5["groups"]) == 18
    assert m5["reused_groups"] == 18 and m5["reexecuted_groups"] == 1
    comparisons = byte_comparisons = absences = 0
    for row in m5["groups"]:
        verified(row["report"], row["report_sha256"], "M5-leaf-report")
        if row.get("execution_log"):
            verified(row["execution_log"], row["execution_log_sha256"], "M5-leaf-log")
        assert load(row["report"]).get("failed", 0) == 0
        for item in row.get("comparison", []):
            comparisons += 1
            path = run + "/" + item["file"]
            if item.get("output_absent"):
                assert not Path(path).exists() and path not in entries
                absences += 1
            elif "sha256" in item:
                if item["identical"]:
                    assert item["sha256"] == item["baseline_sha256"]
                assert entries[path]["sha256"] == item["sha256"]
                byte_comparisons += 1
            elif item.get("refused") or item.get("absent") or item.get("refusal"):
                assert not Path(path).exists()
    assert comparisons == 161, comparisons
    assert byte_comparisons == 119 and absences == 42
    editing = reports["evidence/dev40-editing-independent-summary-final.json"]
    assert len(editing["groups"]) == 9
    assert (
        sum(row["count"] for row in editing["groups"]) == 160
        and editing["reused_groups"] == 0
    )
    snapshots = 0
    for row in editing["groups"]:
        verified(row["report"], row["report_sha256"], "editing-leaf-report")
        verified(row["execution_log"], row["execution_log_sha256"], "editing-leaf-log")
        for item in row["case_byte_snapshot"]:
            snapshots += 1
            entry = entries[item["path"]]
            assert entry["sha256"] == item["sha256"] and entry["bytes"] == item["bytes"]
    assert snapshots == 160
    references = editing["generated_reference_files"]
    assert (
        len(references) == 321 and sum(row["bytes"] for row in references) == 16028625
    )
    for row in references:
        entry = entries[row["path"]]
        assert entry["bytes"] == row["bytes"] and entry["sha256"] == row["sha256"]
    m1 = reports["evidence/dev40-M1-independent-summary-final.json"]
    verified(m1["report"], m1["report_sha256"], "M1-leaf-report")
    assert len(m1["status_fields"]) == 26 and all(
        row["status"] == "PASS" for row in m1["status_fields"]
    )
    assert len(m1["non_search_boolean_checks"]) == 7 and all(
        row["value"] is True for row in m1["non_search_boolean_checks"]
    )
    for key in ("parent_integration", "regression_runner_recovery"):
        verified(aggregate[key]["path"], aggregate[key]["sha256"], key)
    assert (
        subprocess.check_output(
            ["git", "cat-file", "-t", proof["source_commit"]], text=True
        ).strip()
        == "commit"
    )
    return dict(
        status="PASS_SCOPED_METADATA_REVIEW",
        review_scope="Actual report/evaluator/criteria/executable/container/sample SHA readback and metadata arithmetic; no native suite, raster, full decompression, or new disk restoration",
        required_runtime_pass_count_added=0,
        old_unexecuted_review_unchanged=True,
        source_commit=proof["source_commit"],
        producer_tests=256,
        fresh_CVT_hint_boundaries_conditions=90,
        old_M5_conditions=124,
        old_M5_reused_groups=18,
        old_M5_fresh_groups=1,
        old_M5_case_aux_index_matches=comparisons,
        old_M5_present_copy_SHA_index_matches=byte_comparisons,
        producer_bound_refusal_absences=absences,
        editing_conditions=160,
        editing_case_snapshot_index_matches=snapshots,
        generated_reference_index_matches=321,
        generated_reference_bytes=16028625,
        archive_entries=1383,
        archive_original_bytes=207440786,
        archive_current_bytes=62990528,
        archive_full_payload_readback_this_review="UNEXECUTED",
        current_existing_restore_sample_sha_verified=True,
        verified_file_SHA_checks=checks,
        general_M1_M3_M5_acceptance="withheld",
        low_DPI_rendering_effect_this_review="UNEXECUTED",
    )


def primitives():
    for name, pin in OLD_EVALUATORS.items():
        assert sha(ROOT / "scripts" / name) == pin, ("Old evaluator changed", name)
    spec = importlib.util.spec_from_file_location(
        "cvt_reference_audit", ROOT / "scripts/evaluate-redaction-cvt.py"
    )
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module, module.hinting


def checksum(data):
    padded = data + b"\0" * (-len(data) % 4)
    return sum(struct.unpack(">" + "I" * (len(padded) // 4), padded)) & 0xFFFFFFFF


def sfnt_header_audit(record, cvt):
    """Check raw checksums plus independently recalculate retained geometry."""
    data, font = record["data"], record["sfnt"]
    assert data[:4] == b"\0\x01\0\0" and len(data) >= 12
    count, search_range, selector, shift = struct.unpack_from(">4H", data, 4)
    power = 1 << (count.bit_length() - 1)
    assert (search_range, selector, shift) == (
        power * 16,
        power.bit_length() - 1,
        count * 16 - power * 16,
    )
    table_rows, spans, tags = [], [], []
    for index in range(count):
        tag, expected, offset, size = struct.unpack_from(
            ">4sIII", data, 12 + 16 * index
        )
        assert (
            offset % 4 == 0 and offset >= 12 + 16 * count and offset + size <= len(data)
        )
        body = data[offset : offset + size]
        if tag == b"head":
            assert len(body) >= 54
            body = body[:8] + b"\0\0\0\0" + body[12:]
        assert checksum(body) == expected, ("Table checksum", tag)
        tags.append(tag)
        spans.append((offset, offset + size))
        table_rows.append(tag.decode("ascii"))
    assert tags == sorted(set(tags)), "Duplicate or unordered SFNT table"
    assert set(tags) <= {
        b"head",
        b"hhea",
        b"maxp",
        b"hmtx",
        b"loca",
        b"glyf",
        b"cmap",
        b"name",
        b"post",
        b"prep",
        b"cvt ",
    }, "Unknown SFNT table retained"
    assert "prep" not in font or font.getTableData("prep") == cvt.hinting.PREP
    ordered = sorted(spans)
    assert all(
        a[1] <= b[0] for a, b in zip(ordered, ordered[1:])
    ), "Overlapping SFNT tables"
    assert checksum(data) == 0xB1B0AFBA, "Whole SFNT checkSumAdjustment"
    outlines, advances, bearings, right_bearings, extents = [], [], [], [], []
    for name in font.getGlyphOrder():
        glyph = font["glyf"][name]
        advance, bearing = font["hmtx"][name]
        advances.append(advance)
        if glyph.numberOfContours == 0:
            continue
        old_bounds = (glyph.xMin, glyph.yMin, glyph.xMax, glyph.yMax)
        glyph.recalcBounds(font["glyf"])
        actual_bounds = (glyph.xMin, glyph.yMin, glyph.xMax, glyph.yMax)
        assert old_bounds == actual_bounds, (
            "Glyph bbox differs from retained geometry",
            name,
        )
        outlines.append(actual_bounds)
        width = glyph.xMax - glyph.xMin
        bearings.append(bearing)
        right_bearings.append(advance - bearing - width)
        extents.append(bearing + width)
    expected_bbox = (
        (
            min(row[0] for row in outlines),
            min(row[1] for row in outlines),
            max(row[2] for row in outlines),
            max(row[3] for row in outlines),
        )
        if outlines
        else (0, 0, 0, 0)
    )
    head, hhea = font["head"], font["hhea"]
    assert (head.xMin, head.yMin, head.xMax, head.yMax) == expected_bbox
    assert hhea.advanceWidthMax == max(advances)
    assert (hhea.minLeftSideBearing, hhea.minRightSideBearing, hhea.xMaxExtent) == (
        min(bearings, default=0),
        min(right_bearings, default=0),
        max(extents, default=0),
    )
    assert hhea.numberOfHMetrics == len(font.getGlyphOrder())
    decoded = [
        cvt.bounded_program(font, gid) for gid in range(len(font.getGlyphOrder()))
    ]
    longest = max((len(program) for program, _ in decoded), default=0)
    referenced = any(operands for _, operands in decoded)
    assert font["maxp"].maxStackElements == (
        2 if referenced else int(bool(longest or "prep" in font))
    )
    assert font["maxp"].maxSizeOfInstructions == longest
    assert font["maxp"].maxZones == 1
    assert all(
        getattr(font["maxp"], name) == 0
        for name in (
            "maxTwilightPoints",
            "maxStorage",
            "maxFunctionDefs",
            "maxInstructionDefs",
        )
    )
    raw_maxp = font.getTableData("maxp")
    font["maxp"].recalc(font)
    assert font.getTableData("maxp") == raw_maxp, "Recalculated maxp geometry"
    assert ("cvt " in font) == referenced
    return dict(
        tables=table_rows,
        all_table_checksums_exact=True,
        checkSumAdjustment_exact=True,
        head_hhea_geometry_recalculated=True,
        maxp_geometry_stack_instruction_length_recalculated=True,
        max_instruction_bytes=longest,
        max_stack=font["maxp"].maxStackElements,
    )


def expected_signature(record, gid, mapping, cvt, hinting):
    signature = list(hinting.glyph_signature(record, gid))
    signature[1] = cvt.rewrite_expected(record["sfnt"], gid, mapping)
    return structural_signature(record, gid, tuple(signature), mapping, cvt, hinting)


def structural_signature(record, gid, base, mapping, cvt, hinting):
    """Preserve compound flags/arguments/transforms as well as drawn topology."""
    font = record["sfnt"]
    glyph = font["glyf"][font.getGlyphOrder()[gid]]
    children = []
    if glyph.isComposite():
        for component in glyph.components:
            attributes = tuple(
                sorted(
                    (key, repr(value))
                    for key, value in component.__dict__.items()
                    if key != "glyphName"
                )
            )
            child = font.getGlyphID(component.glyphName)
            child_signature = (
                actual_signature(record, child, cvt, hinting)
                if mapping is None
                else expected_signature(record, child, mapping, cvt, hinting)
            )
            children.append((attributes, child_signature))
    return (glyph.numberOfContours, base, tuple(children))


def actual_signature(record, gid, cvt, hinting):
    return structural_signature(
        record, gid, hinting.glyph_signature(record, gid), None, cvt, hinting
    )


def expected_usage(source, usage, case):
    expected = {}
    uses = {}
    for key, text in usage.items():
        deleted = set()
        if key.startswith("page/0/"):
            for match in re.finditer(re.escape(case["remove"]), text):
                deleted.update(range(match.start(), match.end()))
        kept = "".join(
            character for index, character in enumerate(text) if index not in deleted
        )
        if kept:
            expected[key] = kept
            uses[key] = [
                row
                for index, row in enumerate(source[key]["used_glyphs"])
                if index not in deleted
            ]
    return expected, uses


def frozen_closure_match(case, keys):
    matches = []
    for row in case["expected_local_closures"]:
        prefixes = row["binding_prefixes"]
        if any(any(key == prefix for prefix in prefixes) for key in keys):
            matches.append(row)
    assert matches, ("Missing frozen local closure", keys)
    assigned = [prefix for row in matches for prefix in row["binding_prefixes"]]
    assert set(assigned) == set(keys) and len(assigned) == len(keys), (
        "Ambiguous frozen local closure",
        keys,
    )
    return matches


def audit_closures(before, after, case, cvt, hinting):
    from pypdf.generic import IndirectObject, StreamObject

    source, usage = hinting.resource_inventory(before)
    target, actual_usage = hinting.resource_inventory(after)
    assert set(target) == set(
        actual_usage
    ), "Unused embedded font resource binding survives"
    expected, uses = expected_usage(source, usage, case)
    assert set(actual_usage) == set(expected), "Used resource bindings changed"
    groups = defaultdict(list)
    for key, actual in actual_usage.items():
        assert Counter(actual) == Counter(expected[key]), (
            "Used characters changed",
            key,
        )
        groups[target[key]["identity"]].append(key)
    known, bound_files, union, reports = set(), set(), defaultdict(set), []
    covered = []
    for keys in groups.values():
        original, retained = source[keys[0]], target[keys[0]]
        source_sha = digest(original["data"])
        assert {digest(source[key]["data"]) for key in keys} == {
            source_sha
        }, "Different input SFNTs merged"
        old_roots = {gid for key in keys for _, gid in uses[key]}
        new_roots = {gid for _, gid in retained["mappings"].values()}
        old_closure = hinting.glyph_closure(original, old_roots)
        new_closure = hinting.glyph_closure(retained, new_roots)
        old_indices = sorted(
            {
                operand["old"]
                for gid in old_closure
                for operand in cvt.bounded_program(original["sfnt"], gid)[1]
            }
        )
        mapping = {old: dense for dense, old in enumerate(old_indices)}
        original_cvt = (
            original["sfnt"].getTableData("cvt ") if "cvt " in original["sfnt"] else b""
        )
        expected_cvt = b"".join(
            original_cvt[index * 2 : index * 2 + 2] for index in old_indices
        )
        actual_cvt = (
            retained["sfnt"].getTableData("cvt ") if "cvt " in retained["sfnt"] else b""
        )
        assert actual_cvt == expected_cvt, ("Exact local CVT bytes", keys, old_indices)
        assert bool(actual_cvt) == ("cvt " in retained["sfnt"])
        frozen_rows = frozen_closure_match(case, keys)
        assert old_roots == {
            gid for row in frozen_rows for gid in row["expected_root_gids"]
        }
        assert old_closure == {
            gid for row in frozen_rows for gid in row["expected_closure_gids"]
        }
        for frozen in frozen_rows:
            assert frozen["source_font_sha256"] == source_sha
            assert frozen["expected_original_indices"] == old_indices
            assert {
                int(key): value for key, value in frozen["expected_index_map"].items()
            } == mapping
            assert frozen["expected_cvt_hex"] == expected_cvt.hex()
            for row in frozen.get("expected_programs", []):
                assert (
                    cvt.rewrite_expected(
                        original["sfnt"], row["hint_gid"], mapping
                    ).hex()
                    == row["program_hex"]
                )
        covered.extend(keys)
        if original_cvt:
            union[source_sha].update(old_indices)
        allowed = set("".join(expected[key] for key in keys))
        assert {character for character, _ in retained["mappings"].values()} == allowed
        assert set(range(len(retained["sfnt"].getGlyphOrder()))) == new_closure | {
            0
        }, "Unmapped noncomponent glyph remains"
        if 0 not in new_closure:
            empty = retained["sfnt"]["glyf"][retained["sfnt"].getGlyphOrder()[0]]
            assert empty.numberOfContours == 0 and (
                not hasattr(empty, "program") or not empty.program.getBytecode()
            )
        assert Counter(
            expected_signature(original, gid, mapping, cvt, hinting)
            for gid in old_closure
        ) == Counter(
            actual_signature(retained, gid, cvt, hinting) for gid in new_closure
        ), "Exact retained outline/component flags/metrics/hint closure"
        for key in keys:
            assert Counter(
                (character, expected_signature(source[key], gid, mapping, cvt, hinting))
                for character, gid in uses[key]
            ) == Counter(
                (character, actual_signature(target[key], gid, cvt, hinting))
                for character, gid in target[key]["used_glyphs"]
            ), (
                "Actual resource/GID glyph use",
                key,
            )
        actual_refs = sorted(
            {
                operand["old"]
                for gid in new_closure
                for operand in cvt.bounded_program(retained["sfnt"], gid)[1]
            }
        )
        assert actual_refs == list(
            range(len(old_indices))
        ), "Unused/non-dense CVT entries"
        known.add(digest(retained["data"]))
        identity = retained["identity"]
        assert (
            identity[0] != "direct"
        ), "Frozen output audit requires indirect font objects"
        font_object = after.get_object(IndirectObject(identity[0], identity[1], after))
        child = (
            font_object["/DescendantFonts"][0]
            if "/DescendantFonts" in font_object
            else font_object
        )
        descriptor = child.get_object()["/FontDescriptor"].get_object()
        bound_files.add(hinting.object_key(descriptor.raw_get("/FontFile2")))
        reports.append(
            dict(
                bindings=keys,
                source_payload_sha256=source_sha,
                retained_payload_sha256=digest(retained["data"]),
                original_indices=old_indices,
                local_dense_map=mapping,
                exact_cvt_hex=expected_cvt.hex(),
                kept_glyph_closure=len(old_closure),
                **sfnt_header_audit(retained, cvt),
            )
        )
    assert len(covered) == len(set(covered)) == len(expected)
    frozen_keys = [
        prefix
        for row in case["expected_local_closures"]
        for prefix in row["binding_prefixes"]
    ]
    assert set(frozen_keys) == set(covered), "Frozen closure omitted from actual use"
    expected_union = {
        row["source_font_sha256"]: row["indices"]
        for row in case["expected_source_reference_union"]
    }
    assert len(expected_union) == len(case["expected_source_reference_union"])
    actual_union = {key: sorted(value) for key, value in union.items()}
    assert actual_union == expected_union and all(
        actual_union.values()
    ), "Final same-input-SFNT reachable CVT union differs"
    all_payloads = {
        digest(value["/FontFile2"].get_data())
        for value in hinting.all_objects(after)
        if isinstance(value, dict) and "/FontFile2" in value
    }
    assert all_payloads == known, "Unbound/orphan FontFile2 remains"
    descriptor_files = {
        hinting.object_key(value.raw_get("/FontFile2"))
        for value in hinting.all_objects(after)
        if isinstance(value, dict) and "/FontFile2" in value
    }
    sfnt_streams = {
        hinting.object_key(value)
        for value in hinting.all_objects(after)
        if isinstance(value, StreamObject)
        and value.get_data().startswith(b"\0\x01\0\0")
    }
    assert (
        descriptor_files == bound_files == sfnt_streams
    ), "Unbound SFNT stream or FontFile2 reference survives"
    mapped_font_objects = {
        hinting.object_key(value)
        for value in hinting.all_objects(after)
        if isinstance(value, dict)
        and "/ToUnicode" in value
        and hinting.font_record(value) is not None
    }
    assert mapped_font_objects == {
        row["identity"] for row in target.values()
    }, "Orphan embedded font/CID/ToUnicode mapping survives"
    return dict(
        resource_bindings=len(expected),
        local_closures=reports,
        final_reachable_input_SFNT_reference_union=actual_union,
        local_mapping_separate_from_source_union=True,
        all_FontFile2_payloads_and_object_references_bound=True,
    )


def inspect_source(source, case, cvt, hinting):
    from pypdf import PdfReader

    reader = PdfReader(source)
    inventory, usage = hinting.resource_inventory(reader)
    records = {digest(row["data"]): row for row in inventory.values()}
    assert sha(case["source_file"]) == case["source_sha256"]
    baseline_bindings, _ = hinting.resource_inventory(PdfReader(case["source_file"]))
    baselines = {digest(row["data"]): row for row in baseline_bindings.values()}
    position_error = 0
    with cvt.pdfium.PdfDocument(source) as document:
        for page_index in range(len(document)):
            terms = case["keep"] if page_index == 0 else case["second_page_terms"]
            text = document[page_index].get_textpage()
            content = text.get_text_range()
            boxes = (
                case["boxes"] if len(document) == 1 else case["boxes"][str(page_index)]
            )
            for term in terms:
                assert content.count(term) == 1, (
                    "Source search term multiplicity",
                    term,
                )
                start = content.index(term)
                actual = [
                    list(text.get_charbox(index))
                    for index in range(start, start + len(term))
                ]
                assert len(actual) == len(boxes[term])
                position_error = max(
                    position_error,
                    max(
                        abs(a - b)
                        for position, expected in zip(actual, boxes[term])
                        for a, b in zip(position, expected)
                    ),
                )
            text.close()
    assert position_error <= 0.0001, (
        "Frozen position truth differs from constructed input",
        position_error,
    )
    observed = []
    for criterion in case["source_font_glyphs"]:
        record = records[criterion["font_input_sha256"]]
        font, gid = record["sfnt"], criterion["hint_gid"]
        glyph = font["glyf"][font.getGlyphOrder()[gid]]
        assert not glyph.isComposite() and glyph.numberOfContours > 0
        assert len(glyph.coordinates) == criterion["point_count"]
        raw = bytes(glyph.program.getBytecode())
        assert raw.hex() == criterion["expected_program_hex"]
        assert font.getTableData("cvt ").hex() == criterion["original_cvt_hex"]
        assert len(font.getTableData("cvt ")) == case["source_cvt_entry_count"] * 2
        assert glyph.program.getAssembly(), "fontTools instruction decoding"
        assert (criterion["unicode_char"], criterion["original_gid"]) in record[
            "mappings"
        ].values()
        baseline = baselines[criterion["source_font_sha256"]]
        baseline_font = baseline["sfnt"]
        original = baseline_font["glyf"][
            baseline_font.getGlyphOrder()[criterion["original_gid"]]
        ]

        def outline(font_value, name):
            glyph_value = font_value["glyf"][name]
            coordinates, endpoints, flags = glyph_value.getCoordinates(
                font_value["glyf"]
            )
            return dict(
                coordinates=[list(point) for point in coordinates],
                contour_endpoints=list(endpoints),
                flags=list(flags),
                bbox=[
                    glyph_value.xMin,
                    glyph_value.yMin,
                    glyph_value.xMax,
                    glyph_value.yMax,
                ],
                advance=list(font_value["hmtx"][name]),
            )

        baseline_outline = outline(
            baseline_font, baseline_font.getGlyphOrder()[criterion["original_gid"]]
        )
        assert (
            baseline_outline == criterion["baseline_outline"]
        ), "Baseline outline truth does not match original input"
        current_outline = outline(font, font.getGlyphOrder()[gid])
        encoded = (
            json.dumps(current_outline, ensure_ascii=False, indent=2) + "\n"
        ).encode("utf-8")
        assert digest(encoded) == criterion["input_outline_sha256"]
        assert criterion["original_points"] == len(original.coordinates)
        if criterion["role"] == "main-dense":
            assert 0 < len(original.coordinates) <= 257 and original.flags[0] & 1
            assert criterion["first_on_curve"] is True and glyph.flags[0] & 1
            extra = 257 - len(original.coordinates)
            assert list(glyph.coordinates) == [original.coordinates[0]] * extra + list(
                original.coordinates
            )
            assert list(glyph.flags) == [original.flags[0]] * extra + list(
                original.flags
            )
            assert list(glyph.endPtsOfContours) == [
                point + extra for point in original.endPtsOfContours
            ]
            assert (
                current_outline["bbox"] == baseline_outline["bbox"]
                and current_outline["advance"] == baseline_outline["advance"]
            )
            expected_program = bytearray(b"\x01")
            for point in range(256):
                expected_program.extend(
                    cvt.encode_push(point) + cvt.encode_push(512 - point) + b"\x3f"
                )
            expected_program.extend(bytes.fromhex("b801002f31"))
            assert raw == bytes(expected_program) and len(raw) == 1542
        else:
            assert (
                current_outline == baseline_outline
            ), "Single MIAP source outline changed"
        if criterion["role"] == "source-single":
            assert raw == bytes.fromhex("01b000b87fff3f31")
            assert len(font.getTableData("cvt ")) >= 32768 * 2
            assert (
                int.from_bytes(
                    font.getTableData("cvt ")[32767 * 2 : 32768 * 2], "big", signed=True
                )
                == glyph.coordinates[0][0]
            )
        elif criterion["role"] == "companion-single":
            assert raw == bytes.fromhex("01b000b802013f31")
            assert (
                int.from_bytes(
                    font.getTableData("cvt ")[513 * 2 : 514 * 2], "big", signed=True
                )
                == glyph.coordinates[0][0]
            )
        elif criterion["role"] == "main-dense":
            raw_cvt = font.getTableData("cvt ")
            assert all(
                int.from_bytes(
                    raw_cvt[(512 - point) * 2 : (513 - point) * 2], "big", signed=True
                )
                == glyph.coordinates[point][0]
                for point in range(256)
            )
        if case["profile"] == "source-index-negative-32768":
            assert len(font.getTableData("cvt ")) >= 32769 * 2
            assert case["unsigned_negative_index_in_range"] is True
            assert raw == bytes.fromhex("01b000b880003f31")
            assert int.from_bytes(raw[4:6], "big", signed=True) == -32768
            try:
                cvt.bounded_program(font, gid)
            except AssertionError as error:
                assert "Negative/oversized operand" in str(error)
            else:
                raise AssertionError("Negative CVT PUSHW accepted independently")
        else:
            parsed, operands = cvt.bounded_program(font, gid)
            assert parsed == raw
            assert (
                sorted({operand["old"] for operand in operands})
                == criterion["source_reference_indices"]
            )
            indices = [row["old"] for row in operands]
            observed.append(
                dict(
                    gid=gid,
                    points=len(glyph.coordinates),
                    instruction_bytes=len(raw),
                    original_indices=indices if len(indices) <= 10 else None,
                    operand_count=len(indices),
                    original_index_bounds=(
                        [min(indices), max(indices)] if indices else []
                    ),
                    operand_index_sequence_sha256=digest(
                        json.dumps(indices, separators=(",", ":")).encode("ascii")
                    ),
                )
            )
    if not case["refused"]:
        expected, uses = expected_usage(inventory, usage, case)
        union = defaultdict(set)
        closed_keys = set()
        for frozen in case["expected_local_closures"]:
            keys = frozen["binding_prefixes"]
            assert all(key in expected and key not in closed_keys for key in keys)
            closed_keys.update(keys)
            original = inventory[keys[0]]
            input_sha = digest(original["data"])
            assert frozen["source_font_sha256"] == input_sha
            roots = {gid for key in keys for _, gid in uses[key]}
            closure = hinting.glyph_closure(original, roots)
            assert sorted(roots) == frozen["expected_root_gids"]
            assert sorted(closure) == frozen["expected_closure_gids"]
            refs = sorted(
                {
                    operand["old"]
                    for gid in closure
                    for operand in cvt.bounded_program(original["sfnt"], gid)[1]
                }
            )
            assert refs == frozen["expected_original_indices"]
            mapping = {old: dense for dense, old in enumerate(refs)}
            assert mapping == {
                int(key): value for key, value in frozen["expected_index_map"].items()
            }
            raw_cvt = original["sfnt"].getTableData("cvt ")
            assert (
                b"".join(raw_cvt[index * 2 : index * 2 + 2] for index in refs).hex()
                == frozen["expected_cvt_hex"]
            )
            for program in frozen["expected_programs"]:
                rewritten = cvt.rewrite_expected(
                    original["sfnt"], program["hint_gid"], mapping
                )
                assert rewritten.hex() == program["program_hex"]
                criterion = next(
                    row
                    for row in case["source_font_glyphs"]
                    if row["hint_gid"] == program["hint_gid"]
                )
                if criterion["role"] == "main-dense":
                    assert len(rewritten) == 1286 and rewritten[1:6] == bytes.fromhex(
                        "b000b0ff3f"
                    )
                    assert rewritten[-5:] == bytes.fromhex(
                        "b801002f31"
                    ), "Real point256 PUSHW rewritten"
                if criterion["role"] == "companion-single":
                    assert rewritten == (
                        bytes.fromhex("01b000b801003f31")
                        if mapping[513] == 256
                        else bytes.fromhex("01b000b0003f31")
                    )
            union[input_sha].update(refs)
        assert closed_keys == set(expected), "Frozen local closure coverage incomplete"
        assert {key: sorted(value) for key, value in union.items()} == {
            row["source_font_sha256"]: row["indices"]
            for row in case["expected_source_reference_union"]
        }
    return dict(
        input_sha256=sha(source),
        frozen_source_glyphs=observed,
        source_position_truth_max_error_pt=position_error,
        baseline_glyph_topology_flags_metrics_exact=True,
        first_on_curve_and_original_257_point_bound_checked=True,
        frozen_local_closures_and_source_union_rederived=not case["refused"],
        signed_negative_semantics_verified=case["profile"]
        == "source-index-negative-32768",
    )


def inspect_control(source, control, case, cvt, hinting):
    from pypdf import PdfReader

    before, _ = hinting.resource_inventory(PdfReader(source))
    after, _ = hinting.resource_inventory(PdfReader(control))
    assert set(before) == set(after)
    control_programs = {
        row["hint_gid"]: bytes.fromhex(row["expected_program_hex"])
        for row in case["control_expected_programs"]
    }
    assert set(control_programs) == set(case["control_target_gids"])
    target_shas = {
        row["font_input_sha256"] for row in case["control_expected_programs"]
    }
    assert len(target_shas) == 1
    target_sha = next(iter(target_shas))
    verified = set()
    for key, original in before.items():
        other = after[key]
        assert original["mappings"] == other["mappings"]
        old_cvt = (
            original["sfnt"].getTableData("cvt ") if "cvt " in original["sfnt"] else b""
        )
        new_cvt = other["sfnt"].getTableData("cvt ") if "cvt " in other["sfnt"] else b""
        assert old_cvt == new_cvt
        for gid in range(len(original["sfnt"].getGlyphOrder())):
            expected = list(hinting.glyph_signature(original, gid))
            if digest(original["data"]) == target_sha and gid in control_programs:
                criterion = next(
                    row
                    for row in case["control_expected_programs"]
                    if row["hint_gid"] == gid
                )
                assert digest(other["data"]) == criterion["control_font_sha256"]
                program, operands = cvt.bounded_program(original["sfnt"], gid)
                assert program.hex() == criterion["source_program_hex"]
                # The frozen control removes precisely a point PUSH, CVT PUSH,
                # and MIAP triple, leaving axis/IUP/other operations unchanged.
                replacements = []
                for operand in operands:
                    end = operand["end"] + 1
                    point_width = len(cvt.encode_push(operand["point"]))
                    start = operand["start"] - point_width
                    candidate = program[:start] + program[end:]
                    if candidate == control_programs[gid]:
                        replacements.append((start, end))
                assert (
                    len(replacements) == 1
                ), "Control is not a single target MIAP removal"
                expected[1] = control_programs[gid]
                verified.add(gid)
            assert tuple(expected) == hinting.glyph_signature(other, gid), (
                "Control changed another glyph or operation",
                key,
                gid,
            )
    assert verified == set(control_programs)
    return dict(
        exact_single_target_Miap_removal=True,
        other_glyphs_CVT_values_outlines_hints_exact=True,
    )


def run_boundary(args):
    assert CRITERIA_PIN is not None, "Criteria not independently frozen"
    cvt, hinting = primitives()
    from pypdf import PdfReader

    criteria = args.fixtures / "criteria.json"
    assert sha(criteria) == CRITERIA_PIN, "Boundary criteria changed"
    fixed = load(criteria)
    assert fixed["position_error_pt"] == 0.5 and fixed["dpi"] == [72, 96, 300]
    assert (
        fixed["outside_pixel_difference"] == 0 and fixed["rectangle_boundary_px"] == 1
    )
    assert (
        fixed["expected_max_stack_elements"] == 2
        and fixed["instruction_bytes_limit"] == 4096
    )
    assert (
        fixed["retain_equal_values_at_distinct_indices"]
        and fixed["local_dense_indices_are_independent_of_same_source_union"]
    )
    for relative, pin in fixed["source_pins"].items():
        assert sha(args.fixtures.parent / relative) == pin
    assert args.source_only or args.run
    process, tests = None, {}
    if not args.source_only:
        names = {"M5RH08_cvt_index_boundaries"}
        if not args.cases_only:
            names.add("M5RH09_cvt_index_boundaries_UI")
        if args.producer_executable:
            process = load(args.run / "process-result.json")
            rows = load(args.run / "selftest.json")["tests"]
            assert (
                process["completed"]
                and process["report_valid"]
                and process["exit_code"] == 0
            )
            assert process["exe_sha256"] == sha(args.producer_executable)
            assert process["test_count"] == len(rows) and all(
                row["status"] == "PASS" for row in rows
            )
            tests = {row["name"]: row for row in rows if row["name"] in names}
            assert set(tests) == names
        else:
            process, tests = hinting.validate_producer(args.run, names)
        hinting.validate_case_outcomes(
            tests["M5RH08_cvt_index_boundaries"]["details"], fixed["cases"]
        )
    engine = None if args.source_only else hinting.poppler_tool("pdftoppm")
    results, observations = [], []
    for case in fixed["cases"]:
        row = dict(file=case["file"], profile=case["profile"], status="FAIL")
        try:
            source = args.fixtures / case["file"]
            assert sha(source) == case["sha256"]
            row.update(inspect_source(source, case, cvt, hinting))
            if case.get("control_file"):
                control = args.fixtures / case["control_file"]
                assert sha(control) == case["control_sha256"]
                row.update(inspect_control(source, control, case, cvt, hinting))
                if not args.source_only:
                    changed = {
                        f"{label}-{dpi}": hinting.pixels(
                            source, control, dpi, 0, None, renderer
                        )
                        for dpi in (72, 96)
                        for label, renderer in (("PDFium", None), ("Poppler", engine))
                    }
                    observations.append(
                        dict(
                            file=case["file"],
                            target_operation_only=True,
                            changed_pixels=changed,
                            effect_per_engine={
                                label: (
                                    "OBSERVED"
                                    if any(
                                        changed[f"{label}-{dpi}"] for dpi in (72, 96)
                                    )
                                    else "UNOBSERVED"
                                )
                                for label in ("PDFium", "Poppler")
                            },
                        )
                    )
            if not args.source_only:
                target = args.run / ("cvt-index-boundary-" + case["file"])
                if case["refused"]:
                    assert not target.exists()
                    row.update(
                        producer_refusal_verified=True, rejected_output_absent=True
                    )
                else:
                    # Legacy primitive performs fixed text/position and raster
                    # comparison; new closure audit supports no-CVT local fonts.
                    before, after = PdfReader(source), PdfReader(target)
                    row.update(audit_closures(before, after, case, cvt, hinting))
                    row.update(cvt.audit_all_streams(before, after, case, target))
                    maximum = 0
                    with cvt.pdfium.PdfDocument(target) as document:
                        assert len(document) == len(before.pages)
                        for page_index in range(len(document)):
                            terms = (
                                case["keep"]
                                if page_index == 0
                                else case["second_page_terms"]
                            )
                            text = document[page_index].get_textpage()
                            content = text.get_text_range()
                            assert (
                                case["remove"] not in content
                                and case["remove"]
                                not in after.pages[page_index].extract_text()
                            )
                            for term in terms:
                                assert (
                                    content.count(term) == 1
                                    and term in after.pages[page_index].extract_text()
                                )
                                boxes = (
                                    case["boxes"]
                                    if len(before.pages) == 1
                                    else case["boxes"][str(page_index)]
                                )
                                start = content.index(term)
                                actual = [
                                    list(text.get_charbox(index))
                                    for index in range(start, start + len(term))
                                ]
                                assert len(actual) == len(boxes[term])
                                maximum = max(
                                    maximum,
                                    max(
                                        abs(a - b)
                                        for position, expected in zip(
                                            actual, boxes[term]
                                        )
                                        for a, b in zip(position, expected)
                                    ),
                                )
                            text.close()
                    assert maximum <= 0.5
                    raster = {
                        f"{label}-{dpi}": [
                            hinting.pixels(
                                source,
                                target,
                                dpi,
                                page,
                                case["rect"] if page == 0 else None,
                                renderer,
                            )
                            for page in range(len(before.pages))
                        ]
                        for dpi in (72, 96, 300)
                        for label, renderer in (("PDFium", None), ("Poppler", engine))
                    }
                    assert all(
                        all(value == 0 for value in values)
                        for values in raster.values()
                    ), raster
                    row.update(
                        output_sha256=sha(target),
                        max_position_error_pt=maximum,
                        raster=raster,
                    )
            row["status"] = "PASS_SOURCE_INSPECTION" if args.source_only else "PASS"
        except Exception as error:
            row.update(error=repr(error), traceback=traceback.format_exc())
        results.append(row)
    if not args.source_only and not args.cases_only:
        ui = dict(file="cvt-index-boundary-ui.pdf", status="FAIL")
        try:
            details = tests["M5RH09_cvt_index_boundaries_UI"]["details"]
            assert (
                details["source_unchanged"] and details["outside_signature_and_reedit"]
            )
            assert details["refusal_UI_atomic"] and details["cancelled"] == 3
            assert details["cancellation_checkpoints"] > 3
            assert not (args.run / "cvt-index-boundary-ui-refused.pdf").exists()
            assert not list(args.run.glob("cvt-index-boundary-cancel-*.pdf"))
            before = args.run / "cvt-index-boundary-ui-before.pdf"
            after = args.run / "cvt-index-boundary-ui.pdf"
            reedited = args.run / "cvt-index-boundary-ui-reedited.pdf"
            assert before.exists() and after.exists() and reedited.exists()
            case = next(
                row for row in fixed["cases"] if row["file"] == "shared-pages-split.pdf"
            )
            raster = {
                f"{label}-{dpi}": [
                    hinting.pixels(
                        before,
                        after,
                        dpi,
                        page,
                        case["rect"] if page == 0 else None,
                        renderer,
                    )
                    for page in (0, 1)
                ]
                for dpi in (72, 96, 300)
                for label, renderer in (("PDFium", None), ("Poppler", engine))
            }
            assert all(value == [0, 0] for value in raster.values())
            ui.update(
                status="PASS",
                producer_atomicity_boolean_checks_verified=True,
                unsaved_signature_pixels_preserved=True,
                raster=raster,
                before_sha256=sha(before),
                after_sha256=sha(after),
                reedited_sha256=sha(reedited),
            )
        except Exception as error:
            ui.update(error=repr(error), traceback=traceback.format_exc())
        results.append(ui)
    return dict(
        status=(
            "PASS_SOURCE_INSPECTION"
            if args.source_only and all(row["status"] != "FAIL" for row in results)
            else ("PASS" if all(row["status"] == "PASS" for row in results) else "FAIL")
        ),
        criteria_sha256=CRITERIA_PIN,
        evaluator_sha256=sha(__file__),
        immutable_primitives=OLD_EVALUATORS,
        source_only=args.source_only,
        cases_only=args.cases_only,
        UI_independent_audit=(
            "UNEXECUTED" if args.cases_only or args.source_only else "EXECUTED"
        ),
        producer_completed=bool(process),
        producer_executable_sha256=process["exe_sha256"] if process else None,
        results=results,
        passed=sum(row["status"] == "PASS" for row in results),
        source_inspections=sum(
            row["status"] == "PASS_SOURCE_INSPECTION" for row in results
        ),
        failed=sum(row["status"] == "FAIL" for row in results),
        target_miap_low_DPI_observations=observations,
        native_external_GUI="UNEXECUTED",
        general_CVT_M1_M3_M5_acceptance="withheld",
    )


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--review-dev40-metadata", action="store_true")
    parser.add_argument("--fixtures", type=Path)
    parser.add_argument("--source-only", action="store_true")
    parser.add_argument(
        "--cases-only",
        action="store_true",
        help="Audit filtered boundary producer only; leave UI independent audit UNEXECUTED",
    )
    parser.add_argument("--run", type=Path)
    parser.add_argument("--producer-executable", type=Path)
    parser.add_argument("--max-report-bytes", type=int, default=150_000)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    assert not args.output.exists(), "Preserve prior reports and failures"
    assert args.review_dev40_metadata or args.fixtures
    try:
        report = metadata_review() if args.review_dev40_metadata else run_boundary(args)
    except Exception as error:
        report = dict(
            status=(
                "FAIL_SCOPED_METADATA_REVIEW" if args.review_dev40_metadata else "FAIL"
            ),
            error=repr(error),
            traceback=traceback.format_exc(),
            required_runtime_pass_count_added=0,
            general_M1_M3_M5_acceptance="withheld",
        )
    report["run_time_utc"] = datetime.datetime.now(datetime.timezone.utc).isoformat()
    limit = min(args.max_report_bytes, 25_000 if args.source_only else 150_000)
    assert 0 < limit <= 150_000
    encoded = (json.dumps(report, ensure_ascii=False, indent=2) + "\n").encode("utf-8")
    if len(encoded) > limit:
        encoded = (
            json.dumps(report, ensure_ascii=False, separators=(",", ":")) + "\n"
        ).encode("utf-8")
    assert len(encoded) <= limit, "Report exceeds reserved cap; no report written"
    with args.output.open("xb") as stream:
        stream.write(encoded)
    print(
        json.dumps(
            dict(
                status=report["status"],
                failed=report.get("failed"),
                error=report.get("error"),
                output=str(args.output),
                bytes=args.output.stat().st_size,
            )
        )
    )
    return int(report["status"].startswith("FAIL"))


if __name__ == "__main__":
    raise SystemExit(main())
