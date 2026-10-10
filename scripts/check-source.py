"""Portable source checks; never substitutes for the Windows acceptance suite."""

import ast
import hashlib
import json
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[1]
REAL_SCAN_MANIFEST_SHA256 = (
    "6c8da43a51bc94126e5043cb3725fc0c842391326934d7dcecd06f35996306ab"
)
GROUND_TRUTH_SHA256 = "7a8878927ec067c80f8b5b42640203beb7d7a99daec17a790d7f5e661e4b15e7"
VIEWER_SEARCH_MANIFEST_SHA256 = (
    "5f44a96e203ee9513eff16cbeb72631279691cce079998c52b2a65421e82c77b"
)
VIEWER_SELECTION_MANIFEST_SHA256 = (
    "b3cfd62a5614ee72bba9aff5177ad1e8c85c4a37b19c97cb37fb880e7149bb15"
)
VIEWER_SELECTION_RESTRICTED_SHA256 = (
    "ccbcf6bc03f45bdcfce814a50ad651f7d2149e50d37ce8037a7bfcf0a52954a5"
)
VIEWER_NAVIGATION_MANIFEST_SHA256 = (
    "2421d94d73d68ca17e4686bc77f7e59a5cf7ae6256ac22aa2f5a5b9f319b55e0"
)
CERTIFICATE_MANIFEST_SHA256 = (
    "9d20d7d9624cc5236c859df5343dc46c289b7797a1c6c2b5cdafc01e62c92c74"
)
REDACTION_CRITERIA_SHA256 = {
    "foundation": "f00f689b6e0a896a1ca610f1b6c3feff23d856b4f1d7bc0c833accec40d04370",
    "fonts": "3e35b3dffd1b5d44982975d24af625ec6f8a5df7cf5e0960abf175e3a2f19ecd",
}
REDACTION_SCAN_CRITERIA_SHA256 = (
    "3bd0e41e88e1e8cb3d8e2bb65cee9c16904e0dc05ad7989441578432960c25e9"
)
IMAGE_EDIT_CRITERIA_SHA256 = {
    "forms/criteria.json": "a060299145df5301d76491f31b5306c2e767608702620a2f4d7b39d2f662d04b",
    "forms/positive-criteria.json": "95b7a889413628fa60dea234f5a1fffc4c5a9f8b7bc17b51a23b47f7fe1c0e40",
    "forms/ui-criteria.json": "4e7d1597b6333dfa12e736f1df15b467200bc9ba22c25be9e95b8a5b6398afe2",
    "forms/bbox-criteria.json": "8a97afa0513b36f2f434278f2be140c67c666dd5edbde7e1f7e63c30f332e389",
    "criteria.json": "919048740c57e365678a13b59ae7df83b4325adb258c69b0152f3aef7ac3906c",
    "ocr-refusal.json": "72d5357f7ab5fdee46c5190bae314e8b036f418d10d1a9d90409ed9c1a0d8535",
    "ui-criteria.json": "27e1806a8c0c0c1a99510fc08b729fc25db0e7aa4236fcf86a09728f9b405ee5",
    "ui-positive-criteria.json": "c7b7e4dcf6194f429c7e3bd7168c810ac78c5a1a5368786b54d944b4f3d098dd",
}
TEXT_EDIT_CRITERIA_SHA256 = {
    "wrapped/criteria.json": "66dd3c29b08a65171450aeaac7e804a38412ccbbeb92e9cbd11ad18628130b6f",
    "wrapped/overflow-criteria.json": "56ed71ab5635790577709fbc165acfb320816dd22d299dd3f5d2cae14913ab37",
    "forms/geometry-criteria.json": "941dd5db80d8f66a04220fb435ee1e65f0a632a06a85e403d563aeeb64f116c4",
    "forms/criteria.json": "49abe9aa1b38dda9c8b37913c9b345e53c52659ddd44fb04e0c249a5fa30ee05",
    "line-count/criteria.json": "7c0f87e39173b366f09db6f6b74c2a01ee89d6b3e6bdbc3cbe72e35177e2d33d",
    "relative-lines/criteria.json": "fd7998ba1be2f3f084fb54dd95ef258f96ce5ad22e3a903c7feb1e54a06c3c7f",
    "multiline-rotated/criteria.json": "1a1be54eae357d7f38f27434472035ba3fcbb61ecfa467998eb02515d496b3c5",
    "multiline/criteria.json": "014c1d1cce3474212ddbe69c6ff588fcaf716fa8e8010e597af30b3c99230bbd",
    "font-styles.json": "138df9c19f016742f08c9de03b9a05c2df20db53ae90a69f462fcd6164bc7f39",
    "font-state/criteria.json": "956a205c49361825e92b64a0bfc0a8972b7183d52a7d9c7a3b56ec091f05e06e",
    "criteria.json": "79897ed58a76ef9f78840827807256e82b6bc47b49491e0a8a8b7b3dfa1abdcb",
    "visible/criteria.json": "50d1ccff8c01bcfd3892cba6147154d910b05e00b86319b5db2c51fcae47d17d",
    "rotated/criteria.json": "4da0d1a272198846cd6c46a12969ddd66ec53da5f71f1a1ba0c91933cf586ed9",
    "ui-criteria.json": "ec9cc2964bf31f21143b2ec2a9b3ae51fe690df0ebca544c55677797e569cb3e",
}


def sha256(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    lock = json.loads((ROOT / "dependency-lock.json").read_text(encoding="utf-8"))
    truth = ROOT / "fixtures/ground-truth.json"
    if sha256(truth) != GROUND_TRUTH_SHA256:
        raise RuntimeError(
            "Frozen ground truth changed; do not retune acceptance inputs."
        )
    entries = json.loads((ROOT / "fixtures/manifest.json").read_text(encoding="utf-8"))
    for entry in entries:
        # The frozen manifest was produced on Windows. Keep its bytes unchanged
        # while interpreting its relative paths on both Windows and CI's Linux.
        path = ROOT / "fixtures" / entry["file"].replace("\\", "/")
        if sha256(path) != entry["sha256"]:
            raise RuntimeError(f"Fixture hash mismatch: {entry['file']}")
    viewer_manifest = ROOT / "fixtures/viewer-search-manifest.json"
    if sha256(viewer_manifest) != VIEWER_SEARCH_MANIFEST_SHA256:
        raise RuntimeError("Frozen viewer search input/expectations changed")
    viewer = json.loads(viewer_manifest.read_text(encoding="utf-8"))
    if sha256(ROOT / "fixtures" / viewer["file"]) != viewer["sha256"]:
        raise RuntimeError("Viewer search PDF hash mismatch")
    selection_manifest = ROOT / "fixtures/viewer-selection-manifest.json"
    if sha256(selection_manifest) != VIEWER_SELECTION_MANIFEST_SHA256:
        raise RuntimeError("Frozen viewer selection expectations changed")
    restricted_manifest = ROOT / "fixtures/viewer-selection-restricted-manifest.json"
    if sha256(restricted_manifest) != VIEWER_SELECTION_RESTRICTED_SHA256:
        raise RuntimeError("Frozen copy permission expectations changed")
    restricted = json.loads(restricted_manifest.read_text(encoding="utf-8"))
    if sha256(ROOT / "fixtures" / restricted["file"]) != restricted["sha256"]:
        raise RuntimeError("Copy permission PDF hash mismatch")
    navigation_manifest = ROOT / "fixtures/viewer-navigation-manifest.json"
    if sha256(navigation_manifest) != VIEWER_NAVIGATION_MANIFEST_SHA256:
        raise RuntimeError("Frozen navigation destinations/expectations changed")
    navigation = json.loads(navigation_manifest.read_text(encoding="utf-8"))
    for file_key, hash_key in [
        ("file", "sha256"),
        ("restricted_file", "restricted_sha256"),
    ]:
        if sha256(ROOT / "fixtures" / navigation[file_key]) != navigation[hash_key]:
            raise RuntimeError("Navigation fixture hash mismatch")
    for path in (ROOT / "scripts").glob("*.py"):
        ast.parse(path.read_text(encoding="utf-8-sig"), filename=str(path))
    certificates = ROOT / "fixtures/certificate-signatures"
    images = ROOT / "fixtures/existing-image-edit"
    body = ROOT / "fixtures/existing-text-edit"
    for name, expected in TEXT_EDIT_CRITERIA_SHA256.items():
        fixed = body / name
        if sha256(fixed) != expected:
            raise RuntimeError("Frozen body-text criteria changed: " + name)
        contents = json.loads(fixed.read_text(encoding="utf-8"))
        if (
            "file" in contents
            and sha256(fixed.parent / contents["file"]) != contents["sha256"]
        ):
            raise RuntimeError("Frozen body-text input changed: " + name)
        for relative, expected_input in contents.get("files", {}).items():
            source_folder = (
                (body / contents["source_folder"]).resolve()
                if "source_folder" in contents
                else fixed.parent
            )
            if not source_folder.is_relative_to(body.resolve()):
                raise RuntimeError("Body-text source outside fixture directory")
            if sha256(source_folder / relative) != expected_input:
                raise RuntimeError("Frozen body-text input changed: " + relative)
    for name, expected in IMAGE_EDIT_CRITERIA_SHA256.items():
        if sha256(images / name) != expected:
            raise RuntimeError("Frozen existing-image criteria changed: " + name)
        fixed = images / name
        contents = json.loads(fixed.read_text(encoding="utf-8"))
        for relative, expected_input in contents.get("files", {}).items():
            if sha256(fixed.parent / relative) != expected_input:
                raise RuntimeError("Frozen existing-image input changed: " + relative)
        if "source" in contents and "source_sha256" in contents:
            if sha256(fixed.parent / contents["source"]) != contents["source_sha256"]:
                raise RuntimeError("Frozen existing-image UI input changed: " + name)
    image_plan = json.loads((images / "criteria.json").read_text(encoding="utf-8"))
    for name, expected in image_plan["files"].items():
        if sha256(images / name) != expected:
            raise RuntimeError("Existing-image fixture changed: " + name)
    for name, expected in image_plan["source_inputs"].items():
        if sha256(ROOT / name) != expected:
            raise RuntimeError("Existing-image source changed: " + name)
    ocr_image = json.loads((images / "ocr-refusal.json").read_text(encoding="utf-8"))
    if sha256(images / ocr_image["file"]) != ocr_image["sha256"]:
        raise RuntimeError("Existing-image OCR refusal input changed")
    redaction = ROOT / "fixtures/redaction-copy"
    scan_criteria = redaction / "scan-then-ocr.json"
    if sha256(scan_criteria) != REDACTION_SCAN_CRITERIA_SHA256:
        raise RuntimeError("Frozen scan redaction/OCR criteria changed")
    scan = json.loads(scan_criteria.read_text(encoding="utf-8"))
    for file_key, hash_key in (
        ("source", "source_sha256"),
        ("digital_source", "digital_source_sha256"),
    ):
        if sha256(ROOT / "fixtures" / scan[file_key]) != scan[hash_key]:
            raise RuntimeError("Frozen scan redaction/OCR source changed")
    for folder, expected in REDACTION_CRITERIA_SHA256.items():
        criteria = redaction / folder / "criteria.json"
        if sha256(criteria) != expected:
            raise RuntimeError("Frozen redaction criteria changed: " + folder)
        contents = json.loads(criteria.read_text(encoding="utf-8"))
        if folder == "foundation":
            if (
                sha256(criteria.parent / "unsafe-source.pdf")
                != contents["source_sha256"]
            ):
                raise RuntimeError("Redaction foundation input changed")
        else:
            font = "assets/fonts/NotoSansJP.ttf"
            if lock["files"][font]["sha256"] != contents["font_sha256"]:
                raise RuntimeError("Redaction font input changed")
            if (ROOT / font).is_file() and sha256(ROOT / font) != contents[
                "font_sha256"
            ]:
                raise RuntimeError("Installed redaction font differs from its pin")
            for case in contents["cases"]:
                if sha256(criteria.parent / case["file"]) != case["sha256"]:
                    raise RuntimeError("Redaction font PDF changed: " + case["file"])
    if sha256(certificates / "manifest.json") != CERTIFICATE_MANIFEST_SHA256:
        raise RuntimeError("Frozen certificate verification criteria changed")
    certificate_cases = json.loads(
        (certificates / "manifest.json").read_text(encoding="utf-8")
    )
    if sha256(certificates / "test-ca.der") != certificate_cases["test_ca_sha256"]:
        raise RuntimeError("Certificate test CA changed")
    for entry in certificate_cases["cases"]:
        if sha256(certificates / entry["file"]) != entry["sha256"]:
            raise RuntimeError("Certificate fixture changed: " + entry["file"])
    scans = ROOT / "fixtures/real-scans"
    if sha256(scans / "manifest.json") != REAL_SCAN_MANIFEST_SHA256:
        raise RuntimeError("Frozen real-scan provenance/truth changed")
    for entry in json.loads((scans / "manifest.json").read_text(encoding="utf-8")):
        for name, hash_name in (("image", "source_sha256"), ("pdf", "pdf_sha256")):
            if sha256(scans / entry[name]) != entry[hash_name]:
                raise RuntimeError("Real-scan input hash mismatch")
    head = subprocess.check_output(
        ["git", "-C", str(ROOT / "vendor/PDF4QT"), "rev-parse", "HEAD"], text=True
    ).strip()
    if head != lock["pdf4qt"]["commit"]:
        raise RuntimeError("PDF4QT checkout does not match dependency-lock.json")
    for name in (
        "compiler_adaptation",
        "manipulator_adaptation",
        "image_decode_adaptation",
        "writer_adaptation",
        "security_adaptation",
    ):
        adaptation = lock["pdf4qt"][name]
        for path_key, hash_key in [
            ("file", "sha256_LF"),
            ("source_file", "source_sha256_LF"),
        ]:
            contents = (
                (ROOT / adaptation[path_key]).read_bytes().replace(b"\r\n", b"\n")
            )
            if hashlib.sha256(contents).hexdigest() != adaptation[hash_key]:
                raise RuntimeError(f"Pinned {name} changed; review and update its lock")
        if name == "image_decode_adaptation":
            contents = (
                (ROOT / adaptation["cms_source_file"])
                .read_bytes()
                .replace(b"\r\n", b"\n")
            )
            if (
                hashlib.sha256(contents).hexdigest()
                != adaptation["cms_source_sha256_LF"]
            ):
                raise RuntimeError("Pinned generic CMS conversion changed")
    metrics = json.loads(
        (ROOT / "scripts/standard-font-metrics-source.json").read_text(encoding="utf-8")
    )
    if metrics["reportlab_version"] != "4.4.9" or metrics["license"] != "BSD-3-Clause":
        raise RuntimeError("Review standard font metric provenance before changing it")
    header = (ROOT / metrics["generated_header"]).read_bytes().replace(b"\r\n", b"\n")
    if hashlib.sha256(header).hexdigest() != metrics["generated_header_sha256_LF"]:
        raise RuntimeError("Generated standard PDF widths changed")
    if sha256(ROOT / metrics["license_file"]) != metrics["license_sha256"]:
        raise RuntimeError("Standard metric copyright or license notice changed")
    print(
        f"PASS: {len(entries)} M1 fixtures, 21 certificate cases, viewer search/selection/navigation expectations and permissions, ground truth, Python syntax, PDF4QT pin"
    )


if __name__ == "__main__":
    main()
