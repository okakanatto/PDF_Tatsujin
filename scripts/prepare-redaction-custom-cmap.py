"""Freeze fixed two-byte custom-CMap redaction inputs before implementation."""

import argparse
import copy
import hashlib
import json
import re
from pathlib import Path

import pypdfium2 as pdfium
from pypdf import PdfReader, PdfWriter
from pypdf.generic import ArrayObject, DictionaryObject, NameObject, NumberObject
from prepare_redaction_helpers import stream


def sha(data):
    return hashlib.sha256(data).hexdigest()


def cmap(body, kind):
    return (
        b"/CIDInit /ProcSet findresource begin 12 dict begin begincmap\n"
        b"/CIDSystemInfo << /Registry (Adobe) /Ordering (Identity) /Supplement 0 >> def\n"
        + f"/CMapName /CustomFixed{kind} def /CMapType {kind} def /WMode 0 def\n".encode()
        + b"1 begincodespacerange <4100> <41ff> endcodespacerange\n"
        + body
        + b"endcmap CMapName currentdict /CMap defineresource pop end end\n"
    )


def positions(path, page_index, terms):
    with pdfium.PdfDocument(path) as document:
        text = document[page_index].get_textpage()
        content = text.get_text_range()
        result = {}
        for term in terms:
            assert content.count(term) == 1, (path, term, content)
            start = content.index(term)
            result[term] = [
                list(text.get_charbox(i)) for i in range(start, start + len(term))
            ]
        return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=False)
    root = Path(__file__).resolve().parents[1]
    base = root / "fixtures/redaction-copy/partial-text"
    old = next(
        row
        for row in json.loads((base / "criteria.json").read_text(encoding="utf-8"))[
            "cases"
        ]
        if row["file"] == "identity-H.pdf"
    )
    assert sha((base / old["file"]).read_bytes()) == old["sha256"]
    names = [
        "cidchar",
        "cidrange",
        "bfrange-array",
        "TJ",
        "shared-page",
        "quote",
        "double-quote",
        "alias-same",
        "odd-refused",
        "unmapped-refused",
        "codespace-refused",
        "overlap-refused",
        "count-refused",
        "inherit-refused",
        "multi-refused",
        "alias-conflict-refused",
        "Tw-refused",
        "vertical-refused",
    ]
    rows = []
    for name in names:
        writer = PdfWriter(clone_from=PdfReader(base / old["file"]))
        page = writer.pages[0]
        fonts = page["/Resources"]["/Font"]
        font_name = next(k for k, v in fonts.items() if "/ToUnicode" in v.get_object())
        font = fonts[font_name]
        child = font["/DescendantFonts"][0].get_object()
        mappings = re.findall(
            rb"beginbfchar\s*<([0-9a-fA-F]+)>\s*<([0-9a-fA-F]+)>",
            font["/ToUnicode"].get_data(),
        )
        pairs = {
            int(code, 16): bytes.fromhex(value.decode()) for code, value in mappings
        }
        body = b"".join(
            f"1 begincidchar <{0x4100 + code:04x}> {512 + code} endcidchar\n".encode()
            for code in pairs
        )
        if name in ("cidrange", "bfrange-array"):
            body = b"1 begincidrange <4100> <417f> 512 endcidrange\n"
        unicode_body = b"".join(
            f"1 beginbfchar <{0x4100 + code:04x}> <{value.hex()}> endbfchar\n".encode()
            for code, value in pairs.items()
        )
        if name == "bfrange-array":
            unicode_body = (
                b"1 beginbfrange <4100> <417f> ["
                + b" ".join(
                    b"<" + pairs[code].hex().encode() + b">" for code in range(128)
                )
                + b"] endbfrange\n"
            )
        encoded = lambda phrase: b"".join(
            (
                0x4100
                + next(
                    code
                    for code, value in pairs.items()
                    if value == c.encode("utf-16-be")
                )
            ).to_bytes(2, "big")
            for c in phrase
        )
        phrase = "公開 秘匿機密亀鶴 文字保持"
        data = encoded(phrase)
        extra = []
        following = b""
        if name in ("quote", "double-quote"):
            extra = ["EXTRA_LINE"]
            quoted = b"<" + encoded(extra[0]).hex().encode() + b">"
            following = (
                (quoted + b" '\n")
                if name == "quote"
                else (b"0 1.25 " + quoted + b' "\n')
            )
        if name in ("alias-same", "alias-conflict-refused"):
            body += b"1 begincidchar <4180> 513 endcidchar\n"
            unicode_body += (
                b"1 beginbfchar <4180> <"
                + (b"516c" if name == "alias-same" else b"958b")
                + b"> endbfchar\n"
            )
            data = b"\x41\x80" + data[2:]
        encoding = cmap(body, 1)
        unicode = cmap(unicode_body, 2)
        if name == "odd-refused":
            data += b"\x41"
        elif name == "unmapped-refused":
            data += b"\x41\xfe"
        elif name == "codespace-refused":
            encoding = encoding.replace(b"<4100> <41ff>", b"<4100> <4102>")
        elif name == "overlap-refused":
            encoding = encoding.replace(
                b"endcmap", b"1 begincidchar <4101> 514 endcidchar\nendcmap"
            )
        elif name == "count-refused":
            encoding = encoding.replace(b"1 begincidchar", b"2 begincidchar", 1)
        elif name == "inherit-refused":
            encoding = encoding.replace(b"endcmap", b"/Identity-H usecmap\nendcmap")
        elif name == "multi-refused":
            unicode = unicode.replace(b"<4101> <516c>", b"<4101> <516c958b>")
        elif name == "vertical-refused":
            encoding = encoding.replace(b"/WMode 0", b"/WMode 1")
        old_gid = child["/CIDToGIDMap"].get_data()
        child[NameObject("/CIDToGIDMap")] = writer._add_object(
            stream(b"\0" * 1024 + old_gid)
        )
        child[NameObject("/W")] = ArrayObject([NumberObject(512), child["/W"][1]])
        font[NameObject("/Encoding")] = writer._add_object(stream(encoding))
        font[NameObject("/ToUnicode")] = writer._add_object(stream(unicode))
        text = b"<" + data.hex().encode() + b"> Tj\n"
        settings = b""
        if name == "TJ":
            text = (
                b"[<"
                + data[:6].hex().encode()
                + b"> 125 <"
                + data[6:].hex().encode()
                + b">] TJ\n"
            )
            settings = b"1.25 Tc 85 Tz\n"
        elif name == "Tw-refused":
            settings = b"3 Tw\n"
        commands = (
            b"BT " + font_name.encode() + b" 22 Tf 26.4 TL\n"
            b"1 0 0 1 60 650 Tm\n" + settings + text + following + b"ET\n"
            b"BT /F1 16 Tf 1 0 0 1 60 550 Tm (KEEP_FOLLOWING_LINE) Tj ET\n"
        )
        page[NameObject("/Contents")] = writer._add_object(stream(commands))
        second_terms = []
        if name == "shared-page":
            second = writer.add_blank_page(width=612, height=792)
            second[NameObject("/Resources")] = copy.copy(page["/Resources"])
            second[NameObject("/Contents")] = writer._add_object(
                stream(
                    b"BT "
                    + font_name.encode()
                    + b" 22 Tf 1 0 0 1 60 650 Tm <"
                    + encoded("公開 文字保持").hex().encode()
                    + b"> Tj ET\n"
                )
            )
            second_terms = ["公開", "文字保持"]
        path = args.output / (name + ".pdf")
        writer.write(path)
        refused = name.endswith("refused")
        keep = old["keep"] + extra
        row = {
            "file": path.name,
            "sha256": sha(path.read_bytes()),
            "refused": refused,
            "remove": old["remove"],
            "keep": keep,
            "rect": old["rect"],
            "second_page_terms": second_terms,
            "max_position_error_pt": 0.5,
            "source_encoding": {str(0x4100 + code): 512 + code for code in pairs},
        }
        if not refused:
            all_boxes = positions(path, 0, [old["remove"]] + keep)
            target = all_boxes.pop(old["remove"])
            x0, y0 = min(b[0] for b in target) - 1, min(b[1] for b in target) - 1
            x1, y1 = max(b[2] for b in target) + 1, max(b[3] for b in target) + 1
            row["rect"] = [x0, y0, x1 - x0, y1 - y0]
            row["boxes"] = {"0": all_boxes}
            if second_terms:
                row["boxes"]["1"] = positions(path, 1, second_terms)
            if name == "alias-same":
                row["source_encoding"][str(0x4180)] = 513
        rows.append(row)
    fixed = {
        "version": 1,
        "scope": "Fixed two-byte horizontal custom CMap only",
        "cases": rows,
        "outside_pixel_difference": 0,
        "native_GUI": "未実行",
    }
    target = args.output / "criteria.json"
    target.write_text(
        json.dumps(fixed, ensure_ascii=False, indent=2) + "\n", encoding="utf-8"
    )
    print(json.dumps({"cases": len(rows), "criteria_sha256": sha(target.read_bytes())}))


if __name__ == "__main__":
    main()
