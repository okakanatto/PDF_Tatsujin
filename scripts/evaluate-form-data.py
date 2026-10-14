"""Independent XFDF values, PDF fields/appearances, body and OCR preservation."""

import argparse
import json
from pathlib import Path
import xml.etree.ElementTree as ET

from PIL import ImageChops
from pypdf import PdfReader
from pypdf.generic import NameObject
import pypdfium2 as pdfium

NS = "{http://ns.adobe.com/xfdf/}"
EXPECTED = {
    "name": ["髙橋 香織"],
    "notes": ["東京都\n申請内容 & <追記>"],
    "agree": ["Off"],
    "choice": ["B"],
    "combo": ["Red"],
    "list": ["West"],
}


def check(value, message):
    if not value:
        raise RuntimeError(message)


def xml_values(path):
    root = ET.parse(path).getroot()
    check(root.tag == NS + "xfdf", "Incorrect XFDF namespace")
    fields = root.find(NS + "fields")
    result = {}
    for field in fields:
        check(field.tag == NS + "field", "Unexpected XML element")
        name = field.attrib["name"]
        check(name not in result, "Duplicate XML field")
        result[name] = [value.text or "" for value in field.findall(NS + "value")]
    return result


def values(reader):
    result = {}
    for name, field in reader.get_fields().items():
        value = field.get("/V")
        parts = value if isinstance(value, list) else ([] if value is None else [value])
        result[name] = [
            str(part)[1:] if isinstance(part, NameObject) else str(part)
            for part in parts
        ]
    return result


def body_unchanged(before, after):
    a, b = PdfReader(before), PdfReader(after)
    check(len(a.pages) == len(b.pages), "Page count changed")
    for old, new in zip(a.pages, b.pages):
        check(
            old.get_contents().get_data() == new.get_contents().get_data(),
            "Body stream changed",
        )
        for key in ("/MediaBox", "/CropBox", "/Rotate", "/UserUnit"):
            check(old.get(key) == new.get(key), "Page geometry changed")
        old_annots = [
            ref.get_object()
            for ref in old.get("/Annots", [])
            if ref.get_object().get("/Subtype") != "/Widget"
        ]
        new_annots = [
            ref.get_object()
            for ref in new.get("/Annots", [])
            if ref.get_object().get("/Subtype") != "/Widget"
        ]
        check(len(old_annots) == len(new_annots), "Non-form annotations changed")
        for x, y in zip(old_annots, new_annots):
            for key in ("/Subtype", "/Rect", "/Tatsujin", "/Contents", "/Dest"):
                check(
                    x.get(key) == y.get(key), "Signature/annotation attributes changed"
                )
            if "/AP" in x:
                check(
                    x["/AP"]["/N"].get_data() == y["/AP"]["/N"].get_data(),
                    "Existing appearance changed",
                )


def scan_exact(before, after):
    with pdfium.PdfDocument(str(before)) as a, pdfium.PdfDocument(str(after)) as b:
        for number in (0, 1):
            old, new = a[number], b[number]
            old_image, new_image = old.render(scale=0.8, draw_annots=True), new.render(
                scale=0.8, draw_annots=True
            )
            check(
                ImageChops.difference(
                    old_image.to_pil().convert("RGB"), new_image.to_pil().convert("RGB")
                ).getbbox()
                is None,
                "OCR scan pixels changed",
            )
            x, y = old.get_textpage(), new.get_textpage()
            check(
                x.get_text_range() == y.get_text_range(), "OCR search/copy text changed"
            )
            for resource in (x, y, old_image, new_image, old, new):
                resource.close()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("directory", type=Path)
    args = parser.parse_args()
    run = args.directory
    target = run / "independent-form-data.json"
    check(not target.exists(), "Preserve previous results")
    result = {
        "status": "FAIL",
        "engines": ["Python ElementTree", "pypdf", "PDFium"],
        "native_Reader_XFDF_import": "未実行",
    }
    try:
        for name in (
            "form-data-input.xfdf",
            "form-data-export.xfdf",
            "form-data-ui-export.xfdf",
        ):
            check(
                xml_values(run / name) == EXPECTED,
                "Independent XML values differ: " + name,
            )
        check(
            xml_values(run / "form-data-special.xfdf")
            == {"空白 & <名>": [" 前後 \r\n改行\r終端\t"]},
            "CR/whitespace/XML escape roundtrip differs",
        )
        for name in ("form-data-after.pdf", "form-data-ui.pdf"):
            check(
                values(PdfReader(run / name)) == EXPECTED,
                "Independent PDF values differ: " + name,
            )
        changed = dict(EXPECTED, name=["再編集した氏名"])
        check(
            values(PdfReader(run / "form-data-reedited.pdf")) == changed,
            "Reedited field or neighbors incorrect",
        )
        aliases = PdfReader(run / "form-data-aliases.pdf").get_fields()
        check(
            aliases["name"]["/TU"] == aliases["notes"]["/TU"] == "同じ表示名",
            "Alias setup missing",
        )
        check(
            aliases["name"]["/V"] == "識別した氏名"
            and aliases["notes"]["/V"] == "別の項目",
            "Caption redirected values",
        )
        check(
            values(PdfReader(run / "form-data-multiple.pdf"))["list"]
            == ["North", "West"],
            "Multiple choice values differ",
        )
        check(
            values(PdfReader(run / "form-data-hierarchy.pdf"))["person.child"]
            == ["階層の値"],
            "Hierarchy data did not reach field",
        )
        body_unchanged(run / "form-data-before.pdf", run / "form-data-after.pdf")
        body_unchanged(
            run / "form-data-ocr-before.pdf", run / "form-data-ocr-after.pdf"
        )
        scan_exact(run / "form-data-ocr-before.pdf", run / "form-data-ocr-after.pdf")
        ocr = values(PdfReader(run / "form-data-ocr-after.pdf"))
        check(
            ocr["name"] == ["髙橋 香織"] and ocr["notes"] == ["OCRと入力データの併用"],
            "Form values alongside OCR lost",
        )
        check(
            xml_values(run / "form-data-ocr.xfdf") == ocr,
            "Combined OCR export mismatch",
        )
        permitted = PdfReader(run / "form-data-copy-permitted.pdf")
        check(
            permitted.is_encrypted and permitted.decrypt("form-user-2026") == 1,
            "Protected copy role missing",
        )
        check(
            xml_values(run / "form-data-copy-permitted.xfdf") == values(permitted),
            "Explicit permitted data export mismatch",
        )
        with pdfium.PdfDocument(str(run / "form-data-after.pdf")) as doc:
            page = doc[0]
            bitmap = page.render(scale=1.2, draw_annots=True)
            bitmap.to_pil().save(run / "form-data-independent-render.png")
            bitmap.close()
            page.close()
        result.update(
            status="PASS",
            six_field_kinds=True,
            hierarchical_names=True,
            duplicate_captions=True,
            multiple_choice=True,
            CR_XML_whitespace_exact=True,
            body_and_signatures_unchanged=True,
            OCR_scan_pixel_difference=0,
            OCR_text_exact=True,
        )
    except Exception as error:
        result["error"] = str(error)
    target.write_text(
        json.dumps(result, ensure_ascii=False, indent=2), encoding="utf-8"
    )
    print(
        json.dumps(
            {
                "status": result["status"],
                "error": result.get("error"),
                "output": str(target),
            },
            ensure_ascii=False,
        )
    )
    if result["status"] != "PASS":
        raise SystemExit(1)


if __name__ == "__main__":
    main()
