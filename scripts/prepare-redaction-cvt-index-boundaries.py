"""Freeze CVT source/dense index boundaries before any product execution.

The fixture owns exact source operands, local CVT expectations and final
same-input-SFNT witnesses. Existing fixtures and criteria are never changed.
"""

import argparse
import array
import copy
import hashlib
import io
import json
import re
from pathlib import Path

from fontTools.ttLib import TTFont
from fontTools.ttLib.tables.DefaultTable import DefaultTable
from fontTools.ttLib.tables._g_l_y_f import Glyph, GlyphComponent, GlyphCoordinates
from fontTools.ttLib.tables.ttProgram import Program
import pypdfium2 as pdfium
from pypdf import PdfReader, PdfWriter
from pypdf.generic import (
    ContentStream,
    DecodedStreamObject,
    NameObject,
    NumberObject,
    StreamObject,
)

PREP = bytes.fromhex("b801ff85b0048d")
PINS = {
    "partial-text/visible.pdf": "f9bacb0ca4b4b19f5bc100e68bce571dbb474b55665ad6dbfb8097e7c9f2fd39",
    "cvt-references-v1/criteria.json": "1234729946a784d36e2d32bca4fc1e10642b01c3e900539996f236a440858be7",
    "hint-boundaries-v1/criteria.json": "1fcd232da7b7ae1af28430c8fdb7a0e92318c72895637a258f66c874dd3e4941",
    "shared-fonts-v1/criteria.json": "a24478e7395e3bfcc4046eb0266aba324c5858d6c3c9db0e447aba5d9ec0f586",
}
MARKERS = (b"UNREFERENCED_CVT_PRIVATE_41", b"REMOVED_CVT_BLOCK_PRIVATE_41")
MAX_GENERATED_BYTES = 1_900_000  # Parent reserves 2 MB including this script.
PROFILES = [
    "source-index-32767",
    "source-index-negative-32768",
    "dense-index-255",
    "dense-index-256",
    "compound-child-dense-index-256",
    "required-notdef-child-dense-index-256",
    "shared-pages-split",
    "shared-alias-font-split",
    "shared-sibling-form-split",
    "shared-appearance-states-split",
]


def sha(data):
    return hashlib.sha256(data).hexdigest()


def json_bytes(value):
    return (json.dumps(value, ensure_ascii=False, indent=2) + "\n").encode("utf-8")


def program(data):
    result = Program()
    result.fromBytecode(data)
    return result


def push(value):
    assert 0 <= value <= 32767
    return (
        bytes((0xB0, value)) if value <= 255 else bytes((0xB8, value >> 8, value & 255))
    )


def mdap(points):
    return b"".join(
        bytes((axis,))
        + b"".join(push(point) + b"\x2f" for point in sorted({0, points - 1}))
        + bytes((0x30 + axis,))
        for axis in (0, 1)
    )


def one_miap(index):
    return b"\x01" + push(0) + push(index) + b"\x3f\x31"


def dense_program(index_map=None):
    result = bytearray(b"\x01")
    for point in range(256):
        result.extend(push(point))
        result.extend(
            push(512 - point if index_map is None else index_map[512 - point])
        )
        result.append(0x3F)
    result.extend(push(256) + b"\x2f\x31")
    assert len(result) == (1542 if index_map is None else 1286)
    return bytes(result)


def outline_record(font, name):
    glyph = font["glyf"][name]
    coordinates, endpoints, flags = glyph.getCoordinates(font["glyf"])
    return dict(
        coordinates=[list(point) for point in coordinates],
        contour_endpoints=list(endpoints),
        flags=list(flags),
        bbox=[glyph.xMin, glyph.yMin, glyph.xMax, glyph.yMax],
        advance=list(font["hmtx"][name]),
    )


def expand_first_on_curve(glyph):
    original = len(glyph.coordinates)
    assert glyph.numberOfContours > 0 and 0 < original <= 257
    assert glyph.flags[0] & 1, "Only an on-curve first point can be duplicated"
    before, flags = list(glyph.coordinates), list(glyph.flags)
    endpoints = list(glyph.endPtsOfContours)
    extra = 257 - original
    glyph.coordinates = GlyphCoordinates([before[0]] * extra + before)
    glyph.flags = array.array("B", [flags[0]] * extra + flags)
    glyph.endPtsOfContours = [point + extra for point in endpoints]
    assert (
        list(glyph.coordinates)[extra:] == before and list(glyph.flags)[extra:] == flags
    )
    assert [point - extra for point in glyph.endPtsOfContours] == endpoints
    return original


def encode_font(font):
    out = io.BytesIO()
    font.save(out)
    return out.getvalue()


def put_font(writer, descriptor, data):
    stream = DecodedStreamObject()
    stream.set_data(data)
    stream[NameObject("/Length1")] = NumberObject(len(data))
    descriptor[NameObject("/FontFile2")] = writer._add_object(stream.flate_encode())


def mapping_for(writer, descriptor):
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


def write_pdf(writer):
    out = io.BytesIO()
    writer.write(out)
    return out.getvalue()


def actual_positions(payload, row):
    """Pin text geometry from this constructed INPUT, before product execution."""
    result = {}
    with pdfium.PdfDocument(payload) as document:
        for index in range(len(document)):
            page = document[index]
            text = page.get_textpage()
            content = text.get_text_range()
            terms = row["keep"] if index == 0 else row.get("second_page_terms", [])
            positions = {}
            for term in terms:
                assert content.count(term) == 1, (row["file"], index, term, content)
                start = content.index(term)
                positions[term] = [
                    list(text.get_charbox(i)) for i in range(start, start + len(term))
                ]
            result[str(index)] = positions
    return result


def fixture_inventory(reader):
    """Record explicit simple-font bindings and code uses in these fixed inputs."""
    bindings, usage = {}, {}

    def inspect(stream, resources, path):
        local, current, stack, form_index = {}, None, [], 0
        for alias, value in resources.get("/Font", {}).items():
            font = value.get_object()
            if "/FontDescriptor" not in font or "/ToUnicode" not in font:
                continue
            descriptor = font["/FontDescriptor"]
            if "/FontFile2" not in descriptor:
                continue
            data = descriptor["/FontFile2"].get_data()
            sfnt = TTFont(io.BytesIO(data), recalcTimestamp=False)
            cmap = sfnt["cmap"].tables[0].cmap
            mapping = {
                int(code, 16): (
                    bytes.fromhex(cp.decode()).decode("utf-16-be"),
                    sfnt.getGlyphID(cmap[int(code, 16)]),
                )
                for body in re.findall(
                    rb"beginbfchar(.*?)endbfchar", font["/ToUnicode"].get_data(), re.S
                )
                for code, cp in re.findall(
                    rb"<([0-9a-fA-F]+)>\s*<([0-9a-fA-F]+)>", body
                )
                if int(code, 16) in cmap
            }
            key = path + "/font" + str(alias)
            local[str(alias)] = key
            bindings[key] = dict(data=data, sfnt=sfnt, mappings=mapping, used_glyphs=[])
        for args, operator in ContentStream(stream, reader).operations:
            if operator == b"q":
                stack.append(current)
            elif operator == b"Q":
                assert stack
                current = stack.pop()
            elif operator == b"Tf":
                current = local.get(str(args[0]))
            elif operator in (b"Tj", b"TJ", b"'", b'"') and current:
                for value in args[0] if operator == b"TJ" else [args[-1]]:
                    if not hasattr(value, "original_bytes"):
                        continue
                    decoded = [
                        bindings[current]["mappings"][code]
                        for code in value.original_bytes
                    ]
                    bindings[current]["used_glyphs"].extend(decoded)
                    usage[current] = usage.get(current, "") + "".join(
                        char for char, _ in decoded
                    )
            elif operator == b"Do":
                child = resources["/XObject"][args[0]].get_object()
                if child.get("/Subtype") == "/Form":
                    inspect(
                        child,
                        child.get("/Resources", resources),
                        path + "/form/" + str(form_index),
                    )
                    form_index += 1

    def appearance(value, resources, path):
        value = value.get_object()
        if isinstance(value, StreamObject):
            inspect(value, value.get("/Resources", resources), path)
        else:
            for state, child in value.items():
                appearance(child, resources, path + str(state))

    for index, page in enumerate(reader.pages):
        resources = page["/Resources"]
        inspect(page.get_contents(), resources, f"page/{index}")
        for number, annotation in enumerate(page.get("/Annots", [])):
            for kind, value in annotation.get_object().get("/AP", {}).items():
                appearance(value, resources, f"page/{index}/AP/{number}{kind}")
    return bindings, usage


def fixture_closure(font, roots):
    complete = set()

    def visit(gid):
        if gid in complete:
            return
        complete.add(gid)
        glyph = font["glyf"][font.getGlyphOrder()[gid]]
        if glyph.isComposite():
            for component in glyph.components:
                visit(font.getGlyphID(component.glyphName))

    for gid in roots:
        visit(gid)
    return complete


def split_outside_commands(writer, mapping):
    phrase = "公開 秘 文字保持"
    encoded = b"".join(mapping[char].to_bytes(1, "big") for char in phrase)
    replacement = mapping["秘"].to_bytes(1, "big")
    old, new = (
        b"<" + encoded.hex().encode() + b">",
        b"<" + replacement.hex().encode() + b">",
    )
    replaced = 0
    for obj in writer._objects:
        if not isinstance(obj, StreamObject):
            continue
        data = obj.get_data()
        if old not in data:
            continue
        assert data.count(old) == 1
        obj.set_data(data.replace(old, new))
        replaced += 1
    assert replaced == 1, "The original shared outside stream is explicit and unique"


def frozen_closures(payload, row, font_sha, programs, cvt_raw, shared):
    bindings, usage = fixture_inventory(PdfReader(io.BytesIO(payload)))
    closures = []
    union = set()
    for key, text in usage.items():
        record = bindings[key]
        if sha(record["data"]) != font_sha:
            continue
        remaining = (
            text.replace(row["remove"], "") if key.startswith("page/0/") else text
        )
        roots = {gid for char, gid in record["used_glyphs"] if char in remaining}
        closure = fixture_closure(record["sfnt"], roots)
        active = [entry for entry in programs if entry["hint_gid"] in closure]
        refs = sorted(
            {index for entry in active for index in entry["source_reference_indices"]}
        )
        union.update(refs)
        index_map = {index: dense for dense, index in enumerate(refs)}
        expected = []
        for entry in active:
            gid, kind = entry["hint_gid"], entry["role"]
            expected.append(
                dict(
                    hint_gid=gid,
                    program_hex=(
                        dense_program(index_map)
                        if kind == "main-dense"
                        else one_miap(index_map[entry["source_reference_indices"][0]])
                    ).hex(),
                )
            )
        closures.append(
            dict(
                binding_prefixes=[key],
                source_font_sha256=font_sha,
                expected_root_gids=sorted(roots),
                expected_closure_gids=sorted(closure),
                expected_original_indices=refs,
                expected_index_map={
                    str(index): dense for index, dense in index_map.items()
                },
                expected_cvt_hex=b"".join(
                    cvt_raw[index * 2 : index * 2 + 2] for index in refs
                ).hex(),
                expected_programs=expected,
            )
        )
    assert closures and union
    if shared:
        first = [
            item
            for item in closures
            if item["binding_prefixes"][0].startswith("page/0/")
        ]
        outside = [item for item in closures if item not in first]
        assert len(first) == 1 and outside
        assert first[0]["expected_original_indices"] == list(range(257, 513))
        assert all(item["expected_original_indices"] == [513] for item in outside)
        assert all(item["expected_index_map"] == {"513": 0} for item in outside)
    return closures, [dict(source_font_sha256=font_sha, indices=sorted(union))]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--verify-existing", action="store_true")
    parser.add_argument(
        "--finalize-preexecution-positions",
        action="store_true",
        help="Replace only the preserved provisional criterion; all PDFs must be byte identical",
    )
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    destination = args.output.resolve()
    destination.relative_to(root)
    assert not (args.verify_existing and args.finalize_preexecution_positions)
    if destination.exists() and not (
        args.verify_existing or args.finalize_preexecution_positions
    ):
        raise FileExistsError(destination)
    sources = root / "fixtures/redaction-copy"
    for relative, pin in PINS.items():
        assert sha((sources / relative).read_bytes()) == pin, relative
    partial = json.loads((sources / "partial-text/criteria.json").read_bytes())
    single = next(row for row in partial["cases"] if row["file"] == "visible.pdf")
    shared_criteria = json.loads(
        (sources / "shared-fonts-v1/criteria.json").read_bytes()
    )
    files, rows = [], []
    for profile in PROFILES:
        shared = profile.startswith("shared-")
        original = (
            next(
                row
                for row in shared_criteria["cases"]
                if row["file"]
                == profile.removeprefix("shared-").removesuffix("-split") + ".pdf"
            )
            if shared
            else single
        )
        folder = "shared-fonts-v1" if shared else "partial-text"
        source = sources / folder / original["file"]
        source_payload = source.read_bytes()
        assert sha(source_payload) == original["sha256"]
        writer = PdfWriter(clone_from=PdfReader(io.BytesIO(source_payload)))
        control = (
            PdfWriter(clone_from=PdfReader(io.BytesIO(source_payload)))
            if profile in ("source-index-32767", "dense-index-256")
            else None
        )
        row = copy.deepcopy(original)
        if shared:
            row["second_page_terms"] = (
                [] if row.get("appearance") or "appearance" in profile else ["秘"]
            )
            row["boxes"]["1"] = (
                {"秘": row["boxes"]["1"]["秘"]}
                if "秘" in row["boxes"].get("1", {})
                else {}
            )
        records, controls = [], []
        for object_index, descriptor in enumerate(list(writer._objects)):
            if not isinstance(descriptor, dict) or "/FontFile2" not in descriptor:
                continue
            baseline_font = descriptor["/FontFile2"].get_data()
            font = TTFont(io.BytesIO(baseline_font), recalcTimestamp=False)
            assert font.getTableData("prep") == PREP
            mapping = mapping_for(writer, descriptor)
            assert all(char in mapping for char in ("公", "開", "秘", "匿"))
            cmap = font["cmap"].tables[0].cmap
            main_name, companion_name = (
                cmap[mapping["公"]],
                cmap[mapping["秘" if shared else "開"]],
            )
            assert main_name != companion_name
            for name in font.getGlyphOrder():
                glyph = font["glyf"][name]
                if glyph.numberOfContours > 0:
                    glyph.program = program(mdap(len(glyph.coordinates)))
            if shared:
                split_outside_commands(writer, mapping)
            baseline_main = outline_record(font, main_name)
            main_gid = font.getGlyphID(main_name)
            hint_name = main_name
            if profile.startswith("compound-child") or profile.startswith(
                "required-notdef-child"
            ):
                hint_name = (
                    font.getGlyphOrder()[0]
                    if profile.startswith("required-notdef")
                    else "cvtBoundaryPublicComponent"
                )
                order = list(font.getGlyphOrder())
                font["glyf"][hint_name] = copy.deepcopy(font["glyf"][main_name])
                font["hmtx"][hint_name] = font["hmtx"][main_name]
                if hint_name not in order:
                    order.append(hint_name)
                    font.setGlyphOrder(order)
                    font["glyf"].glyphOrder = order
                compound = Glyph()
                compound.numberOfContours = -1
                component = GlyphComponent()
                component.glyphName, component.x, component.y, component.flags = (
                    hint_name,
                    0,
                    0,
                    4,
                )
                compound.components = [component]
                font["glyf"][main_name] = compound
                compound.recalcBounds(font["glyf"])
            glyph = font["glyf"][hint_name]
            dense = profile not in ("source-index-32767", "source-index-negative-32768")
            original_points = (
                expand_first_on_curve(glyph) if dense else len(glyph.coordinates)
            )
            count = (
                32769
                if profile == "source-index-negative-32768"
                else 32768 if profile == "source-index-32767" else 600
            )
            values = [(index % 4000) - 2000 for index in range(count)]
            if dense:
                for point in range(256):
                    values[512 - point] = int(glyph.coordinates[point][0])
                values[513] = int(font["glyf"][companion_name].coordinates[0][0])
                main_instructions, refs, role = (
                    dense_program(),
                    list(range(257, 513)),
                    "main-dense",
                )
            elif profile == "source-index-32767":
                values[32767] = int(glyph.coordinates[0][0])
                main_instructions, refs, role = (
                    one_miap(32767),
                    [32767],
                    "source-single",
                )
            else:
                values[32768] = int(glyph.coordinates[0][0])
                main_instructions, refs, role = (
                    bytes.fromhex("01b000b880003f31"),
                    [],
                    "signed-negative",
                )
            glyph.program = program(main_instructions)
            removed_name = cmap[mapping["匿"]]
            removed_glyph = font["glyf"][removed_name]
            assert removed_glyph.numberOfContours > 0
            values[90] = int(removed_glyph.coordinates[0][0])
            removed_glyph.program = program(one_miap(90))
            cvt_raw = bytearray(
                b"".join(value.to_bytes(2, "big", signed=True) for value in values)
            )
            for start, marker in zip((16, 100), MARKERS):
                cvt_raw[start * 2 : start * 2 + len(marker)] = marker
            cvt_raw = bytes(cvt_raw)
            table = DefaultTable("cvt ")
            table.data = cvt_raw
            font["cvt "] = table
            active = [
                (hint_name, "公", main_gid, role, refs, original_points, baseline_main)
            ]
            if profile != "dense-index-255" and dense:
                companion = font["glyf"][companion_name]
                companion.program = program(one_miap(513))
                active.append(
                    (
                        companion_name,
                        "秘" if shared else "開",
                        font.getGlyphID(companion_name),
                        "companion-single",
                        [513],
                        len(companion.coordinates),
                        outline_record(font, companion_name),
                    )
                )
            font["maxp"].recalc(font)
            font["maxp"].maxStackElements = 2
            font["maxp"].maxSizeOfInstructions = max(
                len(font["glyf"][name].program.getBytecode())
                for name in font.getGlyphOrder()
                if hasattr(font["glyf"][name], "program")
            )
            payload = encode_font(font)
            font_sha = sha(payload)
            put_font(writer, descriptor, payload)
            for (
                name,
                char,
                mapped_gid,
                kind,
                indices,
                before_points,
                baseline_outline,
            ) in active:
                current = font["glyf"][name]
                records.append(
                    dict(
                        font_descriptor_object_no=object_index + 1,
                        font_input_sha256=font_sha,
                        source_font_sha256=sha(baseline_font),
                        original_gid=mapped_gid,
                        hint_gid=font.getGlyphID(name),
                        unicode_char=char,
                        role=kind,
                        expected_program_hex=current.program.getBytecode().hex(),
                        point_count=len(current.coordinates),
                        original_points=before_points,
                        first_on_curve=bool(current.flags[0] & 1),
                        baseline_outline=baseline_outline,
                        input_outline_sha256=sha(
                            json_bytes(outline_record(font, name))
                        ),
                        original_cvt_hex=cvt_raw.hex(),
                        source_reference_indices=indices,
                    )
                )
            if control is not None:
                target_name = companion_name if dense else hint_name
                control_font = copy.deepcopy(font)
                expected_control = bytes.fromhex("0131")
                control_font["glyf"][target_name].program = program(expected_control)
                # The control changes exactly one MIAP operation and its two PUSHes;
                # the source maxp/table configuration is intentionally unchanged.
                control_payload = encode_font(control_font)
                put_font(control, control._objects[object_index], control_payload)
                controls.append(
                    dict(
                        hint_gid=font.getGlyphID(target_name),
                        expected_program_hex=expected_control.hex(),
                        source_program_hex=font["glyf"][target_name]
                        .program.getBytecode()
                        .hex(),
                        font_input_sha256=font_sha,
                        control_font_sha256=sha(control_payload),
                    )
                )
        assert records and len({item["font_input_sha256"] for item in records}) == 1
        payload = write_pdf(writer)
        filename = profile + ".pdf"
        files.append((filename, payload))
        row.update(
            file=filename,
            sha256=sha(payload),
            refused=profile == "source-index-negative-32768",
            profile=profile,
            cvt_profile=profile,
            source_file=source.relative_to(root).as_posix(),
            source_sha256=original["sha256"],
            private_markers=[marker.decode() for marker in MARKERS],
            removed_only_reference_indices=[90],
            source_font_glyphs=records,
            refusal_reason=(
                "signed_negative_cvt_operand_with_in_range_unsigned_value"
                if profile == "source-index-negative-32768"
                else None
            ),
            source_cvt_entry_count=count,
            unsigned_negative_index_in_range=profile == "source-index-negative-32768",
        )
        measured = actual_positions(payload, row)
        row["boxes"] = measured if shared else measured["0"]
        row["source_positions_measured_before_product_execution"] = True
        if not row["refused"]:
            row["expected_local_closures"], row["expected_source_reference_union"] = (
                frozen_closures(payload, row, font_sha, records, cvt_raw, shared)
            )
        else:
            row["expected_local_closures"], row["expected_source_reference_union"] = (
                [],
                [],
            )
        if control is not None:
            control_payload = write_pdf(control)
            control_filename = profile + "-control.pdf"
            files.append((control_filename, control_payload))
            row.update(
                control_file=control_filename,
                control_sha256=sha(control_payload),
                control_target_gids=[item["hint_gid"] for item in controls],
                control_expected_programs=controls,
            )
        rows.append(row)
    criteria = dict(
        version=1,
        license="CC0 synthetic document derivatives; embedded Noto license unchanged",
        source_pins=PINS,
        cases=rows,
        position_error_pt=0.5,
        outside_pixel_difference=0,
        dpi=[72, 96, 300],
        rectangle_boundary_px=1,
        instruction_bytes_limit=4096,
        expected_max_stack_elements=2,
        retain_equal_values_at_distinct_indices=True,
        local_dense_indices_are_independent_of_same_source_union=True,
        independent_observability_dpi=[72, 96],
    )
    payload = json_bytes(criteria)
    files.append(("criteria.json", payload))
    total = sum(len(data) for _, data in files)
    assert total <= MAX_GENERATED_BYTES, (total, MAX_GENERATED_BYTES)
    if args.finalize_preexecution_positions:
        receipt_path = root / "evidence/dev41-preexecution-truth-draft-r1.json"
        preserved = json.loads(receipt_path.read_bytes())
        assert preserved["all_entries_read_back_sha_exact"]
        assert (
            sha((root / preserved["archive"]).read_bytes())
            == preserved["archive_sha256"]
        )
        assert (
            sha((destination / "criteria.json").read_bytes())
            == "4eb5aa33ec6450918df679ebdfb5e379ef7f87cef7c16fc420c4e4889d81364a"
        )
        assert {path.name for path in destination.iterdir()} == {
            name for name, _ in files
        }
        for name, data in files:
            if name != "criteria.json":
                assert (destination / name).read_bytes() == data, name
        (destination / "criteria.json").write_bytes(payload)
    elif args.verify_existing:
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
                ui_success_index=6,
                ui_refusal_index=1,
                verified=args.verify_existing,
            )
        )
    )


if __name__ == "__main__":
    main()
