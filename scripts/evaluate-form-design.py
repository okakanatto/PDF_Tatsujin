"""Independently inspect app-created AcroForms, complete fonts and saved geometry."""

import argparse
import hashlib
import importlib.util
import io
import json
import re
from pathlib import Path

from fontTools.ttLib import TTFont
from pypdf import PdfReader, PdfWriter
from pypdf.generic import DecodedStreamObject, NameObject
import pypdfium2 as pdfium


ROOT = Path(__file__).resolve().parents[1]


def sha(data):
    return hashlib.sha256(data).hexdigest()


def appearance_text(appearance, target):
    writer = PdfWriter()
    bbox = list(appearance["/BBox"])
    page = writer.add_blank_page(
        width=float(bbox[2]) - float(bbox[0]), height=float(bbox[3]) - float(bbox[1])
    )
    stream = DecodedStreamObject()
    stream.set_data(appearance.get_data())
    page[NameObject("/Contents")] = writer._add_object(stream)
    page[NameObject("/Resources")] = appearance["/Resources"].clone(writer)
    writer.write(target)
    with pdfium.PdfDocument(target) as pdf:
        p = pdf[0]
        t = p.get_textpage()
        text = t.get_text_range().replace("\r\n", "\n").rstrip("\n")
        t.close()
        p.close()
    return text


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--run", required=True, type=Path)
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    run = args.run.resolve()
    report_path = args.output or run / "independent-form-design.json"
    if report_path.exists():
        raise FileExistsError(f"Preserve earlier evidence: {report_path}")
    report_path.parent.mkdir(parents=True, exist_ok=True)
    result = {
        "status": "FAIL",
        "scope": "Independent pypdf/PDFium and deterministic font derivation; synthetic form-design inputs",
        "Reader_GUI": "未実行",
    }
    try:
        source = PdfReader(ROOT / "fixtures/D07.pdf")
        expected_foreign = source.get_fields()
        files = (
            "designed-six.pdf",
            "designed-filled.pdf",
            "designed-reedited.pdf",
            "designed-external-empty.pdf",
        )
        field_rows = []
        expected_types = ["/Tx", "/Tx", "/Btn", "/Btn", "/Ch", "/Ch"]
        metadata = json.loads(
            (ROOT / "assets/fonts/TatsujinSansJP-Regular.json").read_text(
                encoding="utf8"
            )
        )
        for filename in files:
            reader = PdfReader(run / filename)
            fields = reader.get_fields()
            for name, original in expected_foreign.items():
                actual = fields[name]
                for key in ("/FT", "/Ff", "/V", "/DV", "/Opt", "/MaxLen"):
                    assert actual.get(key) == original.get(key), (filename, name, key)
            form = reader.trailer["/Root"]["/AcroForm"].get_object()
            font_ref = form.raw_get("/TatsujinFormFont")
            font = font_ref.get_object()
            assert font["/Encoding"] == "/Identity-H"
            cid = font["/DescendantFonts"][0].get_object()
            descriptor = cid["/FontDescriptor"].get_object()
            embedded = descriptor["/FontFile2"].get_object().get_data()
            assert sha(embedded) == metadata["font_sha256"]
            cmap = font["/ToUnicode"].get_object().get_data()
            mappings = {
                int(a, 16): bytes.fromhex(b.decode("ascii")).decode("utf-16-be")
                for a, b in re.findall(
                    rb"<([0-9a-fA-F]{4})>\s*<([0-9a-fA-F]{4,8})>", cmap
                )
            }
            mapping = cid["/CIDToGIDMap"].get_object().get_data()
            widths = cid["/W"][1]
            assert len(widths) == len(metadata["characters"]) + 1
            for index, (code, gid, width) in enumerate(metadata["characters"], 1):
                assert mappings[index] == chr(code)
                assert int.from_bytes(mapping[index * 2 : index * 2 + 2], "big") == gid
                assert float(widths[index]) == width
            for index, expected_type in enumerate(expected_types, 1):
                name = f"designed-{index}"
                field = fields[name]
                assert field["/FT"] == expected_type
                root_ref = next(
                    value
                    for value in form["/Fields"]
                    if value.get_object().get("/T") == name
                )
                root = root_ref.get_object()
                kids = root["/Kids"]
                assert len(kids) == (2 if index == 4 else 1)
                for child in kids:
                    widget = child.get_object()
                    assert widget.raw_get("/Parent") == root_ref
                    assert int(widget["/F"]) & 4
                    assert child in reader.pages[0]["/Annots"]
                    assert widget.raw_get("/P") == reader.pages[0].indirect_reference
                    if expected_type != "/Btn":
                        assert widget.raw_get("/TatsujinFormFont") == font_ref
                        assert widget["/AP"]["/N"].get_object().get_data()
                    else:
                        normal = widget["/AP"]["/N"].get_object()
                        assert "/Off" in normal and len(normal) == 2
                        value = root["/V"]
                        assert widget["/AS"] == (value if value in normal else "/Off")
            expected_text = (
                ""
                if filename == "designed-external-empty.pdf"
                else (
                    "髙橋 𠮷野"
                    if filename == "designed-six.pdf"
                    else "山田 太郎 髙橋 𠮷野"
                )
            )
            assert fields["designed-1"]["/V"] == expected_text
            first = next(
                a.get_object()
                for a in reader.pages[0]["/Annots"]
                if a.get_object().get("/Parent")
                and a.get_object()["/Parent"].get_object().get("/T") == "designed-1"
            )
            actual_text = appearance_text(
                first["/AP"]["/N"].get_object(),
                run / (filename.removesuffix(".pdf") + "-text-appearance.pdf"),
            )
            assert actual_text == expected_text, (filename, actual_text)
            field_rows.append(
                {
                    "file": filename,
                    "six_kinds": True,
                    "canonical_tree_and_widgets": True,
                    "complete_font_shared": True,
                    "foreign_values_exact": True,
                    "appearance_Unicode_exact": True,
                }
            )
        font = TTFont(io.BytesIO(embedded))
        assert font["OS/2"].fsType == 0 and "fvar" not in font
        assert font["name"].getDebugName(1) == "Tatsujin Sans JP"
        assert font["name"].getDebugName(2) == "Regular"
        assert (
            font.getBestCmap().keys()
            == TTFont(ROOT / "assets/fonts/NotoSansJP.ttf").getBestCmap().keys()
        )
        spec = importlib.util.spec_from_file_location(
            "tatsu_font_recipe", ROOT / "scripts/derive-form-font.py"
        )
        recipe = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(recipe)
        first_font, first_map = recipe.derive(ROOT / "assets/fonts/NotoSansJP.ttf")
        second_font, second_map = recipe.derive(ROOT / "assets/fonts/NotoSansJP.ttf")
        assert first_font == second_font == embedded
        assert (
            first_map
            == second_map
            == (ROOT / "assets/fonts/TatsujinSansJP-Regular.json").read_bytes()
        )
        geometry = PdfReader(run / "designed-geometry-filled.pdf")
        rectangles = []
        for index, page in enumerate(geometry.pages):
            annotation = next(
                a.get_object()
                for a in page["/Annots"]
                if a.get_object().get("/TatsujinWidget")
            )
            x0, y0, x1, y1 = map(float, annotation["/Rect"])
            cx0, cy0, cx1, cy1 = map(float, page.cropbox)
            unit = float(page.get("/UserUnit", 1))
            rotation = int(page.get("/Rotate", 0)) % 360
            corners = [(x, y) for x in (x0, x1) for y in (y0, y1)]

            def physical(x, y):
                if rotation == 0:
                    return ((x - cx0) * unit, (cy1 - y) * unit)
                if rotation == 90:
                    return ((y - cy0) * unit, (x - cx0) * unit)
                if rotation == 180:
                    return ((cx1 - x) * unit, (y - cy0) * unit)
                return ((cy1 - y) * unit, (cx1 - x) * unit)

            points = [physical(x, y) for x, y in corners]
            actual = [
                min(p[0] for p in points),
                min(p[1] for p in points),
                max(p[0] for p in points) - min(p[0] for p in points),
                max(p[1] for p in points) - min(p[1] for p in points),
            ]
            assert max(abs(a - b) for a, b in zip(actual, [36, 45, 180, 32])) <= 0.5
            assert int(annotation["/MK"]["/R"]) == rotation
            rectangles.append(
                {
                    "page": index + 1,
                    "rotation": rotation,
                    "UserUnit": unit,
                    "visible_rect": actual,
                }
            )
        with pdfium.PdfDocument(run / "designed-geometry-filled.pdf") as pdf:
            pdf.init_forms()
            for index in range(len(pdf)):
                page = pdf[index]
                bitmap = page.render(scale=1.4, draw_annots=True)
                bitmap.to_pil().save(run / f"independent-form-geometry-{index}.png")
                bitmap.close()
                page.close()
        result.update(
            status="PASS",
            fields=field_rows,
            geometry=rectangles,
            full_font_sha256=sha(embedded),
            Unicode_characters=len(metadata["characters"]),
            supplementary_characters=sum(
                code > 65535 for code, _, _ in metadata["characters"]
            ),
            deterministic_derivation=True,
            editable_embedding=True,
        )
    except Exception as error:
        result["error"] = str(error)
        raise
    finally:
        report_path.write_text(
            json.dumps(result, ensure_ascii=False, indent=2), encoding="utf8"
        )
    print(
        json.dumps(
            {
                "status": result["status"],
                "fields": len(result.get("fields", [])),
                "geometry": len(result.get("geometry", [])),
            }
        )
    )


if __name__ == "__main__":
    main()
