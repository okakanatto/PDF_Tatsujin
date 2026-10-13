"""Create a genuinely CMS-signed synthetic PDF using a disposable test certificate."""

from pathlib import Path
import datetime, hashlib, json, re
from pypdf import PdfWriter
from pypdf.generic import (
    DictionaryObject,
    NameObject,
    ArrayObject,
    NumberObject,
    ByteStringObject,
    TextStringObject,
)
from cryptography import x509
from cryptography.x509.oid import NameOID
from cryptography.hazmat.primitives.asymmetric import rsa
from cryptography.hazmat.primitives import hashes, serialization
from cryptography.hazmat.primitives.serialization import pkcs7

ROOT = Path(__file__).resolve().parents[1]
FX = ROOT / "fixtures"
key = rsa.generate_private_key(public_exponent=65537, key_size=2048)
name = x509.Name(
    [x509.NameAttribute(NameOID.COMMON_NAME, "PDFTatsujin Synthetic Test Only")]
)
cert = (
    x509.CertificateBuilder()
    .subject_name(name)
    .issuer_name(name)
    .public_key(key.public_key())
    .serial_number(123456789)
    .not_valid_before(datetime.datetime(2026, 1, 1, tzinfo=datetime.timezone.utc))
    .not_valid_after(datetime.datetime(2036, 1, 1, tzinfo=datetime.timezone.utc))
    .sign(key, hashes.SHA256())
)
w = PdfWriter(clone_from=FX / "D01.pdf")
sig = DictionaryObject(
    {
        NameObject("/Type"): NameObject("/Sig"),
        NameObject("/Filter"): NameObject("/Adobe.PPKLite"),
        NameObject("/SubFilter"): NameObject("/adbe.pkcs7.detached"),
        NameObject("/ByteRange"): ArrayObject(
            [NumberObject(1111111111) for _ in range(4)]
        ),
        NameObject("/Contents"): ByteStringObject(bytes(8192)),
        NameObject("/Reason"): TextStringObject(
            "Synthetic acceptance fixture; no real identity claim"
        ),
    }
)
field = DictionaryObject(
    {
        NameObject("/FT"): NameObject("/Sig"),
        NameObject("/T"): TextStringObject("TestSignature"),
        NameObject("/V"): w._add_object(sig),
    }
)
w._root_object[NameObject("/AcroForm")] = w._add_object(
    DictionaryObject(
        {
            NameObject("/Fields"): ArrayObject([w._add_object(field)]),
            NameObject("/SigFlags"): NumberObject(3),
        }
    )
)
p = FX / "D08-signed.pdf"
w.write(p)
data = p.read_bytes()
contents = re.search(rb"/Contents\s*(<0{16384}>)", data)
a, b = contents.span(1)
m = re.search(rb"/ByteRange\s*(\[[^\]]+\])", data)
ranges = [0, a, b, len(data) - b]
replacement = ("[ " + " ".join(f"{i:010d}" for i in ranges) + " ]").encode()
assert len(replacement) == m.end(1) - m.start(1)
data = data[: m.start(1)] + replacement + data[m.end(1) :]
cms = (
    pkcs7.PKCS7SignatureBuilder()
    .set_data(data[:a] + data[b:])
    .add_signer(cert, key, hashes.SHA256())
    .sign(
        serialization.Encoding.DER,
        [pkcs7.PKCS7Options.DetachedSignature, pkcs7.PKCS7Options.Binary],
    )
)
encoded = cms.hex().encode()
assert len(encoded) <= 16384
data = data[: a + 1] + encoded.ljust(16384, b"0") + data[b - 1 :]
p.write_bytes(data)
# Deliberately do not persist the test private key.
manifest = json.loads((FX / "manifest.json").read_text(encoding="utf-8"))
manifest = [i for i in manifest if i["file"] != p.name]
manifest.append(
    {
        "id": "D08",
        "file": p.name,
        "sha256": hashlib.sha256(data).hexdigest(),
        "pages": 1,
        "language": "jpn+eng",
        "origin": "scripts/make-signed-fixture.py; disposable test CMS certificate",
        "license": "synthetic test fixture",
        "purpose": "genuine detached CMS signature; certificate trust deliberately not established",
    }
)
(FX / "manifest.json").write_text(
    json.dumps(manifest, ensure_ascii=False, indent=2), encoding="utf-8"
)
print(p.name)
