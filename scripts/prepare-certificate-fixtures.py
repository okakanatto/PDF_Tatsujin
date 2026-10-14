"""Generate CC0 synthetic certificate PDFs; never write private keys or trust stores."""

import argparse
from datetime import datetime, timezone
import hashlib
import json
from pathlib import Path
import re

from cryptography import x509
from cryptography.hazmat.primitives import hashes, serialization
from cryptography.hazmat.primitives.asymmetric import ec, rsa
from cryptography.hazmat.primitives.serialization import pkcs7
from cryptography.x509.oid import NameOID


def certificate(key, subject, issuer, issuer_key, start, end, ca=False):
    name = x509.Name([x509.NameAttribute(NameOID.COMMON_NAME, subject)])
    issuer_name = issuer.subject if issuer else name
    return (
        x509.CertificateBuilder()
        .subject_name(name)
        .issuer_name(issuer_name)
        .public_key(key.public_key())
        .serial_number(x509.random_serial_number())
        .not_valid_before(datetime.fromisoformat(start).replace(tzinfo=timezone.utc))
        .not_valid_after(datetime.fromisoformat(end).replace(tzinfo=timezone.utc))
        .add_extension(
            x509.BasicConstraints(ca=ca, path_length=1 if ca else None), True
        )
        .add_extension(
            x509.SubjectKeyIdentifier.from_public_key(key.public_key()), False
        )
        .add_extension(
            x509.AuthorityKeyIdentifier.from_issuer_public_key(issuer_key.public_key()),
            False,
        )
        .add_extension(
            x509.KeyUsage(True, not ca, False, False, False, ca, ca, False, False), True
        )
        .sign(issuer_key, hashes.SHA256())
    )


def signature_dict(format_name=b"adbe.pkcs7.detached"):
    return (
        b"<< /Type /Sig /Filter /Adobe.PPKLite /SubFilter /"
        + format_name
        + b" /ByteRange [9999999999 9999999999 9999999999 9999999999] /Contents <"
        + b"00" * 8192
        + b"> >>"
    )


def base_pdf(blank=False, unbound=False, format_name=b"adbe.pkcs7.detached"):
    content = b"BT /F1 18 Tf 40 700 Td (SYNTHETIC CERTIFICATE DOCUMENT) Tj ET\n"
    objects = {
        1: b"<< /Type /Catalog /Pages 2 0 R /AcroForm 5 0 R >>",
        2: b"<< /Type /Pages /Count 1 /Kids [3 0 R] >>",
        3: b"<< /Type /Page /Parent 2 0 R /MediaBox [0 0 595 842] /Resources << /Font << /F1 6 0 R >> >> /Contents 4 0 R >>",
        4: b"<< /Length "
        + str(len(content)).encode()
        + b" >>\nstream\n"
        + content
        + b"endstream",
        5: b"<< /Fields [] >>" if unbound else b"<< /Fields [7 0 R] /SigFlags 3 >>",
        6: b"<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>",
        7: b"<< /FT /Sig /T (SyntheticSignature)"
        + (b"" if blank else b" /V 8 0 R")
        + b" >>",
    }
    if not blank:
        objects[8] = signature_dict(format_name)
    result = b"%PDF-1.7\n%\xe2\xe3\xcf\xd3\n"
    offsets = {}
    for number, value in objects.items():
        offsets[number] = len(result)
        result += f"{number} 0 obj\n".encode() + value + b"\nendobj\n"
    xref = len(result)
    result += f"xref\n0 {max(objects) + 1}\n0000000000 65535 f \n".encode()
    result += b"".join(
        f"{offsets[i]:010} 00000 n \n".encode() for i in range(1, max(objects) + 1)
    )
    return (
        result
        + f"trailer\n<< /Size {max(objects) + 1} /Root 1 0 R >>\nstartxref\n{xref}\n%%EOF\n".encode()
    )


def sign(source, cert, key, ca=None, digest=None):
    match = list(re.finditer(rb"/Contents (<[0-9a-fA-F]+>)", source))[-1]
    first, second = match.span(1)
    values = [0, first, second, len(source) - second]
    placeholder = b"9999999999 9999999999 9999999999 9999999999"
    source = source.replace(
        placeholder, b" ".join(f"{value:010}".encode() for value in values), 1
    )
    builder = (
        pkcs7.PKCS7SignatureBuilder()
        .set_data(source[:first] + source[second:])
        .add_signer(cert, key, digest or hashes.SHA256())
    )
    if ca:
        builder = builder.add_certificate(ca)
    der = builder.sign(
        serialization.Encoding.DER,
        [pkcs7.PKCS7Options.DetachedSignature, pkcs7.PKCS7Options.Binary],
    )
    hex_data = der.hex().encode()
    capacity = second - first - 2
    if len(hex_data) > capacity:
        raise RuntimeError("CMS fixture exceeds placeholder")
    return source[: first + 1] + hex_data.ljust(capacity, b"0") + source[second - 1 :]


def append_revision(source, second_signature=False):
    previous = int(re.findall(rb"startxref\s+(\d+)", source)[-1])
    objects = (
        {
            5: b"<< /Fields [7 0 R 9 0 R] /SigFlags 3 >>",
            9: b"<< /FT /Sig /T (SecondSignature) /V 10 0 R >>",
            10: signature_dict(),
        }
        if second_signature
        else {9: b"<< /Producer (UNSIGNED REVISION) >>"}
    )
    result, offsets = source + b"\n", {}
    for number, value in objects.items():
        offsets[number] = len(result)
        result += f"{number} 0 obj\n".encode() + value + b"\nendobj\n"
    xref = len(result)
    result += b"xref\n"
    for number, offset in offsets.items():
        result += f"{number} 1\n{offset:010} 00000 n \n".encode()
    info = "" if second_signature else "/Info 9 0 R "
    return (
        result
        + f"trailer\n<< /Size {max(objects) + 1} /Root 1 0 R {info}/Prev {previous} >>\nstartxref\n{xref}\n%%EOF\n".encode()
    )


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=False)
    key = rsa.generate_private_key(public_exponent=65537, key_size=2048)
    ca = certificate(
        key, "Synthetic PDF Test CA", None, key, "2020-01-01", "2040-01-01", True
    )
    signer_key = rsa.generate_private_key(public_exponent=65537, key_size=2048)
    signer = certificate(
        signer_key, "Synthetic PDF Signer", ca, key, "2020-01-01", "2035-01-01"
    )
    expired = certificate(
        signer_key, "Expired Synthetic Signer", ca, key, "2020-01-01", "2021-01-01"
    )
    future = certificate(
        signer_key, "Future Synthetic Signer", ca, key, "2035-01-01", "2036-01-01"
    )
    self_signed = certificate(
        signer_key,
        "Self Signed Synthetic",
        None,
        signer_key,
        "2020-01-01",
        "2035-01-01",
    )
    weak_key = rsa.generate_private_key(public_exponent=65537, key_size=1024)
    weak_cert = certificate(
        weak_key, "Weak Synthetic Key", None, weak_key, "2020-01-01", "2035-01-01"
    )
    ec_key = ec.generate_private_key(ec.SECP256R1())
    ec_cert = certificate(
        ec_key, "Synthetic EC Signer", ca, key, "2020-01-01", "2035-01-01"
    )
    valid = sign(base_pdf(), signer, signer_key, ca)
    cases = {
        "weak-hash.pdf": (
            sign(base_pdf(), self_signed, signer_key, digest=hashes.SHA224()),
            "Unsupported",
            "NotChecked",
            False,
        ),
        "weak-key.pdf": (
            sign(base_pdf(), weak_cert, weak_key),
            "Unsupported",
            "NotChecked",
            True,
        ),
        "valid.pdf": (valid, "Unchanged", "LocalTrusted", True),
        "expired.pdf": (
            sign(base_pdf(), expired, signer_key, ca),
            "Unchanged",
            "Expired",
            True,
        ),
        "future.pdf": (
            sign(base_pdf(), future, signer_key, ca),
            "Unchanged",
            "NotYetValid",
            True,
        ),
        "self-signed.pdf": (
            sign(base_pdf(), self_signed, signer_key),
            "Unchanged",
            "Untrusted",
            True,
        ),
        "EC.pdf": (
            sign(base_pdf(), ec_cert, ec_key, ca),
            "Unchanged",
            "LocalTrusted",
            True,
        ),
        "CAdES.pdf": (
            sign(base_pdf(format_name=b"ETSI.CAdES.detached"), signer, signer_key, ca),
            "Unchanged",
            "LocalTrusted",
            True,
        ),
        "tampered.pdf": (
            valid.replace(
                b"SYNTHETIC CERTIFICATE DOCUMENT", b"ALTEREDXX CERTIFICATE DOCUMENT"
            ),
            "Invalid",
            "NotChecked",
            True,
        ),
        "appended.pdf": (append_revision(valid), "Unchanged", "LocalTrusted", False),
        "blank.pdf": (base_pdf(blank=True), "Unsigned", "NotChecked", False),
        "unknown.pdf": (
            sign(base_pdf(format_name=b"unknown.detached"), signer, signer_key, ca),
            "Unsupported",
            "NotChecked",
            False,
        ),
        "unbound.pdf": (
            sign(base_pdf(unbound=True), signer, signer_key, ca),
            "Unsupported",
            "NotChecked",
            False,
        ),
        "double.pdf": (
            sign(append_revision(valid, True), signer, signer_key, ca),
            "Unchanged",
            "LocalTrusted",
            True,
        ),
    }
    # Mutate the actual DER, preserving file length and ByteRange.
    contents = re.search(rb"/Contents <([0-9a-f]+)>", valid)
    cms_bad = bytearray(valid)
    offset = contents.start(1) + 20
    cms_bad[offset] = ord("0") if cms_bad[offset] != ord("0") else ord("1")
    cases["bad-CMS.pdf"] = (bytes(cms_bad), "Invalid", "NotChecked", True)
    range_match = re.search(rb"/ByteRange \[([^]]+)\]", valid)
    range_values = [int(value) for value in range_match[1].split()]
    invalid_ranges = {
        "negative": [-1, *range_values[1:]],
        "overlap": [0, range_values[2] + 1, *range_values[2:]],
        "overflow": [0, range_values[1], range_values[2], 9223372036854775807],
        "extra": [*range_values, 0, 1],
        "mismatch": [0, range_values[1] + 1, *range_values[2:]],
        "noninteger": ["0.0", *range_values[1:]],
    }
    for label, values in invalid_ranges.items():
        replacement = " ".join(str(value) for value in values).encode()
        replacement = replacement.ljust(len(range_match[1]), b" ")
        data = valid[: range_match.start(1)] + replacement + valid[range_match.end(1) :]
        cases[f"range-{label}.pdf"] = (data, "Invalid", "NotChecked", False)
    records = []
    for file, (data, integrity, chain, full) in cases.items():
        (args.output / file).write_bytes(data)
        records.append(
            {
                "file": file,
                "sha256": hashlib.sha256(data).hexdigest(),
                "integrity": integrity,
                "chain": chain,
                "entire_file": full,
            }
        )
    ca_data = ca.public_bytes(serialization.Encoding.DER)
    (args.output / "test-ca.der").write_bytes(ca_data)
    (args.output / "manifest.json").write_text(
        json.dumps(
            {
                "license": "CC0-1.0; generated synthetic test data",
                "private_keys_written": False,
                "test_ca_sha256": hashlib.sha256(ca_data).hexdigest(),
                "cases": records,
            },
            ensure_ascii=False,
            indent=2,
        )
        + "\n",
        encoding="utf-8",
    )
    print(
        f"Prepared {len(records)} synthetic certificate cases; no private keys retained."
    )


if __name__ == "__main__":
    main()
