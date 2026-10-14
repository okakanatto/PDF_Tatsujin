"""Compare raw Office exports with compatible PDFs without relaxing text checks."""

import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import tempfile

import pypdfium2 as pdfium


def check(value, why):
    if not value:
        raise AssertionError(why)


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("before", type=Path)
    parser.add_argument("after", type=Path)
    parser.add_argument("--poppler", type=Path, required=True)
    args = parser.parse_args()
    results = []
    for name in ("sheets", "slides"):
        before = args.before / f"office-{name}.pdf"
        after = args.after / f"office-{name}.pdf"
        row = {"case": name, "before_sha256": sha(before), "after_sha256": sha(after)}
        try:
            with pdfium.PdfDocument(before) as a, pdfium.PdfDocument(after) as b:
                check(len(a) == len(b), "same page count")
                page_count = len(a)
                max_difference = 0.0
                for index in range(len(a)):
                    pa, pb = a[index], b[index]
                    ta, tb = pa.get_textpage(), pb.get_textpage()
                    try:
                        check(pa.get_size() == pb.get_size(), "same page dimensions")
                        check(
                            ta.get_text_range() == tb.get_text_range(),
                            "same PDFium text",
                        )
                        check(
                            ta.count_chars() == tb.count_chars(), "same character count"
                        )
                        for char in range(ta.count_chars()):
                            difference = max(
                                abs(x - y)
                                for x, y in zip(
                                    ta.get_charbox(char), tb.get_charbox(char)
                                )
                            )
                            max_difference = max(max_difference, difference)
                            check(difference <= 0.0001, "unchanged glyph coordinates")
                        ia = pa.render(scale=1).to_pil()
                        ib = pb.render(scale=1).to_pil()
                        check(
                            ia.mode == ib.mode and ia.size == ib.size,
                            "same raster format",
                        )
                        check(ia.tobytes() == ib.tobytes(), "identical PDFium pixels")
                    finally:
                        ta.close()
                        tb.close()
                        pa.close()
                        pb.close()
            with tempfile.TemporaryDirectory(
                prefix="compat-", dir=args.after
            ) as temporary:
                root = Path(temporary)
                for label, path in (("before", before), ("after", after)):
                    subprocess.run(
                        [str(args.poppler), "-r", "72", str(path), str(root / label)],
                        capture_output=True,
                        timeout=60,
                        check=True,
                    )
                first = sorted(root.glob("before-*.ppm"))
                second = sorted(root.glob("after-*.ppm"))
                check(
                    len(first) == len(second) == page_count, "same Poppler page count"
                )
                for x, y in zip(first, second):
                    check(sha(x) == sha(y), "identical Poppler pixels")
            row.update(status="PASS", max_glyph_difference_pt=max_difference)
        except Exception as error:
            row.update(status="FAIL", error=str(error))
        results.append(row)
    report = {"cases": results, "failures": sum(r["status"] != "PASS" for r in results)}
    with (args.after / "office-text-compat.json").open("x", encoding="utf-8") as file:
        json.dump(report, file, ensure_ascii=False, indent=2)
    print(json.dumps(report, ensure_ascii=False, indent=2))
    return bool(report["failures"])


if __name__ == "__main__":
    raise SystemExit(main())
