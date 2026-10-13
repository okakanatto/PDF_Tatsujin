"""Fetch runtime assets at pinned revisions, verifying size and SHA-256 before install."""

import argparse
import hashlib
import json
from pathlib import Path
import urllib.request

ROOT = Path(__file__).resolve().parents[1]


def matches(path, expected):
    return (
        path.is_file()
        and path.stat().st_size == expected["bytes"]
        and hashlib.sha256(path.read_bytes()).hexdigest() == expected["sha256"]
    )


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--verify-only", action="store_true")
    args = parser.parse_args()
    lock = json.loads((ROOT / "dependency-lock.json").read_text(encoding="utf-8"))
    models = lock["models"]
    base = "https://raw.githubusercontent.com"
    urls = {
        "assets/fonts/NotoSansJP.ttf": (
            f"{base}/google/fonts/{models['fontRevision']}/ofl/notosansjp/NotoSansJP%5Bwght%5D.ttf"
        ),
        "assets/tessdata/pdf.ttf": f"{base}/tesseract-ocr/tesseract/5.5.1/tessdata/pdf.ttf",
    }
    for language in ("jpn", "eng", "jpn_vert"):
        urls[f"assets/tessdata/{language}.traineddata"] = (
            f"{base}/tesseract-ocr/tessdata_best/{models['ocrModelRevision']}/{language}.traineddata"
        )
    for name, url in urls.items():
        path = ROOT / name
        expected = lock["files"][name]
        if matches(path, expected):
            print(f"Verified {name}")
            continue
        if args.verify_only:
            raise RuntimeError(f"Missing or modified asset: {name}")
        if path.exists():
            raise RuntimeError(f"Refusing to replace a modified asset: {name}")
        path.parent.mkdir(parents=True, exist_ok=True)
        temporary = path.with_suffix(path.suffix + ".download")
        # Exclusive creation also prevents two setup processes from sharing a partial file.
        with temporary.open("xb") as output:
            try:
                with urllib.request.urlopen(url, timeout=60) as response:
                    received = 0
                    while chunk := response.read(1024 * 1024):
                        received += len(chunk)
                        if received > expected["bytes"]:
                            raise RuntimeError(f"Unexpected download size: {name}")
                        output.write(chunk)
            except BaseException:
                output.close()
                temporary.unlink()
                raise
        if not matches(temporary, expected):
            temporary.unlink()
            raise RuntimeError(f"Asset hash mismatch: {name}")
        temporary.rename(path)
        print(f"Installed {name}")


if __name__ == "__main__":
    main()
