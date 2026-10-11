"""Run the candidate's real OCR worker against frozen public-domain book scans."""

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import unicodedata
import pypdfium2 as pdfium
from PIL import ImageChops


def norm(value):
    return re.sub(r"\s+", " ", unicodedata.normalize("NFC", value)).strip()


def distance(left, right):
    previous = list(range(len(right) + 1))
    for i, character in enumerate(left, 1):
        current = [i]
        for j, other in enumerate(right, 1):
            current.append(
                min(
                    current[-1] + 1,
                    previous[j] + 1,
                    previous[j - 1] + (character != other),
                )
            )
        previous = current
    return previous[-1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--app-directory", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument(
        "--language", choices=["jpn+eng", "jpn", "eng", "jpn_vert"], default="jpn+eng"
    )
    parser.add_argument("--case", choices=["R01", "R02"], action="append")
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=False)
    root = Path(__file__).resolve().parents[1]
    scans = root / "fixtures/real-scans"
    entries = json.loads((scans / "manifest.json").read_text("utf-8"))
    if args.case:
        entries = [entry for entry in entries if entry["id"] in args.case]
    executable = args.app_directory.resolve() / "PDFTatsujin.exe"
    env = os.environ.copy()
    env["QT_QPA_PLATFORM"] = "offscreen"
    env["PATH"] = str(
        Path(env.get("SystemRoot", env.get("SYSTEMROOT", "C:/Windows"))) / "System32"
    )
    env.pop("TATSU_ASSETS", None)
    env.pop("QT_PLUGIN_PATH", None)
    result = {
        "executable_sha256": hashlib.sha256(executable.read_bytes()).hexdigest(),
        "truth_manifest_sha256": hashlib.sha256(
            (scans / "manifest.json").read_bytes()
        ).hexdigest(),
        "scope": "two public-domain book pages; diagnostic only, separate from fixed D03 thresholds",
        "normalization": "NFC and whitespace only",
        "pages": [],
    }
    for entry in entries:
        source = scans / entry["pdf"]
        if hashlib.sha256(source.read_bytes()).hexdigest() != entry["pdf_sha256"]:
            raise RuntimeError("Frozen real-scan PDF changed")
        target = args.output.resolve() / (entry["id"] + "-ocr.pdf")
        options = args.output / (entry["id"] + "-options.json")
        options.write_text(
            json.dumps({"pages": "1", "language": args.language}), "utf-8"
        )
        report = args.output.resolve() / (entry["id"] + "-worker.json")
        process = subprocess.run(
            [
                str(executable),
                "--ocr-worker",
                str(source),
                str(target),
                str(options.resolve()),
                str(report),
            ],
            env=env,
            capture_output=True,
            timeout=180,
        )
        (args.output / (entry["id"] + "-stderr.txt")).write_bytes(process.stderr)
        row = dict(entry, exit_code=process.returncode)
        if process.returncode:
            row["status"] = "FAIL"
        else:
            original = pdfium.PdfDocument(source)
            saved = pdfium.PdfDocument(target)
            textpage = saved[0].get_textpage()
            actual = textpage.get_text_range()
            (args.output / (entry["id"] + "-copied.txt")).write_text(actual, "utf-8")
            expected, copied = norm(entry["truth"]), norm(actual)
            row["diagnostic_CER"] = distance(expected, copied) / len(expected)
            row["search"] = {term: term in actual for term in entry["search_terms"]}
            before = original[0].render(scale=1.5).to_pil().convert("RGB")
            after = saved[0].render(scale=1.5).to_pil().convert("RGB")
            row["visible_content_unchanged"] = (
                ImageChops.difference(before, after).getbbox() is None
            )
            row["worker"] = json.loads(report.read_text("utf-8"))
            row["copied_characters"] = len(actual)
            row["status"] = "DIAGNOSTIC"
            after.save(args.output / (entry["id"] + "-saved.png"))
        result["pages"].append(row)
    (args.output / "real-scans.json").write_text(
        json.dumps(result, ensure_ascii=False, indent=2) + "\n", "utf-8"
    )
    print(
        json.dumps(
            [
                {
                    key: row[key]
                    for key in (
                        "id",
                        "status",
                        "diagnostic_CER",
                        "search",
                        "visible_content_unchanged",
                    )
                    if key in row
                }
                for row in result["pages"]
            ],
            ensure_ascii=False,
            indent=2,
        )
    )


if __name__ == "__main__":
    main()
