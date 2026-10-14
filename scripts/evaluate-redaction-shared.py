"""Independent shared-font text, outline, resource and 300dpi checks."""

import argparse
import importlib.util
import io
import json
import re
import subprocess
from pathlib import Path

import numpy as np
import pypdfium2 as pdfium
from PIL import Image
from pypdf import PdfReader
from pypdf.generic import StreamObject
from test_tools import poppler_tool

module = importlib.util.spec_from_file_location(
    "partial_checks", Path(__file__).with_name("evaluate-redaction-partial.py")
)
partial = importlib.util.module_from_spec(module)
module.loader.exec_module(partial)
PIN = "a24478e7395e3bfcc4046eb0266aba324c5858d6c3c9db0e447aba5d9ec0f586"
PINS = {
    "shared-fonts-v1": PIN,
    "font-aliases-v1": "06fd7bbcef23bf82d58852c351bd3d0d5098b96cbfea1ea23d5639e61332bcd9",
    "font-aliases-followup-v1": "4aa425628996b0a58f49d8f99b88421e19db3a3d61f355553607118519687550",
}


def text_boxes(path, page_index, terms):
    with pdfium.PdfDocument(path) as pdf:
        page = pdf[page_index]
        text = page.get_textpage()
        content = text.get_text_range()
        result = {}
        for term in terms:
            assert content.count(term) == 1, (path, term, content)
            start = content.index(term)
            result[term] = [
                list(text.get_charbox(i)) for i in range(start, start + len(term))
            ]
        return content, result


def untouched_page(source, target, engine):
    if engine:

        def image(path):
            process = subprocess.run(
                [
                    str(engine),
                    "-cropbox",
                    "-singlefile",
                    "-png",
                    "-r",
                    "300",
                    "-f",
                    "2",
                    "-l",
                    "2",
                    str(path),
                ],
                capture_output=True,
                timeout=60,
                check=True,
            )
            return np.array(Image.open(io.BytesIO(process.stdout)).convert("RGB"))

        before, after = image(source), image(target)
    else:
        with pdfium.PdfDocument(source) as left, pdfium.PdfDocument(target) as right:
            before = np.array(
                left[1].render(scale=300 / 72, draw_annots=True).to_pil().convert("RGB")
            )
            after = np.array(
                right[1]
                .render(scale=300 / 72, draw_annots=True)
                .to_pil()
                .convert("RGB")
            )
    assert before.shape == after.shape
    return int(np.count_nonzero(np.any(before != after, axis=2)))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--fixtures", type=Path, required=True)
    parser.add_argument("--run", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--probe-layout", action="store_true")
    args = parser.parse_args()
    assert not args.output.exists(), "Preserve prior independent results"
    raw = (args.fixtures / "criteria.json").read_bytes()
    pin = PINS[args.fixtures.name]
    assert partial.sha(raw) == pin
    fixed = json.loads(raw)
    poppler = poppler_tool("pdftoppm")
    results = []
    for case in fixed["cases"]:
        if case["refused"]:
            continue
        record = {"file": case["file"], "status": "FAIL"}
        try:
            source = args.fixtures / case["file"]
            assert partial.sha(source.read_bytes()) == case["sha256"]
            prefix = (
                "alias-"
                if args.fixtures.name.startswith("font-aliases-")
                else "shared-"
            )
            target = (
                args.run / Path(case["file"]).stem / "candidate.pdf"
                if args.probe_layout
                else args.run / (prefix + case["file"])
            )
            before, after = PdfReader(source), PdfReader(target)
            assert len(after.pages) == len(before.pages) == 2
            maximum = 0
            for page_index, terms in enumerate(
                [case["keep"], case["second_page_terms"]]
            ):
                content, positions = text_boxes(target, page_index, terms)
                assert case["remove"] not in content
                pypdf_text = after.pages[page_index].extract_text()
                assert case["remove"] not in pypdf_text
                for term in terms:
                    assert term in pypdf_text, (page_index, term, pypdf_text)
                    baseline = case["boxes"][str(page_index)][term]
                    assert len(baseline) == len(positions[term])
                    maximum = max(
                        maximum,
                        max(
                            abs(a - b)
                            for left, right in zip(baseline, positions[term])
                            for a, b in zip(left, right)
                        ),
                    )
                for key in ("/MediaBox", "/CropBox", "/Rotate", "/UserUnit"):
                    assert before.pages[page_index].get(key) == after.pages[
                        page_index
                    ].get(key)
            assert maximum <= case["max_position_error_pt"], maximum
            originals = {partial.sha(before.pages[0].get_contents().get_data())}
            for value in partial.objects(before):
                if isinstance(value, dict) and "/ToUnicode" in value:
                    originals.add(partial.sha(value["/ToUnicode"].get_data()))
                if isinstance(value, dict) and "/FontFile2" in value:
                    originals.add(partial.sha(value["/FontFile2"].get_data()))
            for value in partial.objects(after):
                if isinstance(value, StreamObject):
                    assert (
                        partial.sha(value.get_data()) not in originals
                    ), "Original selected font/map/content remains"
            outlines_a, outlines_b = partial.font_characters(
                before
            ), partial.font_characters(after)
            assert outlines_b
            for character, outline in outlines_b.items():
                assert outline == outlines_a[character], (character, "changed outline")
            for value in partial.objects(after):
                if isinstance(value, dict) and value.get("/Type") == "/Pages":
                    assert "/Resources" not in value, "Old inherited resources remain"
            if case.get("appearance", case["file"] == "appearance-states.pdf"):
                original, result = (
                    before.pages[1]["/Annots"][0].get_object(),
                    after.pages[1]["/Annots"][0].get_object(),
                )
                for key in ("/Subtype", "/Rect", "/AS", "/F"):
                    assert original[key] == result[key]
                assert original["/AP"].keys() == result["/AP"].keys()
                for key in ("/N", "/R", "/D"):
                    old, new = original["/AP"][key], result["/AP"][key]
                    if not isinstance(old, StreamObject):
                        assert old.keys() == new.keys()
                        old, new = old["/On"], new["/On"]
                    for prop in ("/BBox", "/Matrix"):
                        assert old.get(prop) == new.get(prop)
            changed = [
                partial.raster_unchanged_outside(source, target, case["rect"], engine)
                for engine in (None, poppler)
            ]
            second = [
                untouched_page(source, target, engine) for engine in (None, poppler)
            ]
            assert changed == [0, 0] and second == [0, 0], (changed, second)
            record.update(
                status="PASS",
                max_position_error_pt=maximum,
                outside_changed_pixels=changed,
                second_page_changed_pixels=second,
                surviving_outlines_exact=True,
                old_fonts_maps_content_absent=True,
            )
        except FileNotFoundError as error:
            record.update(status="未実行", error=str(error))
        except Exception as error:
            record["error"] = str(error)
        results.append(record)
    report = {
        "criteria_sha256": pin,
        "results": results,
        "passed": sum(r["status"] == "PASS" for r in results),
        "failed": sum(r["status"] == "FAIL" for r in results),
        "unexecuted": sum(r["status"] == "未実行" for r in results),
        "engines": ["PDFium", "pypdf", "fontTools", "Poppler"],
        "native_external_GUI": "未実行",
    }
    args.output.write_text(
        json.dumps(report, ensure_ascii=False, indent=2) + "\n", encoding="utf-8"
    )
    print(json.dumps(report, ensure_ascii=False))
    return int(report["failed"] or report["unexecuted"])


if __name__ == "__main__":
    raise SystemExit(main())
