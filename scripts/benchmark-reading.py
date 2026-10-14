"""Measure repeated real Qt PDF scrolling without controlling the user's desktop.

The independent harness links the product UI. Timer delays are event-loop
measurements, not physical-display frame rates. All raw samples are retained.
"""

import argparse
import hashlib
import json
import math
import os
from pathlib import Path
import statistics
import subprocess
import shutil
import time

import psutil


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def percentiles(values):
    values = sorted(values)
    if not values:
        raise RuntimeError("No latency samples")
    return {
        "samples": len(values),
        "median": statistics.median(values),
        "p95_nearest_rank": values[math.ceil(len(values) * 0.95) - 1],
        "maximum": values[-1],
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--harness", type=Path, required=True)
    parser.add_argument("--app-directory", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--runs", type=int, default=3, choices=[1, 3])
    parser.add_argument(
        "--renderer",
        default="product",
        choices=["product", "blend2d-single", "blend2d-multi"],
    )
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    product = args.app_directory.resolve()
    executable = args.harness.resolve()
    args.output.mkdir(parents=True, exist_ok=False)
    # Windows searches beside the EXE before PATH. Isolate the harness from the
    # build folder's mutable DLLs so measurements use the specified candidate.
    runner = args.output.resolve() / "runner"
    runner.mkdir()
    isolated_executable = runner / executable.name
    shutil.copy2(executable, isolated_executable)
    env = os.environ.copy()
    env["PATH"] = (
        str(product)
        + os.pathsep
        + str(
            Path(
                os.environ.get("SystemRoot", os.environ.get("SYSTEMROOT", "C:/Windows"))
            )
            / "System32"
        )
    )
    env["QT_QPA_PLATFORM"] = "offscreen"
    env["QT_PLUGIN_PATH"] = str(product)
    env["TATSU_ASSETS"] = str(product / "assets")
    env["TATSU_MEASURE_RENDERER"] = args.renderer
    result = {
        "mode": "Windows Qt offscreen, shared product UI, repeated continuous scrolling",
        "runs_per_document": args.runs,
        "laps_per_run": 3,
        "renderer_override": args.renderer,
        "harness_sha256": sha(executable),
        "harness_source_sha256": sha(root / "tests/reading_benchmark.cpp"),
        "wrapper_source_sha256": sha(Path(__file__)),
        "product_exe_sha256": sha(product / "PDFTatsujin.exe"),
        "UI_DLL_sha256": {
            name: sha(product / name)
            for name in ["Pdf4QtLibCore.dll", "Pdf4QtLibWidgets.dll", "Qt6Widgets.dll"]
        },
        "unexecuted": [
            "Physical-display FPS and input-to-display latency",
            "Cold OS file cache",
            "Hours-long soak",
            "Acrobat comparison",
        ],
        "documents": {},
    }
    (args.output / "measurement-environment.json").write_text(
        json.dumps(result, ensure_ascii=False, indent=2) + "\n",
        encoding="utf-8",
        newline="\n",
    )
    for name in ("D10-digital-100.pdf", "D10-image-50.pdf"):
        source = root / "fixtures" / name
        rows = []
        for index in range(args.runs):
            target = args.output.resolve() / f"{name}-{index}"
            samples = []
            started = time.perf_counter()
            with subprocess.Popen(
                [str(isolated_executable), str(source), str(target)],
                env=env,
                stdout=subprocess.DEVNULL,
                stderr=subprocess.PIPE,
                creationflags=subprocess.CREATE_NO_WINDOW,
            ) as process:
                while process.poll() is None:
                    if time.perf_counter() - started > 600:
                        process.kill()
                        raise RuntimeError(f"Reading benchmark timed out: {name}")
                    try:
                        memory = psutil.Process(process.pid).memory_info()
                        samples.append(
                            {
                                "process_seconds": time.perf_counter() - started,
                                "unix_ms": time.time_ns() // 1_000_000,
                                "rss_bytes": memory.rss,
                                "private_bytes": memory.private,
                            }
                        )
                    except psutil.Error:
                        pass
                    time.sleep(0.01)
                _, error = process.communicate()
                if process.returncode:
                    raise RuntimeError(
                        f"Harness exit {process.returncode}: "
                        + error.decode("utf-8", "replace")
                    )
            run = json.loads((target / "run.json").read_text("utf-8"))
            if run["status"] != "PASS" or not samples:
                raise RuntimeError("Incomplete measurement")
            (target / "process-memory.json").write_text(
                json.dumps(samples, indent=2) + "\n", encoding="utf-8", newline="\n"
            )
            # Sample inside each one-second idle interval, before Qt teardown.
            # Shared OS time aligns the two processes without an exit-time offset.
            process_elapsed = time.perf_counter() - started
            lap_memory = []
            for lap in run["laps"]:
                at = lap["idle_sample_unix_ms"]
                sample = min(samples, key=lambda s: abs(s["unix_ms"] - at))
                if abs(sample["unix_ms"] - at) > 100:
                    raise RuntimeError(
                        "No process-memory sample inside lap idle interval"
                    )
                lap_memory.append({"lap": lap["lap"], "target_unix_ms": at, **sample})
            row = {
                "run_directory": target.name,
                "status": run["status"],
                "pages": run["pages"],
                "visited_pages": run["visited_pages"],
                "elapsed_seconds": process_elapsed,
                "scroll_ready_ms": percentiles([s["ready_ms"] for s in run["steps"]]),
                "event_loop_interval_ms": percentiles(
                    [s["interval_ms"] for s in run["timer_ticks"]]
                ),
                "peak_rss_bytes": max(s["rss_bytes"] for s in samples),
                "peak_private_bytes": max(s["private_bytes"] for s in samples),
                "lap_idle_memory": lap_memory,
                "max_text_cache_bytes": run["max_text_cache_bytes"],
                "max_preview_cache_bytes": run["max_preview_cache_bytes"],
                "raw_run_sha256": sha(target / "run.json"),
                "raw_memory_sha256": sha(target / "process-memory.json"),
            }
            rows.append(row)
            print(name, index + 1, json.dumps(row), flush=True)
        result["documents"][name] = {"source_sha256": sha(source), "runs": rows}
    result["status"] = "PASS"
    (args.output / "summary.json").write_text(
        json.dumps(result, ensure_ascii=False, indent=2) + "\n",
        encoding="utf-8",
        newline="\n",
    )


if __name__ == "__main__":
    main()
