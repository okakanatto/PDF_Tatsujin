"""Terminate only our own PDF save worker before destination replacement.

The worker uses the actual Document::save implementation without a product hook.
This covers observed pre-replacement boundaries, not power loss, every possible
timing, or recovery of unsaved in-memory work after process termination.
"""

import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import time

import psutil
from pypdf import PdfReader


def sha256(path):
    return hashlib.sha256(path.read_bytes()).hexdigest() if path.exists() else ""


def wait_for(condition, process, timeout):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        value = condition()
        if value:
            return value
        if process.poll() is not None:
            raise RuntimeError("Owned save worker ended before the observed boundary")
        time.sleep(0.001)
    raise RuntimeError("Owned save worker exceeded its bounded preparation")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--harness", required=True, type=Path)
    parser.add_argument("--app-directory", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument(
        "--check-cleanup",
        action="store_true",
        help="After termination, run the new owned-candidate cleanup and verify the destination.",
    )
    parser.add_argument(
        "--boundary",
        choices=("candidate-created", "candidate-written"),
        default="candidate-created",
    )
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    output = args.output.resolve()
    if output.parent != root / "evidence":
        raise RuntimeError(
            "Use a new direct child of this checkout's evidence directory"
        )
    output.mkdir(exist_ok=False)
    executable = output / "PDFTatsujinFilesystemProbe.exe"
    shutil.copyfile(args.harness, executable)
    product = args.app_directory.resolve()
    environment = os.environ.copy()
    environment.update(
        PATH=str(product)
        + os.pathsep
        + str(Path(os.environ.get("SystemRoot", "C:/Windows")) / "System32"),
        TATSU_ASSETS=str(product / "assets"),
        QT_PLUGIN_PATH=str(product),
        QT_QPA_PLATFORM="offscreen",
    )
    fixtures = root / "fixtures"
    inputs = {name: sha256(fixtures / name) for name in ("D01.pdf", "D03.pdf")}
    source_hashes = {
        name: sha256(root / name)
        for name in (
            "tests/filesystem_probe.cpp",
            "tests/save_interruption_worker.cpp",
            "src/document.cpp",
            "src/pdf_io.cpp",
            "src/save_candidate.cpp",
            "src/private_temp.cpp",
        )
    }
    wrapper_hash = sha256(Path(__file__))
    rows = []
    for case in ("new", "existing"):
        work = output / case
        with (output / f"{case}-stderr.log").open("wb") as errors:
            process = subprocess.Popen(
                [
                    str(executable),
                    "--interrupt-save-worker",
                    str(fixtures),
                    str(work),
                    case,
                ],
                env=environment,
                stdout=subprocess.DEVNULL,
                stderr=errors,
                creationflags=subprocess.CREATE_NO_WINDOW,
            )
            owner = psutil.Process(process.pid)
            suspended = False
            try:
                wait_for(lambda: (work / "prepared.json").exists(), process, 60)
                prepared = json.loads(
                    (work / "prepared.json").read_text(encoding="utf-8")
                )
                if sha256(work / "expected.pdf") != prepared["expected_sha256"]:
                    raise RuntimeError("Prepared expected PDF changed")
                if len(PdfReader(work / "expected.pdf").pages) != 8:
                    raise RuntimeError("Prepared expected PDF is invalid")
                (work / "begin").write_bytes(b"start owned save")

                def observed_candidate():
                    path = next(work.glob(".pdf-tatsujin-*/candidate.pdf"), None)
                    if path and (
                        args.boundary == "candidate-created"
                        or path.stat().st_size >= prepared["expected_bytes"]
                    ):
                        return path
                    return None

                candidate = wait_for(observed_candidate, process, 30)
                if Path(owner.exe()).resolve() != executable.resolve():
                    raise RuntimeError(
                        "Unexpected executable; refusing process manipulation"
                    )
                owner.suspend()
                suspended = True
                destination = work / "destination.pdf"
                if sha256(destination) != prepared["baseline_sha256"]:
                    raise RuntimeError(
                        "Save already replaced its destination; boundary not captured"
                    )
                candidate_bytes = candidate.stat().st_size
                if (
                    args.boundary == "candidate-written"
                    and sha256(candidate) != prepared["expected_sha256"]
                ):
                    raise RuntimeError(
                        "Written candidate is not the prepared complete PDF"
                    )
                process.kill()
                process.wait(timeout=10)
                suspended = False
                if sha256(destination) != prepared["baseline_sha256"]:
                    raise RuntimeError("Interrupted save changed its destination")
                if case == "existing" and len(PdfReader(destination).pages) != 1:
                    raise RuntimeError(
                        "Existing PDF is unreadable after interrupted save"
                    )
                row = {
                    "case": case,
                    "status": "PASS",
                    "actual_owned_process_terminated": True,
                    "boundary": args.boundary + "; destination not yet replaced",
                    "complete_candidate_verified": args.boundary == "candidate-written",
                    "candidate_bytes_at_suspension": candidate_bytes,
                    "expected_pdf_bytes": prepared["expected_bytes"],
                    "destination_sha256": sha256(destination),
                    "destination_unchanged_or_absent": True,
                    "orphan_private_candidate_retained": candidate.exists(),
                }
                if args.check_cleanup:
                    cleanup_report = work / "cleanup-result.json"
                    subprocess.run(
                        [
                            str(executable),
                            "--cleanup-save-candidates",
                            str(work),
                            str(cleanup_report),
                        ],
                        env=environment,
                        stdout=subprocess.DEVNULL,
                        stderr=errors,
                        creationflags=subprocess.CREATE_NO_WINDOW,
                        timeout=10,
                        check=True,
                    )
                    cleanup = json.loads(cleanup_report.read_text(encoding="utf-8"))
                    if (
                        not cleanup["actual_save_retry_and_reopen"]
                        or len(PdfReader(work / "retry.pdf").pages) != 8
                        or sha256(work / "expected.pdf") != prepared["expected_sha256"]
                    ):
                        raise RuntimeError(
                            "Actual save retry failed or changed the expected PDF"
                        )
                    if (
                        cleanup["removed"] != [candidate.parent.name]
                        or candidate.parent.exists()
                    ):
                        raise RuntimeError(
                            "Interrupted owned candidate was not cleaned exactly"
                        )
                    if sha256(destination) != prepared["baseline_sha256"]:
                        raise RuntimeError("Cleanup changed the saved destination")
                    row["next_cleanup_removed_only_owned_candidate"] = True
                    row["actual_save_retry_reopened_eight_pages"] = True
                    row["orphan_private_candidate_retained"] = False
                rows.append(row)
            finally:
                if process.poll() is None:
                    if suspended:
                        owner.resume()
                    process.kill()
                    process.wait(timeout=10)
    if any(sha256(fixtures / name) != value for name, value in inputs.items()):
        raise RuntimeError("Frozen source input changed")
    result = {
        "status": "PASS",
        "harness_sha256": sha256(executable),
        "product_exe_sha256": sha256(product / "PDFTatsujin.exe"),
        "wrapper_sha256": wrapper_hash,
        "sources_sha256": source_hashes,
        "inputs_sha256": inputs,
        "tests": rows,
        "scope": "Actual owned Windows process termination; synthetic PDFs; API harness, no GUI",
        "unexecuted": [
            "power loss",
            "all termination timings",
            "post-replacement crash",
        ],
        "cleanup_requested": args.check_cleanup,
        "limits": (
            "Unsaved in-memory work is not recovered after a crash. "
            + (
                "Only marked abandoned candidates were removed after actual termination."
                if args.check_cleanup
                else "Private candidates survive abrupt termination and are retained as evidence."
            )
        ),
    }
    (output / "result.json").write_text(
        json.dumps(result, ensure_ascii=False, indent=2) + "\n", encoding="utf-8"
    )
    print(json.dumps(result, ensure_ascii=False, indent=2))


if __name__ == "__main__":
    main()
