"""Freeze solid vector path removal and outside preservation criteria."""

import argparse
import hashlib
import json
from pathlib import Path

from pypdf import PdfReader, PdfWriter
from pypdf.generic import ArrayObject, DictionaryObject, NameObject, NumberObject
from prepare_redaction_helpers import stream


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=False)
    root = Path(__file__).resolve().parents[1]
    folder = root / "fixtures/redaction-copy/partial-text"
    base = folder / "visible.pdf"
    row = next(
        r
        for r in json.loads((folder / "criteria.json").read_text(encoding="utf-8"))[
            "cases"
        ]
        if r["file"] == base.name
    )
    assert hashlib.sha256(base.read_bytes()).hexdigest() == row["sha256"]
    x, y, w, h = row["rect"]

    def n(value):
        return f"{value:.10f}".encode()

    a, b, c, d = x + 4, y + 4, x + w - 4, y + h - 4
    rectangle = b" ".join(map(n, (a, b, c - a, d - b))) + b" re\n"
    line = n(a) + b" " + n(b) + b" m " + n(c) + b" " + n(d) + b" l\n"
    bezier = (
        n(a)
        + b" "
        + n(b)
        + b" m "
        + b" ".join(map(n, (a + 10, d, c - 10, b, c, d)))
        + b" c\n"
    )
    outside = b"q 0 0 1 rg 60 480 90 35 re f 1 0 0 RG 3 w 1 J 60 460 m 160 460 l S Q\n"
    drawings = {
        "fill": rectangle + b"f\n",
        "stroke-round": b"2 w 1 J 1 j " + line + b"S\n",
        "stroke-square": b"2 w 2 J 2 j " + line + b"S\n",
        "bezier": b"2 w " + bezier + b"S\n",
        "compound": rectangle + line + b"h B*\n",
        "form": rectangle + b"f\n",
        "crossing-refused": b" ".join(map(n, (a, b, w + 20, d - b))) + b" re f\n",
        "hairline-refused": b"0 w " + line + b"S\n",
        "clip-refused": rectangle + b"W n\n",
        "unpainted-refused": rectangle + b"n\n",
    }
    cases = []
    for name, drawing in drawings.items():
        writer = PdfWriter(clone_from=PdfReader(base))
        page = writer.pages[0]
        content = (
            page.get_contents().get_data()
            + outside
            + b"q 0.25 0.4 0.6 rg 0.2 0.1 0.3 RG "
            + drawing
            + b"Q\n"
        )
        if name == "form":
            form = writer._add_object(
                stream(
                    content,
                    Type=NameObject("/XObject"),
                    Subtype=NameObject("/Form"),
                    BBox=ArrayObject(list(map(NumberObject, (0, 0, 612, 792)))),
                    Resources=page["/Resources"],
                )
            )
            page[NameObject("/Resources")] = DictionaryObject(
                {
                    NameObject("/XObject"): DictionaryObject(
                        {NameObject("/Vector"): form}
                    )
                }
            )
            content = b"/Vector Do\n"
        page[NameObject("/Contents")] = writer._add_object(stream(content))
        path = args.output / (name + ".pdf")
        writer.write(path)
        case = dict(row)
        case.update(
            file=path.name,
            sha256=hashlib.sha256(path.read_bytes()).hexdigest(),
            refused=name.endswith("-refused"),
            removed_path_commands=(
                rectangle.decode()
                if name not in ("stroke-round", "stroke-square", "bezier")
                else (bezier if name == "bezier" else line).decode()
            ),
            outside_changed_pixels=0,
        )
        cases.append(case)
    destination = args.output / "criteria.json"
    destination.write_text(
        json.dumps(
            {"version": 1, "cases": cases, "native_GUI": "未実行"},
            ensure_ascii=False,
            indent=2,
        )
        + "\n",
        encoding="utf-8",
    )
    print(
        json.dumps(
            {
                "cases": len(cases),
                "criteria_sha256": hashlib.sha256(destination.read_bytes()).hexdigest(),
            }
        )
    )


if __name__ == "__main__":
    main()
