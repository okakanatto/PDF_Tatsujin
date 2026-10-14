"""Freeze original whole-ink regions without editing the earlier source corpus."""

import argparse
import copy
import hashlib
import json
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
GEOMETRY_SHA = "7b7d6bb8887a4668277feef16aeebf08cae7d1b6547a3bce67792b8b0d8dfb4e"
PROBE_SHA = "b45e01a22497dff531e0fe0903adb32deb98e7b6318199a476e6a25f9b6a74ad"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--geometry", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    out = args.output
    out.mkdir(exist_ok=False)
    base = ROOT / "fixtures/redaction-copy/standard-font-families-v1"
    raw = (base / "criteria.json").read_bytes()
    assert (
        hashlib.sha256(raw).hexdigest()
        == "86c1deda2fe3701e4da15e4277905006d06d6907bc98b5764aa5b841edc6308d"
    )
    geometry = args.geometry.read_bytes()
    assert hashlib.sha256(geometry).hexdigest() == GEOMETRY_SHA
    report = json.loads(geometry)
    assert report["probe_sha256"] == PROBE_SHA
    records = report["rows"]
    rows = []
    for case in json.loads(raw)["cases"]:
        row = copy.deepcopy(case)
        source = base / row["file"]
        data = source.read_bytes()
        assert hashlib.sha256(data).hexdigest() == row["sha256"]
        (out / source.name).write_bytes(data)
        original = next(r for r in records if r["file"] == row["file"])
        assert (
            original["source_sha256"] == row["sha256"]
            and original["original_unchanged"]
        )
        x, y, width, height = row["rect"]
        boxes = original["qt_secret_bounds"]
        x0 = min(x, min(b[0] for b in boxes) - 1)
        y0 = min(y, min(b[1] for b in boxes) - 1)
        x1 = max(x + width, max(b[0] + b[2] for b in boxes) + 1)
        y1 = max(y + height, max(b[1] + b[3] for b in boxes) + 1)
        row["rect"] = [x0, y0, x1 - x0, y1 - y0]
        row["source_qt_secret_bounds"] = boxes
        row["source_region_provenance"] = (
            "Union of PDFium source character bounds and Qt source outlines, each with the same 1pt margin; before any family-support implementation; v1 input and outcomes preserved"
        )
        rows.append(row)
    criteria = out / "criteria.json"
    criteria.write_text(
        json.dumps(
            dict(
                cases=rows,
                raster_dpi=300,
                outside_changed_pixels=0,
                boundary_px=1,
                scope="Whole source glyphs in both app and independent renderer; v1 single-renderer region remains separately tracked",
                source_geometry_probe_sha256=PROBE_SHA,
            ),
            ensure_ascii=False,
            indent=2,
        )
        + "\n",
        encoding="utf-8",
    )
    print(
        json.dumps(
            dict(
                criteria_sha256=hashlib.sha256(criteria.read_bytes()).hexdigest(),
                cases=len(rows),
            )
        )
    )


if __name__ == "__main__":
    main()
