"""Independently inspect new signed copies with pypdf, PDFium and Poppler."""

import argparse
import hashlib
import json
import subprocess
from pathlib import Path

import pypdfium2 as pdfium
from pypdf import PdfReader
from pypdf.generic import IndirectObject, StreamObject

from test_tools import poppler_tool


def sha(data):
    return hashlib.sha256(data).hexdigest()


def structure(value):
    # Existing references must retain their identities. Resolve each referenced
    # object separately below rather than following Parent/Pages cycles here.
    if isinstance(value, IndirectObject):
        return ("reference", value.idnum, value.generation)
    if isinstance(value, dict):
        result = {str(k): structure(v) for k, v in value.items()}
        if isinstance(value, StreamObject):
            result["decoded_stream_sha256"] = sha(value.get_data())
        return result
    if isinstance(value, (list, tuple)):
        return [structure(v) for v in value]
    if isinstance(value, bytes):
        return ("bytes", value.hex())
    if value is None or isinstance(value, (str, bool, int, float)):
        return value
    return str(value)


def inspect(before, after):
    source, copy = PdfReader(before), PdfReader(after)
    assert len(source.pages) == len(copy.pages), "Page count changed"
    original = source.get_fields() or {}
    fields = copy.get_fields() or {}
    added = set(fields) - set(original)
    assert len(added) == 1 and fields[next(iter(added))]["/FT"] == "/Sig"
    assert set(original).issubset(fields), "Existing form removed"
    # All old objects survive byte-for-byte in meaning, except the first page,
    # catalog and AcroForm containers that explicitly add the new signature.
    roots = {source.trailer["/Root"].indirect_reference.idnum}
    form = source.trailer["/Root"].get("/AcroForm")
    if isinstance(form, IndirectObject):
        roots.add(form.idnum)
    roots.add(source.pages[0].indirect_reference.idnum)
    checked = 0
    for generation, objects in source.xref.items():
        for number in objects:
            if number == 0 or number in roots:
                continue
            left = source.get_object(IndirectObject(number, generation, source))
            right = copy.get_object(IndirectObject(number, generation, copy))
            assert structure(left) == structure(right), (
                "Existing object changed",
                number,
            )
            checked += 1
    for first, second in zip(source.pages, copy.pages):
        for key in (
            "/MediaBox",
            "/CropBox",
            "/Rotate",
            "/UserUnit",
            "/Contents",
            "/Resources",
        ):
            assert structure(first.get(key)) == structure(second.get(key)), key
        annotations = list(first.get("/Annots", []))
        saved = list(second.get("/Annots", []))
        assert [structure(a) for a in saved[: len(annotations)]] == [
            structure(a) for a in annotations
        ]
    with pdfium.PdfDocument(before) as a, pdfium.PdfDocument(after) as b:
        a.init_forms()
        b.init_forms()
        for i in range(len(a)):
            p, q = a[i], b[i]
            try:
                assert p.get_size() == q.get_size(), "PDFium page size differs"
                x, y = p.get_textpage(), q.get_textpage()
                try:
                    assert (
                        x.get_text_range() == y.get_text_range()
                    ), "PDFium text differs"
                finally:
                    x.close()
                    y.close()
                x, y = p.render(scale=1, draw_annots=True), q.render(
                    scale=1, draw_annots=True
                )
                try:
                    assert (
                        x.to_pil().tobytes() == y.to_pil().tobytes()
                    ), "PDFium form/page pixels differ"
                finally:
                    x.close()
                    y.close()
            finally:
                p.close()
                q.close()
    for page in range(1, len(source.pages) + 1):
        rendered = [
            subprocess.run(
                [
                    str(poppler_tool("pdftoppm")),
                    "-r",
                    "48",
                    "-singlefile",
                    "-f",
                    str(page),
                    "-l",
                    str(page),
                    str(path),
                ],
                capture_output=True,
                check=True,
            ).stdout
            for path in (before, after)
        ]
        assert rendered[0] and rendered[0] == rendered[1], "Poppler page pixels differ"
    return {
        "file": after.name,
        "file_sha256": sha(after.read_bytes()),
        "pages": len(source.pages),
        "retained_fields": len(original),
        "existing_objects_checked": checked,
        "PDFium_identical_pixels_text_and_form_appearances": True,
        "Poppler_identical_page_pixels": True,
        "Poppler_pdfsig": "未実行: bundled Poppler has no pdfsig executable; CMS verified separately with .NET",
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--run", type=Path, required=True)
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    run = args.run.resolve()
    output = args.output or run / "independent-certificate-signing.json"
    if output.exists():
        raise FileExistsError("Preserve earlier inspection")
    result = {
        "status": "FAIL",
        "Reader_GUI": "未実行",
        "chain_revocation_identity_guarantee": "未評価",
    }
    try:
        process = json.loads(
            (run / "process-result.json").read_text(encoding="utf-8-sig")
        )
        assert process["completed"] and process["report_valid"]
        pairs = [
            ("certificate-signing-before.pdf", f"certificate-signed-{kind}.pdf")
            for kind in ("RSA", "EC")
        ]
        pairs += [
            ("certificate-geometry-before.pdf", "certificate-geometry-signed.pdf"),
            ("certificate-ocr-before.pdf", "certificate-ocr-signed.pdf"),
        ]
        result["copies"] = [
            inspect(run / first, run / second) for first, second in pairs
        ]
        assert (
            result["copies"][0]["retained_fields"] >= 10
        ), "Six designed and foreign fields required"
        result["status"] = "PASS"
    except Exception as error:
        result["error"] = str(error)
        raise
    finally:
        output.write_text(
            json.dumps(result, ensure_ascii=False, indent=2), encoding="utf-8"
        )
    print(
        "PASS: four signed copies; old objects, six forms, geometry, OCR, PDFium pixels/text and Poppler pixels"
    )


if __name__ == "__main__":
    main()
