"""Check packaged PE dependencies, assets and notices without loading product DLLs.

dumpbin is the Microsoft toolchain's PE inspector. OS API contracts are resolved
with LOAD_LIBRARY_SEARCH_SYSTEM32; development PATH is never a dependency source.
This is an audit of the current Windows host, not a clean-Windows acceptance test.
"""

import argparse
import ctypes
from ctypes import wintypes
import hashlib
import json
import os
from pathlib import Path
import re
import struct
import subprocess


def sha256(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--app-directory", required=True, type=Path)
    parser.add_argument("--dumpbin", required=True, type=Path)
    parser.add_argument("--crt-directory", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    app = args.app_directory.resolve()
    if args.output.exists():
        raise RuntimeError("Preserve earlier audit results")
    kernel = ctypes.WinDLL("kernel32", use_last_error=True)
    load = kernel.LoadLibraryExW
    load.argtypes = [wintypes.LPCWSTR, wintypes.HANDLE, wintypes.DWORD]
    load.restype = wintypes.HMODULE
    free = kernel.FreeLibrary
    free.argtypes = [wintypes.HMODULE]
    system = Path(os.environ["SystemRoot"]) / "System32"
    rows = []
    binaries = sorted(p for p in app.rglob("*") if p.suffix.lower() in (".dll", ".exe"))
    for binary in binaries:
        data = binary.read_bytes()
        pe = struct.unpack_from("<I", data, 0x3C)[0]
        if data[:2] != b"MZ" or data[pe : pe + 4] != b"PE\0\0":
            raise RuntimeError(f"Invalid PE: {binary.name}")
        if struct.unpack_from("<H", data, pe + 4)[0] != 0x8664:
            raise RuntimeError(f"Not x64: {binary.name}")
        inspection = subprocess.check_output(
            [args.dumpbin, "/DEPENDENTS", binary],
            text=True,
            encoding="utf-8",
            errors="replace",
            creationflags=subprocess.CREATE_NO_WINDOW,
        )
        dependencies = {}
        for name in sorted(
            set(re.findall(r"^\s+(\S+\.dll)\s*$", inspection, re.M | re.I))
        ):
            local = next(
                (p for p in (binary.parent / name, app / name) if p.is_file()), None
            )
            if local:
                source = "package:" + local.relative_to(app).as_posix()
            elif (system / name).is_file():
                source = "Windows:System32"
            elif name.lower().startswith(("api-ms-", "ext-ms-")):
                handle = load(name, None, 0x800)
                if not handle:
                    raise ctypes.WinError(ctypes.get_last_error())
                free(handle)
                source = "Windows:API-contract-resolved"
            else:
                raise RuntimeError(f"Missing dependency: {binary.name} -> {name}")
            dependencies[name] = source
        if not dependencies:
            raise RuntimeError(f"No PE imports inspected: {binary.name}")
        rows.append(
            {
                "file": binary.relative_to(app).as_posix(),
                "sha256": sha256(binary),
                "machine": "x64",
                "dependencies": dependencies,
            }
        )
    assets, notices = 0, 0
    for folder, counter in (("assets", "assets"), ("licenses", "notices")):
        for original in (root / folder).rglob("*"):
            if not original.is_file():
                continue
            relative = original.relative_to(root)
            copy = app / relative
            if not copy.is_file() or sha256(copy) != sha256(original):
                raise RuntimeError(f"Packaged resource mismatch: {relative}")
            if counter == "assets":
                assets += 1
            else:
                notices += 1
    for notice in (root / "tools/vcpkg/installed/x64-windows/share").glob(
        "*/copyright"
    ):
        copy = app / "licenses" / (notice.parent.name + ".txt")
        if not copy.is_file() or sha256(copy) != sha256(notice):
            raise RuntimeError(f"Third-party notice mismatch: {notice.parent.name}")
        notices += 1
    if sha256(app / "licenses/PDF4QT-MIT.txt") != sha256(
        root / "vendor/PDF4QT/LICENSE"
    ):
        raise RuntimeError("PDF4QT notice mismatch")
    crt = args.crt_directory.resolve()
    if {p.lower() for p in crt.parts} & {"onecore", "debug_nonredist"}:
        raise RuntimeError("Release desktop audit cannot use OneCore/debug CRT")
    runtime = json.loads((app / "runtime-origin.json").read_text(encoding="utf-8-sig"))
    runtime_files = list(crt.glob("*.dll"))
    if not runtime_files or {p.name for p in runtime_files} != {
        p["file"] for p in runtime["files"]
    }:
        raise RuntimeError("Runtime origin manifest incomplete")
    for original in runtime_files:
        if sha256(original) != sha256(app / original.name):
            raise RuntimeError(f"Runtime differs from desktop redist: {original.name}")
    result = {
        "status": "PASS",
        "executable_sha256": sha256(app / "PDFTatsujin.exe"),
        "binaries": rows,
        "binary_count": len(rows),
        "assets_matched": assets,
        "notices_matched": notices + 1,
        "desktop_release_CRT": runtime,
        "scope": "Static PE imports including delay imports, bundled assets and notices; current Windows System32/API contracts. Does not establish clean Windows, all dynamic plugin use, or legal approval.",
    }
    args.output.write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
    print(f"PASS: {len(rows)} x64 PE files, {assets} assets, {notices + 1} notices")


if __name__ == "__main__":
    main()
