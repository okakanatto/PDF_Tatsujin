"""Archive a packaged app with Qt sources and verify every archived file.

Refuses to overwrite an existing ZIP or verification record. Binary packages stay
outside Git; the report includes exact executable and archive hashes.
"""

import argparse
import hashlib
import json
from pathlib import Path
import zipfile


def sha256(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--app-directory", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--verification", required=True, type=Path)
    args = parser.parse_args()
    app = args.app_directory.resolve()
    dist = app.parent
    sources = dist / "third-party-sources"
    if not (app / "PDFTatsujin.exe").is_file() or not sources.is_dir():
        raise RuntimeError(
            "Packaged application or corresponding Qt sources are missing"
        )
    if args.output.exists() or args.verification.exists():
        raise RuntimeError(
            "Preserve existing archives and evidence; use new output paths"
        )
    files = sorted(
        path
        for directory in (app, sources)
        for path in directory.rglob("*")
        if path.is_file()
    )
    if any(path.is_symlink() for path in files):
        raise RuntimeError("Do not include symbolic links in the portable archive")
    with zipfile.ZipFile(
        args.output, "x", zipfile.ZIP_DEFLATED, compresslevel=6
    ) as archive:
        for path in files:
            archive.write(path, path.relative_to(dist).as_posix())
    with zipfile.ZipFile(args.output) as archive:
        expected = {path.relative_to(dist).as_posix() for path in files}
        if set(archive.namelist()) != expected:
            raise RuntimeError("Archive entries differ from the packaged source")
        for path in files:
            # Reading the complete member also validates its ZIP CRC.
            actual = hashlib.sha256(
                archive.read(path.relative_to(dist).as_posix())
            ).hexdigest()
            if actual != sha256(path):
                raise RuntimeError(f"Archived bytes differ: {path}")
    result = {
        "all_files_match": True,
        "files": len(files),
        "exe_sha256": sha256(app / "PDFTatsujin.exe"),
        "zip_sha256": sha256(args.output),
        "zip_bytes": args.output.stat().st_size,
        "method": "Every ZIP member checked by CRC and SHA-256 against its packaged source",
    }
    args.verification.write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(result, indent=2))


if __name__ == "__main__":
    main()
