"""Verify batch PDFs, OCR and partial outcomes with independent PDF engines."""

import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import re
import unicodedata

from PIL import ImageChops
from pypdf import PdfReader
import pypdfium2 as pdfium

spec = importlib.util.spec_from_file_location(
    "optimization_inspection", Path(__file__).with_name("evaluate-pdf-optimization.py")
)
inspection = importlib.util.module_from_spec(spec)
spec.loader.exec_module(inspection)
check = inspection.check


def normalized(value):
    return re.sub(r"\s+", " ", unicodedata.normalize("NFC", value)).strip()


def distance(a, b):
    previous = list(range(len(b) + 1))
    for i, char in enumerate(a, 1):
        current = [i]
        for j, other in enumerate(b, 1):
            current.append(
                min(current[-1] + 1, previous[j] + 1, previous[j - 1] + (char != other))
            )
        previous = current
    return previous[-1]


def inspect_pages(before, after, *, OCR=False):
    first, second = PdfReader(before), PdfReader(after)
    check(len(first.pages) == len(second.pages), "Page count changed")
    observations = []
    with pdfium.PdfDocument(str(before)) as source, pdfium.PdfDocument(
        str(after)
    ) as saved:
        source.init_forms()
        saved.init_forms()
        for index in range(len(source)):
            old, new = source[index], saved[index]
            scale = float(first.pages[index].get("/UserUnit", 1)) * 0.8
            a, b = old.render(scale=scale, draw_annots=True), new.render(
                scale=scale, draw_annots=True
            )
            check(
                a.width == b.width
                and a.height == b.height
                and ImageChops.difference(
                    a.to_pil().convert("RGB"), b.to_pil().convert("RGB")
                ).getbbox()
                is None,
                "Independent visible pixels changed",
            )
            x, y = old.get_textpage(), new.get_textpage()
            before_text, after_text = x.get_text_range(), y.get_text_range()
            if not OCR or index > 0:
                check(before_text == after_text, "Existing body/form text changed")
            observations.append(
                dict(page=index + 1, pixels_changed=0, body_text=after_text)
            )
            for resource in (x, y, a, b, old, new):
                resource.close()
    return observations


def annotations(reader):
    return [
        [
            (
                str(a.get_object().get("/Subtype")),
                str(a.get_object().get("/Contents", "")),
                list(a.get_object().get("/Rect", [])),
                (
                    bytes(a.get_object()["/Tatsujin"].original_bytes).hex()
                    if "/Tatsujin" in a.get_object()
                    else None
                ),
            )
            for a in page.get("/Annots", [])
        ]
        for page in reader.pages
    ]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("directory", type=Path)
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    run = args.directory.resolve()
    output = args.output.resolve() if args.output else run / "independent-batch.json"
    check(not output.exists(), "Preserve earlier evidence")
    process = json.loads((run / "process-result.json").read_text("utf-8-sig"))
    suite = json.loads((run / "selftest.json").read_bytes())
    tests = [r for r in suite["tests"] if r["name"].startswith("M5T")]
    check(
        process["completed"]
        and process["report_valid"]
        and len(tests) == 4
        and all(r["status"] == "PASS" for r in tests),
        "Require four completed batch execution tests",
    )
    result = dict(status="FAIL", comparisons=[], OCR=[])
    try:
        for name in ("batch-rich", "batch-別の資料"):
            before = run / f"{name}.pdf"
            after = run / f"batch-optimization/{name}_optimized.pdf"
            check(
                after.stat().st_size < before.stat().st_size,
                "Saved optimization is not smaller",
            )
            graph = inspection.compare_graph(PdfReader(before), PdfReader(after))
            pages = inspect_pages(before, after)
            result["comparisons"].append(
                dict(
                    source=before.name,
                    output=after.name,
                    source_SHA256=hashlib.sha256(before.read_bytes()).hexdigest(),
                    output_SHA256=hashlib.sha256(after.read_bytes()).hexdigest(),
                    before_bytes=before.stat().st_size,
                    after_bytes=after.stat().st_size,
                    **graph,
                    pages=pages,
                )
            )
            check(
                PdfReader(after).get_fields()["name"]["/V"] == "髙橋 香織",
                "Japanese form after optimized output",
            )
        root = Path(__file__).resolve().parents[1]
        truth_path = root / "fixtures/ground-truth.json"
        check(
            hashlib.sha256(truth_path.read_bytes()).hexdigest()
            == "7a8878927ec067c80f8b5b42640203beb7d7a99daec17a790d7f5e661e4b15e7",
            "Frozen truth changed",
        )
        truth = json.loads(truth_path.read_bytes())
        for language, index, term, threshold in (
            ("jpn", 2, "市民公園", 0.02),
            ("eng", 6, "coastal", 0.01),
        ):
            before = run / f"batch-scan-{language}.pdf"
            after = run / f"batch-ocr-output/batch-scan-{language}_ocr.pdf"
            old, new = PdfReader(before), PdfReader(after)
            check(
                annotations(old) == annotations(new), "OCR changed retained annotations"
            )
            pages = inspect_pages(before, after, OCR=True)
            text = pages[0]["body_text"]
            expected = normalized(truth["pages"][index]["text"])
            actual = normalized(text)
            cer = distance(expected, actual) / len(expected)
            check(
                cer <= threshold and term in text,
                "Frozen language CER/search threshold",
            )
            if language == "jpn":
                check(
                    new.get_fields()["name"]["/V"] == "一括OCRのフォーム",
                    "Japanese form after batch OCR",
                )
            result["OCR"].append(
                dict(
                    language=language,
                    frozen_source_page=index + 1,
                    CER=cer,
                    maximum_CER=threshold,
                    fixed_search_term=term,
                    source_SHA256=hashlib.sha256(before.read_bytes()).hexdigest(),
                    output_SHA256=hashlib.sha256(after.read_bytes()).hexdigest(),
                    annotations_exact=True,
                    pages=pages,
                )
            )
        for manifest, statuses in (
            ("batch-optimization-results.json", ["保存済み", "保存済み", "処理不要"]),
            (
                "batch-partial-results.json",
                ["失敗", "失敗", "失敗", "失敗", "保存済み"],
            ),
            ("batch-partial-cancel-results.json", ["保存済み", "未処理（中止）"]),
            ("batch-crash-results.json", ["失敗", "処理不要"]),
        ):
            rows = json.loads((run / manifest).read_bytes())
            check(
                [r["status"] for r in rows] == statuses,
                "Fixed per-file result statuses",
            )
        check(
            not (
                run / "batch-optimization/batch-already-optimized_optimized.pdf"
            ).exists(),
            "Unneeded output was published",
        )
        for path in (
            "batch-partial-cancel/batch-scan-eng_ocr.pdf",
            "batch-worker-crash/batch-crash-scan_ocr.pdf",
            "batch-ocr-active-cancel/batch-crash-scan_ocr.pdf",
            "batch-ocr-active-cancel/batch-scan-eng_ocr.pdf",
        ):
            check(not (run / path).exists(), "Cancelled/crashed output was published")
        edited = PdfReader(run / "batch-ocr-edited.pdf")
        check(
            edited.get_fields()["name"]["/V"] == "一括OCRのフォーム",
            "Reediting loses form",
        )
        check("市民公園" in edited.pages[0].extract_text(), "Saved reediting loses OCR")
        result.update(
            status="PASS",
            fixed_result_statuses=True,
            partial_files_preserved=True,
            scope="pypdf reachable PDF meaning and distinct editing dictionaries; PDFium pixels including forms/annotations, body/OCR extraction with frozen CER normalization. Not native IME, OS DPI, Reader or physical printing.",
        )
    except Exception as error:
        result["error"] = str(error)
        raise
    finally:
        output.write_text(
            json.dumps(result, ensure_ascii=False, indent=2) + "\n", encoding="utf-8"
        )
    print(
        json.dumps(
            dict(
                status="PASS",
                comparisons=2,
                OCR={r["language"]: r["CER"] for r in result["OCR"]},
            )
        )
    )


if __name__ == "__main__":
    main()
