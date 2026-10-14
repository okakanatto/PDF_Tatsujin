"""Compare actual product UA machine results with immutable input and direct XML."""

import argparse
import json
from pathlib import Path
from xml.etree import ElementTree as ET

from pdf_validation_tools import digest, invoke

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("run", type=Path)
    parser.add_argument("--attempt", default="r1")
    args = parser.parse_args()
    output = args.run / f"pdfua-independent-{args.attempt}.json"
    assert not output.exists(), "Preserve former results"
    criterion = ROOT / "fixtures/pdfua-validation/criteria.json"
    assert (
        digest(criterion)
        == "d1ebf99ed5edfb6d81c440fb9d0a1c38c8ed8cb06444a3532d2e8ffde2507efc"
    )
    fixed = json.loads(criterion.read_text("utf-8"))
    lock = json.loads((ROOT / "pdfa-engine-lock.json").read_text("utf-8"))
    java, jar = ROOT / lock["java"]["executable"], ROOT / lock["verapdf"]["jar"]
    assert (
        digest(java) == lock["java"]["sha256"]
        and digest(jar) == lock["verapdf"]["sha256"]
    )
    cases = [
        (
            item["case"],
            ROOT / "fixtures/pdfua-validation" / item["file"],
            item["sha256"],
            item["upstream_expected"] == "pass",
            args.run / f"pdfua-{item['case']}.json",
        )
        for item in fixed["inputs"]
    ]
    cases.append(
        ("UI", cases[0][1], cases[0][2], cases[0][3], args.run / "pdfua-ui-result.json")
    )
    cases.append(
        (
            "UI-fail",
            cases[1][1],
            cases[1][2],
            cases[1][3],
            args.run / "pdfua-ui-fail-result.json",
        )
    )
    candidate = args.run / "pdfua-unsaved-candidate.pdf"
    cases.append(
        (
            "unsaved-candidate",
            candidate,
            digest(candidate),
            None,
            args.run / "pdfua-existing.json",
        )
    )
    rows = []
    for name, source, source_hash, expected, result_file in cases:
        row = dict(case=name, status="FAIL")
        try:
            assert digest(source) == source_hash
            xml = args.run / f"pdfua-independent-{args.attempt}-{name}.xml"
            stderr = xml.with_suffix(".stderr.txt")
            assert not xml.exists() and not stderr.exists()
            process = invoke(java, jar, source.resolve(), "ua1", args.run.resolve())
            xml.write_bytes(process.stdout)
            stderr.write_bytes(process.stderr)
            reports = ET.fromstring(process.stdout).findall(
                "./jobs/job/validationReport"
            )
            assert len(reports) == 1
            report = reports[0]
            assert (
                report.get("jobEndStatus") == "normal"
                and report.get("profileName") == "PDF/UA-1 validation profile"
            )
            assert report.get("isCompliant") in ("true", "false")
            compliant = report.get("isCompliant") == "true"
            assert expected is None or compliant == expected
            assert process.returncode == int(not compliant)
            app = json.loads(result_file.read_text("utf-8"))
            assert app["profile"] == "ua1" and app["snapshot_sha256"] == source_hash
            assert app["engine_version"] == lock["verapdf"]["version"]
            assert app["machine_compliant"] == compliant and "compliant" not in app
            assert (
                app["validation_scope"] == "machine-verifiable"
                and app["full_conformance"] == "not-determined"
            )
            details = report.find("details")
            assert app["failed_rules"] == int(details.get("failedRules")) and app[
                "failed_checks"
            ] == int(details.get("failedChecks"))
            assert digest(source) == source_hash
            row.update(
                status="PASS",
                machine_compliant=compliant,
                input_sha256=source_hash,
                failed_rules=app["failed_rules"],
                failed_checks=app["failed_checks"],
                original_unchanged=True,
                saved_app_matches_direct_XML=True,
                full_conformance="not-determined",
            )
        except Exception as error:
            row["error"] = str(error)
        rows.append(row)
    result = dict(
        cases=rows,
        failures=sum(row["status"] != "PASS" for row in rows),
        scope="Direct pinned engine plus independent Python XML; product UI results and exact candidate compared. Human checkpoints/native GUI/clean Windows未実行",
    )
    output.write_text(json.dumps(result, ensure_ascii=False, indent=2) + "\n", "utf-8")
    print(json.dumps(result, ensure_ascii=False))
    raise SystemExit(bool(result["failures"]))


if __name__ == "__main__":
    main()
