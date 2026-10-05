"""Check saved navigation semantics with independent pypdf/PDFium readers."""

import argparse
import hashlib
import json
from pathlib import Path

import pypdfium2 as pdfium
from pypdf import PdfReader
from pypdf.generic import IndirectObject, DictionaryObject, ArrayObject


def semantics(reader):
    pages = {
        (p.indirect_reference.idnum, p.indirect_reference.generation): i + 1
        for i, p in enumerate(reader.pages)
    }

    def value(obj):
        if isinstance(obj, IndirectObject):
            key = (obj.idnum, obj.generation)
            if key in pages:
                return {"physical_page": pages[key]}
            return value(obj.get_object())
        if isinstance(obj, DictionaryObject):
            return {str(k): value(v) for k, v in obj.items()}
        if isinstance(obj, ArrayObject):
            return [value(v) for v in obj]
        if obj is None or type(obj).__name__ == "NullObject":
            return None
        if isinstance(obj, (float, int, str, bool)):
            return obj
        return str(obj)

    root = reader.trailer["/Root"]

    def outline(first):
        result = []
        node = first
        while node:
            node = node.get_object()
            entry = {k: value(node[k]) for k in ("/Title", "/A", "/Dest") if k in node}
            if "/First" in node:
                entry["children"] = outline(node["/First"])
            result.append(entry)
            node = node.get("/Next")
        return result

    return {
        "page_labels": value(root["/PageLabels"]),
        "names": value(root["/Names"]),
        "outline": outline(root["/Outlines"].get("/First")),
        "links": [
            {
                k: value(annotation[k])
                for k in (
                    "/Subtype",
                    "/Rect",
                    "/QuadPoints",
                    "/F",
                    "/Border",
                    "/A",
                    "/Dest",
                )
                if k in annotation
            }
            for page in reader.pages
            for obj in page.get("/Annots", [])
            if (annotation := obj.get_object()).get("/Subtype") == "/Link"
        ],
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("input", type=Path)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    args.output.mkdir(parents=True, exist_ok=False)
    source = root / "fixtures/viewer-navigation.pdf"
    saved = args.input / "navigation-preserved.pdf"
    before = semantics(PdfReader(source))
    saved_reader = PdfReader(saved)
    after = semantics(saved_reader)
    assert (
        before == after
    ), "Saved outline, named destinations, labels and links must retain semantics"
    texts = []
    with pdfium.PdfDocument(saved) as document:
        assert len(document) == 6
        for i in range(len(document)):
            page = document[i]
            text = page.get_textpage()
            bitmap = None
            try:
                texts.append(text.get_text_bounded())
                bitmap = page.render(scale=1.2, draw_annots=True)
                bitmap.to_pil().save(args.output / f"navigation-page-{i+1}.png")
            finally:
                if bitmap is not None:
                    bitmap.close()
                text.close()
                page.close()
    signatures = [
        a.get_object()
        for a in saved_reader.pages[0]["/Annots"]
        if a.get_object().get("/Subtype") == "/Stamp"
    ]
    assert (
        len(signatures) == 1 and signatures[0]["/Contents"] == "山田 太郎"
    ), "Signature annotation retains exact Unicode text"
    assert signatures[0]["/AP"][
        "/N"
    ].get_data(), "Signature has a nonempty appearance stream"
    assert all(
        "文書内の参照" in text for text in texts
    ), "Digital text retained on all pages"
    result = {
        "status": "PASS",
        "saved_sha256": hashlib.sha256(saved.read_bytes()).hexdigest(),
        "source_sha256": hashlib.sha256(source.read_bytes()).hexdigest(),
        "pypdf_semantics_identical": True,
        "link_count": len(after["links"]),
        "PDFium_pages_rendered": len(texts),
        "Japanese_signature_annotation_text": "山田 太郎",
        "signature_body_text_search": "Not required by SPEC 5.2; stamp annotation is separate from body text",
        "semantics": after,
        "native_external_navigation": "未実行; semantic read and independent page rendering",
    }
    (args.output / "navigation-export.json").write_text(
        json.dumps(result, ensure_ascii=False, indent=2) + "\n", encoding="utf-8"
    )
    print("PASS: saved navigation semantics, six rendered pages, Japanese signature")


if __name__ == "__main__":
    main()
