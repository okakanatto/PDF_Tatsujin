"""Exercise late Windows save failures with real locks on owned synthetic copies."""

import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess


def sha256(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--harness", required=True, type=Path)
    parser.add_argument("--app-directory", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()
    if os.name != "nt":
        raise RuntimeError("This probe requires real Windows file-sharing semantics")
    root = Path(__file__).resolve().parents[1]
    product = args.app_directory.resolve()
    manifest = json.loads(
        (product / "build-manifest.json").read_text(encoding="utf-8-sig")
    )
    changed = subprocess.check_output(
        ["git", "-C", str(root), "diff", manifest["source_commit"], "--", "src"],
        text=True,
        encoding="utf-8",
    )
    if changed:
        raise RuntimeError("Shared Document source differs from the candidate source")
    candidate_hash = sha256(product / "PDFTatsujin.exe")
    args.output.mkdir(parents=True, exist_ok=False)
    runner = args.output / "runner"
    runner.mkdir()
    executable = runner / args.harness.name
    shutil.copyfile(args.harness, executable)
    env = os.environ.copy()
    env.update(
        QT_QPA_PLATFORM="windows",
        QT_PLUGIN_PATH=str(product),
        TATSU_ASSETS=str(product / "assets"),
        PATH=str(product)
        + ";"
        + str(Path(env.get("SystemRoot", "C:/Windows")) / "System32"),
    )
    with (args.output / "stderr.txt").open("wb") as errors:
        result = subprocess.run(
            [
                str(executable.resolve()),
                str(root / "fixtures"),
                str((args.output / "run").resolve()),
            ],
            env=env,
            stdout=subprocess.DEVNULL,
            stderr=errors,
            timeout=60,
            creationflags=subprocess.CREATE_NO_WINDOW,
            check=False,
        )
    if result.returncode:
        raise RuntimeError(f"Probe failed: {result.returncode}; inspect stderr.txt")
    report = json.loads(
        (args.output / "run/filesystem-probe.json").read_text(encoding="utf-8")
    )
    if report.get("status") != "PASS" or len(report.get("tests", [])) != 2:
        raise RuntimeError("Probe did not complete both real sharing-lock cases")
    if sha256(product / "PDFTatsujin.exe") != candidate_hash:
        raise RuntimeError("Candidate changed during measurement")
    report.update(
        executable_sha256=candidate_hash,
        harness_sha256=sha256(executable),
        harness_source_sha256=sha256(root / "tests/filesystem_probe.cpp"),
        driver_source_sha256=sha256(Path(__file__)),
        product_source_commit=manifest["source_commit"],
        candidate_manifest_sha256=sha256(product / "build-manifest.json"),
        exit_code=result.returncode,
        limits="Separate harness links the shared Document implementation. "
        "Native Qt backend and real Windows locks; no visible window or GUI input. "
        "This does not simulate disk-full or establish IME/OS scaling acceptance.",
    )
    (args.output / "summary.json").write_text(
        json.dumps(report, ensure_ascii=False, indent=2) + "\n", encoding="utf-8"
    )
    print(json.dumps(report, ensure_ascii=False, indent=2))


if __name__ == "__main__":
    main()
