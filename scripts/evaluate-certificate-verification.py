"""Inspect app verification against fixed PDFs with independent PDF/certificate parsers."""

import argparse
import hashlib
import json
from pathlib import Path

from cryptography.hazmat.primitives import hashes
from cryptography.hazmat.primitives.serialization import pkcs7
from pypdf import PdfReader
import pypdfium2 as pdfium


def cms_der(data):
    # PDF reserves space after the DER value with zero bytes. Keep the actual
    # ASN.1 value intact rather than asking the certificate parser to ignore it.
    if len(data) < 2 or data[0] != 0x30:
        raise RuntimeError("Expected CMS DER sequence")
    length = data[1]
    header = 2
    if length & 0x80:
        count = length & 0x7F
        if count == 0 or count > 4 or len(data) < 2 + count:
            raise RuntimeError("Invalid DER length")
        length = int.from_bytes(data[2 : 2 + count], "big")
        header += count
    end = header + length
    if end > len(data) or any(data[end:]):
        raise RuntimeError("Invalid CMS padding")
    return data[:end]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--fixtures", type=Path, default=Path("fixtures/certificate-signatures")
    )
    parser.add_argument("--run", type=Path, required=True)
    args = parser.parse_args()
    output = args.run / "independent-certificate-inspection.json"
    if output.exists():
        raise RuntimeError("Preserve existing inspection")
    manifest = json.loads((args.fixtures / "manifest.json").read_text(encoding="utf-8"))
    actual = json.loads(
        (args.run / "certificate-cases.json").read_text(encoding="utf-8")
    )
    process = json.loads(
        (args.run / "process-result.json").read_text(encoding="utf-8-sig")
    )
    if not process["completed"] or not process["report_valid"]:
        raise RuntimeError("Require confirmed application execution")
    app_rows = {row["file"]: row["details"] for row in actual["cases"]}
    results = []
    for expected in manifest["cases"]:
        file = expected["file"]
        path = args.fixtures / file
        if hashlib.sha256(path.read_bytes()).hexdigest() != expected["sha256"]:
            raise RuntimeError("Changed certificate fixture")
        row = app_rows[file]
        if (
            row["integrity"] != expected["integrity"]
            or row["chain"] != expected["chain"]
        ):
            raise RuntimeError("Application result disagrees with frozen criteria")
        reader = PdfReader(path)
        fields = reader.get_fields() or {}
        count = 2 if file == "double.pdf" else 0 if file == "unbound.pdf" else 1
        if len(fields) != count:
            raise RuntimeError("Independent signature field count changed")
        cert_matched = None
        if row["certificate_sha256"]:
            value = list(fields.values())[-1]["/V"].get_object()
            certs = pkcs7.load_der_pkcs7_certificates(
                cms_der(bytes(value["/Contents"]))
            )
            cert_matched = row["certificate_sha256"] in {
                cert.fingerprint(hashes.SHA256()).hex() for cert in certs
            }
            if not cert_matched:
                raise RuntimeError(
                    "App certificate identity differs from actual CMS certificates"
                )
        with pdfium.PdfDocument(path) as pdf:
            if len(pdf) != 1:
                raise RuntimeError("Fixture page count changed")
            page = pdf[0]
            text = page.get_textpage().get_text_range()
            phrase = (
                "ALTEREDXX CERTIFICATE DOCUMENT"
                if file == "tampered.pdf"
                else "SYNTHETIC CERTIFICATE DOCUMENT"
            )
            if phrase not in text:
                raise RuntimeError("PDFium cannot read the actual signed PDF content")
            if file in {"valid.pdf", "double.pdf"}:
                page.render(scale=1).to_pil().save(
                    args.run / f"certificate-{path.stem}-PDFium.png"
                )
        results.append(
            {
                "file": file,
                "field_count": count,
                "certificate_identity_matches_CMS": cert_matched,
                "PDFium_text": "PASS",
                "status": "PASS",
            }
        )
    output.write_text(
        json.dumps(
            {
                "engine": "pypdf / PDFium / cryptography certificate parser",
                "exe_sha256": process["exe_sha256"],
                "cases": results,
                "scope": "Independent file structure, content and certificate identity. Cryptographic verification is separately exercised by .NET SignedCms; revocation and native Reader are unexecuted.",
            },
            ensure_ascii=False,
            indent=2,
        )
        + "\n",
        encoding="utf-8",
    )
    print(f"Independent certificate/PDF inspection: {len(results)} PASS")


if __name__ == "__main__":
    main()
