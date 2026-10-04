from pathlib import Path
import json, os, statistics, subprocess, time
import psutil

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / "evidence/performance"
OUT.mkdir(exist_ok=True)
env = os.environ.copy()
env["PATH"] = str(Path(os.environ["SystemRoot"]) / "System32")
env.pop("TATSU_ASSETS", None)
env.pop("QT_PLUGIN_PATH", None)
env["TATSU_TEST_FILTER"] = "A05_A08_same_window"
rows = []
for n in range(3):
    output = OUT / f"UI-OCR-final-{n}"
    start = time.perf_counter()
    peak = 0
    peak_children = 0
    p = subprocess.Popen(
        [
            str(ROOT / "dist/PDFTatsujin-M1/PDFTatsujin.exe"),
            "--selftest",
            str(ROOT / "fixtures"),
            str(output),
        ],
        env=env,
    )
    while p.poll() is None:
        try:
            parent = psutil.Process(p.pid)
            children = parent.children(recursive=True)
            peak_children = max(peak_children, len(children))
            total = parent.memory_info().rss + sum(
                c.memory_info().rss for c in children
            )
            peak = max(peak, total)
        except psutil.Error:
            pass
        time.sleep(0.02)
    if p.returncode:
        raise RuntimeError(f"UI OCR acceptance flow failed: {p.returncode}")
    rows.append(
        {
            "elapsed_seconds": time.perf_counter() - start,
            "process_group_peak_rss_bytes": peak,
            "max_worker_count": peak_children,
            "scope": "D05 1-page Japanese scan with digital heading, annotation, signature; UI OCR, search, copy, save, undo/redo",
        }
    )
result = {
    "runs": rows,
    "median_seconds": statistics.median(r["elapsed_seconds"] for r in rows),
    "peak_process_group_bytes": max(r["process_group_peak_rss_bytes"] for r in rows),
}
(OUT / "UI-OCR-summary.json").write_text(json.dumps(result, indent=2), encoding="utf-8")
print(json.dumps(result, indent=2))
