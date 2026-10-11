"""Write the additional line-count contract without altering existing inputs."""

import argparse
import hashlib
import json
from pathlib import Path


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    source = root / "fixtures/existing-text-edit/relative-lines"
    assert (
        hashlib.sha256((source / "criteria.json").read_bytes()).hexdigest()
        == "fd7998ba1be2f3f084fb54dd95ef258f96ce5ad22e3a903c7feb1e54a06c3c7f"
    )
    fixed = json.loads((source / "criteria.json").read_text(encoding="utf-8"))
    files = {
        name: fixed["files"][name]
        for name in ("relative-position.pdf", "relative-leading.pdf")
    }
    for name, sha in files.items():
        assert hashlib.sha256((source / name).read_bytes()).hexdigest() == sha
    plan = dict(
        source_folder="relative-lines",
        files=files,
        selected=fixed["selected"],
        english="New first line\nNew second line\nNew third line",
        japanese="一行目の本文を変更。\n二行目の本文を変更。\n三行目の本文を追加。",
        reedited="一行へ再編集しました。",
        explicit_family="Meiryo UI",
        leading_ratio=1.5,
        refused=dict(empty_line="first\n\nthird", line_count=65, ratios=[0.49, 4.01]),
        scope="Additional CC0 criteria fixed before line-count implementation and first product operation; original fixtures and acceptance unchanged",
    )
    args.output.mkdir(parents=True, exist_ok=False)
    (args.output / "criteria.json").write_text(
        json.dumps(plan, ensure_ascii=False, indent=2), encoding="utf-8"
    )


if __name__ == "__main__":
    main()
