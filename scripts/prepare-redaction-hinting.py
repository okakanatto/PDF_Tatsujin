"""Freeze hinted fonts, shared usage and hostile instructions before implementation."""

import argparse
import hashlib
import io
import json
from pathlib import Path

from fontTools.ttLib import TTFont
from fontTools.ttLib.tables.ttProgram import Program
from pypdf import PdfReader, PdfWriter
from pypdf.generic import DecodedStreamObject, NameObject, NumberObject

PREP = bytes.fromhex("b801ff85b0048d")
MARKER = b"REMOVED_HINT_PRIVATE_38"


def hint(points, axes=(0, 1)):
    result = bytearray()
    for axis in axes:
        result.append(axis)
        for point in sorted({0, points - 1}):
            result.extend(
                bytes((0xB0, point))
                if point <= 255
                else bytes((0xB8, point >> 8, point & 255))
            )
            result.append(0x2F)
        result.append(0x30 + axis)
    return bytes(result)


def program(data):
    value = Program()
    value.fromBytecode(data)
    return value


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--global-only", action="store_true")
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=False)
    root = Path(__file__).resolve().parents[1]
    sources = root / "fixtures/redaction-copy"
    partial = json.loads((sources / "partial-text/criteria.json").read_text("utf-8"))
    single = next(row for row in partial["cases"] if row["file"] == "visible.pdf")
    shared = json.loads((sources / "shared-fonts-v1/criteria.json").read_text("utf-8"))
    inputs = [("single", "partial-text", single)]
    inputs += [
        ("shared-" + Path(row["file"]).stem, "shared-fonts-v1", row)
        for row in shared["cases"]
    ]
    inputs += [
        (mode, "partial-text", single)
        for mode in (
            "y-only",
            "removed-private",
            "unknown-prep",
            "font-function",
            "control-values",
            "gasp",
            "bad-point",
            "phantom-point",
            "leftover-stack",
            "duplicate-point",
            "reversed-axes",
            "unknown-opcode",
            "truncated-push",
        )
    ]
    if args.global_only:
        inputs = [
            (mode, "partial-text", single)
            for mode in ("unknown-prep", "font-function", "control-values", "gasp")
        ]
    rows = []
    rejected = {
        "unknown-prep",
        "font-function",
        "control-values",
        "gasp",
        "bad-point",
        "phantom-point",
        "leftover-stack",
        "duplicate-point",
        "reversed-axes",
        "unknown-opcode",
        "truncated-push",
    }
    for mode, folder, original in inputs:
        source = sources / folder / original["file"]
        assert hashlib.sha256(source.read_bytes()).hexdigest() == original["sha256"]
        writer = PdfWriter(clone_from=PdfReader(source))
        count = 0
        for value in writer._objects:
            if not isinstance(value, dict) or "/FontFile2" not in value:
                continue
            data = value["/FontFile2"].get_data()
            font = TTFont(io.BytesIO(data), recalcTimestamp=False)
            assert font.getTableData("prep") == PREP
            nonempty = []
            for name in font.getGlyphOrder():
                glyph = font["glyf"][name]
                if glyph.numberOfContours > 0:
                    points = len(glyph.coordinates)
                    if not args.global_only:
                        glyph.program = program(
                            hint(points, (0,) if mode == "y-only" else (0, 1))
                        )
                    nonempty.append((name, points))
            cmap = font["cmap"].tables[0].cmap
            # Source PDF code maps to the source font's cmap; choose a surviving
            # Japanese outline by its PDF ToUnicode correspondence.
            import re

            parent = next(
                obj
                for obj in writer._objects
                if isinstance(obj, dict)
                and obj.get("/FontDescriptor") is not None
                and obj["/FontDescriptor"] is value
            )
            unicode_data = parent["/ToUnicode"].get_data()
            mapping = {
                bytes.fromhex(cp.decode()).decode("utf-16-be"): int(code, 16)
                for body in re.findall(
                    rb"beginbfchar(.*?)endbfchar", unicode_data, re.S
                )
                for code, cp in re.findall(
                    rb"<([0-9a-fA-F]+)>\s*<([0-9a-fA-F]+)>", body
                )
            }
            kept = next(
                cmap[mapping[ch]]
                for term in original["keep"]
                for ch in term
                if ch in mapping and cmap[mapping[ch]] in dict(nonempty)
            )
            points = dict(nonempty)[kept]
            if mode == "removed-private":
                removed = cmap[mapping[original["remove"][0]]]
                font["glyf"][removed].program = program(
                    bytes((0x40, len(MARKER))) + MARKER
                )
            malformed = {
                "bad-point": bytes.fromhex("00b87fff2f30"),
                "phantom-point": bytes(
                    (0, 0xB8, points >> 8, points & 255, 0x2F, 0x30)
                ),
                "leftover-stack": hint(points) + bytes.fromhex("b042"),
                "duplicate-point": bytes.fromhex("00b0002fb0002f30"),
                "reversed-axes": hint(points, (1, 0)),
                "unknown-opcode": bytes.fromhex("00b0002f3021"),
                "truncated-push": bytes.fromhex("00b8"),
            }
            if mode in malformed:
                font["glyf"][kept].program = program(malformed[mode])
            from fontTools.ttLib import newTable

            if mode == "unknown-prep":
                font["prep"].program = program(PREP + bytes.fromhex("b00021"))
            elif mode == "font-function":
                font["fpgm"] = newTable("fpgm")
                font["fpgm"].program = program(bytes.fromhex("b0002c2d"))
            elif mode == "control-values":
                import array

                font["cvt "] = newTable("cvt ")
                font["cvt "].values = array.array("h", [38])
            elif mode == "gasp":
                font["gasp"] = newTable("gasp")
                font["gasp"].version = 1
                font["gasp"].gaspRange = {65535: 15}
            font["maxp"].maxStackElements = max(
                1, len(MARKER) if mode == "removed-private" else 1
            )
            font["maxp"].maxSizeOfInstructions = max(
                len(font["glyf"][name].program.getBytecode()) for name, _ in nonempty
            )
            encoded = io.BytesIO()
            font.save(encoded)
            payload = encoded.getvalue()
            stream = DecodedStreamObject()
            stream.set_data(payload)
            stream[NameObject("/Length1")] = NumberObject(len(payload))
            value[NameObject("/FontFile2")] = writer._add_object(stream.flate_encode())
            count += 1
        assert count
        path = args.output / (mode + ".pdf")
        writer.write(path)
        row = dict(original)
        row.update(
            file=path.name,
            sha256=hashlib.sha256(path.read_bytes()).hexdigest(),
            refused=original.get("refused", False) or mode in rejected,
            source_file=str(source.relative_to(root)).replace("\\", "/"),
            source_sha256=original["sha256"],
            private_marker=MARKER.decode() if mode == "removed-private" else None,
        )
        rows.append(row)
    criteria = dict(
        version=1,
        license="CC0 synthetic source derivatives",
        cases=rows,
        position_error_pt=0.5,
        outside_pixel_difference=0,
        dpi=[72, 96, 300],
        rectangle_boundary_px=1,
    )
    (args.output / "criteria.json").write_text(
        json.dumps(criteria, ensure_ascii=False, indent=2) + "\n", encoding="utf-8"
    )
    print(hashlib.sha256((args.output / "criteria.json").read_bytes()).hexdigest())


if __name__ == "__main__":
    main()
