"""Inspect unsafe redaction trials independently; execution is not acceptance."""

import argparse
import hashlib
import json
from pathlib import Path

from PIL import Image
from pypdf import PdfReader
import pypdfium2 as pdfium


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def text(path):
    parts = []
    with pdfium.PdfDocument(path) as document:
        for i in range(len(document)):
            page = document[i]
            contents = page.get_textpage()
            parts.append(contents.get_text_range())
            contents.close()
            page.close()
    return "\n".join(parts)


def geometry(reader):
    return [
        dict(
            media=list(page.mediabox),
            crop=list(page.cropbox),
            rotation=page.get("/Rotate", 0),
            unit=float(page.get("/UserUnit", 1)),
        )
        for page in reader.pages
    ]


def character_boxes(path, phrase):
    with pdfium.PdfDocument(path) as document:
        page = document[0]
        contents = page.get_textpage()
        start = contents.get_text_range().index(phrase)
        boxes = [contents.get_charbox(i) for i in range(start, start + len(phrase))]
        contents.close()
        page.close()
    return boxes


def payload(reader):
    chunks = []
    for object_number in sorted(reader.xref.get(0, {})):
        if not object_number:
            continue
        from pypdf.generic import IndirectObject

        value = IndirectObject(object_number, 0, reader).get_object()
        chunks.append(str(value).encode("utf-8"))
        if hasattr(value, "get_data"):
            chunks.append(value.get_data())
    return b"\n".join(chunks)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--fixture", required=True, type=Path)
    parser.add_argument("--run", required=True, type=Path)
    args = parser.parse_args()
    target = args.run / "independent.json"
    if target.exists():
        raise FileExistsError("Preserve prior independent evaluation")
    criteria = json.loads((args.fixture / "criteria.json").read_text(encoding="utf-8"))
    source = args.fixture / "unsafe-source.pdf"
    original = PdfReader(source)
    assert digest(source) == criteria["source_sha256"]
    assert "KEEP_VISIBLE_9df67" in text(source)
    assert "SECRET_TEXT_9df67" in text(source)
    original_image = Image.open(args.fixture / "secret-image.png").convert("RGB")
    source_pixels = list(original_image.crop((44, 54, 116, 86)).get_flattened_data())
    rows = []
    probe = json.loads((args.run / "probe.json").read_text(encoding="utf-8"))
    for name in ("upstream-redact", "editor-trial", "content-trial", "sanitized-trial"):
        path = args.run / (name + ".pdf")
        if not path.exists():
            assert name == "editor-trial" and probe["editor_candidate_valid"] is False
            rows.append(
                dict(
                    strategy=name,
                    accepted=False,
                    recovered_image_pixels=None,
                    checks={"valid_serializable_candidate": False},
                    diagnostics={
                        key: value
                        for key, value in probe.items()
                        if key.endswith("writer_errors")
                    },
                )
            )
            continue
        reader = PdfReader(path)
        extracted_text = text(path)
        fields = reader.get_fields() or {}
        leaked_pixels = 0
        exact_pixels = 0
        image_count = 0
        for page in reader.pages:
            for image in page.images:
                image_count += 1
                pixels = image.image.convert("RGB")
                if pixels.size == (240, 120):
                    actual = list(pixels.crop((44, 54, 116, 86)).get_flattened_data())
                    # JPEG recompression changes exact bytes. Check a conservative
                    # interior, away from antialiasing at the region boundary.
                    leaked_pixels += sum(max(pixel) > 32 for pixel in actual)
                    exact_pixels += sum(
                        a == b and a != (0, 0, 0) for a, b in zip(actual, source_pixels)
                    )
                    pixels.save(args.run / (name + "-extracted-image.png"))
                else:
                    raise RuntimeError(
                        "Unexpected diagnostic image geometry; do not assume safe"
                    )
        data = payload(reader)
        checks = {
            "selected_visible_and_invisible_text_removed": all(
                word not in extracted_text and word.encode() not in data
                for word in ("SECRET_TEXT_9df67", "SECRET_HIDDEN_9df67")
            ),
            "selected_image_interior_removed_from_extracted_images": leaked_pixels == 0,
            "selected_form_payload_removed": b"SECRET_FORM_9df67" not in data,
            "unselected_searchable_text_preserved": all(
                word in extracted_text
                for word in ("KEEP_VISIBLE_9df67", "KEEP_SECOND_PAGE_9df67")
            ),
            "unselected_form_preserved": fields.get("keep-field", {}).get("/V")
            == "KEEP_FORM_9df67",
            "page_geometry_preserved": geometry(reader) == geometry(original),
            "metadata_attachment_orphan_payload_removed": all(
                word not in data
                for word in (
                    b"SECRET_META_9df67",
                    b"SECRET_XMP_9df67",
                    b"SECRET_ATTACH_9df67",
                    b"SECRET_ORPHAN_9df67",
                )
            ),
            "source_unchanged": digest(source) == criteria["source_sha256"],
        }
        rows.append(
            dict(
                strategy=name,
                sha256=digest(path),
                checks=checks,
                accepted=all(checks.values()),
                recovered_image_pixels=leaked_pixels,
                exact_original_pixels=exact_pixels,
                extracted_images=image_count,
                fields=list(fields),
                geometry=geometry(reader),
            )
        )
    followup = []
    if "candidate_checks" in probe:
        assert probe["candidate_checks"]["status"] == "PASS"
        assert len(probe["candidate_checks"]["rejected_cases"]) == 16
        before_boxes = character_boxes(
            args.run / "text-advance-source.pdf", "KEEP_FOLLOW_9df67"
        )
        after_boxes = character_boxes(
            args.run / "sanitized-advance.pdf", "KEEP_FOLLOW_9df67"
        )
        assert before_boxes == after_boxes
        assert "SECRET_TEXT_9df67" not in text(args.run / "sanitized-advance.pdf")
        followup.append(
            dict(
                scope="Following text in same BT; independent PDFium boxes exactly equal",
                characters=len(after_boxes),
                source_sha256=digest(args.run / "text-advance-source.pdf"),
                output_sha256=digest(args.run / "sanitized-advance.pdf"),
            )
        )
        annotation_path = args.run / "sanitized-annotations.pdf"
        annotation_reader = PdfReader(annotation_path)
        annotations = [
            value.get_object() for value in annotation_reader.pages[0]["/Annots"]
        ]
        assert len(annotations) == 2
        assert any(value.get("/Contents") == "KEEP_NOTE_9df67" for value in annotations)
        annotation_payload = payload(annotation_reader)
        for marker in (
            b"SECRET_NOTE_9df67",
            b"SECRET_POPUP_9df67",
            b"SECRET_REPLY_9df67",
        ):
            assert marker not in annotation_payload
        followup.append(
            dict(
                scope="Selected comment and its outside popup/reply removed; outside comment/form preserved",
                sha256=digest(annotation_path),
                remaining_annotations=2,
            )
        )
        for name, expected in (
            ("sanitized-reinput", "KEEP_REEDIT_9df67"),
            ("sanitized-unsaved-signature", "KEEP_FORM_9df67"),
        ):
            path = args.run / (name + ".pdf")
            reader = PdfReader(path)
            fields = reader.get_fields() or {}
            data = payload(reader)
            assert set(fields) == {"keep-field"}
            assert fields["keep-field"]["/V"] == expected
            assert fields["keep-field"]["/DV"] == "KEEP_FORM_9df67"
            assert geometry(reader) == geometry(original)
            for marker in (
                b"SECRET_TEXT_9df67",
                b"SECRET_HIDDEN_9df67",
                b"SECRET_FORM_9df67",
                b"SECRET_META_9df67",
                b"SECRET_XMP_9df67",
                b"SECRET_ATTACH_9df67",
                b"SECRET_ORPHAN_9df67",
            ):
                assert marker not in data
            assert "KEEP_VISIBLE_9df67" in text(path)
            followup.append(
                dict(
                    file=name + ".pdf",
                    sha256=digest(path),
                    current_value=expected,
                    reset_default="KEEP_FORM_9df67",
                    preserved_geometry=True,
                    secret_markers_absent=True,
                )
            )
    if probe.get("candidate_checks", {}).get("shared_image_resource_rejections"):
        assert len(probe["candidate_checks"]["shared_image_resource_rejections"]) == 2
        assert not (args.run / "shared-image-unsafe-trial.pdf").exists()
        for name in ("sanitized-all-image-uses", "sanitized-metadata-image-alias"):
            path = args.run / (name + ".pdf")
            reader = PdfReader(path)
            assert geometry(reader) == geometry(original)
            assert set(reader.get_fields()) == {"keep-field"}
            data = payload(reader)
            for marker in (
                b"SECRET_TEXT_9df67",
                b"SECRET_HIDDEN_9df67",
                b"SECRET_FORM_9df67",
                b"SECRET_META_9df67",
                b"SECRET_XMP_9df67",
                b"SECRET_ATTACH_9df67",
                b"SECRET_ORPHAN_9df67",
            ):
                assert marker not in data
            recovered = 0
            images = 0
            for page in reader.pages:
                for image in page.images:
                    images += 1
                    pixels = list(
                        image.image.convert("RGB")
                        .crop((44, 54, 116, 86))
                        .get_flattened_data()
                    )
                    recovered += sum(a == b for a, b in zip(pixels, source_pixels))
            assert images == 1 and recovered == 0, "Original shared image survived"
            assert "KEEP_VISIBLE_9df67" in text(path)
            followup.append(
                dict(
                    file=path.name,
                    sha256=digest(path),
                    recovered_selected_original_pixels=recovered,
                    geometry_and_remaining_form_preserved=True,
                )
            )
    result = dict(
        status="EXECUTED",
        safe_product_implementation=False,
        source_sha256=digest(source),
        criteria=criteria,
        strategies=rows,
        candidate_checks=probe.get("candidate_checks"),
        saved_reedit_checks=followup,
        scope="Synthetic fixed fixture; pypdf all objects/streams and images, PDFium text. No general security guarantee.",
    )
    target.write_text(
        json.dumps(result, ensure_ascii=False, indent=2), encoding="utf-8"
    )
    print(
        json.dumps(
            {
                row["strategy"]: dict(
                    accepted=row["accepted"],
                    recovered_pixels=row["recovered_image_pixels"],
                    failed=[key for key, value in row["checks"].items() if not value],
                )
                for row in rows
            },
            indent=2,
        )
    )


if __name__ == "__main__":
    main()
