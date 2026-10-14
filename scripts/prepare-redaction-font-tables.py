"""Freeze private font header payloads before changing font reconstruction."""

import argparse
import hashlib
import io
import json
import struct
from pathlib import Path

from fontTools.ttLib import TTFont
from pypdf import PdfReader, PdfWriter
from pypdf.generic import DecodedStreamObject, NameObject, NumberObject


def checksum(data):
    data += b"\0" * (-len(data) % 4)
    return sum(struct.unpack(f">{len(data) // 4}I", data)) & 0xFFFFFFFF


def encode(tables):
    tags = sorted(tables)
    count = len(tags)
    power = 1 << (count.bit_length() - 1)
    output = bytearray(
        struct.pack(
            ">IHHHH",
            0x10000,
            count,
            power * 16,
            power.bit_length() - 1,
            count * 16 - power * 16,
        )
    )
    output.extend(b"\0" * (16 * count))
    head = 0
    for index, tag in enumerate(tags):
        data = tables[tag]
        if tag == b"head":
            data = data[:8] + b"\0" * 4 + data[12:]
            head = len(output)
        struct.pack_into(
            ">4sIII",
            output,
            12 + 16 * index,
            tag,
            checksum(data),
            len(output),
            len(data),
        )
        output.extend(data)
        output.extend(b"\0" * (-len(output) % 4))
    struct.pack_into(
        ">I", output, head + 8, (0xB1B0AFBA - checksum(bytes(output))) & 0xFFFFFFFF
    )
    return bytes(output)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=False)
    root = Path(__file__).resolve().parents[1]
    base = root / "fixtures/redaction-copy/partial-text"
    original = next(
        row
        for row in json.loads((base / "criteria.json").read_text(encoding="utf-8"))[
            "cases"
        ]
        if row["file"] == "visible.pdf"
    )
    assert (
        hashlib.sha256((base / "visible.pdf").read_bytes()).hexdigest()
        == original["sha256"]
    )
    rows = []
    for mode in (
        "reserved",
        "revision",
        "hint-summary",
        "all-metadata",
        "bad-glyph-box",
    ):
        writer = PdfWriter(clone_from=PdfReader(base / "visible.pdf"))
        font = next(
            ref.get_object()
            for ref in writer.pages[0]["/Resources"]["/Font"].values()
            if "/ToUnicode" in ref.get_object()
        )
        descriptor = font["/FontDescriptor"]
        data = descriptor["/FontFile2"].get_data()
        count = struct.unpack_from(">H", data, 4)[0]
        tables = {}
        for index in range(count):
            tag, _, offset, length = struct.unpack_from(">4sIII", data, 12 + index * 16)
            tables[tag] = data[offset : offset + length]
        payloads = []
        if mode in ("reserved", "all-metadata"):
            value = bytearray(tables[b"hhea"])
            value[24:32] = b"META_9q!"
            tables[b"hhea"] = bytes(value)
            payloads.append("META_9q!")
        if mode in ("revision", "all-metadata"):
            value = bytearray(tables[b"head"])
            value[4:8] = b"REV!"
            tables[b"head"] = bytes(value)
            payloads.append("REV!")
        if mode in ("hint-summary", "all-metadata"):
            value = bytearray(tables[b"maxp"])
            value[18:26] = b"HNTLEAK!"
            tables[b"maxp"] = bytes(value)
            payloads.append("HNTLEAK!")
        if mode == "bad-glyph-box":
            program = TTFont(io.BytesIO(data))
            glyph_name = program["cmap"].tables[0].cmap[1]
            gid = program.getGlyphID(glyph_name)
            offset = program["loca"].locations[gid]
            value = bytearray(tables[b"glyf"])
            assert struct.unpack_from(">h", value, offset)[0] > 0
            xmin = struct.unpack_from(">h", value, offset + 2)[0]
            struct.pack_into(">h", value, offset + 2, xmin + 1)
            tables[b"glyf"] = bytes(value)
        encoded = encode(tables)
        stream = DecodedStreamObject()
        stream.set_data(encoded)
        stream[NameObject("/Length1")] = NumberObject(len(encoded))
        descriptor[NameObject("/FontFile2")] = writer._add_object(stream.flate_encode())
        path = args.output / (mode + ".pdf")
        writer.write(path)
        row = dict(original)
        row.update(
            file=path.name,
            sha256=hashlib.sha256(path.read_bytes()).hexdigest(),
            private_payloads=payloads,
            refused=mode == "bad-glyph-box",
        )
        rows.append(row)
    (args.output / "criteria.json").write_text(
        json.dumps(
            dict(
                cases=rows,
                required="Private reserved/revision/hint-summary data absent; same fixed text, outline, 0.5pt and outside 300dpi pixels. Malformed simple glyph bbox refused atomically.",
            ),
            ensure_ascii=False,
            indent=2,
        )
        + "\n",
        encoding="utf-8",
    )
    (args.output / "plan.json").write_text(
        json.dumps([dict(page=0, rect=original["rect"])]) + "\n", encoding="utf-8"
    )
    print(hashlib.sha256((args.output / "criteria.json").read_bytes()).hexdigest())


if __name__ == "__main__":
    main()
