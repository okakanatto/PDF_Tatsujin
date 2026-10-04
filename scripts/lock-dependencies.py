from pathlib import Path
import hashlib, importlib.metadata, json, subprocess

ROOT = Path(__file__).resolve().parents[1]


def git(path):
    return subprocess.check_output(
        ["git", "-C", str(path), "rev-parse", "HEAD"], text=True
    ).strip()


files = {
    str(p.relative_to(ROOT)).replace("\\", "/"): {
        "bytes": p.stat().st_size,
        "sha256": hashlib.sha256(p.read_bytes()).hexdigest(),
    }
    for directory in ["assets", "dist/third-party-sources"]
    for p in (ROOT / directory).rglob("*")
    if p.is_file()
}
packages = []
status = ROOT / "tools/vcpkg/installed/vcpkg/status"
if status.exists():
    for block in status.read_text(encoding="utf-8").split("\n\n"):
        fields = dict(
            line.split(": ", 1) for line in block.splitlines() if ": " in line
        )
        if fields.get("Status") == "install ok installed":
            packages.append(fields)
tests = {}
for p in ["pypdf", "pypdfium2", "reportlab", "Pillow", "fonttools", "psutil"]:
    try:
        tests[p] = importlib.metadata.version(p)
    except importlib.metadata.PackageNotFoundError:
        tests[p] = "unavailable in this interpreter"
data = {
    "pdf4qt": {
        "tag": "v1.6.0.0",
        "commit": git(ROOT / "vendor/PDF4QT"),
        "source": "https://github.com/JakubMelka/PDF4QT",
        "license": "MIT",
    },
    "vcpkg": {
        "commit": git(ROOT / "tools/vcpkg"),
        "source": "https://github.com/microsoft/vcpkg",
    },
    "qt": {
        "version": "6.9.3",
        "arch": "win64_msvc2022_64",
        "license": "LGPL-3.0, with per-component notices; see licenses/Qt",
    },
    "models": json.loads(
        (ROOT / "assets/revisions.json").read_text(encoding="utf-8-sig")
    ),
    "files": files,
    "vcpkg_packages": packages,
    "test_only_python": tests,
    "storage_budget_bytes": 20000000000,
}
(ROOT / "dependency-lock.json").write_text(
    json.dumps(data, ensure_ascii=False, indent=2), encoding="utf-8"
)
print(
    "Recorded",
    len(packages),
    "installed package records and",
    len(files),
    "asset/source hashes",
)
