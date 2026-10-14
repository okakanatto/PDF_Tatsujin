"""Freeze refusal boundaries for inherited fonts without local declarations."""

import argparse
import hashlib
import json
from pathlib import Path

from pypdf import PdfReader, PdfWriter
from pypdf.generic import DictionaryObject, NameObject


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=False)
    base = (
        Path(__file__).resolve().parents[1]
        / "fixtures/redaction-copy/font-aliases-followup-v1"
    )
    fixed = json.loads((base / "criteria.json").read_text(encoding="utf-8"))
    source = next(c for c in fixed["cases"] if c["file"] == "inherited-form-font.pdf")
    assert (
        hashlib.sha256((base / source["file"]).read_bytes()).hexdigest()
        == source["sha256"]
    )
    rows = []
    for name in ("missing-local-font-dictionary", "empty-local-font-dictionary"):
        writer = PdfWriter(clone_from=PdfReader(base / source["file"]))
        form = writer.pages[0]["/Resources"]["/XObject"]["/Outside"]
        resources = DictionaryObject()
        if name.startswith("empty"):
            resources[NameObject("/Font")] = DictionaryObject()
        form[NameObject("/Resources")] = resources
        path = args.output / (name + ".pdf")
        writer.write(path)
        rows.append(
            dict(
                source,
                file=path.name,
                sha256=hashlib.sha256(path.read_bytes()).hexdigest(),
                refused=True,
            )
        )
    path = args.output / "criteria.json"
    path.write_text(
        json.dumps(
            {
                "cases": rows,
                "scope": "Refuse unbound inherited font resources without output",
            },
            ensure_ascii=False,
            indent=2,
        )
        + "\n",
        encoding="utf-8",
    )
    print(
        json.dumps(
            {
                "cases": len(rows),
                "criteria_sha256": hashlib.sha256(path.read_bytes()).hexdigest(),
            }
        )
    )


if __name__ == "__main__":
    main()
