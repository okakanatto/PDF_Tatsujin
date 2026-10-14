"""Freeze a no-contour space glyph carrying an unused private bbox."""

import argparse
import hashlib
import importlib.util
import io
import json
import struct
from pathlib import Path

from fontTools.ttLib import TTFont
from pypdf import PdfReader, PdfWriter
from pypdf.generic import DecodedStreamObject, NameObject, NumberObject


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=False)
    root = Path(__file__).resolve().parents[1]
    base = root / "fixtures/redaction-copy/partial-text"
    row = next(
        case
        for case in json.loads((base / "criteria.json").read_text(encoding="utf-8"))[
            "cases"
        ]
        if case["file"] == "visible.pdf"
    )
    assert (
        hashlib.sha256((base / row["file"]).read_bytes()).hexdigest() == row["sha256"]
    )
    spec = importlib.util.spec_from_file_location(
        "sfnt_builder", Path(__file__).with_name("prepare-redaction-font-tables.py")
    )
    builder = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(builder)
    writer = PdfWriter(clone_from=PdfReader(base / row["file"]))
    font = next(
        ref.get_object()
        for ref in writer.pages[0]["/Resources"]["/Font"].values()
        if "/ToUnicode" in ref.get_object()
    )
    descriptor = font["/FontDescriptor"]
    source = descriptor["/FontFile2"].get_data()
    program = TTFont(io.BytesIO(source))
    name = program["cmap"].tables[0].cmap[32]
    gid = program.getGlyphID(name)
    assert program["glyf"][name].numberOfContours == 0
    tables = {}
    for index in range(struct.unpack_from(">H", source, 4)[0]):
        tag, _, offset, length = struct.unpack_from(">4sIII", source, 12 + 16 * index)
        tables[tag] = source[offset : offset + length]
    long = struct.unpack_from(">H", tables[b"head"], 50)[0]
    total = struct.unpack_from(">H", tables[b"maxp"], 4)[0]
    locations = [
        struct.unpack_from(
            ">I" if long else ">H", tables[b"loca"], index * (4 if long else 2)
        )[0]
        * (1 if long else 2)
        for index in range(total + 1)
    ]
    first, last = locations[gid : gid + 2]
    assert first == last
    empty = b"\0\0EMPTYBOX\0\0"
    assert len(empty) == 12
    tables[b"glyf"] = tables[b"glyf"][:first] + empty + tables[b"glyf"][last:]
    locations = [
        offset + (12 if index > gid else 0) for index, offset in enumerate(locations)
    ]
    tables[b"loca"] = b"".join(
        struct.pack(">I" if long else ">H", offset if long else offset // 2)
        for offset in locations
    )
    encoded = builder.encode(tables)
    stream = DecodedStreamObject()
    stream.set_data(encoded)
    stream[NameObject("/Length1")] = NumberObject(len(encoded))
    descriptor[NameObject("/FontFile2")] = writer._add_object(stream.flate_encode())
    path = args.output / "empty-space-box.pdf"
    writer.write(path)
    row.update(
        file=path.name,
        sha256=hashlib.sha256(path.read_bytes()).hexdigest(),
        private_payloads=["EMPTYBOX"],
        refused=False,
    )
    (args.output / "criteria.json").write_text(
        json.dumps(
            dict(
                cases=[row],
                required="Empty outline has no private bbox in the rebuilt font; preserve exact space advance, retained text/outline/0.5pt and outside 300dpi pixels.",
            ),
            ensure_ascii=False,
            indent=2,
        )
        + "\n",
        encoding="utf-8",
    )
    (args.output / "plan.json").write_text(
        json.dumps([dict(page=0, rect=row["rect"])]) + "\n", encoding="utf-8"
    )
    print(hashlib.sha256((args.output / "criteria.json").read_bytes()).hexdigest())


if __name__ == "__main__":
    main()
