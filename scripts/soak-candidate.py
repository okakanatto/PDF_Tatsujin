"""Measure a bounded, repeatable product-UI soak without keeping all events in RAM."""

import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import statistics
import subprocess
import time

import psutil


def sha256(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--harness", required=True, type=Path)
    parser.add_argument("--app-directory", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--seconds", type=int, default=900)
    parser.add_argument(
        "--source-commit",
        help="Optional committed product/harness source to verify before launch",
    )
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    source_commit = None
    if args.source_commit:
        source_commit = subprocess.check_output(
            ["git", "rev-parse", args.source_commit + "^{commit}"], cwd=root, text=True
        ).strip()
        if subprocess.check_output(
            [
                "git",
                "diff",
                source_commit,
                "--",
                "src",
                "tests",
                "CMakeLists.txt",
                "cmake",
                "vendor",
            ],
            cwd=root,
        ):
            raise RuntimeError("Product/harness source differs from the stated commit")
    args.output.mkdir(exist_ok=False, parents=True)
    runner = args.output / "runner"
    runner.mkdir()
    executable = runner / args.harness.name
    shutil.copyfile(args.harness, executable)
    harness_sources = {
        name: sha256(root / "tests" / name)
        for name in ("soak_test.cpp", "memory_diagnostics.cpp", "memory_diagnostics.h")
    }
    wrapper_sha = sha256(Path(__file__))
    product = args.app_directory.resolve()
    env = os.environ.copy()
    env.update(
        QT_QPA_PLATFORM="offscreen",
        QT_PLUGIN_PATH=str(product),
        TATSU_ASSETS=str(product / "assets"),
        PATH=str(product)
        + ";"
        + str(Path(env.get("SystemRoot", "C:/Windows")) / "System32"),
    )
    samples = []
    start = time.monotonic()
    with (args.output / "stderr.txt").open("wb") as errors:
        with subprocess.Popen(
            [
                str(executable.resolve()),
                str(root / "fixtures"),
                str((args.output / "run").resolve()),
                str(args.seconds),
            ],
            env=env,
            stdout=subprocess.DEVNULL,
            stderr=errors,
            creationflags=subprocess.CREATE_NO_WINDOW,
        ) as process:
            owner = psutil.Process(process.pid)
            while process.poll() is None:
                if time.monotonic() - start > args.seconds + 180:
                    process.kill()
                    raise RuntimeError("Soak exceeded its bounded duration")
                try:
                    memory = owner.memory_info()
                    samples.append(
                        {
                            "unix_ms": round(time.time() * 1000),
                            "rss": memory.rss,
                            "private": memory.private,
                            "threads": owner.num_threads(),
                            "handles": owner.num_handles(),
                        }
                    )
                except psutil.NoSuchProcess:
                    pass
                time.sleep(0.2)
            if process.returncode:
                raise RuntimeError(f"Soak failed: {process.returncode}; inspect stderr")
    result = json.loads((args.output / "run/result.json").read_text(encoding="utf-8"))
    if result.get("mode") != env.get("TATSU_SOAK_MODE", "mixed"):
        raise RuntimeError("Soak mode did not match the requested workload")
    cycles = [
        json.loads(line)
        for line in (args.output / "run/cycles.jsonl")
        .read_text(encoding="utf-8")
        .splitlines()
    ]
    diagnostics = [c.get("heap_diagnostic") for c in cycles]
    if "TATSU_HEAP_DIAGNOSTICS" in env:
        if not all(d and d.get("status") == "PASS" for d in diagnostics):
            raise RuntimeError("Default-heap diagnosis did not complete every cycle")
    elif any(diagnostics):
        raise RuntimeError("Unexpected heap diagnostics in a normal soak")
    memory_diagnostics = [c.get("memory_diagnostic") for c in cycles]
    if "TATSU_MEMORY_DIAGNOSTICS" in env:
        if not all(d and d.get("status") == "PASS" for d in memory_diagnostics):
            raise RuntimeError(
                "Own-process memory diagnosis did not complete every cycle"
            )
    elif any(memory_diagnostics):
        raise RuntimeError("Unexpected memory diagnostics in a normal soak")
    if "TATSU_TRIM_DIAGNOSTICS" in env:
        if not all(
            result.get(key, {}).get("status") == "PASS"
            for key in ("before_trim", "heap_trim", "after_trim")
        ):
            raise RuntimeError("Heap-cache comparison did not complete")
    elif any(key in result for key in ("before_trim", "heap_trim", "after_trim")):
        raise RuntimeError("Unexpected heap-cache intervention in a normal soak")
    pixmap_keys = (
        "before_pixmap_clear",
        "after_pixmap_clear",
        "before_pixmap_heap",
        "after_pixmap_heap",
    )
    if "TATSU_PIXMAP_DIAGNOSTICS" in env:
        if not all(result.get(key, {}).get("status") == "PASS" for key in pixmap_keys):
            raise RuntimeError("Pixmap-cache comparison did not complete")
    elif any(key in result for key in pixmap_keys):
        raise RuntimeError("Unexpected pixmap-cache intervention in a normal soak")
    idle_samples = []
    for cycle in cycles:
        preceding = [s for s in samples if s["unix_ms"] <= cycle["idle_unix_ms"]]
        sample = max(preceding, key=lambda s: s["unix_ms"])
        if cycle["idle_unix_ms"] - sample["unix_ms"] > 300:
            raise RuntimeError(
                "No process sample inside the closed-window idle interval"
            )
        idle_samples.append(sample)
    result.update(
        executable_sha256=sha256(product / "PDFTatsujin.exe"),
        harness_sha256=sha256(executable),
        harness_source_sha256=harness_sources["soak_test.cpp"],
        harness_sources_sha256=harness_sources,
        wrapper_source_sha256=wrapper_sha,
        source_commit=source_commit,
        heap_diagnostics="TATSU_HEAP_DIAGNOSTICS" in env,
        memory_diagnostics="TATSU_MEMORY_DIAGNOSTICS" in env,
        trim_diagnostics="TATSU_TRIM_DIAGNOSTICS" in env,
        pixmap_diagnostics="TATSU_PIXMAP_DIAGNOSTICS" in env,
        peak_RSS_bytes=max(s["rss"] for s in samples),
        peak_private_bytes=max(s["private"] for s in samples),
        peak_threads=max(s["threads"] for s in samples),
        peak_handles=max(s["handles"] for s in samples),
        idle_sampling="Last preceding sample within the 300ms closed-window interval",
        idle_median_first_3={
            k: statistics.median(s[k] for s in idle_samples[:3])
            for k in ("rss", "private", "threads", "handles")
        },
        idle_median_last_3={
            k: statistics.median(s[k] for s in idle_samples[-3:])
            for k in ("rss", "private", "threads", "handles")
        },
        limits="Same development PC, shared product UI in isolated harness, other user "
        "activity and OS cache uncontrolled; duration is stated, not hours-long proof",
    )
    (args.output / "memory-samples.json").write_text(
        json.dumps(samples), encoding="utf-8"
    )
    (args.output / "summary.json").write_text(
        json.dumps(result, indent=2) + "\n", encoding="utf-8"
    )
    print(json.dumps(result, indent=2))


if __name__ == "__main__":
    main()
