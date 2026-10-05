"""Independently inspect PDFs saved through the Windows native M1 UI.

This verifies saved artifacts and recorded clipboard samples, not execution of
the native UI itself. Native observations remain a separate evidence record.
"""

import argparse
import hashlib
import json
import re
import subprocess
import unicodedata
from contextlib import closing
from pathlib import Path

import pypdfium2 as pdfium
from PIL import ImageChops
from pypdf import PdfReader

from test_tools import poppler_tool

ROOT = Path(__file__).resolve().parents[1]
SIGNATURE = "山田 太郎\n髙橋\n2026年10月4日"


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def norm(text):
    return re.sub(r"\s+", " ", unicodedata.normalize("NFC", text)).strip()


def distance(a, b):
    previous = list(range(len(b) + 1))
    for i, x in enumerate(a, 1):
        current = [i]
        for j, y in enumerate(b, 1):
            current.append(
                min(
                    current[-1] + 1,
                    previous[j] + 1,
                    previous[j - 1] + (x != y),
                )
            )
        previous = current
    return previous[-1]


def signature(path):
    reader = PdfReader(path)
    stamps = []
    for page in reader.pages:
        for ref in page.get("/Annots", []):
            annotation = ref.get_object()
            if "/Tatsujin" in annotation:
                metadata = annotation["/Tatsujin"]
                raw = (
                    metadata.original_bytes
                    if hasattr(metadata, "original_bytes")
                    else bytes(metadata)
                )
                stamps.append(
                    {
                        "metadata": json.loads(raw),
                        "contents": str(annotation["/Contents"]),
                        "rect": [float(v) for v in annotation["/Rect"]],
                        "print_flag": bool(int(annotation["/F"]) & 4),
                        "appearance": bool(annotation["/AP"]["/N"].get_data()),
                    }
                )
    if len(stamps) != 1:
        raise ValueError(f"Expected one editable signature in {path.name}")
    return stamps[0]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("directory", type=Path)
    parser.add_argument("--app", required=True, type=Path)
    parser.add_argument(
        "--signature",
        default="native-signature.pdf",
        choices=["native-signature.pdf", "native-signature-corrected.pdf"],
        help="Select the original or corrected UI output; expected text is unchanged",
    )
    parser.add_argument("--result", default="independent-verification.json")
    args = parser.parse_args()
    out = args.directory.resolve()
    observations = json.loads((out / "native-observations.json").read_text("utf-8"))
    truth = json.loads((ROOT / "fixtures/ground-truth.json").read_text("utf-8"))
    manifest = json.loads((ROOT / "fixtures/manifest.json").read_text("utf-8"))
    checks = []

    def check(name, passed, **detail):
        checks.append({"id": name, "status": "PASS" if passed else "FAIL", **detail})

    check(
        "final_executable",
        sha(args.app) == observations["exe_sha256"],
        sha256=sha(args.app),
    )
    inputs = []
    for name in ["D01.pdf", "D03.pdf"]:
        expected = next(m["sha256"] for m in manifest if m["file"] == name)
        actual = sha(ROOT / "fixtures" / name)
        check("source_unchanged_" + name, actual == expected, sha256=actual)
        inputs.append({"file": "fixtures/" + name, "sha256": actual})

    files = [
        args.signature,
        "native-signature-reopened-reedited.pdf",
        "native-same-window-ocr.pdf",
    ]
    stamps = []
    for name in files:
        stamp = signature(out / name)
        stamps.append(stamp)
        expected_text = SIGNATURE + ("\n確認済み" if "reopened" in name else "")
        expected_size = 28 if "reopened" in name else 20
        check(
            "signature_" + name,
            stamp["metadata"]["text"] == expected_text
            and stamp["contents"] == expected_text
            and stamp["metadata"]["size"] == expected_size
            and stamp["metadata"]["version"] == 1
            and stamp["print_flag"]
            and stamp["appearance"],
            **stamp,
        )
    check("reedited_signature_moved", stamps[0]["rect"][:2] != stamps[1]["rect"][:2])

    copies = {
        c.get("id", c.get("test")): c for c in observations["checks"] if "actual" in c
    }
    for language, page in [("jpn", 3), ("eng", 7)]:
        actual = copies["A05_native_copy_" + language]["actual"]
        expected = truth["pages"][page - 1]["text"].split("\n")[0]
        check(
            "native_clipboard_sample_" + language,
            actual == expected,
            reference_page=page,
            characters=len(actual),
            expected=expected,
            actual=actual,
            scope="One native-selected line; not a whole-document CER measurement",
        )

    document = pdfium.PdfDocument(out / files[2])
    original = pdfium.PdfDocument(ROOT / "fixtures/D03.pdf")
    pages = []
    for i, reference in enumerate(truth["pages"]):
        with closing(document[i]) as page:
            with closing(page.get_textpage()) as textpage:
                text = textpage.get_text_range()
            a, b = norm(reference["text"]), norm(text)
            errors = distance(a, b)
            pages.append(
                {
                    "page": i + 1,
                    "language": reference["language"],
                    "split": reference["split"],
                    "reference_characters": len(a),
                    "edit_distance": errors,
                    "CER": errors / len(a),
                    "copy_text": text,
                }
            )
            with closing(page.render(scale=1, draw_annots=False)) as bitmap:
                actual_image = bitmap.to_pil().convert("RGB")
            with closing(original[i]) as original_page:
                with closing(
                    original_page.render(scale=1, draw_annots=False)
                ) as bitmap:
                    expected_image = bitmap.to_pil().convert("RGB")
            diff = ImageChops.difference(expected_image, actual_image)
            red, green, blue = diff.split()
            maximum = ImageChops.lighter(ImageChops.lighter(red, green), blue)
            changed = diff.width * diff.height - maximum.histogram()[0]
            check(
                f"OCR_visible_page_{i+1}",
                changed == 0,
                changed_pixels=changed,
                renderer="PDFium, scale=1, draw_annots=False",
                scope="Exclude added signature annotations to isolate OCR body changes",
            )
    for language, limit in [("jpn", 0.02), ("eng", 0.01)]:
        evaluation = [
            p for p in pages if p["language"] == language and p["split"] == "evaluation"
        ]
        errors = sum(p["edit_distance"] for p in evaluation)
        characters = sum(p["reference_characters"] for p in evaluation)
        cer = errors / characters
        check("CER_" + language, cer <= limit, CER=cer, threshold=limit)
        terms = []
        for term in truth["search_terms"][language]:
            matches = []
            for index in range(len(document)):
                with closing(document[index]) as page, closing(
                    page.get_textpage()
                ) as textpage:
                    with closing(textpage.search(term)) as search:
                        if search.get_next() is not None:
                            matches.append(index + 1)
            terms.append({"term": term, "pages": matches})
        found = sum(bool(t["pages"]) for t in terms)
        check(
            "fixed_search_" + language, found >= 19, found=found, total=20, terms=terms
        )
    document.close()
    original.close()

    poppler = poppler_tool("pdftoppm")
    renders = []
    for name, page in [
        (files[0], 1),
        (files[1], 1),
        (files[2], 1),
        (files[2], 3),
        (files[2], 7),
    ]:
        prefix = out / (Path(name).stem + f"-page-{page}-poppler")
        subprocess.run(
            [
                str(poppler),
                "-f",
                str(page),
                "-l",
                str(page),
                "-singlefile",
                "-scale-to",
                "1200",
                "-png",
                str(out / name),
                str(prefix),
            ],
            check=True,
            capture_output=True,
        )
        renders.append(prefix.with_suffix(".png").name)

    result = {
        "date": observations["date"],
        "scope": "Independent saved-PDF verification; native UI observations are separate",
        "exe_sha256": sha(args.app),
        "script_sha256": sha(Path(__file__)),
        "observations_sha256": sha(out / "native-observations.json"),
        "ground_truth_sha256": sha(ROOT / "fixtures/ground-truth.json"),
        "engines": {
            "PDFium": str(pdfium.PDFIUM_INFO),
            "Poppler": subprocess.run(
                [str(poppler), "-v"], capture_output=True, text=True
            ).stderr.strip(),
        },
        "inputs": inputs,
        "outputs": [
            {
                "file": name,
                "sha256": sha(out / name),
                "bytes": (out / name).stat().st_size,
            }
            for name in files
        ],
        "checks": checks,
        "OCR_pages": pages,
        "poppler_renders": renders,
        "status": "PASS" if all(c["status"] == "PASS" for c in checks) else "FAIL",
        "unexecuted": [
            "Adobe Reader native viewer",
            "Firefox native clipboard/search/print",
            "physical printer",
            "real disk full",
            "clean Windows with network disabled",
            "Microsoft IME",
            "OS scaling 100/150/200%",
        ],
    }
    (out / args.result).write_text(
        json.dumps(result, ensure_ascii=False, indent=2) + "\n",
        encoding="utf-8",
        newline="\n",
    )
    print(
        json.dumps(
            {
                "status": result["status"],
                "checks": len(checks),
                "output": str(out / args.result),
            }
        )
    )
    return 0 if result["status"] == "PASS" else 1


if __name__ == "__main__":
    raise SystemExit(main())
