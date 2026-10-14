"""Describe closed-window memory over time; does not invent an acceptance limit."""

import argparse
from bisect import bisect_right
import csv
import hashlib
import json
from pathlib import Path
import statistics


def read(path):
    return json.loads(path.read_text(encoding="utf-8"))


def slope(points):
    if len(points) < 2:
        return None
    xmean = statistics.mean(p[0] for p in points)
    ymean = statistics.mean(p[1] for p in points)
    denominator = sum((x - xmean) ** 2 for x, _ in points)
    return (
        sum((x - xmean) * (y - ymean) for x, y in points) / denominator
        if denominator
        else None
    )


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("input", type=Path)
    parser.add_argument("--prefix", default="memory-analysis")
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    directory = args.input.resolve()
    if (
        directory.parent != root / "evidence"
        or not args.prefix
        or any(c not in "0123456789abcdefghijklmnopqrstuvwxyz-" for c in args.prefix)
    ):
        raise RuntimeError("Use an owned evidence child and a plain new prefix")
    destination = directory / (args.prefix + ".json")
    csv_path = directory / (args.prefix + ".csv")
    if destination.exists() or csv_path.exists():
        raise RuntimeError("Analysis output already exists")
    summary = read(directory / "summary.json")
    samples = read(directory / "memory-samples.json")
    cycles_path = directory / "run/cycles.jsonl"
    cycles = [
        json.loads(line)
        for line in cycles_path.read_text(encoding="utf-8").splitlines()
    ]
    if summary["status"] != "PASS" or len(cycles) != summary["cycles"] or not samples:
        raise RuntimeError("Soak did not complete its reported workload")
    times = [s["unix_ms"] for s in samples]
    if times != sorted(times):
        raise RuntimeError("Clock moved backwards; refuse chronological analysis")
    idle = []
    for cycle in cycles:
        index = bisect_right(times, cycle["idle_unix_ms"]) - 1
        if index < 0 or cycle["idle_unix_ms"] - times[index] > 300:
            raise RuntimeError(
                "Closed-window sample was not inside the recorded idle interval"
            )
        idle.append({"minute": cycle["at_ms"] / 60000, **samples[index]})
    last_bin = max(0, (summary["requested_seconds"] - 1) // 300)
    groups = {}
    for row in idle:
        index = min(int(row["minute"] // 5), last_bin)
        groups.setdefault(index, []).append(row)
    bins = []
    for index, rows in groups.items():
        bins.append(
            {
                "start_minute": index * 5,
                "end_minute": (
                    min((index + 1) * 5, summary["elapsed_ms"] / 60000)
                    if index < last_bin
                    else summary["elapsed_ms"] / 60000
                ),
                "samples": len(rows),
                "sample_midpoint_minute": statistics.median(r["minute"] for r in rows),
                **{
                    "median_" + key: statistics.median(r[key] for r in rows)
                    for key in ("private", "rss", "handles", "threads")
                },
            }
        )
    elapsed = summary["elapsed_ms"] / 60000
    last_ten = [r for r in idle if r["minute"] >= max(0, elapsed - 10)]
    result = {
        "status": "ANALYSIS_COMPLETED",
        "elapsed_minutes": elapsed,
        "cycles": len(cycles),
        "pages_visited": summary["pages_visited"],
        "mode": summary.get("mode", "mixed"),
        "executable_sha256": summary["executable_sha256"],
        "source_commit": summary.get("source_commit"),
        "diagnostic_interventions": {
            key: summary.get(key, False)
            for key in (
                "heap_diagnostics",
                "memory_diagnostics",
                "trim_diagnostics",
                "pixmap_diagnostics",
            )
        },
        "closed_window_idle_5min_bins": bins,
        "last_10min_medians": {
            key: statistics.median(r[key] for r in last_ten)
            for key in ("private", "rss", "handles", "threads")
        },
        "after_15min": {
            "idle_private_MB_per_min": slope(
                [(r["minute"], r["private"] / 1e6) for r in idle if r["minute"] >= 15]
            ),
            "bin_median_private_MB_per_min": slope(
                [
                    (r["sample_midpoint_minute"], r["median_private"] / 1e6)
                    for r in bins
                    if r["start_minute"] >= 15
                ]
            ),
        },
        "first_3_medians": summary["idle_median_first_3"],
        "last_3_medians": summary["idle_median_last_3"],
        "peaks": {
            key: summary[key]
            for key in (
                "peak_private_bytes",
                "peak_RSS_bytes",
                "peak_handles",
                "peak_threads",
            )
        },
        "input_sha256": {
            name: hashlib.sha256((directory / name).read_bytes()).hexdigest()
            for name in ("summary.json", "memory-samples.json", "run/cycles.jsonl")
        },
        "analysis_source_sha256": hashlib.sha256(
            Path(__file__).read_bytes()
        ).hexdigest(),
        "limits": "Descriptive slopes and medians, not a new pass threshold or proof of no leaks. "
        "Offscreen lifecycle on this development PC; OS cache and other activity uncontrolled.",
    }
    destination.write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
    with csv_path.open("w", newline="", encoding="utf-8") as output:
        writer = csv.DictWriter(output, fieldnames=bins[0].keys())
        writer.writeheader()
        writer.writerows(bins)
    print(
        json.dumps(
            {
                key: result[key]
                for key in (
                    "elapsed_minutes",
                    "cycles",
                    "after_15min",
                    "last_10min_medians",
                )
            }
        )
    )


if __name__ == "__main__":
    main()
