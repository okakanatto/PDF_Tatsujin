"""Compare vertical OCR visible pages with an independent Poppler renderer."""

import argparse
import hashlib
import json
from pathlib import Path
import subprocess

from test_tools import poppler_tool

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("run", type=Path)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()
    tool = poppler_tool("pdftoppm")
    version = subprocess.run([str(tool), "-v"], capture_output=True, check=True)
    fixed = json.loads(
        (ROOT / "fixtures/vertical-ocr-criteria.json").read_text("utf-8")
    )
    source = ROOT / "fixtures" / fixed["source"]
    assert hashlib.sha256(source.read_bytes()).hexdigest() == fixed["source_sha256"]

    def render(path, page):
        result = subprocess.run(
            [
                str(tool),
                "-f",
                str(page),
                "-l",
                str(page),
                "-r",
                "72",
                "-hide-annotations",
                "-singlefile",
                str(path),
            ],
            capture_output=True,
            check=True,
            timeout=60,
        )
        assert result.stdout.startswith(b"P6"), "Actual PPM pixels required"
        return result.stdout

    original = [render(source, page) for page in range(1, 6)]
    cases = []
    for name in ("vertical-ui-saved.pdf", "vertical-ui-reedited.pdf"):
        path = args.run / name
        equal = [render(path, page) == original[page - 1] for page in range(1, 6)]
        cases.append(
            dict(
                file=name,
                sha256=hashlib.sha256(path.read_bytes()).hexdigest(),
                all_five_pages_identical=all(equal),
                pages_identical=equal,
                status="PASS" if all(equal) else "FAIL",
            )
        )
    result = dict(
        engine=(version.stderr + version.stdout).decode("utf-8", "replace"),
        cases=cases,
        failures=sum(row["status"] != "PASS" for row in cases),
        annotations=False,
        dpi=72,
        poppler_text_search_bounds="未実行（pdftotextがこの環境にない）",
        native_external_viewer_GUI="未実行",
    )
    with args.output.open("x", encoding="utf-8") as target:
        json.dump(result, target, ensure_ascii=False, indent=2)
        target.write("\n")
    print(json.dumps(result, ensure_ascii=False, indent=2))
    raise SystemExit(bool(result["failures"]))


if __name__ == "__main__":
    main()
