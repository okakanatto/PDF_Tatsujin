"""Freeze Td/TD inputs before implementing their body editing support."""

import argparse
import hashlib
import json
from pathlib import Path
from pypdf import PdfReader, PdfWriter
from pypdf.generic import DecodedStreamObject, NameObject


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    source = root / "fixtures/existing-text-edit/font-state/body-text.pdf"
    digest = hashlib.sha256(source.read_bytes()).hexdigest()
    assert digest == "878c862cbf57ad38409c5841e47c60d0399c1e9e90a7624bafeb3baadc582693"
    args.output.mkdir(parents=True, exist_ok=False)
    files = {}
    for name, advances in (
        ("relative-position.pdf", [b"0 -22 Td"]),
        ("relative-leading.pdf", [b"0 -22 TD"]),
        ("horizontal.pdf", [b"5 -22 Td"]),
        ("uneven.pdf", [b"0 -22 Td", b"0 -24 Td"]),
    ):
        writer = PdfWriter()
        writer.clone_document_from_reader(PdfReader(source))
        page = writer.pages[0]
        fonts = page["/Resources"]["/Font"].get_object()
        font = next(
            k
            for k, v in fonts.items()
            if v.get_object().get("/BaseFont") == "/Helvetica"
        )
        command = (
            b"\nBT 0 Tr "
            + font.encode("ascii")
            + b" 16 Tf 7 TL 1 0 0 1 60 380 Tm (Relative first line) Tj "
        )
        for advance, line in zip(
            advances, (b"Relative second line", b"Relative third line")
        ):
            command += advance + b" (" + line + b") Tj "
        command += b"ET\nBT 1 0 0 1 60 310 Tm (AFTER RELATIVE STATE) Tj ET\n"
        stream = DecodedStreamObject()
        stream.set_data(page.get_contents().get_data() + command)
        page[NameObject("/Contents")] = writer._add_object(stream)
        path = args.output / name
        with path.open("xb") as target:
            writer.write(target)
        files[name] = hashlib.sha256(path.read_bytes()).hexdigest()
    plan = dict(
        files=files,
        source_sha256=digest,
        selected="Relative first line\nRelative second line",
        replacement="Changed first line\nChanged second line",
        japanese="相対位置の一行目を編集。\n相対位置の二行目を編集。",
        reedited="一行目を再編集しました。\n二行目を再編集しました。",
        explicit_family="Meiryo UI",
        geometry=[120, 300, 320, 80],
        refused=["horizontal.pdf", "uneven.pdf"],
        scope="Synthetic CC0 inputs and expectations fixed before first product operation; original acceptance unchanged",
    )
    (args.output / "criteria.json").write_text(
        json.dumps(plan, ensure_ascii=False, indent=2), encoding="utf-8"
    )
    print(json.dumps(plan, ensure_ascii=False, indent=2))


if __name__ == "__main__":
    main()
