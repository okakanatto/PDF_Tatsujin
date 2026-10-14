"""Freeze surviving composite glyphs, including a required .notdef component."""

import argparse
import copy
import hashlib
import io
import json
from pathlib import Path
from fontTools.ttLib import TTFont
from fontTools.ttLib.tables._g_l_y_f import Glyph, GlyphComponent
from pypdf import PdfReader, PdfWriter
from pypdf.generic import DecodedStreamObject, NameObject, NumberObject


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=False)
    root = Path(__file__).resolve().parents[1]
    frozen = root / "fixtures/redaction-copy/partial-text"
    row = json.loads((frozen / "criteria.json").read_text(encoding="utf-8"))["cases"][1]
    rows = []
    for notdef in [False, True]:
        writer = PdfWriter(clone_from=PdfReader(frozen / "visible.pdf"))
        font = next(
            ref.get_object()
            for ref in writer.pages[0]["/Resources"]["/Font"].values()
            if "/FontDescriptor" in ref.get_object()
        )
        descriptor = font["/FontDescriptor"]
        program = TTFont(io.BytesIO(descriptor["/FontFile2"].get_data()))
        order = list(program.getGlyphOrder())
        # The first non-ASCII code of this fixed ReportLab source is 公 (code 1).
        name = program["cmap"].tables[0].cmap[1]
        original = copy.deepcopy(program["glyf"][name])
        if notdef:
            component_name = order[0]
            program["glyf"][component_name] = original
            program["hmtx"][component_name] = program["hmtx"][name]
        else:
            component_name = "publicComponent"
            program["glyf"][component_name] = original
            program["hmtx"][component_name] = program["hmtx"][name]
            order.append(component_name)
            program.setGlyphOrder(order)
            program["glyf"].glyphOrder = order
        compound = Glyph()
        compound.numberOfContours = -1
        component = GlyphComponent()
        component.glyphName = component_name
        component.x = component.y = 0
        component.flags = 4
        compound.components = [component]
        program["glyf"][name] = compound
        encoded = io.BytesIO()
        program.save(encoded)
        stream = DecodedStreamObject()
        stream.set_data(encoded.getvalue())
        stream[NameObject("/Length1")] = NumberObject(len(encoded.getvalue()))
        descriptor[NameObject("/FontFile2")] = writer._add_object(stream.flate_encode())
        path = args.output / ("required-notdef.pdf" if notdef else "compound.pdf")
        writer.write(path)
        entry = dict(row)
        entry["file"] = path.name
        entry["sha256"] = hashlib.sha256(path.read_bytes()).hexdigest()
        entry["required_notdef"] = notdef
        rows.append(entry)
    data = (
        json.dumps(
            {
                "cases": rows,
                "required": "Remove the same frozen target and preserve the exact surviving glyph outlines; a required component at GID 0 must not be replaced with an empty glyph.",
            },
            ensure_ascii=False,
            indent=2,
        )
        + "\n"
    )
    (args.output / "criteria.json").write_text(data, encoding="utf-8")
    print(hashlib.sha256((args.output / "criteria.json").read_bytes()).hexdigest())


if __name__ == "__main__":
    main()
