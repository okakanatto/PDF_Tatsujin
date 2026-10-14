"""Freeze mixed-font relative multiline inputs before product operations."""

import argparse
import hashlib
import json
import re
from pathlib import Path

from pypdf import PdfReader, PdfWriter
from pypdf.generic import DecodedStreamObject, NameObject, RectangleObject


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def lines(data, form, command):
    begin = re.search(rb"BT 0 Tr [01] Tc 0 Tw 100 Tz 0 Ts", data).start()
    end = data.index(b"ET", begin)
    block = data[begin:end]
    matrix = b"5 95" if form else b"60.25 610.125"
    origin = b"5 170" if form else b"60.25 480.125"
    block = block.replace(matrix + b" Tm", origin + b" Tm")
    block = block.replace(b"0 Ts", b"0 Ts 25 TL", 1).replace(b"19.2 TL", b"25 TL")
    # The original Japanese subset already encodes both permutations. Reuse its
    # exact bytes for each unselected line; no product encoder is involved.
    second = block[block.index(b"\n") + 1 :]
    second = re.sub(rb"1 0 0 1 [0-9.]+ [0-9.]+ Tm", b"", second)
    second = (
        second.replace(b"(Intro )", b"(Second )")
        .replace(b"[(Styled ) 25 (phrase)] TJ", b"(Bold second) Tj")
        .replace(b"(Suffix intact)", b"(Suffix second)")
    )
    third = (
        second.replace(b"(Second )", b"(Third )")
        .replace(b"(Bold second)", b"(Bold third)")
        .replace(b"(Suffix second)", b"(Suffix third)")
    )
    japanese = re.search(rb"\((?:\\[0-7]{3})+\)", block).group()
    codes = dict(zip("編集します日本語本文", re.findall(rb"\\[0-7]{3}", japanese)))

    def encoded(text):
        return b"(" + b"".join(codes[c] for c in text) + b")"

    second = second.replace(japanese, encoded("日本語本文編集します"))
    third = third.replace(japanese, encoded("本文編集します日本語"))
    return data[:begin] + block + command + second + command + third + data[end:]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=False)
    root = Path(__file__).resolve().parents[1]
    base = root / "fixtures/existing-text-edit/flowing-parts-v1"
    assert (
        sha(base / "criteria.json")
        == "1ac4f62edf2325101e0c8d0230a339518d6ae449eafd74fec2f5351fea968818"
    )
    previous = json.loads((base / "criteria.json").read_text(encoding="utf-8"))
    variants = {
        "page": ("page", b" T* "),
        "rotated": ("rotated", b" 0 -25 Td "),
        "td": ("page", b" 0 -25 TD "),
        "shared": ("shared", b" 0 -25 TD "),
        "inherited": ("inherited", b" T* "),
        "horizontal-refused": ("page", b" 1 -25 Td "),
        "repeat-refused": ("page", b" T* T* "),
        "leading-refused": ("page", b" 0 TL T* "),
        "position-refused": ("page", b" 1 0 0 1 60.25 455.125 Tm "),
        "spacing-refused": ("spacing-refused", b" T* "),
        "bbox-refused": ("shared", b" T* "),
        "cycle-refused": ("cycle-refused", b" T* "),
        "group-refused": ("group-refused", b" T* "),
        "clip-refused": ("clip-refused", b" T* "),
    }
    files, inputs = {}, {}
    for name, (source_name, command) in variants.items():
        source = base / (source_name + ".pdf")
        assert sha(source) == previous["files"][source.name]
        inputs[source.name] = sha(source)
        writer = PdfWriter()
        writer.clone_document_from_reader(PdfReader(source))
        page = writer.pages[0]
        is_form = source_name in (
            "shared",
            "inherited",
            "cycle-refused",
            "group-refused",
            "clip-refused",
        )
        if is_form:
            outer = page["/Resources"]["/XObject"]["/TatsujinFixtureOuter"]
            inner = outer["/Resources"]["/XObject"]["/Inner"]
            inner.set_data(lines(inner.get_data(), True, command))
            inner[NameObject("/BBox")] = RectangleObject(
                [0, 0, 200 if name == "bbox-refused" else 310, 205]
            )
            outer[NameObject("/BBox")] = RectangleObject([0, 0, 330, 225])
            for index, item in enumerate(writer.pages[:2]):
                data = (
                    item.get_contents().get_data().replace(b"80 480 cm", b"80 280 cm")
                )
                if index == 0:
                    data = data.replace(b"80 300 cm", b"80 20 cm")
                stream = DecodedStreamObject()
                stream.set_data(data)
                item[NameObject("/Contents")] = writer._add_object(stream)
        else:
            data = lines(page.get_contents().get_data(), False, command)
            data = data.replace(b"60.25 430.125 Tm", b"60.25 400.125 Tm")
            stream = DecodedStreamObject()
            stream.set_data(data)
            page[NameObject("/Contents")] = writer._add_object(stream)
        path = args.output / (name + ".pdf")
        with path.open("xb") as handle:
            writer.write(handle)
        files[path.name] = sha(path)
    fixed = dict(
        files=files,
        source_inputs=inputs,
        positives={"page": 0, "rotated": 0, "td": 0, "shared": 2, "inherited": 2},
        replacements={
            "Second ": "Start ",
            "Bold second": "Bold text",
            "Suffix second": "End intact",
            "日本語本文編集します": "編集します日本語本文",
        },
        remove="Bold second",
        reedited="Bold edit",
        following=previous["following"],
        refused=["cycle-refused", "group-refused", "clip-refused"],
        restricted=[
            "position-refused",
            "horizontal-refused",
            "repeat-refused",
            "leading-refused",
            "spacing-refused",
            "bbox-refused",
        ],
        page_overflow="W" * 200,
        form_overflow="W" * 24,
        output_cases=[
            "intro",
            "bold",
            "suffix",
            "japanese",
            "removed",
            "saved",
            "reedited",
        ],
        second_call_cases=["shared", "inherited"],
        ui_cases=["replaced", "removed"],
        show_offset=4,
        selected_line=1,
        parts_per_line=4,
        arithmetic_tolerance_pt=1e-8,
        outside_pixels=0,
        raster_dpi=300,
        boundary_pixels=1,
        coordinates="All real character boxes exactly equal independent source-and-criteria reference; only selected physical line suffix moves",
        scope="Synthetic CC0 relative multiline mixed-font profile; OFL Japanese subset; fixed before product operations; prior input and acceptance unchanged.",
    )
    criterion = args.output / "criteria.json"
    criterion.write_text(
        json.dumps(fixed, ensure_ascii=False, indent=2), encoding="utf-8"
    )
    print(json.dumps(dict(criterion_sha256=sha(criterion), files=files)))


if __name__ == "__main__":
    main()
