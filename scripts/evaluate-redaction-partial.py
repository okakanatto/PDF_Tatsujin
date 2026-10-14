"""Independently inspect partial redaction outputs with PDFium/pypdf/fontTools."""

import argparse
import ctypes
import hashlib
import io
import math
import json
import re
import subprocess
from pathlib import Path
from PIL import Image

import numpy as np
import pypdfium2 as pdfium
import pypdfium2.raw as pdfium_raw
from fontTools.pens.recordingPen import DecomposingRecordingPen
from fontTools.ttLib import TTFont
from pypdf import PdfReader
from pypdf.generic import IndirectObject, StreamObject
from test_tools import poppler_tool


def sha(data):
    return hashlib.sha256(data).hexdigest()


def objects(reader):
    for generation, entries in reader.xref.items():
        for number in entries:
            if number:
                yield reader.get_object(IndirectObject(number, generation, reader))


def font_characters(reader):
    result = {}
    for value in objects(reader):
        if not isinstance(value, dict) or "/ToUnicode" not in value:
            continue
        data = value["/ToUnicode"].get_data()
        mappings = []
        for body in re.findall(rb"beginbfchar(.*?)endbfchar", data, re.S):
            mappings += re.findall(rb"<([0-9a-fA-F]+)>\s*<([0-9a-fA-F]+)>", body)
        child = value
        composite = "/DescendantFonts" in value
        if composite:
            child = value["/DescendantFonts"][0].get_object()
        program = TTFont(io.BytesIO(child["/FontDescriptor"]["/FontFile2"].get_data()))
        source_cmap = next((t.cmap for t in program["cmap"].tables), {})
        gid_map = child.get("/CIDToGIDMap")
        for code, cp in mappings:
            character = bytes.fromhex(cp.decode()).decode("utf-16-be")
            cid = int(code, 16)
            if composite:
                gid = (
                    cid
                    if str(gid_map) == "/Identity" or gid_map is None
                    else int.from_bytes(
                        gid_map.get_object().get_data()[cid * 2 : cid * 2 + 2], "big"
                    )
                )
                glyph = program.getGlyphOrder()[gid]
            else:
                glyph = source_cmap.get(cid)
            if glyph:
                glyphs = program.getGlyphSet()
                pen = DecomposingRecordingPen(glyphs)
                glyphs[glyph].draw(pen)
                result[character] = pen.value
    return result


def text_and_boxes(path, phrases):
    with pdfium.PdfDocument(path) as document:
        page = document[0]
        text = page.get_textpage()
        contents = text.get_text_range()
        result = {}
        for term in phrases:
            assert contents.count(term) == 1, (path, term, contents)
            start = contents.index(term)
            result[term] = [
                list(text.get_charbox(i)) for i in range(start, start + len(term))
            ]
        text.close()
        page.close()
        return contents, result


def raster_unchanged_outside(source, target, rectangle, poppler=None, page_index=0):
    with pdfium.PdfDocument(source) as before, pdfium.PdfDocument(target) as after:
        left, right = before[page_index], after[page_index]
        if poppler:

            def render(path):
                process = subprocess.run(
                    [
                        str(poppler),
                        "-cropbox",
                        "-singlefile",
                        "-png",
                        "-r",
                        "300",
                        "-f",
                        str(page_index + 1),
                        "-l",
                        str(page_index + 1),
                        str(path),
                    ],
                    capture_output=True,
                    timeout=60,
                    check=True,
                )
                return np.array(Image.open(io.BytesIO(process.stdout)).convert("RGB"))

            image_a, image_b = render(source), render(target)
        else:
            image_a = np.array(left.render(scale=300 / 72).to_pil().convert("RGB"))
            image_b = np.array(right.render(scale=300 / 72).to_pil().convert("RGB"))
        assert image_a.shape == image_b.shape
        height, width = image_a.shape[:2]
        x, y, w, h = rectangle
        points = []
        for a, b in ((x, y), (x + w, y), (x, y + h), (x + w, y + h)):
            out_x, out_y = ctypes.c_int(), ctypes.c_int()
            assert pdfium_raw.FPDF_PageToDevice(
                left.raw,
                0,
                0,
                width * 200,
                height * 200,
                0,
                a,
                b,
                ctypes.byref(out_x),
                ctypes.byref(out_y),
            )
            points.append((out_x.value / 200, out_y.value / 200))
        x0 = max(0, math.floor(min(p[0] for p in points)) - 1)
        x1 = min(width, math.ceil(max(p[0] for p in points)) + 1)
        y0 = max(0, math.floor(min(p[1] for p in points)) - 1)
        y1 = min(height, math.ceil(max(p[1] for p in points)) + 1)
        changed = np.any(image_a != image_b, axis=2)
        changed[y0:y1, x0:x1] = False
        result = int(np.count_nonzero(changed))
        left.close()
        right.close()
        return result


def real_ocr(run, fixtures, poppler):
    row = {"file": "real-D03-existing-OCR", "status": "FAIL"}
    try:
        data = (fixtures.parent / "partial-real-ocr.json").read_bytes()
        assert (
            sha(data)
            == "dd0d29256ec4b6628a68b8dac74b03bb98aa53825c5551588fd76890fd8e1ffa"
        )
        criterion = json.loads(data)
        source = run / "redaction-real-ocr-before.pdf"
        target = run / "redaction-partial-real-ocr.pdf"
        before, after = PdfReader(source), PdfReader(target)
        japanese, positions = text_and_boxes(target, criterion["keep_japanese"])
        _, originals = text_and_boxes(source, criterion["keep_japanese"])
        assert criterion["remove"] not in japanese
        assert criterion["remove"] not in after.pages[0].extract_text()
        for term in criterion["keep_japanese"]:
            assert term in after.pages[0].extract_text()
        for term in criterion["keep_english"]:
            assert term in after.pages[1].extract_text().lower()
        error = max(
            abs(a - b)
            for term in positions
            for left, right in zip(positions[term], originals[term])
            for a, b in zip(left, right)
        )
        assert error <= criterion["max_position_error_pt"], error
        original_streams = set()
        for name, ref in before.pages[0]["/Resources"]["/XObject"].items():
            obj = ref.get_object()
            if name.startswith("/TatsuOCR_"):
                for font_ref in obj["/Resources"]["/Font"].values():
                    font = font_ref.get_object()
                    original_streams.add(sha(font["/ToUnicode"].get_data()))
                    for child in font["/DescendantFonts"]:
                        original_streams.add(
                            sha(
                                child.get_object()["/FontDescriptor"][
                                    "/FontFile2"
                                ].get_data()
                            )
                        )
            elif obj.get("/Subtype") == "/Image":
                original_streams.add(sha(obj.get_data()))
        assert len(original_streams) >= 3
        for value in objects(after):
            if isinstance(value, StreamObject):
                assert (
                    sha(value.get_data()) not in original_streams
                ), "Original Japanese OCR/font/image stream remains"
        for key in ("/MediaBox", "/CropBox", "/Rotate", "/UserUnit"):
            assert all(
                a.get(key) == b.get(key) for a, b in zip(before.pages, after.pages)
            ), key
        changed = raster_unchanged_outside(
            source, target, criterion["raw_pdf_rectangle"]
        )
        external_changed = raster_unchanged_outside(
            source, target, criterion["raw_pdf_rectangle"], poppler
        )
        assert changed == 0 and external_changed == 0, (changed, external_changed)
        with pdfium.PdfDocument(source) as left, pdfium.PdfDocument(target) as right:
            assert np.array_equal(
                np.array(left[1].render(scale=1).to_pil()),
                np.array(right[1].render(scale=1).to_pil()),
            )
        row.update(
            status="PASS",
            max_position_error_pt=error,
            old_Japanese_OCR_font_image_absent=True,
            outside_changed_pixels=changed,
            Poppler_outside_changed_pixels=external_changed,
            second_page_unchanged=True,
        )
    except FileNotFoundError as error:
        row.update(status="未実行", error=str(error))
    except Exception as error:
        row["error"] = str(error)
    return row


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--fixtures", type=Path, required=True)
    parser.add_argument("--run", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    assert not args.output.exists(), "Keep prior independent evidence"
    criteria_bytes = (args.fixtures / "criteria.json").read_bytes()
    assert (
        sha(criteria_bytes)
        == "fdde07694bcdc03c679b56d8aaacd81150809cdea3e6862150b0a8d5411dbffa"
    )
    criteria = json.loads(criteria_bytes)
    poppler = poppler_tool("pdftoppm")
    results = []
    for case in criteria["cases"]:
        row = {"file": case["file"], "status": "FAIL"}
        try:
            source = args.fixtures / case["file"]
            target = args.run / ("partial-" + case["file"])
            assert sha(source.read_bytes()) == case["sha256"]
            before, after = PdfReader(source), PdfReader(target)
            text, positions = text_and_boxes(target, case["keep"])
            assert case["remove"] not in text
            pypdf_text = after.pages[0].extract_text()
            assert case["remove"] not in pypdf_text
            for term in case["keep"]:
                assert term in pypdf_text, (term, "pypdf retained text")
            max_error = 0
            for term, boxes in positions.items():
                baseline = case["boxes"][term]
                assert len(boxes) == len(baseline)
                max_error = max(
                    max_error,
                    max(
                        abs(a - b)
                        for x, y in zip(boxes, baseline)
                        for a, b in zip(x, y)
                    ),
                )
            assert max_error <= case["max_position_error_pt"], max_error
            for key in ("/MediaBox", "/CropBox", "/Rotate", "/UserUnit"):
                assert before.pages[0].get(key) == after.pages[0].get(key), key
            originals = set()
            for value in objects(before):
                if isinstance(value, dict) and "/ToUnicode" in value:
                    originals.add(sha(value["/ToUnicode"].get_data()))
                if isinstance(value, dict) and "/FontFile2" in value:
                    originals.add(sha(value["/FontFile2"].get_data()))
                if (
                    case["file"] == "scan-OCR.pdf"
                    and isinstance(value, StreamObject)
                    and value.get("/Subtype") == "/Image"
                ):
                    originals.add(sha(value.get_data()))
            for value in objects(after):
                if isinstance(value, StreamObject):
                    assert (
                        sha(value.get_data()) not in originals
                    ), "Source secret-bearing stream remains"
                    if value.get("/Subtype") != "/Image":
                        for phrase in (
                            case["remove"].encode("utf-8"),
                            case["remove"].encode("utf-16-be"),
                        ):
                            assert (
                                phrase not in value.get_data()
                            ), "Secret recovered in decoded stream"
            old_glyphs, new_glyphs = font_characters(before), font_characters(after)
            for character in case["remove"]:
                if not any(character in term for term in case["keep"]):
                    assert (
                        character not in new_glyphs
                    ), "Removed character remains in rebuilt font map"
            for character, outline in new_glyphs.items():
                assert old_glyphs[character] == outline, (character, "Outline changed")
            assert "/Prev" not in after.trailer and "/Info" not in after.trailer
            changed = raster_unchanged_outside(source, target, case["rect"])
            assert changed == 0, (
                changed,
                "Changed pixels outside fixed rectangle and one pixel border",
            )
            external_changed = raster_unchanged_outside(
                source, target, case["rect"], poppler
            )
            assert external_changed == 0, (
                external_changed,
                "Poppler changed outside pixels",
            )
            row.update(
                status="PASS",
                max_position_error_pt=max_error,
                preserved_glyphs=len(new_glyphs),
                old_fonts_maps_images_absent=True,
                outside_changed_pixels=changed,
                Poppler_outside_changed_pixels=external_changed,
                rendering_dpi=300,
            )
        except FileNotFoundError as error:
            row["status"] = "未実行"
            row["error"] = str(error)
        except Exception as error:
            row["error"] = str(error)
        results.append(row)
    results.append(real_ocr(args.run, args.fixtures, poppler))
    report = {
        "criteria_sha256": sha(criteria_bytes),
        "engines": [
            "PDFium text/render",
            "pypdf text/objects",
            "fontTools outlines/maps",
            "Poppler render",
        ],
        "Poppler_text": "未実行; pdftotext not available",
        "results": results,
        "passed": sum(r["status"] == "PASS" for r in results),
        "failed": sum(r["status"] == "FAIL" for r in results),
        "unexecuted": sum(r["status"] == "未実行" for r in results),
        "native_external_GUI": "未実行",
    }
    args.output.write_text(
        json.dumps(report, ensure_ascii=False, indent=2) + "\n", encoding="utf-8"
    )
    print(json.dumps(report, ensure_ascii=False))
    return int(report["failed"] != 0 or report["unexecuted"] != 0)


if __name__ == "__main__":
    raise SystemExit(main())
