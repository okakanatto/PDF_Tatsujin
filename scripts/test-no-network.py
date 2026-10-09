"""Test packaged signature/OCR under Windows AppContainer with zero capabilities.

No adapter, firewall, or global security settings are changed. Copies of the
application and synthetic fixtures are granted to a temporary per-test identity.
This is still the development PC, not the contractual clean Windows environment.
"""

import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import socket
import subprocess

import psutil
from win_appcontainer import AppContainer


def sha256(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--app-directory", required=True, type=Path)
    parser.add_argument("--probe", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--with-long-paths", action="store_true")
    parser.add_argument("--with-image-creation", action="store_true")
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    out = args.output.resolve()
    if not out.is_relative_to(root):
        raise RuntimeError("Use an owned new directory inside this checkout")
    out.mkdir(exist_ok=False, parents=True)
    app = out / "app"
    shutil.copytree(args.app_directory, app)
    fixtures = out / "fixtures"
    shutil.copytree(root / "fixtures", fixtures)
    probe = out / args.probe.name
    shutil.copyfile(args.probe, probe)
    for pattern in ("vcruntime*.dll", "msvcp*.dll", "concrt*.dll"):
        for runtime in app.glob(pattern):
            shutil.copyfile(runtime, out / runtime.name)
    temporary = out / "tmp"
    temporary.mkdir()
    env = os.environ.copy()
    for key in (
        "QT_PLUGIN_PATH",
        "TATSU_ASSETS",
        "TATSU_UI_REVIEW",
        "TATSU_TEST_FILTER",
    ):
        env.pop(key, None)
    env.update(
        PATH=str(Path(env.get("SystemRoot", "C:/Windows")) / "System32"),
        QT_QPA_PLATFORM="offscreen",
        TMP=str(temporary),
        TEMP=str(temporary),
        LOCALAPPDATA=str(out),
        APPDATA=str(out),
    )
    observations = {}
    container = AppContainer()
    failure = None
    results = []
    try:
        subprocess.run(
            [
                "icacls",
                str(out),
                "/grant",
                "*" + container.sid_text + ":(OI)(CI)F",
                "/T",
                "/Q",
            ],
            stdout=subprocess.DEVNULL,
            check=True,
        )
        with socket.socket() as listener:
            listener.bind(("127.0.0.1", 0))
            listener.listen(2)
            port = str(listener.getsockname()[1])
            subprocess.run(
                [str(probe), str(out / "control.json"), port, "control"],
                env=env,
                check=True,
                timeout=10,
                creationflags=subprocess.CREATE_NO_WINDOW,
            )
            client, _ = listener.accept()
            client.close()
            code = container.run(
                [probe, out / "denied.json", port, "denied"], env, out, timeout=20
            )
            if code:
                raise RuntimeError(f"Network restriction probe failed: {code}")
            resolved_temp = Path(
                (out / "denied.json.temp-path.txt").read_text(encoding="utf-8")
            ).resolve()
            if not resolved_temp.is_relative_to(out):
                raise RuntimeError(
                    "Restricted process TEMP escaped the owned QA payload"
                )
            resolved_temp.mkdir(parents=True, exist_ok=True)

        def observe(pid):
            try:
                for process in [
                    psutil.Process(pid),
                    *psutil.Process(pid).children(recursive=True),
                ]:
                    if process.pid not in observations:
                        token = container.token(process.pid)
                        if token != {"is_AppContainer": 1, "capability_count": 0}:
                            raise RuntimeError(
                                "Payload or OCR child has network capabilities"
                            )
                        observations[process.pid] = {
                            **token,
                            "OCR_child": process.pid != pid,
                        }
            except psutil.NoSuchProcess:
                pass

        cases = [
            ("signature", "A02"),
            ("OCR", "A05"),
            ("cancel", "A09_UI_cancel"),
        ]
        if args.with_long_paths:
            cases.append(("long-paths", "C07_LongPaths"))
        if args.with_image_creation:
            cases.append(("image-creation", "M4I"))
        for name, filter in cases:
            case_env = env.copy()
            case_env["TATSU_TEST_FILTER"] = filter
            code = container.run(
                [app / "PDFTatsujin.exe", "--selftest", fixtures, out / name],
                case_env,
                out,
                timeout=240,
                observe=observe,
            )
            if code:
                raise RuntimeError(f"{name} AppContainer flow failed: {code}")
            test = json.loads(
                (out / name / "selftest.json").read_text(encoding="utf-8")
            )
            if test["failures"] or not test["tests"]:
                raise RuntimeError(f"{name} operations were incomplete")
            results.append(
                {
                    "name": name,
                    "exit": code,
                    "tests": len(test["tests"]),
                    "failures": test["failures"],
                }
            )
        if not any(p["OCR_child"] for p in observations.values()):
            raise RuntimeError("OCR child token was not observed")
        result = {
            "status": "PASS",
            "executable_sha256": sha256(app / "PDFTatsujin.exe"),
            "probe_sha256": sha256(probe),
            "network_probe": json.loads((out / "denied.json").read_text()),
            "control_probe": json.loads((out / "control.json").read_text()),
            "payload_and_OCR_tokens": list(observations.values()),
            "flows": results,
            "scope": "Windows AppContainer, no network capabilities; same development PC, Qt offscreen and synthetic UI input. Not clean Windows or native IME acceptance.",
        }
    except Exception as error:
        failure = {
            "status": "FAIL",
            "error": str(error),
            "flows": results,
            "executable_sha256": sha256(app / "PDFTatsujin.exe"),
        }
        raise
    finally:
        try:
            subprocess.run(
                ["icacls", str(out), "/remove:g", "*" + container.sid_text, "/T", "/Q"],
                stdout=subprocess.DEVNULL,
                check=True,
            )
        finally:
            container.remove()
            if failure is not None:
                failure["temporary_profile_removed"] = container.removed
                (out / "failure.json").write_text(
                    json.dumps(failure, indent=2) + "\n", encoding="utf-8"
                )
    result["temporary_profile_removed"] = container.removed
    (out / "result.json").write_text(
        json.dumps(result, indent=2) + "\n", encoding="utf-8"
    )
    print(json.dumps(result, indent=2))


if __name__ == "__main__":
    main()
