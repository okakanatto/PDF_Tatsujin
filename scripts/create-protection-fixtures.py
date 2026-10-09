"""Create additional synthetic compatibility inputs without replacing any fixture."""

import hashlib
import json
from pathlib import Path

from pypdf import PdfReader, PdfWriter
from pypdf.constants import UserAccessPermissions
from pypdf._encryption import AlgV5
from pypdf.generic import ByteStringObject, NameObject

root = Path(__file__).resolve().parents[1]
destination = root / "fixtures/protection"
if destination.exists():
    raise RuntimeError("Protection inputs already exist; do not replace test evidence")
destination.mkdir()
rows = []
for algorithm in ("RC4-40", "RC4-128", "AES-128", "AES-256-R5", "AES-256"):
    for empty_owner in (False, True) if algorithm == "AES-256" else (False,):
        owner = "" if empty_owner else "compat-owner-2026"
        writer = PdfWriter(clone_from=PdfReader(root / "fixtures/D02.pdf"))
        writer.encrypt(
            "compat-user-2026",
            owner,
            permissions_flag=UserAccessPermissions(0xFFFFF2C0),
            algorithm=algorithm,
        )
        if empty_owner:
            # Fixture-only edge case: the public writer replaces an empty owner
            # password with the user password. Keep its random key/U unchanged
            # and generate the actual empty-owner O/OE entries independently.
            encryption = writer._encryption
            owner_value, owner_key = AlgV5.compute_O_value(
                encryption.R, b"", encryption._key, encryption.values.U
            )
            encryption.values.O, encryption.values.OE = owner_value, owner_key
            writer._encrypt_entry[NameObject("/O")] = ByteStringObject(owner_value)
            writer._encrypt_entry[NameObject("/OE")] = ByteStringObject(owner_key)
        name = algorithm + ("-empty-owner" if empty_owner else "") + ".pdf"
        path = destination / name
        with path.open("xb") as output:
            writer.write(output)
        reader = PdfReader(path)
        revision = reader.trailer["/Encrypt"]["/R"]
        if reader.decrypt(owner).name != "OWNER_PASSWORD":
            raise RuntimeError("Owner fixture authentication failed")
        rows.append(
            dict(
                file=name,
                algorithm=algorithm,
                revision=revision,
                user_password="compat-user-2026",
                owner_password=owner,
                sha256=hashlib.sha256(path.read_bytes()).hexdigest(),
            )
        )
(destination / "manifest.json").write_text(
    json.dumps(dict(source="D02.pdf; synthetic CC0 content", cases=rows), indent=2),
    encoding="utf-8",
)
print(json.dumps(dict(created=len(rows), directory=str(destination))))
