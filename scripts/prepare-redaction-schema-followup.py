"""Freeze private values in obsolete Name and known descriptor string fields."""

import argparse
import copy
import hashlib
import json
from pathlib import Path
from pypdf import PdfReader, PdfWriter
from pypdf.generic import NameObject, TextStringObject

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, required=True)
    out = parser.parse_args().output
    out.mkdir(exist_ok=False)
    base = ROOT / "fixtures/redaction-copy/font-schema-v2"
    raw = (base / "criteria.json").read_bytes()
    assert (
        hashlib.sha256(raw).hexdigest()
        == "91765aeee26617872f16ba46f4b9cadba61468279ef157a141d8e9891630ad63"
    )
    rows = json.loads(raw)["cases"]
    selected = []
    for name, index in (("obsolete-name", 1), ("descriptor-known-string-refused", 9)):
        row = copy.deepcopy(rows[index])
        source = base / row["file"]
        assert hashlib.sha256(source.read_bytes()).hexdigest() == row["sha256"]
        writer = PdfWriter(clone_from=PdfReader(source))
        font = writer.pages[0]["/Resources"]["/Font"]["/F1"]
        del font["/PrivateAuditPayload"]
        if name == "obsolete-name":
            font[NameObject("/Name")] = NameObject("/" + row["private_marker"])
        else:
            descriptor = font["/FontDescriptor"]
            del descriptor["/PrivateAuditPayload"]
            descriptor[NameObject("/FontFamily")] = TextStringObject(
                row["private_marker"]
            )
        path = out / (name + ".pdf")
        writer.write(path)
        row.update(file=path.name, sha256=hashlib.sha256(path.read_bytes()).hexdigest())
        selected.append(row)
    criteria = out / "criteria.json"
    criteria.write_text(
        json.dumps(
            dict(
                cases=selected, raster_dpi=300, outside_changed_pixels=0, boundary_px=1
            ),
            ensure_ascii=False,
            indent=2,
        )
        + "\n",
        encoding="utf-8",
    )
    print(
        json.dumps(
            dict(criteria_sha256=hashlib.sha256(criteria.read_bytes()).hexdigest())
        )
    )


if __name__ == "__main__":
    main()
