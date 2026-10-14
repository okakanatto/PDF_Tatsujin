"""Independent raw-code CMap/spacing examiner for the frozen dev43 contract.

Source mode never imports or runs a renderer and never counts runtime PASS.
No product, fixture generator or previous evaluator supplies semantic truth.
"""

import argparse
import copy
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
from decimal import Decimal
from importlib.metadata import distribution
from pathlib import Path

from fontTools.pens.boundsPen import BoundsPen
from fontTools.pens.recordingPen import DecomposingRecordingPen
from fontTools.pens.transformPen import TransformPen
from fontTools.ttLib import TTFont
from pypdf import PdfReader, PdfWriter
from pypdf.generic import (
    ArrayObject,
    ByteStringObject,
    ContentStream,
    DecodedStreamObject,
    FloatObject,
    IndirectObject,
    NameObject,
    StreamObject,
)

ROOT = Path(__file__).resolve().parents[1]
IDENTITY = (1, 0, 0, 1, 0, 0)
DEV42_PIN = "796b7ae0ef979583d0180c91c1020dd30405530d044daf39e83339f0b2ae326a"


def digest(data):
    return hashlib.sha256(data).hexdigest()


def file_info(path):
    data = path.read_bytes()
    return dict(path=str(path), bytes=len(data), sha256=digest(data))


def same_path(left, right):
    return Path(left).samefile(Path(right))


def ninja_header_audit():
    ninja = ROOT / "build/app/build.ninja"
    header = ROOT / "build/app/adapted/pdffont.h"
    font = ROOT / "build/app/adapted/pdffont.cpp"
    processor = ROOT / "build/app/adapted/pdfpagecontentprocessor.cpp"
    groups = Counter()
    missing = []
    force_paths = set()
    excluded = []
    blocks = re.split(r"(?m)(?=^build )", ninja.read_text(encoding="utf-8"))
    for block in blocks:
        first = block.splitlines()[0]
        match = re.search(r": (CXX_COMPILER_\S+)", first)
        if not match:
            continue
        if "__PDFTatsujinNetworkProbe_" in match.group(1):
            network_source = (ROOT / "tests/network_probe.cpp").read_text()
            assert (
                "pdffont" not in network_source and "TextSequence" not in network_source
            )
            assert (
                "target_link_libraries(PDFTatsujinNetworkProbe PRIVATE ws2_32 advapi32)"
                in (ROOT / "CMakeLists.txt").read_text()
            )
            excluded.append(
                dict(
                    rule=match.group(1),
                    reason="Standalone Windows network harness links only ws2_32/advapi32, includes no PDF4QT or TextSequence",
                    source=file_info(ROOT / "tests/network_probe.cpp"),
                )
            )
            continue
        flags = re.search(r"(?m)^  FLAGS = (.*)$", block)
        paths = re.findall(
            r"/FI(?:\"([^\"]+)\"|(\S+))", flags.group(1) if flags else ""
        )
        targets = [a or b for a, b in paths]
        if not any(same_path(p, header) for p in targets):
            missing.append(first)
        force_paths.update(targets)
        groups[match.group(1)] += 1
    assert groups and not missing, missing
    assert all(
        any(name in target for target in groups)
        for name in (
            "Pdf4QtLibCore",
            "Pdf4QtLibWidgets",
            "tatsu_document",
            "tatsu_ocr",
            "tatsu_ui",
            "PDFTatsujin",
        )
    )
    h = header.read_text()
    f = font.read_text()
    p = processor.read_text()
    assert "#ifndef PDFFONT_H\n#define PDFFONT_H" in h
    assert "std::optional<bool> sourceWordSpacingApplies;" in h
    assert "std::optional<bool> sourceWordSpacingApplies = std::nullopt" in h
    assert f.count("mappedCode.byteCount == 1 && mappedCode.code == 32") == 2
    assert (
        "textSequence.items.emplace_back(nullpath, QChar(), -glyphWidth, cid, mappedCode.byteCount == 1 && mappedCode.code == 32);"
        in f
    )
    assert (
        "item.sourceWordSpacingApplies.value_or(character == QChar(QChar::Space))" in p
    )
    vendor = ROOT / "vendor/PDF4QT/Pdf4QtLibCore/sources"
    original_pins = {
        "pdffont.h": "cf648602eb796be79c37e2d16538d1820f88020b2a428ae42cc0ddac007293c9",
        "pdffont.cpp": "85b9b3faaff6eb88e80f1cf267d9dcf9889affc7ebd42edb708d53eefadb331c",
        "pdfpagecontentprocessor.cpp": "0ceee241804b1a5f6d59986973a3e1fe3555dfcf2c4d3f4ec29730ded35a32aa",
    }
    for name, pin in original_pins.items():
        assert digest((vendor / name).read_bytes().replace(b"\r\n", b"\n")) == pin
    return dict(
        status="PASS_STATIC_ALL_PDF4QT_CXX_HEADER_ABI",
        source_only=True,
        ninja=file_info(ninja),
        derived_header=file_info(header),
        derived_font=file_info(font),
        derived_processor=file_info(processor),
        all_PDF4QT_CXX_rule_count=sum(groups.values()),
        rule_groups=dict(groups),
        forced_header_paths=sorted(force_paths),
        all_forced_paths_same_physical_file=True,
        missing_rules=[],
        non_PDF4QT_harness_exceptions=excluded,
        PDFFONT_H_guard_uniform=True,
        Type0_raw_flag_both_branches=True,
        legacy_simple_Type3_numeric_flags_unset=True,
        missing_glyph_advance="unchanged; unsupported by scoped redaction, not newly qualified",
        vendor_normalized_source_pins_verified=original_pins,
        runtime="UNEXECUTED",
    )


def obj_id(value):
    ref = (
        value
        if isinstance(value, IndirectObject)
        else getattr(value, "indirect_reference", None)
    )
    return (ref.idnum, ref.generation) if ref else ("direct", id(value))


def objects(reader):
    keys = {(n, g) for g, numbers in reader.xref.items() for n in numbers if n}
    keys.update((n, 0) for n in reader.xref_objStm)
    return [reader.get_object(IndirectObject(n, g, reader)) for n, g in sorted(keys)]


def cmap(stream, unicode=False):
    """Parse bounded CMap sections keyed by bytes, never integer alone."""
    stream = stream.get_object()
    assert isinstance(stream, StreamObject)
    assert "/UseCMap" not in stream
    data = re.sub(rb"%[^\r\n]*", b"", stream.get_data())
    assert data.count(b"begincmap") == data.count(b"endcmap") == 1
    assert not re.search(rb"\busecmap\b|\bMystery\w*|\bmystery\w*", data)
    assert not re.search(rb"/WMode\s+(?!0(?:\s|\b))\S+", data)
    sections = re.findall(
        rb"(\d+)\s+begin(codespacerange|bfchar|bfrange|cidchar|cidrange)(.*?)end\2",
        data,
        re.S,
    )
    assert sections
    spaces, routes = [], {}

    def hx(token):
        assert token.startswith(b"<") and token.endswith(b">") and len(token) % 2 == 0
        return bytes.fromhex(token[1:-1].decode())

    def add(code, target):
        assert len(code) in (1, 2) and code not in routes
        assert any(
            len(code) == len(lo) and all(a <= v <= b for v, a, b in zip(code, lo, hi))
            for lo, hi in spaces
        )
        if unicode:
            assert len(target) == 2
            target = target.decode("utf-16-be")
            assert len(target) == 1 and not 0xD800 <= ord(target) <= 0xDFFF
        else:
            assert 0 <= target <= 65535
        routes[code] = target

    for count, kind, body in sections:
        tokens = re.findall(rb"<[0-9A-Fa-f]+>|\d+|\[|\]", body)
        # Every non-whitespace byte in a section must be a recognised token.

        assert not re.sub(rb"<[0-9A-Fa-f]+>|\d+|\[|\]|\s+", b"", body)
        count, cursor = int(count), 0
        if kind == b"codespacerange":
            assert len(tokens) == count * 2
            for i in range(count):
                lo, hi = hx(tokens[i * 2]), hx(tokens[i * 2 + 1])
                assert len(lo) == len(hi) and len(lo) in (1, 2)
                assert all(a <= b for a, b in zip(lo, hi))
                for a, b in spaces:
                    if len(a) == len(lo):
                        assert any(
                            d < e or f < c for c, d, e, f in zip(lo, hi, a, b)
                        ), "same-width codespace overlap"
                    else:
                        assert hi[0] < a[0] or b[0] < lo[0], "prefix ambiguity"
                spaces.append((lo, hi))
            continue
        assert kind.startswith(b"bf" if unicode else b"cid"), "wrong CMap section type"
        for _ in range(count):
            lo = hx(tokens[cursor])
            cursor += 1
            if kind.endswith(b"char"):
                target = hx(tokens[cursor]) if unicode else int(tokens[cursor])
                cursor += 1
                add(lo, target)
                continue
            hi = hx(tokens[cursor])
            cursor += 1
            assert len(lo) == len(hi) and int.from_bytes(hi, "big") >= int.from_bytes(
                lo, "big"
            )
            length = int.from_bytes(hi, "big") - int.from_bytes(lo, "big") + 1
            if tokens[cursor] == b"[":
                assert unicode
                cursor += 1
                targets = [hx(tokens[cursor + i]) for i in range(length)]
                cursor += length
                assert tokens[cursor] == b"]"
                cursor += 1
            else:
                start = (
                    int.from_bytes(hx(tokens[cursor]), "big")
                    if unicode
                    else int(tokens[cursor])
                )
                cursor += 1
                targets = [
                    (start + i).to_bytes(2, "big") if unicode else start + i
                    for i in range(length)
                ]
            for i, target in enumerate(targets):
                add((int.from_bytes(lo, "big") + i).to_bytes(len(lo), "big"), target)
        assert cursor == len(tokens), "section count mismatch"
    assert spaces and routes
    return spaces, routes


def codes(data, spaces):
    at, result = 0, []
    while at < len(data):
        match = [
            data[at : at + n]
            for n in (1, 2)
            if at + n <= len(data)
            and any(
                len(lo) == n
                and all(a <= v <= b for v, a, b in zip(data[at : at + n], lo, hi))
                for lo, hi in spaces
            )
        ]
        assert len(match) == 1, ("ambiguous/truncated/outside codespace", at)
        result.append(match[0])
        at += len(match[0])
    return result


def font_record(value):
    font = value.get_object()
    if font.get("/Subtype") != "/Type0":
        return None
    assert len(font["/DescendantFonts"]) == 1
    child = font["/DescendantFonts"][0].get_object()
    assert child.get("/Subtype") == "/CIDFontType2"
    descriptor = child["/FontDescriptor"].get_object()
    payload = descriptor["/FontFile2"].get_data()
    sfnt = TTFont(io.BytesIO(payload))
    assert "glyf" in sfnt and "gvar" not in sfnt and "CFF " not in sfnt
    unicode_spaces, meaning = cmap(font["/ToUnicode"], True)
    encoding = font["/Encoding"]
    identity = str(encoding) == "/Identity-H"
    if identity:
        spaces, route = [(b"\0\0", b"\xff\xff")], {
            code: int.from_bytes(code, "big") for code in meaning
        }
        assert all(len(code) == 2 for code in meaning)
    else:
        spaces, route = cmap(encoding)
        assert set(route) == set(meaning), "Encoding/ToUnicode keys"
    mapping = child.get("/CIDToGIDMap")
    mapping = (
        None
        if mapping is None or str(mapping) == "/Identity"
        else mapping.get_object().get_data()
    )
    entries, cid_meanings = {}, {}
    for code, cid in route.items():
        assert any(
            len(code) == len(lo) and all(a <= v <= b for v, a, b in zip(code, lo, hi))
            for lo, hi in unicode_spaces
        )
        gid = (
            cid
            if mapping is None
            else int.from_bytes(mapping[cid * 2 : cid * 2 + 2], "big")
        )
        cp = meaning[code]
        if cp == "\0":
            assert cid == gid == 0
            continue
        assert cid > 0 and 0 < gid < len(sfnt.getGlyphOrder())
        assert cid not in cid_meanings or cid_meanings[cid] == cp
        cid_meanings[cid] = cp
        entries[code] = (cp, cid, gid)
    widths, cursor, array = {}, 0, child.get("/W", [])
    while cursor < len(array):
        start, next_value = int(array[cursor]), array[cursor + 1]
        if isinstance(next_value, list):
            widths.update((start + i, float(w)) for i, w in enumerate(next_value))
            cursor += 2
        else:
            widths.update(
                (i, float(array[cursor + 2])) for i in range(start, int(next_value) + 1)
            )
            cursor += 3
    return dict(
        font=font,
        child=child,
        identity=obj_id(value),
        payload_identity=obj_id(descriptor.raw_get("/FontFile2")),
        unicode_identity=obj_id(font.raw_get("/ToUnicode")),
        encoding_identity=None if identity else obj_id(font.raw_get("/Encoding")),
        payload=payload,
        sha256=digest(payload),
        sfnt=sfnt,
        spaces=spaces,
        unicode_spaces=unicode_spaces,
        entries=entries,
        widths=widths,
        default_width=float(child.get("/DW", 1000)),
        is_identity=identity,
    )


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


def shift(matrix, x, y):
    return compose(matrix, (1, 0, 0, 1, x, y))


def point(matrix, x=0, y=0):
    a, b, c, d, e, f = matrix
    return [a * x + c * y + e, b * x + d * y + f]


def inspect_pdf(reader):
    """Independent stream interpreter, including empty operations and all appearances."""
    records, glyphs, operations = {}, defaultdict(list), []
    defaults = dict(
        ctm=IDENTITY,
        tm=IDENTITY,
        line=IDENTITY,
        font=None,
        size=0.0,
        tc=0.0,
        tw=0.0,
        hz=1.0,
        rise=0.0,
        leading=0.0,
        known=True,
    )

    def walk(content, resources, path, initial, active):
        resources = resources.get_object()
        local = {}
        for name, value in resources.get("/Font", {}).items():
            record = font_record(value)
            if record:
                binding = path + "/font" + str(name)
                records[binding] = record
                local[str(name)] = binding
        state, stack, ordinal, show_ordinal = copy.copy(initial), [], 0, 0

        def newline():
            state["line"] = shift(state["line"], 0, -state["leading"])
            state["tm"], state["known"] = state["line"], True

        def show(value):
            binding = local.get(state["font"])
            if not binding:
                # Simple footer is checked separately by unchanged stream/raster.

                state["known"] = False
                return
            assert state["known"], "unknown preceding simple-font advance"
            record, data = records[binding], value.original_bytes
            decoded = codes(data, record["spaces"])
            assert decoded == codes(data, record["unicode_spaces"])
            for code in decoded:
                cp, cid, gid = record["entries"][code]
                sfnt = record["sfnt"]
                factor = state["size"] / sfnt["head"].unitsPerEm
                matrix = compose(
                    state["ctm"],
                    compose(
                        state["tm"],
                        (factor * state["hz"], 0, 0, factor, 0, state["rise"]),
                    ),
                )
                pen = BoundsPen(sfnt.getGlyphSet())
                sfnt.getGlyphSet()[sfnt.getGlyphOrder()[gid]].draw(
                    TransformPen(pen, matrix)
                )
                width = record["widths"].get(cid, record["default_width"])
                word = state["tw"] if code == b"\x20" else 0.0
                advance = (width * state["size"] / 1000 + state["tc"] + word) * state[
                    "hz"
                ]
                row = dict(
                    binding=binding,
                    show_ordinal=show_ordinal,
                    code_hex=code.hex(),
                    byte_count=len(code),
                    value=int.from_bytes(code, "big"),
                    cid=cid,
                    gid=gid,
                    unicode=cp,
                    width=width,
                    origin_pt=point(
                        compose(state["ctm"], shift(state["tm"], 0, state["rise"]))
                    ),
                    advance_pt=advance,
                    effective_tw=state["tw"],
                    effective_tc=state["tc"],
                    tw_applies=code == b"\x20",
                    tw_advance_pt=word * state["hz"],
                    matrix=list(matrix),
                    bounds_pt=list(pen.bounds) if pen.bounds else None,
                )
                glyphs[binding].append(row)
                state["tm"] = shift(state["tm"], advance, 0)

        for args, operator in ContentStream(content, reader).operations:
            if operator == b"q":
                stack.append(copy.copy(state))
            elif operator == b"Q":
                assert stack
                # Text matrices are not saved/restored by q/Q; text parameters are.

                tm, line, known = state["tm"], state["line"], state["known"]
                state = stack.pop()
                state.update(tm=tm, line=line, known=known)
            elif operator == b"cm":
                state["ctm"] = compose(state["ctm"], tuple(float(x) for x in args))
            elif operator == b"BT":
                state["tm"] = state["line"] = IDENTITY
                state["known"] = True
            elif operator == b"Tf":
                state["font"], state["size"] = str(args[0]), float(args[1])
            elif operator in (b"Tw", b"Tc", b"Tz", b"Ts", b"TL"):
                item = {
                    b"Tw": "tw",
                    b"Tc": "tc",
                    b"Tz": "hz",
                    b"Ts": "rise",
                    b"TL": "leading",
                }[operator]
                state[item] = float(args[0]) / (100 if item == "hz" else 1)
            elif operator == b"Tm":
                state["tm"] = state["line"] = tuple(float(x) for x in args)
                state["known"] = True
            elif operator in (b"Td", b"TD"):
                x, y = map(float, args)
                if operator == b"TD":
                    state["leading"] = -y
                state["line"] = shift(state["line"], x, y)
                state["tm"], state["known"] = state["line"], True
            elif operator == b"T*":
                newline()
            elif operator in (b"Tj", b"TJ", b"'", b'"'):
                before_tm, before_tw, before_tc = state["tm"], state["tw"], state["tc"]
                if operator == b'"':
                    state["tw"], state["tc"] = float(args[0]), float(args[1])
                if operator in (b"'", b'"'):
                    newline()
                values = args[0] if operator == b"TJ" else [args[-1]]
                binding = local.get(state["font"])
                operation = dict(
                    stream=path,
                    binding=binding,
                    show_ordinal=show_ordinal,
                    operator=operator.decode(),
                    before_tm=list(before_tm),
                    before_tw=before_tw,
                    before_tc=before_tc,
                    effective_tw=state["tw"],
                    effective_tc=state["tc"],
                    horizontal_scale=state["hz"],
                    size=state["size"],
                    start_tm=list(state["tm"]),
                    strings_hex=[
                        v.original_bytes.hex()
                        for v in values
                        if hasattr(v, "original_bytes")
                    ],
                    adjustments=[
                        float(v) for v in values if not hasattr(v, "original_bytes")
                    ],
                )
                for value in values:
                    if hasattr(value, "original_bytes"):
                        show(value)
                    else:
                        state["tm"] = shift(
                            state["tm"],
                            -float(value) * state["size"] * state["hz"] / 1000,
                            0,
                        )
                operation["end_tm"] = list(state["tm"])
                operations.append(operation)
                show_ordinal += 1
            elif operator == b"Do":
                form = resources["/XObject"][args[0]].get_object()
                if form.get("/Subtype") == "/Form":
                    identity = obj_id(form)
                    assert identity not in active
                    nested = copy.copy(state)
                    nested["ctm"] = compose(
                        state["ctm"],
                        tuple(float(v) for v in form.get("/Matrix", IDENTITY)),
                    )
                    walk(
                        form,
                        form.get("/Resources", resources),
                        path + "/form/" + str(ordinal),
                        nested,
                        active | {identity},
                    )
                    ordinal += 1
        assert not stack

    def appearance(value, resources, path):
        value = value.get_object()
        if isinstance(value, StreamObject):
            state = copy.copy(defaults)
            state["ctm"] = tuple(float(v) for v in value.get("/Matrix", IDENTITY))
            # Appearance geometry is frozen in its own coordinate system.

            walk(
                value, value.get("/Resources", resources), path, state, {obj_id(value)}
            )
        else:
            for name, child in value.items():
                appearance(child, resources, path + "/" + str(name).lstrip("/"))

    for index, page in enumerate(reader.pages):
        resources, path = page["/Resources"], "page/" + str(index)
        walk(page.get_contents(), resources, path, defaults, set())
        for number, annotation in enumerate(page.get("/Annots", [])):
            for state, value in annotation.get_object().get("/AP", {}).items():
                appearance(
                    value,
                    resources,
                    path + "/AP/" + str(number) + "/" + str(state).lstrip("/"),
                )
    return records, dict(glyphs), operations


def glyph_signature(record, gid):
    font = record["sfnt"]
    name = font.getGlyphOrder()[gid]
    glyph = font["glyf"][name]
    coords, ends, flags = glyph.getCoordinates(font["glyf"])
    pen = DecomposingRecordingPen(font.getGlyphSet())
    font.getGlyphSet()[name].draw(pen)
    return (
        repr(pen.value),
        tuple(tuple(p) for p in coords),
        tuple(ends),
        bytes(flags),
        tuple(font["hmtx"][name]),
        bytes(glyph.program.getBytecode()) if hasattr(glyph, "program") else b"",
        (
            tuple((c.flags, tuple(c.getComponentInfo()[1])) for c in glyph.components)
            if glyph.isComposite()
            else ()
        ),
    )


def closure(record, roots):
    result, active, font = set(), set(), record["sfnt"]

    def visit(gid):
        assert gid not in active
        if gid in result:
            return
        active.add(gid)
        glyph = font["glyf"][font.getGlyphOrder()[gid]]
        if glyph.isComposite():
            for component in glyph.components:
                visit(font.getGlyphID(component.glyphName))
        active.remove(gid)
        result.add(gid)

    for gid in roots:
        visit(gid)
    return result


def sfnt_checks(record):
    payload, font = record["payload"], record["sfnt"]

    def checksum(data):
        data += bytes((-len(data)) % 4)
        return sum(struct.unpack(">" + "I" * (len(data) // 4), data)) & 0xFFFFFFFF

    assert checksum(payload) == 0xB1B0AFBA
    for index in range(struct.unpack_from(">H", payload, 4)[0]):
        tag, expected, offset, length = struct.unpack_from(
            ">4sIII", payload, 12 + index * 16
        )
        data = payload[offset : offset + length]
        if tag == b"head":
            data = data[:8] + bytes(4) + data[12:]
        assert checksum(data) == expected, ("SFNT table checksum", tag)
    assert font["maxp"].maxZones == 1
    assert all(
        getattr(font["maxp"], k) == 0
        for k in (
            "maxTwilightPoints",
            "maxStorage",
            "maxFunctionDefs",
            "maxInstructionDefs",
        )
    )
    baseline = font.getTableData("maxp")
    font["maxp"].recalc(font)
    assert font.getTableData("maxp") == baseline


def same_numbers(left, right, tolerance=1e-8):
    if left is None or right is None:
        return left == right
    assert len(left) == len(right)
    return max((abs(a - b) for a, b in zip(left, right)), default=0) <= tolerance


def dev42_diagnosis():
    base = ROOT / "fixtures/redaction-copy/mixed-cmap-v2"
    raw = (base / "criteria.json").read_bytes()
    assert digest(raw) == DEV42_PIN
    fixed = json.loads(raw)
    report_path = ROOT / "evidence/dev42-independent-full-mixed-cmap-r1.json"
    report = json.loads(report_path.read_bytes())
    failed = [r for r in report["results"] if r["status"] == "FAIL"]
    assert [r["file"] for r in failed] == [
        "code32-nonspace.pdf",
        "shared-pages-alias.pdf",
        "shared-sibling-form.pdf",
        "shared-appearance-states.pdf",
        "UI_atomicity",
    ]
    rows = []
    for case in fixed["cases"]:
        if case["file"] not in {r["file"] for r in failed} | {"mixed-value-length.pdf"}:
            continue
        source = base / case["file"]
        assert digest(source.read_bytes()) == case["sha256"]
        records, usage, operations = inspect_pdf(PdfReader(source))
        collisions = []
        for binding, record in records.items():
            groups = defaultdict(list)
            for code, (cp, cid, gid) in record["entries"].items():
                groups[int.from_bytes(code, "big")].append(
                    dict(
                        code_hex=code.hex(),
                        byte_count=len(code),
                        unicode=cp,
                        cid=cid,
                        gid=gid,
                    )
                )
            collisions += [
                dict(binding=binding, integer_value=n, routes=values)
                for n, values in groups.items()
                if len(values) > 1
            ]
        rows.append(
            dict(
                file=case["file"],
                source_sha256=case["sha256"],
                collision_routes=collisions,
                source_glyphs=sum(map(len, usage.values())),
                effective_spacing=[
                    dict(
                        binding=o["binding"],
                        operator=o["operator"],
                        Tw=o["effective_tw"],
                        Tc=o["effective_tc"],
                    )
                    for o in operations
                    if o["binding"]
                ],
                prior_direct_failure=case["file"] in {r["file"] for r in failed},
            )
        )
    package = distribution("pypdfium2")
    version_path = Path(package.locate_file("pypdfium2_raw/version.json"))
    binding_path = Path(package.locate_file("pypdfium2_raw/bindings.py"))
    dll_path = Path(package.locate_file("pypdfium2_raw/pdfium.dll"))
    return dict(
        status="SOURCE_TRACE_WITH_BINARY_COMMIT_LIMITATION",
        source_only=True,
        prior_contract=dict(
            passed=41, failed=5, unchanged=True, report=file_info(report_path)
        ),
        local_binary=dict(
            wrapper_version=package.version,
            metadata=json.loads(version_path.read_bytes()),
            version_metadata=file_info(version_path),
            ctypes_bindings=file_info(binding_path),
            dll=file_info(dll_path),
        ),
        results=rows,
        UI_source="shared-pages-alias.pdf",
        upstream_source=dict(
            url="https://pdfium.googlesource.com/pdfium/%2B/e697afe4fa90045e56f7bda475bab18575ce06ea/core/fpdfapi/font/cpdf_cmap.cpp",
            commit="e697afe4fa90045e56f7bda475bab18575ce06ea",
            blob="f43c072902a0b89a755e5f0d8a27544143195264",
            trace="GetNextChar returns integer after codespace matching; CIDFromCharCode indexes by that integer. Code length is absent from lookup key; 41 and 0041 therefore collide.",
            qualification="Mechanism inference consistent with literal prior controls; upstream commit is not proved to be the installed binary commit (metadata hash null).",
        ),
        new_runtime_executions=0,
        thresholds_changed=False,
        general_M1_M3_M5_acceptance="withheld",
    )


def source_contract(args):
    raw = args.criteria.read_bytes()
    assert digest(raw) == args.criteria_sha256
    fixed = json.loads(raw)
    assert fixed.get("position_error_pt", 0.5) == 0.5
    assert fixed.get("outside_pixel_difference", 0) == 0
    assert fixed.get("boundary_pixels", 1) == 1
    rows = []
    for case in fixed["cases"]:
        path = args.criteria.parent / case["file"]
        assert digest(path.read_bytes()) == case["sha256"]
        primary_content = PdfReader(path).pages[0].get_contents().get_data()
        assert primary_content == case["content_bytes"].encode()
        assert digest(primary_content) == case["content_sha256"]
        row = dict(
            file=case["file"],
            status="FAIL",
            source_sha256=case["sha256"],
            product_runtime="UNEXECUTED",
        )
        try:
            records, uses, operations = inspect_pdf(PdfReader(path))
            if case.get("refused"):
                row.update(
                    status="PASS_INTEGRITY_ONLY_REFUSAL_RUNTIME_UNEXECUTED",
                    bindings=len(records),
                )
            else:
                audit_case(case, records, uses, operations)
                audit_frozen_glyphs(fixed, records)
                row.update(
                    status="PASS_SOURCE_INSPECTION",
                    bindings=len(records),
                    glyphs=sum(map(len, uses.values())),
                    raw_single_byte32=sum(
                        g["tw_applies"] for gs in uses.values() for g in gs
                    ),
                    unicode_spaces=sum(
                        g["unicode"] == " " for gs in uses.values() for g in gs
                    ),
                    show_operations_verified=sum(
                        bool(o["binding"]) for o in operations
                    ),
                    independently_parsed_show_operations_sha256=digest(
                        json.dumps(
                            [o for o in operations if o["binding"]],
                            sort_keys=True,
                            separators=(",", ":"),
                        ).encode()
                    ),
                    independent_true_Bezier_bounds=True,
                )
        except Exception as error:
            if case.get("refused"):
                row.update(
                    status="PASS_INTEGRITY_ONLY_REFUSAL_RUNTIME_UNEXECUTED",
                    source_rejection=repr(error),
                )
            else:
                row.update(error=repr(error), traceback=traceback.format_exc())
        rows.append(row)
    return dict(
        status=(
            "PASS_SOURCE_INSPECTION"
            if all(r["status"] != "FAIL" for r in rows)
            else "FAIL"
        ),
        source_only=True,
        source_inspections=sum(r["status"] != "FAIL" for r in rows),
        runtime_passes=0,
        failed=sum(r["status"] == "FAIL" for r in rows),
        criteria_sha256=digest(raw),
        results=rows,
        general_M1_M3_M5_acceptance="withheld",
    )


def audit_case(case, records, uses, operations):
    """Criteria fields are checked against independent PDF bytes and font bounds."""
    binding_rows = {r["path"]: r for r in case["font_bindings"]}
    assert set(uses) == set(binding_rows), "source binding paths"
    for binding, glyphs in uses.items():
        assert "".join(g["unicode"] for g in glyphs) == binding_rows[binding]["text"]
    primary_binding = next(p for p in uses if p.startswith("page/0/font"))
    primary_expected = next(
        o["entries"] for o in binding_rows[primary_binding]["streams"] if o["entries"]
    )
    primary = [
        g
        for g in uses[primary_binding]
        if g["show_ordinal"] == (1 if case.get("empty_operator") else 0)
    ]
    for measured, expected in zip(primary, primary_expected):
        for field in ("code_hex", "cid", "gid", "unicode", "width", "advance_pt"):
            assert measured[field] == expected[field] or (
                isinstance(measured[field], (int, float))
                and abs(measured[field] - expected[field]) < 1e-8
            ), (field, measured, expected)
        assert same_numbers(measured["origin_pt"], expected["origin_pt"])
        assert same_numbers(measured["bounds_pt"], expected["outline_box_pt"])
        assert (
            abs(
                measured["tw_advance_pt"]
                - expected["raw_code_word_spacing"] * case["horizontal_scale"]
            )
            < 1e-8
        )
        assert measured["tw_applies"] == (expected["code_hex"] == "20")
        assert measured["effective_tw"] == case["effective_tw"]
        assert measured["effective_tc"] == case["effective_tc"]
    assert len(primary) == len(primary_expected)
    for binding, condition in binding_rows.items():
        actual_operations = [o for o in operations if o["binding"] == binding]
        expected_operations = condition["streams"]
        assert len(actual_operations) == len(expected_operations), (
            "show-operation count",
            binding,
            len(actual_operations),
            len(expected_operations),
        )
        for actual, expected in zip(actual_operations, expected_operations):
            assert actual["operator"] == expected["operator"]
            assert same_numbers(actual["start_tm"][4:], expected["origin_pt"])
            assert same_numbers(actual["end_tm"][4:], expected["end_origin_pt"])
            for field in ("effective_tw", "effective_tc", "before_tw", "before_tc"):
                if field in expected:
                    assert abs(actual[field] - expected[field]) < 1e-8
            if "before_origin_pt" in expected:
                assert same_numbers(
                    actual["before_tm"][4:], expected["before_origin_pt"]
                )
            gs = [
                g for g in uses[binding] if g["show_ordinal"] == actual["show_ordinal"]
            ]
            assert len(gs) == len(expected["entries"])
            for measured, truth in zip(gs, expected["entries"]):
                for field in ("code_hex", "cid", "gid", "unicode", "width"):
                    assert measured[field] == truth[field], (binding, field)
                assert same_numbers(measured["origin_pt"], truth["origin_pt"])
                assert same_numbers(measured["bounds_pt"], truth["outline_box_pt"])
                assert abs(measured["advance_pt"] - truth["advance_pt"]) < 1e-8
                assert (
                    abs(
                        measured["tw_advance_pt"]
                        - truth["raw_code_word_spacing"] * actual["horizontal_scale"]
                    )
                    < 1e-8
                )
    original = records[next(p for p in uses if p.startswith("page/0/font"))]
    assert original["sha256"] == case["source_sfnt_sha256"]
    assert digest(original["font"]["/Encoding"].get_data()) == case["encoding_sha256"]
    assert digest(original["font"]["/ToUnicode"].get_data()) == case["tounicode_sha256"]
    assert {
        (len(code), int.from_bytes(code, "big"), cid, gid)
        for code, (cp, cid, gid) in original["entries"].items()
    } == {
        (g["byte_count"], g["value"], g["cid"], g["gid"])
        for g in case["encoding_route"]
    }
    assert {
        (len(code), int.from_bytes(code, "big"), cp)
        for code, (cp, cid, gid) in original["entries"].items()
    } == {(g["byte_count"], g["value"], g["unicode"]) for g in case["unicode_route"]}
    removed = [
        g["bounds_pt"]
        for g, e in zip(primary, primary_expected)
        if e["removed"] and g["bounds_pt"]
    ]
    kept = [
        g["bounds_pt"]
        for g, e in zip(primary, primary_expected)
        if not e["removed"] and g["bounds_pt"]
    ]
    x, y, w, h = case["rect"]
    rect = (x, y, x + w, y + h)
    assert removed and all(
        rect[0] <= b[0] <= b[2] <= rect[2] and rect[1] <= b[1] <= b[3] <= rect[3]
        for b in removed
    )
    assert all(
        b[2] <= rect[0] or rect[2] <= b[0] or b[3] <= rect[1] or rect[3] <= b[1]
        for b in kept
    ), "removal rectangle touches retained outline"
    # Empty-space selection is a semantic contract too. The initial prefreeze
    # inspection checked ink bounds but missed seven inconsistent v2 flags.
    # Keep that evidence literal; new source audits must check the full signed
    # width + Tc + raw-code Tw interval against the existing rectangle.
    for measured, expected in zip(primary, primary_expected):
        if measured["unicode"] != " " or measured["bounds_pt"] is not None:
            continue
        origin_x, origin_y = measured["origin_pt"]
        end_x = origin_x + measured["advance_pt"]
        covered = (
            rect[0] <= min(origin_x, end_x)
            and max(origin_x, end_x) <= rect[2]
            and rect[1] <= origin_y <= rect[3]
        )
        assert expected["removed"] is covered, (
            "frozen empty-space selection conflicts with full advance rule",
            measured["code_hex"],
            measured["origin_pt"],
            end_x,
            expected["removed"],
            covered,
        )


def audit_frozen_glyphs(fixed, records):
    source = ROOT / fixed["original_source"]["path"]
    assert digest(source.read_bytes()) == fixed["original_source"]["sha256"]
    assert (
        digest((ROOT / fixed["license"]["path"]).read_bytes())
        == fixed["license"]["sha256"]
    )
    assert (
        digest((ROOT / "assets/fonts/NotoSansJP.ttf").read_bytes())
        == fixed["license"]["source_font_sha256"]
    )
    by_sha = {r["sha256"]: r for r in records.values()}
    assert set(by_sha) == {fixed["original_source"]["sfnt_sha256"]}
    record = next(iter(by_sha.values()))
    font = record["sfnt"]
    for truth in fixed["source_glyphs"]:
        name = font.getGlyphOrder()[truth["gid"]]
        glyph = font["glyf"][name]
        coords, ends, flags = glyph.getCoordinates(font["glyf"])
        assert (
            digest(
                json.dumps([list(p) for p in coords], separators=(",", ":")).encode()
            )
            == truth["coordinates_sha256"]
        )
        assert (
            list(ends) == truth["endpoints"]
            and digest(bytes(flags)) == truth["flags_sha256"]
        )
        assert list(font["hmtx"][name]) == truth["metrics"]
        program = (
            bytes(glyph.program.getBytecode()) if hasattr(glyph, "program") else b""
        )
        assert program.hex() == truth["instruction_hex"]
        components = (
            [font.getGlyphID(c.glyphName) for c in glyph.components]
            if glyph.isComposite()
            else []
        )
        assert components == truth["components"]
        pen = BoundsPen(font.getGlyphSet())
        font.getGlyphSet()[name].draw(pen)
        assert same_numbers(
            list(pen.bounds) if pen.bounds else None, truth["true_bezier_bounds"]
        )


def retained_source(case, uses):
    binding_rows = {r["path"]: r for r in case["font_bindings"]}
    result = {}
    for binding, glyphs in uses.items():
        expected = [
            g
            for operation in binding_rows[binding]["streams"]
            for g in operation["entries"]
        ]
        assert len(expected) == len(glyphs)
        result[binding] = [
            g for g, truth in zip(glyphs, expected) if not truth["removed"]
        ]
    return {p: gs for p, gs in result.items() if gs}


def output_contract(source, target, case):
    before, after = PdfReader(source), PdfReader(target)
    old, old_uses, source_operations = inspect_pdf(before)
    new, new_uses, target_operations = inspect_pdf(after)
    expected = retained_source(case, old_uses)
    raw_eligible = {
        binding
        for binding, glyphs in old_uses.items()
        if not old[binding]["is_identity"]
        and any(g["effective_tw"] != 0 for g in glyphs)
    }
    assert set(new_uses) == set(expected), "output binding uses"
    max_error = 0.0
    for binding, glyphs in new_uses.items():
        truth = expected[binding]
        assert new[binding]["is_identity"] is (
            binding not in raw_eligible
        ), "RAW/default binding selector"
        assert len(glyphs) == len(truth)
        assert [g["unicode"] for g in glyphs] == [g["unicode"] for g in truth]
        for actual, original in zip(glyphs, truth):
            assert (
                actual["width"] == original["width"]
            ), "PDF declared glyph width changed"
            if binding in raw_eligible:
                assert (
                    actual["code_hex"] == original["code_hex"]
                ), "RAW source code key changed"
                assert (
                    actual["tw_applies"] == original["tw_applies"]
                ), "RAW Tw eligibility changed"
            assert glyph_signature(new[binding], actual["gid"]) == glyph_signature(
                old[binding], original["gid"]
            ), ("glyph flags/metrics/instructions/outline", binding)
            for field in ("origin_pt", "bounds_pt"):
                a, b = actual[field], original[field]
                if a is None or b is None:
                    assert a == b
                else:
                    max_error = max(max_error, max(abs(x - y) for x, y in zip(a, b)))
    assert max_error <= 0.5, ("position error", max_error)
    # Final text cursor is observed independently even when a last span was removed.

    old_ops = [o for o in source_operations if o["binding"]]
    new_ops = [o for o in target_operations if o["binding"]]
    assert len(old_ops) == len(new_ops), "show-operation count changed"
    for original, actual in zip(old_ops, new_ops):
        assert (
            original["stream"] == actual["stream"]
            and original["binding"] == actual["binding"]
        )
        assert same_numbers(original["end_tm"], actual["end_tm"], 0.5), (
            "text cursor",
            original,
            actual,
        )
        for field in ("effective_tw", "effective_tc", "horizontal_scale", "size"):
            assert original[field] == actual[field], ("effective show state", field)
    font_groups, payload_groups = defaultdict(list), defaultdict(list)
    for binding in new_uses:
        font_groups[new[binding]["identity"]].append(binding)
        payload_groups[new[binding]["payload_identity"]].append(binding)
    for bindings in font_groups.values():
        record = new[bindings[0]]
        raw = any(p in raw_eligible for p in bindings)
        assert record["is_identity"] is (
            not raw
        ), "source-derived RAW/default font selector"
        if raw:
            wanted = {
                bytes.fromhex(g["code_hex"]) for p in bindings for g in expected[p]
            }
            assert set(record["entries"]) == wanted, "RAW unused or missing source keys"
            source_cid_equivalence = defaultdict(set)
            for p in bindings:
                for glyph in expected[p]:
                    code = bytes.fromhex(glyph["code_hex"])
                    cp, cid, gid = record["entries"][code]
                    assert cp == glyph["unicode"]
                    assert glyph_signature(record, gid) == glyph_signature(
                        old[p], glyph["gid"]
                    )
                    source_cid_equivalence[(old[p]["identity"], glyph["cid"])].add(cid)
            assert all(
                len(cids) == 1 for cids in source_cid_equivalence.values()
            ), "RAW source CID aliases split"
        allowed = {
            (g["unicode"], glyph_signature(old[p], g["gid"]))
            for p in bindings
            for g in expected[p]
        }
        mapped = {
            (cp, glyph_signature(record, gid))
            for cp, cid, gid in record["entries"].values()
        }
        assert mapped == allowed, ("unused/missing font mappings", bindings)
    reports, old_union, new_union = [], defaultdict(set), defaultdict(set)
    known_payloads, known_unicode, known_encoding = set(), set(), set()
    for bindings in payload_groups.values():
        source_shas = {old[p]["sha256"] for p in bindings}
        assert len(source_shas) == 1
        source_sha = next(iter(source_shas))
        original, actual = old[bindings[0]], new[bindings[0]]
        roots = {g["gid"] for p in bindings for g in expected[p]}
        actual_roots = {g["gid"] for p in bindings for g in new_uses[p]}
        old_closed, new_closed = closure(original, roots), closure(actual, actual_roots)
        assert set(range(len(actual["sfnt"].getGlyphOrder()))) == new_closed | {
            0
        }, "orphan GID"
        assert Counter(glyph_signature(original, g) for g in old_closed) == Counter(
            glyph_signature(actual, g) for g in new_closed
        ), "local glyph closure"
        source_head = [
            getattr(original["sfnt"]["head"], k)
            for k in ("xMin", "yMin", "xMax", "yMax")
        ]
        target_head = [
            getattr(actual["sfnt"]["head"], k) for k in ("xMin", "yMin", "xMax", "yMax")
        ]
        assert source_head == target_head, "frozen source global head bounds"
        for gid in new_closed:
            sig = glyph_signature(actual, gid)
            matches = {g for g in old_closed if glyph_signature(original, g) == sig}
            assert matches
            new_union[source_sha].update(matches)
        old_union[source_sha].update(old_closed)
        sfnt_checks(actual)
        known_payloads.add(actual["payload_identity"])
        known_unicode.update(new[p]["unicode_identity"] for p in bindings)
        known_encoding.update(
            new[p]["encoding_identity"]
            for p in bindings
            if new[p]["encoding_identity"] is not None
        )
        reports.append(
            dict(
                bindings=bindings,
                original_sfnt_sha256=source_sha,
                output_sfnt_sha256=actual["sha256"],
                local_source_roots=sorted(roots),
                local_source_closure=sorted(old_closed),
                local_output_roots=sorted(actual_roots),
                local_output_closure=sorted(new_closed),
                source_head_bounds=source_head,
                checksum_verified=True,
            )
        )
    assert old_union == new_union, "same source SHA final reference union"
    target_fonts = {
        obj_id(v)
        for v in objects(after)
        if isinstance(v, dict) and v.get("/Subtype") == "/Type0"
    }
    assert target_fonts == set(font_groups), "orphan Type0 font"
    forbidden_sha = {digest(p.get_contents().get_data()) for p in before.pages}
    for value in objects(before):
        if isinstance(value, dict):
            for key in ("/FontFile2", "/Encoding", "/ToUnicode", "/CIDToGIDMap"):
                stream = value.get(key)
                if stream is not None and isinstance(stream.get_object(), StreamObject):
                    forbidden_sha.add(digest(stream.get_object().get_data()))
    forbidden = [
        case["remove"].encode(enc) for enc in ("utf-8", "utf-16-be", "utf-16-le")
    ]
    primary = next(
        o["entries"] for o in case["font_bindings"][0]["streams"] if o["entries"]
    )
    sequence = b"".join(bytes.fromhex(g["code_hex"]) for g in primary if g["removed"])
    forbidden += [sequence.hex().encode(), sequence.hex().upper().encode()]
    for value in objects(after):
        if isinstance(value, StreamObject):
            data = value.get_data()
            assert (
                digest(data) not in forbidden_sha
            ), "old expanded content/map/font remains"
            assert not any(
                p in data for p in forbidden
            ), "removed phrase/code sequence remains"
            if b"begincmap" in data:
                assert (
                    obj_id(value) in known_unicode | known_encoding
                ), "orphan CMap stream"
            if data.startswith((b"\0\1\0\0", b"true", b"OTTO")):
                assert obj_id(value) in known_payloads, "orphan SFNT stream"
        if isinstance(value, dict) and "/FontFile2" in value:
            assert obj_id(value.raw_get("/FontFile2")) in known_payloads
    assert not any(
        p in target.read_bytes() for p in forbidden
    ), "removed data in raw PDF"
    assert len(before.pages) == len(after.pages)
    for old_page, new_page in zip(before.pages, after.pages):
        for key in ("/MediaBox", "/CropBox", "/Rotate", "/UserUnit"):
            assert old_page.get(key) == new_page.get(key)
    return dict(
        max_position_error_pt=max_error,
        local_fonts=reports,
        original_SFNT_final_union={k: sorted(v) for k, v in old_union.items()},
        all_font_glyph_closures_and_old_data_checked=True,
        raw_code_to_CID_GID_and_Unicode_independent=True,
        RAW_source_keys_only_for_nonzero_Tw_bindings=sorted(raw_eligible),
        effective_show_end_text_matrices_verified=True,
    )


def raster_compare(source, target, dpi, page_index, rect, engine, temp):
    import ctypes
    import numpy as np
    import pypdfium2 as pdfium
    import pypdfium2.raw as raw
    from PIL import Image

    with pdfium.PdfDocument(source) as old, pdfium.PdfDocument(target) as new:
        page = old[page_index]
        if engine is None:
            a = np.array(page.render(scale=dpi / 72).to_pil().convert("RGB"))
            b = np.array(new[page_index].render(scale=dpi / 72).to_pil().convert("RGB"))
        else:
            env = dict(
                os.environ, TEMP=str(temp), TMP=str(temp), PYTHONDONTWRITEBYTECODE="1"
            )

            def render(path):
                proc = subprocess.run(
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
                    env=env,
                    check=True,
                    timeout=60,
                )
                return np.array(Image.open(io.BytesIO(proc.stdout)).convert("RGB"))

            a, b = render(source), render(target)
        assert a.shape == b.shape
        delta = np.any(a != b, axis=2)
        if rect:
            x, y, width, height = rect
            h, w = delta.shape
            corners = []
            for px, py in (
                (x, y),
                (x + width, y),
                (x, y + height),
                (x + width, y + height),
            ):
                dx, dy = ctypes.c_int(), ctypes.c_int()
                assert raw.FPDF_PageToDevice(
                    page.raw,
                    0,
                    0,
                    w * 200,
                    h * 200,
                    0,
                    px,
                    py,
                    ctypes.byref(dx),
                    ctypes.byref(dy),
                )
                corners.append((dx.value / 200, dy.value / 200))
            x0 = max(0, math.floor(min(p[0] for p in corners)) - 1)
            x1 = min(w, math.ceil(max(p[0] for p in corners)) + 1)
            y0 = max(0, math.floor(min(p[1] for p in corners)) - 1)
            y1 = min(h, math.ceil(max(p[1] for p in corners)) + 1)
            delta[y0:y1, x0:x1] = False
        return int(np.count_nonzero(delta))


class ExactControlNumber(FloatObject):
    def write_to_stream(self, stream, encryption_key=None):
        stream.write(format(float(self), ".15g").encode())


def float32(value):
    return struct.unpack("<f", struct.pack("<f", value))[0]


def float32_kerning(cursor, number, size):
    return float32(cursor - float32(float32(float32(number) * float32(size)) / 1000))


def float32_glyph(cursor, width, size, word, spacing):
    cursor = float32(cursor + float32(float32(float32(width) * float32(size)) / 1000))
    cursor = float32(cursor + float32(word))
    return float32(cursor + float32(spacing))


def reversible_kerning(cursor, desired, size, nominal):
    base = struct.unpack("<I", struct.pack("<f", nominal))[0]
    for distance in range(257):
        for delta in (0,) if not distance else (-distance, distance):
            bits = base + delta
            if not 0 <= bits < 2**32:
                continue
            value = struct.unpack("<f", struct.pack("<I", bits))[0]
            if math.isfinite(value) and float32_kerning(cursor, value, size) == desired:
                return value, delta
    raise AssertionError(("no reversible aggregate kerning", cursor, desired, nominal))


def raw_source_control(source, case):
    """Memory-only source transformation; full original SFNT is not redaction-safe."""
    data = source.read_bytes()
    original = PdfReader(io.BytesIO(data))
    records, uses, original_ops = inspect_pdf(original)
    audit_case(case, records, uses, original_ops)
    remaining = retained_source(case, uses)
    writer = PdfWriter(clone_from=io.BytesIO(data))
    body = next(r for r in case["font_bindings"] if r["path"].startswith("page/0/"))
    primary = body["path"]
    original_font = records[primary]
    truth = [g for o in body["streams"] for g in o["entries"]]
    truth_at = 0
    replays = []
    state, stack = dict(font=None, size=0.0, tw=0.0, tc=0.0), []
    content = ContentStream(writer.pages[0].get_contents(), writer)
    output_ops = []

    def exact(value):
        if isinstance(value, FloatObject):
            return ExactControlNumber(float(value))
        if isinstance(value, ArrayObject):
            return ArrayObject(exact(v) for v in value)
        return value

    for operands, operator in content.operations:
        operands = [exact(v) for v in operands]
        if operator == b"q":
            stack.append(copy.copy(state))
        elif operator == b"Q":
            state = stack.pop()
        elif operator == b"Tf":
            state["font"], state["size"] = str(operands[0]), float(operands[1])
        elif operator in (b"Tw", b"Tc"):
            state["tw" if operator == b"Tw" else "tc"] = float(operands[0])
        if operator == b'"':
            state["tw"], state["tc"] = map(float, operands[:2])
        if operator not in (b"Tj", b"TJ", b"'", b'"') or primary != (
            "page/0/font" + str(state["font"])
        ):
            output_ops.append((operands, operator))
            continue
        values = operands[0] if operator == b"TJ" else [operands[-1]]
        # PDFium combines adjacent numeric TJ entries at each string boundary.
        segments, initial = [], 0.0
        for value in values:
            if hasattr(value, "original_bytes"):
                if value.original_bytes:
                    segments.append([value.original_bytes, 0.0])
            elif segments:
                segments[-1][1] = float32(segments[-1][1] + float32(float(value)))
            else:
                initial = float32(initial + float32(float(value)))
        if not segments:
            output_ops.append((operands, operator))
            continue
        entries = []
        for payload, kern in segments:
            for code in codes(payload, original_font["spaces"]):
                cp, cid, gid = original_font["entries"][code]
                expected = truth[truth_at]
                truth_at += 1
                assert expected["code_hex"] == code.hex() and expected["unicode"] == cp
                entries.append((code, expected, original_font["widths"][cid]))
            entries.append((None, kern, None))
        real = [e for e in entries if e[0] is not None]
        assert (
            not real[0][1]["removed"] and not real[-1][1]["removed"]
        ), "source-control scope requires first and last original glyph retained"
        source_cursor = target_cursor = 0.0
        pending = 0.0
        selected = False
        encoded, current = ArrayObject(), bytearray()
        if initial:
            encoded.append(ExactControlNumber(initial))
        this_replay = []

        def flush_string():
            if current:
                encoded.append(ByteStringObject(bytes(current)))
                current.clear()

        for code, expected, width in entries:
            if code is None:
                kern = expected
                if selected:
                    pending -= kern * state["size"] / 1000
                elif kern:
                    flush_string()
                    encoded.append(ExactControlNumber(kern))
                    target_cursor = float32_kerning(target_cursor, kern, state["size"])
                source_cursor = float32_kerning(source_cursor, kern, state["size"])
                continue
            word = state["tw"] if code == b"\x20" else 0.0
            if expected["removed"]:
                selected = True
                pending += width * state["size"] / 1000 + word + state["tc"]
            else:
                if selected:
                    flush_string()
                    nominal = -pending * 1000 / state["size"]
                    operand, delta = reversible_kerning(
                        target_cursor, source_cursor, state["size"], nominal
                    )
                    encoded.append(ExactControlNumber(operand))
                    this_replay.append(
                        dict(
                            source_float_before_retained=source_cursor,
                            target_float_before_aggregate=target_cursor,
                            true_removed_advance=pending,
                            nominal_TJ=nominal,
                            emitted_TJ=operand,
                            adjacent_float_steps=delta,
                            real_unscaled_error_pt=abs(
                                -operand * state["size"] / 1000 - pending
                            ),
                        )
                    )
                    target_cursor = float32_kerning(
                        target_cursor, operand, state["size"]
                    )
                    pending, selected = 0.0, False
                assert target_cursor == source_cursor, (
                    "source-derived float cursor",
                    code,
                )
                current.extend(code)
                target_cursor = float32_glyph(
                    target_cursor, width, state["size"], word, state["tc"]
                )
            source_cursor = float32_glyph(
                source_cursor, width, state["size"], word, state["tc"]
            )
        assert not selected
        flush_string()
        if operator == b"'":
            output_ops.append(([], b"T*"))
        elif operator == b'"':
            output_ops.extend(
                (([operands[0]], b"Tw"), ([operands[1]], b"Tc"), ([], b"T*"))
            )
        output_ops.append(([encoded], b"TJ"))
        replays.extend(this_replay)
    assert truth_at == len(truth)
    content.operations = output_ops
    rewritten = DecodedStreamObject()
    rewritten.set_data(content.get_data())
    writer.pages[0][NameObject("/Contents")] = writer._add_object(rewritten)

    # Source CID values and full SFNT remain solely in this contrast. The fresh
    # product must separately prove subset CID/GID closure and final-used keys.
    def cmap(record, glyphs, unicode):
        used = {bytes.fromhex(g["code_hex"]): g for g in glyphs}
        op = b"bfchar" if unicode else b"cidchar"
        raw = b"/CIDInit /ProcSet findresource begin 12 dict begin begincmap\n"
        raw += (
            b"/CMapName /IndependentRawControl def /CMapType "
            + (b"2" if unicode else b"1")
            + b" def\n/WMode 0 def\n"
        )
        raw += str(len(record["spaces"])).encode() + b" begincodespacerange\n"
        for low, high in record["spaces"]:
            raw += b"<" + low.hex().encode() + b"> <" + high.hex().encode() + b">\n"
        raw += b"endcodespacerange\n" + str(len(used)).encode() + b" begin" + op + b"\n"
        for code, glyph in sorted(used.items()):
            value = (
                b"<" + glyph["unicode"].encode("utf-16-be").hex().encode() + b">"
                if unicode
                else str(glyph["cid"]).encode()
            )
            raw += b"<" + code.hex().encode() + b"> " + value + b"\n"
        return (
            raw
            + b"end"
            + op
            + b" endcmap CMapName currentdict /CMap defineresource pop end end\n"
        )

    resources = writer.pages[0]["/Resources"]
    name = NameObject(primary.split("/font", 1)[1])
    font = copy.copy(resources["/Font"][name].get_object())
    for key, unicode in (("/Encoding", False), ("/ToUnicode", True)):
        stream = DecodedStreamObject()
        stream.set_data(cmap(original_font, remaining[primary], unicode))
        font[NameObject(key)] = writer._add_object(stream)
    resources["/Font"][name] = writer._add_object(font)
    result = io.BytesIO()
    writer.write(result)
    return result.getvalue(), replays


def canonical_source_control(source):
    """Independently retain every glyph, changing raw code Tw into explicit TJ."""

    class ExactControlNumber(FloatObject):
        def write_to_stream(self, stream, encryption_key=None):
            stream.write(format(float(self), ".15g").encode())

    writer = PdfWriter(clone_from=source)
    fonts, rewritten = {}, set()

    def collect_resources(resources):
        resources = resources.get_object()
        for value in resources.get("/Font", {}).values():
            record = font_record(value)
            if record:
                fonts[obj_id(value)] = record
        for value in resources.get("/XObject", {}).values():
            form = value.get_object()
            if form.get("/Subtype") == "/Form" and "/Resources" in form:
                collect_resources(form["/Resources"])

    for page in writer.pages:
        collect_resources(page["/Resources"])
        for annotation in page.get("/Annots", []):

            def collect_ap(value):
                value = value.get_object()
                if isinstance(value, StreamObject):
                    collect_resources(value.get("/Resources", page["/Resources"]))
                else:
                    for child in value.values():
                        collect_ap(child)

            for value in annotation.get_object().get("/AP", {}).values():
                collect_ap(value)

    def rewrite(content, resources):
        resources = resources.get_object()
        cs = ContentStream(content, writer)
        ops = []
        state = dict(font=None, tw=0.0, size=0.0)
        stack = []
        for args, op in cs.operations:
            if op == b"q":
                stack.append(dict(state))
            elif op == b"Q":
                state = stack.pop()
            elif op == b"Tw":
                state["tw"] = float(args[0])
            elif op == b"Tf":
                state["font"], state["size"] = str(args[0]), float(args[1])
            elif op == b'"':
                state["tw"] = float(args[0])
            record = (
                fonts.get(obj_id(resources["/Font"][state["font"]]))
                if state["font"] and state["font"] in resources.get("/Font", {})
                else None
            )
            if op in (b"Tj", b"TJ", b"'", b'"') and record:
                if op == b"'":
                    ops.append(([], b"T*"))
                elif op == b'"':
                    ops.extend([([args[0]], b"Tw"), ([args[1]], b"Tc"), ([], b"T*")])
                values = args[0] if op == b"TJ" else [args[-1]]
                encoded = ArrayObject()
                for item in values:
                    if hasattr(item, "original_bytes"):
                        for code in codes(item.original_bytes, record["spaces"]):
                            cp, cid, gid = record["entries"][code]
                            encoded.append(ByteStringObject(cid.to_bytes(2, "big")))
                            if code == b"\x20" and state["tw"]:
                                encoded.append(
                                    ExactControlNumber(
                                        -state["tw"] * 1000 / state["size"]
                                    )
                                )
                    else:
                        encoded.append(item)
                ops.append(([encoded], b"TJ"))
            else:
                ops.append((args, op))
            if op == b"Do":
                form = resources["/XObject"][args[0]].get_object()
                if form.get("/Subtype") == "/Form" and obj_id(form) not in rewritten:
                    rewritten.add(obj_id(form))
                    form.set_data(rewrite(form, form.get("/Resources", resources)))
        cs.operations = ops
        return cs.get_data()

    for page in writer.pages:
        stream = DecodedStreamObject()
        stream.set_data(rewrite(page.get_contents(), page["/Resources"]))
        page[NameObject("/Contents")] = writer._add_object(stream)

        def rewrite_ap(value):
            value = value.get_object()
            if isinstance(value, StreamObject):
                if obj_id(value) not in rewritten:
                    rewritten.add(obj_id(value))
                    value.set_data(
                        rewrite(value, value.get("/Resources", page["/Resources"]))
                    )
            else:
                for child in value.values():
                    rewrite_ap(child)

        for annotation in page.get("/Annots", []):
            for value in annotation.get_object().get("/AP", {}).values():
                rewrite_ap(value)
    for record in fonts.values():
        mapping = {cid: cp for cp, cid, gid in record["entries"].values()}
        data = (
            b"/CIDInit /ProcSet findresource begin 12 dict begin begincmap\n/CMapType 2 def\n1 begincodespacerange <0000> <ffff> endcodespacerange\n"
            + str(len(mapping)).encode()
            + b" beginbfchar\n"
        )
        for cid, cp in sorted(mapping.items()):
            data += (
                b"<"
                + cid.to_bytes(2, "big").hex().encode()
                + b"> <"
                + cp.encode("utf-16-be").hex().encode()
                + b">\n"
            )
        data += (
            b"endbfchar endcmap CMapName currentdict /CMap defineresource pop end end\n"
        )
        stream = DecodedStreamObject()
        stream.set_data(data)
        record["font"][NameObject("/Encoding")] = NameObject("/Identity-H")
        record["font"][NameObject("/ToUnicode")] = writer._add_object(stream)
    output = io.BytesIO()
    writer.write(output)
    return output.getvalue()


def source_render_controls(args):
    import numpy as np
    import pypdfium2 as pdfium
    from PIL import Image
    from test_tools import poppler_tool

    raw = args.criteria.read_bytes()
    assert digest(raw) == args.criteria_sha256
    fixed = json.loads(raw)
    engine = poppler_tool("pdftoppm")
    args.engine_temp.resolve().relative_to(ROOT)
    args.engine_temp.mkdir(parents=True, exist_ok=True)
    env = dict(
        os.environ,
        TEMP=str(args.engine_temp.resolve()),
        TMP=str(args.engine_temp.resolve()),
        PYTHONDONTWRITEBYTECODE="1",
    )
    rows = []
    for case in fixed["cases"]:
        if case["refused"]:
            continue
        if args.case and case["file"] not in args.case:
            continue
        path = args.criteria.parent / case["file"]
        data = path.read_bytes()
        assert digest(data) == case["sha256"]
        row = dict(file=case["file"], status="FAIL", source_sha256=digest(data))
        try:
            control, numeric_replay = (
                raw_source_control(path, case)
                if args.raw_source_controls
                else (canonical_source_control(io.BytesIO(data)), [])
            )
            source_fonts, source_glyphs, source_ops = inspect_pdf(
                PdfReader(io.BytesIO(data))
            )
            control_fonts, control_glyphs, control_ops = inspect_pdf(
                PdfReader(io.BytesIO(control))
            )
            expected_glyphs = (
                retained_source(case, source_glyphs)
                if args.raw_source_controls
                else source_glyphs
            )
            assert set(expected_glyphs) == set(control_glyphs)
            maximum_geometry_error = 0.0
            for binding in expected_glyphs:
                assert (
                    source_fonts[binding]["payload"]
                    == control_fonts[binding]["payload"]
                )
                for original, actual in zip(
                    expected_glyphs[binding], control_glyphs[binding]
                ):
                    assert (original["unicode"], original["cid"], original["gid"]) == (
                        actual["unicode"],
                        actual["cid"],
                        actual["gid"],
                    )
                    for field in ("origin_pt", "bounds_pt"):
                        a, b = original[field], actual[field]
                        assert (a is None) is (b is None)
                        if a is not None:
                            error = max(abs(x - y) for x, y in zip(a, b))
                            maximum_geometry_error = max(maximum_geometry_error, error)
                            assert error <= (0.5 if args.raw_source_controls else 1e-7)
                    if args.raw_source_controls:
                        assert original["code_hex"] == actual["code_hex"]
                assert len(expected_glyphs[binding]) == len(control_glyphs[binding])
            raster = {}
            with pdfium.PdfDocument(data) as before, pdfium.PdfDocument(
                control
            ) as after:
                for dpi in fixed["dpi"]:
                    values = []
                    for i in range(len(before)):
                        a = np.array(
                            before[i].render(scale=dpi / 72).to_pil().convert("RGB")
                        )
                        b = np.array(
                            after[i].render(scale=dpi / 72).to_pil().convert("RGB")
                        )
                        assert a.shape == b.shape
                        values.append(
                            memory_outside_delta(
                                before[i],
                                a,
                                b,
                                (
                                    case["rect"]
                                    if args.raw_source_controls and i == 0
                                    else None
                                ),
                            )
                        )
                    raster[f"PDFium-{dpi}"] = values
                    values = []
                    for i in range(len(before)):

                        def render(payload):
                            proc = subprocess.run(
                                [
                                    str(engine),
                                    "-cropbox",
                                    "-singlefile",
                                    "-png",
                                    "-r",
                                    str(dpi),
                                    "-f",
                                    str(i + 1),
                                    "-l",
                                    str(i + 1),
                                    "-",
                                ],
                                input=payload,
                                capture_output=True,
                                env=env,
                                timeout=60,
                                check=True,
                            )
                            return np.array(
                                Image.open(io.BytesIO(proc.stdout)).convert("RGB")
                            )

                        a, b = render(data), render(control)
                        assert a.shape == b.shape
                        values.append(
                            memory_outside_delta(
                                before[i],
                                a,
                                b,
                                (
                                    case["rect"]
                                    if args.raw_source_controls and i == 0
                                    else None
                                ),
                            )
                        )
                    raster[f"Poppler-{dpi}"] = values
            row.update(
                status=(
                    "PASS_SOURCE_CONTROL"
                    if all(all(v == 0 for v in vs) for vs in raster.values())
                    else "SOURCE_ENGINE_MISMATCH"
                ),
                control_sha256=digest(control),
                control_bytes=len(control),
                source_SFNT_bytes_unchanged=True,
                independent_raw_code_routes_and_true_geometry_equal=True,
                max_real_control_geometry_error_pt=maximum_geometry_error,
                source_only_numeric_replay=numeric_replay,
                raster=raster,
            )
        except Exception as error:
            row.update(error=repr(error), traceback=traceback.format_exc())
        rows.append(row)
    return dict(
        status=(
            "PASS_SOURCE_CONTROLS"
            if all(r["status"] == "PASS_SOURCE_CONTROL" for r in rows)
            else "SOURCE_ENGINE_MISMATCH"
        ),
        source_only=True,
        criteria_sha256=args.criteria_sha256,
        results=rows,
        source_controls=len(rows),
        runtime_passes=0,
        product_edited=False,
        raw_source_control=args.raw_source_controls,
        source_control_security=(
            "UNQUALIFIED: full original SFNT retained in memory; clean product closure must be tested separately"
            if args.raw_source_controls
            else "source-only all-glyph comparison"
        ),
        chronology=(
            "Before first RAW product edit/test, after previous Identity-H product failures"
            if args.raw_source_controls
            else "source-only renderer diagnostics"
        ),
        control_PDFs_memory_only=True,
        raster_files_written=0,
        engine_temp=str(args.engine_temp.resolve()),
        temp_residue=[file_info(p) for p in args.engine_temp.rglob("*") if p.is_file()],
        general_M1_M3_M5_acceptance="withheld",
    )


def memory_outside_delta(page, before, after, rect):
    import ctypes
    import numpy as np
    import pypdfium2.raw as raw

    delta = np.any(before != after, axis=2)
    if rect:
        h, w = delta.shape
        x, y, width, height = rect
        corners = []
        for px, py in (
            (x, y),
            (x + width, y),
            (x, y + height),
            (x + width, y + height),
        ):
            dx, dy = ctypes.c_int(), ctypes.c_int()
            assert raw.FPDF_PageToDevice(
                page.raw,
                0,
                0,
                w * 200,
                h * 200,
                0,
                px,
                py,
                ctypes.byref(dx),
                ctypes.byref(dy),
            )
            corners.append((dx.value / 200, dy.value / 200))
        x0 = max(0, math.floor(min(p[0] for p in corners)) - 1)
        y0 = max(0, math.floor(min(p[1] for p in corners)) - 1)
        x1 = min(w, math.ceil(max(p[0] for p in corners)) + 1)
        y1 = min(h, math.ceil(max(p[1] for p in corners)) + 1)
        delta[y0:y1, x0:x1] = False
    return int(np.count_nonzero(delta))


def runtime_contract(args):
    raw = args.criteria.read_bytes()
    assert digest(raw) == args.criteria_sha256
    fixed = json.loads(raw)
    process_path, test_path = (
        args.run / "process-result.json",
        args.run / "selftest.json",
    )
    process, test = json.loads(process_path.read_bytes()), json.loads(
        test_path.read_bytes()
    )
    assert (
        process["completed"] and process["report_valid"] and process["exit_code"] == 0
    )
    assert digest(args.producer_executable.read_bytes()) == process["exe_sha256"]
    assert same_path(process["executable"], args.producer_executable)
    runtime = args.product_runtime or args.producer_executable.resolve().parent
    runtime.resolve().relative_to(ROOT)
    assert same_path(process["runtime_directory"], runtime)
    sibling_paths = [
        runtime / name
        for name in ("PDFTatsujin.exe", "Pdf4QtLibCore.dll", "Pdf4QtLibWidgets.dll")
    ]
    assert same_path(sibling_paths[0], args.producer_executable)
    binding_before = [file_info(path) for path in sibling_paths]
    proof_path = args.module_proof or args.run / "modules.json"
    proof = json.loads(proof_path.read_bytes())
    assert proof["format"] == "dev43-live-module-proof-v1" and proof["completed"]
    assert same_path(proof["process_executable"], args.producer_executable)
    assert same_path(proof["runtime_directory"], runtime)
    loaded = proof["loaded_modules"]
    assert len(loaded) == 3
    for path, binding in zip(sibling_paths, binding_before):
        matches = [row for row in loaded if same_path(row["path"], path)]
        assert len(matches) == 1
        actual = matches[0]
        assert actual["sha256"] == binding["sha256"]
        assert actual["process_id"] == proof["process_id"] and actual["observed_at_utc"]
    assert test["failures"] == 0 and process["test_count"] == len(test["tests"])
    assert all(t["status"] == "PASS" for t in test["tests"])
    native = next(t for t in test["tests"] if t["name"] == args.path_test_name)
    details = native["details"]
    assert {r["file"]: r["refused"] for r in details["cases"]} == {
        r["file"]: r["refused"] for r in fixed["cases"]
    }
    assert len(details["cases"]) == len(fixed["cases"])
    assert details["refused"] == sum(r["refused"] for r in fixed["cases"])
    if "fixture_criteria_sha256" in details:
        assert details["fixture_criteria_sha256"] == args.criteria_sha256
    source_viewer = audit_qt_source_geometry(
        fixed,
        details,
        args.run / "mixed-word-spacing-source-Qt-origins.json",
        args.criteria_sha256,
    )
    args.engine_temp.resolve().relative_to(ROOT)
    assert args.engine_temp.is_dir()
    from test_tools import poppler_tool

    engine = poppler_tool("pdftoppm")
    rows = []
    for case in fixed["cases"]:
        source = args.criteria.parent / case["file"]
        target = args.run / (args.output_prefix + case["file"])
        assert digest(source.read_bytes()) == case["sha256"]
        row = dict(file=case["file"], status="FAIL", refused=case["refused"])
        try:
            if case["refused"]:
                assert not target.exists(), "refusal produced output"
                row.update(
                    status="PASS", completed_native_refusal=True, source_unchanged=True
                )
            else:
                assert target.is_file()
                row["structural"] = output_contract(source, target, case)
                raster = {}
                for dpi in fixed["dpi"]:
                    for label, renderer in (("PDFium", None), ("Poppler", engine)):
                        raster[f"{label}-{dpi}"] = [
                            raster_compare(
                                source,
                                target,
                                dpi,
                                i,
                                case["rect"] if i == 0 else None,
                                renderer,
                                args.engine_temp,
                            )
                            for i in range(len(PdfReader(source).pages))
                        ]
                row["raster"] = raster
                assert all(
                    all(n == 0 for n in numbers) for numbers in raster.values()
                ), raster
                import pypdfium2 as pdfium

                with pdfium.PdfDocument(target) as document:
                    primary = document[0].get_textpage().get_text_range()
                    assert case["remove"] not in primary
                    assert all(term in primary for term in case["keep"])
                    for index in range(1, len(document)):
                        actual = document[index].get_textpage().get_text_range()
                        assert all(term in actual for term in case["second_page_terms"])
                assert digest(source.read_bytes()) == case["sha256"]
                row.update(
                    status="PASS", output=file_info(target), source_unchanged=True
                )
        except Exception as error:
            row.update(error=repr(error), traceback=traceback.format_exc())
        rows.append(row)
    if not args.cases_only:
        try:
            rows.append(audit_runtime_ui(args, fixed, test, engine))
        except Exception as error:
            rows.append(
                dict(
                    file="UI_atomicity",
                    status="FAIL",
                    error=repr(error),
                    traceback=traceback.format_exc(),
                )
            )
    binding_after = [file_info(path) for path in sibling_paths]
    assert (
        binding_before == binding_after
    ), "producer runtime siblings changed during independent tests"
    return dict(
        status="PASS_SCOPED" if all(r["status"] == "PASS" for r in rows) else "FAIL",
        source_only=False,
        results=rows,
        passed=sum(r["status"] == "PASS" for r in rows),
        failed=sum(r["status"] != "PASS" for r in rows),
        criteria_sha256=args.criteria_sha256,
        original_Qt_source_viewer_geometry=source_viewer,
        producer=dict(
            executable=file_info(args.producer_executable),
            process=file_info(process_path),
            selftest=file_info(test_path),
            actual_loaded_modules_proof=file_info(proof_path),
            runtime_siblings_before=binding_before,
            runtime_siblings_after=binding_after,
            assets_runtime_directory=process.get("assets_runtime_directory"),
        ),
        prior_dev42_direct_five_FAIL="unchanged; this run cannot qualify those source PDFs",
        UI_audit="UNEXECUTED" if args.cases_only else "EXECUTED",
        general_M1_M3_M5_acceptance="withheld",
    )


def audit_qt_source_geometry(fixed, details, path, pin):
    report = json.loads(path.read_bytes())
    assert report["criteria_sha256"] == pin
    assert report["cases"] == details["original_Qt_source_geometry"]
    measured = {r["file"]: r for r in report["cases"]}
    positive = {r["file"]: r for r in fixed["cases"] if not r["refused"]}
    assert set(measured) == set(positive) and len(measured) == len(report["cases"])
    maximum_origin, maximum_end, count_glyphs, count_shows, raw_nonspace = (
        0.0,
        0.0,
        0,
        0,
        0,
    )
    for name, case in positive.items():
        actual = measured[name]
        assert actual["source_sha256"] == case["sha256"]
        expected_shows = case["font_bindings"][0]["streams"]
        expected_glyphs = [g for o in expected_shows for g in o["entries"]]
        assert len(actual["glyphs"]) == len(expected_glyphs)
        assert len(actual["shows"]) == len(expected_shows)
        for truth, glyph in zip(expected_glyphs, actual["glyphs"]):
            assert glyph["unicode"] == truth["unicode"] and glyph["cid"] == truth["cid"]
            assert glyph["raw_word_spacing_applies"] is (truth["code_hex"] == "20")
            error = math.hypot(
                *(a - b for a, b in zip(glyph["origin_pt"], truth["origin_pt"]))
            )
            assert error <= 0.5, (name, "original Qt source origin", glyph, truth)
            maximum_origin = max(maximum_origin, error)
            count_glyphs += 1
            raw_nonspace += truth["code_hex"] == "20" and truth["unicode"] != " "
        for truth, show in zip(expected_shows, actual["shows"]):
            assert show["operator"] == truth["operator"]
            assert (
                show["effective_tw"] == case["effective_tw"]
                and show["effective_tc"] == case["effective_tc"]
            )
            assert (
                show["horizontal_scale"] == case["horizontal_scale"]
                and show["text_rise_pt"] == case["text_rise_pt"]
            )
            error = math.hypot(
                *(a - b for a, b in zip(show["end_origin_pt"], truth["end_origin_pt"]))
            )
            assert error <= 0.5, (name, "original Qt text cursor", show, truth)
            maximum_end = max(maximum_end, error)
            count_shows += 1
    return dict(
        status="PASS_SCOPED",
        source_cases=len(positive),
        glyphs=count_glyphs,
        show_operations=count_shows,
        raw_code32_nonspace_glyphs=raw_nonspace,
        max_origin_error_pt=maximum_origin,
        max_end_error_pt=maximum_end,
        actual_Qt_trace=file_info(path),
        glyph_semantics_and_Tw_flags_and_effective_states_independently_verified=True,
        native_GUI_OS_clipboard="UNEXECUTED",
    )


def owned_signatures(reader, page):
    found = []
    for value in reader.pages[page].get("/Annots", []):
        annotation = value.get_object()
        metadata = annotation.get("/Tatsujin")
        if metadata is None:
            continue
        data = (
            metadata.original_bytes
            if hasattr(metadata, "original_bytes")
            else str(metadata).encode()
        )
        found.append(
            dict(
                metadata=json.loads(data),
                rectangle=[float(v) for v in annotation["/Rect"]],
            )
        )
    return found


def audit_runtime_ui(args, fixed, test, engine):
    native = next(r for r in test["tests"] if r["name"] == args.ui_test_name)
    details = native["details"]
    for field in (
        "source_unchanged",
        "outside_signature_and_reedit",
        "refusal_UI_atomic",
        "source_undo_redo_exact",
        "raw_source_sha256_unchanged",
    ):
        assert details[field] is True, field
    assert details["cancelled"] == 3 and details["cancellation_checkpoints"] > 3
    assert details["reedit_signature_count"] == 2
    assert not (args.run / (args.ui_prefix + "-ui-refused.pdf")).exists()
    assert not list(args.run.glob(args.ui_prefix + "-cancel-*.pdf"))
    before = args.run / (args.ui_prefix + "-ui-before.pdf")
    after = args.run / (args.ui_prefix + "-ui.pdf")
    reedited = args.run / (args.ui_prefix + "-ui-reedited.pdf")
    source_reader, target_reader, reedit_reader = (
        PdfReader(before),
        PdfReader(after),
        PdfReader(reedited),
    )
    assert (
        len(source_reader.pages)
        == len(target_reader.pages)
        == len(reedit_reader.pages)
        == 2
    )
    original = owned_signatures(source_reader, 1)
    preserved = owned_signatures(target_reader, 1)
    edited = owned_signatures(reedit_reader, 1)
    assert len(original) == 1 and original == preserved
    assert original[0]["metadata"]["text"] == "別ページの未保存署名"
    assert len(edited) == 2 and preserved[0] in edited
    assert any(s["metadata"]["text"] == "共有字体の処理後も再編集" for s in edited)
    case = next(r for r in fixed["cases"] if r["file"] == fixed["UI_source"])
    old, old_uses, old_operations = inspect_pdf(source_reader)
    new, new_uses, new_operations = inspect_pdf(target_reader)
    binding_rows = {r["path"]: r for r in case["font_bindings"]}
    assert set(old_uses) == set(new_uses)
    max_error = 0.0
    for binding, glyphs in old_uses.items():
        if binding in binding_rows:
            frozen = [g for o in binding_rows[binding]["streams"] for g in o["entries"]]
            assert len(glyphs) == len(frozen)
            expected = [g for g, truth in zip(glyphs, frozen) if not truth["removed"]]
        else:
            assert binding.startswith("page/1/AP/"), "unfixed extra UI text resource"
            expected = glyphs
        actual = new_uses[binding]
        assert len(expected) == len(actual)
        for a, b in zip(actual, expected):
            assert a["unicode"] == b["unicode"]
            assert glyph_signature(new[binding], a["gid"]) == glyph_signature(
                old[binding], b["gid"]
            )
            for field in ("origin_pt", "bounds_pt"):
                if a[field] is None or b[field] is None:
                    assert a[field] == b[field]
                else:
                    max_error = max(
                        max_error, max(abs(x - y) for x, y in zip(a[field], b[field]))
                    )
    assert max_error <= 0.5
    raster = {}
    for dpi in fixed["dpi"]:
        for label, renderer in (("PDFium", None), ("Poppler", engine)):
            raster[f"{label}-{dpi}"] = [
                raster_compare(
                    before,
                    after,
                    dpi,
                    i,
                    case["rect"] if i == 0 else None,
                    renderer,
                    args.engine_temp,
                )
                for i in (0, 1)
            ]
    assert all(numbers == [0, 0] for numbers in raster.values()), raster
    assert digest((args.criteria.parent / case["file"]).read_bytes()) == case["sha256"]
    return dict(
        file="UI_atomicity",
        status="PASS",
        native_completed_cancel_and_refusal_metadata_verified=True,
        original_source_Undo_Redo_exact=True,
        unsaved_signature_metadata_glyphs_and_pixels_preserved=True,
        reedit_signature_count=2,
        max_position_error_pt=max_error,
        raster=raster,
        before=file_info(before),
        after=file_info(after),
        reedited=file_info(reedited),
        scope="Completed Qt offscreen assertions plus independent saved PDFs/signature metadata/true glyph geometry/both renderer pixels. Native GUI/OS clipboard UNEXECUTED.",
    )


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--dev42-diagnosis", action="store_true")
    parser.add_argument("--ninja-header-audit", action="store_true")
    parser.add_argument("--source-render-controls", action="store_true")
    parser.add_argument("--raw-source-controls", action="store_true")
    parser.add_argument("--case", action="append", default=[])
    parser.add_argument("--criteria", type=Path)
    parser.add_argument("--criteria-sha256")
    parser.add_argument("--run", type=Path)
    parser.add_argument("--producer-executable", type=Path)
    parser.add_argument("--product-runtime", type=Path)
    parser.add_argument("--module-proof", type=Path)
    parser.add_argument("--cases-only", action="store_true")
    parser.add_argument("--ui-test-name", default="M5RW02_mixed_word_spacing_UI")
    parser.add_argument("--ui-prefix", default="mixed-word-spacing")
    parser.add_argument("--path-test-name", default="M5RW01_mixed_word_spacing")
    parser.add_argument("--output-prefix", default="mixed-word-spacing-")
    parser.add_argument("--engine-temp", type=Path)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()
    try:
        assert (
            args.dev42_diagnosis
            or args.ninja_header_audit
            or (args.criteria and args.criteria_sha256)
        )
        report = (
            ninja_header_audit()
            if args.ninja_header_audit
            else (
                dev42_diagnosis()
                if args.dev42_diagnosis
                else (
                    source_render_controls(args)
                    if args.source_render_controls or args.raw_source_controls
                    else runtime_contract(args) if args.run else source_contract(args)
                )
            )
        )
    except Exception as error:
        report = dict(
            status="FAIL",
            error=repr(error),
            traceback=traceback.format_exc(),
            runtime_passes=0,
        )
    report.update(
        run_time_utc=datetime.datetime.now(datetime.timezone.utc).isoformat(),
        examiner=file_info(Path(__file__)),
    )
    args.output.resolve().relative_to(ROOT)
    assert (
        not args.output.exists()
    ), "independent report is immutable; choose a fresh suffix"
    args.output.write_text(
        json.dumps(report, ensure_ascii=False, indent=2) + "\n", encoding="utf-8"
    )
    print(
        json.dumps(
            {
                k: report[k]
                for k in ("status", "source_inspections", "failed", "runtime_passes")
                if k in report
            }
        )
    )
    return 1 if report["status"] == "FAIL" else 0


if __name__ == "__main__":
    raise SystemExit(main())
