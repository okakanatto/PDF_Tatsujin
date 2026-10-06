"""Compare completed three-run reading measurements without altering thresholds.

Requires every representative viewport PNG to have identical pixels. Performance
numbers describe this PC and procedure, not Acrobat or physical-display FPS.
"""

import argparse
import hashlib
import json
from pathlib import Path
import statistics

from PIL import Image, ImageChops


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("baseline", type=Path)
    parser.add_argument("candidate", type=Path)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    if args.output.exists():
        raise RuntimeError("Preserve earlier comparison evidence")
    before = json.loads((args.baseline / "summary.json").read_text("utf-8"))
    after = json.loads((args.candidate / "summary.json").read_text("utf-8"))
    for result in (before, after):
        if (
            result["status"] != "PASS"
            or result["runs_per_document"] != 3
            or result["laps_per_run"] != 3
            or result["renderer_override"] != "product"
        ):
            raise RuntimeError("Incomplete or different-renderer measurement")
    comparisons = {}
    for name, old_document in before["documents"].items():
        new_document = after["documents"][name]
        if old_document["source_sha256"] != new_document["source_sha256"]:
            raise RuntimeError("Reading source changed")
        images = []
        for old, new in zip(old_document["runs"], new_document["runs"], strict=True):
            old_folder = args.baseline / old["run_directory"]
            new_folder = args.candidate / new["run_directory"]
            names = {path.name for path in old_folder.glob("*.png")}
            if not names or names != {path.name for path in new_folder.glob("*.png")}:
                raise RuntimeError("Representative images missing or different")
            for filename in sorted(names):
                original = Image.open(old_folder / filename).convert("RGBA")
                candidate = Image.open(new_folder / filename).convert("RGBA")
                if (
                    original.size != candidate.size
                    or ImageChops.difference(original, candidate).getbbox(
                        alpha_only=False
                    )
                    is not None
                ):
                    raise RuntimeError(f"Viewport pixels changed: {name}/{filename}")
                images.append(
                    {
                        "run": old["run_directory"],
                        "file": filename,
                        "pixels_equal": True,
                    }
                )

        def metrics(document):
            rows = document["runs"]
            return {
                "p95_ms_each_run": [
                    row["scroll_ready_ms"]["p95_nearest_rank"] for row in rows
                ],
                "median_of_run_p95_ms": statistics.median(
                    row["scroll_ready_ms"]["p95_nearest_rank"] for row in rows
                ),
                "elapsed_seconds_each_run": [row["elapsed_seconds"] for row in rows],
                "median_elapsed_seconds": statistics.median(
                    row["elapsed_seconds"] for row in rows
                ),
                "peak_RSS_bytes": max(row["peak_rss_bytes"] for row in rows),
            }

        comparisons[name] = {
            "baseline": metrics(old_document),
            "candidate": metrics(new_document),
            "viewport_images": images,
        }
    result = {
        "status": "PASS",
        "scope": "Same frozen PDFs, QPainter, 3 runs x 3 laps, no downsampling; representative viewport pixels exact",
        "baseline_executable_sha256": before["product_exe_sha256"],
        "candidate_executable_sha256": after["product_exe_sha256"],
        "baseline_summary_sha256": hashlib.sha256(
            (args.baseline / "summary.json").read_bytes()
        ).hexdigest(),
        "candidate_summary_sha256": hashlib.sha256(
            (args.candidate / "summary.json").read_bytes()
        ).hexdigest(),
        "documents": comparisons,
        "limits": "OS caches and other user activity are not controlled; no physical FPS, cold-cache, long-soak or Acrobat claim",
    }
    args.output.write_text(
        json.dumps(result, ensure_ascii=False, indent=2) + "\n", "utf-8"
    )
    print(
        json.dumps(
            {
                name: {
                    key: value
                    for key, value in data.items()
                    if key != "viewport_images"
                }
                for name, data in comparisons.items()
            },
            indent=2,
        )
    )


if __name__ == "__main__":
    main()
