"""Repeat startup and real same-window OCR flows, recording process-group memory.

Qt offscreen measurements do not establish physical-display latency, cold-cache
startup, or superiority to Acrobat. Existing acceptance operations are reused.
"""

import argparse
import hashlib
import json
import os
from pathlib import Path
import platform
import statistics
import subprocess
import time
import psutil


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--app-directory", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=False)
    root = Path(__file__).resolve().parents[1]
    executable = args.app_directory.resolve() / "PDFTatsujin.exe"
    env = os.environ.copy()
    env["PATH"] = str(
        Path(env.get("SystemRoot", env.get("SYSTEMROOT", "C:/Windows"))) / "System32"
    )
    env["QT_QPA_PLATFORM"] = "offscreen"
    for name in (
        "QT_PLUGIN_PATH",
        "TATSU_ASSETS",
        "TATSU_TEST_FILTER",
        "TATSU_UI_REVIEW",
    ):
        env.pop(name, None)
    result = {
        "executable_sha256": hashlib.sha256(executable.read_bytes()).hexdigest(),
        "platform": platform.platform(),
        "CPU": platform.processor(),
        "RAM_bytes": psutil.virtual_memory().total,
        "runs_per_case": 3,
        "memory_scope": "parent plus recursive OCR children, sampled every 20ms",
        "mode": "Windows real Qt application, offscreen; OS file cache not cleared",
        "unexecuted": ["physical-display FPS", "Acrobat comparison", "hours-long soak"],
        "cases": {},
    }

    def run(arguments, case_env):
        started = time.perf_counter()
        samples = []
        with subprocess.Popen(
            [str(executable), *map(str, arguments)],
            env=case_env,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.PIPE,
            creationflags=subprocess.CREATE_NO_WINDOW,
        ) as process:
            while process.poll() is None:
                if time.perf_counter() - started > 300:
                    process.kill()
                    raise RuntimeError("Measurement timeout")
                try:
                    parent = psutil.Process(process.pid)
                    children = parent.children(recursive=True)
                    samples.append(
                        {
                            "seconds": time.perf_counter() - started,
                            "rss_bytes": sum(
                                p.memory_info().rss for p in [parent, *children]
                            ),
                            "processes": 1 + len(children),
                        }
                    )
                except psutil.Error:
                    pass
                time.sleep(0.02)
            _, error = process.communicate()
        if process.returncode or not samples:
            raise RuntimeError(
                f"Measurement failed {process.returncode}: {error.decode('utf-8', 'replace')}"
            )
        return {
            "elapsed_seconds": time.perf_counter() - started,
            "peak_process_group_rss_bytes": max(row["rss_bytes"] for row in samples),
            "max_process_count": max(row["processes"] for row in samples),
            "samples": samples,
        }

    for name in ("D01.pdf", "D10-digital-100.pdf", "D10-image-50.pdf"):
        rows = []
        for index in range(3):
            report = args.output.resolve() / f"{name}-{index}.json"
            row = run(["--measure", root / "fixtures" / name, report], env)
            row.update(json.loads(report.read_text("utf-8")))
            (args.output / f"{name}-{index}-memory.json").write_text(
                json.dumps(row.pop("samples")), "utf-8"
            )
            rows.append(row)
        result["cases"][name] = {
            "runs": rows,
            "median_first_readable_ms": statistics.median(
                row["first_readable_ms"] for row in rows
            ),
            "peak_rss_bytes": max(row["peak_process_group_rss_bytes"] for row in rows),
        }
    rows = []
    ocr_env = dict(env, TATSU_TEST_FILTER="A05_A08_same_window")
    for index in range(3):
        directory = args.output.resolve() / f"same-window-OCR-{index}"
        row = run(["--selftest", root / "fixtures", directory], ocr_env)
        (args.output / f"OCR-{index}-memory.json").write_text(
            json.dumps(row.pop("samples")), "utf-8"
        )
        suite = json.loads((directory / "selftest.json").read_text("utf-8"))
        if suite["failures"] or len(suite["tests"]) != 1:
            raise RuntimeError("Incomplete same-window OCR measurement")
        row["operation"] = suite["tests"][0]
        rows.append(row)
    result["cases"]["same-window-OCR"] = {
        "runs": rows,
        "median_seconds": statistics.median(row["elapsed_seconds"] for row in rows),
        "peak_rss_bytes": max(row["peak_process_group_rss_bytes"] for row in rows),
    }
    (args.output / "measurements.json").write_text(
        json.dumps(result, ensure_ascii=False, indent=2) + "\n", "utf-8"
    )
    print(
        json.dumps(
            {
                name: {key: value for key, value in case.items() if key != "runs"}
                for name, case in result["cases"].items()
            },
            indent=2,
        )
    )


if __name__ == "__main__":
    main()
