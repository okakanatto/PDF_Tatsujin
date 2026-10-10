"""Independently inspect actual app redaction copies; no general security claim."""

import argparse
import hashlib
import importlib.util
import json
import math
import subprocess
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


def poppler(executable, path, destination, page=1, dpi=72):
    result = subprocess.run(
        [
            str(executable),
            "-cropbox",
            "-singlefile",
            "-png",
            "-r",
            str(dpi),
            "-f",
            str(page),
            "-l",
            str(page),
            str(path),
            str(destination),
        ],
        check=True,
        capture_output=True,
        timeout=60,
    )
    image = Image.open(destination.with_suffix(".png")).convert("RGB")
    return image, result.stderr.decode("utf-8", errors="replace")


def group_streams(reference):
    pending = [reference]
    visited = set()
    hashes = set()
    while pending:
        value = pending.pop()
        if isinstance(value, IndirectObject):
            key = (value.idnum, value.generation)
            if key in visited:
                continue
            visited.add(key)
            value = value.get_object()
        if hasattr(value, "get_data"):
            hashes.add(hashlib.sha256(value.get_data()).hexdigest())
        if isinstance(value, dict):
            pending.extend(value.values())
        elif isinstance(value, list):
            pending.extend(value)
    return hashes


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--fixtures", type=Path, required=True)
    parser.add_argument("--run", type=Path, required=True)
    parser.add_argument("--poppler", type=Path, required=True)
    args = parser.parse_args()
    output = args.run / "independent-redaction-copy.json"
    if output.exists():
        raise FileExistsError("Preserve prior inspection")
    base = args.fixtures / "redaction-copy/foundation"
    font_base = args.fixtures / "redaction-copy/fonts"
    inspect = module("evaluate-redaction-probe.py")
    fonts = module("evaluate-redaction-font-probe.py")
    source = base / "unsafe-source.pdf"
    criteria = json.loads((base / "criteria.json").read_text(encoding="utf-8"))
    assert inspect.digest(source) == criteria["source_sha256"], "Frozen source changed"
    plan = json.loads((base / "plan.json").read_text(encoding="utf-8"))
    rows = []
    render = args.run / "independent-poppler"
    render.mkdir(exist_ok=False)
    for label, original_path in (
        ("foundation", source),
        ("ui", args.run / "redaction-ui-source-snapshot.pdf"),
    ):
        path = args.run / ("redaction-" + label + ".pdf")
        reader, original = PdfReader(path), PdfReader(original_path)
        data = inspect.payload(reader)
        extracted = inspect.text(path)
        field = (reader.get_fields() or {}).get("keep-field", {})
        protected = (
            "SECRET_TEXT_9df67",
            "SECRET_HIDDEN_9df67",
            "SECRET_FORM_9df67",
            "SECRET_META_9df67",
            "SECRET_XMP_9df67",
            "SECRET_ATTACH_9df67",
            "SECRET_ORPHAN_9df67",
        )
        images = [
            image.image.convert("RGB") for page in reader.pages for image in page.images
        ]
        assert len(images) == 1 and images[0].size == (
            240,
            120,
        ), "Unexpected image geometry"
        leaked = sum(
            max(pixel) > 32
            for pixel in images[0].crop((44, 54, 116, 86)).get_flattened_data()
        )
        checks = dict(
            protected_payload_absent=all(
                word not in extracted and word.encode() not in data
                for word in protected
            ),
            targeted_image_interior_recovered_pixels=leaked,
            outside_text_preserved=all(
                word in extracted
                for word in ("KEEP_VISIBLE_9df67", "KEEP_SECOND_PAGE_9df67")
            ),
            canonical_form_value_and_default_preserved=field.get("/V")
            == "KEEP_FORM_9df67"
            and field.get("/DV") == "KEEP_FORM_9df67",
            geometry_preserved=inspect.geometry(reader) == inspect.geometry(original),
            no_previous_revision="/Prev" not in reader.trailer,
        )
        before, first_warning = poppler(
            args.poppler, original_path, render / (label + "-source")
        )
        after, second_warning = poppler(args.poppler, path, render / (label + "-copy"))
        assert before.size == after.size, "Poppler geometry changed"
        crop = original.pages[0].cropbox
        scale_x, scale_y = before.width / float(crop.width), before.height / float(
            crop.height
        )
        mask = Image.new("L", before.size, 255)
        draw = ImageDraw.Draw(mask)
        for entry in plan:
            x, y, width, height = entry["rect"]
            # Exclude the selected boundary and two antialias pixels, fixed before comparison.
            left = (x - float(crop.left)) * scale_x
            top = (float(crop.top) - y - height) * scale_y
            draw.rectangle(
                (
                    math.floor(left) - 2,
                    math.floor(top) - 2,
                    math.ceil(left + width * scale_x) + 2,
                    math.ceil(top + height * scale_y) + 2,
                ),
                fill=0,
            )
        difference = ImageChops.difference(before, after)
        outside = ImageChops.multiply(difference.convert("RGB"), mask.convert("RGB"))
        checks["poppler_outside_ranges_pixels_identical"] = outside.getbbox() is None
        if label == "ui":
            before_second, _ = poppler(
                args.poppler, original_path, render / "ui-second-source", 2
            )
            after_second, _ = poppler(args.poppler, path, render / "ui-second-copy", 2)
            checks["outside_signature_page_pixels_identical"] = (
                before_second.size == after_second.size
                and ImageChops.difference(before_second, after_second).getbbox() is None
            )
        passed = (
            all(
                value is True
                for key, value in checks.items()
                if key != "targeted_image_interior_recovered_pixels"
            )
            and leaked == 0
        )
        rows.append(
            dict(
                file=path.name,
                status="PASS" if passed else "FAIL",
                checks=checks,
                poppler_warnings=[first_warning, second_warning],
                sha256=inspect.digest(path),
            )
        )
    for label, expected in (
        ("form", "EDIT_AFTER_REDACTION"),
        ("ui", "UI_EDIT_AFTER_REDACTION"),
    ):
        path = args.run / ("redaction-" + label + "-reedited.pdf")
        reader = PdfReader(path)
        field = (reader.get_fields() or {}).get("keep-field", {})
        widget = reader.pages[0]["/Annots"][0].get_object()
        owner = widget.get("/Parent", widget).get_object()
        valid = (
            field.get("/V") == expected
            and field.get("/DV") == "KEEP_FORM_9df67"
            and owner.get("/V") == expected
            and bool(widget["/AP"]["/N"].get_data())
        )
        poppler(args.poppler, path, render / path.stem)
        rows.append(
            dict(
                file=path.name,
                status="PASS" if valid else "FAIL",
                canonical_value=field.get("/V"),
                reset_default=field.get("/DV"),
                widget_value_and_appearance=valid,
            )
        )
    for case in json.loads((font_base / "criteria.json").read_text(encoding="utf-8"))[
        "cases"
    ]:
        if case["expect_rejection"]:
            continue
        original = font_base / case["file"]
        assert fonts.sha(original) == case["sha256"]
        paths = [args.run / ("redaction-font-" + case["file"])]
        if case["file"] in ("visible.pdf", "invisible-OCR.pdf"):
            paths.append(args.run / ("redaction-ui-font-" + case["file"]))
        for path in paths:
            contents = fonts.page_text(path)
            valid = (
                case["remove"] not in contents
                and case["keep"] in contents
                and case["remove"] not in fonts.subset_characters(path)
                and not (fonts.font_payloads(original) & fonts.stream_hashes(path))
            )
            if case.get("preserve_position_phrase"):
                valid = (
                    valid
                    and fonts.phrase_boxes(path, case["preserve_position_phrase"])
                    == case["preserved_character_boxes"]
                )
            poppler(args.poppler, path, render / path.stem)
            rows.append(
                dict(
                    file=path.name,
                    status="PASS" if valid else "FAIL",
                    target_and_original_font_payloads_absent=valid,
                    sha256=fonts.sha(path),
                )
            )
    geometry_path = args.run / "redaction-geometry-ui.pdf"
    if geometry_path.exists():
        reader = PdfReader(geometry_path)
        valid = (
            inspect.geometry(reader) == inspect.geometry(PdfReader(source))
            and "KEEP_SECOND_PAGE_9df67" not in inspect.text(geometry_path)
            and "SECRET_TEXT_9df67" in inspect.text(geometry_path)
            and len(reader.get_fields() or {}) == 2
        )
        poppler(args.poppler, geometry_path, render / "rotated-copy", 2)
        rows.append(
            dict(
                file=geometry_path.name,
                status="PASS" if valid else "FAIL",
                selected_rotated_page_and_unselected_page_preserved=valid,
            )
        )
    ocr_path = args.run / "redaction-real-ocr-ui.pdf"
    if ocr_path.exists():
        before_path = args.run / "redaction-real-ocr-before.pdf"
        before, after = PdfReader(before_path), PdfReader(ocr_path)
        removed = set()
        for reference in before.pages[0]["/Resources"]["/XObject"].values():
            if reference.get_object().get("/Subtype") == "/Form":
                removed |= group_streams(reference)
        assert removed, "Original actual OCR group required"
        images = [image.image.convert("RGB") for image in after.pages[0].images]
        first = fonts.page_text(ocr_path)
        source_text = inspect.text(before_path)
        output_text = inspect.text(ocr_path)
        left, _ = poppler(
            args.poppler, before_path, render / "real-ocr-english-source", 2
        )
        right, _ = poppler(args.poppler, ocr_path, render / "real-ocr-english-copy", 2)
        poppler(args.poppler, ocr_path, render / "real-ocr-redacted", 1)
        checks = dict(
            actual_Japanese_source_term="市民公園" in source_text,
            selected_OCR_text_absent=not first.strip(),
            selected_image_pixels_all_black=bool(images)
            and all(image.getextrema() == ((0, 0), (0, 0), (0, 0)) for image in images),
            original_OCR_group_font_and_mapping_streams_absent=not bool(
                removed & fonts.stream_hashes(ocr_path)
            ),
            outside_English_term_preserved="coastal" in output_text.lower(),
            outside_signature_page_pixels_identical=left.size == right.size
            and ImageChops.difference(left, right).getbbox() is None,
            geometry_preserved=inspect.geometry(before) == inspect.geometry(after),
        )
        rows.append(
            dict(
                file=ocr_path.name,
                status="PASS" if all(checks.values()) else "FAIL",
                checks=checks,
                sha256=fonts.sha(ocr_path),
            )
        )
    result = dict(
        status="PASS" if all(row["status"] == "PASS" for row in rows) else "FAIL",
        cases=rows,
        engines=["pypdf", "PDFium", "Poppler"],
        scope="Fixed synthetic app-copy outputs only; no general security acceptance or native viewer test",
    )
    output.write_text(
        json.dumps(result, ensure_ascii=False, indent=2), encoding="utf-8"
    )
    print(json.dumps(result, ensure_ascii=False))
    if result["status"] != "PASS":
        raise SystemExit(1)


if __name__ == "__main__":
    main()
