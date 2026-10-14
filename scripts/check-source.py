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
REDACTION_PARTIAL_CRITERIA_SHA256 = (
    "fdde07694bcdc03c679b56d8aaacd81150809cdea3e6862150b0a8d5411dbffa"
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
    "flowing-parts-v1/criteria.json": "1ac4f62edf2325101e0c8d0230a339518d6ae449eafd74fec2f5351fea968818",
    "flowing-lines-v1/criteria.json": "7d30fb24b74b61ffd35558f513d68a87d2b21a7871a86eb69161ccb97a629bd7",
    "styled-form-parts-v1/criteria.json": "d7ae8793fb2d2e3e27f452885c75a005a30ab7a55ab2b18d452358302c6969e9",
    "styled-parts-v1/criteria.json": "76781b4b06150f72fcc94d5cf9b693cd4c2485e0a58e0ba78549df5348e1aab9",
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
    for path in (ROOT / "assets/office").glob("*.py"):
        ast.parse(path.read_text(encoding="utf-8-sig"), filename=str(path))
    certificates = ROOT / "fixtures/certificate-signatures"
    office = ROOT / "fixtures/office-import"
    tables = ROOT / "fixtures/table-extraction"
    word = ROOT / "fixtures/pdf-text-docx/criteria.json"
    pdfa = ROOT / "fixtures/pdfa-validation/criteria.json"
    if (
        sha256(pdfa)
        != "f100bd83cc754dfa2ebfc8d7cf399d90082c48309d7e20552a4f123fae279397"
    ):
        raise RuntimeError("Frozen supplementary PDF/A criteria changed")
    for row in json.loads(pdfa.read_text(encoding="utf-8"))["files"]:
        if sha256(pdfa.parent / row["file"]) != row["sha256"]:
            raise RuntimeError("Frozen PDF/A input changed: " + row["file"])
    if (
        sha256(word)
        != "52a3dbd5e25b943939066defc94e51840d7601cb392b75c9bd5df63115d570f9"
    ):
        raise RuntimeError("Frozen supplementary Word text criteria changed")
    word_fixed = json.loads(word.read_text(encoding="utf-8"))
    for name in ("source", "image_only", "copy_restricted"):
        if sha256(word.parent / word_fixed[name]) != word_fixed[name + "_sha256"]:
            raise RuntimeError("Frozen supplementary Word text input changed: " + name)
    if (
        sha256(tables / "criteria.json")
        != "07c15ed09f71d8b5f1b5eeff45b65a00a313d8a05492f43346e7d9718b752672"
    ):
        raise RuntimeError("Frozen table extraction criteria changed")
    table_plan = json.loads((tables / "criteria.json").read_text(encoding="utf-8"))
    for name, expected in table_plan["files"].items():
        if sha256(tables / name) != expected:
            raise RuntimeError("Frozen table input changed: " + name)
    if (
        lock["files"][table_plan["font"]["file"]]["sha256"]
        != table_plan["font"]["sha256"]
    ):
        raise RuntimeError("Frozen table font pin differs")
    if sha256(office / "criteria.json") != (
        "18a8ddead69890390cf52cf9fca782563cd23c4d30dfdbed58e9bb6800ac3785"
    ):
        raise RuntimeError("Frozen DOCX criteria changed")
    office_plan = json.loads((office / "criteria.json").read_text(encoding="utf-8"))
    for name, expected in office_plan["files"].items():
        if sha256(office / name) != expected:
            raise RuntimeError("Frozen DOCX input changed: " + name)
    sheets = office / "sheets-slides"
    if sha256(sheets / "criteria.json") != (
        "71d849f6df2d8743e291fe2038ddfc819881588eef226b11070d74b51f4addbc"
    ):
        raise RuntimeError("Frozen spreadsheet/presentation criteria changed")
    for name, expected in json.loads(
        (sheets / "criteria.json").read_text(encoding="utf-8")
    )["files"].items():
        if sha256(sheets / name) != expected:
            raise RuntimeError("Frozen spreadsheet/presentation input changed: " + name)
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
    for folder, expected in {
        "shared-fonts-v1": "a24478e7395e3bfcc4046eb0266aba324c5858d6c3c9db0e447aba5d9ec0f586",
        "vector-paths-v1": "027f0ab58bf461ce4f47762055244e6ddbe4e3141cfeffed5e60aa7483fbc869",
        "shared-followup-v2": "d4e635ed3ec17063fece57722f81dc84d9378919ad39835c730e8387dc848c5b",
        "font-tables-v1": "7a8d6e747eba9816486ec202aac858acc1bdbf686c9c0931808fc9f725981cdd",
        "hinting-v1": "9704e6be7527959c40f97e3e1cb097798777f9edb17d855ea17430f30631ff27",
        "hinting-global-v1": "107fd6c0880449634310db823e4afe69e9937b0557ce28c321db8d94ef91170d",
        "hint-boundaries-v1": "1fcd232da7b7ae1af28430c8fdb7a0e92318c72895637a258f66c874dd3e4941",
        "cvt-references-v1": "1234729946a784d36e2d32bca4fc1e10642b01c3e900539996f236a440858be7",
        "cvt-index-boundaries-v1": "d8243815eeec4a579639502d3d66533ba876bec483fdc6e3edc813d83453c596",
        "mixed-cmap-v1": "fc20b71f3990ffe4fa468389f0867297ebe0fcf2c8aa261de03c694b8f8af118",
        "mixed-cmap-v2": "796b7ae0ef979583d0180c91c1020dd30405530d044daf39e83339f0b2ae326a",
        "empty-glyph-v1": "229109449cedde155678515bd2a6bb628d0602203541289e0e40b271f1ccf5d1",
        "custom-cmap-v1": "044024c5bf7ec5a52802e6ba0d0265237aefee83471147207c61a882f7d4a36f",
        "custom-cmap-followup-v1": "463ab0757312ee1ceb00605452bf6369e71f25c1acdd1b15ea8eb1fcfbcb720c",
        "redundant-clips-v1": "e3d31bbc74fdf0c28830fb746b1b6d29e4c058eca08f0d8940766cf401bd0c46",
        "redundant-clips-followup-v1": "81489b5b301af21191ab4103e3425a417fe2da379232e47faf56d48b848d8bc0",
        "font-aliases-v1": "06fd7bbcef23bf82d58852c351bd3d0d5098b96cbfea1ea23d5639e61332bcd9",
        "font-aliases-followup-v1": "4aa425628996b0a58f49d8f99b88421e19db3a3d61f355553607118519687550",
        "font-aliases-boundaries-v1": "e0bef06a28cfd3ae32ccb1c09c67baa8373259fbfdb63eede56ef4b1b3aadf32",
        "font-binding-v1": "b167c53f03b5939aae9206d8bf0c38b205790413698e87c93462732b4c07f62b",
        "font-binding-followup-v1": "7f585559474ea31e06f94c1b1f18c40a575e0b11e7e9dd5741d473d3bc3c9720",
        "font-schema-v2": "91765aeee26617872f16ba46f4b9cadba61468279ef157a141d8e9891630ad63",
        "font-schema-followup-v1": "a2a73f6c9a8d2a087bc511509ae7b83f2eb81a267ad5dc42eb944d38440e02c6",
        "font-schema-direct-v1": "8c40a3c704d4581ef751ddc6217295f970c92cd189bd39277ec8bb9e0c7d4f2c",
        "font-schema-direct-followup-v1": "e7569c49f5917663c07232ee1cd542ccebbbc719fd545fb9841428ad1b87361d",
        "standard-font-families-v1": "86c1deda2fe3701e4da15e4277905006d06d6907bc98b5764aa5b841edc6308d",
        "standard-font-families-v2": "708cfc9929203151427b7d09a9914cfe294b9112b13fab024abbd5fb5e58ed9d",
    }.items():
        fixed = redaction / folder / "criteria.json"
        if sha256(fixed) != expected:
            raise RuntimeError("Frozen shared/vector criteria changed: " + folder)
        for case in json.loads(fixed.read_text(encoding="utf-8"))["cases"]:
            if sha256(fixed.parent / case["file"]) != case["sha256"]:
                raise RuntimeError(
                    "Frozen shared/vector input changed: " + case["file"]
                )
    for name, expected in {
        "decoder-safety.json": "516ca5b2fb56ee44735f03679ef257949854d67107d61b447b69195fcfd9c44c",
        "decoder-safety-r2.json": "5f39b3584811a0411573999bc5e7e0aa7eb241127c1ae0266f6ab87299287efa",
        "space-selection.json": "2168ad0c1ffd1bfb0d618d2068c80265197fb3f8f21e8f2511cf4754c26e7698",
        "renderer-metrics.json": "7d0fdd3d8a1888796566467e385a0d8c11695c9b8b236065ba75241161b75056",
    }.items():
        if sha256(redaction / "mixed-cmap-v2" / name) != expected:
            raise RuntimeError("Frozen direct decoder safety history changed: " + name)
    composite = redaction / "partial-composite-v3/criteria.json"
    if (
        sha256(composite)
        != "894aa5fc4fab49b3af99676c08f34b225924b0344d402d0f27fcdbde8f919ea7"
    ):
        raise RuntimeError("Frozen surviving composite glyph criteria changed")
    for case in json.loads(composite.read_text(encoding="utf-8"))["cases"]:
        if sha256(composite.parent / case["file"]) != case["sha256"]:
            raise RuntimeError("Frozen composite font input changed")
    if (
        sha256(redaction / "partial-real-ocr.json")
        != "dd0d29256ec4b6628a68b8dac74b03bb98aa53825c5551588fd76890fd8e1ffa"
    ):
        raise RuntimeError("Frozen partial real OCR criteria changed")
    partial_criteria = redaction / "partial-text/criteria.json"
    if sha256(partial_criteria) != REDACTION_PARTIAL_CRITERIA_SHA256:
        raise RuntimeError("Frozen partial redaction criteria changed")
    for case in json.loads(partial_criteria.read_text(encoding="utf-8"))["cases"]:
        if sha256(partial_criteria.parent / case["file"]) != case["sha256"]:
            raise RuntimeError(
                "Frozen partial redaction input changed: " + case["file"]
            )
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
    vertical = ROOT / "fixtures/vertical-ocr-criteria.json"
    if (
        sha256(vertical)
        != "516522131307b548f3b46e8d8a274b398d732857455806c8df2484ee40f3e604"
    ):
        raise RuntimeError("Frozen supplementary vertical OCR criterion changed")
    vertical_fixed = json.loads(vertical.read_text(encoding="utf-8"))
    if (
        sha256(ROOT / "fixtures" / vertical_fixed["source"])
        != vertical_fixed["source_sha256"]
    ):
        raise RuntimeError("Vertical OCR source changed")
    # The published PDF and criterion are mandatory and checked above. This
    # separately downloaded generator font is needed only to regenerate them;
    # a source-only checkout deliberately does not vendor font binaries.
    fixture_font = ROOT / "fixtures/NotoSansJP-fixture.ttf"
    if (
        fixture_font.is_file()
        and sha256(fixture_font) != vertical_fixed["fixture_font_sha256"]
    ):
        raise RuntimeError("Vertical OCR fixture font changed")
    geometry = ROOT / "fixtures/vertical-ocr-geometry/criteria.json"
    if (
        sha256(geometry)
        != "4e087d6138ce25f08c7f5cc70b0b2ee2b85a7c6da69bab370d2eb74ed9417ac8"
    ):
        raise RuntimeError("Frozen supplementary vertical geometry criterion changed")
    geometry_fixed = json.loads(geometry.read_text(encoding="utf-8"))
    if (
        sha256(geometry.parent / geometry_fixed["source"])
        != geometry_fixed["source_sha256"]
    ):
        raise RuntimeError("Vertical geometry input changed")
    unit = ROOT / "fixtures/vertical-ocr-unit/criteria.json"
    if (
        sha256(unit)
        != "86afa075759f530758ed6e4580622c9743a2cd9c3bf3c9fdc86791d8ca7e8beb"
    ):
        raise RuntimeError("Frozen supplementary vertical unit criterion changed")
    unit_fixed = json.loads(unit.read_text(encoding="utf-8"))
    if sha256(unit.parent / unit_fixed["source"]) != unit_fixed["source_sha256"]:
        raise RuntimeError("Vertical unit input changed")
    pdfua = ROOT / "fixtures/pdfua-validation/criteria.json"
    if (
        sha256(pdfua)
        != "d1ebf99ed5edfb6d81c440fb9d0a1c38c8ed8cb06444a3532d2e8ffde2507efc"
    ):
        raise RuntimeError("Frozen PDF/UA machine criterion changed")
    for case in json.loads(pdfua.read_text(encoding="utf-8"))["inputs"]:
        if sha256(pdfua.parent / case["file"]) != case["sha256"]:
            raise RuntimeError("Frozen PDF/UA input changed")
    for rotation, digest in (
        (180, "66eb2f2f58a96f1bd52d469bc08d7421998bc1ea4fd5d66fcfea05d3c547ca7d"),
        (270, "f532130ff146ed8e09f32f34cdb8e2950d0218d503dac506d4eefe27a3b62c7f"),
    ):
        criterion = ROOT / f"fixtures/vertical-ocr-rotations/{rotation}/criteria.json"
        if sha256(criterion) != digest:
            raise RuntimeError(
                "Frozen supplementary vertical rotation criterion changed"
            )
        fixed = json.loads(criterion.read_text(encoding="utf-8"))
        if (
            fixed["rotation"] != rotation
            or sha256(criterion.parent / fixed["source"]) != fixed["source_sha256"]
        ):
            raise RuntimeError("Vertical rotation input changed")
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
        "font_vertical_adaptation",
    ):
        adaptation = lock["pdf4qt"][name]
        if name == "font_vertical_adaptation":
            for path_key, hash_key in (
                ("layout_source_file", "layout_source_sha256_LF"),
                ("origin_helper_file", "origin_helper_sha256_LF"),
            ):
                contents = (
                    (ROOT / adaptation[path_key]).read_bytes().replace(b"\r\n", b"\n")
                )
                if hashlib.sha256(contents).hexdigest() != adaptation[hash_key]:
                    raise RuntimeError(
                        "Pinned Japanese vertical layout/origin source changed"
                    )
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
