"""Create a synthetic PDF whose user password cannot copy content.

The owner and user passwords deliberately differ. The original D08 document
tests password/read-only handling; it must not be assumed to forbid copying.
Generated files are frozen before app tests and never overwritten here.
"""

import hashlib
import json
from pathlib import Path

from pypdf import PdfWriter
from pypdf.constants import UserAccessPermissions

root = Path(__file__).resolve().parents[1]
source = root / "fixtures/viewer-search.pdf"
target = root / "fixtures/viewer-selection-restricted.pdf"
manifest = root / "fixtures/viewer-selection-restricted-manifest.json"
assert not target.exists() and not manifest.exists(), "Preserve frozen input"
writer = PdfWriter(clone_from=source)
writer.encrypt(
    user_password="selection-user",
    owner_password="selection-owner",
    permissions_flag=UserAccessPermissions.PRINT,
    algorithm="AES-256",
)
writer.write(target)
manifest.write_text(
    json.dumps(
        {
            "file": target.name,
            "sha256": hashlib.sha256(target.read_bytes()).hexdigest(),
            "origin": "Own synthetic viewer-search.pdf, MIT; embedded Noto Sans JP, OFL.",
            "user_password": "selection-user",
            "owner_password": "selection-owner",
            "expected_user_copy_permission": False,
            "expected_user_print_permission": True,
        },
        ensure_ascii=False,
        indent=2,
    )
    + "\n",
    encoding="utf-8",
)
print("Restricted fixture frozen before app verification")
