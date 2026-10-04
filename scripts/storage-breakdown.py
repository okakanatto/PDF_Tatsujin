from pathlib import Path
import argparse
import datetime, json

ROOT = Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser(
    description="Measure project and distribution file sizes."
)
parser.add_argument("--app-directory", type=Path, default=Path("dist/PDFTatsujin-M1"))
parser.add_argument(
    "--archive", type=Path, default=Path("dist/PDFTatsujin-M1-windows-x64.zip")
)
parser.add_argument(
    "--output", type=Path, default=Path("evidence/storage-breakdown.json")
)
args = parser.parse_args()
APP = ROOT / args.app_directory


def size(path):
    return (
        sum(p.stat().st_size for p in path.rglob("*") if p.is_file())
        if path.is_dir()
        else path.stat().st_size
    )


folders = {p.name: size(p) for p in ROOT.iterdir() if p.is_dir()}
parts = {
    "app_and_PDF4QT": 0,
    "Qt_and_plugins": 0,
    "native_and_MSVC": 0,
    "OCR_models": 0,
    "fonts": 0,
    "licenses_and_metadata": 0,
}
for p in APP.rglob("*"):
    if not p.is_file():
        continue
    rel = p.relative_to(APP)
    name = p.name
    if rel.parts[:2] == ("assets", "tessdata"):
        key = "OCR_models"
    elif rel.parts[:2] == ("assets", "fonts"):
        key = "fonts"
    elif name in ["PDFTatsujin.exe", "Pdf4QtLibCore.dll", "Pdf4QtLibWidgets.dll"]:
        key = "app_and_PDF4QT"
    elif name.startswith("Qt6") or rel.parts[0] in [
        "platforms",
        "styles",
        "imageformats",
        "iconengines",
        "generic",
        "networkinformation",
        "tls",
    ]:
        key = "Qt_and_plugins"
    elif p.suffix.lower() == ".dll":
        key = "native_and_MSVC"
    else:
        key = "licenses_and_metadata"
    parts[key] += p.stat().st_size
total = sum(folders.values()) + sum(
    p.stat().st_size for p in ROOT.iterdir() if p.is_file()
)
result = {
    "measured_at": datetime.datetime.now().astimezone().isoformat(),
    "logical_bytes_note": "File lengths, not NTFS allocated size; T: is a subst alias and counted only once. Existing system MSVC/Windows and shared test runtimes are outside this folder.",
    "project_bytes": total,
    "project_GB": total / 1e9,
    "limit_bytes": 20000000000,
    "remaining_bytes": 20000000000 - total,
    "folders": folders,
    "distribution_bytes": sum(parts.values()),
    "distribution_components": parts,
    "Qt_source_archives_bytes": size(ROOT / "dist/third-party-sources"),
}
archive = ROOT / args.archive
if archive.exists():
    result["zip_bytes"] = archive.stat().st_size
(ROOT / args.output).write_text(json.dumps(result, indent=2), encoding="utf-8")
assert total < 20000000000, "Project budget exceeded"
print(json.dumps(result, indent=2))
