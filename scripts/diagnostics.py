from pathlib import Path
import json, os, subprocess
import pypdfium2 as pdfium
from PIL import ImageChops

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / "evidence/diagnostics"
OUT.mkdir(exist_ok=True)
env = os.environ.copy()
env["PATH"] = str(Path(os.environ["SystemRoot"]) / "System32")
env.pop("TATSU_ASSETS", None)
env.pop("QT_PLUGIN_PATH", None)
options = OUT / "options.json"
options.write_text('{"pages":"1-5","language":"jpn+eng"}', encoding="utf-8")
exe = ROOT / "dist/PDFTatsujin-M1/PDFTatsujin.exe"
source = ROOT / "fixtures/D11-diagnostics.pdf"
result = OUT / "D11-ocr.pdf"
p = subprocess.run(
    [
        str(exe),
        "--ocr-worker",
        str(source),
        str(result),
        str(options),
        str(OUT / "worker-report.json"),
    ],
    env=env,
    capture_output=True,
    timeout=300,
)
(OUT / "process.json").write_text(
    json.dumps(
        {"exit": p.returncode, "stderr": p.stderr.decode("utf-8", "replace")}, indent=2
    ),
    encoding="utf-8",
)
if p.returncode:
    raise RuntimeError(
        "Diagnostic worker failed; see evidence/diagnostics/process.json"
    )
a, b = pdfium.PdfDocument(source), pdfium.PdfDocument(result)
rows = []
for i, label in enumerate(
    ["100dpi", "3-degree skew", "vertical Japanese", "two columns", "table"]
):
    text = b[i].get_textpage().get_text_range()
    diff = ImageChops.difference(
        a[i].render(scale=1).to_pil().convert("RGB"),
        b[i].render(scale=1).to_pil().convert("RGB"),
    )
    rows.append(
        {
            "page": i + 1,
            "condition": label,
            "copied_characters": len(text),
            "visible_difference": diff.getbbox(),
            "accuracy": "diagnostic; no CER threshold assigned",
            "copy_text": text,
        }
    )
(OUT / "results.json").write_text(
    json.dumps(rows, ensure_ascii=False, indent=2), encoding="utf-8"
)
print(
    json.dumps(
        [{k: v for k, v in row.items() if k != "copy_text"} for row in rows], indent=2
    )
)
