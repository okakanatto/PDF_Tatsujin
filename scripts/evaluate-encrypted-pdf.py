"""Independently inspect AES-256 roles, permissions, document meaning and pixels.

Only synthetic test passwords appear here; product passwords are never written.
Native Acrobat/OS clipboard/physical print acceptance remains separate.
"""

import argparse
import hashlib
import json
from pathlib import Path
import stringprep
import unicodedata

from PIL import ImageChops
from pypdf import PdfReader
import pypdfium2 as pdfium
from pypdf.generic import IndirectObject, StreamObject


def check(value, message):
    if not value:
        raise RuntimeError(message)


def saslprep(value):
    value = "".join(
        " " if stringprep.in_table_c12(c) else c
        for c in value
        if not stringprep.in_table_b1(c)
    )
    value = unicodedata.ucd_3_2_0.normalize("NFKC", value)
    tables = [
        getattr(stringprep, "in_table_" + name)
        for name in (
            "a1",
            "c12",
            "c21",
            "c22",
            "c3",
            "c4",
            "c5",
            "c6",
            "c7",
            "c8",
            "c9",
        )
    ]
    check(not any(test(c) for c in value for test in tables), "SASLprep prohibited")
    if any(stringprep.in_table_d1(c) for c in value):
        check(
            not any(stringprep.in_table_d2(c) for c in value)
            and stringprep.in_table_d1(value[0])
            and stringprep.in_table_d1(value[-1]),
            "SASLprep bidi",
        )
    check(1 <= len(value.encode("utf-8")) <= 127, "PDF byte limit")
    return value


def graph(before, after, *, upgraded_pdf_version=True):
    visited, identities, reverse = set(), {}, {}
    streams = 0

    def compare(a, b, path):
        nonlocal streams
        if isinstance(a, IndirectObject) or isinstance(b, IndirectObject):
            check(
                isinstance(a, IndirectObject) and isinstance(b, IndirectObject),
                "Reference changed " + path,
            )
            first, second = (a.idnum, a.generation), (b.idnum, b.generation)
            check(
                identities.get(first, second) == second
                and reverse.get(second, first) == first,
                "Object identity changed " + path,
            )
            identities[first], reverse[second] = second, first
            if (first, second) in visited:
                return
            visited.add((first, second))
            compare(a.get_object(), b.get_object(), path)
        elif isinstance(a, StreamObject):
            check(
                isinstance(b, StreamObject) and a.get_data() == b.get_data(),
                "Decoded stream changed " + path,
            )
            streams += 1
            keys = set(a) - {"/Length"}
            check(keys == set(b) - {"/Length"}, "Stream dictionary changed " + path)
            for key in keys:
                compare(a.raw_get(key), b.raw_get(key), path + key)
        elif isinstance(a, dict):
            keys, other = set(a), set(b)
            if path == "/Root" and upgraded_pdf_version:
                check(b.get("/Version") == "/2.0", "Explicit PDF 2.0 missing")
                keys, other = keys - {"/Version"}, other - {"/Version"}
            check(isinstance(b, dict) and keys == other, "Dictionary changed " + path)
            for key in keys:
                compare(a.raw_get(key), b.raw_get(key), path + key)
        elif isinstance(a, list):
            check(isinstance(b, list) and len(a) == len(b), "Array changed " + path)
            for index, (first, second) in enumerate(zip(a, b)):
                compare(first, second, path + f"[{index}]")
        else:
            check(a == b, "Value changed " + path)

    for key in ("/Root", "/Info"):
        check((key in before.trailer) == (key in after.trailer), "Trailer changed")
        if key in before.trailer:
            compare(before.trailer.raw_get(key), after.trailer.raw_get(key), key)
    check(len(after.trailer["/ID"]) == 2, "Encryption ID missing")
    if isinstance(before.trailer.get("/ID"), list):
        check(
            before.trailer["/ID"] == after.trailer["/ID"],
            "Original document ID changed",
        )
    return {
        "reachable_streams": streams,
        "independent_object_identities": len(identities),
    }


def pages(before, after, password):
    with pdfium.PdfDocument(str(before)) as old, pdfium.PdfDocument(
        str(after), password=password
    ) as new:
        check(len(old) == len(new), "Page count changed")
        count = len(old)
        for index in range(count):
            a, b = old[index], new[index]
            x, y = a.render(scale=0.8, draw_annots=True), b.render(
                scale=0.8, draw_annots=True
            )
            check(
                ImageChops.difference(
                    x.to_pil().convert("RGB"), y.to_pil().convert("RGB")
                ).getbbox()
                is None,
                "Independent pixels changed",
            )
            ta, tb = a.get_textpage(), b.get_textpage()
            check(
                ta.get_text_range() == tb.get_text_range(),
                "Independent body/OCR copy changed",
            )
            for resource in (ta, tb, x, y, a, b):
                resource.close()
    return {"pages": count, "pixel_difference": 0, "extracted_text_exact": True}


def inspect(run, after_name, before_name, user, owner, expected_permissions):
    path = run / after_name
    raw = path.read_bytes()
    check(
        raw.startswith(b"%PDF-2.0") and b"ENCRYPTION-METADATA-SECRET-2026" not in raw,
        "Header or metadata encryption",
    )
    encryption = PdfReader(path).trailer["/Encrypt"]
    check(
        encryption["/Filter"] == "/Standard"
        and encryption["/V"] == 5
        and encryption["/R"] == 6
        and encryption["/Length"] == 256,
        "Incorrect algorithm",
    )
    check(
        encryption.get("/EncryptMetadata", True)
        and encryption["/CF"]["/StdCF"]["/CFM"] == "/AESV3"
        and encryption["/StmF"] == "/StdCF"
        and encryption["/StrF"] == "/StdCF",
        "Streams/strings/metadata not encrypted",
    )
    p = int(encryption["/P"]) & 0xFFFFFFFF
    check(p == expected_permissions, "Permission bits differ")
    check(
        PdfReader(path).decrypt("wrong-password") == 0
        and PdfReader(path).decrypt("") == 0,
        "Incorrect password accepted",
    )
    for password in ("", "wrong-password"):
        try:
            pdfium.PdfDocument(str(path), password=password)
        except pdfium.PdfiumError:
            pass
        else:
            raise RuntimeError("PDFium accepted incorrect password")
    results = []
    for password, role in ((user, 1), (owner, 2)):
        reader = PdfReader(path)
        check(reader.decrypt(password) == role, "Independent password role differs")
        before = PdfReader(run / before_name)
        comparison = graph(before, reader)
        results.append(
            {
                "role": "user" if role == 1 else "owner",
                **comparison,
                **pages(run / before_name, path, password),
            }
        )
        # Original encoded stream payloads must not survive in clear output.
        for generation, object_number in (
            (generation, number)
            for generation, offsets in before.xref.items()
            if generation != 65535
            for number in offsets
            if number != 0
        ):
            value = IndirectObject(object_number, generation, before).get_object()
            if isinstance(value, StreamObject) and len(value._data) >= 32:
                check(value._data not in raw, "An original stream remains unencrypted")
    return {
        "file": after_name,
        "permissions_hex": f"{p:08x}",
        "roles": results,
        "sha256": hashlib.sha256(raw).hexdigest(),
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("directory", type=Path)
    args = parser.parse_args()
    run = args.directory
    target = run / "independent-encrypted-pdf.json"
    check(not target.exists(), "Preserve previous results")
    result = {
        "status": "FAIL",
        "scope": "pypdf authentication/graph/encryption dictionaries; PDFium rendering/text; independent RFC 4013 vectors. Native UI acceptance is 未実行.",
    }
    try:
        vectors = json.loads(
            (run / "encryption-password-vectors.json").read_text(encoding="utf-8")
        )
        for row in vectors:
            try:
                actual = saslprep(row["input"])
            except (RuntimeError, UnicodeError):
                check(row["status"] == "REJECTED", "Unexpected vector rejection")
            else:
                check(
                    row["status"] == "PASS" and actual == row["prepared"],
                    "Independent SASLprep vector mismatch",
                )
        user, owner = "閲覧-User-2026", "変更-Owner-2026"
        base = 0xFFFFF000 | 0xC0 | 0x200
        normal = base | 4 | 0x800 | 0x10 | 0x100
        rows = []
        for after, before, first, second, permissions in (
            (
                "encryption-rich-after.pdf",
                "encryption-rich-before.pdf",
                user,
                owner,
                normal,
            ),
            (
                "encryption-rich-repeat.pdf",
                "encryption-rich-before.pdf",
                user,
                owner,
                normal,
            ),
            ("encryption-denied.pdf", "encryption-rich-before.pdf", user, owner, base),
            (
                "encryption-allowed.pdf",
                "encryption-rich-before.pdf",
                user,
                owner,
                normal | 8 | 0x20 | 0x400,
            ),
            (
                "encryption-ascii.pdf",
                "encryption-rich-before.pdf",
                "user-ASCII-2026",
                "owner-ASCII-2026",
                normal,
            ),
            (
                "encryption-form-after.pdf",
                "encryption-form-before.pdf",
                user,
                owner,
                normal,
            ),
            (
                "encryption-ocr-after.pdf",
                "encryption-ocr-before.pdf",
                user,
                owner,
                normal,
            ),
        ):
            rows.append(inspect(run, after, before, first, second, permissions))
        form = PdfReader(run / "encryption-form-after.pdf")
        form.decrypt(user)
        check(
            form.get_fields()["name"]["/V"] == "髙橋 香織", "Japanese form value lost"
        )
        result.update(
            status="PASS",
            files=rows,
            password_vectors=len(vectors),
            metadata_encrypted=True,
        )
    except Exception as error:
        result["error"] = str(error)
    target.write_text(
        json.dumps(result, ensure_ascii=False, indent=2), encoding="utf-8"
    )
    print(
        json.dumps(
            {
                "status": result["status"],
                "error": result.get("error"),
                "output": str(target),
            },
            ensure_ascii=False,
        )
    )
    if result["status"] != "PASS":
        raise SystemExit(1)


if __name__ == "__main__":
    main()
