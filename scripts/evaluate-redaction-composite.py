"""Inspect frozen composite glyphs with independent PDF engines."""

import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
from pypdf import PdfReader
from test_tools import poppler_tool


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--fixtures", required=True, type=Path)
    parser.add_argument("--run", required=True, type=Path)
    parser.add_argument("--probe-layout", action="store_true")
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()
    assert not args.output.exists()
    data = (args.fixtures / "criteria.json").read_bytes()
    assert (
        hashlib.sha256(data).hexdigest()
        == "894aa5fc4fab49b3af99676c08f34b225924b0344d402d0f27fcdbde8f919ea7"
    )
    spec = importlib.util.spec_from_file_location(
        "tatsu_partial", Path(__file__).with_name("evaluate-redaction-partial.py")
    )
    partial = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(partial)
    poppler = poppler_tool("pdftoppm")
    results = []
    for row in json.loads(data)["cases"]:
        result = {"file": row["file"], "status": "FAIL"}
        try:
            source = args.fixtures / row["file"]
            target = args.run / (
                Path(row["file"]).stem + "/candidate.pdf"
                if args.probe_layout
                else "partial-composite-" + row["file"]
            )
            assert partial.sha(source.read_bytes()) == row["sha256"]
            old, new = partial.font_characters(
                PdfReader(source)
            ), partial.font_characters(PdfReader(target))
            result["public_outline_preserved"] = old["公"] == new["公"]
            assert result[
                "public_outline_preserved"
            ], "Required component erased from surviving 公"
            for character, outline in new.items():
                assert old[character] == outline
            text, boxes = partial.text_and_boxes(target, row["keep"])
            assert row["remove"] not in text
            error = max(
                abs(a - b)
                for term in boxes
                for left, right in zip(boxes[term], row["boxes"][term])
                for a, b in zip(left, right)
            )
            assert error <= row["max_position_error_pt"], error
            changed = partial.raster_unchanged_outside(source, target, row["rect"])
            external = partial.raster_unchanged_outside(
                source, target, row["rect"], poppler
            )
            assert changed == 0 and external == 0, (changed, external)
            result.update(
                status="PASS",
                max_position_error_pt=error,
                outside_changed_pixels=changed,
                Poppler_outside_changed_pixels=external,
            )
        except Exception as error:
            result["error"] = str(error)
        results.append(result)
    report = {
        "results": results,
        "passed": sum(r["status"] == "PASS" for r in results),
        "failed": sum(r["status"] != "PASS" for r in results),
        "engines": [
            "fontTools decomposed outlines",
            "PDFium text/render",
            "Poppler render",
        ],
    }
    args.output.write_text(
        json.dumps(report, ensure_ascii=False, indent=2) + "\n", encoding="utf-8"
    )
    print(json.dumps(report, ensure_ascii=False))
    return int(report["failed"] != 0)


if __name__ == "__main__":
    raise SystemExit(main())
