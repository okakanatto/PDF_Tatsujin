"""Copy tested binaries into a fresh delivery folder and bind them to source/tests.

Documentation can be updated independently; product binaries and assets must
remain byte-identical. This does not publish the candidate or declare acceptance.
"""

import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess


def sha256(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def read(path):
    return json.loads(path.read_text(encoding="utf-8-sig"))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--candidate", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--regression", required=True, type=Path)
    parser.add_argument("--source-commit", required=True)
    parser.add_argument("--version", default="0.2.0-rc4")
    parser.add_argument("--report", default="M3_RC4_REPORT.md")
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    if Path(args.report).name != args.report or not (root / args.report).is_file():
        raise RuntimeError("Report must be a saved file in the checkout root")
    if not args.version or any(
        c not in "0123456789abcdefghijklmnopqrstuvwxyz.-" for c in args.version
    ):
        raise RuntimeError("Invalid version identifier")
    output = args.output.resolve()
    if output.parent != root / "dist" or output.exists():
        raise RuntimeError("Use a new direct child of this checkout's dist directory")
    source = subprocess.check_output(
        ["git", "rev-parse", args.source_commit + "^{commit}"], cwd=root, text=True
    ).strip()
    changed = subprocess.check_output(
        [
            "git",
            "diff",
            source,
            "--",
            "src",
            "tests",
            "CMakeLists.txt",
            "cmake",
            "vendor",
        ],
        cwd=root,
    )
    if changed:
        raise RuntimeError("Product source differs from the stated commit")
    suite = read(args.regression / "selftest.json")
    process = read(args.regression / "process-result.json")
    executable = sha256(args.candidate / "PDFTatsujin.exe")
    if (
        suite["failures"]
        or not suite["tests"]
        or any(test["status"] != "PASS" for test in suite["tests"])
        or process["exit_code"] != 0
        or not process["completed"]
        or process["filter"]
        or executable != process["exe_sha256"]
    ):
        raise RuntimeError("Candidate is not the successfully tested executable")
    for path in args.candidate.rglob("*"):
        if path.is_symlink() or path.is_junction():
            raise RuntimeError("Candidate contains a link")
    shutil.copytree(args.candidate, output)
    shutil.copytree(root / "docs", output / "docs", dirs_exist_ok=True)
    shutil.copytree(root / "licenses", output / "licenses", dirs_exist_ok=True)
    for name in (
        "README.md",
        "LICENSE",
        "ROADMAP.md",
        "dependency-lock.json",
        "M1_REPORT.md",
        "M3_REPORT.md",
        "M3_RC3_REPORT.md",
        "M3_RC4_REPORT.md",
    ):
        shutil.copyfile(root / name, output / name)
    shutil.copyfile(root / args.report, output / args.report)
    for path in args.candidate.rglob("*"):
        relative = path.relative_to(args.candidate)
        if path.is_file() and (
            path.suffix.lower() in (".exe", ".dll") or relative.parts[0] == "assets"
        ):
            if sha256(path) != sha256(output / relative):
                raise RuntimeError(f"Tested binary/resource changed: {relative}")
    manifest = {
        "version": args.version,
        "source_commit": source,
        "source_url": "https://github.com/okakanatto/PDF_Tatsujin/commit/" + source,
        "configuration": "Release, TATSU_ENABLE_SELFTEST=ON, compiler startup fix ON, test delay 0; Desktop x64 Release CRT",
        "platform": "Windows x64, Qt 6.9.3, MSVC 19.50",
        "executable_sha256": executable,
        "regression_tests_passed": len(suite["tests"]),
        "selftest_process_exit": process["exit_code"],
        "full_acceptance": "not declared; see " + args.report,
        "binary_publication": "pending publisher's MSVC redistribution eligibility confirmation",
        "files": [
            {
                "path": p.relative_to(output).as_posix(),
                "bytes": p.stat().st_size,
                "sha256": sha256(p),
            }
            for p in sorted(output.rglob("*"))
            if p.is_file()
        ],
    }
    (output / "build-manifest.json").write_text(
        json.dumps(manifest, indent=2) + "\n", encoding="utf-8"
    )
    print(f"Prepared {output.name}; executable {executable}")


if __name__ == "__main__":
    main()
