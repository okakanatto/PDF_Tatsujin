"""Freeze selected quote operators, Unicode ranges and transformed CMap Forms."""

import argparse
import json
import re
from pathlib import Path

from pypdf import PdfReader, PdfWriter
from pypdf.generic import (
    ArrayObject,
    DictionaryObject,
    FloatObject,
    NameObject,
    NumberObject,
)
from prepare_redaction_helpers import stream
import importlib.util

specification = importlib.util.spec_from_file_location(
    "custom_generator", Path(__file__).with_name("prepare-redaction-custom-cmap.py")
)
custom = importlib.util.module_from_spec(specification)
specification.loader.exec_module(custom)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=False)
    root = Path(__file__).resolve().parents[1]
    base = root / "fixtures/redaction-copy/custom-cmap-v1"
    raw = (base / "criteria.json").read_bytes()
    assert (
        custom.sha(raw)
        == "044024c5bf7ec5a52802e6ba0d0265237aefee83471147207c61a882f7d4a36f"
    )
    source = json.loads(raw)["cases"][0]
    assert custom.sha((base / source["file"]).read_bytes()) == source["sha256"]
    rows = []
    for name in (
        "target-quote",
        "target-double-quote",
        "bfrange-scalar",
        "rotated-form",
    ):
        writer = PdfWriter(clone_from=PdfReader(base / source["file"]))
        page = writer.pages[0]
        font_name = next(
            k
            for k, v in page["/Resources"]["/Font"].items()
            if "/ToUnicode" in v.get_object()
        )
        font = page["/Resources"]["/Font"][font_name]
        unicode = font["/ToUnicode"].get_data()
        pairs = re.findall(
            rb"beginbfchar\s*<([0-9a-fA-F]+)>\s*<([0-9a-fA-F]+)>", unicode
        )
        codes = {
            bytes.fromhex(cp.decode()).decode("utf-16-be"): int(code, 16)
            for code, cp in pairs
        }
        encoded = lambda phrase: b"".join(codes[c].to_bytes(2, "big") for c in phrase)
        commands = page.get_contents().get_data()
        keep = list(source["keep"])
        if name.startswith("target-"):
            keep.append("EXTRA_LINE")
            quote = b"'" if name == "target-quote" else b'"'
            prefix = b"" if name == "target-quote" else b"0 1.25 "
            commands = (
                b"BT "
                + font_name.encode()
                + b" 22 Tf 26.4 TL 1 0 0 1 60 650 Tm <"
                + encoded("EXTRA_LINE").hex().encode()
                + b"> Tj\n"
                + prefix
                + b"<"
                + encoded("公開 秘匿機密亀鶴 文字保持").hex().encode()
                + b"> "
                + quote
                + b"\nET\n0 Tc\n"
                b"BT /F1 16 Tf 1 0 0 1 60 550 Tm (KEEP_FOLLOWING_LINE) Tj ET\n"
            )
        elif name == "bfrange-scalar":
            body = b"".join(
                b"1 beginbfchar <" + code + b"> <" + cp + b"> endbfchar\n"
                for code, cp in pairs
                if not 0x4120 <= int(code, 16) <= 0x417E
            )
            body += b"1 beginbfrange <4120> <417e> <0020> endbfrange\n"
            font[NameObject("/ToUnicode")] = writer._add_object(
                stream(custom.cmap(body, 2))
            )
        if name == "rotated-form":
            resources = page["/Resources"]
            bounds = ArrayObject(
                [NumberObject(0), NumberObject(0), NumberObject(612), NumberObject(792)]
            )
            form = stream(
                commands,
                Type=NameObject("/XObject"),
                Subtype=NameObject("/Form"),
                BBox=bounds,
                Resources=resources,
            )
            inner = writer._add_object(form)
            outer = stream(
                b"/Inner Do\n",
                Type=NameObject("/XObject"),
                Subtype=NameObject("/Form"),
                BBox=bounds,
                Resources=DictionaryObject(
                    {
                        NameObject("/XObject"): DictionaryObject(
                            {NameObject("/Inner"): inner}
                        )
                    }
                ),
            )
            page[NameObject("/Resources")] = DictionaryObject(
                {
                    NameObject("/XObject"): DictionaryObject(
                        {NameObject("/Outer"): writer._add_object(outer)}
                    )
                }
            )
            commands = b"/Outer Do\n"
            page[NameObject("/Rotate")] = NumberObject(90)
            page[NameObject("/CropBox")] = ArrayObject(
                [
                    NumberObject(12),
                    NumberObject(18),
                    NumberObject(600),
                    NumberObject(780),
                ]
            )
            page[NameObject("/UserUnit")] = FloatObject(1.5)
        page[NameObject("/Contents")] = writer._add_object(stream(commands))
        path = args.output / (name + ".pdf")
        writer.write(path)
        boxes = custom.positions(path, 0, [source["remove"]] + keep)
        target = boxes.pop(source["remove"])
        x0, y0 = min(b[0] for b in target) - 1, min(b[1] for b in target) - 1
        x1, y1 = max(b[2] for b in target) + 1, max(b[3] for b in target) + 1
        rows.append(
            {
                "file": path.name,
                "sha256": custom.sha(path.read_bytes()),
                "refused": False,
                "remove": source["remove"],
                "keep": keep,
                "second_page_terms": [],
                "rect": [x0, y0, x1 - x0, y1 - y0],
                "boxes": {"0": boxes},
                "source_encoding": source["source_encoding"],
                "max_position_error_pt": 0.5,
            }
        )
    fixed = {
        "version": 1,
        "cases": rows,
        "outside_pixel_difference": 0,
        "native_GUI": "未実行",
    }
    target = args.output / "criteria.json"
    target.write_text(
        json.dumps(fixed, ensure_ascii=False, indent=2) + "\n", encoding="utf-8"
    )
    print(
        json.dumps(
            {"cases": len(rows), "criteria_sha256": custom.sha(target.read_bytes())}
        )
    )


if __name__ == "__main__":
    main()
