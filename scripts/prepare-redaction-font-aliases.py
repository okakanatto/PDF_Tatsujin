"""Freeze same-reference font resource aliases before changing their handling."""

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


def position(path, page_index, terms):
    with pdfium.PdfDocument(path) as pdf:
        text = pdf[page_index].get_textpage()
        content = text.get_text_range()
        result = {}
        for term in terms:
            assert content.count(term) == 1, (path, page_index, term, content)
            start = content.index(term)
            result[term] = [
                list(text.get_charbox(i)) for i in range(start, start + len(term))
            ]
        return result


def sha(data):
    return hashlib.sha256(data).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=False)
    root = Path(__file__).resolve().parents[1]
    base = root / "fixtures/redaction-copy/partial-text/visible.pdf"
    fixed = json.loads((base.parent / "criteria.json").read_text(encoding="utf-8"))
    baseline = next(row for row in fixed["cases"] if row["file"] == base.name)
    assert sha(base.read_bytes()) == baseline["sha256"]
    rows = []
    for name in (
        "same-page-tj",
        "same-page-tj-array",
        "restore-q",
        "form-only-alias",
        "form-restoration",
        "nested-form",
        "appearance-states",
        "other-page-alias",
        "alias-first",
        "private-alias-refused",
        "unknown-font-state-refused",
    ):
        writer = PdfWriter(clone_from=PdfReader(base))
        first = writer.pages[0]
        resources = first["/Resources"]
        fonts = resources["/Font"]
        font_name = next(k for k, v in fonts.items() if "/ToUnicode" in v.get_object())
        reference = fonts.raw_get(font_name)
        font = reference.get_object()
        mapping = re.findall(
            rb"<([0-9a-fA-F]+)>\s*<([0-9a-fA-F]+)>",
            font["/ToUnicode"].get_data().split(b"beginbfchar")[1],
        )
        reverse = {
            bytes.fromhex(v.decode()).decode("utf-16-be"): bytes.fromhex(k.decode())
            for k, v in mapping
        }
        alias = NameObject("/AliasFont")
        fonts[alias] = reference
        assert fonts.raw_get(alias) == fonts.raw_get(font_name)

        def text(term, x=60, y=430, key=alias, array=False):
            encoded = b"".join(reverse[c] for c in term)
            show = b"<" + encoded.hex().encode() + b"> Tj"
            if array:
                # Keep a real numeric TJ adjustment between glyph strings.
                show = (
                    b"[<"
                    + encoded[:5].hex().encode()
                    + b"> 12.5 <"
                    + encoded[5:].hex().encode()
                    + b">] TJ"
                )
            return (
                b"BT "
                + key.encode()
                + b" 22 Tf 1 0 0 1 "
                + str(x).encode()
                + b" "
                + str(y).encode()
                + b" Tm "
                + show
                + b" ET\n"
            )

        def form(commands, nested=False):
            local = DictionaryObject(
                {NameObject("/Font"): DictionaryObject({alias: reference})}
            )
            bbox = ArrayObject([NumberObject(v) for v in (0, 0, 612, 792)])
            result = writer._add_object(
                stream(
                    commands,
                    Type=NameObject("/XObject"),
                    Subtype=NameObject("/Form"),
                    BBox=bbox,
                    Resources=local,
                )
            )
            if nested:
                result = writer._add_object(
                    stream(
                        b"/Child Do\n",
                        Type=NameObject("/XObject"),
                        Subtype=NameObject("/Form"),
                        BBox=bbox,
                        Resources=DictionaryObject(
                            {
                                NameObject("/XObject"): DictionaryObject(
                                    {NameObject("/Child"): result}
                                )
                            }
                        ),
                    )
                )
            return result

        original = first.get_contents().get_data()
        # End with the original embedded font active, including outside a BT.
        original += b"BT " + font_name.encode() + b" 22 Tf ET\n"
        first_terms = list(baseline["keep"])
        second_terms = ["SHARED_ALIAS"]
        second = writer.add_blank_page(width=612, height=792)
        second[NameObject("/Resources")] = DictionaryObject(
            {NameObject("/Font"): DictionaryObject({alias: reference})}
        )
        second[NameObject("/Contents")] = writer._add_object(
            stream(text("SHARED_ALIAS", y=500))
        )
        if name == "other-page-alias":
            pass
        elif name == "appearance-states":
            ap = form(text("AP_ALIAS", x=10, y=10))
            states = DictionaryObject({NameObject("/On"): ap, NameObject("/Off"): ap})
            second[NameObject("/Annots")] = ArrayObject(
                [
                    writer._add_object(
                        DictionaryObject(
                            {
                                NameObject("/Type"): NameObject("/Annot"),
                                NameObject("/Subtype"): NameObject("/Stamp"),
                                NameObject("/Rect"): ArrayObject(
                                    [NumberObject(v) for v in (0, 0, 612, 792)]
                                ),
                                NameObject("/AP"): DictionaryObject(
                                    {
                                        NameObject("/N"): states,
                                        NameObject("/R"): ap,
                                        NameObject("/D"): ap,
                                    }
                                ),
                                NameObject("/AS"): NameObject("/On"),
                                NameObject("/F"): NumberObject(4),
                            }
                        )
                    )
                ]
            )
        elif name == "private-alias-refused":
            writer._root_object[NameObject("/PrivateOriginalFont")] = reference
        elif name == "unknown-font-state-refused":
            resources[NameObject("/ExtGState")] = DictionaryObject(
                {
                    NameObject("/FontState"): writer._add_object(
                        DictionaryObject(
                            {
                                NameObject("/Font"): ArrayObject(
                                    [reference, NumberObject(22)]
                                )
                            }
                        )
                    )
                }
            )
            original += b"/FontState gs\n" + text("ALIAS_PUBLIC")
        elif name in ("form-only-alias", "form-restoration", "nested-form"):
            resources[NameObject("/XObject")] = DictionaryObject(
                {
                    NameObject("/Outside"): form(
                        text("FORM_ALIAS"), name == "nested-form"
                    )
                }
            )
            original += b"/Outside Do\n"
            first_terms.append("FORM_ALIAS")
            if name == "form-restoration":
                # Form processing must restore the caller's active resource name.
                encoded = b"".join(reverse[c] for c in "AFTER_FORM")
                original += (
                    b"BT 1 0 0 1 60 370 Tm <" + encoded.hex().encode() + b"> Tj ET\n"
                )
                first_terms.append("AFTER_FORM")
        elif name == "restore-q":
            original += b"q\n" + text("ALIAS_PUBLIC") + b"Q\n"
            encoded = b"".join(reverse[c] for c in "RESTORED_FONT")
            original += (
                b"BT 1 0 0 1 60 370 Tm <" + encoded.hex().encode() + b"> Tj ET\n"
            )
            first_terms.extend(["ALIAS_PUBLIC", "RESTORED_FONT"])
        else:
            if name == "alias-first":
                original = original.replace(
                    font_name.encode() + b" ", alias.encode() + b" "
                )
            original += text(
                "ALIAS_PUBLIC",
                key=font_name if name == "alias-first" else alias,
                array=name == "same-page-tj-array",
            )
            first_terms.append("ALIAS_PUBLIC")
        first[NameObject("/Contents")] = writer._add_object(stream(original))
        path = args.output / (name + ".pdf")
        writer.write(path)
        row = dict(
            file=path.name,
            sha256=sha(path.read_bytes()),
            refused=name.endswith("refused"),
            remove=baseline["remove"],
            keep=first_terms,
            second_page_terms=second_terms,
            rect=baseline["rect"],
            max_position_error_pt=0.5,
            same_font_reference=True,
        )
        if not row["refused"]:
            row["boxes"] = {
                "0": position(path, 0, first_terms),
                "1": position(path, 1, second_terms),
            }
        rows.append(row)
    criteria = args.output / "criteria.json"
    criteria.write_text(
        json.dumps(
            {
                "scope": "Same-reference font resource names; old criteria unchanged",
                "raster_dpi": 300,
                "outside_changed_pixels": 0,
                "boundary_px": 1,
                "cases": rows,
            },
            ensure_ascii=False,
            indent=2,
        )
        + "\n",
        encoding="utf-8",
    )
    print(
        json.dumps({"cases": len(rows), "criteria_sha256": sha(criteria.read_bytes())})
    )


if __name__ == "__main__":
    main()
