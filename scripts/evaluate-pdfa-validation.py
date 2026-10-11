"""Compare saved app results with fixed inputs and directly invoked veraPDF."""

import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import xml.etree.ElementTree as ET

ROOT = Path(__file__).resolve().parents[1]


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("run", type=Path)
    parser.add_argument("--attempt", default="r1")
    args = parser.parse_args()
    output = args.run / f"pdfa-independent-{args.attempt}.json"
    assert not output.exists(), "Preserve previous evidence"
    criterion = ROOT / "fixtures/pdfa-validation/criteria.json"
    assert (
        digest(criterion)
        == "f100bd83cc754dfa2ebfc8d7cf399d90082c48309d7e20552a4f123fae279397"
    )
    fixed = json.loads(criterion.read_text("utf-8"))
    lock = json.loads((ROOT / "pdfa-engine-lock.json").read_text("utf-8"))
    java, jar = ROOT / lock["java"]["executable"], ROOT / lock["verapdf"]["jar"]
    assert digest(java) == lock["java"]["sha256"]
    assert digest(jar) == lock["verapdf"]["sha256"]
    rows = []
    for item in fixed["files"]:
        source = ROOT / "fixtures/pdfa-validation" / item["file"]
        assert digest(source) == item["sha256"]
        if "expected_compliant" not in item:
            continue  # Encryption refusal is checked by the app's failure tests.
        row = {"file": item["file"], "status": "FAIL"}
        try:
            stem = source.stem
            xml = args.run / f"pdfa-independent-{args.attempt}-{stem}.xml"
            stderr = xml.with_suffix(".stderr.txt")
            assert not xml.exists() and not stderr.exists()
            process = subprocess.run(
                [
                    str(java),
                    "-Xmx512m",
                    "-Djava.awt.headless=true",
                    "-jar",
                    str(jar),
                    "--format",
                    "xml",
                    "--flavour",
                    item["profile"],
                    "--maxfailuresdisplayed",
                    "1",
                    str(source),
                ],
                capture_output=True,
                timeout=120,
                creationflags=subprocess.CREATE_NO_WINDOW,
            )
            xml.write_bytes(process.stdout)
            stderr.write_bytes(process.stderr)
            report = ET.fromstring(process.stdout)
            validations = report.findall("./jobs/job/validationReport")
            assert len(validations) == 1
            validation = validations[0]
            assert validation.get("jobEndStatus") == "normal"
            assert (
                validation.get("profileName")
                == f"PDF/A-{item['profile']} validation profile"
            )
            compliant = validation.get("isCompliant") == "true"
            assert compliant == item["expected_compliant"]
            assert process.returncode == int(not compliant)
            app = json.loads((args.run / f"pdfa-{stem}.json").read_text("utf-8"))
            assert app["compliant"] == compliant and app["profile"] == item["profile"]
            assert app["snapshot_sha256"] == item["sha256"]
            assert app["engine_version"] == lock["verapdf"]["version"]
            details = validation.find("details")
            assert app["failed_rules"] == int(details.get("failedRules"))
            assert app["failed_checks"] == int(details.get("failedChecks"))
            assert digest(source) == item["sha256"], "Original changed"
            row.update(
                status="PASS",
                compliant=compliant,
                exit_code=process.returncode,
                original_unchanged=True,
                saved_result_matches_independent_engine=True,
            )
        except Exception as error:
            row["error"] = str(error)
        rows.append(row)
    result = {
        "cases": rows,
        "failures": sum(r["status"] != "PASS" for r in rows),
        "scope": "Direct engine invocation and Python XML parsing on the development PC; native viewer and clean Windows unexecuted.",
    }
    output.write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(result, indent=2))
    raise SystemExit(bool(result["failures"]))


if __name__ == "__main__":
    main()
