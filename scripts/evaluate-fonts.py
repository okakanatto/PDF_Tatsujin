"""Inspect embedded text faces and render their saved PDFs in independent engines.

This evaluates font appearance streams, not native Reader GUI or OS clipboard.
Windows font programs are not copied into the application distribution.
"""

import argparse
from io import BytesIO
import json
from pathlib import Path
import subprocess

from fontTools.ttLib import TTFont
import pypdfium2 as pdfium
from pypdf import PdfReader

from test_tools import poppler_tool


def check(ok, message):
    if not ok:
        raise RuntimeError(message)


def inspect(path, expected_family, expected_text, expected_permissions):
    reader = PdfReader(path)
    annotations = reader.pages[0].get("/Annots", [])
    check(annotations, f"No annotations: {path.name}")
    annotation = annotations[-1].get_object()
    metadata = json.loads(annotation["/Tatsujin"].original_bytes.decode("utf-8"))
    check(metadata["fontFamily"] == expected_family, "Wrong reediting family")
    check(metadata["text"] == expected_text, "Wrong multiline text")
    check(annotation["/F"] & 4, "Appearance is not printable")
    appearance = annotation["/AP"]["/N"].get_object()
    check(appearance.get_data(), "Empty appearance commands")
    fonts = appearance["/Resources"]["/Font"].get_object()
    programs = []
    for reference in fonts.values():
        font = reference.get_object()
        check(font["/ToUnicode"].get_object().get_data(), "Missing character map")
        descendant = font["/DescendantFonts"][0].get_object()
        descriptor = descendant["/FontDescriptor"].get_object()
        program = descriptor["/FontFile2"].get_object().get_data()
        check(len(program) > 100, "Font program is not embedded")
        embedded = TTFont(BytesIO(program))
        check(
            embedded["OS/2"].fsType == expected_permissions,
            "Embedding permissions changed",
        )
        programs.append(
            {
                "base_font": str(font["/BaseFont"]),
                "embedded_bytes": len(program),
                "glyphs": embedded["maxp"].numGlyphs,
                "family": embedded["name"].getDebugName(1),
                "postscript_name": embedded["name"].getDebugName(6),
                "fsType": embedded["OS/2"].fsType,
            }
        )
    check(programs, "No embedded face inspected")
    compact_name = expected_family.replace(" ", "")
    check(
        any(
            compact_name.lower() in row["base_font"].replace("-", "").lower()
            for row in programs
        ),
        f"Embedded face differs from selection: {expected_family}",
    )
    document = pdfium.PdfDocument(path)
    with_annotations = document[0].render(scale=1.5, draw_annots=True).to_pil()
    without_annotations = document[0].render(scale=1.5, draw_annots=False).to_pil()
    check(
        with_annotations.tobytes() != without_annotations.tobytes(),
        "Independent renderer did not display the added text",
    )
    with_annotations.save(path.with_name(path.stem + "-pdfium.png"))
    document.close()
    subprocess.run(
        [
            str(poppler_tool("pdftoppm")),
            "-f",
            "1",
            "-singlefile",
            "-scale-to",
            "1300",
            "-png",
            str(path),
            str(path.with_name(path.stem + "-poppler")),
        ],
        check=True,
        capture_output=True,
    )
    return {"file": path.name, "family": expected_family, "programs": programs}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("evidence", type=Path)
    args = parser.parse_args()
    output = args.evidence / "independent-fonts.json"
    check(not output.exists(), "Preserve earlier verification")
    tests = json.loads((args.evidence / "selftest.json").read_text(encoding="utf-8"))
    test = next(t for t in tests["tests"] if t["name"] == "B01_fonts_roundtrip")
    check(test["status"] == "PASS", "Font roundtrip suite failed")
    rows = []
    for row in test["details"]["families"]:
        path = args.evidence / row["file"]
        rows.append(inspect(path, row["family"], row["text"], row["fsType"]))
        rows.append(
            inspect(
                path.with_name(path.stem + "-edited.pdf"),
                row["family"],
                row["edited_text"],
                row["fsType"],
            )
        )
    result = {
        "status": "PASS",
        "engines": {
            "pdfium": str(pdfium.PDFIUM_INFO),
            "poppler": subprocess.run(
                [str(poppler_tool("pdftoppm")), "-v"], capture_output=True, text=True
            ).stderr.strip(),
        },
        "files": rows,
        "native_Reader_GUI": "未実行",
        "machine_without_the_selected_system_fonts": "未実行",
    }
    output.write_text(
        json.dumps(result, ensure_ascii=False, indent=2), encoding="utf-8"
    )
    print(json.dumps({"status": result["status"], "files": len(rows)}))


if __name__ == "__main__":
    main()
