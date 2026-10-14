"""Independent mixed-width CMap oracle; source inspection never claims runtime PASS.

This module uses PDF parsers/fontTools/renderers directly. No product decoder,
serializer, generator helper or previous evaluator is used as a semantic oracle.
"""

import argparse
import ctypes
import datetime
import hashlib
import io
import json
import math
import os
import re
import struct
import subprocess
import traceback
from collections import Counter, defaultdict
from pathlib import Path

import numpy as np
import pypdfium2 as pdfium
import pypdfium2.raw as raw
from fontTools.pens.recordingPen import DecomposingRecordingPen
from fontTools.pens.boundsPen import BoundsPen
from fontTools.pens.transformPen import TransformPen
from fontTools.ttLib import TTFont
from PIL import Image
from pypdf import PdfReader
from pypdf import PdfWriter
from pypdf.generic import (
    ByteStringObject,
    ContentStream,
    DecodedStreamObject,
    IndirectObject,
    NameObject,
    StreamObject,
)
from test_tools import poppler_tool

ROOT = Path(__file__).resolve().parents[1]
CRITERIA_PIN = "fc20b71f3990ffe4fa468389f0867297ebe0fcf2c8aa261de03c694b8f8af118"
CRITERIA_PINS = {
    "mixed-cmap-v1": CRITERIA_PIN,
    "mixed-cmap-v2": "796b7ae0ef979583d0180c91c1020dd30405530d044daf39e83339f0b2ae326a",
}
DECODER_SAFETY_PIN = "5f39b3584811a0411573999bc5e7e0aa7eb241127c1ae0266f6ab87299287efa"
SPACE_SELECTION_PIN = "2168ad0c1ffd1bfb0d618d2068c80265197fb3f8f21e8f2511cf4754c26e7698"
RENDERER_METRICS_PIN = (
    "7d0fdd3d8a1888796566467e385a0d8c11695c9b8b236065ba75241161b75056"
)


def sha(data):
    return hashlib.sha256(data).hexdigest()


def key(value):
    reference = (
        value
        if isinstance(value, IndirectObject)
        else getattr(value, "indirect_reference", None)
    )
    return (
        (reference.idnum, reference.generation) if reference else ("direct", id(value))
    )


def all_objects(reader):
    entries = {
        (number, generation)
        for generation, numbers in reader.xref.items()
        for number in numbers
        if number
    }
    entries.update((number, 0) for number in reader.xref_objStm)
    for number, generation in sorted(entries):
        yield reader.get_object(IndirectObject(number, generation, reader))


def hexbytes(value):
    assert len(value) % 2 == 0, "odd hex value"
    return bytes.fromhex(value.decode())


def in_space(source, space):
    lo, hi = space
    return len(source) == len(lo) and all(
        a <= value <= b for value, a, b in zip(source, lo, hi)
    )


def parse_cmap(value, unicode_map=False):
    value = value.get_object()
    assert isinstance(value, StreamObject)
    assert set(value) <= {
        "/Length",
        "/Filter",
        "/DecodeParms",
        "/Type",
        "/CMapName",
        "/CIDSystemInfo",
        "/WMode",
    }, "unknown CMap stream key"
    assert "/UseCMap" not in value
    data = value.get_data()
    assert (
        b"usecmap" not in data.lower()
        and b"mysterycommand" not in data
        and b"MysteryMetadata" not in data
    ), "unknown/inherited CMap"
    assert data.count(b"begincmap") == data.count(b"endcmap") == 1
    assert not re.search(rb"/WMode\s+(?!0\b)\d+", data), "vertical CMap"
    spaces = []
    for count, section in re.findall(
        rb"(\d+)\s+begincodespacerange(.*?)endcodespacerange", data, re.S
    ):
        pairs = re.findall(rb"<([0-9a-fA-F]+)>\s*<([0-9a-fA-F]+)>", section)
        assert int(count) == len(pairs)
        for lo, hi in pairs:
            lo, hi = hexbytes(lo), hexbytes(hi)
            assert len(lo) == len(hi) and len(lo) in (1, 2)
            assert all(a <= b for a, b in zip(lo, hi)), "nonrectangular codespace"
            for oldlo, oldhi in spaces:
                if len(lo) == len(oldlo):
                    assert any(
                        b < c or d < a for a, b, c, d in zip(lo, hi, oldlo, oldhi)
                    ), "codespace overlap"
                else:
                    assert hi[0] < oldlo[0] or oldhi[0] < lo[0], "prefix ambiguous"
            spaces.append((lo, hi))
    assert spaces, "missing codespace"
    mappings = {}

    def enter(source, target):
        assert source not in mappings, "source redefinition"
        assert any(
            in_space(source, space) for space in spaces
        ), "mapping outside codespace"
        if unicode_map:
            assert len(target) == 2, "non-single BMP mapping"
            target = target.decode("utf-16-be")
            assert len(target) == 1
        else:
            assert 0 <= target <= 65535, "CID overflow"
        mappings[source] = target

    char = b"bfchar" if unicode_map else b"cidchar"
    for count, section in re.findall(
        rb"(\d+)\s+begin" + char + rb"(.*?)end" + char, data, re.S
    ):
        entries = re.findall(
            rb"<([0-9a-fA-F]+)>\s*"
            + (rb"<([0-9a-fA-F]+)>" if unicode_map else rb"(\d+)"),
            section,
        )
        assert int(count) == len(entries)
        for source, target in entries:
            enter(hexbytes(source), hexbytes(target) if unicode_map else int(target))
    kind = b"bfrange" if unicode_map else b"cidrange"
    for count, section in re.findall(
        rb"(\d+)\s+begin" + kind + rb"(.*?)end" + kind, data, re.S
    ):
        expression = rb"<([0-9a-fA-F]+)>\s*<([0-9a-fA-F]+)>\s*" + (
            rb"(?:\[(.*?)\]|<([0-9a-fA-F]+)>)" if unicode_map else rb"(\d+)"
        )
        entries = re.findall(expression, section, re.S)
        assert int(count) == len(entries)
        for entry in entries:
            lo, hi = hexbytes(entry[0]), hexbytes(entry[1])
            assert len(lo) == len(hi) and int.from_bytes(hi, "big") >= int.from_bytes(
                lo, "big"
            )
            length = int.from_bytes(hi, "big") - int.from_bytes(lo, "big") + 1
            if unicode_map:
                values = (
                    [
                        hexbytes(value)
                        for value in re.findall(rb"<([0-9a-fA-F]+)>", entry[2])
                    ]
                    if entry[2]
                    else [
                        (int(entry[3], 16) + index).to_bytes(2, "big")
                        for index in range(length)
                    ]
                )
                assert len(values) == length
            else:
                values = [int(entry[2]) + index for index in range(length)]
            for index, target in enumerate(values):
                enter(
                    (int.from_bytes(lo, "big") + index).to_bytes(len(lo), "big"), target
                )
    assert mappings, "empty mapping"
    return spaces, mappings


def decode(encoded, spaces):
    result, offset = [], 0
    while offset < len(encoded):
        candidates = [
            encoded[offset : offset + size]
            for size in (1, 2)
            if offset + size <= len(encoded)
            and any(
                in_space(encoded[offset : offset + size], space) for space in spaces
            )
        ]
        assert len(candidates) == 1, (
            "truncated, out-of-codespace or ambiguous",
            offset,
            encoded.hex(),
        )
        result.append(candidates[0])
        offset += len(candidates[0])
    return result


def source_font(reader):
    font = next(
        value.get_object()
        for value in reader.pages[0]["/Resources"]["/Font"].values()
        if "/ToUnicode" in value.get_object()
    )
    child = font["/DescendantFonts"][0].get_object()
    spaces, encoding = parse_cmap(font["/Encoding"])
    unicode_spaces, unicode = parse_cmap(font["/ToUnicode"], True)
    assert set(encoding) == set(unicode), "Encoding/ToUnicode key sets differ"
    cid_unicode = {}
    for source, cid in encoding.items():
        assert (
            cid not in cid_unicode or cid_unicode[cid] == unicode[source]
        ), "CID conflicting Unicode"
        cid_unicode[cid] = unicode[source]
    gidmap = child["/CIDToGIDMap"].get_data()
    data = child["/FontDescriptor"]["/FontFile2"].get_data()
    return font, child, spaces, unicode_spaces, encoding, unicode, gidmap, data


def inspect_source(source, case):
    assert sha(source.read_bytes()) == case["sha256"]
    reader = PdfReader(source)
    font, child, spaces, unicode_spaces, encoding, unicode, gidmap, data = source_font(
        reader
    )
    assert sha(font["/Encoding"].get_data()) == case["encoding_sha256"]
    assert sha(font["/ToUnicode"].get_data()) == case["tounicode_sha256"]
    assert sha(data) == case["source_sfnt_sha256"]
    assert {
        (value["byte_count"], value["value"], value["cid"], value["gid"])
        for value in case["encoding_route"]
    } == {
        (
            len(source),
            int.from_bytes(source, "big"),
            cid,
            int.from_bytes(gidmap[cid * 2 : cid * 2 + 2], "big"),
        )
        for source, cid in encoding.items()
    }
    assert {
        (value["byte_count"], value["value"], value["unicode"])
        for value in case["unicode_route"]
    } == {
        (len(source), int.from_bytes(source, "big"), cp)
        for source, cp in unicode.items()
    }
    codes = decode(bytes.fromhex(case["text_bytes_hex"]), spaces)
    assert codes == decode(bytes.fromhex(case["text_bytes_hex"]), unicode_spaces)
    expected = case["expected_decomposition"]
    assert [value.hex() for value in codes] == [value["code_hex"] for value in expected]
    assert all(
        encoding[source] > 0 and unicode[source] != "\0" for source in codes
    ), "CID0 or Unicode NUL used"
    assert [unicode[value] for value in codes] == [
        value["unicode"] for value in expected
    ]
    assert [encoding[value] for value in codes] == [value["cid"] for value in expected]
    assert [
        int.from_bytes(gidmap[encoding[value] * 2 : encoding[value] * 2 + 2], "big")
        for value in codes
    ] == [value["gid"] for value in expected]
    # Independently inspect effective spacing, including empty text operations.
    current_font, tw, tc, hscale, x, y, line_y = None, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0
    custom_name = next(
        name
        for name, value in reader.pages[0]["/Resources"]["/Font"].items()
        if value.get_object() == font
    )
    actual = []
    widths = {
        int(child["/W"][0]) + index: float(width)
        for index, width in enumerate(child["/W"][1])
    }
    for arguments, operator in ContentStream(
        reader.pages[0].get_contents(), reader
    ).operations:
        if operator == b"Tf":
            current_font, size = arguments[0], float(arguments[1])
        elif operator == b"Tw":
            tw = float(arguments[0])
        elif operator == b"Tc":
            tc = float(arguments[0])
        elif operator == b"Tz":
            hscale = float(arguments[0]) / 100
        elif operator == b"TL":
            leading = float(arguments[0])
        elif operator == b"Tm":
            x, y = float(arguments[4]), float(arguments[5])
            line_y = y
        elif operator in (b"Tj", b"TJ", b"'", b'"') and current_font == custom_name:
            if operator in (b"'", b'"'):
                y = line_y - leading
                line_y = y
                x = 60.0
            if operator == b'"':
                tw, tc = float(arguments[0]), float(arguments[1])
            assert not (
                tw and any(len(lo) == 1 for lo, hi in spaces)
            ), "effective nonzero Tw"
            items = arguments[0] if operator == b"TJ" else [arguments[-1]]
            for item in items:
                if hasattr(item, "original_bytes"):
                    for source_code in decode(item.original_bytes, spaces):
                        assert source_code in encoding and source_code in unicode
                        cid = encoding[source_code]
                        advance = (
                            widths[cid] * size / 1000
                            + tc
                            + (tw if source_code == b"\x20" else 0)
                        ) * hscale
                        actual.append(
                            dict(
                                code_hex=source_code.hex(),
                                origin_pt=[x, y],
                                advance_pt=advance,
                            )
                        )
                        x += advance
                else:
                    x -= float(item) * size / 1000 * hscale
    assert len(actual) == len(expected)
    for actual, truth in zip(actual, expected):
        assert actual["code_hex"] == truth["code_hex"]
        assert (
            max(abs(a - b) for a, b in zip(actual["origin_pt"], truth["origin_pt"]))
            < 1e-9
        )
        assert abs(actual["advance_pt"] - truth["advance_pt"]) < 1e-9
    return dict(
        code_count=len(codes),
        separate_encoding_unicode_routes=True,
        independent_origin_arithmetic=True,
        source_sfnt_sha256=sha(data),
        external_source_Unicode_oracle=False,
    )


def source_report(args, fixed):
    rows = []
    base = PdfReader(ROOT / fixed["original_source"]["path"])
    assert (
        sha((ROOT / fixed["original_source"]["path"]).read_bytes())
        == fixed["original_source"]["sha256"]
    )
    first = next(
        value.get_object()
        for value in base.pages[0]["/Resources"]["/Font"].values()
        if "/ToUnicode" in value.get_object()
    )
    payload = first["/DescendantFonts"][0]["/FontDescriptor"]["/FontFile2"].get_data()
    assert sha(payload) == fixed["original_source"]["sfnt_sha256"]
    font = TTFont(io.BytesIO(payload))
    for row in fixed["source_glyphs"]:
        name = font.getGlyphOrder()[row["gid"]]
        glyph = font["glyf"][name]
        coordinates, endpoints, flags = glyph.getCoordinates(font["glyf"])
        assert [list(point) for point in coordinates] == row["coordinates"]
        assert (
            list(endpoints) == row["endpoints"]
            and bytes(flags).hex() == row["flags_hex"]
        )
        assert list(font["hmtx"][name]) == row["metrics"]
        assert (
            bytes(glyph.program.getBytecode()) if hasattr(glyph, "program") else b""
        ).hex() == row["instruction_hex"]
        assert (
            [font.getGlyphID(component.glyphName) for component in glyph.components]
            if glyph.isComposite()
            else []
        ) == row["components"]
    assert (
        sha((ROOT / fixed["license"]["path"]).read_bytes())
        == fixed["license"]["sha256"]
    )
    assert (
        sha((ROOT / "assets/fonts/NotoSansJP.ttf").read_bytes())
        == fixed["license"]["source_font_sha256"]
    )
    assert len({case["file"].casefold() for case in fixed["cases"]}) == len(
        fixed["cases"]
    )
    for case in fixed["cases"]:
        # Frozen integrity is a guard, not an expected source rejection.
        assert sha((args.fixtures / case["file"]).read_bytes()) == case["sha256"]
        row = dict(file=case["file"], status="FAIL", expected_refusal=case["refused"])
        try:
            audit = inspect_source(args.fixtures / case["file"], case)
            assert not case[
                "refused"
            ], "refusal input unexpectedly satisfies limited source contract"
            records, uses = inventory(PdfReader(args.fixtures / case["file"]))
            assert {
                binding: "".join(cp for cp, cid, gid in entries)
                for binding, entries in uses.items()
            } == {row["path"]: row["text"] for row in case["font_bindings"]}
            if fixed["version"] == 2:
                audit["independent_true_outline_geometry"] = inspect_v2_geometry(
                    args.fixtures / case["file"], case
                )
                policy = renderer_metrics_policy(args, fixed)[case["file"]]
                policy_rows = {item["binding"]: item for item in policy["resources"]}
                assert set(policy_rows) == set(uses)
                for binding in uses:
                    condition = policy_rows[binding]
                    record = records[binding]
                    box = [
                        getattr(record["sfnt"]["head"], field)
                        for field in ("xMin", "yMin", "xMax", "yMax")
                    ]
                    assert (
                        box
                        == condition["source_head_bbox"]
                        == condition["expected_output_head_bbox"]
                    )
                    assert (
                        struct.pack(">4h", *box).hex()
                        == condition["source_head_bbox_bytes_hex"]
                        == condition["expected_output_head_bbox_bytes_hex"]
                    )
                    assert sha(record["payload"]) == condition["original_SFNT_sha256"]
                    assert all(
                        isinstance(value, int) and -32768 <= value <= 32767
                        for value in box
                    )
                    assert box[0] <= box[2] and box[1] <= box[3]
                    for glyph in condition["enclosed_nonempty_glyph_bounds"]:
                        original = record["sfnt"]["glyf"][
                            record["sfnt"].getGlyphOrder()[glyph["source_gid"]]
                        ]
                        bounds = [
                            getattr(original, field)
                            for field in ("xMin", "yMin", "xMax", "yMax")
                        ]
                        assert bounds == glyph["raw_glyf_bbox"]
                        assert (
                            box[0] <= bounds[0] <= bounds[2] <= box[2]
                            and box[1] <= bounds[1] <= bounds[3] <= box[3]
                        )
                audit["independent_renderer_metrics_policy_source_only"] = dict(
                    policy_sha256=RENDERER_METRICS_PIN,
                    bindings=len(uses),
                    original_signed_global_bounds_and_retained_outline_enclosure_verified=True,
                    product_runtime="UNEXECUTED",
                )
            row.update(status="PASS_SOURCE_INSPECTION", **audit)
        except Exception as error:
            if case["refused"]:
                row.update(
                    status="PASS_SOURCE_REJECTION_INSPECTION",
                    source_rejection=repr(error),
                    product_refusal="UNEXECUTED",
                )
            else:
                row.update(error=repr(error), traceback=traceback.format_exc())
        rows.append(row)
    return dict(
        status=(
            "PASS_SOURCE_INSPECTION"
            if all(row["status"] != "FAIL" for row in rows)
            else "FAIL"
        ),
        source_only=True,
        results=rows,
        passed=0,
        source_inspections=sum(row["status"] != "FAIL" for row in rows),
        failed=sum(row["status"] == "FAIL" for row in rows),
        source_glyph_topology_flags_metrics_instructions_fixed=True,
        source_license_SHA_verified=True,
        runtime_acceptance="UNEXECUTED",
        general_M1_M3_M5_acceptance="withheld",
    )


def inspect_v2_geometry(path, case):
    reader = PdfReader(path)
    _, _, _, _, encoding, unicode, gidmap, payload = source_font(reader)
    font = TTFont(io.BytesIO(payload))
    glyphs = font.getGlyphSet()
    factor = case["font_size_pt"] / font["head"].unitsPerEm
    true_font_bounds = {}

    def bounds(gid, x, y, horizontal):
        pen = BoundsPen(glyphs)
        glyphs[font.getGlyphOrder()[gid]].draw(pen)
        assert pen.bounds
        true_font_bounds[str(gid)] = list(pen.bounds)
        lo, bottom, hi, top = pen.bounds
        return [
            x + lo * factor * horizontal,
            y + bottom * factor,
            x + hi * factor * horizontal,
            y + top * factor,
        ]

    primary, kept, removed = [], [], []
    for entry in case["expected_decomposition"]:
        code = bytes.fromhex(entry["code_hex"])
        cid = encoding[code]
        gid = int.from_bytes(gidmap[cid * 2 : cid * 2 + 2], "big")
        assert gid == entry["gid"] and unicode[code] == entry["unicode"]
        glyph = font["glyf"][font.getGlyphOrder()[gid]]
        if glyph.numberOfContours == 0:
            primary.append(None)
            continue
        box = bounds(gid, *entry["origin_pt"], case["horizontal_scale"])
        primary.append(box)
        (removed if entry["removed"] else kept).append(box)
    oracle = case["geometry_oracle"]
    assert primary == oracle["primary_glyph_bounds_pt"]
    assert oracle["margin_pt"] == 1.0
    minimum = [min(box[0] for box in removed), min(box[1] for box in removed)]
    maximum = [max(box[2] for box in removed), max(box[3] for box in removed)]
    expected = [
        minimum[0] - 1,
        minimum[1] - 1,
        maximum[0] - minimum[0] + 2,
        maximum[1] - minimum[1] + 2,
    ]
    assert expected == case["rect"], "true outline plus exact 1pt margin rectangle"
    x, y, width, height = expected
    assert all(
        box[2] <= x or x + width <= box[0] or box[3] <= y or y + height <= box[1]
        for box in kept
    )
    phrase = "".join(entry["unicode"] for entry in case["expected_decomposition"])
    for term in ("公開", "文字保持"):
        first = phrase.index(term)
        assert primary[first : first + len(term)] == case["boxes"]["0"][term]
    if case["second_page_terms"]:
        stream = reader.pages[1].get_contents()
        if case["file"] == "shared-sibling-form.pdf":
            stream = reader.pages[1]["/Resources"]["/XObject"]["/Outside"].get_object()
        operations = ContentStream(stream, reader).operations
        matrices = [
            arguments for arguments, operator in operations if operator == b"Tm"
        ]
        sizes = [
            float(arguments[1])
            for arguments, operator in operations
            if operator == b"Tf"
        ]
        assert len(matrices) == len(sizes) == 1
        assert [float(value) for value in matrices[0]] == [
            1,
            0,
            0,
            1,
            60,
            500,
        ] and sizes[0] == 22
        assert not any(
            operator in (b"Tc", b"Tw", b"Tz", b"TJ", b"'", b'"')
            for _, operator in operations
        )
        _, uses = inventory(reader)
        binding = next(
            binding
            for binding in uses
            if binding.startswith("page/1/font") or binding.startswith("page/1/form")
        )
        phrase = "".join(cp for cp, cid, gid in uses[binding])
        child = next(
            value.get_object()
            for value in reader.pages[0]["/Resources"]["/Font"].values()
            if "/ToUnicode" in value.get_object()
        )["/DescendantFonts"][0].get_object()
        widths = {
            int(child["/W"][0]) + index: float(value)
            for index, value in enumerate(child["/W"][1])
        }
        secondary, x = [], 60.0
        for cp, cid, gid in uses[binding]:
            glyph = font["glyf"][font.getGlyphOrder()[gid]]
            secondary.append(
                None if glyph.numberOfContours == 0 else bounds(gid, x, 500.0, 1)
            )
            x += widths[cid] * 22 / 1000
        for term in case["second_page_terms"]:
            first = phrase.index(term)
            assert secondary[first : first + len(term)] == case["boxes"]["1"][term]
    assert true_font_bounds == {
        key: value
        for key, value in oracle["font_glyph_true_bounds"].items()
        if value is not None
    }
    return dict(
        actual_Encoding_CID_GID_and_ToUnicode_routes_verified=True,
        glyph_Bezier_bounds_and_positions_rederived=True,
        removed_outlines_inside_exact_1pt_margin=True,
        kept_outlines_outside=True,
        source_PDFium_positions_used=False,
    )


def font_record(value, output=False):
    font = value.get_object()
    if "/ToUnicode" not in font or "/DescendantFonts" not in font:
        return None
    child = font["/DescendantFonts"][0].get_object()
    descriptor = child["/FontDescriptor"].get_object()
    payload = descriptor["/FontFile2"].get_data()
    sfnt = TTFont(io.BytesIO(payload))
    unicode_spaces, unicode = parse_cmap(font["/ToUnicode"], True)
    encoding_object = font["/Encoding"]
    if str(encoding_object) == "/Identity-H":
        spaces = [(bytes(2), b"\xff\xff")]
        encoding = {code: int.from_bytes(code, "big") for code in unicode}
        assert all(len(code) == 2 for code in unicode)
    else:
        assert not output, "output must use fresh Identity-H"
        spaces, encoding = parse_cmap(encoding_object)
        assert set(encoding) == set(unicode)
    cidmap = child.get("/CIDToGIDMap")
    cidmap = (
        None
        if cidmap is None or str(cidmap) == "/Identity"
        else cidmap.get_object().get_data()
    )
    entries = {}
    for code, cp in unicode.items():
        cid = encoding[code]
        gid = (
            cid
            if cidmap is None
            else int.from_bytes(cidmap[cid * 2 : cid * 2 + 2], "big")
        )
        if cp == "\0":
            assert cid == gid == 0
            continue
        assert cid and gid and gid < len(sfnt.getGlyphOrder()), (code.hex(), cid, gid)
        entries[code] = (cp, cid, gid)
    return dict(
        identity=key(value),
        payload_identity=key(descriptor.raw_get("/FontFile2")),
        tounicode_identity=key(font.raw_get("/ToUnicode")),
        payload=payload,
        sfnt=sfnt,
        spaces=spaces,
        unicode_spaces=unicode_spaces,
        entries=entries,
    )


def inventory(reader, output=False):
    records, uses = {}, defaultdict(list)

    def inspect(content, resources, path, active):
        resources = resources.get_object()
        local = {}
        for name, value in resources.get("/Font", {}).items():
            record = font_record(value, output)
            if record:
                binding = path + "/font" + str(name)
                records[binding], local[str(name)] = record, binding
        current, stack, ordinal = None, [], 0
        for arguments, operator in ContentStream(content, reader).operations:
            if operator == b"q":
                stack.append(current)
            elif operator == b"Q":
                assert stack
                current = stack.pop()
            elif operator == b"Tf":
                current = local.get(str(arguments[0]))
            elif operator in (b"Tj", b"TJ", b"'", b'"') and current:
                items = arguments[0] if operator == b"TJ" else [arguments[-1]]
                record = records[current]
                for item in items:
                    if hasattr(item, "original_bytes"):
                        for source in decode(item.original_bytes, record["spaces"]):
                            assert source in record["entries"]
                            uses[current].append(record["entries"][source])
            elif operator == b"Do":
                form = resources["/XObject"][arguments[0]].get_object()
                if form.get("/Subtype") == "/Form":
                    identity = key(form)
                    assert identity not in active, "cyclic Form"
                    inspect(
                        form,
                        form.get("/Resources", resources),
                        path + "/form/" + str(ordinal),
                        active | {identity},
                    )
                    ordinal += 1

    def appearance(value, resources, path, active):
        value = value.get_object()
        if isinstance(value, StreamObject):
            inspect(value, value.get("/Resources", resources), path, active)
        else:
            for state, child in value.items():
                appearance(
                    child, resources, path + "/" + str(state).lstrip("/"), active
                )

    for index, page in enumerate(reader.pages):
        resources = page["/Resources"]
        inspect(page.get_contents(), resources, "page/" + str(index), set())
        for number, reference in enumerate(page.get("/Annots", [])):
            for state, value in reference.get_object().get("/AP", {}).items():
                appearance(
                    value,
                    resources,
                    f"page/{index}/AP/{number}/" + str(state).lstrip("/"),
                    set(),
                )
    return records, dict(uses)


def glyph_signature(record, gid):
    sfnt = record["sfnt"]
    name = sfnt.getGlyphOrder()[gid]
    glyph = sfnt["glyf"][name]
    pen = DecomposingRecordingPen(sfnt.getGlyphSet())
    sfnt.getGlyphSet()[name].draw(pen)
    coordinates, endpoints, flags = glyph.getCoordinates(sfnt["glyf"])
    component_metadata = []
    if glyph.isComposite():
        for component in glyph.components:
            _, transform = component.getComponentInfo()
            component_metadata.append((component.flags, tuple(transform)))
    return (
        repr(pen.value),
        tuple(tuple(point) for point in coordinates),
        tuple(endpoints),
        bytes(flags),
        sfnt["hmtx"][name],
        bytes(glyph.program.getBytecode()) if hasattr(glyph, "program") else b"",
        tuple(component_metadata),
    )


def glyph_closure(record, roots):
    sfnt, active, complete = record["sfnt"], set(), set()

    def visit(gid):
        assert gid not in active, "cyclic composite"
        if gid in complete:
            return
        active.add(gid)
        glyph = sfnt["glyf"][sfnt.getGlyphOrder()[gid]]
        if glyph.isComposite():
            for component in glyph.components:
                visit(sfnt.getGlyphID(component.glyphName))
        active.remove(gid)
        complete.add(gid)

    for gid in roots:
        visit(gid)
    return complete


def sfnt_checks(record):
    payload, font = record["payload"], record["sfnt"]
    padded = payload + bytes((-len(payload)) % 4)
    assert (
        sum(struct.unpack(">" + "I" * (len(padded) // 4), padded)) & 0xFFFFFFFF
        == 0xB1B0AFBA
    ), "SFNT checksumAdjustment"
    tables = struct.unpack_from(">H", payload, 4)[0]
    for index in range(tables):
        tag, expected, offset, length = struct.unpack_from(
            ">4sIII", payload, 12 + index * 16
        )
        table = bytearray(payload[offset : offset + length])
        if tag == b"head":
            table[8:12] = bytes(4)
        table.extend(bytes((-len(table)) % 4))
        checksum = sum(struct.unpack(">" + "I" * (len(table) // 4), table)) & 0xFFFFFFFF
        assert checksum == expected, ("table checksum", tag)
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
    }, "unqualified font table"
    longest = max(
        (
            len(bytes(font["glyf"][name].program.getBytecode()))
            for name in font.getGlyphOrder()
            if hasattr(font["glyf"][name], "program")
        ),
        default=0,
    )
    assert font["maxp"].maxSizeOfInstructions == longest
    assert font["maxp"].maxZones == 1
    assert all(
        getattr(font["maxp"], item) == 0
        for item in (
            "maxTwilightPoints",
            "maxStorage",
            "maxFunctionDefs",
            "maxInstructionDefs",
        )
    )
    baseline = font.getTableData("maxp")
    font["maxp"].recalc(font)
    assert baseline == font.getTableData("maxp")
    return dict(
        checksums_verified=True,
        glyph_count=len(font.getGlyphOrder()),
        max_instruction_bytes=longest,
    )


def retained_uses(uses, case):
    result = {}
    for binding, entries in uses.items():
        text = "".join(cp for cp, cid, gid in entries)
        removed = set()
        if binding.startswith("page/0/"):
            for match in re.finditer(re.escape(case["remove"]), text):
                removed.update(range(match.start(), match.end()))
        actual = [entry for index, entry in enumerate(entries) if index not in removed]
        if actual:
            result[binding] = actual
    return result


def audit_resources(
    before, after, case, expected_uses_override=None, renderer_metrics=None
):
    source, original_uses = inventory(before)
    target, actual_uses = inventory(after, True)
    expected_uses = (
        retained_uses(original_uses, case)
        if expected_uses_override is None
        else expected_uses_override
    )
    assert set(actual_uses) == set(expected_uses), (
        "resource binding uses changed",
        set(actual_uses),
        set(expected_uses),
    )
    frozen_bindings = {row["path"]: row for row in case["font_bindings"]}
    assert set(original_uses) == set(frozen_bindings), "unfixed source binding"
    for binding, entries in original_uses.items():
        assert (
            "".join(cp for cp, cid, gid in entries) == frozen_bindings[binding]["text"]
        )
    font_groups, payload_groups = defaultdict(list), defaultdict(list)
    for binding, entries in actual_uses.items():
        assert "".join(cp for cp, cid, gid in entries) == "".join(
            cp for cp, cid, gid in expected_uses[binding]
        ), ("Unicode meaning/order", binding)
        assert [
            (cp, glyph_signature(target[binding], gid)) for cp, cid, gid in entries
        ] == [
            (cp, glyph_signature(source[binding], gid))
            for cp, cid, gid in expected_uses[binding]
        ], (
            "actual GID outline/flags/metrics/instructions",
            binding,
        )
        font_groups[target[binding]["identity"]].append(binding)
        payload_groups[target[binding]["payload_identity"]].append(binding)
    inspected_entries = 0
    for bindings in font_groups.values():
        record = target[bindings[0]]
        allowed = {
            cp for binding in bindings for cp, cid, gid in expected_uses[binding]
        }
        assert {cp for cp, cid, gid in record["entries"].values()} == allowed, (
            "font mapping contains unused/missing Unicode",
            bindings,
        )
        for cp, cid, gid in record["entries"].values():
            originals = [
                (source[binding], original_gid)
                for binding in bindings
                for source_cp, source_cid, original_gid in source[binding][
                    "entries"
                ].values()
                if source_cp == cp
            ]
            assert any(
                glyph_signature(record, gid) == glyph_signature(original, original_gid)
                for original, original_gid in originals
            ), ("map Unicode/GID route changed", cp)
            inspected_entries += 1
    reports, source_union, target_union, known = (
        [],
        defaultdict(set),
        defaultdict(set),
        set(),
    )
    for bindings in payload_groups.values():
        record = target[bindings[0]]
        source_shas = {sha(source[binding]["payload"]) for binding in bindings}
        assert len(source_shas) == 1, "different source SFNT payloads merged"
        source_sha = next(iter(source_shas))
        original = source[bindings[0]]
        if renderer_metrics is not None:
            fixed_resources = {
                row["binding"]: row for row in renderer_metrics["resources"]
            }
            for binding in bindings:
                condition = fixed_resources[binding]
                assert condition["original_SFNT_sha256"] == sha(
                    source[binding]["payload"]
                )
                original_box = [
                    getattr(source[binding]["sfnt"]["head"], field)
                    for field in ("xMin", "yMin", "xMax", "yMax")
                ]
                output_box = [
                    getattr(record["sfnt"]["head"], field)
                    for field in ("xMin", "yMin", "xMax", "yMax")
                ]
                assert original_box == condition["source_head_bbox"]
                assert output_box == condition["expected_output_head_bbox"], (
                    "frozen source global rendering bounds",
                    binding,
                    output_box,
                )
        old_roots = {
            gid for binding in bindings for cp, cid, gid in expected_uses[binding]
        }
        new_roots = {
            gid for binding in bindings for cp, cid, gid in actual_uses[binding]
        }
        old_closure = glyph_closure(original, old_roots)
        new_closure = glyph_closure(record, new_roots)
        assert set(range(len(record["sfnt"].getGlyphOrder()))) == new_closure | {
            0
        }, "orphan/unused GID remains"
        if 0 not in new_closure:
            notdef = record["sfnt"]["glyf"][record["sfnt"].getGlyphOrder()[0]]
            assert notdef.numberOfContours == 0 and (
                not hasattr(notdef, "program") or not notdef.program.getBytecode()
            )
        assert Counter(
            glyph_signature(original, gid) for gid in old_closure
        ) == Counter(
            glyph_signature(record, gid) for gid in new_closure
        ), "local FontFile2 glyph/component closure changed"
        source_union[source_sha].update(old_closure)
        # Resolve output glyphs to explicit source signatures; do not infer CP from GID.
        recovered = set()
        for gid in new_closure:
            matching = {
                old
                for old in old_closure
                if glyph_signature(record, gid) == glyph_signature(original, old)
            }
            assert matching
            recovered.update(matching)
        target_union[source_sha].update(recovered)
        known.add(sha(record["payload"]))
        reports.append(
            dict(
                bindings=bindings,
                original_sha256=source_sha,
                output_sha256=sha(record["payload"]),
                source_root_gids=sorted(old_roots),
                source_closure_gids=sorted(old_closure),
                output_root_gids=sorted(new_roots),
                output_closure_gids=sorted(new_closure),
                source_global_head_bbox=[
                    getattr(original["sfnt"]["head"], field)
                    for field in ("xMin", "yMin", "xMax", "yMax")
                ],
                output_global_head_bbox=[
                    getattr(record["sfnt"]["head"], field)
                    for field in ("xMin", "yMin", "xMax", "yMax")
                ],
                **sfnt_checks(record),
            )
        )
    assert (
        source_union == target_union
    ), "same original SFNT final reachable GID union changed"
    all_payloads = {
        sha(value["/FontFile2"].get_data())
        for value in all_objects(after)
        if isinstance(value, dict) and "/FontFile2" in value
    }
    assert all_payloads == known, "unbound/orphan FontFile2 remains"
    assert {record["payload_identity"] for record in target.values()} <= set(
        payload_groups
    ), "unbound resource FontFile2 remains"
    known_font_objects = {target[binding]["identity"] for binding in actual_uses}
    all_font_objects = {
        key(value)
        for value in all_objects(after)
        if isinstance(value, dict) and value.get("/Subtype") == "/Type0"
    }
    assert (
        all_font_objects == known_font_objects
    ), "orphan/unused Type0 declaration remains"
    return dict(
        resource_bindings=len(actual_uses),
        mapped_entries=inspected_entries,
        local_fonts=reports,
        source_SFNT_final_reference_union={
            digest: sorted(gids) for digest, gids in source_union.items()
        },
        independent_encoding_and_unicode_routes=True,
        outlines_flags_metrics_instructions_exact=True,
    )


def audit_deleted_data(before, after, target, case):
    forbidden = {sha(before.pages[0].get_contents().get_data())}
    for value in all_objects(before):
        if isinstance(value, dict):
            for name in ("/Encoding", "/ToUnicode", "/FontFile2", "/CIDToGIDMap"):
                item = value.get(name)
                if item is not None and isinstance(item.get_object(), StreamObject):
                    forbidden.add(sha(item.get_object().get_data()))
    patterns = [
        case["remove"].encode(encoding)
        for encoding in ("utf-8", "utf-16-be", "utf-16-le")
    ]
    original_codes = b"".join(
        bytes.fromhex(row["code_hex"])
        for row in case["expected_decomposition"]
        if row["removed"]
    )
    patterns += [original_codes.hex().encode(), original_codes.hex().upper().encode()]
    fonts, usage = inventory(after, True)
    allowed_cmaps = {fonts[binding]["tounicode_identity"] for binding in usage}
    allowed_font_streams = {fonts[binding]["payload_identity"] for binding in usage}
    for value in all_objects(after):
        if isinstance(value, StreamObject):
            data = value.get_data()
            assert (
                sha(data) not in forbidden
            ), "old FontFile2/Encoding/ToUnicode/CID map/content stream retained"
            assert not any(
                pattern in data for pattern in patterns
            ), "removed data in decoded stream"
            if b"begincmap" in data:
                assert (
                    key(value) in allowed_cmaps
                ), "orphan Encoding/ToUnicode CMap stream remains"
            if data.startswith((b"\x00\x01\x00\x00", b"true", b"OTTO")):
                assert (
                    key(value) in allowed_font_streams
                ), "orphan embedded font stream remains"
    assert not any(
        pattern in target.read_bytes() for pattern in patterns
    ), "removed data in raw PDF"
    return dict(
        all_xref_and_object_streams_checked=True,
        old_source_streams_absent=True,
        removed_sequence_absent=True,
    )


def type0_outline_positions(reader, page_number):
    """Parse the bounded fixture's output text geometry without PDFium charboxes."""
    glyphs = []

    def compose(left, right):
        a, b, c, d, e, f = left
        g, h, i, j, k, l = right
        return (
            a * g + c * h,
            b * g + d * h,
            a * i + c * j,
            b * i + d * j,
            a * k + c * l + e,
            b * k + d * l + f,
        )

    def shifted(matrix, x, y):
        return compose(matrix, (1, 0, 0, 1, x, y))

    def inspect(content, resources, initial, active):
        resources = resources.get_object()
        fonts = {}
        for name, value in resources.get("/Font", {}).items():
            record = font_record(value, output=True)
            if record:
                child = value.get_object()["/DescendantFonts"][0].get_object()
                widths, items, at = {}, child.get("/W", []), 0
                while at < len(items):
                    first, following = int(items[at]), items[at + 1]
                    if isinstance(following, list):
                        widths.update(
                            {
                                first + i: float(width)
                                for i, width in enumerate(following)
                            }
                        )
                        at += 2
                    else:
                        last, width = int(following), float(items[at + 2])
                        widths.update({cid: width for cid in range(first, last + 1)})
                        at += 3
                fonts[str(name)] = (record, widths, float(child.get("/DW", 1000)))
        state, stack = dict(initial), []

        def show(item):
            if state["font"] not in fonts:
                state["unknown_advance"] = True
                return
            assert not state[
                "unknown_advance"
            ], "unknown simple-font advance before Type0 glyph"
            record, widths, default = fonts[state["font"]]
            sfnt = record["sfnt"]
            font_glyphs = sfnt.getGlyphSet()
            for code in decode(item.original_bytes, record["spaces"]):
                cp, cid, gid = record["entries"][code]
                factor = state["size"] / sfnt["head"].unitsPerEm
                scale = (factor * state["horizontal"], 0, 0, factor, 0, state["rise"])
                matrix = compose(state["ctm"], compose(state["tm"], scale))
                pen = BoundsPen(font_glyphs)
                font_glyphs[sfnt.getGlyphOrder()[gid]].draw(TransformPen(pen, matrix))
                glyphs.append((cp, list(pen.bounds) if pen.bounds else None))
                word = state["tw"] if code == b"\x20" else 0
                advance = (
                    widths.get(cid, default) * state["size"] / 1000 + state["tc"] + word
                ) * state["horizontal"]
                state["tm"] = shifted(state["tm"], advance, 0)

        def newline():
            state["line"] = shifted(state["line"], 0, -state["leading"])
            state["tm"], state["unknown_advance"] = state["line"], False

        for args, operator in ContentStream(content, reader).operations:
            if operator == b"q":
                stack.append(dict(state))
            elif operator == b"Q":
                assert stack
                state = stack.pop()
            elif operator == b"cm":
                state["ctm"] = compose(state["ctm"], tuple(float(v) for v in args))
            elif operator == b"BT":
                state["tm"] = state["line"] = (1, 0, 0, 1, 0, 0)
                state["unknown_advance"] = False
            elif operator == b"Tf":
                state["font"], state["size"] = str(args[0]), float(args[1])
            elif operator in (b"Tc", b"Tw", b"TL", b"Ts"):
                state[
                    {b"Tc": "tc", b"Tw": "tw", b"TL": "leading", b"Ts": "rise"}[
                        operator
                    ]
                ] = float(args[0])
            elif operator == b"Tz":
                state["horizontal"] = float(args[0]) / 100
            elif operator == b"Tm":
                state["tm"] = state["line"] = tuple(float(v) for v in args)
                state["unknown_advance"] = False
            elif operator in (b"Td", b"TD"):
                x, y = map(float, args)
                if operator == b"TD":
                    state["leading"] = -y
                state["line"] = shifted(state["line"], x, y)
                state["tm"], state["unknown_advance"] = state["line"], False
            elif operator == b"T*":
                newline()
            elif operator in (b"Tj", b"TJ", b"'", b'"'):
                if operator == b'"':
                    state["tw"], state["tc"] = float(args[0]), float(args[1])
                if operator in (b"'", b'"'):
                    newline()
                for item in args[0] if operator == b"TJ" else [args[-1]]:
                    if hasattr(item, "original_bytes"):
                        show(item)
                    else:
                        state["tm"] = shifted(
                            state["tm"],
                            -float(item) * state["size"] * state["horizontal"] / 1000,
                            0,
                        )
            elif operator == b"Do":
                form = resources["/XObject"][args[0]].get_object()
                if form.get("/Subtype") == "/Form":
                    identity = key(form)
                    assert identity not in active
                    nested = dict(state)
                    nested["ctm"] = compose(
                        state["ctm"],
                        tuple(
                            float(v) for v in form.get("/Matrix", [1, 0, 0, 1, 0, 0])
                        ),
                    )
                    inspect(
                        form,
                        form.get("/Resources", resources),
                        nested,
                        active | {identity},
                    )
        assert not stack, "unbalanced output graphics state"

    page = reader.pages[page_number]
    inspect(
        page.get_contents(),
        page["/Resources"],
        dict(
            ctm=(1, 0, 0, 1, 0, 0),
            tm=(1, 0, 0, 1, 0, 0),
            line=(1, 0, 0, 1, 0, 0),
            font=None,
            size=0,
            tc=0,
            tw=0,
            horizontal=1,
            rise=0,
            leading=0,
            unknown_advance=False,
        ),
        set(),
    )
    return glyphs


def positions(target, before, case):
    after = PdfReader(target)
    assert len(after.pages) == len(before.pages)
    maximum = 0.0
    with pdfium.PdfDocument(target) as document:
        for index in range(len(before.pages)):
            for name in ("/MediaBox", "/CropBox", "/Rotate", "/UserUnit"):
                assert before.pages[index].get(name) == after.pages[index].get(name)
            page_text = document[index].get_textpage()
            outlines = type0_outline_positions(after, index)
            outline_text = "".join(cp for cp, box in outlines)
            actual = page_text.get_text_range()
            external = after.pages[index].extract_text()
            assert case["remove"] not in actual and case["remove"] not in external
            terms = case["keep"] if index == 0 else case["second_page_terms"]
            for term in terms:
                assert actual.count(term) == 1 and term in external, (
                    "retained text",
                    index,
                    term,
                    actual,
                    external,
                )
                if case.get("geometry_oracle") and term in outline_text:
                    assert outline_text.count(term) == 1
                    start = outline_text.index(term)
                    measured = [box for cp, box in outlines[start : start + len(term)]]
                    assert all(box is not None for box in measured)
                else:
                    # The unchanged simple Helvetica footer retains its v1 charbox oracle.
                    assert term == "KEEP_FOLLOWING_LINE" or not case.get(
                        "geometry_oracle"
                    )
                    start = actual.index(term)
                    measured = [
                        list(page_text.get_charbox(i))
                        for i in range(start, start + len(term))
                    ]
                baseline = case["boxes"][str(index)][term]
                assert len(measured) == len(baseline)
                maximum = max(
                    maximum,
                    max(
                        abs(a - b)
                        for left, right in zip(measured, baseline)
                        for a, b in zip(left, right)
                    ),
                )
    assert maximum <= case["max_position_error_pt"], maximum
    return maximum


def pdfium_charbox_observation(target, case):
    maximum = 0.0
    with pdfium.PdfDocument(target) as document:
        for index in range(len(document)):
            page = document[index].get_textpage()
            text = page.get_text_range()
            for term, boxes in case["boxes"][str(index)].items():
                first = text.index(term)
                for offset, baseline in enumerate(boxes):
                    maximum = max(
                        maximum,
                        *(
                            abs(a - b)
                            for a, b in zip(page.get_charbox(first + offset), baseline)
                        ),
                    )
    return dict(
        maximum_difference_pt=maximum,
        acceptance="observation only: PDFium charboxes and true Bezier bounds have different definitions",
    )


def output_text_semantics(target, case):
    reader = PdfReader(target)
    with pdfium.PdfDocument(target) as document:
        for index in range(len(reader.pages)):
            content = document[index].get_textpage().get_text_range()
            external = reader.pages[index].extract_text()
            assert case["remove"] not in content and case["remove"] not in external
            terms = case["keep"] if index == 0 else case["second_page_terms"]
            for term in terms:
                assert content.count(term) == 1 and term in external, (
                    index,
                    term,
                    content,
                    external,
                )
    return dict(PDFium_and_pypdf_fixed_terms_verified=True, removed_phrase_absent=True)


def pixels(source, target, dpi, page_index, rectangle, engine, inspection_region=None):
    with pdfium.PdfDocument(source) as before, pdfium.PdfDocument(target) as after:
        page = before[page_index]
        if engine:

            def render(path):
                result = subprocess.run(
                    [
                        str(engine),
                        "-cropbox",
                        "-singlefile",
                        "-png",
                        "-r",
                        str(dpi),
                        "-f",
                        str(page_index + 1),
                        "-l",
                        str(page_index + 1),
                        str(path),
                    ],
                    capture_output=True,
                    env=project_engine_environment(),
                    timeout=60,
                    check=True,
                )
                return np.array(Image.open(io.BytesIO(result.stdout)).convert("RGB"))

            a, b = render(source), render(target)
        else:
            a = np.array(page.render(scale=dpi / 72).to_pil().convert("RGB"))
            b = np.array(
                after[page_index].render(scale=dpi / 72).to_pil().convert("RGB")
            )
        assert a.shape == b.shape
        difference = np.any(a != b, axis=2)
        if rectangle or inspection_region:
            x, y, width, height = inspection_region or rectangle
            h, w, _ = a.shape
            corners = []
            for px, py in (
                (x, y),
                (x + width, y),
                (x, y + height),
                (x + width, y + height),
            ):
                ox, oy = ctypes.c_int(), ctypes.c_int()
                assert raw.FPDF_PageToDevice(
                    page.raw,
                    0,
                    0,
                    w * 200,
                    h * 200,
                    0,
                    px,
                    py,
                    ctypes.byref(ox),
                    ctypes.byref(oy),
                )
                corners.append((ox.value / 200, oy.value / 200))
            x0, x1 = max(0, math.floor(min(p[0] for p in corners)) - 1), min(
                w, math.ceil(max(p[0] for p in corners)) + 1
            )
            y0, y1 = max(0, math.floor(min(p[1] for p in corners)) - 1), min(
                h, math.ceil(max(p[1] for p in corners)) + 1
            )
            if inspection_region:
                selected = difference[y0:y1, x0:x1].copy()
                difference[:] = False
                difference[y0:y1, x0:x1] = selected
            else:
                difference[y0:y1, x0:x1] = False
        return int(np.count_nonzero(difference))


def project_engine_environment():
    directory = ROOT / "build/dev42-engine-temp"
    directory.mkdir(parents=True, exist_ok=True)
    environment = dict(os.environ)
    environment.update(TEMP=str(directory), TMP=str(directory))
    return environment


def owned_signatures(reader, page):
    result = []
    for value in reader.pages[page].get("/Annots", []):
        annotation = value.get_object()
        metadata = annotation.get("/Tatsujin")
        if metadata is None:
            continue
        payload = (
            metadata.original_bytes
            if hasattr(metadata, "original_bytes")
            else str(metadata).encode()
        )
        info = json.loads(payload)
        result.append(
            dict(
                metadata=info, rectangle=[float(value) for value in annotation["/Rect"]]
            )
        )
    return result


def source_render_controls(args, fixed):
    """Recode authored input to CID bytes directly, without a product helper.

    This is an input rendering cross-check, never a product redaction result.
    The existing source FontFile2/CIDToGIDMap/W tables stay byte-identical.
    """
    engine = poppler_tool("pdftoppm")
    results = []
    for filename in ("mixed-value-length.pdf", "shared-pages-alias.pdf"):
        case = next(row for row in fixed["cases"] if row["file"] == filename)
        source = args.fixtures / filename
        inspect_source(source, case)
        writer = PdfWriter(clone_from=PdfReader(source))
        records = {}
        for page in writer.pages:
            local = {}
            for name, value in page["/Resources"]["/Font"].items():
                font = value.get_object()
                if "/ToUnicode" not in font:
                    continue
                identity = key(value)
                if identity not in records:
                    record = font_record(value)
                    assert record
                    records[identity] = record
                    mapping = {cid: cp for cp, cid, gid in record["entries"].values()}
                    data = (
                        b"begincmap\n1 begincodespacerange <0000> <ffff> endcodespacerange\n"
                        + str(len(mapping)).encode()
                        + b" beginbfchar\n"
                    )
                    data += b"".join(
                        f"<{cid:04x}> <{cp.encode('utf-16-be').hex()}>\n".encode()
                        for cid, cp in sorted(mapping.items())
                    )
                    data += b"endbfchar\nendcmap\n"
                    unicode_stream = DecodedStreamObject()
                    unicode_stream.set_data(data)
                    font[NameObject("/Encoding")] = NameObject("/Identity-H")
                    font[NameObject("/ToUnicode")] = writer._add_object(unicode_stream)
                local[str(name)] = records[identity]
            content = ContentStream(page.get_contents(), writer)
            current = None
            for arguments, operator in content.operations:
                if operator == b"Tf":
                    current = local.get(str(arguments[0]))
                elif operator in (b"Tj", b"TJ", b"'", b'"') and current:
                    if operator == b"TJ":
                        for index, value in enumerate(arguments[0]):
                            if hasattr(value, "original_bytes"):
                                arguments[0][index] = ByteStringObject(
                                    b"".join(
                                        current["entries"][code][1].to_bytes(2, "big")
                                        for code in decode(
                                            value.original_bytes, current["spaces"]
                                        )
                                    )
                                )
                    else:
                        arguments[-1] = ByteStringObject(
                            b"".join(
                                current["entries"][code][1].to_bytes(2, "big")
                                for code in decode(
                                    arguments[-1].original_bytes, current["spaces"]
                                )
                            )
                        )
            page[NameObject("/Contents")] = writer._add_object(content)
        target = args.output.parent / (args.output.stem + "-" + filename)
        assert not target.exists()
        writer.write(target)
        before, after = inventory(PdfReader(source)), inventory(PdfReader(target))
        assert (
            before[1] == after[1]
        ), "independent Encoding/Unicode/CID/GID control routes differ"
        for binding, record in before[0].items():
            assert record["payload"] == after[0][binding]["payload"]
        raster = {
            f"{label}-{dpi}": [
                pixels(source, target, dpi, index, None, renderer)
                for index in range(len(PdfReader(source).pages))
            ]
            for dpi in (72, 96)
            for label, renderer in (("PDFium", None), ("Poppler", engine))
        }
        outside = {
            f"{label}-{dpi}": [
                pixels(
                    source,
                    target,
                    dpi,
                    index,
                    case["rect"] if index == 0 else None,
                    renderer,
                )
                for index in range(len(PdfReader(source).pages))
            ]
            for dpi in (72, 96)
            for label, renderer in (("PDFium", None), ("Poppler", engine))
        }
        source_text = []
        output_text = []
        with pdfium.PdfDocument(source) as original, pdfium.PdfDocument(
            target
        ) as control:
            for index in range(len(original)):
                source_text.append(original[index].get_textpage().get_text_range())
                output_text.append(control[index].get_textpage().get_text_range())
            assert case["remove"] in output_text[0]
            if filename == "mixed-value-length.pdf":
                image_path = args.output.parent / (
                    args.output.stem + "-source-preview.png"
                )
                assert not image_path.exists()
                original[0].render(scale=1).to_pil().convert("RGB").crop(
                    (50, 110, 420, 155)
                ).save(image_path)
        results.append(
            dict(
                file=filename,
                status=(
                    "PASS_INPUT_RENDERING_CONTROL"
                    if all(
                        all(value == 0 for value in values)
                        for values in raster.values()
                    )
                    else "INPUT_ENGINE_RENDERING_MISMATCH"
                ),
                source_sha256=sha(source.read_bytes()),
                control_path=str(target),
                control_sha256=sha(target.read_bytes()),
                unchanged_font_payloads=True,
                independent_code_CID_GID_Unicode_routes=True,
                raster=raster,
                outside_fixed_redaction_rect=outside,
                source_PDFium_extraction=source_text,
                control_PDFium_extraction=output_text,
            )
        )
    return dict(
        status=(
            "PASS_INPUT_RENDERING_CONTROLS"
            if all(row["status"] == "PASS_INPUT_RENDERING_CONTROL" for row in results)
            else "INPUT_ENGINE_RENDERING_MISMATCH"
        ),
        source_only=True,
        results=results,
        passed=0,
        product_runtime_acceptance="UNEXECUTED",
        general_M1_M3_M5_acceptance="withheld",
    )


def renderer_metrics_policy(args, fixed):
    if fixed["version"] != 2:
        return None
    raw = (args.fixtures / "renderer-metrics.json").read_bytes()
    assert sha(raw) == RENDERER_METRICS_PIN
    policy = json.loads(raw)
    assert policy["source_criteria_sha256"] == CRITERIA_PIN
    assert policy["positive_cases"] == 19 and policy["source_resource_bindings"] == 25
    assert {row["file"]: row["source_sha256"] for row in policy["cases"]} == {
        row["file"]: row["sha256"] for row in fixed["cases"] if not row["refused"]
    }
    return {row["file"]: row for row in policy["cases"]}


def audit_space_selection(args, fixed, native_details, renderer_metrics):
    raw = (args.fixtures / "space-selection.json").read_bytes()
    assert sha(raw) == SPACE_SELECTION_PIN
    condition = json.loads(raw)
    assert condition["source_criteria_sha256"] == CRITERIA_PIN
    assert native_details["source_unchanged"] is True
    case = next(case for case in fixed["cases"] if case["file"] == condition["source"])
    source = args.fixtures / condition["source"]
    assert sha(source.read_bytes()) == condition["source_sha256"] == case["sha256"]
    inspect_source(source, case)
    original = PdfReader(source)
    records, uses = inventory(original)
    assert set(uses) == {"page/0/font/F2+0"}
    binding = "page/0/font/F2+0"
    ordinal = condition["selected_ordinal_zero_based"]
    removed = uses[binding][ordinal]
    record = records[binding]
    code = bytes.fromhex(condition["source_code_hex"])
    assert record["entries"][code] == removed
    assert removed[:2] == (condition["source_unicode"], condition["source_cid"])
    assert (
        record["sfnt"]["glyf"][
            record["sfnt"].getGlyphOrder()[removed[2]]
        ].numberOfContours
        == 0
    )
    retained = uses[binding][:ordinal] + uses[binding][ordinal + 1 :]
    assert [cp for cp, cid, gid in retained] == condition["expected_retained_unicode"]
    assert [
        record["entries"][bytes.fromhex(code)]
        for code in condition["expected_retained_code_hex"]
    ] == retained
    original_entries = case["expected_decomposition"]
    assert original_entries[ordinal]["origin_pt"] == condition["source_origin_pt"]
    assert (
        original_entries[ordinal + 1]["origin_pt"] == condition["source_next_origin_pt"]
    )
    assert [
        entry["origin_pt"]
        for index, entry in enumerate(original_entries)
        if index != ordinal
    ] == condition["expected_retained_origins_pt"]
    left, y = condition["source_origin_pt"]
    right, next_y = condition["source_next_origin_pt"]
    x, bottom, width, height = condition["rect"]
    assert (
        x <= min(left, right)
        and max(left, right) <= x + width
        and bottom <= y == next_y <= bottom + height
    )
    target = args.run / condition["native_output"]
    after = PdfReader(target)
    assert (
        native_details["retained_glyphs"] == 13
        and native_details["full_space_removed"] is True
    )
    resources = audit_resources(
        original, after, case, {binding: retained}, renderer_metrics[case["file"]]
    )
    actual = type0_outline_positions(after, 0)
    assert [cp for cp, box in actual] == condition["expected_retained_unicode"]
    maximum = 0.0
    font = record["sfnt"]
    glyphs = font.getGlyphSet()
    factor = condition["source_font_size_pt"] / font["head"].unitsPerEm
    for (cp, box), (expected_cp, cid, gid), origin in zip(
        actual, retained, condition["expected_retained_origins_pt"]
    ):
        assert cp == expected_cp
        pen = BoundsPen(glyphs)
        glyphs[font.getGlyphOrder()[gid]].draw(
            TransformPen(pen, (factor, 0, 0, factor, *origin))
        )
        if pen.bounds is None:
            assert box is None
        else:
            assert box is not None
            maximum = max(maximum, *(abs(a - b) for a, b in zip(box, pen.bounds)))
    assert maximum <= condition["max_position_error_pt"]
    assert sha(source.read_bytes()) == condition["source_sha256"]
    return dict(
        file="space_selection",
        status="PASS",
        scope=condition["scope"],
        fixed_condition_sha256=SPACE_SELECTION_PIN,
        completed_native_check=native_details,
        independent_U0020_ordinal_and_advance_segment_verified=True,
        retained_Unicode_use_count=len(retained),
        retained_Unicode="".join(condition["expected_retained_unicode"]),
        max_true_outline_position_error_pt=maximum,
        resource_audit=resources,
        output_sha256=sha(target.read_bytes()),
        external_raster_acceptance="UNEXECUTED by explicit fixed positive-condition scope",
    )


def validate_producer(args, fixed):
    assert args.run
    process = json.loads((args.run / "process-result.json").read_bytes())
    assert (
        process["completed"] and process["report_valid"] and process["exit_code"] == 0
    )
    executable = args.producer_executable or Path(process["executable"])
    assert sha(executable.read_bytes()) == process["exe_sha256"]
    tests = json.loads((args.run / "selftest.json").read_bytes())["tests"]
    assert process["test_count"] == len(tests) and len(
        {row["name"] for row in tests}
    ) == len(tests)
    assert all(row["status"] == "PASS" for row in tests)
    path_test = next(row for row in tests if row["name"] == args.path_test_name)
    details = path_test["details"]
    assert {row["file"]: row["refused"] for row in details["cases"]} == {
        row["file"]: row["refused"] for row in fixed["cases"]
    }
    assert len(details["cases"]) == len(fixed["cases"])
    assert details["refused"] == sum(row["refused"] for row in fixed["cases"])
    if "fixture_criteria_sha256" in details:
        assert details["fixture_criteria_sha256"] == CRITERIA_PIN
    ui = (
        None
        if args.cases_only
        else next(row for row in tests if row["name"] == args.ui_test_name)
    )
    safety = None
    if fixed["version"] == 2:
        raw = (args.fixtures / "decoder-safety-r2.json").read_bytes()
        assert sha(raw) == DECODER_SAFETY_PIN
        truth = json.loads(raw)
        assert truth["source_criteria_sha256"] == CRITERIA_PIN
        source = args.fixtures / truth["source"]
        _, _, _, _, encoding, unicode, _, _ = source_font(PdfReader(source))
        code = bytes.fromhex(truth["valid_code_hex"])
        assert (
            encoding[code] == truth["expected_cid"]
            and unicode[code] == truth["expected_unicode"]
        )
        direct = details["decoder_safety"]
        assert direct["valid_codes"] == truth["successful_validation_codes"]
        assert direct["invalid_tail_bytes"] == truth["malformed_input"]["tail_bytes"]
        assert direct["cancelled"] == 2 * len(truth["cancellation"]["cutoffs"])
        assert direct["source_unchanged"] is True
        assert direct["PDF_rendering"] == "not exercised by direct scanner test"
        safety = dict(
            status="PASS_COMPLETED_NATIVE_DIRECT_SCANNER_METADATA",
            criteria_sha256=DECODER_SAFETY_PIN,
            input_source_sha256=sha(source.read_bytes()),
            expected_encoding_cid=encoding[code],
            expected_tounicode=unicode[code],
            decoded_valid_codes=truth["successful_decode_codes"],
            validated_codes=direct["valid_codes"],
            malformed_tail_bytes=direct["invalid_tail_bytes"],
            cancellation_checks=direct["cancelled"],
            early_prefix_and_exact_cutoff_assertions_completed=True,
            source_unchanged=True,
            extra_runtime_case_PASS_count_added=0,
            scope=truth["scope"],
        )
    return process, ui, safety, details.get("space_selection")


def runtime_report(args, fixed):
    process, ui_test, decoder_safety, space_selection = validate_producer(args, fixed)
    renderer_metrics = renderer_metrics_policy(args, fixed)
    engine = poppler_tool("pdftoppm")
    rows = []
    for case in fixed["cases"]:
        source = args.fixtures / case["file"]
        assert sha(source.read_bytes()) == case["sha256"]
        target = args.run / (args.output_prefix + case["file"])
        row = dict(file=case["file"], status="FAIL")
        try:
            if case["refused"]:
                assert not target.exists()
                row.update(
                    status="PASS",
                    completed_producer_refusal_verified=True,
                    output_absent=True,
                    source_unchanged=True,
                )
            else:
                inspect_source(source, case)
                before, after = PdfReader(source), PdfReader(target)
                checks, maximum, resources, deleted = {}, None, None, None
                for label, operation in (
                    (
                        "output_text_semantics",
                        lambda: output_text_semantics(target, case),
                    ),
                    (
                        "fixed_source_geometry_and_text",
                        lambda: positions(target, before, case),
                    ),
                    (
                        "independent_resource_GID_Unicode_and_closure",
                        lambda: audit_resources(
                            before,
                            after,
                            case,
                            renderer_metrics=(
                                renderer_metrics[case["file"]]
                                if renderer_metrics
                                else None
                            ),
                        ),
                    ),
                    (
                        "deleted_data",
                        lambda: audit_deleted_data(before, after, target, case),
                    ),
                ):
                    try:
                        value = operation()
                        checks[label] = dict(status="PASS", evidence=value)
                        if label == "fixed_source_geometry_and_text":
                            maximum = value
                        elif label == "independent_resource_GID_Unicode_and_closure":
                            resources = value
                        else:
                            deleted = value
                    except Exception as error:
                        checks[label] = dict(
                            status="FAIL",
                            error=repr(error),
                            traceback=traceback.format_exc(),
                        )
                raster = {}
                for dpi in fixed["dpi"]:
                    for label, renderer in (("PDFium", None), ("Poppler", engine)):
                        differences = [
                            pixels(
                                source,
                                target,
                                dpi,
                                page,
                                case["rect"] if page == 0 else None,
                                renderer,
                            )
                            for page in range(len(before.pages))
                        ]
                        raster[f"{label}-{dpi}"] = differences
                raster_ok = all(
                    all(
                        value == fixed["outside_pixel_difference"]
                        for value in differences
                    )
                    for differences in raster.values()
                )
                checks["direct_source_render_preservation"] = dict(
                    status="PASS" if raster_ok else "FAIL", evidence=raster
                )
                row.update(
                    status=(
                        "PASS"
                        if all(check["status"] == "PASS" for check in checks.values())
                        else "FAIL"
                    ),
                    checks=checks,
                    max_position_error_pt=maximum,
                    position_measurement_method=(
                        "independent output code/CID/GID and text-operator origins plus true Bezier bounds against frozen original true bounds; simple Helvetica footer uses unchanged v1 charboxes"
                        if fixed["version"] == 2
                        else "v1 frozen PDFium charboxes"
                    ),
                    PDFium_charbox_position_observation=pdfium_charbox_observation(
                        target, case
                    ),
                    resource_audit=resources,
                    deleted_data=deleted,
                    raster=raster,
                    output_sha256=sha(target.read_bytes()),
                )
        except Exception as error:
            row.update(error=repr(error), traceback=traceback.format_exc())
        rows.append(row)
    if ui_test:
        row = dict(file="UI_atomicity", status="FAIL")
        try:
            details = ui_test["details"]
            for name in (
                "source_unchanged",
                "outside_signature_and_reedit",
                "refusal_UI_atomic",
            ):
                assert details[name] is True, name
            assert (
                details["cancelled"] == 3 and details["cancellation_checkpoints"] >= 3
            )
            case = next(
                case for case in fixed["cases"] if case["file"] == fixed["UI_source"]
            )
            before = args.run / (args.ui_prefix + "-before.pdf")
            after = args.run / (args.ui_prefix + ".pdf")
            reedited = args.run / (args.ui_prefix + "-reedited.pdf")
            assert before.exists() and after.exists() and reedited.exists()
            assert not list(args.run.glob(args.ui_prefix + "-cancel-*.pdf"))
            original_signatures = owned_signatures(PdfReader(before), 1)
            kept_signatures = owned_signatures(PdfReader(after), 1)
            reedited_signatures = owned_signatures(PdfReader(reedited), 1)
            assert len(original_signatures) == len(kept_signatures) == 1
            assert original_signatures == kept_signatures
            assert original_signatures[0]["metadata"]["text"] == "別ページの未保存署名"
            assert (
                len(reedited_signatures) == 2
                and kept_signatures[0] in reedited_signatures
            )
            assert any(
                item["metadata"]["text"] == "共有字体の処理後も再編集"
                for item in reedited_signatures
            )
            x0, y0, x1, y1 = original_signatures[0]["rectangle"]
            signature_rect = [x0 - 1, y0 - 1, x1 - x0 + 2, y1 - y0 + 2]
            signature_raster = {
                f"{label}-{dpi}": pixels(
                    before, after, dpi, 1, None, renderer, signature_rect
                )
                for dpi in fixed["dpi"]
                for label, renderer in (("PDFium", None), ("Poppler", engine))
            }
            assert all(
                value == 0 for value in signature_raster.values()
            ), signature_raster
            raster = {
                f"{label}-{dpi}": [
                    pixels(
                        before,
                        after,
                        dpi,
                        page,
                        case["rect"] if page == 0 else None,
                        renderer,
                    )
                    for page in (0, 1)
                ]
                for dpi in fixed["dpi"]
                for label, renderer in (("PDFium", None), ("Poppler", engine))
            }
            rendering_ok = all(value == [0, 0] for value in raster.values())
            row.update(
                status="PASS" if rendering_ok else "FAIL",
                producer_atomicity_booleans_verified=True,
                unsaved_signature_metadata_and_reedit_verified=True,
                unsaved_signature_pixels_preserved=True,
                unsaved_signature_region_raster=signature_raster,
                direct_source_render_preservation="PASS" if rendering_ok else "FAIL",
                raster=raster,
                before_sha256=sha(before.read_bytes()),
                after_sha256=sha(after.read_bytes()),
                reedited_sha256=sha(reedited.read_bytes()),
            )
        except Exception as error:
            row.update(error=repr(error), traceback=traceback.format_exc())
        rows.append(row)
    if space_selection is not None:
        try:
            rows.append(
                audit_space_selection(args, fixed, space_selection, renderer_metrics)
            )
        except Exception as error:
            rows.append(
                dict(
                    file="space_selection",
                    status="FAIL",
                    error=repr(error),
                    traceback=traceback.format_exc(),
                )
            )
    failed_paths = {}
    for row in rows:
        if row["status"] != "FAIL":
            continue
        stem = (
            args.ui_prefix
            if row["file"] == "UI_atomicity"
            else (
                "mixed-cmap-space-fully-contained"
                if row["file"] == "space_selection"
                else args.output_prefix + Path(row["file"]).stem
            )
        )
        for path in args.run.iterdir():
            if path.name.startswith(stem) and path.suffix.lower() in (".pdf", ".png"):
                failed_paths[
                    str(path.resolve().relative_to(ROOT)).replace("\\", "/")
                ] = dict(bytes=path.stat().st_size, sha256=sha(path.read_bytes()))
    return dict(
        status="PASS" if all(row["status"] == "PASS" for row in rows) else "FAIL",
        source_only=False,
        cases_only=args.cases_only,
        UI_independent_audit="UNEXECUTED" if args.cases_only else "EXECUTED",
        producer_completed=True,
        producer_executable_sha256=process["exe_sha256"],
        producer_process_sha256=sha((args.run / "process-result.json").read_bytes()),
        producer_selftest_sha256=sha((args.run / "selftest.json").read_bytes()),
        results=rows,
        passed=sum(row["status"] == "PASS" for row in rows),
        failed=sum(row["status"] == "FAIL" for row in rows),
        engines=["PDFium", "pypdf", "fontTools", "Poppler"],
        native_external_GUI="UNEXECUTED",
        general_M1_M3_M5_acceptance="withheld",
        MIAP_4096_action_evidence="UNEXECUTED",
        failed_output_paths=failed_paths,
        direct_decoder_safety=decoder_safety,
    )


def semantic_report(report, direct_output):
    rows = []
    for original in report.get("results", []):
        row = dict(
            file=original["file"],
            status="FAIL",
            frozen_full_contract_status=original["status"],
        )
        if original["file"] == "UI_atomicity":
            if original.get("producer_atomicity_booleans_verified"):
                row.update(
                    status="PASS",
                    scope="completed native cancellation/refusal/original Undo/unsaved signature/reedit state checks only",
                    direct_render_contract=original.get(
                        "direct_source_render_preservation"
                    ),
                )
        elif original["file"] == "space_selection":
            row.update(
                status=original["status"],
                scope=original.get("scope"),
                independent_space_selection=original,
            )
        elif original.get("completed_producer_refusal_verified"):
            row.update(
                status="PASS",
                scope="completed native refusal, output absence and unchanged original",
            )
        else:
            required = (
                "output_text_semantics",
                "independent_resource_GID_Unicode_and_closure",
                "deleted_data",
            )
            checks = original.get("checks", {})
            if all(checks.get(name, {}).get("status") == "PASS" for name in required):
                row.update(
                    status="PASS",
                    scope="fixed output Unicode plus resource CID/GID/FontFile2 closure and removed-data audits only",
                    checks={name: checks[name] for name in required},
                    direct_position_contract=checks.get(
                        "fixed_source_geometry_and_text", {}
                    ).get("status"),
                    direct_render_contract=checks.get(
                        "direct_source_render_preservation", {}
                    ).get("status"),
                )
        rows.append(row)
    return dict(
        status=(
            "PASS_SCOPED_OUTPUT_SEMANTICS"
            if rows and all(row["status"] == "PASS" for row in rows)
            else "FAIL_SCOPED_OUTPUT_SEMANTICS"
        ),
        scope="This report excludes direct source rendering and frozen measured position acceptance; their original PASS/FAIL remain in the linked full-contract report. It does not qualify general CMap/M1/M3/M5 or MIAP/4096.",
        source_only=False,
        producer_completed=report.get("producer_completed", False),
        producer_executable_sha256=report.get("producer_executable_sha256"),
        producer_process_sha256=report.get("producer_process_sha256"),
        producer_selftest_sha256=report.get("producer_selftest_sha256"),
        criteria_sha256=CRITERIA_PIN,
        direct_full_contract_report=str(
            direct_output.resolve().relative_to(ROOT)
        ).replace("\\", "/"),
        direct_full_contract_report_sha256=sha(direct_output.read_bytes()),
        direct_full_contract_status=report["status"],
        results=rows,
        passed=sum(row["status"] == "PASS" for row in rows),
        failed=sum(row["status"] == "FAIL" for row in rows),
        original_failed_outputs_must_remain_literal=report.get(
            "failed_output_paths", {}
        ),
        general_M1_M3_M5_acceptance="withheld",
        MIAP_4096_action_evidence="UNEXECUTED",
        direct_decoder_safety=report.get("direct_decoder_safety"),
    )


def main():
    global CRITERIA_PIN
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--fixtures", type=Path, required=True)
    parser.add_argument("--source-only", action="store_true")
    parser.add_argument("--source-render-controls", action="store_true")
    parser.add_argument("--run", type=Path)
    parser.add_argument("--producer-executable", type=Path)
    parser.add_argument("--cases-only", action="store_true")
    parser.add_argument("--path-test-name", default="M5RM01_mixed_width_CMap")
    parser.add_argument("--ui-test-name", default="M5RM02_mixed_width_CMap_UI")
    parser.add_argument("--output-prefix", default="mixed-cmap-")
    parser.add_argument("--ui-prefix", default="mixed-cmap-ui")
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--semantic-output", type=Path)
    args = parser.parse_args()
    CRITERIA_PIN = CRITERIA_PINS[args.fixtures.name]
    assert not args.output.exists(), "preserve all previous reports and failures"
    try:
        criteria = (args.fixtures / "criteria.json").read_bytes()
        assert sha(criteria) == CRITERIA_PIN
        fixed = json.loads(criteria)
        report = (
            source_render_controls(args, fixed)
            if args.source_render_controls
            else (
                source_report(args, fixed)
                if args.source_only
                else runtime_report(args, fixed)
            )
        )
    except Exception as error:
        report = dict(
            status="FAIL",
            error=repr(error),
            traceback=traceback.format_exc(),
            required_runtime_pass_count_added=0,
        )
    report.update(
        criteria_sha256=CRITERIA_PIN,
        evaluator_sha256=sha(Path(__file__).read_bytes()),
        run_time_utc=datetime.datetime.now(datetime.timezone.utc).isoformat(),
    )
    payload = (json.dumps(report, ensure_ascii=False, indent=2) + "\n").encode()
    assert len(payload) <= 250_000
    with args.output.open("xb") as output:
        output.write(payload)
    if args.semantic_output:
        assert not args.semantic_output.exists()
        semantic = semantic_report(report, args.output)
        semantic.update(
            evaluator_sha256=sha(Path(__file__).read_bytes()),
            run_time_utc=datetime.datetime.now(datetime.timezone.utc).isoformat(),
        )
        encoded = (json.dumps(semantic, ensure_ascii=False, indent=2) + "\n").encode()
        assert len(encoded) <= 250_000
        with args.semantic_output.open("xb") as output:
            output.write(encoded)
    print(
        json.dumps(
            dict(
                status=report["status"],
                failed=report.get("failed"),
                output=str(args.output),
                bytes=len(payload),
            )
        )
    )
    return int(report["status"] == "FAIL")


if __name__ == "__main__":
    raise SystemExit(main())
