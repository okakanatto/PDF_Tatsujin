"""Freeze quarter-turn clipping and exact containment/operator boundaries."""

import argparse
import importlib.util
import json
from pathlib import Path

from pypdf import PdfReader, PdfWriter
from pypdf.generic import NameObject
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
    base = root / "fixtures/redaction-copy/partial-text"
    baseline = next(
        row
        for row in json.loads((base / "criteria.json").read_text(encoding="utf-8"))[
            "cases"
        ]
        if row["file"] == "visible.pdf"
    )
    assert custom.sha((base / baseline["file"]).read_bytes()) == baseline["sha256"]
    rows = []
    for name in (
        "quarter-turn",
        "almost-full-refused",
        "bezier-refused",
        "stroke-refused",
    ):
        writer = PdfWriter(clone_from=PdfReader(base / baseline["file"]))
        page = writer.pages[0]
        rectangle = b"0 0 612 792 re\n"
        before, after = b"q\n", b"W n\n"
        if name == "quarter-turn":
            before += b"0 1 -1 0 612 0 cm\n"
            rectangle = b"0 0 792 612 re\n"
            after += b"0 -1 1 0 0 612 cm\n"
        elif name == "almost-full-refused":
            rectangle = b"0 0 611.999 792 re\n"
        elif name == "bezier-refused":
            rectangle = b"0 0 m 204 0 408 0 612 0 c 612 792 l 0 792 l h\n"
        elif name == "stroke-refused":
            after = b"W S\n"
        page[NameObject("/Contents")] = writer._add_object(
            stream(before + rectangle + after + page.get_contents().get_data() + b"Q\n")
        )
        path = args.output / (name + ".pdf")
        writer.write(path)
        case = dict(
            file=path.name,
            sha256=custom.sha(path.read_bytes()),
            refused=name.endswith("refused"),
            remove=baseline["remove"],
            keep=baseline["keep"],
            rect=baseline["rect"],
            max_position_error_pt=0.5,
            removed_clip_commands=rectangle.decode(),
            second_page_terms=[],
        )
        if not case["refused"]:
            boxes = custom.positions(path, 0, [case["remove"]] + case["keep"])
            target = boxes.pop(case["remove"])
            x0, y0 = min(b[0] for b in target) - 1, min(b[1] for b in target) - 1
            x1, y1 = max(b[2] for b in target) + 1, max(b[3] for b in target) + 1
            case["rect"] = [x0, y0, x1 - x0, y1 - y0]
            case["boxes"] = {"0": boxes}
        rows.append(case)
    path = args.output / "criteria.json"
    path.write_text(
        json.dumps(
            {
                "version": 1,
                "cases": rows,
                "outside_pixel_difference": 0,
                "native_GUI": "未実行",
            },
            ensure_ascii=False,
            indent=2,
        )
        + "\n",
        encoding="utf-8",
    )
    print(
        json.dumps(
            {"cases": len(rows), "criteria_sha256": custom.sha(path.read_bytes())}
        )
    )


if __name__ == "__main__":
    main()
