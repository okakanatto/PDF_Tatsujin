"""Measure real Qt PDF-viewer startup with a separate offscreen process per run.

This is not physical-display frame timing or an Acrobat comparison. No user's
desktop is controlled. All samples and the exact executable/input hashes remain
in a new output directory. Process elapsed includes the 250 ms observation.
"""

import argparse
import hashlib
import json
import os
from pathlib import Path
import statistics
import subprocess
import time

import psutil


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--app-directory", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    executable = (args.app_directory / "PDFTatsujin.exe").resolve()
    args.output.mkdir(parents=True, exist_ok=False)
    env = os.environ.copy()
    env["PATH"] = str(Path(os.environ["SystemRoot"]) / "System32")
    env["QT_QPA_PLATFORM"] = "offscreen"
    env.pop("QT_PLUGIN_PATH", None)
    env.pop("TATSU_ASSETS", None)
    result = {
        "mode": "Windows Qt offscreen, real PDFWidget rendering, separate processes",
        "cold_start": "未実行: OS file cache was not flushed",
        "physical_display_fps": "未実行",
        "Acrobat_comparison": "未実行",
        "samples_per_document": 3,
        "executable_sha256": hashlib.sha256(executable.read_bytes()).hexdigest(),
        "documents": {},
    }
    for name in ("D01.pdf", "D10-digital-100.pdf", "D10-image-50.pdf"):
        source = root / "fixtures" / name
        rows = []
        for index in range(3):
            target = args.output.resolve() / f"{name}-{index}.json"
            started = time.perf_counter()
            with subprocess.Popen(
                [str(executable), "--measure", str(source), str(target)],
                env=env,
                stdout=subprocess.DEVNULL,
                stderr=subprocess.PIPE,
                creationflags=subprocess.CREATE_NO_WINDOW,
            ) as process:
                peak_rss = peak_private = 0
                while process.poll() is None:
                    if time.perf_counter() - started > 60:
                        process.kill()
                        raise RuntimeError(f"Viewer measurement timed out: {name}")
                    try:
                        parent = psutil.Process(process.pid)
                        memory = [parent.memory_info()] + [
                            child.memory_info()
                            for child in parent.children(recursive=True)
                        ]
                        peak_rss = max(peak_rss, sum(item.rss for item in memory))
                        peak_private = max(
                            peak_private, sum(item.private for item in memory)
                        )
                    except psutil.Error:
                        pass
                    time.sleep(0.01)
                _, error = process.communicate()
                if process.returncode:
                    raise RuntimeError(
                        f"Viewer exit {process.returncode}: {error.decode('utf-8', 'replace')}"
                    )
            row = json.loads(target.read_text(encoding="utf-8"))
            row.update(
                {
                    "process_elapsed_seconds": time.perf_counter() - started,
                    "process_group_peak_rss_bytes": peak_rss,
                    "process_group_peak_private_bytes": peak_private,
                }
            )
            rows.append(row)
            print(name, index + 1, row["first_readable_ms"], "ms", flush=True)
        result["documents"][name] = {
            "input_sha256": hashlib.sha256(source.read_bytes()).hexdigest(),
            "runs": rows,
            "median_first_readable_ms": statistics.median(
                row["first_readable_ms"] for row in rows
            ),
        }
    (args.output / "summary.json").write_text(
        json.dumps(result, ensure_ascii=False, indent=2) + "\n", encoding="utf-8"
    )


if __name__ == "__main__":
    main()
