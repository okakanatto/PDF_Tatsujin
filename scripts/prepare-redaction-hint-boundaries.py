"""Freeze successful PUSHW point boundaries and exact instruction-size limits."""

import argparse
import array
import hashlib
import io
import json
import re
from pathlib import Path

from fontTools.ttLib import TTFont
from fontTools.ttLib.tables._g_l_y_f import Glyph, GlyphCoordinates
from fontTools.ttLib.tables.ttProgram import Program
from pypdf import PdfReader, PdfWriter
from pypdf.generic import DecodedStreamObject, NameObject, NumberObject

PREP = bytes.fromhex("b801ff85b0048d")
MARKER = b"REMOVED_BOUNDARY_PRIVATE_39"
MAX_GENERATED_BYTES = 15_000_000


def sha(payload):
    return hashlib.sha256(payload).hexdigest()


def program(payload):
    result = Program()
    result.fromBytecode(payload)
    return result


def push(point):
    assert 0 <= point <= 32767
    return (
        bytes((0xB0, point)) if point <= 255 else bytes((0xB8, point >> 8, point & 255))
    )


def axis_program(points, axes=(0, 1)):
    result = bytearray()
    for axis in axes:
        result.append(axis)
        for point in points:
            result.extend(push(point))
            result.append(0x2F)
        result.append(0x30 + axis)
    return bytes(result)


def expand_points(glyph, count):
    original = len(glyph.coordinates)
    assert glyph.numberOfContours > 0 and original <= count <= 65535
    extra = count - original
    before = list(glyph.coordinates)
    glyph.coordinates = GlyphCoordinates([before[0]] * extra + before)
    glyph.flags = array.array("B", [glyph.flags[0]] * extra + list(glyph.flags))
    glyph.endPtsOfContours = [point + extra for point in glyph.endPtsOfContours]
    assert list(glyph.coordinates)[extra:] == before
    return original


def profile_program(profile):
    if profile == "max-4096":
        result = axis_program([256], (0,)) + axis_program(range(256, 1278), (1,))
        assert len(result) == 4096
        return 1278, result, 1277, None
    if profile == "max-4097":
        result = axis_program(range(3), (0,)) + axis_program(range(256, 1277), (1,))
        assert len(result) == 4097
        return 1277, result, 1276, "instruction_bytes_above_4096"
    if profile == "word-32768-signed":
        return 32769, bytes.fromhex("00b880002f30"), 32768, "signed_negative_word"
    if profile == "word-65535-signed":
        return 65535, bytes.fromhex("00b8ffff2f30"), 65535, "signed_negative_word"
    if profile == "phantom-257":
        return 257, bytes.fromhex("00b801012f30"), 257, "first_phantom_point"
    if profile == "noncanonical-255":
        return 256, bytes.fromhex("00b800ff2f30"), 255, "word_for_byte_point"
    if profile == "truncated-word":
        return 257, bytes.fromhex("00b801"), 256, "truncated_word_operand"
    if profile == "empty-outline-instructions":
        return 0, bytes.fromhex("b0002f"), 0, "instructions_on_empty_outline"
    if profile == "empty-program":
        return 257, b"", None, None
    point = 255 if profile == "byte-255" else 32767 if profile == "word-32767" else 256
    return point + 1, axis_program([0, point]), point, None


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--verify-existing", action="store_true")
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    destination = args.output.resolve()
    destination.relative_to(root)
    if destination.exists() and not args.verify_existing:
        raise FileExistsError(destination)
    sources = root / "fixtures/redaction-copy"
    partial = json.loads((sources / "partial-text/criteria.json").read_text("utf-8"))
    single = next(row for row in partial["cases"] if row["file"] == "visible.pdf")
    shared = json.loads((sources / "shared-fonts-v1/criteria.json").read_text("utf-8"))
    profiles = [
        "byte-255",
        "word-256",
        "word-32767",
        "empty-program",
        "max-4096",
        "removed-private-word-256",
        "word-32768-signed",
        "word-65535-signed",
        "phantom-257",
        "noncanonical-255",
        "truncated-word",
        "max-4097",
        "empty-outline-instructions",
    ]
    inputs = [(profile, "partial-text", single) for profile in profiles]
    inputs.extend(
        (
            "shared-" + Path(filename).stem + "-word-256",
            "shared-fonts-v1",
            next(row for row in shared["cases"] if row["file"] == filename),
        )
        for filename in (
            "pages.pdf",
            "alias-font.pdf",
            "sibling-form.pdf",
            "appearance-states.pdf",
        )
    )
    files = []
    rows = []
    for profile, folder, original in inputs:
        source = sources / folder / original["file"]
        source_payload = source.read_bytes()
        assert sha(source_payload) == original["sha256"]
        writer = PdfWriter(clone_from=PdfReader(io.BytesIO(source_payload)))
        count, instructions, point, refusal = profile_program(profile)
        records = []
        for object_no, value in enumerate(list(writer._objects), 1):
            if not isinstance(value, dict) or "/FontFile2" not in value:
                continue
            source_font = value["/FontFile2"].get_data()
            font = TTFont(io.BytesIO(source_font), recalcTimestamp=False)
            assert font.getTableData("prep") == PREP
            parent = next(
                obj
                for obj in writer._objects
                if isinstance(obj, dict)
                and obj.get("/FontDescriptor") is not None
                and obj["/FontDescriptor"] is value
            )
            mapping = {
                bytes.fromhex(cp.decode()).decode("utf-16-be"): int(code, 16)
                for body in re.findall(
                    rb"beginbfchar(.*?)endbfchar", parent["/ToUnicode"].get_data(), re.S
                )
                for code, cp in re.findall(
                    rb"<([0-9a-fA-F]+)>\s*<([0-9a-fA-F]+)>", body
                )
            }
            cmap = font["cmap"].tables[0].cmap
            characters = [
                ch
                for term in original["keep"]
                for ch in term
                if ch in mapping
                and font["glyf"][cmap[mapping[ch]]].numberOfContours > 0
            ]
            assert characters
            character = characters[0]
            kept = cmap[mapping[character]]
            glyph = font["glyf"][kept]
            original_points = len(glyph.coordinates)
            for name in font.getGlyphOrder():
                current = font["glyf"][name]
                if current.numberOfContours > 0:
                    current.program = program(
                        axis_program(sorted({0, len(current.coordinates) - 1}))
                    )
            if count:
                expand_points(glyph, count)
                glyph.program = program(instructions)
            else:
                # fontTools normally drops zero-contour programs. Retain exact
                # raw bytes so this unsupported source actually exercises them.
                font["glyf"][kept] = Glyph(
                    b"\0" * 10 + len(instructions).to_bytes(2, "big") + instructions
                )
                font.recalcBBoxes = False
            if profile == "removed-private-word-256":
                removed = cmap[mapping[original["remove"][0]]]
                font["glyf"][removed].program = program(
                    bytes((0x40, len(MARKER))) + MARKER
                )
            if count:
                font["maxp"].recalc(font)
            font["maxp"].maxStackElements = (
                len(MARKER) if profile == "removed-private-word-256" else 1
            )
            font["maxp"].maxSizeOfInstructions = max(
                len(instructions),
                17,
                len(MARKER) + 2 if profile == "removed-private-word-256" else 0,
            )
            encoded = io.BytesIO()
            font.save(encoded)
            payload = encoded.getvalue()
            stream = DecodedStreamObject()
            stream.set_data(payload)
            stream[NameObject("/Length1")] = NumberObject(len(payload))
            value[NameObject("/FontFile2")] = writer._add_object(stream.flate_encode())
            records.append(
                dict(
                    font_descriptor_object_no=object_no,
                    source_font_sha256=sha(source_font),
                    font_input_sha256=sha(payload),
                    original_gid=font.getGlyphID(kept),
                    fonttools_name=kept,
                    unicode_char=character,
                    original_points=original_points,
                    expanded_points=count,
                    expected_program_hex=instructions.hex(),
                )
            )
        assert records
        encoded_pdf = io.BytesIO()
        writer.write(encoded_pdf)
        payload = encoded_pdf.getvalue()
        filename = profile + ".pdf"
        files.append((filename, payload))
        row = dict(original)
        row.update(
            file=filename,
            sha256=sha(payload),
            refused=refusal is not None,
            source_file=source.relative_to(root).as_posix(),
            source_sha256=original["sha256"],
            private_marker=(
                MARKER.decode() if profile == "removed-private-word-256" else None
            ),
            boundary_profile=profile,
            source_font_glyphs=records,
            program_bytes=len(instructions),
            point_operand=point,
            refusal_reason=refusal,
        )
        rows.append(row)
    criteria = dict(
        version=1,
        license="CC0 synthetic document derivatives; embedded Noto license unchanged",
        cases=rows,
        position_error_pt=0.5,
        outside_pixel_difference=0,
        dpi=[72, 96, 300],
        rectangle_boundary_px=1,
        instruction_bytes_limit=4096,
    )
    payload = (json.dumps(criteria, ensure_ascii=False, indent=2) + "\n").encode(
        "utf-8"
    )
    files.append(("criteria.json", payload))
    total = sum(len(data) for _, data in files)
    if total > MAX_GENERATED_BYTES:
        raise RuntimeError(
            f"Frozen inputs require {total} bytes; budget is {MAX_GENERATED_BYTES}"
        )
    if args.verify_existing:
        assert {path.name for path in destination.iterdir()} == {
            name for name, _ in files
        }
        for name, data in files:
            assert (destination / name).read_bytes() == data, name
    else:
        destination.mkdir(parents=True)
        for name, data in files:
            (destination / name).write_bytes(data)
    print(
        json.dumps(
            dict(
                criteria_sha256=sha(payload),
                files=len(files),
                bytes=total,
                verified=args.verify_existing,
            )
        )
    )


if __name__ == "__main__":
    main()
