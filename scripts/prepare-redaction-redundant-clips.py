"""Freeze redundant rectangular clips and refusal boundaries before implementation."""

import argparse
import copy
import importlib.util
import json
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
    folder = root / "fixtures/redaction-copy/partial-text"
    fixed = json.loads((folder / "criteria.json").read_text(encoding="utf-8"))
    baseline = next(row for row in fixed["cases"] if row["file"] == "visible.pdf")
    assert custom.sha((folder / baseline["file"]).read_bytes()) == baseline["sha256"]
    modes = [
        "page",
        "oversize",
        "negative-width",
        "scaled",
        "reflected",
        "repeated",
        "evenodd",
        "rotation-90",
        "rotation-180",
        "rotation-270",
        "form",
        "custom-CMap",
        "shared-page",
        "narrow-refused",
        "compound-refused",
        "curve-refused",
        "paint-refused",
        "state-refused",
        "missing-n-refused",
        "oblique-refused",
        "zero-refused",
    ]
    rows = []
    for name in modes:
        source = folder / baseline["file"]
        row = dict(baseline)
        source_encoding = None
        if name == "custom-CMap":
            other = root / "fixtures/redaction-copy/custom-cmap-v1"
            raw = (other / "criteria.json").read_bytes()
            assert (
                custom.sha(raw)
                == "044024c5bf7ec5a52802e6ba0d0265237aefee83471147207c61a882f7d4a36f"
            )
            row = json.loads(raw)["cases"][0]
            source = other / row["file"]
            source_encoding = row["source_encoding"]
        assert custom.sha(source.read_bytes()) == row["sha256"]
        writer = PdfWriter(clone_from=PdfReader(source))
        page = writer.pages[0]
        commands = page.get_contents().get_data()
        rectangle = b"0 0 612 792 re\n"
        before, after, suffix = b"q\n", b"W n\n", b"Q\n"
        if name == "oversize":
            rectangle = b"-7 -9 626 810 re\n"
        elif name == "negative-width":
            rectangle = b"612 792 -612 -792 re\n"
        elif name == "scaled":
            before += b"2 0 0 2 0 0 cm\n"
            rectangle = b"0 0 306 396 re\n"
            after += b".5 0 0 .5 0 0 cm\n"
        elif name == "reflected":
            before += b"-1 0 0 1 612 0 cm\n"
            after += b"-1 0 0 1 612 0 cm\n"
        elif name == "repeated":
            before += b"0 0 612 792 re W n\n"
        elif name == "evenodd":
            after = b"W* n\n"
        elif name.startswith("rotation-"):
            page[NameObject("/Rotate")] = NumberObject(int(name.split("-")[1]))
            page[NameObject("/CropBox")] = ArrayObject(
                [
                    NumberObject(12),
                    NumberObject(18),
                    NumberObject(600),
                    NumberObject(780),
                ]
            )
            page[NameObject("/UserUnit")] = FloatObject(1.5)
            rectangle = b"12 18 588 762 re\n"
        elif name == "narrow-refused":
            rectangle = b"0 0 300 792 re\n"
        elif name == "compound-refused":
            rectangle += b"100 100 20 20 re\n"
        elif name == "curve-refused":
            rectangle = b"0 0 m 612 0 l 612 792 l 0 792 l h\n"
        elif name == "paint-refused":
            after = b"W f\n"
        elif name == "state-refused":
            after = b"1 0 0 1 0 0 cm W n\n"
        elif name == "missing-n-refused":
            after = b"W\n"
        elif name == "oblique-refused":
            before += b"1 .1 0 1 0 0 cm\n"
            rectangle = b"-1000 -1000 4000 4000 re\n"
        elif name == "zero-refused":
            rectangle = b"0 0 0 792 re\n"
        clipped = before + rectangle + after + commands + suffix
        if name == "form":
            bounds = ArrayObject(
                [NumberObject(0), NumberObject(0), NumberObject(612), NumberObject(792)]
            )
            form = stream(
                clipped,
                Type=NameObject("/XObject"),
                Subtype=NameObject("/Form"),
                BBox=bounds,
                Resources=page["/Resources"],
            )
            page[NameObject("/Resources")] = DictionaryObject(
                {
                    NameObject("/XObject"): DictionaryObject(
                        {NameObject("/Clipped"): writer._add_object(form)}
                    )
                }
            )
            clipped = b"/Clipped Do\n"
        elif name == "shared-page":
            second = writer.add_blank_page(width=612, height=792)
            second[NameObject("/Resources")] = copy.copy(page["/Resources"])
            # This second clip narrows the page and must remain on this unselected page.
            second[NameObject("/Contents")] = writer._add_object(
                stream(b"q 0 0 612 700 re W n\n" + commands + b"Q\n")
            )
        page[NameObject("/Contents")] = writer._add_object(stream(clipped))
        path = args.output / (name + ".pdf")
        writer.write(path)
        refused = name.endswith("refused")
        case = dict(
            file=path.name,
            sha256=custom.sha(path.read_bytes()),
            refused=refused,
            remove=baseline["remove"],
            keep=baseline["keep"],
            rect=baseline["rect"],
            max_position_error_pt=0.5,
            removed_clip_commands=rectangle.decode(),
            second_page_terms=[],
        )
        if source_encoding:
            case["source_encoding"] = source_encoding
        if not refused:
            boxes = custom.positions(path, 0, [case["remove"]] + case["keep"])
            target = boxes.pop(case["remove"])
            x0, y0 = min(b[0] for b in target) - 1, min(b[1] for b in target) - 1
            x1, y1 = max(b[2] for b in target) + 1, max(b[3] for b in target) + 1
            case["rect"] = [x0, y0, x1 - x0, y1 - y0]
            case["boxes"] = {"0": boxes}
            if name == "shared-page":
                case["second_page_terms"] = [case["remove"]] + case["keep"]
                case["boxes"]["1"] = custom.positions(
                    path, 1, case["second_page_terms"]
                )
        rows.append(case)
    criteria = {
        "version": 1,
        "cases": rows,
        "outside_pixel_difference": 0,
        "native_GUI": "未実行",
    }
    path = args.output / "criteria.json"
    path.write_text(
        json.dumps(criteria, ensure_ascii=False, indent=2) + "\n", encoding="utf-8"
    )
    print(
        json.dumps(
            {"cases": len(rows), "criteria_sha256": custom.sha(path.read_bytes())}
        )
    )


if __name__ == "__main__":
    main()
