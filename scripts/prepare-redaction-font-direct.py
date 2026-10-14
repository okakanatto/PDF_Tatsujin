"""Freeze direct font declaration sharing without overwriting prior inputs."""

import argparse
import copy
import hashlib
import json
from pathlib import Path
from pypdf import PdfReader, PdfWriter
from pypdf.generic import ArrayObject, DictionaryObject, NameObject, NumberObject
from prepare_redaction_helpers import stream

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--followup", action="store_true")
    args = parser.parse_args()
    out = args.output
    out.mkdir(exist_ok=False)
    base = ROOT / "fixtures/redaction-copy/font-schema-v2"
    raw = (base / "criteria.json").read_bytes()
    assert (
        hashlib.sha256(raw).hexdigest()
        == "91765aeee26617872f16ba46f4b9cadba61468279ef157a141d8e9891630ad63"
    )
    original = json.loads(raw)["cases"][2]
    source = base / original["file"]
    assert hashlib.sha256(source.read_bytes()).hexdigest() == original["sha256"]
    rows = []
    names = (
        ("direct-appearance", "direct-reference-page")
        if args.followup
        else (
            "direct-shared-page",
            "direct-shared-form",
            "direct-alias-page",
            "direct-outside-refused",
        )
    )
    for name in names:
        row = copy.deepcopy(original)
        writer = PdfWriter(clone_from=PdfReader(source))
        page = writer.pages[0]
        resources = page["/Resources"]
        font = resources["/Font"]["/F1"]
        assert not hasattr(resources["/Font"].raw_get("/F1"), "idnum")
        if name == "direct-appearance":
            appearances = DictionaryObject()
            terms = []
            for state in ("N", "R", "D"):
                term = "KEEP_DIRECT_AP_" + state
                terms.append(term)
                appearances[NameObject("/" + state)] = writer._add_object(
                    stream(
                        b"BT /F1 12 Tf 1 0 0 1 10 12 Tm ("
                        + term.encode()
                        + b") Tj ET\n",
                        Type=NameObject("/XObject"),
                        Subtype=NameObject("/Form"),
                        BBox=ArrayObject([NumberObject(v) for v in (0, 0, 220, 40)]),
                        Resources=DictionaryObject(
                            {NameObject("/Font"): copy.copy(resources["/Font"])}
                        ),
                    )
                )
            page[NameObject("/Annots")] = ArrayObject(
                [
                    writer._add_object(
                        DictionaryObject(
                            {
                                NameObject("/Type"): NameObject("/Annot"),
                                NameObject("/Subtype"): NameObject("/Stamp"),
                                NameObject("/Rect"): ArrayObject(
                                    [NumberObject(v) for v in (60, 320, 280, 360)]
                                ),
                                NameObject("/F"): NumberObject(4),
                                NameObject("/AP"): appearances,
                            }
                        )
                    )
                ]
            )
            row["appearance_terms"] = terms
        if name == "direct-shared-form":
            form = writer._add_object(
                stream(
                    b"BT /F1 12 Tf 1 0 0 1 60 330 Tm (KEEP_DIRECT_FORM) Tj ET\n",
                    Type=NameObject("/XObject"),
                    Subtype=NameObject("/Form"),
                    BBox=ArrayObject([NumberObject(v) for v in (0, 0, 612, 792)]),
                    Resources=DictionaryObject(
                        {NameObject("/Font"): copy.copy(resources["/Font"])}
                    ),
                )
            )
            resources[NameObject("/XObject")] = DictionaryObject(
                {NameObject("/Direct"): form}
            )
            page[NameObject("/Contents")] = writer._add_object(
                stream(page.get_contents().get_data() + b"/Direct Do\n")
            )
        if name in ("direct-shared-page", "direct-alias-page", "direct-reference-page"):
            writer.add_page(PdfReader(source).pages[0])
            other = writer.pages[1]
            other[NameObject("/Resources")] = copy.copy(resources)
            other["/Resources"][NameObject("/Font")] = copy.copy(resources["/Font"])
            other[NameObject("/Contents")] = page.raw_get("/Contents")
            if name == "direct-reference-page":
                other["/Resources"]["/Font"][NameObject("/F1")] = writer._add_object(
                    copy.copy(font)
                )
            if name == "direct-alias-page":
                other["/Resources"]["/Font"][NameObject("/Alias")] = copy.copy(font)
                del other["/Resources"]["/Font"]["/F1"]
                other[NameObject("/Contents")] = writer._add_object(
                    stream(page.get_contents().get_data().replace(b"/F1 ", b"/Alias "))
                )
            row["second_page_terms"] = row["keep"] + [row["remove"]]
        if name == "direct-outside-refused":
            writer._root_object[NameObject("/PrivateAuditCopy")] = copy.copy(font)
        path = out / (name + ".pdf")
        writer.write(path)
        row.update(
            file=path.name,
            sha256=hashlib.sha256(path.read_bytes()).hexdigest(),
            refused=name.endswith("refused"),
        )
        rows.append(row)
    criteria = out / "criteria.json"
    criteria.write_text(
        json.dumps(
            dict(cases=rows, raster_dpi=300, outside_changed_pixels=0, boundary_px=1),
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
