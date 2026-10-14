"""Freeze direct-font, inherited Form state and composite resource alias checks."""

import argparse
import copy
import importlib.util
import json
import re
from pathlib import Path

from pypdf import PdfReader, PdfWriter
from pypdf.generic import DictionaryObject, NameObject
from prepare_redaction_helpers import stream

spec = importlib.util.spec_from_file_location(
    "aliases", Path(__file__).with_name("prepare-redaction-font-aliases.py")
)
aliases = importlib.util.module_from_spec(spec)
spec.loader.exec_module(aliases)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=False)
    root = Path(__file__).resolve().parents[1]
    rows = []
    for name in ("direct-standard", "inherited-form-font", "identity-H-alias"):
        folder = root / "fixtures/redaction-copy/font-aliases-v1"
        fixed = json.loads((folder / "criteria.json").read_text(encoding="utf-8"))
        row = copy.deepcopy(
            next(
                c
                for c in fixed["cases"]
                if c["file"]
                == (
                    "form-restoration.pdf"
                    if name == "inherited-form-font"
                    else "same-page-tj.pdf"
                )
            )
        )
        if name == "identity-H-alias":
            folder = root / "fixtures/redaction-copy/partial-text"
            fixed = json.loads((folder / "criteria.json").read_text(encoding="utf-8"))
            row = copy.deepcopy(
                next(c for c in fixed["cases"] if c["file"] == "identity-H.pdf")
            )
            row["second_page_terms"] = []
        assert aliases.sha((folder / row["file"]).read_bytes()) == row["sha256"]
        writer = PdfWriter(clone_from=PdfReader(folder / row["file"]))
        first = writer.pages[0]
        fonts = first["/Resources"]["/Font"]
        original_key = next(
            k
            for k, v in fonts.items()
            if "/ToUnicode" in v.get_object() and k != "/AliasFont"
        )
        if name == "direct-standard":
            fonts[NameObject("/F1")] = copy.copy(fonts["/F1"])
        elif name == "inherited-form-font":
            form = first["/Resources"]["/XObject"]["/Outside"]
            reference = fonts.raw_get(original_key)
            form[NameObject("/Resources")] = DictionaryObject(
                {NameObject("/Font"): DictionaryObject({original_key: reference})}
            )
            form.set_data(form.get_data().replace(b"/AliasFont 22 Tf ", b""))
        else:
            reference = fonts.raw_get(original_key)
            fonts[NameObject("/AliasFont")] = reference
            data = reference.get_object()["/ToUnicode"].get_data()
            mapping = []
            for body in re.findall(rb"beginbfchar(.*?)endbfchar", data, re.S):
                mapping += re.findall(rb"<([0-9a-fA-F]+)>\s*<([0-9a-fA-F]+)>", body)
            reverse = {
                bytes.fromhex(v.decode()).decode("utf-16-be"): bytes.fromhex(k.decode())
                for k, v in mapping
            }
            phrase = "ALIAS_PUBLIC"
            encoded = b"".join(reverse[c] for c in phrase)
            first[NameObject("/Contents")] = writer._add_object(
                stream(
                    first.get_contents().get_data()
                    + b"BT /AliasFont 22 Tf 1 0 0 1 60 430 Tm <"
                    + encoded.hex().encode()
                    + b"> Tj ET\n"
                )
            )
            row["keep"].append(phrase)
            second = writer.add_blank_page(width=612, height=792)
            second[NameObject("/Resources")] = DictionaryObject()
            second[NameObject("/Contents")] = writer._add_object(stream(b""))
        path = args.output / (name + ".pdf")
        writer.write(path)
        row.update(
            file=path.name,
            sha256=aliases.sha(path.read_bytes()),
            refused=False,
            max_position_error_pt=0.5,
            boxes={
                "0": aliases.position(path, 0, row["keep"]),
                "1": aliases.position(path, 1, row["second_page_terms"]),
            },
        )
        rows.append(row)
    path = args.output / "criteria.json"
    path.write_text(
        json.dumps(
            {
                "cases": rows,
                "raster_dpi": 300,
                "outside_changed_pixels": 0,
                "boundary_px": 1,
            },
            ensure_ascii=False,
            indent=2,
        )
        + "\n",
        encoding="utf-8",
    )
    print(
        json.dumps(
            {"cases": len(rows), "criteria_sha256": aliases.sha(path.read_bytes())}
        )
    )


if __name__ == "__main__":
    main()
