"""Freeze shared-font use cases before changing the redaction engine."""

import argparse
import copy
import hashlib
import json
import re
from pathlib import Path

import pypdfium2 as pdfium
from pypdf import PdfReader, PdfWriter
from pypdf.generic import (
    ArrayObject,
    ByteStringObject,
    DictionaryObject,
    NameObject,
    NumberObject,
)
from prepare_redaction_helpers import stream


def sha(data):
    return hashlib.sha256(data).hexdigest()


def position(path, page_index, terms):
    with pdfium.PdfDocument(path) as pdf:
        page = pdf[page_index]
        text = page.get_textpage()
        content = text.get_text_range()
        result = {}
        for term in terms:
            assert content.count(term) == 1, (path, page_index, term, content)
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
    base = root / "fixtures/redaction-copy/partial-text/visible.pdf"
    old = json.loads((base.parent / "criteria.json").read_text(encoding="utf-8"))
    original = next(row for row in old["cases"] if row["file"] == base.name)
    assert sha(base.read_bytes()) == original["sha256"]
    rows = []
    names = [
        "pages",
        "alias-font",
        "copied-streams",
        "inherited",
        "unused",
        "sibling-form",
        "nested-outside-form",
        "appearance",
        "appearance-states",
        "private-alias-refused",
        "shared-target-form-refused",
        "cycle-refused",
    ]
    for name in names:
        writer = PdfWriter(clone_from=PdfReader(base))
        first = writer.pages[0]
        resources = first["/Resources"]
        fonts = resources["/Font"]
        font_name = next(k for k, v in fonts.items() if "/ToUnicode" in v.get_object())
        reference = fonts.raw_get(font_name)
        font = reference.get_object()
        mapping = dict(
            re.findall(
                rb"<([0-9a-fA-F]+)>\s*<([0-9a-fA-F]+)>",
                font["/ToUnicode"].get_data().split(b"beginbfchar")[1],
            )
        )
        reverse = {
            bytes.fromhex(v.decode()).decode("utf-16-be"): bytes.fromhex(k.decode())
            for k, v in mapping.items()
        }
        phrase = "公開 秘 文字保持"
        encoded = b"".join(reverse[c] for c in phrase)
        commands = (
            b"BT "
            + font_name.encode()
            + b" 22 Tf 1 0 0 1 60 500 Tm <"
            + encoded.hex().encode()
            + b"> Tj ET\n"
        )
        outside_resources = copy.copy(resources)
        if name in ("alias-font", "copied-streams"):
            declaration = copy.copy(font)
            if name == "copied-streams":
                declaration[NameObject("/ToUnicode")] = writer._add_object(
                    stream(font["/ToUnicode"].get_data())
                )
                descriptor = copy.copy(font["/FontDescriptor"])
                descriptor[NameObject("/FontFile2")] = writer._add_object(
                    stream(descriptor["/FontFile2"].get_data())
                )
                declaration[NameObject("/FontDescriptor")] = writer._add_object(
                    descriptor
                )
            outside_fonts = copy.copy(fonts)
            outside_fonts[font_name] = writer._add_object(declaration)
            outside_resources[NameObject("/Font")] = outside_fonts
        second = writer.add_blank_page(width=612, height=792)
        second[NameObject("/Resources")] = outside_resources
        second[NameObject("/Contents")] = writer._add_object(stream(commands))
        if name == "inherited":
            parent = first.raw_get("/Parent").get_object()
            parent[NameObject("/Resources")] = resources
            del first["/Resources"]
            del second["/Resources"]
        elif name == "unused":
            second[NameObject("/Contents")] = writer._add_object(
                stream(b"q 0 0 1 rg 60 480 100 30 re f Q\n")
            )
        elif name in ("sibling-form", "nested-outside-form"):
            form = stream(
                commands,
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
            if name == "nested-outside-form":
                inner = writer._add_object(form)
                form = stream(
                    b"/Inner Do\n",
                    Type=NameObject("/XObject"),
                    Subtype=NameObject("/Form"),
                    BBox=form["/BBox"],
                    Resources=DictionaryObject(
                        {
                            NameObject("/XObject"): DictionaryObject(
                                {NameObject("/Inner"): inner}
                            )
                        }
                    ),
                )
            second[NameObject("/Resources")] = DictionaryObject(
                {
                    NameObject("/XObject"): DictionaryObject(
                        {NameObject("/Outside"): writer._add_object(form)}
                    )
                }
            )
            second[NameObject("/Contents")] = writer._add_object(
                stream(b"/Outside Do\n")
            )
        elif name in ("appearance", "appearance-states"):
            ap_commands = commands.replace(b"60 500 Tm", b"10 10 Tm")
            appearance = writer._add_object(
                stream(
                    ap_commands,
                    Type=NameObject("/XObject"),
                    Subtype=NameObject("/Form"),
                    BBox=ArrayObject(
                        [
                            NumberObject(0),
                            NumberObject(0),
                            NumberObject(300),
                            NumberObject(50),
                        ]
                    ),
                    Resources=outside_resources,
                )
            )
            states = DictionaryObject(
                {NameObject("/On"): appearance, NameObject("/Off"): appearance}
            )
            ap = DictionaryObject(
                {
                    NameObject("/N"): appearance if name == "appearance" else states,
                    NameObject("/R"): appearance,
                    NameObject("/D"): appearance,
                }
            )
            annotation = DictionaryObject(
                {
                    NameObject("/Type"): NameObject("/Annot"),
                    NameObject("/Subtype"): NameObject("/Stamp"),
                    NameObject("/Rect"): ArrayObject(
                        [
                            NumberObject(50),
                            NumberObject(300),
                            NumberObject(350),
                            NumberObject(350),
                        ]
                    ),
                    NameObject("/AP"): ap,
                    NameObject("/AS"): NameObject("/On"),
                    NameObject("/F"): NumberObject(4),
                }
            )
            second[NameObject("/Annots")] = ArrayObject(
                [writer._add_object(annotation)]
            )
            second[NameObject("/Contents")] = writer._add_object(stream(b""))
        elif name == "private-alias-refused":
            writer._root_object[NameObject("/PrivateOriginalFont")] = reference
        elif name == "shared-target-form-refused":
            target = writer._add_object(
                stream(
                    first.get_contents().get_data(),
                    Type=NameObject("/XObject"),
                    Subtype=NameObject("/Form"),
                    BBox=second["/MediaBox"],
                    Resources=resources,
                )
            )
            for page in (first, second):
                page[NameObject("/Resources")] = DictionaryObject(
                    {
                        NameObject("/XObject"): DictionaryObject(
                            {NameObject("/Shared"): target}
                        )
                    }
                )
                page[NameObject("/Contents")] = writer._add_object(
                    stream(b"/Shared Do\n")
                )
        elif name == "cycle-refused":
            form = stream(
                b"/Self Do\n",
                Type=NameObject("/XObject"),
                Subtype=NameObject("/Form"),
                BBox=second["/MediaBox"],
                Resources=DictionaryObject({NameObject("/Font"): fonts}),
            )
            reference_form = writer._add_object(form)
            form["/Resources"][NameObject("/XObject")] = DictionaryObject(
                {NameObject("/Self"): reference_form}
            )
            second[NameObject("/Resources")] = form["/Resources"]
            second[NameObject("/Contents")] = writer._add_object(stream(b"/Self Do\n"))
        path = args.output / (name + ".pdf")
        writer.write(path)
        refused = name.endswith("-refused")
        terms = (
            []
            if name in ("unused", "appearance", "appearance-states") or refused
            else ["公開", "秘", "文字保持"]
        )
        rows.append(
            {
                "file": path.name,
                "sha256": sha(path.read_bytes()),
                "refused": refused,
                "remove": original["remove"],
                "rect": original["rect"],
                "keep": original["keep"],
                "boxes": {
                    "0": original["boxes"],
                    "1": {} if refused else position(path, 1, terms),
                },
                "second_page_terms": terms,
                "max_position_error_pt": 0.5,
                "outside_changed_pixels": 0,
                "appearance": name.startswith("appearance"),
            }
        )
    fixed = {
        "version": 1,
        "source_sha256": original["sha256"],
        "cases": rows,
        "outside_same_single_character_is_preserved": True,
        "original_font_and_mapping_streams_absent": True,
        "native_GUI": "未実行",
    }
    destination = args.output / "criteria.json"
    destination.write_text(
        json.dumps(fixed, ensure_ascii=False, indent=2) + "\n", encoding="utf-8"
    )
    print(
        json.dumps(
            {"cases": len(rows), "criteria_sha256": sha(destination.read_bytes())}
        )
    )


if __name__ == "__main__":
    main()
