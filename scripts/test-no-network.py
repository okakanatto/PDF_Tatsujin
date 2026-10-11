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
    parser.add_argument("--with-image-export", action="store_true")
    parser.add_argument("--with-page-geometry", action="store_true")
    parser.add_argument("--with-page-crop", action="store_true")
    parser.add_argument("--with-page-decorations", action="store_true")
    parser.add_argument("--with-bookmark-editing", action="store_true")
    parser.add_argument("--with-link-editing", action="store_true")
    parser.add_argument("--with-optimization", action="store_true")
    parser.add_argument("--with-encryption", action="store_true")
    parser.add_argument("--with-form-data", action="store_true")
    parser.add_argument("--with-owner-copy", action="store_true")
    parser.add_argument("--with-comparison", action="store_true")
    parser.add_argument("--with-batch", action="store_true")
    parser.add_argument("--with-form-design", action="store_true")
    parser.add_argument("--with-certificate-verification", action="store_true")
    parser.add_argument("--with-redaction-copy", action="store_true")
    parser.add_argument("--with-existing-images", action="store_true")
    parser.add_argument("--with-existing-text", action="store_true")
    parser.add_argument("--with-table-extraction", action="store_true")
    parser.add_argument("--with-vertical-ocr", action="store_true")
    parser.add_argument("--with-word-text", action="store_true")
    parser.add_argument("--office-engine-directory", type=Path)
    parser.add_argument("--pdfa-engine-directory", type=Path)
    parser.add_argument("--only-pdfa", action="store_true")
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
    engine_acl_saved = False
    engine_acl_restored = False
    engine = (
        args.office_engine_directory.resolve() if args.office_engine_directory else None
    )
    acl_record = out / "office-engine-original-acl.json"
    pdfa_engine = (
        args.pdfa_engine_directory.resolve() if args.pdfa_engine_directory else None
    )
    pdfa_acl_record = out / "pdfa-engine-original-acl.json"
    pdfa_acl_restored = False
    powershell = Path(
        shutil.which("pwsh")
        or (
            str(
                Path(os.environ["SystemRoot"])
                / "System32/WindowsPowerShell/v1.0/powershell.exe"
            )
        )
    )

    def engine_acl(mode, pdfa=False):
        completed = subprocess.run(
            [
                str(powershell),
                "-NoProfile",
                "-File",
                str(
                    root
                    / (
                        "scripts/pdfa-engine-access.ps1"
                        if pdfa
                        else "scripts/office-engine-access.ps1"
                    )
                ),
                "-EngineRoot",
                str(pdfa_engine if pdfa else engine),
                "-Sid",
                container.sid_text,
                "-Backup",
                str(pdfa_acl_record if pdfa else acl_record),
                "-Mode",
                mode,
            ],
            creationflags=subprocess.CREATE_NO_WINDOW,
            timeout=180,
            capture_output=True,
        )
        (
            out
            / (("pdfa-engine-acl-" if pdfa else "office-engine-acl-") + mode + ".log")
        ).write_bytes(completed.stdout + completed.stderr)
        completed.check_returncode()

    try:
        if engine:
            engine_acl("Grant")
            engine_acl_saved = True
            env["TATSU_OFFICE_CONVERTER"] = str(engine / "program/soffice.com")
        if pdfa_engine:
            engine_acl("Grant", pdfa=True)
            lock = json.loads((root / "pdfa-engine-lock.json").read_text("utf-8"))
            for key, value in (
                ("TATSU_PDFA_JAVA", lock["java"]["executable"]),
                ("TATSU_PDFA_JAR", lock["verapdf"]["jar"]),
            ):
                relative = Path(value).relative_to("tools/pdfa-verifier")
                env[key] = str(pdfa_engine / relative)
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
                            "OCR_child": "--ocr-worker" in process.cmdline(),
                            "office_child": process.name().lower()
                            in ("soffice.bin", "soffice.com", "python.exe"),
                            "PDF_A_child": process.name().lower() == "java.exe",
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
        if args.with_image_export:
            cases.append(("image-export", "M4E"))
        if args.with_page_geometry:
            cases.append(("page-geometry", "Geometry"))
        if args.with_page_crop:
            cases.append(("page-crop", "M4C"))
        if args.with_page_decorations:
            cases.append(("page-decorations", "M4D"))
        if args.with_bookmark_editing:
            cases.append(("bookmark-editing", "M4B"))
        if args.with_link_editing:
            cases.append(("link-editing", "M4L"))
        if args.with_optimization:
            cases.append(("optimization", "M4O"))
        if args.with_encryption:
            cases.append(("encryption", "M5P"))
        if args.with_form_data:
            cases.append(("form-data", "M5F"))
        if args.with_owner_copy:
            cases.append(("owner-copy", "M5U"))
        if args.with_comparison:
            cases.append(("comparison", "M5C"))
        if args.with_batch:
            cases.append(("batch", "M5T"))
        if args.with_form_design:
            cases.append(("form-design", "M5D"))
        if args.with_certificate_verification:
            cases.append(("certificate-verification", "M5S"))
        if args.with_redaction_copy:
            cases.append(("redaction-copy", "M5R"))
        if args.with_existing_images:
            cases.append(("existing-images", "M6I"))
        if args.with_existing_text:
            # M6TB also starts with M6T, and its Office interoperability test
            # needs the separate engine. Select the existing text families by name.
            cases.append(("existing-text", "existing_text"))
            cases.append(("existing-form-text", "existing_form_text"))
        if args.with_table_extraction:
            cases.append(("table-extraction", "M6TB01"))
            cases.append(("table-extraction-ui", "M6TB02"))
        if args.with_vertical_ocr:
            cases.append(("vertical-ocr", "M6V"))
        if args.with_word_text:
            cases.extend(
                ("word-text-" + str(index), "M6W0" + str(index)) for index in (1, 2, 3)
            )
        if engine:
            cases.append(("office-import", "M6O"))
        if pdfa_engine:
            cases.append(("pdfa-validation", "M6A"))
        if args.only_pdfa:
            if not pdfa_engine:
                raise RuntimeError("--only-pdfa requires --pdfa-engine-directory")
            cases = [("pdfa-validation", "M6A")]
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
        if not args.only_pdfa and not any(
            p["OCR_child"] for p in observations.values()
        ):
            raise RuntimeError("OCR child token was not observed")
        if pdfa_engine and not any(p["PDF_A_child"] for p in observations.values()):
            raise RuntimeError("PDF/A Java child token was not observed")
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
            try:
                if engine and (engine_acl_saved or acl_record.exists()):
                    engine_acl("Restore")
                    engine_acl_restored = True
            finally:
                if pdfa_engine and pdfa_acl_record.exists():
                    try:
                        engine_acl("Restore", pdfa=True)
                        pdfa_acl_restored = True
                    except Exception:
                        container.remove()
                        raise
                container.remove()
                if failure is not None:
                    failure["temporary_profile_removed"] = container.removed
                    failure["engine_acl_restored"] = engine_acl_restored
                    failure["pdfa_acl_restored"] = pdfa_acl_restored
                    (out / "failure.json").write_text(
                        json.dumps(failure, indent=2) + "\n", encoding="utf-8"
                    )
    result["temporary_profile_removed"] = container.removed
    if engine:
        result["engine_acl_restored"] = engine_acl_restored
    if pdfa_engine:
        result["pdfa_acl_restored"] = pdfa_acl_restored
    (out / "result.json").write_text(
        json.dumps(result, indent=2) + "\n", encoding="utf-8"
    )
    print(json.dumps(result, indent=2))


if __name__ == "__main__":
    main()
