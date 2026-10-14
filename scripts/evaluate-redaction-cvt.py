"""Independently audit resource/GID CVT compaction and bounded MIAP rewriting."""

import argparse
import importlib.util
import io
import json
import re
import traceback
from collections import Counter, defaultdict
from pathlib import Path

import pypdfium2 as pdfium
from pypdf import PdfReader, PdfWriter
from pypdf.generic import (
    ArrayObject,
    DecodedStreamObject,
    DictionaryObject,
    NameObject,
    NumberObject,
    StreamObject,
)
from fontTools.ttLib.tables.DefaultTable import DefaultTable
from fontTools.ttLib.tables.ttProgram import Program

spec = importlib.util.spec_from_file_location(
    "hinting", Path(__file__).with_name("evaluate-redaction-hinting.py")
)
hinting = importlib.util.module_from_spec(spec)
spec.loader.exec_module(hinting)
PIN = "1234729946a784d36e2d32bca4fc1e10642b01c3e900539996f236a440858be7"


def read_push(data, cursor):
    assert cursor < len(data), "Missing PUSH"
    opcode = data[cursor]
    width = {0xB0: 1, 0xB8: 2}.get(opcode)
    assert width is not None and cursor + 1 + width <= len(
        data
    ), "Unknown/truncated PUSH"
    value = int.from_bytes(
        data[cursor + 1 : cursor + 1 + width], "big", signed=width == 2
    )
    assert 0 <= value <= 32767, "Negative/oversized operand"
    assert (width == 1) == (value <= 255), "Noncanonical PUSH"
    return value, cursor + width + 1


def encode_push(value):
    assert 0 <= value <= 32767
    return (
        bytes((0xB0, value)) if value < 256 else bytes((0xB8, value >> 8, value & 255))
    )


def bounded_program(font, gid):
    """Decode source semantics independently, retaining exact CVT operand spans."""
    glyph = font["glyf"][font.getGlyphOrder()[gid]]
    data = bytes(glyph.program.getBytecode()) if hasattr(glyph, "program") else b""
    if not data:
        return data, []
    assert not glyph.isComposite() and glyph.numberOfContours > 0
    assert len(data) <= 4096
    point_count = len(glyph.coordinates)
    cursor, previous_axis, operands = 0, -1, []
    while cursor < len(data):
        axis = data[cursor]
        assert axis in (0, 1) and axis > previous_axis, "Axis order"
        previous_axis, cursor, previous_point, touched = axis, cursor + 1, -1, 0
        while cursor < len(data) and data[cursor] != 0x30 + axis:
            point, cursor = read_push(data, cursor)
            assert previous_point < point < point_count, "Repeated/non-real point"
            previous_point, touched = point, touched + 1
            assert cursor < len(data)
            if data[cursor] == 0x2F:
                cursor += 1
                continue
            start = cursor
            cvt_index, cursor = read_push(data, cursor)
            assert cursor < len(data) and data[cursor] == 0x3F, "Only MIAP[1]"
            assert "cvt " in font, "Missing source CVT"
            cvt_data = font.getTableData("cvt ")
            assert len(cvt_data) % 2 == 0 and cvt_index * 2 + 2 <= len(cvt_data)
            operands.append(
                dict(
                    start=start,
                    end=cursor,
                    old=cvt_index,
                    axis=axis,
                    point=point,
                    fword=int.from_bytes(
                        cvt_data[cvt_index * 2 : cvt_index * 2 + 2], "big", signed=True
                    ),
                )
            )
            cursor += 1
        assert (
            touched and cursor < len(data) and data[cursor] == 0x30 + axis
        ), "Missing IUP"
        cursor += 1
    return data, operands


def rewrite_expected(font, gid, mapping):
    data, operands = bounded_program(font, gid)
    result, cursor = bytearray(), 0
    for operand in operands:
        result.extend(data[cursor : operand["start"]])
        result.extend(encode_push(mapping[operand["old"]]))
        cursor = operand["end"]
    result.extend(data[cursor:])
    return bytes(result)


def expected_signature(record, gid, mapping):
    signature = list(hinting.glyph_signature(record, gid))
    signature[1] = rewrite_expected(record["sfnt"], gid, mapping)
    return tuple(signature)


def inspect_frozen_source(reader, case):
    bindings, _ = hinting.resource_inventory(reader)
    records = {record["identity"]: record for record in bindings.values()}
    observed, referenced = set(), set()
    for criterion in case["source_font_glyphs"]:
        matches = [
            record
            for record in records.values()
            if hinting.partial.sha(record["data"]) == criterion["font_input_sha256"]
        ]
        assert matches, "Frozen source font missing"
        for record in matches:
            font = record["sfnt"]
            gid = criterion["hint_gid"]
            glyph = font["glyf"][font.getGlyphOrder()[gid]]
            assert (criterion["unicode_char"], criterion["original_gid"]) in record[
                "mappings"
            ].values()
            assert len(glyph.coordinates) == criterion["point_count"]
            data, operands = bounded_program(font, gid)
            assert data.hex() == criterion["expected_program_hex"]
            assert font.getTableData("cvt ").hex() == criterion["original_cvt_hex"]
            assert glyph.program.getAssembly(), "Independent fontTools decode"
            referenced.update(operand["old"] for operand in operands)
            observed.add((criterion["font_input_sha256"], gid))
    assert referenced, "Successful source must use CVT"
    return dict(
        frozen_hint_glyphs=len(observed), frozen_reference_indices=sorted(referenced)
    )


def audit_resource_cvt(before, after, case):
    source, source_usage = hinting.resource_inventory(before)
    target, target_usage = hinting.resource_inventory(after)
    expected = {
        key: text.replace(case["remove"], "") if key.startswith("page/0/") else text
        for key, text in source_usage.items()
    }
    expected = {key: text for key, text in expected.items() if text}
    assert set(target_usage) == set(expected), "Resource usage binding changed"
    groups, expected_uses = defaultdict(list), {}
    for key, actual in target_usage.items():
        assert Counter(actual) == Counter(expected[key]), (
            "Resource text-use changed",
            key,
        )
        original = source[key]
        deleted = set()
        if key.startswith("page/0/"):
            for match in re.finditer(re.escape(case["remove"]), source_usage[key]):
                deleted.update(range(match.start(), match.end()))
        expected_uses[key] = [
            entry
            for index, entry in enumerate(original["used_glyphs"])
            if index not in deleted
        ]
        groups[target[key]["identity"]].append(key)
    inspected, known_payloads, cvt_reports = 0, set(), []
    for identity, keys in groups.items():
        retained, original = target[keys[0]], source[keys[0]]
        assert (
            len({hinting.partial.sha(source[key]["data"]) for key in keys}) == 1
        ), "Unrelated fonts merged"
        allowed = set("".join(expected[key] for key in keys))
        assert {character for character, _ in retained["mappings"].values()} == allowed
        old_roots = {gid for key in keys for _, gid in expected_uses[key]}
        new_roots = {gid for _, gid in retained["mappings"].values()}
        old_closure = hinting.glyph_closure(original, old_roots)
        new_closure = hinting.glyph_closure(retained, new_roots)
        old_refs = sorted(
            {
                operand["old"]
                for gid in old_closure
                for operand in bounded_program(original["sfnt"], gid)[1]
            }
        )
        mapping = {old: new for new, old in enumerate(old_refs)}
        original_cvt = original["sfnt"].getTableData("cvt ")
        expected_cvt = b"".join(original_cvt[old * 2 : old * 2 + 2] for old in old_refs)
        if expected_cvt:
            assert (
                "cvt " in retained["sfnt"]
                and retained["sfnt"].getTableData("cvt ") == expected_cvt
            ), ("CVT is not exact referenced-value sequence", keys, old_refs)
        else:
            assert "cvt " not in retained["sfnt"], "Unreferenced CVT retained"
        assert set(range(len(retained["sfnt"].getGlyphOrder()))) == new_closure | {
            0
        }, "Unmapped non-component glyph retained"
        if 0 not in new_closure:
            notdef = retained["sfnt"]["glyf"][retained["sfnt"].getGlyphOrder()[0]]
            assert notdef.numberOfContours == 0 and (
                not hasattr(notdef, "program") or not notdef.program.getBytecode()
            )
        assert Counter(
            expected_signature(original, gid, mapping) for gid in old_closure
        ) == Counter(hinting.glyph_signature(retained, gid) for gid in new_closure), (
            "Glyph/component closure or CVT-only rewriting differs",
            keys,
        )
        original_characters = defaultdict(set)
        for character, gid in original["mappings"].values():
            original_characters[character].add(gid)
        for character, gid in retained["mappings"].values():
            assert character in original_characters
            assert any(
                hinting.glyph_signature(retained, gid)
                == expected_signature(original, old_gid, mapping)
                for old_gid in original_characters[character]
            ), ("Mapped glyph changed", keys, character, gid)
            inspected += 1
        for key in keys:
            assert Counter(
                (character, expected_signature(source[key], gid, mapping))
                for character, gid in expected_uses[key]
            ) == Counter(
                (character, hinting.glyph_signature(target[key], gid))
                for character, gid in target[key]["used_glyphs"]
            ), (
                "Resource/GID actual use changed",
                key,
            )
        actual_refs = sorted(
            {
                operand["old"]
                for gid in new_closure
                for operand in bounded_program(retained["sfnt"], gid)[1]
            }
        )
        assert actual_refs == list(
            range(len(old_refs))
        ), "Dense CVT indexes contain unused entries"
        known_payloads.add(hinting.partial.sha(retained["data"]))
        cvt_reports.append(
            dict(
                bindings=keys,
                old_indices=old_refs,
                dense_map=mapping,
                expected_fwords=[
                    int.from_bytes(expected_cvt[i : i + 2], "big", signed=True)
                    for i in range(0, len(expected_cvt), 2)
                ],
                rewritten_glyph_closure=len(old_closure),
            )
        )
    descriptor_payloads = {
        hinting.partial.sha(value["/FontFile2"].get_data())
        for value in hinting.all_objects(after)
        if isinstance(value, dict) and "/FontFile2" in value
    }
    assert descriptor_payloads == known_payloads, "Unbound embedded font remains"
    assert any(
        report["old_indices"] for report in cvt_reports
    ), "Accepted CVT source must retain MIAP"
    return dict(
        resource_bindings=len(target_usage),
        font_closures=len(groups),
        audited_map_entries=inspected,
        cvt_closures=cvt_reports,
        retained_point_topology_exact=True,
        cvt_rewrite_exact=True,
    )


def audit_all_streams(before, after, case, target):
    hinting.audit_private_streams(before, after, case, target)
    old_font_hashes = set()
    original_cvt = {
        bytes.fromhex(row["original_cvt_hex"])
        for row in case["source_font_glyphs"]
        if row["original_cvt_hex"]
    }
    for value in hinting.all_objects(before):
        if isinstance(value, StreamObject):
            data = value.get_data()
            if data.startswith(b"\0\x01\0\0"):
                old_font_hashes.add(hinting.partial.sha(data))
    decoded_count = 0
    for value in hinting.all_objects(after):
        if isinstance(value, StreamObject):
            data = value.get_data()
            assert (
                hinting.partial.sha(data) not in old_font_hashes
            ), "Original or orphan source font survives"
            for cvt in original_cvt:
                assert cvt not in data, "Whole original CVT survives"
            for marker in case.get("private_markers", []):
                assert (
                    marker.encode() not in data
                ), "Unreferenced/removed CVT marker survives"
            decoded_count += 1
    for marker in case.get("private_markers", []):
        assert marker.encode() not in target.read_bytes()
    return dict(
        decoded_streams_audited=decoded_count, old_cvt_and_private_markers_absent=True
    )


def check_fonts(reader):
    count = hinted = cvt_fonts = 0
    for value in hinting.all_objects(reader):
        if not isinstance(value, dict) or "/FontFile2" not in value:
            continue
        font = hinting.TTFont(hinting.io.BytesIO(value["/FontFile2"].get_data()))
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
            "cvt ",
        }
        assert "prep" not in font or font.getTableData("prep") == hinting.PREP
        decoded = [
            bounded_program(font, gid) for gid in range(len(font.getGlyphOrder()))
        ]
        longest = max((len(data) for data, _ in decoded), default=0)
        uses_cvt = any(operands for _, operands in decoded)
        assert font["maxp"].maxStackElements == (
            2 if uses_cvt else int(bool(longest or "prep" in font))
        )
        assert (
            font["maxp"].maxSizeOfInstructions == longest and font["maxp"].maxZones == 1
        )
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
        assert (
            font.getTableData("maxp") == expected
        ), "maxp geometry differs from actual closure"
        assert ("cvt " in font) == uses_cvt
        hinted += sum(bool(data) for data, _ in decoded)
        cvt_fonts += int(uses_cvt)
        count += 1
    assert count and hinted and cvt_fonts
    return dict(
        fonts=count, hinted_glyphs=hinted, cvt_fonts=cvt_fonts, maxp_recalculated=True
    )


def inspect_control(source, control, case):
    before, _ = hinting.resource_inventory(PdfReader(source))
    after, _ = hinting.resource_inventory(PdfReader(control))
    assert set(before) == set(after)
    records = {row["font_input_sha256"]: row for row in case["source_font_glyphs"]}
    verified = set()
    for key, original in before.items():
        other = after[key]
        criterion = records[hinting.partial.sha(original["data"])]
        assert original["mappings"] == other["mappings"]
        assert original["sfnt"].getTableData("cvt ") == other["sfnt"].getTableData(
            "cvt "
        )
        for gid in range(len(original["sfnt"].getGlyphOrder())):
            expected = list(hinting.glyph_signature(original, gid))
            if gid == criterion["hint_gid"]:
                data, operands = bounded_program(original["sfnt"], gid)
                assert len(operands) == 4
                # Verify removal of point PUSH + CVT PUSH + MIAP leaves axis/IUP.
                assert data[0] == 0 and data[-1] == 0x31
                expected[1] = bytes.fromhex("00300131")
            assert tuple(expected) == hinting.glyph_signature(
                other, gid
            ), "Control alters another outline/hint"
        verified.add(criterion["font_input_sha256"])
    assert verified == set(records)
    return dict(cvt_target_only_miap_removed=True, other_glyph_hints_exact=True)


def inspect_saved(source, target, case, fixed, engine):
    before, after = PdfReader(source), PdfReader(target)
    assert len(after.pages) == len(before.pages)
    maximum = 0
    with pdfium.PdfDocument(target) as document:
        for index in range(len(before.pages)):
            terms = case["keep"] if index == 0 else case["second_page_terms"]
            text = document[index].get_textpage()
            content = text.get_text_range()
            assert (
                case["remove"] not in content
                and case["remove"] not in after.pages[index].extract_text()
            )
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
    resources = audit_resource_cvt(before, after, case)
    streams = audit_all_streams(before, after, case, target)
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
        kept_outline_and_cvt_instructions_exact=True,
        **resources,
        **streams,
        **check_fonts(after),
    )


def font_payload(font):
    data = io.BytesIO()
    font.save(data)
    return data.getvalue()


def replace_font_file(writer, descriptor, font):
    data = font_payload(font)
    stream = DecodedStreamObject()
    stream.set_data(data)
    stream[NameObject("/Length1")] = NumberObject(len(data))
    descriptor[NameObject("/FontFile2")] = writer._add_object(stream.flate_encode())


def replace_raw_cvt(font, data):
    table = DefaultTable("cvt ")
    table.data = data
    font["cvt "] = table


def mutate_first_cvt_font(writer, mode):
    for descriptor in list(writer._objects):
        if not isinstance(descriptor, dict) or "/FontFile2" not in descriptor:
            continue
        font = hinting.TTFont(
            io.BytesIO(descriptor["/FontFile2"].get_data()), recalcTimestamp=False
        )
        if "cvt " not in font:
            continue
        cvt = font.getTableData("cvt ")
        if mode == "unused-value":
            replace_raw_cvt(font, cvt + (31337).to_bytes(2, "big", signed=True))
        else:
            assert mode in ("equal-reference-rebind", "equal-value-dedup")
            assert len(cvt) == 8 and cvt == b"\0" * 8
            references = set()
            for gid, name in enumerate(font.getGlyphOrder()):
                data, operands = bounded_program(font, gid)
                if operands:
                    references.update(operand["old"] for operand in operands)
                    replacement = Program()
                    replacement.fromBytecode(
                        rewrite_expected(
                            font, gid, {operand["old"]: 0 for operand in operands}
                        )
                    )
                    font["glyf"][name].program = replacement
            assert references == {0, 1, 2, 3}
            if mode == "equal-value-dedup":
                replace_raw_cvt(font, b"\0\0")
        replace_font_file(writer, descriptor, font)
        return
    raise AssertionError("No CVT font available for negative mutation")


def mutate_hidden_appearance(writer):
    def streams(value):
        value = value.get_object()
        if isinstance(value, StreamObject):
            yield value
        else:
            for child in value.values():
                yield from streams(child)

    for page_index, page in enumerate(writer.pages):
        for annotation_index, annotation in enumerate(page.get("/Annots", [])):
            appearance = annotation.get_object().get("/AP")
            if appearance is None:
                continue
            appearance = appearance.get_object()
            for kind in ("/R", "/D"):
                if kind not in appearance:
                    continue
                for stream in streams(appearance[kind]):
                    resources = stream["/Resources"]
                    for alias, original_ref in list(resources.get("/Font", {}).items()):
                        original = original_ref.get_object()
                        original_child = (
                            original["/DescendantFonts"][0].get_object()
                            if "/DescendantFonts" in original
                            else original
                        )
                        if "/FontDescriptor" not in original_child:
                            continue
                        original_descriptor = original_child["/FontDescriptor"]
                        if "/FontFile2" not in original_descriptor:
                            continue
                        font = hinting.TTFont(
                            io.BytesIO(original_descriptor["/FontFile2"].get_data()),
                            recalcTimestamp=False,
                        )
                        if "cvt " not in font:
                            continue
                        cvt = bytearray(font.getTableData("cvt "))
                        cvt[1] ^= 1
                        replace_raw_cvt(font, bytes(cvt))
                        descriptor = DictionaryObject(dict(original_descriptor.items()))
                        replace_font_file(writer, descriptor, font)
                        child = DictionaryObject(dict(original_child.items()))
                        child[NameObject("/FontDescriptor")] = writer._add_object(
                            descriptor
                        )
                        if "/DescendantFonts" in original:
                            replacement = DictionaryObject(dict(original.items()))
                            replacement[NameObject("/DescendantFonts")] = ArrayObject(
                                [writer._add_object(child)]
                            )
                        else:
                            replacement = child
                        resources["/Font"][alias] = writer._add_object(replacement)
                        return dict(
                            page=page_index,
                            annotation=annotation_index,
                            appearance_kind=kind,
                            font_alias=str(alias),
                        )
    raise AssertionError("No hidden R/D appearance CVT binding found")


def negative_checks(fixtures, run, destination, fixed, engine):
    """Prove the independent checks reject small copies without altering evidence."""
    root = Path(__file__).resolve().parents[1]
    destination = destination.resolve()
    destination.relative_to(root)
    assert not destination.exists(), "Preserve prior negative fixtures/reports"
    cases = {case["file"]: case for case in fixed["cases"]}
    profiles = [
        (
            "unused-value",
            "removed-private.pdf",
            "CVT is not exact referenced-value sequence",
        ),
        (
            "equal-reference-rebind",
            "equal-fwords.pdf",
            "Glyph/component closure or CVT-only rewriting differs",
        ),
        (
            "equal-value-dedup",
            "equal-fwords.pdf",
            "CVT is not exact referenced-value sequence",
        ),
        (
            "hidden-appearance",
            "shared-appearance-states.pdf",
            "CVT is not exact referenced-value sequence",
        ),
        ("orphan-original-cvt", "removed-private.pdf", "Whole original CVT survives"),
    ]
    destination.mkdir(parents=True)
    results, total = [], 0
    for mode, filename, expected_failure in profiles:
        case = cases[filename]
        source, baseline = fixtures / filename, run / ("cvt-" + filename)
        writer = PdfWriter(clone_from=PdfReader(baseline))
        mutation = {}
        if mode == "hidden-appearance":
            mutation = mutate_hidden_appearance(writer)
        elif mode == "orphan-original-cvt":
            stream = DecodedStreamObject()
            stream.set_data(
                bytes.fromhex(case["source_font_glyphs"][0]["original_cvt_hex"])
            )
            writer._add_object(stream)
        else:
            mutate_first_cvt_font(writer, mode)
        data = io.BytesIO()
        writer.write(data)
        payload = data.getvalue()
        total += len(payload)
        assert (
            len(payload) < 200_000 and total <= 1_500_000
        ), "Small negative fixture reservation exceeded"
        path = destination / (mode + ".pdf")
        path.write_bytes(payload)
        row = dict(
            file=path.name,
            status="FAIL",
            kind="independent-negative-check",
            sha256=hinting.partial.sha(payload),
            bytes=len(payload),
            expected_failure=expected_failure,
            mutation=mutation,
        )
        try:
            before, after = PdfReader(source), PdfReader(path)
            if mode == "hidden-appearance":
                baseline_bindings, _ = hinting.resource_inventory(PdfReader(baseline))
                altered_bindings, _ = hinting.resource_inventory(after)
                assert set(baseline_bindings) == set(altered_bindings)
                changed_bindings = [
                    key
                    for key in baseline_bindings
                    if hinting.partial.sha(baseline_bindings[key]["data"])
                    != hinting.partial.sha(altered_bindings[key]["data"])
                ]
                assert changed_bindings and all(
                    "/AP/" in key and mutation["appearance_kind"] in key
                    for key in changed_bindings
                ), "Mutation also changes a visible font binding"
                row["only_hidden_font_bindings_changed"] = changed_bindings
            if mode == "orphan-original-cvt":
                audit_all_streams(before, after, case, path)
            else:
                audit_resource_cvt(before, after, case)
            raise RuntimeError("Independent audit accepted the corrupted candidate")
        except AssertionError as error:
            if expected_failure in str(error):
                row.update(
                    status="PASS", independent_rejected=True, rejection=str(error)
                )
            else:
                row.update(error=repr(error), traceback=traceback.format_exc())
        except Exception as error:
            row.update(error=repr(error), traceback=traceback.format_exc())
        if mode in ("equal-reference-rebind", "equal-value-dedup", "hidden-appearance"):
            raster = {}
            try:
                for label, renderer in (("PDFium", None), ("Poppler", engine)):
                    differences = [
                        hinting.pixels(baseline, path, 72, index, None, renderer)
                        for index in range(len(PdfReader(baseline).pages))
                    ]
                    assert all(value == 0 for value in differences), (
                        label,
                        differences,
                    )
                    raster[label + "-72"] = differences
                row.update(visible_raster_unchanged=True, raster=raster)
            except Exception as error:
                row.update(
                    status="FAIL", error=repr(error), traceback=traceback.format_exc()
                )
        results.append(row)
    report = dict(
        generated_bytes=total,
        maximum_generated_bytes=1_500_000,
        results=results,
        passed=sum(row["status"] == "PASS" for row in results),
        failed=sum(row["status"] != "PASS" for row in results),
    )
    (destination / "negative-report.json").write_text(
        json.dumps(report, ensure_ascii=False, indent=2) + "\n", "utf-8"
    )
    return results


def validate_cvt_producer(run, executable_override):
    names = {"M5RH05_cvt_references", "M5RH06_cvt_references_UI"}
    if executable_override is None:
        process, tests = hinting.validate_producer(run, names)
        return process, tests, Path(process["executable"])
    process = json.loads((run / "process-result.json").read_bytes())
    assert (
        process["completed"] and process["report_valid"] and process["exit_code"] == 0
    )
    executable = executable_override.resolve()
    assert (
        hinting.partial.sha(executable.read_bytes()) == process["exe_sha256"]
    ), "Preserved producer executable SHA differs"
    rows = json.loads((run / "selftest.json").read_bytes())["tests"]
    matches = {row["name"]: row for row in rows if row["name"] in names}
    assert set(matches) == names and all(
        row["status"] == "PASS" for row in matches.values()
    )
    assert process["test_count"] == len(rows) and all(
        row["status"] == "PASS" for row in rows
    )
    return process, matches, executable


def main():
    evaluator_sha256 = hinting.partial.sha(Path(__file__).read_bytes())
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--fixtures", type=Path, required=True)
    parser.add_argument("--run", type=Path)
    parser.add_argument("--source-only", action="store_true")
    parser.add_argument("--negative-checks-dir", type=Path)
    parser.add_argument("--producer-executable", type=Path)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    assert not args.output.exists(), "Preserve independent prior reports"
    assert args.source_only or args.run, "Product run required"
    assert (
        not args.negative_checks_dir or not args.source_only
    ), "Negative checks require validated product output"
    data = (args.fixtures / "criteria.json").read_bytes()
    assert hinting.partial.sha(data) == PIN
    fixed = json.loads(data)
    assert fixed["position_error_pt"] == 0.5 and fixed["dpi"] == [72, 96, 300]
    assert (
        fixed["outside_pixel_difference"] == 0 and fixed["rectangle_boundary_px"] == 1
    )
    assert (
        fixed["retain_equal_values_at_distinct_indices"]
        and fixed["cvt_requires_retained_miap"]
    )
    process, verified_executable = None, None
    if not args.source_only:
        process, tests, verified_executable = validate_cvt_producer(
            args.run, args.producer_executable
        )
        hinting.validate_case_outcomes(
            tests["M5RH05_cvt_references"]["details"], fixed["cases"]
        )
    engine = hinting.poppler_tool("pdftoppm")
    results = []
    for case in fixed["cases"]:
        row = dict(file=case["file"], status="FAIL")
        try:
            source = args.fixtures / case["file"]
            assert hinting.partial.sha(source.read_bytes()) == case["sha256"]
            if case["refused"]:
                if not args.source_only:
                    assert not (args.run / ("cvt-" + case["file"])).exists()
                row.update(
                    status="PASS",
                    fixed_refusal_input_sha_verified=True,
                    rejected_output_absent=not args.source_only,
                )
            else:
                frozen = inspect_frozen_source(PdfReader(source), case)
                verified = (
                    {}
                    if args.source_only
                    else inspect_saved(
                        source, args.run / ("cvt-" + case["file"]), case, fixed, engine
                    )
                )
                row.update(status="PASS", **frozen, **verified)
        except Exception as error:
            row.update(error=repr(error), traceback=traceback.format_exc())
        results.append(row)
    observation = dict(
        file="sparse-word-control.pdf", status="FAIL", kind="CVT-rendering-observation"
    )
    try:
        case = next(row for row in fixed["cases"] if row.get("control_file"))
        source, control = (
            args.fixtures / case["file"],
            args.fixtures / case["control_file"],
        )
        assert hinting.partial.sha(control.read_bytes()) == case["control_sha256"]
        controlled = inspect_control(source, control, case)
        observable = {
            f"{label}-{dpi}": hinting.pixels(source, control, dpi, 0, None, renderer)
            for dpi in fixed["independent_observability_dpi"]
            for label, renderer in (("PDFium", None), ("Poppler", engine))
        }
        assert any(observable.values()), "No CVT-target MIAP rendering effect observed"
        observation.update(
            status="PASS", hinting_observable_changed_pixels=observable, **controlled
        )
    except Exception as error:
        observation.update(error=repr(error), traceback=traceback.format_exc())
    results.append(observation)
    if not args.source_only:
        ui = dict(file="cvt-ui.pdf", status="FAIL")
        try:
            ui_source, ui_target = (
                args.run / "cvt-ui-before.pdf",
                args.run / "cvt-ui.pdf",
            )
            assert (
                ui_source.exists() and ui_target.exists()
            ), "CVT UI producer outputs missing"
            case = next(
                row for row in fixed["cases"] if row["file"] == "shared-pages.pdf"
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
                            case["rect"] if page == 0 else None,
                            renderer,
                        )
                        for page in (0, 1)
                    ]
                    assert differences == [0, 0], (label, dpi, differences)
                    raster[f"{label}-{dpi}"] = differences
            ui.update(
                status="PASS", raster=raster, unsaved_signature_pixels_preserved=True
            )
        except Exception as error:
            ui.update(error=repr(error), traceback=traceback.format_exc())
        results.append(ui)
        if args.negative_checks_dir:
            try:
                results.extend(
                    negative_checks(
                        args.fixtures, args.run, args.negative_checks_dir, fixed, engine
                    )
                )
            except Exception as error:
                results.append(
                    dict(
                        file="independent-negative-setup",
                        status="FAIL",
                        error=repr(error),
                        traceback=traceback.format_exc(),
                    )
                )
    report = dict(
        criteria_sha256=PIN,
        evaluator_sha256=evaluator_sha256,
        shared_resource_evaluator_sha256=hinting.partial.sha(
            Path(hinting.__file__).read_bytes()
        ),
        resource_audit_schema=3,
        source_only=args.source_only,
        producer_executable_sha256=process["exe_sha256"] if process else None,
        producer_verified_executable=(
            str(verified_executable) if verified_executable else None
        ),
        producer_completed=bool(process),
        results=results,
        passed=sum(row["status"] == "PASS" for row in results),
        failed=sum(row["status"] != "PASS" for row in results),
        engines=["PDFium", "Poppler", "pypdf", "fontTools"],
        general_hinting="unexecuted",
        general_CVT="unexecuted",
        native_external_GUI="unexecuted",
    )
    args.output.write_text(
        json.dumps(report, ensure_ascii=False, indent=2) + "\n", "utf-8"
    )
    print(
        json.dumps(
            dict(
                passed=report["passed"],
                failed=report["failed"],
                source_only=args.source_only,
                output=str(args.output),
            )
        )
    )
    return int(report["failed"] > 0)


if __name__ == "__main__":
    raise SystemExit(main())
