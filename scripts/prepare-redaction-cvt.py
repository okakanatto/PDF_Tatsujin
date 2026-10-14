"""Freeze bounded MIAP CVT references before product implementation."""

import argparse
import array
import copy
import hashlib
import io
import json
import re
from pathlib import Path

from fontTools.ttLib import TTFont, newTable
from fontTools.ttLib.tables.DefaultTable import DefaultTable
from fontTools.ttLib.tables._g_l_y_f import Glyph, GlyphComponent
from fontTools.ttLib.tables.ttProgram import Program
from pypdf import PdfReader, PdfWriter
from pypdf.generic import DecodedStreamObject, NameObject, NumberObject

PREP = bytes.fromhex("b801ff85b0048d")
MARKERS = (b"UNREFERENCED_CVT_PRIVATE_40", b"REMOVED_CVT_PRIVATE_40")
MAX_GENERATED_BYTES = 2_700_000  # Parent reservation includes scripts/docs as well.
SUCCESS = [
    "sparse-byte",
    "sparse-word",
    "signed-fwords",
    "equal-fwords",
    "y-only",
    "mixed-mdap-miap",
    "removed-private",
    "compound-child",
    "required-notdef-child",
]
REFUSED = [
    "missing-cvt",
    "empty-cvt",
    "odd-cvt",
    "out-of-range",
    "negative-word-32768",
    "negative-word-65535",
    "noncanonical-word",
    "truncated-word",
    "miap-zero",
    "wcvt",
    "rcvt",
    "compound-own",
    "point-phantom",
    "duplicate-point",
    "reversed-axes",
    "unknown-prep",
    "font-function",
    "storage",
    "zone-change",
    "no-retained-cvt",
    "unused-only-cvt",
]


def sha(data):
    return hashlib.sha256(data).hexdigest()


def program(data):
    result = Program()
    result.fromBytecode(data)
    return result


def push(number):
    assert 0 <= number <= 32767
    return (
        bytes((0xB0, number))
        if number <= 255
        else bytes((0xB8, number >> 8, number & 255))
    )


def mdap(points):
    return b"".join(
        bytes((axis,))
        + b"".join(push(p) + b"\x2f" for p in sorted({0, points - 1}))
        + bytes((0x30 + axis,))
        for axis in (0, 1)
    )


def miap(points, indices, axes=(0, 1), mixed=False):
    result = bytearray()
    for axis in axes:
        result.append(axis)
        for offset, point in enumerate(sorted({0, points - 1})):
            result.extend(push(point))
            if mixed and axis == 0 and offset == 1:
                result.append(0x2F)
            else:
                result.extend(push(indices[axis * 2 + offset]))
                result.append(0x3F)
        result.append(0x30 + axis)
    return bytes(result)


def encoded_font(font):
    output = io.BytesIO()
    font.save(output)
    return output.getvalue()


def put_font(writer, descriptor, data):
    stream = DecodedStreamObject()
    stream.set_data(data)
    stream[NameObject("/Length1")] = NumberObject(len(data))
    descriptor[NameObject("/FontFile2")] = writer._add_object(stream.flate_encode())


def get_mapping(writer, descriptor):
    parent = next(
        obj
        for obj in writer._objects
        if isinstance(obj, dict)
        and obj.get("/FontDescriptor") is not None
        and obj["/FontDescriptor"] is descriptor
    )
    return {
        bytes.fromhex(cp.decode()).decode("utf-16-be"): int(code, 16)
        for body in re.findall(
            rb"beginbfchar(.*?)endbfchar", parent["/ToUnicode"].get_data(), re.S
        )
        for code, cp in re.findall(rb"<([0-9a-fA-F]+)>\s*<([0-9a-fA-F]+)>", body)
    }


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
    partial = json.loads((sources / "partial-text/criteria.json").read_bytes())
    single = next(row for row in partial["cases"] if row["file"] == "visible.pdf")
    shared = json.loads((sources / "shared-fonts-v1/criteria.json").read_bytes())
    inputs = [(mode, "partial-text", single) for mode in SUCCESS + REFUSED]
    inputs += [
        (
            "shared-" + Path(filename).stem,
            "shared-fonts-v1",
            next(row for row in shared["cases"] if row["file"] == filename),
        )
        for filename in (
            "pages.pdf",
            "alias-font.pdf",
            "sibling-form.pdf",
            "appearance-states.pdf",
        )
    ]
    # Before generating, reserve conservative clone + replacement-font headroom.
    upper_bound = (
        sum(
            3 * (sources / folder / row["file"]).stat().st_size + 16384
            for _, folder, row in inputs
        )
        + 150_000
    )
    assert upper_bound <= MAX_GENERATED_BYTES, (upper_bound, MAX_GENERATED_BYTES)
    print(
        json.dumps(
            dict(
                expected_upper_bound_bytes=upper_bound,
                reserved_fixture_bytes=MAX_GENERATED_BYTES,
            )
        ),
        flush=True,
    )
    files, rows = [], []
    for mode, folder, original in inputs:
        source = sources / folder / original["file"]
        source_payload = source.read_bytes()
        assert sha(source_payload) == original["sha256"]
        writer = PdfWriter(clone_from=PdfReader(io.BytesIO(source_payload)))
        control = (
            PdfWriter(clone_from=PdfReader(io.BytesIO(source_payload)))
            if mode == "sparse-word"
            else None
        )
        descriptors = [
            (i, obj)
            for i, obj in enumerate(list(writer._objects))
            if isinstance(obj, dict) and "/FontFile2" in obj
        ]
        records = []
        for object_index, descriptor in descriptors:
            source_font = descriptor["/FontFile2"].get_data()
            font = TTFont(io.BytesIO(source_font), recalcTimestamp=False)
            assert font.getTableData("prep") == PREP
            mapping = get_mapping(writer, descriptor)
            cmap = font["cmap"].tables[0].cmap
            choices = (
                original["remove"][0]
                if mode.startswith("shared-")
                else "".join(original["keep"])
            )
            character = next(
                ch
                for ch in choices
                if ch in mapping
                and font["glyf"][cmap[mapping[ch]]].numberOfContours > 0
            )
            kept = cmap[mapping[character]]
            removed = cmap[mapping[original["remove"][0]]]
            for name in font.getGlyphOrder():
                glyph = font["glyf"][name]
                if glyph.numberOfContours > 0:
                    glyph.program = program(mdap(len(glyph.coordinates)))
            hint_name = kept
            if mode in ("compound-child", "required-notdef-child", "compound-own"):
                order = list(font.getGlyphOrder())
                hint_name = (
                    order[0]
                    if mode == "required-notdef-child"
                    else "cvtPublicComponent"
                )
                font["glyf"][hint_name] = copy.deepcopy(font["glyf"][kept])
                font["hmtx"][hint_name] = font["hmtx"][kept]
                if hint_name not in order:
                    order.append(hint_name)
                    font.setGlyphOrder(order)
                    font["glyf"].glyphOrder = order
                compound = Glyph()
                compound.numberOfContours = -1
                component = GlyphComponent()
                component.glyphName = hint_name
                component.x = component.y = 0
                component.flags = 4
                compound.components = [component]
                font["glyf"][kept] = compound
                compound.recalcBounds(font["glyf"])
            glyph = font["glyf"][hint_name]
            points = len(glyph.coordinates)
            indices = [17, 7, 23, 3] if mode == "sparse-byte" else [300, 7, 256, 17]
            values = [1000 + i for i in range(320)]
            for axis in (0, 1):
                for offset, point in enumerate(sorted({0, points - 1})):
                    values[indices[axis * 2 + offset]] = int(
                        glyph.coordinates[point][1 - axis]
                    )
            if mode == "signed-fwords":
                for index, value in zip((7, 17, 256, 300), (-32768, -1, 0, 32767)):
                    values[index] = value
            if mode == "equal-fwords":
                for index in indices:
                    values[index] = 0
            cvt_raw = bytearray(
                b"".join(value.to_bytes(2, "big", signed=True) for value in values)
            )
            private_markers = []
            if mode == "removed-private":
                for start, marker in zip((32, 52), MARKERS):
                    cvt_raw[start * 2 : start * 2 + len(marker)] = marker
                font["glyf"][removed].program = program(
                    miap(len(font["glyf"][removed].coordinates), [52] * 4)
                )
                private_markers = [marker.decode() for marker in MARKERS]
            cvt_table = DefaultTable("cvt ")
            cvt_table.data = bytes(cvt_raw)
            font["cvt "] = cvt_table
            instructions = miap(
                points,
                indices,
                (0,) if mode == "y-only" else (0, 1),
                mode == "mixed-mdap-miap",
            )
            if mode == "missing-cvt":
                del font["cvt "]
            elif mode == "empty-cvt":
                font["cvt "].data = b""
            elif mode == "odd-cvt":
                font["cvt "].data += b"\x7f"
            elif mode == "out-of-range":
                instructions = b"\x00" + push(0) + push(320) + b"\x3f\x30"
            elif mode in ("negative-word-32768", "negative-word-65535"):
                raw = b"\x80\x00" if mode.endswith("32768") else b"\xff\xff"
                instructions = b"\x00\xb0\x00\xb8" + raw + b"\x3f\x30"
            elif mode == "noncanonical-word":
                instructions = bytes.fromhex("00b000b800ff3f30")
            elif mode == "truncated-word":
                instructions = bytes.fromhex("00b000b801")
            elif mode == "miap-zero":
                instructions = instructions.replace(b"\x3f", b"\x3e")
            elif mode in ("wcvt", "rcvt", "storage", "zone-change"):
                instructions += {
                    "wcvt": bytes.fromhex("b000b00144"),
                    "rcvt": bytes.fromhex("b00045"),
                    "storage": bytes.fromhex("b000b00142"),
                    "zone-change": bytes.fromhex("b00013"),
                }[mode]
            elif mode == "point-phantom":
                instructions = b"\x00" + push(points) + push(7) + b"\x3f\x30"
            elif mode == "duplicate-point":
                instructions = b"\x00" + (push(0) + push(7) + b"\x3f") * 2 + b"\x30"
            elif mode == "reversed-axes":
                instructions = miap(points, indices, (1, 0))
            elif mode == "unknown-prep":
                font["prep"].program = program(PREP + bytes.fromhex("b00021"))
            elif mode == "font-function":
                font["fpgm"] = newTable("fpgm")
                font["fpgm"].program = program(bytes.fromhex("b0002c2d"))
            elif mode in ("no-retained-cvt", "unused-only-cvt"):
                instructions = mdap(points)
                if mode == "no-retained-cvt":
                    font["glyf"][removed].program = program(
                        miap(len(font["glyf"][removed].coordinates), indices)
                    )
            glyph.program = program(instructions)
            if mode == "compound-own":
                font["glyf"][kept].program = program(bytes.fromhex("00b000b0073f30"))
            font["maxp"].recalc(font)
            font["maxp"].maxStackElements = 2
            font["maxp"].maxSizeOfInstructions = max(
                len(current.program.getBytecode())
                for current in (font["glyf"][name] for name in font.getGlyphOrder())
                if hasattr(current, "program")
            )
            payload = encoded_font(font)
            put_font(writer, descriptor, payload)
            records.append(
                dict(
                    font_input_sha256=sha(payload),
                    source_font_sha256=sha(source_font),
                    original_gid=font.getGlyphID(kept),
                    hint_gid=font.getGlyphID(hint_name),
                    unicode_char=character,
                    expected_program_hex=instructions.hex(),
                    point_count=points,
                    original_cvt_hex=(
                        font.getTableData("cvt ").hex() if "cvt " in font else None
                    ),
                )
            )
            if control is not None:
                control_font = copy.deepcopy(font)
                control_font["glyf"][hint_name].program = program(
                    bytes.fromhex("00300131")
                )
                put_font(
                    control, control._objects[object_index], encoded_font(control_font)
                )
        assert records
        encoded = io.BytesIO()
        writer.write(encoded)
        payload = encoded.getvalue()
        filename = mode + ".pdf"
        files.append((filename, payload))
        row = dict(original)
        row.update(
            file=filename,
            sha256=sha(payload),
            refused=mode in REFUSED,
            cvt_profile=mode,
            source_file=source.relative_to(root).as_posix(),
            source_sha256=original["sha256"],
            private_markers=private_markers,
            source_font_glyphs=records,
            refusal_reason=mode if mode in REFUSED else None,
        )
        if control is not None:
            encoded = io.BytesIO()
            control.write(encoded)
            control_payload = encoded.getvalue()
            control_filename = mode + "-control.pdf"
            files.append((control_filename, control_payload))
            row.update(
                control_file=control_filename, control_sha256=sha(control_payload)
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
        source_cvt_entry_count=320,
        retain_equal_values_at_distinct_indices=True,
        cvt_requires_retained_miap=True,
        independent_observability_dpi=[72, 96],
    )
    payload = (json.dumps(criteria, ensure_ascii=False, indent=2) + "\n").encode(
        "utf-8"
    )
    files.append(("criteria.json", payload))
    total = sum(len(data) for _, data in files)
    assert total <= MAX_GENERATED_BYTES, (total, MAX_GENERATED_BYTES)
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
                success=sum(not row["refused"] for row in rows),
                refused=sum(row["refused"] for row in rows),
                ui_success_index=next(
                    i for i, row in enumerate(rows) if row["file"] == "shared-pages.pdf"
                ),
                ui_refusal_index=next(
                    i for i, row in enumerate(rows) if row["file"] == "out-of-range.pdf"
                ),
                verified=args.verify_existing,
            )
        )
    )


if __name__ == "__main__":
    main()
