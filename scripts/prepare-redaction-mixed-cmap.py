"""Freeze a bounded 1/2-byte horizontal CMap contract before product edits.

No product decoder/serializer is imported. PDFium supplies source char boxes;
origins/advances are calculated directly from explicit source widths and operands.
"""

import argparse
import copy
import hashlib
import io
import json
import re
from pathlib import Path

import pypdfium2 as pdfium
from fontTools.ttLib import TTFont
from fontTools.pens.boundsPen import BoundsPen
from pypdf import PdfReader, PdfWriter
from pypdf.generic import (
    ArrayObject,
    ByteStringObject,
    DictionaryObject,
    FloatObject,
    NameObject,
    NumberObject,
)
from prepare_redaction_helpers import stream

ROOT = Path(__file__).resolve().parents[1]
BASE = ROOT / "fixtures/redaction-copy/partial-text/identity-H.pdf"
BASE_PIN = "4f70ffd86ed6afa3ff8fde78dbe8e89ec0deed7900b3a9b69b045a709c89a399"
REMOVE = "秘匿機密亀鶴"
PHRASE = "公開 " + REMOVE + " 文字保持"
MAX_BYTES = 4_700_000
V1_PIN = "fc20b71f3990ffe4fa468389f0867297ebe0fcf2c8aa261de03c694b8f8af118"


def sha(data):
    return hashlib.sha256(data).hexdigest()


def code(value, width):
    return value.to_bytes(width, "big")


def cmap(spaces, mappings, kind, array=False, ranges=False):
    body = (
        b"/CIDInit /ProcSet findresource begin 12 dict begin begincmap\n"
        b"/CIDSystemInfo << /Registry (Adobe) /Ordering (Identity) /Supplement 0 >> def\n"
        + f"/CMapName /MixedWidth{kind} def /CMapType {kind} def /WMode 0 def\n".encode()
        + f"{len(spaces)} begincodespacerange\n".encode()
    )
    body += b"".join(
        b"<" + lo.hex().encode() + b"> <" + hi.hex().encode() + b">\n"
        for lo, hi in spaces
    )
    body += b"endcodespacerange\n"
    if (ranges and kind == 1) or (array and kind == 2):
        groups = []
        for source, value in sorted(mappings.items()):
            if (
                groups
                and len(groups[-1][-1][0]) == len(source)
                and int.from_bytes(source, "big")
                == int.from_bytes(groups[-1][-1][0], "big") + 1
                and (kind == 2 or value == groups[-1][-1][1] + 1)
            ):
                groups[-1].append((source, value))
            else:
                groups.append([(source, value)])
        for group in groups:
            lo, hi = group[0][0].hex().encode(), group[-1][0].hex().encode()
            if kind == 1:
                body += (
                    b"1 begincidrange <"
                    + lo
                    + b"> <"
                    + hi
                    + b"> "
                    + str(group[0][1]).encode()
                    + b" endcidrange\n"
                )
            else:
                values = b" ".join(
                    b"<" + value.encode("utf-16-be").hex().encode() + b">"
                    for _, value in group
                )
                body += (
                    b"1 beginbfrange <"
                    + lo
                    + b"> <"
                    + hi
                    + b"> ["
                    + values
                    + b"] endbfrange\n"
                )
    else:
        operator = b"cidchar" if kind == 1 else b"bfchar"
        body += str(len(mappings)).encode() + b" begin" + operator + b"\n"
        for source, value in sorted(mappings.items()):
            encoded = (
                str(value).encode()
                if kind == 1
                else b"<" + value.encode("utf-16-be").hex().encode() + b">"
            )
            body += b"<" + source.hex().encode() + b"> " + encoded + b"\n"
        body += b"end" + operator + b"\n"
    return body + b"endcmap CMapName currentdict /CMap defineresource pop end end\n"


def frozen_positions(path, index, terms, explicit_text):
    with pdfium.PdfDocument(path) as document:
        text = document[index].get_textpage()
        content = text.get_text_range()
        result = {}
        for term in terms:
            # PDFium can collapse code41/code0041 in ToUnicode; it is a source
            # geometry observer, never the semantic oracle. The expected index
            # is calculated from the explicitly authored one-code/one-BMP run.
            assert explicit_text.count(term) == 1
            if term == "KEEP_FOLLOWING_LINE":
                assert content.count(term) == 1, (path.name, term, content)
                start = content.index(term)
            else:
                start = explicit_text.index(term)
                assert start + len(term) <= text.count_chars()
            result[term] = [
                list(text.get_charbox(i)) for i in range(start, start + len(term))
            ]
        return result


def generate(destination, verify_existing=False):
    old = next(
        row
        for row in json.loads((BASE.parent / "criteria.json").read_bytes())["cases"]
        if row["file"] == BASE.name
    )
    assert sha(BASE.read_bytes()) == old["sha256"] == BASE_PIN
    reader = PdfReader(BASE)
    base_fonts = reader.pages[0]["/Resources"]["/Font"]
    font_name = next(
        key for key, value in base_fonts.items() if "/ToUnicode" in value.get_object()
    )
    original = base_fonts[font_name].get_object()
    original_child = original["/DescendantFonts"][0].get_object()
    sfnt_data = original_child["/FontDescriptor"]["/FontFile2"].get_data()
    sfnt = TTFont(io.BytesIO(sfnt_data))
    old_gid = original_child["/CIDToGIDMap"].get_data()
    pairs = {}
    for section in re.findall(
        rb"beginbfchar(.*?)endbfchar", original["/ToUnicode"].get_data(), re.S
    ):
        for source, cp in re.findall(rb"<([0-9a-fA-F]+)>\s*<([0-9a-fA-F]+)>", section):
            cid = int(source, 16)
            cp = bytes.fromhex(cp.decode()).decode("utf-16-be")
            gid = int.from_bytes(old_gid[cid * 2 : cid * 2 + 2], "big")
            if gid:
                pairs[cp] = (cid, gid)
    assert all(c in pairs for c in PHRASE)
    original_widths = {
        int(original_child["/W"][0]) + i: float(width)
        for i, width in enumerate(original_child["/W"][1])
    }
    semantic = list(dict.fromkeys(PHRASE))
    cids = {character: 300 + index for index, character in enumerate(semantic)}
    widths = {character: original_widths[pairs[character][0]] for character in semantic}
    successes = [
        "one-byte",
        "two-byte",
        "mixed-value-length",
        "byte-boundaries",
        "cidrange",
        "bfrange-array",
        "alias-same",
        "Tc-positive",
        "Tc-negative",
        "TJ-positive-negative",
        "quote",
        "double-quote",
        "code32-nonspace",
        "space-two-byte",
        "shared-pages-alias",
        "shared-sibling-form",
        "shared-appearance-states",
        "bytewise-rectangle",
        "zero-code",
    ]
    refusals = [
        "prefix-ambiguous",
        "codespace-overlap",
        "out-of-codespace",
        "truncated-two-byte",
        "encoding-redefinition",
        "unicode-redefinition",
        "encoding-only",
        "unicode-only",
        "cid-unicode-conflict",
        "cid-overflow",
        "unicode-overflow",
        "usecmap-command",
        "usecmap-dictionary",
        "unknown-command",
        "unknown-name",
        "unknown-stream-key",
        "Tw-positive",
        "Tw-negative",
        "double-quote-Tw",
        "empty-Tj-Tw",
        "empty-TJ-Tw",
        "empty-double-quote-Tw",
        "zero-CID",
        "zero-Unicode",
        "bytewise-nonrectangle",
    ]
    rows = []
    for name in successes + refusals:
        refused = name in refusals
        writer = PdfWriter(clone_from=PdfReader(BASE))
        page = writer.pages[0]
        fonts = page["/Resources"]["/Font"]
        font = fonts[font_name].get_object()
        child = font["/DescendantFonts"][0].get_object()
        by_char = {
            " ": b"\x20",
            "公": b"\x41",
            "開": b"\x42",
            "文": b"\x43",
            "字": b"\x44",
            "保": b"\x45",
            "持": b"\x46",
        }
        by_char.update(
            {
                character: code(0x8100 + index, 2)
                for index, character in enumerate(REMOVE)
            }
        )
        by_char["秘"] = b"\x00\x41"
        spaces = [
            (b"\x20", b"\x7f"),
            (b"\x00\x01", b"\x00\xff"),
            (b"\x81\x00", b"\x81\xff"),
        ]
        if name == "one-byte":
            by_char = {
                character: code(0x20 if character == " " else 0x40 + index, 1)
                for index, character in enumerate(semantic)
            }
            by_char["公"] = b"\x00"
            spaces = [(b"\x00", b"\x7f")]
        elif name == "two-byte":
            by_char = {
                character: code(index, 2) for index, character in enumerate(semantic)
            }
            spaces = [(b"\x00\x00", b"\x00\xff")]
        elif name == "byte-boundaries":
            by_char.update(
                {
                    "公": b"\x7f",
                    "秘": b"\x00\x01",
                    "匿": b"\x00\xff",
                    "機": b"\x81\x00",
                    "鶴": b"\x81\xff",
                }
            )
        elif name == "code32-nonspace":
            by_char["公"], by_char[" "] = b"\x20", b"\x41"
        elif name == "space-two-byte":
            by_char[" "] = b"\x81\x06"
        elif name == "bytewise-rectangle":
            spaces = [
                (b"\x20", b"\x7f"),
                (b"\x00\x01", b"\x00\xff"),
                (b"\x81\x80", b"\x82\xff"),
            ]
            by_char.update(
                {
                    character: code(0x8180 + index, 2)
                    for index, character in enumerate(REMOVE)
                }
            )
            by_char["鶴"] = b"\x82\xff"
        encoding_map = {value: cids[character] for character, value in by_char.items()}
        unicode_map = {value: character for character, value in by_char.items()}
        selected = dict(by_char)
        if name == "alias-same":
            encoding_map[b"\x47"], unicode_map[b"\x47"] = cids["公"], "公"
            selected["公"] = b"\x47"
        if name == "cid-unicode-conflict":
            encoding_map[b"\x47"], unicode_map[b"\x47"] = cids["公"], "開"
        if name == "encoding-only":
            del unicode_map[by_char["機"]]
        elif name == "unicode-only":
            unicode_map[b"\x47"] = "公"
        elif name == "zero-CID":
            encoding_map[by_char["機"]] = 0
        elif name == "zero-Unicode":
            unicode_map[by_char["機"]] = "\0"
        elif name == "zero-code":
            spaces[1] = (b"\x00\x00", b"\x00\xff")
            del encoding_map[by_char["機"]], unicode_map[by_char["機"]]
            selected["機"] = b"\x00\x00"
            encoding_map[selected["機"]], unicode_map[selected["機"]] = cids["機"], "機"
        if name == "prefix-ambiguous":
            spaces.append((b"\x41\x00", b"\x41\xff"))
        elif name == "codespace-overlap":
            spaces.append((b"\x40", b"\x50"))
        elif name == "bytewise-nonrectangle":
            spaces[2] = (b"\x81\xfe", b"\x82\x01")
        enc = cmap(spaces, encoding_map, 1, ranges=name == "cidrange")
        uni = cmap(spaces, unicode_map, 2, array=name == "bfrange-array")
        if name == "encoding-redefinition":
            enc = enc.replace(
                b"endcmap", b"1 begincidchar <41> 999 endcidchar\nendcmap"
            )
        elif name == "unicode-redefinition":
            uni = uni.replace(
                b"endcmap", b"1 beginbfchar <41> <516c> endbfchar\nendcmap"
            )
        elif name == "cid-overflow":
            enc = enc.replace(b"<41> " + str(cids["公"]).encode(), b"<41> 65536")
        elif name == "unicode-overflow":
            uni = uni.replace(b"<41> <516c>", b"<41> <10000>")
        elif name == "usecmap-command":
            enc = enc.replace(b"endcmap", b"/Identity-H usecmap\nendcmap")
        elif name == "unknown-command":
            enc = enc.replace(b"endcmap", b"0 mysterycommand\nendcmap")
        elif name == "unknown-name":
            enc = enc.replace(
                b"/WMode 0 def", b"/WMode 0 def /MysteryMetadata /InvisibleMeaning def"
            )
        enc_obj = stream(enc)
        if name == "usecmap-dictionary":
            enc_obj[NameObject("/UseCMap")] = NameObject("/Identity-H")
        elif name == "unknown-stream-key":
            enc_obj[NameObject("/Mystery")] = NumberObject(1)
        font[NameObject("/Encoding")] = writer._add_object(enc_obj)
        font[NameObject("/ToUnicode")] = writer._add_object(stream(uni))
        cid_gid = bytearray((max(cids.values()) + 1) * 2)
        for character, cid in cids.items():
            cid_gid[cid * 2 : cid * 2 + 2] = pairs[character][1].to_bytes(2, "big")
        child[NameObject("/CIDToGIDMap")] = writer._add_object(stream(bytes(cid_gid)))
        child[NameObject("/W")] = ArrayObject(
            [
                NumberObject(min(cids.values())),
                ArrayObject([FloatObject(widths[character]) for character in semantic]),
            ]
        )
        encode = lambda phrase: b"".join(selected[character] for character in phrase)
        data = encode(PHRASE)
        if name == "out-of-codespace":
            data += b"\xff"
        elif name == "truncated-two-byte":
            data += b"\x81"
        tc = (
            1.25
            if name in ("Tc-positive", "TJ-positive-negative", "double-quote")
            else -1.25 if name == "Tc-negative" else 0
        )
        horizontal = (
            0.85
            if name in ("Tc-positive", "Tc-negative", "TJ-positive-negative")
            else 1
        )
        tw = (
            3
            if name in ("Tw-positive", "empty-Tj-Tw", "empty-TJ-Tw")
            else -3 if name == "Tw-negative" else 0
        )
        settings = f"{tc} Tc {tw} Tw {horizontal * 100:g} Tz\n".encode()
        token = lambda payload: b"<" + payload.hex().encode() + b">"
        operator = "Tj"
        text_command = token(data) + b" Tj\n"
        segments = [(PHRASE, None)]
        if name == "TJ-positive-negative":
            chunks = ["公開 ", REMOVE[:3], REMOVE[3:] + " 文字保持"]
            text_command = (
                b"["
                + token(encode(chunks[0]))
                + b" 125 "
                + token(encode(chunks[1]))
                + b" -75 "
                + token(encode(chunks[2]))
                + b"] TJ\n"
            )
            operator, segments = "TJ", [
                (chunks[0], 125),
                (chunks[1], -75),
                (chunks[2], None),
            ]
        elif name in ("quote", "double-quote", "double-quote-Tw"):
            text_command = (
                token(data) + b" '\n"
                if name == "quote"
                else (b"3 1.25 " if name == "double-quote-Tw" else b"0 1.25 ")
                + token(data)
                + b' "\n'
            )
            operator = "'" if name == "quote" else '"'
            if name == "double-quote-Tw":
                tw = 3
            if operator == '"':
                tc = 1.25
        elif name == "empty-Tj-Tw":
            text_command = b"<> Tj\n0 Tw\n" + text_command
        elif name == "empty-TJ-Tw":
            text_command = b"[] TJ\n0 Tw\n" + text_command
        elif name == "empty-double-quote-Tw":
            text_command = b'3 0 <> "\n0 Tw\n' + text_command
        initial_y = (
            676.4 if name in ("quote", "double-quote", "double-quote-Tw") else 650
        )
        commands = (
            b"BT "
            + font_name.encode()
            + b" 22 Tf 26.4 TL 1 0 0 1 60 "
            + str(initial_y).encode()
            + b" Tm\n"
            + settings
            + text_command
            + b"ET\nBT /F1 16 Tf 1 0 0 1 60 550 Tm (KEEP_FOLLOWING_LINE) Tj ET\n"
        )
        page[NameObject("/Contents")] = writer._add_object(stream(commands))
        bindings = [
            dict(path="page/0/font" + str(font_name), text=PHRASE, removed=REMOVE)
        ]
        second_terms = []
        if name.startswith("shared-"):
            second = writer.add_blank_page(width=612, height=792)
            outside_resources = copy.copy(page["/Resources"])
            outside_fontname = font_name
            if name == "shared-pages-alias":
                outside_fonts = copy.copy(fonts)
                outside_fonts[NameObject("/MixedAlias")] = writer._add_object(
                    copy.copy(font)
                )
                outside_resources[NameObject("/Font")] = outside_fonts
                outside_fontname = NameObject("/MixedAlias")
            shared_phrase = "公開 秘 文字保持"
            outside = (
                b"BT "
                + outside_fontname.encode()
                + b" 22 Tf 1 0 0 1 60 500 Tm "
                + token(encode(shared_phrase))
                + b" Tj ET\n"
            )
            second[NameObject("/Resources")] = outside_resources
            second[NameObject("/Contents")] = writer._add_object(stream(outside))
            binding_path = "page/1/font" + str(outside_fontname)
            if name == "shared-sibling-form":
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
                        Resources=outside_resources,
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
                binding_path = "page/1/form/0/font" + str(font_name)
            elif name == "shared-appearance-states":
                ap_commands = outside.replace(b"60 500 Tm", b"10 10 Tm")
                appearances = {}
                for state in ("N", "R", "D"):
                    appearances[NameObject("/" + state)] = writer._add_object(
                        stream(
                            ap_commands,
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
                            Resources=outside_resources,
                        )
                    )
                    bindings.append(
                        dict(
                            path="page/1/AP/0/" + state + "/font" + str(font_name),
                            text=shared_phrase,
                            removed="",
                        )
                    )
                annotation = DictionaryObject(
                    {
                        NameObject("/Type"): NameObject("/Annot"),
                        NameObject("/Subtype"): NameObject("/Stamp"),
                        NameObject("/Rect"): ArrayObject(
                            [
                                NumberObject(60),
                                NumberObject(400),
                                NumberObject(330),
                                NumberObject(450),
                            ]
                        ),
                        NameObject("/AP"): DictionaryObject(appearances),
                        NameObject("/F"): NumberObject(4),
                    }
                )
                second[NameObject("/Annots")] = ArrayObject(
                    [writer._add_object(annotation)]
                )
            bindings.append(dict(path=binding_path, text=shared_phrase, removed=""))
            second_terms = ["公開", "秘", "文字保持"]
        filename = (
            "empty-single-Tj-Tw"
            if name == "empty-Tj-Tw"
            else "empty-array-TJ-Tw" if name == "empty-TJ-Tw" else name
        )
        path = destination / (filename + ("-refused" if refused else "") + ".pdf")
        if verify_existing:
            payload = io.BytesIO()
            writer.write(payload)
            assert path.read_bytes() == payload.getvalue(), (
                "regenerated PDF differs",
                path.name,
            )
        else:
            assert not path.exists(), "case-insensitive duplicate fixture path"
            writer.write(path)
        row = dict(
            file=path.name,
            sha256=sha(path.read_bytes()),
            refused=refused,
            refusal_reason=name if refused else None,
            remove=REMOVE,
            removed=REMOVE,
            keep=["公開", "文字保持", "KEEP_FOLLOWING_LINE"],
            retained=["公開", "文字保持", "KEEP_FOLLOWING_LINE"],
            rect=old["rect"],
            second_page_terms=second_terms,
            source_sfnt_sha256=sha(sfnt_data),
            encoding_sha256=sha(enc),
            tounicode_sha256=sha(uni),
            encoding_bytes=enc.decode(),
            tounicode_bytes=uni.decode(),
            codespaces=[
                dict(low=lo.hex(), high=hi.hex(), byte_count=len(lo))
                for lo, hi in spaces
            ],
            encoding_route=[
                dict(
                    code_hex=value.hex(),
                    byte_count=len(value),
                    value=int.from_bytes(value, "big"),
                    cid=cid,
                    gid=int.from_bytes(cid_gid[cid * 2 : cid * 2 + 2], "big"),
                )
                for value, cid in sorted(encoding_map.items())
            ],
            unicode_route=[
                dict(
                    code_hex=value.hex(),
                    byte_count=len(value),
                    value=int.from_bytes(value, "big"),
                    unicode=character,
                )
                for value, character in sorted(unicode_map.items())
            ],
            operator=operator,
            content_sha256=sha(commands),
            content_bytes=commands.decode(),
            effective_tw=tw,
            effective_tc=tc,
            horizontal_scale=horizontal,
            font_size_pt=22,
            leading_pt=26.4,
            font_bindings=bindings,
            max_position_error_pt=0.5,
        )
        decomposition, x = [], 60.0
        for phrase, adjustment in segments:
            for character in phrase:
                value = selected[character]
                advance = (
                    widths[character] * 0.022 + tc + (tw if value == b"\x20" else 0)
                ) * horizontal
                decomposition.append(
                    dict(
                        code_hex=value.hex(),
                        byte_count=len(value),
                        value=int.from_bytes(value, "big"),
                        cid=cids[character],
                        gid=pairs[character][1],
                        unicode=character,
                        width=widths[character],
                        origin_pt=[x, 650.0],
                        advance_pt=advance,
                        removed=character in REMOVE,
                    )
                )
                x += advance
            if adjustment is not None:
                x -= adjustment * 0.022 * horizontal
        row["expected_decomposition"] = decomposition
        row["text_bytes_hex"] = data.hex()
        if not refused:
            box = frozen_positions(
                path, 0, [REMOVE] + row["keep"], PHRASE + "\r\nKEEP_FOLLOWING_LINE"
            )
            target = box.pop(REMOVE)
            x0, y0 = min(b[0] for b in target) - 1, min(b[1] for b in target) - 1
            x1, y1 = max(b[2] for b in target) + 1, max(b[3] for b in target) + 1
            row["rect"] = [x0, y0, x1 - x0, y1 - y0]
            row["boxes"] = {"0": box}
            if second_terms:
                row["boxes"]["1"] = frozen_positions(
                    path, 1, second_terms, shared_phrase
                )
            with pdfium.PdfDocument(path) as source_document:
                row["source_PDFium_extraction_observation"] = [
                    source_document[index].get_textpage().get_text_range()
                    for index in range(len(source_document))
                ]
            row["source_pypdf_extraction_observation"] = [
                source_page.extract_text() for source_page in PdfReader(path).pages
            ]
            row["source_external_extraction_is_Unicode_oracle"] = False
        rows.append(row)
    glyphs = []
    for character in semantic:
        _, gid = pairs[character]
        glyph = sfnt["glyf"][sfnt.getGlyphOrder()[gid]]
        coordinates, endpoints, flags = glyph.getCoordinates(sfnt["glyf"])
        instructions = (
            bytes(glyph.program.getBytecode()) if hasattr(glyph, "program") else b""
        )
        glyphs.append(
            dict(
                unicode=character,
                gid=gid,
                coordinates=[list(point) for point in coordinates],
                endpoints=list(endpoints),
                flags_hex=bytes(flags).hex(),
                metrics=list(sfnt["hmtx"][sfnt.getGlyphOrder()[gid]]),
                instruction_hex=instructions.hex(),
                components=(
                    [sfnt.getGlyphID(item.glyphName) for item in glyph.components]
                    if glyph.isComposite()
                    else []
                ),
            )
        )
    fixed = dict(
        version=1,
        scope="embedded horizontal 1/2-byte CMap, one BMP Unicode per code, effective Tw=0",
        original_source=dict(
            path=str(BASE.relative_to(ROOT)).replace("\\", "/"),
            sha256=sha(BASE.read_bytes()),
            sfnt_sha256=sha(sfnt_data),
        ),
        license=dict(
            path="licenses/NotoSansJP-OFL.txt",
            sha256=sha((ROOT / "licenses/NotoSansJP-OFL.txt").read_bytes()),
            source_font_sha256=sha((ROOT / "assets/fonts/NotoSansJP.ttf").read_bytes()),
        ),
        source_glyphs=glyphs,
        cases=rows,
        position_error_pt=0.5,
        outside_pixel_difference=0,
        boundary_pixels=1,
        dpi=[72, 96, 300],
        UI_source="shared-pages-alias.pdf",
        UI_refused="prefix-ambiguous-refused.pdf",
        general_M1_M3_M5_acceptance="withheld",
        MIAP_4096_observation="not evaluated by this CMap contract",
    )
    criteria = (json.dumps(fixed, ensure_ascii=False, indent=2) + "\n").encode()
    if verify_existing:
        assert (
            destination / "criteria.json"
        ).read_bytes() == criteria, "regenerated criteria differs"
    else:
        (destination / "criteria.json").write_bytes(criteria)
    total = sum(path.stat().st_size for path in destination.iterdir())
    assert total <= MAX_BYTES, (total, MAX_BYTES)
    return dict(
        cases=len(rows),
        success=len(successes),
        refused=len(refusals),
        files=len(rows) + 1,
        bytes=total,
        criteria_sha256=sha(criteria),
    )


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--verify-existing", action="store_true")
    parser.add_argument("--geometry-v2-from", type=Path)
    args = parser.parse_args()
    destination = args.output.resolve()
    destination.relative_to(ROOT)
    if not args.verify_existing:
        destination.mkdir(parents=True, exist_ok=False)
    print(
        json.dumps(
            generate_geometry_v2(
                destination, args.geometry_v2_from, args.verify_existing
            )
            if args.geometry_v2_from
            else generate(destination, args.verify_existing)
        )
    )


def generate_geometry_v2(destination, source, verify_existing=False):
    """Correct geometry from unchanged frozen inputs; never read product output."""
    source = source.resolve()
    source.relative_to(ROOT)
    raw = (source / "criteria.json").read_bytes()
    assert sha(raw) == V1_PIN
    fixed = json.loads(raw)
    changed_fields, audit = [], []

    def write_or_compare(path, payload):
        if verify_existing:
            assert path.read_bytes() == payload, ("v2 regeneration differs", path.name)
        else:
            with path.open("xb") as handle:
                handle.write(payload)

    def change(case, location, before, after):
        if before != after:
            changed_fields.append(
                dict(file=case["file"], field=location, before=before, after=after)
            )

    for case in fixed["cases"]:
        data = (source / case["file"]).read_bytes()
        assert sha(data) == case["sha256"]
        reader = PdfReader(io.BytesIO(data))
        font = next(
            value.get_object()
            for value in reader.pages[0]["/Resources"]["/Font"].values()
            if "/ToUnicode" in value.get_object()
        )
        payload = font["/DescendantFonts"][0]["/FontDescriptor"][
            "/FontFile2"
        ].get_data()
        assert sha(payload) == case["source_sfnt_sha256"]
        program = TTFont(io.BytesIO(payload))
        glyphset = program.getGlyphSet()
        factor = case["font_size_pt"] / program["head"].unitsPerEm
        font_bounds = {}

        def box(gid, origin, horizontal=1):
            if gid not in font_bounds:
                pen = BoundsPen(glyphset)
                glyphset[program.getGlyphOrder()[gid]].draw(pen)
                font_bounds[gid] = list(pen.bounds) if pen.bounds else None
            bounds = font_bounds[gid]
            if bounds is None:
                return None
            lo, bottom, hi, top = bounds
            x, y = origin
            return [
                x + lo * factor * horizontal,
                y + bottom * factor,
                x + hi * factor * horizontal,
                y + top * factor,
            ]

        primary = [
            box(entry["gid"], entry["origin_pt"], case["horizontal_scale"])
            for entry in case["expected_decomposition"]
        ]
        removed = [
            bounds
            for entry, bounds in zip(case["expected_decomposition"], primary)
            if entry["removed"] and bounds is not None
        ]
        kept = [
            bounds
            for entry, bounds in zip(case["expected_decomposition"], primary)
            if not entry["removed"] and bounds is not None
        ]
        envelope = [
            min(bounds[0] for bounds in removed),
            min(bounds[1] for bounds in removed),
            max(bounds[2] for bounds in removed),
            max(bounds[3] for bounds in removed),
        ]
        x0, y0, width, height = case["rect"]
        old_edges = [x0, y0, x0 + width, y0 + height]
        old_covers = all(
            old_edges[0] <= bounds[0]
            and old_edges[1] <= bounds[1]
            and old_edges[2] >= bounds[2]
            and old_edges[3] >= bounds[3]
            for bounds in removed
        )
        row = dict(
            file=case["file"],
            refused=case["refused"],
            source_pdf_sha256=case["sha256"],
            source_sfnt_sha256=case["source_sfnt_sha256"],
            v1_rect=case["rect"],
            intended_removed_outline_envelope_pt=envelope,
            v1_rect_contains_all_intended_remove_outlines=old_covers,
        )
        if case["refused"]:
            row["geometry_execution"] = (
                "UNEXECUTED: rejected before accepted geometry; intended semantic layout diagnostic only"
            )
            audit.append(row)
            write_or_compare(destination / case["file"], data)
            continue
        margin = 1.0
        rect = [
            envelope[0] - margin,
            envelope[1] - margin,
            envelope[2] - envelope[0] + 2 * margin,
            envelope[3] - envelope[1] + 2 * margin,
        ]
        edges = [rect[0], rect[1], rect[0] + rect[2], rect[1] + rect[3]]
        assert all(
            edges[0] <= bounds[0]
            and edges[1] <= bounds[1]
            and edges[2] >= bounds[2]
            and edges[3] >= bounds[3]
            for bounds in removed
        )
        assert all(
            bounds[2] <= edges[0]
            or edges[2] <= bounds[0]
            or bounds[3] <= edges[1]
            or edges[3] <= bounds[1]
            for bounds in kept
        ), "new geometry touches retained outline"
        change(case, "rect", case["rect"], rect)
        case["rect"] = rect
        text = "".join(entry["unicode"] for entry in case["expected_decomposition"])
        for term in ("公開", "文字保持"):
            start = text.index(term)
            true_boxes = primary[start : start + len(term)]
            assert all(bounds is not None for bounds in true_boxes)
            change(case, "boxes/0/" + term, case["boxes"]["0"][term], true_boxes)
            case["boxes"]["0"][term] = true_boxes
        if case["second_page_terms"]:
            binding = next(
                binding
                for binding in case["font_bindings"]
                if binding["path"].startswith("page/1/font")
                or binding["path"].startswith("page/1/form")
            )
            code_routes = {
                entry["code_hex"]: (entry["cid"], entry["gid"])
                for entry in case["encoding_route"]
            }
            meaning_gids = {}
            for entry in case["unicode_route"]:
                cp, gid = entry["unicode"], code_routes[entry["code_hex"]][1]
                assert cp not in meaning_gids or meaning_gids[cp] == gid
                meaning_gids[cp] = gid
            widths = {
                entry["unicode"]: entry["width"]
                for entry in case["expected_decomposition"]
            }
            outside_boxes, x = [], 60.0
            for cp in binding["text"]:
                outside_boxes.append(box(meaning_gids[cp], [x, 500.0]))
                x += widths[cp] * 0.022
            for term in case["second_page_terms"]:
                start = binding["text"].index(term)
                true_boxes = outside_boxes[start : start + len(term)]
                assert all(bounds is not None for bounds in true_boxes)
                change(case, "boxes/1/" + term, case["boxes"]["1"][term], true_boxes)
                case["boxes"]["1"][term] = true_boxes
        case["geometry_oracle"] = dict(
            method="fontTools BoundsPen true Bezier outlines from pinned input FontFile2 and explicit source code/CID/GID routes; frozen origins/Tc/TJ/Tm/quote arithmetic",
            margin_pt=margin,
            old_v1_rect=old_edges,
            primary_glyph_bounds_pt=primary,
            font_glyph_true_bounds={
                str(gid): bounds for gid, bounds in sorted(font_bounds.items())
            },
            rect_encompasses_all_removed_outlines=True,
            rect_excludes_all_retained_outlines=True,
            standard_font_footer_geometry="unchanged v1 measured geometry; input bytes/spacing/source are unchanged",
        )
        row.update(
            v2_rect=rect,
            margin_pt=margin,
            contains_all_removed_outlines=True,
            excludes_all_retained_outlines=True,
        )
        audit.append(row)
        write_or_compare(destination / case["file"], data)
    fixed["version"] = 2
    fixed["geometry_v2"] = dict(
        v1_criteria_sha256=V1_PIN,
        input_PDF_bytes_unchanged=44,
        source_semantics_commands_thresholds_unchanged=True,
        product_outputs_never_read=True,
        source_PDFium_char_indices_not_used_as_geometry_oracle=True,
        v1_historical_native_and_render_failures="retained; not replaced by v2",
        rectangle_audit=audit,
        changed_fields=changed_fields,
    )
    payload = (json.dumps(fixed, ensure_ascii=False, indent=2) + "\n").encode()
    write_or_compare(destination / "criteria.json", payload)
    core_paths = [destination / row["file"] for row in fixed["cases"]] + [
        destination / "criteria.json"
    ]
    assert len(core_paths) == len({path.name.casefold() for path in core_paths}) == 45
    extras = {path.name for path in destination.iterdir()} - {
        path.name for path in core_paths
    }
    assert extras <= {
        "decoder-safety.json",
        "decoder-safety-r2.json",
        "space-selection.json",
        "renderer-metrics.json",
    }, "unrelated file in the core fixture directory"
    total = sum(path.stat().st_size for path in core_paths)
    assert total <= 1_950_000, "v2 exceeds reserved 2MB with proof/headroom"
    return dict(
        cases=44,
        success=19,
        refused=25,
        files=45,
        bytes=total,
        criteria_sha256=sha(payload),
        changed_fields=len(changed_fields),
        same_PDF_bytes=True,
    )


if __name__ == "__main__":
    main()
