"""Freeze raw-code Tw geometry; no product code or rendering engine is used.

Run with -B and the pinned project Python. Inputs are authored from the retained
OFL synthetic source; true Bezier bounds and every text cursor are fixed before
product edits. --verify-existing regenerates in memory and compares all bytes.
--selection-contract-v3 is a separately recorded correction AFTER the first
implementation/r1 failure; its source-only specification never reads outputs.
"""

import argparse
import copy
import hashlib
import gzip
import io
import json
import re
from pathlib import Path

from fontTools.pens.boundsPen import BoundsPen
from fontTools.ttLib import TTFont
from pypdf import PdfReader, PdfWriter
from pypdf.generic import (
    ArrayObject,
    DictionaryObject,
    FloatObject,
    NameObject,
    NumberObject,
)
from prepare_redaction_helpers import stream

ROOT = Path(__file__).resolve().parents[1]
BASE = ROOT / "fixtures/redaction-copy/partial-text/identity-H.pdf"
BASE_SHA = "4f70ffd86ed6afa3ff8fde78dbe8e89ec0deed7900b3a9b69b045a709c89a399"
REMOVE = "秘匿機密亀鶴"
PHRASE = "公開 " + REMOVE + " 文字保持"
SIZE, SCALE, LEADING = 22, 0.85, 26.4
MAX_BYTES = 3_000_000
V1_SHA = "97b90f5ec2d5c3fbe7ee9883afaf52cd5ebb289f2d5e78e23d392fde2425ac0a"
V2_SHA = "549232640c1b627d504f25d17861b3dd46311a33de951fe9298b1c030a9d340f"
SELECTION_SPEC_SHA = "00cff7d7c18a206c91efb55fa6cba7b8a4468350fee5f40e7552fc044e32807a"


def sha(data):
    return hashlib.sha256(data).hexdigest()


def token(payload):
    return b"<" + payload.hex().encode() + b">"


def cmap(spaces, mapping, unicode=False):
    kind = 2 if unicode else 1
    body = (
        b"/CIDInit /ProcSet findresource begin 12 dict begin begincmap\n"
        b"/CIDSystemInfo << /Registry (Adobe) /Ordering (Identity) /Supplement 0 >> def\n"
        + f"/CMapName /RawCodeTw{kind} def /CMapType {kind} def /WMode 0 def\n".encode()
        + f"{len(spaces)} begincodespacerange\n".encode()
    )
    for low, high in spaces:
        body += token(low) + b" " + token(high) + b"\n"
    op = b"bfchar" if unicode else b"cidchar"
    body += b"endcodespacerange\n" + str(len(mapping)).encode() + b" begin" + op + b"\n"
    for source, value in sorted(mapping.items()):
        body += (
            token(source)
            + b" "
            + (token(value.encode("utf-16-be")) if unicode else str(value).encode())
            + b"\n"
        )
    return (
        body
        + b"end"
        + op
        + b"\nendcmap CMapName currentdict /CMap defineresource pop end end\n"
    )


def generate(destination, verify, contract_v2=False):
    base_data = BASE.read_bytes()
    assert sha(base_data) == BASE_SHA
    reader = PdfReader(io.BytesIO(base_data))
    fonts = reader.pages[0]["/Resources"]["/Font"]
    name = next(k for k, v in fonts.items() if "/ToUnicode" in v.get_object())
    original = fonts[name].get_object()
    descendant = original["/DescendantFonts"][0].get_object()
    payload = descendant["/FontDescriptor"]["/FontFile2"].get_data()
    sfnt = TTFont(io.BytesIO(payload))
    gids = descendant["/CIDToGIDMap"].get_data()
    pairs = {}
    for section in re.findall(
        rb"beginbfchar(.*?)endbfchar", original["/ToUnicode"].get_data(), re.S
    ):
        for code, meaning in re.findall(
            rb"<([0-9a-fA-F]+)>\s*<([0-9a-fA-F]+)>", section
        ):
            cid = int(code, 16)
            pairs[bytes.fromhex(meaning.decode()).decode("utf-16-be")] = (
                cid,
                int.from_bytes(gids[2 * cid : 2 * cid + 2], "big"),
            )
    chars = list(dict.fromkeys(PHRASE))
    cids = {ch: 300 + i for i, ch in enumerate(chars)}
    widths = {
        ch: float(descendant["/W"][1][pairs[ch][0] - int(descendant["/W"][0])])
        for ch in chars
    }
    glyphset = sfnt.getGlyphSet()
    glyph_rows = []
    bounds = {}
    for ch in chars:
        gid = pairs[ch][1]
        glyphname = sfnt.getGlyphOrder()[gid]
        pen = BoundsPen(glyphset)
        glyphset[glyphname].draw(pen)
        bounds[ch] = list(pen.bounds) if pen.bounds else None
        glyph = sfnt["glyf"][glyphname]
        coords, ends, flags = glyph.getCoordinates(sfnt["glyf"])
        instructions = (
            bytes(glyph.program.getBytecode()) if hasattr(glyph, "program") else b""
        )
        glyph_rows.append(
            dict(
                unicode=ch,
                gid=gid,
                true_bezier_bounds=bounds[ch],
                metrics=list(sfnt["hmtx"][glyphname]),
                coordinates_sha256=sha(
                    json.dumps(list(map(list, coords)), separators=(",", ":")).encode()
                ),
                endpoints=list(ends),
                flags_sha256=sha(bytes(flags)),
                instruction_hex=instructions.hex(),
                components=(
                    [sfnt.getGlyphID(x.glyphName) for x in glyph.components]
                    if glyph.isComposite()
                    else []
                ),
            )
        )

    specs = []
    for operator, label in (
        ("Tj", "single"),
        ("TJ", "array"),
        ("'", "quote"),
        ('"', "double"),
    ):
        for sign in (1, -1):
            specs.append(
                dict(
                    name=label + ("-positive" if sign > 0 else "-negative"),
                    operator=operator,
                    sign=sign,
                )
            )
    for mode in (
        "code32-kept-nonspace",
        "code32-removed-nonspace",
        "space-other-byte",
        "space-two-byte",
    ):
        for sign in (1, -1):
            specs.append(
                dict(
                    name=mode + ("-positive" if sign > 0 else "-negative"),
                    mode=mode,
                    sign=sign,
                )
            )
    specs += [
        dict(name="array-opposite-Tc", operator="TJ", sign=1, tc=-1.25),
        dict(name="double-opposite-Tc", operator='"', sign=-1, tc=1.25),
        dict(name="shared-pages-alias", sharing="pages", sign=1),
        dict(name="shared-sibling-form", sharing="form", sign=-1),
        dict(name="shared-appearance-states", sharing="AP", sign=1),
        dict(name="state-transitions", state=True, sign=-1),
        dict(name="empty-single", empty="Tj", sign=1),
        dict(name="empty-array", empty="TJ", sign=-1),
        dict(name="empty-double", empty='"', sign=1),
        dict(name="raw-code-alias", alias=True, sign=-1),
    ]
    for reason in (
        "prefix",
        "truncated",
        "encoding-redefinition",
        "unicode-redefinition",
        "usecmap",
        "unknown",
        "vertical",
        "multichar",
        "partial-glyph",
        "equal-value-length",
    ):
        specs.append(dict(name=reason + "-refused", refused=reason, sign=1))
    if contract_v2:
        assert sha((destination / "criteria.json").read_bytes()) == V1_SHA
        for spec in specs:
            if spec.get("mode") == "space-two-byte":
                spec["refused"] = "two-byte32"
        specs += [
            dict(
                name="space-high-two-byte-positive", mode="space-high-two-byte", sign=1
            ),
            dict(
                name="space-high-two-byte-negative", mode="space-high-two-byte", sign=-1
            ),
            dict(name="tilted-Tm-refused", refused="tilted-Tm", sign=1),
            dict(name="scaled-Tm-refused", refused="scaled-Tm", sign=-1),
        ]

    def save(path, data):
        if (
            contract_v2
            and not verify
            and path.name == "criteria-v2.json"
            and path.exists()
        ):
            previous = path.read_bytes()
            if previous == data:
                return
            assert (
                sha(previous)
                == "4e356f407cf67b2b8c8148a2dcb1d7d3a1480e57b6d44aab8c46145737ace05d"
            )
            backup = (
                destination.parent
                / "mixed-word-spacing-prefreeze-failed-r1"
                / "pre-legacy-criteria-v2.json.gz"
            )
            with backup.open("xb") as handle:
                handle.write(gzip.compress(previous, mtime=0))
            assert gzip.decompress(backup.read_bytes()) == previous
            path.write_bytes(data)
            return
        if verify or (contract_v2 and path.suffix == ".pdf" and path.exists()):
            assert path.read_bytes() == data, ("regeneration differs", path.name)
        else:
            with path.open("xb") as handle:
                handle.write(data)

    rows = []
    for spec in specs:
        writer = PdfWriter(clone_from=PdfReader(io.BytesIO(base_data)))
        page = writer.pages[0]
        resources = page["/Resources"]
        font = resources["/Font"][name].get_object()
        child = font["/DescendantFonts"][0].get_object()
        codes = {
            " ": b"\x20",
            "公": b"\x41",
            "開": b"\x42",
            "文": b"\x43",
            "字": b"\x44",
            "保": b"\x45",
            "持": b"\x46",
        }
        codes.update(
            {ch: (0x8101 + i).to_bytes(2, "big") for i, ch in enumerate(REMOVE)}
        )
        mode = spec.get("mode")
        if mode == "code32-kept-nonspace":
            codes["公"], codes[" "] = b"\x20", b"\x47"
        elif mode == "code32-removed-nonspace":
            codes["秘"], codes[" "] = b"\x20", b"\x47"
        elif mode == "space-other-byte":
            codes[" "] = b"\x47"
        elif mode == "space-two-byte":
            codes[" "] = b"\x00\x20"
        elif mode == "space-high-two-byte":
            codes[" "] = b"\x81\x07"
        spaces = [(b"\x20", b"\x7f"), (b"\x81\x00", b"\x81\xff")]
        if mode == "space-two-byte":
            spaces.append((b"\x00\x01", b"\x00\xff"))
        reason = spec.get("refused")
        if reason == "equal-value-length":
            codes["秘"] = b"\x00\x41"
            spaces.append((b"\x00\x01", b"\x00\xff"))
        encoding = {value: cids[ch] for ch, value in codes.items()}
        meanings = {value: ch for ch, value in codes.items()}
        if spec.get("alias"):
            encoding[b"\x47"], meanings[b"\x47"] = cids[" "], " "
        if reason == "prefix":
            spaces.append((b"\x41\x00", b"\x41\xff"))
        enc, uni = cmap(spaces, encoding), cmap(spaces, meanings, True)
        if reason == "encoding-redefinition":
            enc = enc.replace(
                b"endcmap", b"1 begincidchar <41> 999 endcidchar\nendcmap"
            )
        elif reason == "unicode-redefinition":
            uni = uni.replace(
                b"endcmap", b"1 beginbfchar <41> <958b> endbfchar\nendcmap"
            )
        elif reason == "usecmap":
            enc = enc.replace(b"endcmap", b"/Identity-H usecmap\nendcmap")
        elif reason == "unknown":
            enc = enc.replace(b"endcmap", b"0 mysterycommand\nendcmap")
        elif reason == "vertical":
            enc = enc.replace(b"/WMode 0", b"/WMode 1")
        elif reason == "multichar":
            uni = uni.replace(b"<41> <516c>", b"<41> <516c958b>")
        font[NameObject("/Encoding")] = writer._add_object(stream(enc))
        font[NameObject("/ToUnicode")] = writer._add_object(stream(uni))
        cid_gid = bytearray((max(cids.values()) + 1) * 2)
        for ch, cid in cids.items():
            cid_gid[2 * cid : 2 * cid + 2] = pairs[ch][1].to_bytes(2, "big")
        child[NameObject("/CIDToGIDMap")] = writer._add_object(stream(bytes(cid_gid)))
        child[NameObject("/W")] = ArrayObject(
            [NumberObject(300), ArrayObject([FloatObject(widths[ch]) for ch in chars])]
        )
        tw, tc = spec["sign"] * 3.25, spec.get("tc", spec["sign"] * 1.25)
        encode = lambda text: b"".join(codes[ch] for ch in text)
        op = spec.get("operator", "Tj")
        empty = spec.get("empty")
        origin_y = 650 + LEADING if op in ("'", '"') or empty == '"' else 650
        initial_tw, initial_tc = (-9, 2.5) if empty == '"' else (tw, tc)
        content = f"{initial_tw} Tw {initial_tc} Tc 85 Tz 0 Ts\nq 9 Tw -2 Tc 70 Tz 4 Ts Q\nBT {name} 22 Tf 26.4 TL 1 0 0 1 60 {origin_y:g} Tm\n".encode()
        rise = 2 if spec.get("state") else 0
        if rise:
            content += b"2 Ts\n"
        if empty:
            content += (
                b"<> Tj\n"
                if empty == "Tj"
                else b"[] TJ\n" if empty == "TJ" else f'{tw} {tc} <> "\n'.encode()
            )
        segments = [(PHRASE, None)]
        if op == "TJ":
            segments = [
                ("公開 " + REMOVE[:2], 125),
                (REMOVE[2:4], -75),
                (REMOVE[4:] + " 文字保持", None),
            ]
            content += (
                b"["
                + token(encode(segments[0][0]))
                + b" 125 "
                + token(encode(segments[1][0]))
                + b" -75 "
                + token(encode(segments[2][0]))
                + b"] TJ\n"
            )
        else:
            data = encode(PHRASE) + (b"\x81" if reason == "truncated" else b"")
            content += (
                (f"{tw} {tc} ".encode() if op == '"' else b"")
                + token(data)
                + b" "
                + op.encode()
                + b"\n"
            )
        content += (
            token(encode("公開")) + b" Tj\nT*\n" + token(encode("文字保持")) + b" Tj\n"
        )
        if spec.get("state"):
            content += (
                b"10 -35 Td\n"
                + token(encode("公開"))
                + b" Tj\n5 -30 TD\n"
                + token(encode("文字保持"))
                + b" Tj\nT*\n"
                + token(encode("公開"))
                + b" Tj\n"
            )
        content += (
            b"1 0 0 1 60 450 Tm\n"
            + token(encode("文字保持"))
            + b" Tj\nET\nBT /F1 16 Tf 0 Tc 0 Tw 100 Tz 0 Ts 1 0 0 1 60 400 Tm (KEEP_FOLLOWING_LINE) Tj ET\n"
        )
        unsupported_matrix = None
        if reason in ("tilted-Tm", "scaled-Tm"):
            unsupported_matrix = (
                [1, 0.2, 0.1, 1] if reason == "tilted-Tm" else [1.2, 0, 0, 0.9]
            )
            opening = f"1 0 0 1 60 {origin_y:g} Tm".encode()
            modified = (
                " ".join(str(x) for x in unsupported_matrix) + f" 60 {origin_y:g} Tm"
            ).encode()
            content = content.replace(opening, modified, 1)
        page[NameObject("/Contents")] = writer._add_object(stream(content))

        expected = []
        if empty:
            expected.append(
                dict(
                    text="",
                    operator=empty,
                    before_origin_pt=[60, origin_y],
                    origin_pt=[60, 650],
                    end_origin_pt=[60, 650],
                    effective_tw=tw,
                    effective_tc=tc,
                    before_tw=initial_tw,
                    before_tc=initial_tc,
                    entries=[],
                )
            )

        def layout(
            text, x, y, tw_value=tw, tc_value=tc, selected=False, alternate=False
        ):
            entries = []
            for i, ch in enumerate(text):
                raw_code = b"\x47" if alternate and ch == " " and i > 3 else codes[ch]
                word = tw_value if raw_code == b"\x20" else 0
                advance = (widths[ch] * SIZE / 1000 + tc_value + word) * SCALE
                box = None
                if bounds[ch]:
                    low, bottom, high, top = bounds[ch]
                    factor = SIZE / sfnt["head"].unitsPerEm
                    box = [
                        x + low * factor * SCALE,
                        y + rise + bottom * factor,
                        x + high * factor * SCALE,
                        y + rise + top * factor,
                    ]
                entries.append(
                    dict(
                        code_hex=raw_code.hex(),
                        cid=cids[ch],
                        gid=pairs[ch][1],
                        unicode=ch,
                        width=widths[ch],
                        origin_pt=[x, y + rise],
                        advance_pt=advance,
                        raw_code_word_spacing=word,
                        outline_box_pt=box,
                        removed=selected and ch in REMOVE,
                    )
                )
                x += advance
            return entries, x

        x, primary = 60.0, []
        for text, adjustment in segments:
            items, x = layout(
                text, x, 650, selected=True, alternate=spec.get("alias", False)
            )
            primary += items
            if adjustment is not None:
                x -= adjustment * SIZE / 1000 * SCALE
        # Alias is authored in the second (trailing) space only, with the same CID
        # and Unicode; its raw code has no Tw even though the first space does.
        if spec.get("alias"):
            content = content.replace(
                token(encode(PHRASE)),
                token(encode("公開 " + REMOVE) + b"\x47" + encode("文字保持")),
                1,
            )
            page[NameObject("/Contents")] = writer._add_object(stream(content))
        expected.append(
            dict(
                text=PHRASE,
                operator=op,
                origin_pt=[60, 650],
                end_origin_pt=[x, 650],
                entries=primary,
            )
        )
        tail, end = layout("公開", x, 650)
        expected.append(
            dict(
                text="公開",
                operator="Tj",
                origin_pt=[x, 650],
                end_origin_pt=[end, 650],
                entries=tail,
            )
        )
        line, end = layout("文字保持", 60, 650 - LEADING)
        expected.append(
            dict(
                text="文字保持",
                operator="Tj",
                origin_pt=[60, 650 - LEADING],
                end_origin_pt=[end, 650 - LEADING],
                entries=line,
            )
        )
        if spec.get("state"):
            for text, x0, y0 in (
                ("公開", 70, 650 - LEADING - 35),
                ("文字保持", 75, 650 - LEADING - 65),
                ("公開", 75, 650 - LEADING - 95),
            ):
                items, end = layout(text, x0, y0)
                expected.append(
                    dict(
                        text=text,
                        operator="Tj",
                        origin_pt=[x0, y0],
                        end_origin_pt=[end, y0],
                        entries=items,
                    )
                )
        absolute, end = layout("文字保持", 60, 450)
        expected.append(
            dict(
                text="文字保持",
                operator="Tj",
                origin_pt=[60, 450],
                end_origin_pt=[end, 450],
                entries=absolute,
            )
        )
        removed = [
            x["outline_box_pt"] for x in primary if x["removed"] and x["outline_box_pt"]
        ]
        envelope = [
            min(x[0] for x in removed),
            min(x[1] for x in removed),
            max(x[2] for x in removed),
            max(x[3] for x in removed),
        ]
        rect = [
            envelope[0] - 1,
            envelope[1] - 1,
            envelope[2] - envelope[0] + 2,
            envelope[3] - envelope[1] + 2,
        ]
        if reason == "partial-glyph":
            first = removed[0]
            rect = [
                first[0] + (first[2] - first[0]) / 2,
                first[1] - 1,
                1,
                first[3] - first[1] + 2,
            ]
        else:
            for entry in primary:
                box = entry["outline_box_pt"]
                if box and not entry["removed"]:
                    assert (
                        box[2] <= rect[0]
                        or box[0] >= rect[0] + rect[2]
                        or box[3] <= rect[1]
                        or box[1] >= rect[1] + rect[3]
                    ), (spec["name"], entry["unicode"], box, rect)
        bindings = [
            dict(
                path="page/0/font" + str(name),
                text="".join(show["text"] for show in expected),
                removed=REMOVE,
                streams=expected,
            )
        ]
        secondary = []
        if spec.get("sharing"):
            second = writer.add_blank_page(width=612, height=792)
            other = copy.copy(resources)
            fontname = name
            if spec["sharing"] == "pages":
                alias_fonts = copy.copy(resources["/Font"])
                alias_fonts[NameObject("/TwAlias")] = writer._add_object(
                    copy.copy(font)
                )
                other[NameObject("/Font")] = alias_fonts
                fontname = NameObject("/TwAlias")
            text = "公開 秘 文字保持"
            outside = (
                f"BT {fontname} 22 Tf {tw} Tw {tc} Tc 85 Tz 1 0 0 1 60 500 Tm ".encode()
                + token(encode(text))
                + b" Tj ET\n"
            )
            second[NameObject("/Resources")] = other
            second[NameObject("/Contents")] = writer._add_object(stream(outside))
            items, end = layout(text, 60, 500)
            secondary = [
                dict(
                    text=text,
                    operator="Tj",
                    origin_pt=[60, 500],
                    end_origin_pt=[end, 500],
                    entries=items,
                )
            ]
            if spec["sharing"] == "form":
                form = writer._add_object(
                    stream(
                        outside,
                        Type=NameObject("/XObject"),
                        Subtype=NameObject("/Form"),
                        BBox=ArrayObject(
                            [
                                NumberObject(0),
                                NumberObject(0),
                                NumberObject(612),
                                NumberObject(792),
                            ]
                        ),
                        Resources=other,
                    )
                )
                second[NameObject("/Resources")] = DictionaryObject(
                    {
                        NameObject("/XObject"): DictionaryObject(
                            {NameObject("/Outside"): form}
                        )
                    }
                )
                second[NameObject("/Contents")] = writer._add_object(
                    stream(b"/Outside Do\n")
                )
                path = "page/1/form/0/font" + str(name)
            elif spec["sharing"] == "AP":
                appearances = DictionaryObject()
                apcontent = outside.replace(b"60 500 Tm", b"10 10 Tm")
                apitems, apend = layout(text, 10, 10)
                for state in ("N", "R", "D"):
                    appearances[NameObject("/" + state)] = writer._add_object(
                        stream(
                            apcontent,
                            Type=NameObject("/XObject"),
                            Subtype=NameObject("/Form"),
                            BBox=ArrayObject(
                                [
                                    NumberObject(0),
                                    NumberObject(0),
                                    NumberObject(270),
                                    NumberObject(50),
                                ]
                            ),
                            Resources=other,
                        )
                    )
                    bindings.append(
                        dict(
                            path="page/1/AP/0/" + state + "/font" + str(name),
                            text=text,
                            removed="",
                            streams=[
                                dict(
                                    text=text,
                                    operator="Tj",
                                    origin_pt=[10, 10],
                                    end_origin_pt=[apend, 10],
                                    entries=apitems,
                                )
                            ],
                        )
                    )
                annot = DictionaryObject(
                    {
                        NameObject("/Type"): NameObject("/Annot"),
                        NameObject("/Subtype"): NameObject("/Stamp"),
                        NameObject("/Rect"): ArrayObject(
                            [
                                NumberObject(60),
                                NumberObject(300),
                                NumberObject(330),
                                NumberObject(350),
                            ]
                        ),
                        NameObject("/F"): NumberObject(4),
                        NameObject("/AP"): appearances,
                    }
                )
                second[NameObject("/Annots")] = ArrayObject([writer._add_object(annot)])
                path = "page/1/font" + str(name)
            else:
                path = "page/1/font" + str(fontname)
            bindings.append(dict(path=path, text=text, removed="", streams=secondary))
        data = io.BytesIO()
        writer.write(data)
        filename = spec["name"] + ".pdf"
        save(destination / filename, data.getvalue())
        rows.append(
            dict(
                file=filename,
                sha256=sha(data.getvalue()),
                refused=bool(reason),
                refusal_reason=reason,
                remove=REMOVE,
                removed=REMOVE,
                keep=["公開", "文字保持", "KEEP_FOLLOWING_LINE"],
                retained=["公開", "文字保持", "KEEP_FOLLOWING_LINE"],
                rect=rect,
                operator=op,
                effective_tw=tw,
                effective_tc=tc,
                initial_tw=initial_tw,
                initial_tc=initial_tc,
                font_size_pt=SIZE,
                horizontal_scale=SCALE,
                leading_pt=LEADING,
                text_rise_pt=rise,
                encoding_bytes=enc.decode(),
                tounicode_bytes=uni.decode(),
                encoding_sha256=sha(enc),
                tounicode_sha256=sha(uni),
                content_bytes=content.decode(),
                content_sha256=sha(content),
                source_sfnt_sha256=sha(payload),
                codespaces=[
                    dict(low=lo.hex(), high=hi.hex(), byte_count=len(lo))
                    for lo, hi in spaces
                ],
                encoding_route=[
                    dict(
                        code_hex=code.hex(),
                        byte_count=len(code),
                        value=int.from_bytes(code, "big"),
                        cid=cid,
                        gid=int.from_bytes(cid_gid[2 * cid : 2 * cid + 2], "big"),
                    )
                    for code, cid in sorted(encoding.items())
                ],
                unicode_route=[
                    dict(
                        code_hex=code.hex(),
                        byte_count=len(code),
                        value=int.from_bytes(code, "big"),
                        unicode=ch,
                    )
                    for code, ch in sorted(meanings.items())
                ],
                font_bindings=bindings,
                second_page_terms=["公開", "秘", "文字保持"] if secondary else [],
                empty_operator=empty,
                raw_code_word_spacing_rule="Tw iff code length=1 and value=32; Unicode is irrelevant",
            )
        )
        if unsupported_matrix:
            rows[-1]["unsupported_text_matrix"] = unsupported_matrix + [60, origin_y]
            rows[-1][
                "geometry_contract"
            ] = "UNEXECUTED: nonzero Tw with non-identity text matrix linear part is outside this unit; reject before geometry"
            rows[-1]["font_bindings"][0]["streams"] = []
    if contract_v2:
        # Preserve the already fixed all-two-byte refusal with identical input
        # bytes, rather than laundering the new mixed-map scope into its oracle.
        legacy_folder = ROOT / "fixtures/redaction-copy/custom-cmap-v1"
        legacy_criteria = (legacy_folder / "criteria.json").read_bytes()
        assert (
            sha(legacy_criteria)
            == "044024c5bf7ec5a52802e6ba0d0265237aefee83471147207c61a882f7d4a36f"
        )
        legacy = next(
            x
            for x in json.loads(legacy_criteria)["cases"]
            if x["file"] == "Tw-refused.pdf"
        )
        legacy_data = (legacy_folder / legacy["file"]).read_bytes()
        assert (
            sha(legacy_data)
            == legacy["sha256"]
            == "49818de73983e4e7c2f3345898c8d54294b2e922984df772e1ab9e6fe3f0c119"
        )
        filename = "legacy-all-two-byte-Tw-refused.pdf"
        save(destination / filename, legacy_data)
        legacy_pdf = PdfReader(io.BytesIO(legacy_data))
        legacy_font = next(
            v.get_object()
            for v in legacy_pdf.pages[0]["/Resources"]["/Font"].values()
            if "/ToUnicode" in v.get_object()
        )
        legacy_child = legacy_font["/DescendantFonts"][0].get_object()
        legacy_enc, legacy_uni = (
            legacy_font["/Encoding"].get_data(),
            legacy_font["/ToUnicode"].get_data(),
        )
        legacy_program = legacy_child["/FontDescriptor"]["/FontFile2"].get_data()
        legacy_content = legacy_pdf.pages[0]["/Contents"].get_data()
        rows.append(
            dict(
                file=filename,
                sha256=sha(legacy_data),
                refused=True,
                refusal_reason="all-two-byte-nonzero-Tw",
                remove=legacy["remove"],
                removed=legacy["remove"],
                keep=legacy["keep"],
                retained=legacy["keep"],
                rect=legacy["rect"],
                second_page_terms=[],
                exact_source_reuse=dict(
                    path="fixtures/redaction-copy/custom-cmap-v1/Tw-refused.pdf",
                    sha256=sha(legacy_data),
                    criteria_sha256=sha(legacy_criteria),
                ),
                encoding_bytes=legacy_enc.decode(),
                tounicode_bytes=legacy_uni.decode(),
                encoding_sha256=sha(legacy_enc),
                tounicode_sha256=sha(legacy_uni),
                content_bytes=legacy_content.decode(),
                content_sha256=sha(legacy_content),
                source_sfnt_sha256=sha(legacy_program),
                effective_tw=3,
                effective_tc=0,
                font_size_pt=22,
                horizontal_scale=1,
                leading_pt=0,
                text_rise_pt=0,
                operator="Tj",
                font_bindings=[],
                geometry_contract="UNEXECUTED: all-two-byte nonzero Tw remains outside new mixed-map unit; exact prior refusal input retained",
            )
        )
    fixed = dict(
        version=1,
        scope="embedded horizontal unambiguous 1/2-byte CMap, one BMP, finite signed Tw/Tc; fixed raw code interval compensation",
        original_source=dict(
            path=str(BASE.relative_to(ROOT)).replace("\\", "/"),
            sha256=BASE_SHA,
            sfnt_sha256=sha(payload),
        ),
        license=dict(
            path="licenses/NotoSansJP-OFL.txt",
            sha256=sha((ROOT / "licenses/NotoSansJP-OFL.txt").read_bytes()),
            source_font_sha256=sha((ROOT / "assets/fonts/NotoSansJP.ttf").read_bytes()),
        ),
        source_glyphs=glyph_rows,
        cases=rows,
        position_error_pt=0.5,
        outside_pixel_difference=0,
        boundary_pixels=1,
        dpi=[72, 96, 300],
        UI_source="shared-pages-alias.pdf",
        UI_refused="prefix-refused.pdf",
        general_M1_M3_M5_acceptance="withheld",
        external_source_geometry_used=False,
        prior_dev42_fixtures_and_five_failures="unchanged",
        MIAP_4096_observation="not evaluated by this contract",
    )
    if contract_v2:
        fixed.update(
            version=2,
            preimplementation_v1_criteria=dict(
                path="criteria.json",
                sha256=V1_SHA,
                bytes=(destination / "criteria.json").stat().st_size,
            ),
            source_fidelity_boundary="Installed PDFium source control differs for two-byte0020 with nonzeroTw; original bytes and v1 acceptance expectation retained",
            scope="embedded horizontal unambiguous CMap with single-byte codes and optional two-byte codes, one BMP, finite signed Tw/Tc; raw code compensation",
            nonzero_tw_refusal_bounds=[
                "no single-byte source codes",
                "same numeric source value across code lengths",
                "two-byte code value32",
                "text matrix linear part is not identity",
            ],
            source_viewer_contract="Qt source glyph origins from the same page renderer must match frozen raw-code positions within0.5pt before redaction; Type0 source flag drives Tw, not Unicode",
            UI_source="shared-pages-alias.pdf",
            UI_refused="equal-value-length-refused.pdf",
            UI_output_prefix="mixed-word-spacing",
            UI_test_name="M5RW02_mixed_word_spacing_UI",
            UI_expected=dict(
                source_unchanged=True,
                outside_signature_and_reedit=True,
                cancelled=3,
                refusal_UI_atomic=True,
                source_undo_redo_exact=True,
                reedit_signature_count=2,
                raw_source_sha256_unchanged=True,
                native_GUI_OS_clipboard="未実行; Qt offscreen",
            ),
        )
    raw = (json.dumps(fixed, ensure_ascii=False, separators=(",", ":")) + "\n").encode()
    save(destination / ("criteria-v2.json" if contract_v2 else "criteria.json"), raw)
    total = sum(p.stat().st_size for p in destination.iterdir())
    assert total <= MAX_BYTES, (total, MAX_BYTES)
    return dict(
        cases=len(rows),
        success=sum(not x["refused"] for x in rows),
        refused=sum(x["refused"] for x in rows),
        bytes=total,
        criteria_sha256=sha(raw),
    )


def selection_contract_v3(destination, verify):
    """Keep every v2 field except seven source-proven flags; append two refusals."""
    base_raw = (destination / "criteria-v2.json").read_bytes()
    assert sha(base_raw) == V2_SHA
    spec_path = (
        ROOT / "evidence/dev43-independent-corrected-space-selection-spec-r1.json"
    )
    spec_raw = spec_path.read_bytes()
    assert sha(spec_raw) == SELECTION_SPEC_SHA
    source = json.loads(base_raw)
    spec = json.loads(spec_raw)
    assert len(source["cases"]) == 41 and len(spec["changes"]) == 7
    assert spec["base_criteria"]["sha256"] == V2_SHA
    assert spec["runtime_product_outputs_read"] == spec["renderers_run"] == 0
    corrected = copy.deepcopy(source)
    changed_paths = []
    for change in spec["changes"]:
        path = change["json_path"]
        assert path == [
            "cases",
            change["case_index"],
            "font_bindings",
            0,
            "streams",
            change["stream_index"],
            "entries",
            9,
            "removed",
        ]
        assert change["case_index"] in (1, 3, 5, 7, 19, 21, 23)
        row = source["cases"][change["case_index"]]
        entry = row["font_bindings"][0]["streams"][change["stream_index"]]["entries"][9]
        assert row["file"] == change["file"] and entry["removed"] is False
        assert (
            entry["unicode"] == " "
            and entry["code_hex"] == "20"
            and not entry["outline_box_pt"]
        )
        advance = (
            entry["width"] * row["font_size_pt"] / 1000
            + row["effective_tc"]
            + row["effective_tw"]
        ) * row["horizontal_scale"]
        x, y = entry["origin_pt"]
        rx, ry, rw, rh = row["rect"]
        assert abs(advance - entry["advance_pt"]) < 1e-10
        assert rx <= min(x, x + advance) <= max(x, x + advance) <= rx + rw
        assert ry <= y <= ry + rh and change["old"] is False and change["new"] is True
        target = corrected
        for key in path[:-1]:
            target = target[key]
        target[path[-1]] = True
        changed_paths.append(path)
    assert len({tuple(path) for path in changed_paths}) == 7
    # Reverse only those seven edits and prove all old fields are unchanged.
    reverse = copy.deepcopy(corrected)
    for path in changed_paths:
        target = reverse
        for key in path[:-1]:
            target = target[key]
        target[path[-1]] = False
    assert reverse == source
    for row in source["cases"]:
        assert sha((destination / row["file"]).read_bytes()) == row["sha256"]

    primary = source["cases"][0]
    original = (destination / primary["file"]).read_bytes()
    for name, tc, effective in (
        ("zero-effective-advance-refused", -8.75, 0),
        ("reverse-effective-advance-refused", -9.75, -1),
    ):
        writer = PdfWriter(clone_from=PdfReader(io.BytesIO(original)))
        content = primary["content_bytes"].encode()
        assert content.count(b" 22 Tf") == 1 and content.count(b"1.25 Tc") == 1
        content = content.replace(b" 22 Tf", b" 25 Tf").replace(
            b"1.25 Tc", f"{tc} Tc".encode()
        )
        writer.pages[0][NameObject("/Contents")] = writer._add_object(stream(content))
        buffer = io.BytesIO()
        writer.write(buffer)
        data = buffer.getvalue()
        path = destination / (name + ".pdf")
        if verify or path.exists():
            assert path.read_bytes() == data
        else:
            with path.open("xb") as handle:
                handle.write(data)
        row = copy.deepcopy(primary)
        row.update(
            file=path.name,
            sha256=sha(data),
            refused=True,
            refusal_reason="nonpositive-effective-advance",
            content_bytes=content.decode(),
            content_sha256=sha(content),
            font_size_pt=25,
            effective_tc=tc,
            initial_tc=tc,
            font_bindings=[],
            geometry_contract="UNEXECUTED: nonzero Tw with zero/reverse glyph advance is outside this unit; reject before selection",
            advance_boundary_control=dict(
                code_hex="20",
                cid=302,
                unicode=" ",
                width=220,
                font_size_pt=25,
                raw_tw=3.25,
                tc=tc,
                unscaled_advance_pt=effective,
                scaled_advance_pt=effective * 0.85,
                source_file=primary["file"],
                source_sha256=primary["sha256"],
            ),
        )
        assert (220 * 25 / 1000 + tc + 3.25) == effective
        corrected["cases"].append(row)
    corrected.update(
        version=3,
        corrected_after_first_implementation_r1_failure_before_v3_run=dict(
            base=dict(path="criteria-v2.json", sha256=V2_SHA, bytes=len(base_raw)),
            independent_source_spec=dict(
                path=str(spec_path.relative_to(ROOT)).replace("\\", "/"),
                sha256=SELECTION_SPEC_SHA,
                bytes=len(spec_raw),
            ),
            changed_old_fields=changed_paths,
            old_input_pdf_count=41,
            new_refusal_input_count=2,
            chronology=spec["chronology"],
            product_outputs_used_for_truth=False,
        ),
        effective_advance_scope="nonzero Tw requires finite positive horizontal scale and every glyph width*size/1000+Tc+raw-code-Tw >0; zero/reverse advances are unqualified and refused",
        negative_effective_advance_accepted_observations=0,
    )
    raw = (
        json.dumps(corrected, ensure_ascii=False, separators=(",", ":")) + "\n"
    ).encode()
    path = destination / "criteria-v3.json"
    if verify or path.exists():
        assert path.read_bytes() == raw
    else:
        with path.open("xb") as handle:
            handle.write(raw)
    assert (destination / "criteria-v2.json").read_bytes() == base_raw
    assert sha((destination / "criteria.json").read_bytes()) == V1_SHA
    total = sum(p.stat().st_size for p in destination.iterdir())
    assert total <= MAX_BYTES, (total, MAX_BYTES)
    return dict(
        cases=43,
        success=26,
        refused=17,
        old_selection_flag_corrections=7,
        bytes=total,
        criteria_sha256=sha(raw),
        independent_selection_spec_sha256=SELECTION_SPEC_SHA,
    )


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--verify-existing", action="store_true")
    parser.add_argument("--contract-v2", action="store_true")
    parser.add_argument("--selection-contract-v3", action="store_true")
    args = parser.parse_args()
    destination = args.output.resolve()
    destination.relative_to(ROOT)
    assert not (args.contract_v2 and args.selection_contract_v3)
    if args.selection_contract_v3:
        print(json.dumps(selection_contract_v3(destination, args.verify_existing)))
        return
    if not args.verify_existing and not args.contract_v2:
        destination.mkdir(parents=True, exist_ok=False)
    print(json.dumps(generate(destination, args.verify_existing, args.contract_v2)))


if __name__ == "__main__":
    main()
