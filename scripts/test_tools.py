"""Locate optional external test programs without a developer-specific path."""

import os
from pathlib import Path
import shutil


def poppler_tool(name: str) -> Path:
    directory = os.environ.get("TATSU_POPPLER_BIN")
    executable = name + (".exe" if os.name == "nt" else "")
    found = Path(directory) / executable if directory else shutil.which(executable)
    if found and Path(found).is_file():
        return Path(found)
    raise RuntimeError(
        f"{executable} is required for this check. "
        "Add Poppler to PATH or set TATSU_POPPLER_BIN to its bin directory."
    )
