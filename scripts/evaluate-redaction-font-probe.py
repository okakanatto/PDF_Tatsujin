"""Inspect removed Japanese text and its recoverability from subset-font maps."""

import argparse
import hashlib
import json
import re
from pathlib import Path

import pypdfium2 as pdfium
from pypdf import PdfReader


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def subset_characters(path):
    reader = PdfReader(path)
    parts = []
    for page in reader.pages:
        for reference in page["/Resources"]["/Font"].values():
            font = reference.get_object()
            if "/ToUnicode" not in font:
                continue
            data = font["/ToUnicode"].get_object().get_data()
            pairs = re.findall(
                rb"<([0-9a-fA-F]{2,8})>\s*<((?:[0-9a-fA-F]{4}){1,2})>", data
            )
            mapping = {
                int(code, 16): bytes.fromhex(value.decode()).decode("utf-16-be")
                for code, value in pairs
            }
            parts.append(
                "".join(
                    value
                    for code, value in sorted(mapping.items())
                    if any(ord(character) > 127 for character in value)
                )
            )
    return "\n".join(parts)


def page_text(path):
    with pdfium.PdfDocument(path) as document:
        page = document[0]
        text = page.get_textpage()
        try:
            return text.get_text_range()
        finally:
            text.close()
            page.close()


def phrase_boxes(path, phrase):
    with pdfium.PdfDocument(path) as document:
        page = document[0]
        text = page.get_textpage()
        try:
            contents = text.get_text_range()
            if contents.count(phrase) != 1:
                return None
            start = contents.index(phrase)
            return [
                list(text.get_charbox(i)) for i in range(start, start + len(phrase))
            ]
        finally:
            text.close()
            page.close()


def stream_hashes(path):
    reader = PdfReader(path)
    from pypdf.generic import IndirectObject

    result = set()
    for generation, objects in reader.xref.items():
        for number in objects:
            if number == 0:
                continue
            value = reader.get_object(IndirectObject(number, generation, reader))
            if hasattr(value, "get_data"):
                result.add(hashlib.sha256(value.get_data()).hexdigest())
    return result


def font_payloads(path):
    reader = PdfReader(path)
    result = set()
    for page in reader.pages:
        for reference in page["/Resources"]["/Font"].values():
            font = reference.get_object()
            if "/ToUnicode" in font:
                result.add(hashlib.sha256(font["/ToUnicode"].get_data()).hexdigest())
            descriptor = font.get("/FontDescriptor")
            if descriptor:
                for key in ("/FontFile", "/FontFile2", "/FontFile3"):
                    if key in descriptor.get_object():
                        result.add(
                            hashlib.sha256(
                                descriptor.get_object()[key].get_data()
                            ).hexdigest()
                        )
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--fixture", type=Path, required=True)
    parser.add_argument("--run", type=Path, required=True)
    args = parser.parse_args()
    output = args.run / "independent-font-inspection.json"
    if output.exists():
        raise FileExistsError("Preserve earlier evaluation")
    criteria = json.loads((args.fixture / "criteria.json").read_text(encoding="utf-8"))
    rows = []
    for case in criteria["cases"]:
        original = args.fixture / case["file"]
        assert sha(original) == case["sha256"], "Frozen source changed"
        assert case["remove"] in page_text(
            original
        ), "Source extraction must contain the target"
        assert case["remove"] in subset_characters(
            original
        ), "Font attack must reproduce before redaction"
        if case.get("keep_additional"):
            assert case["keep_additional"] in page_text(original)
        position_phrase = case.get("preserve_position_phrase")
        if position_phrase:
            assert (
                phrase_boxes(original, position_phrase)
                == case["preserved_character_boxes"]
            ), "Frozen source geometry changed"
        folder = args.run / Path(case["file"]).stem
        probe = json.loads((folder / "probe.json").read_text(encoding="utf-8"))
        candidate = folder / "candidate.pdf"
        if probe["status"] == "FAIL":
            accepted = not candidate.exists() and "サブセット書体" in probe.get(
                "error", ""
            )
            rows.append(
                dict(
                    file=case["file"],
                    status="PASS" if accepted else "FAIL",
                    rejected_without_publication=not candidate.exists(),
                    error=probe.get("error"),
                )
            )
        else:
            recovered = subset_characters(candidate)
            contents = page_text(candidate)
            leaked = case["remove"] in recovered
            positions_preserved = (
                not position_phrase
                or phrase_boxes(candidate, position_phrase)
                == case["preserved_character_boxes"]
            )
            accepted = (
                case["remove"] not in contents
                and not leaked
                and case["keep"] in contents
                and probe["original_unchanged"]
                and not case.get("expect_rejection", False)
                and not (font_payloads(original) & stream_hashes(candidate))
                and positions_preserved
                and (
                    not case.get("keep_additional")
                    or case["keep_additional"] in contents
                )
            )
            rows.append(
                dict(
                    file=case["file"],
                    status="PASS" if accepted else "FAIL",
                    target_in_extracted_text=case["remove"] in contents,
                    target_recovered_from_subset_font=leaked,
                    recovered_font_characters=recovered,
                    outside_text_preserved=case["keep"] in contents,
                    source_unchanged=probe["original_unchanged"],
                    original_font_payloads_removed=not bool(
                        font_payloads(original) & stream_hashes(candidate)
                    ),
                    following_character_boxes_unchanged=(
                        positions_preserved if position_phrase else None
                    ),
                )
            )
    result = dict(
        status="PASS" if all(r["status"] == "PASS" for r in rows) else "FAIL",
        cases=rows,
        scope="Synthetic visible/invisible Japanese subset-font attack; no general product acceptance",
    )
    output.write_text(
        json.dumps(result, ensure_ascii=False, indent=2), encoding="utf-8"
    )
    print(json.dumps(result, ensure_ascii=False))
    if result["status"] != "PASS":
        raise SystemExit(1)


if __name__ == "__main__":
    main()
