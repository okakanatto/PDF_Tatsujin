from pathlib import Path
import json, os, statistics, subprocess, time
import psutil

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / "evidence/performance"
OUT.mkdir(exist_ok=True)
EXE = ROOT / "dist/PDFTatsujin-M1/PDFTatsujin.exe"
env = os.environ.copy()
env["PATH"] = str(Path(os.environ["SystemRoot"]) / "System32")
env.pop("TATSU_ASSETS", None)
env.pop("QT_PLUGIN_PATH", None)


def run(args):
    start = time.perf_counter()
    p = subprocess.Popen(
        [str(EXE), *map(str, args)],
        env=env,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    )
    peak = 0
    while p.poll() is None:
        try:
            proc = psutil.Process(p.pid)
            memory = proc.memory_info().rss
            for child in proc.children(recursive=True):
                memory += child.memory_info().rss
            peak = max(peak, memory)
        except psutil.Error:
            pass
        time.sleep(0.02)
    stdout, stderr = p.communicate()
    if p.returncode:
        raise RuntimeError(f"exit={p.returncode}: " + stderr.decode("utf-8", "replace"))
    return {
        "elapsed_seconds": time.perf_counter() - start,
        "process_group_peak_rss_bytes": peak,
    }


records = {}
for name in ["D01.pdf", "D10-digital-100.pdf", "D10-image-50.pdf"]:
    rows = []
    for i in range(3):
        details = OUT / f"{name}-{i}.json"
        row = run(["--measure", ROOT / "fixtures" / name, details])
        row.update(json.loads(details.read_text(encoding="utf-8")))
        rows.append(row)
    records[name] = {
        "runs": rows,
        "median_seconds": statistics.median(r["elapsed_seconds"] for r in rows),
    }
options = OUT / "ocr-options.json"
options.write_text(
    json.dumps({"pages": "1,5", "language": "jpn+eng"}), encoding="utf-8"
)
rows = []
for i in range(3):
    rows.append(
        run(
            [
                "--ocr-worker",
                ROOT / "fixtures/D03.pdf",
                OUT / f"ocr-{i}.pdf",
                options,
                OUT / f"ocr-{i}.json",
            ]
        )
    )
records["OCR_jpn_eng_2_pages"] = {
    "runs": rows,
    "median_seconds": statistics.median(r["elapsed_seconds"] for r in rows),
}
records["environment_note"] = (
    "Current Windows host; PATH limited to System32; no clean VM and no network-disconnection test."
)
(OUT / "summary.json").write_text(json.dumps(records, indent=2), encoding="utf-8")
print(json.dumps(records, indent=2))
