"""Read immutable vertical OCR criteria and map raw PDF boxes to physical points."""

import hashlib
import json
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
PROFILES = {
    "standard": (
        "fixtures/vertical-ocr-criteria.json",
        "516522131307b548f3b46e8d8a274b398d732857455806c8df2484ee40f3e604",
    ),
    "unit": (
        "fixtures/vertical-ocr-unit/criteria.json",
        "86afa075759f530758ed6e4580622c9743a2cd9c3bf3c9fdc86791d8ca7e8beb",
    ),
    "rotated": (
        "fixtures/vertical-ocr-geometry/criteria.json",
        "4e087d6138ce25f08c7f5cc70b0b2ee2b85a7c6da69bab370d2eb74ed9417ac8",
    ),
    "rotated180": (
        "fixtures/vertical-ocr-rotations/180/criteria.json",
        "66eb2f2f58a96f1bd52d469bc08d7421998bc1ea4fd5d66fcfea05d3c547ca7d",
    ),
    "rotated270": (
        "fixtures/vertical-ocr-rotations/270/criteria.json",
        "f532130ff146ed8e09f32f34cdb8e2950d0218d503dac506d4eefe27a3b62c7f",
    ),
}


def frozen_vertical(profile):
    relative, digest = PROFILES[profile]
    criterion = ROOT / relative
    data = criterion.read_bytes()
    if hashlib.sha256(data).hexdigest() != digest:
        raise AssertionError("Frozen vertical criterion changed")
    fixed = json.loads(data)
    source = criterion.parent / fixed["source"]
    if hashlib.sha256(source.read_bytes()).hexdigest() != fixed["source_sha256"]:
        raise AssertionError("Frozen vertical source changed")
    return fixed, source, digest


def physical_upright_box(fixed, box, page_height):
    left, bottom, right, top = box
    if "cropbox" not in fixed:
        return [left, page_height - top, right - left, top - bottom]
    crop, unit = fixed["cropbox"], fixed["UserUnit"]
    return [
        (left - crop[0]) * unit,
        (crop[3] - top) * unit,
        (right - left) * unit,
        (top - bottom) * unit,
    ]


def displayed_vertical_box(fixed, box):
    """Apply the preserved page rotation to upright physical-point bounds."""
    x, y, width, height = box
    rotation = fixed.get("rotation", 0)
    if not rotation:
        return box
    page_width = (fixed["cropbox"][2] - fixed["cropbox"][0]) * fixed["UserUnit"]
    page_height = (fixed["cropbox"][3] - fixed["cropbox"][1]) * fixed["UserUnit"]
    if rotation == 90:
        return [page_height - y - height, x, height, width]
    if rotation == 180:
        return [page_width - x - width, page_height - y - height, width, height]
    if rotation == 270:
        return [y, page_width - x - width, height, width]
    raise AssertionError("Unsupported frozen rotation")
