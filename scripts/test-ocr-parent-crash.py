"""Verify worker-owned OCR jobs survive an actual parent crash on Windows.

Only processes started by this script and its isolated TEMP are used. The worker
is briefly suspended after acquiring its real lock, to make the crash boundary
repeatable. This is not an exhaustive audit of every termination timing.
"""

import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import time

import psutil


def sha256(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def wait_for(condition, timeout):
    end = time.monotonic() + timeout
    while time.monotonic() < end:
        value = condition()
        if value:
            return value
        time.sleep(0.02)
    raise RuntimeError("Timed out waiting for the owned OCR process or lock")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--app-directory", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    out = args.output.resolve()
    if not out.is_relative_to(root):
        raise RuntimeError("Use a new directory inside this checkout")
    out.mkdir(exist_ok=False, parents=True)
    temporary = out / "tmp"
    temporary.mkdir()
    executable = args.app_directory.resolve() / "PDFTatsujin.exe"
    fixture = root / "fixtures/D05.pdf"
    original = sha256(fixture)
    environment = os.environ.copy()
    environment.update(
        TMP=str(temporary),
        TEMP=str(temporary),
        QT_QPA_PLATFORM="offscreen",
        PATH=str(Path(environment.get("SystemRoot", "C:/Windows")) / "System32"),
        TATSU_TEST_FILTER="A05_A08_same_window_OCR_search_copy_save",
    )
    for key in ("QT_PLUGIN_PATH", "TATSU_ASSETS", "TATSU_UI_REVIEW"):
        environment.pop(key, None)
    worker = None
    suspended = False
    records = []

    def probe(name):
        probe_env = environment.copy()
        probe_env.pop("TATSU_TEST_FILTER", None)
        result = subprocess.run(
            [
                str(executable),
                "--measure",
                str(root / "fixtures/D01.pdf"),
                str(out / (name + ".json")),
            ],
            env=probe_env,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.PIPE,
            creationflags=subprocess.CREATE_NO_WINDOW,
            timeout=45,
            check=True,
        )
        records.append({"probe": name, "exit_code": result.returncode})

    with (out / "parent-stderr.txt").open("wb") as error:
        parent = subprocess.Popen(
            [
                str(executable),
                "--selftest",
                str(root / "fixtures"),
                str(out / "parent"),
            ],
            env=environment,
            stdout=subprocess.DEVNULL,
            stderr=error,
            creationflags=subprocess.CREATE_NO_WINDOW,
        )
        try:

            def active_job():
                if parent.poll() is not None:
                    raise RuntimeError("Test parent ended before the crash boundary")
                children = psutil.Process(parent.pid).children()
                for candidate in temporary.glob("pdf-tatsujin-job-*"):
                    if (candidate / "worker.lock").is_file() and children:
                        return candidate, children[0]
                return None

            job, worker = wait_for(active_job, 60)
            if Path(worker.exe()).resolve() != executable:
                raise RuntimeError("Unexpected child executable; do not suspend it")
            worker.suspend()
            suspended = True
            snapshot = sha256(job / "input.pdf")
            parent.kill()
            parent.wait(timeout=10)
            probe("active-worker-startup")
            if not job.is_dir() or sha256(job / "input.pdf") != snapshot:
                raise RuntimeError("Startup removed or changed the active worker job")
            worker.resume()
            suspended = False
            worker.wait(timeout=120)
            if (
                not (job / "result.pdf").is_file()
                or not (job / "report.json").is_file()
            ):
                raise RuntimeError("Orphaned worker did not complete its local output")
            worker_report = json.loads(
                (job / "report.json").read_text(encoding="utf-8")
            )
            if not worker_report.get("pages"):
                raise RuntimeError("Worker did not report any completed pages")
            probe("finished-worker-startup")
            if job.exists():
                raise RuntimeError("Startup did not reclaim the finished orphan job")
            if sha256(fixture) != original:
                raise RuntimeError("Original fixture changed")
            result = {
                "status": "PASS",
                "executable_sha256": sha256(executable),
                "real_parent_terminated": True,
                "live_worker_job_preserved": True,
                "worker_output_completed": True,
                "finished_orphan_removed": True,
                "fixture_unchanged": True,
                "worker_report": worker_report,
                "probes": records,
                "scope": "Packaged Windows processes, real file locks, isolated TEMP; "
                "worker suspended briefly to stabilize the tested crash boundary",
                "unexecuted": ["all possible parent/worker termination timings"],
            }
            (out / "result.json").write_text(
                json.dumps(result, indent=2) + "\n", encoding="utf-8"
            )
            print(json.dumps(result, indent=2))
        finally:
            if worker is not None and worker.is_running():
                if suspended:
                    worker.resume()
                worker.kill()
                worker.wait(timeout=10)
            if parent.poll() is None:
                parent.kill()
                parent.wait(timeout=10)


if __name__ == "__main__":
    main()
