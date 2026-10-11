"""Exercise Unicode assets and missing-resource failures in the real Windows app."""

import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess


def sha256(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def extended(path):
    # Filesystem operations only; the application receives ordinary Unicode paths.
    return Path("\\\\?\\" + str(path.resolve()))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--app-directory", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    output = args.output.resolve()
    if os.name != "nt" or not output.is_relative_to(root) or output.exists():
        raise RuntimeError("Use a new owned Windows test directory in this checkout")
    output.mkdir(parents=True)
    app = args.app_directory.resolve()
    executable = app / "PDFTatsujin.exe"
    environment = os.environ.copy()
    environment.update(
        QT_QPA_PLATFORM="offscreen",
        PATH=str(
            Path(
                environment.get(
                    "SystemRoot", environment.get("SYSTEMROOT", "C:/Windows")
                )
            )
            / "System32"
        ),
    )
    for name in ("TATSU_TEST_FILTER", "TATSU_UI_REVIEW", "QT_PLUGIN_PATH"):
        environment.pop(name, None)

    def run(name, arguments, assets, timeout):
        env = dict(environment, TATSU_ASSETS=str(assets))
        result = subprocess.run(
            [str(executable), *map(str, arguments)],
            env=env,
            capture_output=True,
            timeout=timeout,
            creationflags=subprocess.CREATE_NO_WINDOW,
        )
        (output / (name + "-stderr.txt")).write_bytes(result.stderr)
        return result.returncode

    resources = output / "long-assets"
    while len(str(resources)) < 350:
        resources /= "日本語とUnicode-🍀-abcdefghijk"
    shutil.copytree(app / "assets", extended(resources))
    long_result = output / "long-flow"
    environment["TATSU_TEST_FILTER"] = "A05"
    code = run(
        "long-assets", ["--selftest", root / "fixtures", long_result], resources, 240
    )
    if code:
        raise RuntimeError(
            f"Long asset-path OCR process failed: {code}; inspect stderr"
        )
    suite = json.loads((long_result / "selftest.json").read_text(encoding="utf-8"))
    if code or suite["failures"] or len(suite["tests"]) != 2:
        raise RuntimeError("Long asset-path OCR flow failed")
    environment.pop("TATSU_TEST_FILTER")

    missing_model = output / "missing-model"
    shutil.copytree(app / "assets", missing_model)
    # This is a new copy owned by this invocation, never the packaged resource.
    (missing_model / "tessdata/eng.traineddata").unlink()
    source = root / "fixtures/D05.pdf"
    original = sha256(source)
    options = output / "options.json"
    options.write_text('{"pages":"1","language":"jpn+eng"}', encoding="utf-8")
    target = output / "must-not-exist.pdf"
    worker_report = output / "must-not-exist.json"
    code = run(
        "missing-model",
        ["--ocr-worker", source, target, options, worker_report],
        missing_model,
        45,
    )
    if (
        code != 2
        or target.exists()
        or worker_report.exists()
        or sha256(source) != original
    ):
        raise RuntimeError(
            "A missing requested language published a partial OCR result"
        )

    missing_font = output / "missing-font"
    missing_font.mkdir()
    startup_report = output / "must-not-exist-startup.json"
    code = run("missing-font", ["--measure", source, startup_report], missing_font, 10)
    if code != 1 or startup_report.exists() or sha256(source) != original:
        raise RuntimeError("Missing-font diagnostic did not fail promptly and safely")
    result = {
        "status": "PASS",
        "executable_sha256": sha256(executable),
        "long_assets": {
            "model_path_utf16_units": len(
                str(resources / "tessdata/jpn.traineddata").encode("utf-16-le")
            )
            // 2,
            "tests": len(suite["tests"]),
            "failures": suite["failures"],
            "exit_code": 0,
        },
        "missing_requested_English_model": {
            "exit_code": 2,
            "partial_result_not_published": True,
            "source_unchanged": True,
        },
        "missing_bundled_font": {"exit_code": 1, "no_hidden_dialog_wait": True},
        "scope": "Same Windows development PC, Qt offscreen, Unicode assets; executable installation beyond MAX_PATH is a separate Windows startup limitation.",
    }
    (output / "result.json").write_text(json.dumps(result, indent=2) + "\n", "utf-8")
    print(json.dumps(result, indent=2))


if __name__ == "__main__":
    main()
