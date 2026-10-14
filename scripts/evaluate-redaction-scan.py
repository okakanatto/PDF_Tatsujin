"""Inspect fixed partial scan redaction before OCR with independent PDF engines."""

import argparse
import hashlib
import importlib.util
import json
import math
from pathlib import Path

from PIL import Image, ImageChops, ImageDraw
from pypdf import PdfReader
from pypdf.generic import IndirectObject


def module(name):
    path = Path(__file__).with_name(name)
    spec = importlib.util.spec_from_file_location("tatsu_" + path.stem, path)
    result = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(result)
    return result


def streams(reader):
    result = set()
    for number in sorted(reader.xref.get(0, {})):
        if number:
            value = IndirectObject(number, 0, reader).get_object()
            if hasattr(value, "get_data"):
                result.add(hashlib.sha256(value.get_data()).hexdigest())
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--fixtures", required=True, type=Path)
    parser.add_argument("--run", required=True, type=Path)
    parser.add_argument("--poppler", required=True, type=Path)
    args = parser.parse_args()
    output = args.run / "independent-redaction-scan.json"
    if output.exists():
        raise FileExistsError("Preserve prior evaluation")
    criteria = json.loads(
        (args.fixtures / "redaction-copy/scan-then-ocr.json").read_text(
            encoding="utf-8"
        )
    )
    inspect = module("evaluate-redaction-probe.py")
    render = module("evaluate-redaction-copy.py")
    for file_key, hash_key in (
        ("source", "source_sha256"),
        ("digital_source", "digital_source_sha256"),
    ):
        assert (
            inspect.digest(args.fixtures / criteria[file_key]) == criteria[hash_key]
        ), "Frozen input changed"
    original_path = args.run / "redaction-scan-source.pdf"
    original = PdfReader(original_path)
    original_image = original.pages[0].images[0]
    original_stream = hashlib.sha256(
        original_image.indirect_reference.get_object().get_data()
    ).hexdigest()
    original_pixels = original_image.image.convert("RGB")
    crop = original.pages[0].cropbox
    x, y, width, height = criteria["raw_pdf_rectangle"]
    pixel_box = (
        math.ceil((x - float(crop.left)) * original_pixels.width / float(crop.width))
        + 1,
        math.ceil(
            (float(crop.top) - y - height) * original_pixels.height / float(crop.height)
        )
        + 1,
        math.floor(
            (x + width - float(crop.left)) * original_pixels.width / float(crop.width)
        )
        - 1,
        math.floor((float(crop.top) - y) * original_pixels.height / float(crop.height))
        - 1,
    )
    original_nonblack = sum(
        max(pixel) > 0 for pixel in original_pixels.crop(pixel_box).get_flattened_data()
    )
    assert (
        original_nonblack > 100
    ), "Target must contain recoverable image pixels before redaction"
    render_directory = args.run / "independent-scan-poppler"
    render_directory.mkdir(exist_ok=False)
    before, warning = render.poppler(
        args.poppler,
        original_path,
        render_directory / "source-japanese",
        dpi=criteria["render_dpi"],
    )
    mask = Image.new("L", before.size, 255)
    draw = ImageDraw.Draw(mask)
    sx, sy = before.width / float(crop.width), before.height / float(crop.height)
    margin = criteria["boundary_tolerance_pixels"]
    draw.rectangle(
        (
            math.floor((x - float(crop.left)) * sx) - margin,
            math.floor((float(crop.top) - y - height) * sy) - margin,
            math.ceil((x + width - float(crop.left)) * sx) + margin,
            math.ceil((float(crop.top) - y) * sy) + margin,
        ),
        fill=0,
    )
    before_english, english_warning = render.poppler(
        args.poppler,
        original_path,
        render_directory / "source-english",
        page=2,
        dpi=criteria["render_dpi"],
    )
    rows = []
    redacted_render = None
    for name, recognized in (
        ("redaction-scan-before-ocr.pdf", False),
        ("redaction-scan-ocr-saved.pdf", True),
    ):
        path = args.run / name
        reader = PdfReader(path)
        image = reader.pages[0].images[0].image.convert("RGB")
        leaked = sum(
            max(pixel) > 0 for pixel in image.crop(pixel_box).get_flattened_data()
        )
        extracted = inspect.text(path)
        raw = inspect.payload(reader)
        checks = dict(
            geometry_preserved=inspect.geometry(reader) == inspect.geometry(original),
            original_target_image_stream_absent=original_stream not in streams(reader),
            target_internal_image_pixels_black=leaked == 0,
            removed_word_absent_from_text=criteria["remove"] not in extracted,
            removed_word_literal_absent_from_all_objects_and_streams=all(
                criteria["remove"].encode(encoding) not in raw
                for encoding in ("utf-8", "utf-16-be", "utf-16-le")
            ),
            no_old_revision="/Prev" not in reader.trailer,
        )
        if recognized:
            checks["outside_japanese_terms_preserved"] = all(
                term in extracted for term in criteria["keep_japanese"]
            )
            checks["outside_english_terms_preserved"] = all(
                term.lower() in extracted.lower() for term in criteria["keep_english"]
            )
        else:
            checks["old_scan_had_no_text_layer"] = not extracted.strip()
        after, after_warning = render.poppler(
            args.poppler,
            path,
            render_directory / (path.stem + "-japanese"),
            dpi=criteria["render_dpi"],
        )
        assert before.size == after.size, "Rendered geometry changed"
        difference = ImageChops.difference(before, after)
        checks["pixels_outside_fixed_rectangle_identical"] = (
            ImageChops.multiply(difference, mask.convert("RGB")).getbbox() is None
        )
        if recognized:
            checks["OCR_keeps_redacted_page_pixels_identical"] = (
                ImageChops.difference(redacted_render, after).getbbox() is None
            )
        else:
            redacted_render = after
        after_english, after_english_warning = render.poppler(
            args.poppler,
            path,
            render_directory / (path.stem + "-english"),
            page=2,
            dpi=criteria["render_dpi"],
        )
        checks["outside_english_signature_page_pixels_identical"] = (
            before_english.size == after_english.size
            and ImageChops.difference(before_english, after_english).getbbox() is None
        )
        rows.append(
            dict(
                file=name,
                status="PASS" if all(checks.values()) else "FAIL",
                checks=checks,
                original_target_nonblack_pixels=original_nonblack,
                remaining_target_nonblack_pixels=leaked,
                sha256=inspect.digest(path),
                poppler_warnings=[
                    warning,
                    english_warning,
                    after_warning,
                    after_english_warning,
                ],
            )
        )
    result = dict(
        status="PASS" if all(row["status"] == "PASS" for row in rows) else "FAIL",
        cases=rows,
        criteria_sha256=inspect.digest(
            args.fixtures / "redaction-copy/scan-then-ocr.json"
        ),
        scope="Fixed synthetic partial scan redaction before OCR; independent PDFium, pypdf and 300dpi Poppler. No general resource safety or native viewer acceptance.",
    )
    output.write_text(
        json.dumps(result, ensure_ascii=False, indent=2), encoding="utf-8"
    )
    print(json.dumps(result, ensure_ascii=False, indent=2))
    if result["status"] != "PASS":
        raise SystemExit(1)


if __name__ == "__main__":
    main()
