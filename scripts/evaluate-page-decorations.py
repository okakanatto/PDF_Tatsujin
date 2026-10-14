"""Independently inspect saved vector decorations, rotation, Unicode and alpha."""

import argparse
import hashlib
import json
import math
from pathlib import Path
import subprocess

from PIL import Image, ImageChops
from pypdf import PdfReader, PdfWriter
from pypdf.generic import DictionaryObject, NameObject, DecodedStreamObject
import pypdfium2 as pdfium

from test_tools import poppler_tool


def check(value, message):
    if not value:
        raise RuntimeError(message)


def owned(page):
    result = []
    for ref in page.get("/Annots", []):
        annotation = ref.get_object()
        if "/TatsujinDecoration" in annotation:
            metadata = json.loads(annotation["/TatsujinDecoration"].original_bytes)
            result.append((annotation, metadata))
    return result


def retained(before, after):
    check(len(before.pages) == len(after.pages), "Page count changed")
    for old, new in zip(before.pages, after.pages):
        for key in ("/MediaBox", "/CropBox", "/Rotate", "/UserUnit"):
            check(old.get(key) == new.get(key), f"Geometry changed: {key}")
        content = old.get_contents()
        expected = content.get_data() if content is not None else b""
        content = new.get_contents()
        actual = content.get_data() if content is not None else b""
        check(expected and actual == expected, "Nonempty body was changed or flattened")
        check(old.extract_text() == new.extract_text(), "Body text changed")
        # Existing widgets can have no NM: compare their complete dictionaries
        # by matching original indirect reference numbers, which are preserved.
        by_ref = {ref.idnum: ref.get_object() for ref in new.get("/Annots", [])}
        for ref in old.get("/Annots", []):
            annotation = ref.get_object()
            check(ref.idnum in by_ref, "Existing annotation deleted")
            other = by_ref[ref.idnum]
            for key in ("/Subtype", "/Rect", "/Contents", "/Tatsujin", "/V", "/FT"):
                check(
                    annotation.get(key) == other.get(key), f"Annotation changed: {key}"
                )
    old_fields, new_fields = before.get_fields() or {}, after.get_fields() or {}
    check(
        {name: str(value.get("/V", "")) for name, value in old_fields.items()}
        == {name: str(value.get("/V", "")) for name, value in new_fields.items()},
        "Form values changed",
    )


def isolate(page, kind, target):
    writer = PdfWriter()
    clone = writer.add_page(page)
    annotation, metadata = next(
        (value, meta)
        for value, meta in owned(clone)
        if meta["settings"]["kind"] == kind
    )
    appearance = annotation["/AP"]["/N"]
    check(appearance.get("/Subtype") == "/Form", "Missing standard vector appearance")
    fonts = appearance["/Resources"]["/Font"]
    check(fonts, "No embedded font")
    for ref in fonts.values():
        font = ref.get_object()
        check("/ToUnicode" in font, "Unicode mapping missing")
        for descendant in font.get("/DescendantFonts", [font]):
            descriptor = descendant.get_object()["/FontDescriptor"]
            check(
                any(
                    key in descriptor
                    for key in ("/FontFile", "/FontFile2", "/FontFile3")
                ),
                "Font program missing",
            )
    if kind == 1:
        states = appearance["/Resources"].get("/ExtGState", {})
        check(
            any(
                abs(
                    float(ref.get_object().get("/ca", 1))
                    - metadata["settings"]["opacity"]
                )
                < 1e-6
                for ref in states.values()
            ),
            "Saved watermark alpha is wrong",
        )
    del clone["/Annots"]
    clone[NameObject("/Resources")] = DictionaryObject(
        {
            NameObject("/XObject"): DictionaryObject(
                {NameObject("/Decor"): writer._add_object(appearance)}
            )
        }
    )
    x0, y0, _, _ = page.cropbox
    commands = DecodedStreamObject()
    commands.set_data(f"q 1 0 0 1 {x0} {y0} cm /Decor Do Q\n".encode("ascii"))
    clone[NameObject("/Contents")] = writer._add_object(commands)
    writer.write(target)


def external_images(path, unit, scale):
    prefix = path.with_suffix("")
    subprocess.run(
        [
            str(poppler_tool("pdftoppm")),
            "-cropbox",
            "-r",
            str(72 * scale * unit),
            "-singlefile",
            "-png",
            str(path),
            str(prefix),
        ],
        check=True,
        capture_output=True,
        creationflags=subprocess.CREATE_NO_WINDOW,
    )
    poppler = Image.open(prefix.with_suffix(".png")).convert("RGB")
    document = pdfium.PdfDocument(str(path))
    page = document[0]
    bitmap = page.render(scale=scale * unit, draw_annots=True)
    image = bitmap.to_pil().convert("RGB")
    image.save(prefix.with_name(prefix.name + "-pdfium").with_suffix(".png"))
    textpage = page.get_textpage()
    text = textpage.get_text_range()
    # The raw API reports glyphs in unrotated PDF coordinates. PDFium inserts
    # layout CR/LF and inferred spaces on counter-rotated text. Keep those in
    # `text` for the diagnostic, but inspect actual glyph Unicode and boxes
    # together rather than indexing the formatted string as a glyph array.
    glyphs = [
        (chr(pdfium.raw.FPDFText_GetUnicode(textpage, i)), textpage.get_charbox(i))
        for i in range(textpage.count_chars())
        if pdfium.raw.FPDFText_IsGenerated(textpage, i) == 0
    ]
    textpage.close()
    bitmap.close()
    page.close()
    document.close()
    return image, poppler, text, glyphs


def ink(image):
    return image.convert("L").point(lambda value: 255 if value < 245 else 0).getbbox()


def colored_ink(image, kind):
    red, green, blue = image.convert("RGB").split()
    if kind == 0:
        masks = (
            red.point(lambda v: 255 if v < 80 else 0),
            green.point(lambda v: 255 if v < 150 else 0),
            blue.point(lambda v: 255 if v > 140 else 0),
        )
    else:
        masks = (
            ImageChops.subtract(red, green).point(lambda v: 255 if v > 10 else 0),
            ImageChops.difference(green, blue).point(lambda v: 255 if v < 12 else 0),
        )
    result = masks[0]
    for mask in masks[1:]:
        result = ImageChops.multiply(result, mask)
    return result.getbbox()


def visual_point(page, x, y):
    x0, y0, x1, y1 = (float(value) for value in page.cropbox)
    unit = float(page.get("/UserUnit", 1))
    rotation = int(page.get("/Rotate", 0)) % 360
    point = {
        0: (x - x0, y1 - y),
        90: (y - y0, x - x0),
        180: (x1 - x, y - y0),
        270: (y1 - y, x1 - x),
    }[rotation]
    return tuple(value * unit for value in point)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("directory", type=Path)
    directory = parser.parse_args().directory.resolve()
    target = directory / "independent-page-decorations.json"
    check(not target.exists(), "Preserve earlier verification")
    process = json.loads((directory / "process-result.json").read_text("utf-8-sig"))
    suite = json.loads((directory / "selftest.json").read_text("utf-8-sig"))
    cases = [row for row in suite["tests"] if row["name"].startswith("M4D")]
    check(
        process["completed"]
        and process["report_valid"]
        and process["exit_code"] == 0
        and len(cases) == 5
        and all(row["status"] == "PASS" for row in cases),
        "Five decoration workflows must pass",
    )
    before = PdfReader(directory / "decoration-before.pdf")
    both = PdfReader(directory / "decoration-both.pdf")
    retained(before, both)
    retained(
        PdfReader(directory / "decoration-form-before.pdf"),
        PdfReader(directory / "decoration-form.pdf"),
    )
    check(
        str(PdfReader(directory / "decoration-form.pdf").get_fields()["name"]["/V"])
        == "髙橋 香織",
        "Japanese form value lost",
    )
    rows = []
    product_renders = []
    scale = 2
    for index, page in enumerate(both.pages):
        single = directory / f"decoration-whole-{index + 1}.pdf"
        writer = PdfWriter()
        writer.add_page(page)
        writer.write(single)
        image, poppler, _, _ = external_images(
            single, float(page.get("/UserUnit", 1)), 1.5
        )
        qt = Image.open(directory / f"decoration-qt-{index + 1}.png").convert("RGB")
        for kind in (0, 1):
            bounds = [colored_ink(value, kind) for value in (qt, image, poppler)]
            check(all(bounds), "Actual saved annotation or product render is invisible")
            error = max(
                abs(a - b) for box in bounds[1:] for a, b in zip(bounds[0], box)
            )
            check(
                error <= 2,
                "Actual Qt/PDFium/Poppler annotation ink bounds differ by more than 2px",
            )
            product_renders.append(
                {
                    "page": index + 1,
                    "kind": kind,
                    "Qt_PDFium_Poppler_bounds": bounds,
                    "max_difference_px": error,
                    "status": "PASS",
                }
            )
        annotations = owned(page)
        check(len(annotations) == 2, "Two owned groups expected")
        for annotation, meta in annotations:
            check(
                list(annotation["/Rect"]) == list(page.cropbox),
                "Standard annotation rectangle must match its page appearance",
            )
            check(
                meta["version"] == 1 and int(annotation["/F"]) & 4,
                "Version or print flag lost",
            )
            options = meta["settings"]
            kind = options["kind"]
            expected_text = "社外秘 SAMPLE" if kind else f"{7 + index} / 4"
            check(
                expected_text in str(annotation["/Contents"]),
                "Fixed page number or Japanese text wrong",
            )
            isolated = directory / f"decoration-isolated-{index + 1}-{kind}.pdf"
            isolate(page, kind, isolated)
            image, poppler, text, glyphs = external_images(
                isolated, float(page.get("/UserUnit", 1)), scale
            )
            painted_text = "".join(character for character, _ in glyphs)
            expected_glyphs = (
                "社外秘SAMPLE"
                if kind
                else f"日本語Header中央資料作成済{7 + index}/4END"
            )
            check(
                painted_text.replace(" ", "") == expected_glyphs,
                "Exact actual glyph Unicode sequence is wrong",
            )
            mask, other = ink(image), ink(poppler)
            check(mask and other, "External decoration is invisible")
            error = max(abs(a - b) for a, b in zip(mask, other))
            check(error <= 2, "Independent renderer ink bounds differ by more than 2px")
            # PDFium text boxes are in unrotated PDF coordinates. Transform them
            # independently using the PDF dictionary, not the product helper.
            centers = [
                visual_point(page, (box[0] + box[2]) / 2, (box[1] + box[3]) / 2)
                for _, box in glyphs
            ]
            if kind == 0:
                phrase = "日本語Header"
                # Explicit space glyphs, when present, retain their own index.
                offset = painted_text.index("日本語")
                last_index = painted_text.index("Header") + len("Header") - 1
                first, last = centers[offset], centers[last_index]
                check(
                    last[0] > first[0] and abs(last[1] - first[1]) < 4,
                    "Header reads sideways/backwards after page rotation",
                )
                check(
                    4 * 72 / 25.4 <= first[0] <= 4 * 72 / 25.4 + 9
                    and 6 * 72 / 25.4 <= first[1] <= 6 * 72 / 25.4 + 18,
                    "Header physical margin wrong",
                )
                offset = painted_text.index(str(7 + index))
                last_index = painted_text.index("4", offset)
                first, last = centers[offset], centers[last_index]
                check(
                    last[0] > first[0] and abs(last[1] - first[1]) < 4,
                    "Footer number reading direction wrong",
                )
                angle = 0
            else:
                offset = painted_text.index("SAMPLE")
                first, last = centers[offset], centers[offset + 5]
                angle = math.degrees(math.atan2(last[1] - first[1], last[0] - first[0]))
                check(
                    abs(angle + 30) <= 4,
                    "Watermark angle is not in visual-page coordinates",
                )
                extrema = image.getextrema()
                check(
                    210 <= extrema[0][0] <= 245 and 180 <= extrema[1][0] <= 205,
                    "Rendered watermark transparency is wrong",
                )
            rows.append(
                {
                    "page": index + 1,
                    "rotation": int(page.get("/Rotate", 0)),
                    "UserUnit": float(page.get("/UserUnit", 1)),
                    "kind": kind,
                    "Japanese_Unicode": "PASS",
                    "ink_bounds_max_difference_px": error,
                    "visual_angle_degrees": angle,
                    "status": "PASS",
                }
            )
    reedited = PdfReader(directory / "decoration-reedited.pdf")
    for index, page in enumerate(reedited.pages):
        groups = owned(page)
        check(
            len(groups) == (2 if index in (1, 3) else 1),
            "Saved reedit changed target pages",
        )
        for _, meta in groups:
            if meta["settings"]["kind"] == 0:
                check(
                    meta["settings"]["header"][0] == "再編集済み",
                    "Reedited group settings lost",
                )
    reordered = PdfReader(directory / "decoration-reordered.pdf")
    for new_index, old_index in enumerate((3, 1, 0, 2)):
        header = next(
            annotation
            for annotation, meta in owned(reordered.pages[new_index])
            if meta["settings"]["kind"] == 0
        )
        check(
            f"{7 + old_index} / 4" in str(header["/Contents"]),
            "Page reordering silently renumbered a saved header",
        )
    scan_before = pdfium.PdfDocument(str(directory / "decoration-scan-before.pdf"))
    scan = pdfium.PdfDocument(str(directory / "decoration-scan-ocr.pdf"))
    ocr = []
    try:
        for index, term in enumerate(("市民公園", "coastal")):
            old_page, page = scan_before[index], scan[index]
            old_bitmap, bitmap = old_page.render(
                scale=0.8, draw_annots=True
            ), page.render(scale=0.8, draw_annots=True)
            check(
                ImageChops.difference(
                    old_bitmap.to_pil().convert("RGB"), bitmap.to_pil().convert("RGB")
                ).getbbox()
                is None,
                "Independent OCR changed visible decorations",
            )
            textpage = page.get_textpage()
            check(
                term in textpage.get_text_range(), "Saved OCR search/copy text missing"
            )
            ocr.append(
                {
                    "page": index + 1,
                    "fixed_term": term,
                    "visible_difference": 0,
                    "status": "PASS",
                }
            )
            textpage.close()
            old_bitmap.close()
            bitmap.close()
            old_page.close()
            page.close()
    finally:
        scan_before.close()
        scan.close()
    opacity_rows = []
    for percent in (5, 20, 50, 100):
        reader = PdfReader(directory / f"decoration-opacity-{percent}.pdf")
        _, metadata = owned(reader.pages[0])[0]
        check(
            metadata["settings"]["opacity"] == percent / 100, "Stored opacity changed"
        )
        isolated = directory / f"decoration-opacity-isolated-{percent}.pdf"
        isolate(reader.pages[0], 1, isolated)
        image, _, _, _ = external_images(
            isolated, float(reader.pages[0].get("/UserUnit", 1)), 1
        )
        check(
            image.convert("L").getextrema()[0] < 253, "Opacity boundary render is blank"
        )
        opacity_rows.append(
            {
                "percent": percent,
                "exact_pdf_alpha": "PASS",
                "PDFium_Poppler_render": "PASS",
            }
        )
    result = {
        "status": "PASS",
        "executable_sha256": process["exe_sha256"],
        "saved_pdf_sha256": hashlib.sha256(
            (directory / "decoration-both.pdf").read_bytes()
        ).hexdigest(),
        "rotated_appearances": rows,
        "actual_product_and_external_renders": product_renders,
        "OCR": ocr,
        "opacity_boundaries": opacity_rows,
        "scope": "pypdf structure/fonts, independent PDFium/Poppler pixels and PDFium Unicode/reading direction. Not Reader GUI or native clipboard/physical printing.",
    }
    target.write_text(
        json.dumps(result, ensure_ascii=False, indent=2) + "\n", encoding="utf-8"
    )
    print(json.dumps(result, ensure_ascii=False))


if __name__ == "__main__":
    main()
