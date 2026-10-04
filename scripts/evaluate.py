"""Independent PDFium + Poppler + pypdf verification. Thresholds from ACCEPTANCE.md."""

from pathlib import Path
import json, re, unicodedata, subprocess, difflib, sys
import pypdfium2 as pdfium
from pypdf import PdfReader
from PIL import Image, ImageChops

ROOT = Path(__file__).resolve().parents[1]
OUT = Path(sys.argv[1]).resolve() if len(sys.argv) > 1 else ROOT / "evidence/run"
FX = ROOT / "fixtures"
from test_tools import poppler_tool

PDFTOPPM = poppler_tool("pdftoppm")


def norm(s):
    return re.sub(r"\s+", " ", unicodedata.normalize("NFC", s)).strip()


def distance(a, b):
    prev = list(range(len(b) + 1))
    for i, x in enumerate(a, 1):
        cur = [i]
        for j, y in enumerate(b, 1):
            cur.append(min(cur[-1] + 1, prev[j] + 1, prev[j - 1] + (x != y)))
        prev = cur
    return prev[-1]


def text_lines(page):
    tp = page.get_textpage()
    rows = []
    chars = []
    boxes = []

    def flush():
        if boxes:
            rows.append(
                {
                    "text": "".join(chars),
                    "bbox": [
                        min(b[0] for b in boxes),
                        min(b[1] for b in boxes),
                        max(b[2] for b in boxes),
                        max(b[3] for b in boxes),
                    ],
                }
            )
        chars.clear()
        boxes.clear()

    for index in range(tp.count_chars()):
        c = tp.get_text_range(index, 1)
        if c in ["\r", "\n"]:
            flush()
            continue
        if c.strip():
            box = tp.get_charbox(index)
            # A viewer can join a short paragraph's final line with the next
            # paragraph in extracted text. Measure physical lines by their
            # disjoint vertical ink bands, independently of inserted newlines.
            if boxes and (
                box[3] < min(b[1] for b in boxes) or box[1] > max(b[3] for b in boxes)
            ):
                flush()
            boxes.append(box)
        chars.append(c)
    flush()
    return rows


result = {
    "engines": {
        "pdfium": str(pdfium.PDFIUM_INFO),
        "pypdf": "see dependency-lock.json",
        "poppler": subprocess.run(
            [str(PDFTOPPM), "-v"], capture_output=True, text=True
        ).stderr.strip(),
    },
    "acceptance_thresholds": {
        "CER_jpn": 0.02,
        "CER_eng": 0.01,
        "search_each_language": 19,
        "search_total": 20,
        "position_max_mm": 2,
    },
    "viewer_GUI": {
        "Adobe_Reader": "未実行",
        "Firefox": "未実行",
        "physical_print": "未実行",
        "PDF_printer": "未実行",
    },
}
if (OUT / "D03-ocr.pdf").exists():
    truth = json.loads((FX / "ground-truth.json").read_text(encoding="utf-8"))
    doc = pdfium.PdfDocument(OUT / "D03-ocr.pdf")
    source = pdfium.PdfDocument(FX / "D03-digital-source.pdf")
    rows = []
    texts = []
    for i, p in enumerate(truth["pages"]):
        page = doc[i]
        textpage = page.get_textpage()
        text = textpage.get_text_range()
        texts.append(text)
        a, b = norm(p["text"]), norm(text)
        err = distance(a, b)
        rows.append(
            {
                "page": i + 1,
                "language": p["language"],
                "split": p["split"],
                "reference_characters": len(a),
                "edit_distance": err,
                "CER": err / len(a),
                "copy_text": text,
            }
        )
        (OUT / f"D03-page-{i+1}-diff.txt").write_text(
            "\n".join(difflib.ndiff(a.split(" "), b.split(" "))), encoding="utf-8"
        )
    result["OCR_pages"] = rows
    result["OCR_evaluation"] = {}
    result["OCR_line_alignment"] = []
    for i in range(len(doc)):
        reference, actual = text_lines(source[i]), text_lines(doc[i])
        measurements = []
        for line, (a, b) in enumerate(zip(reference, actual), 1):
            delta = max(abs(x - y) for x, y in zip(a["bbox"], b["bbox"])) * 25.4 / 72
            measurements.append(
                {
                    "line": line,
                    "reference_bbox_pt": a["bbox"],
                    "output_bbox_pt": b["bbox"],
                    "max_edge_delta_mm": delta,
                    "text_edit_distance": distance(norm(a["text"]), norm(b["text"])),
                }
            )
        matched = len(reference) == len(actual)
        maximum = max(
            (m["max_edge_delta_mm"] for m in measurements), default=float("inf")
        )
        result["OCR_line_alignment"].append(
            {
                "page": i + 1,
                "reference_lines": len(reference),
                "output_lines": len(actual),
                "max_mm": maximum,
                "status": "PASS" if matched and maximum <= 2 else "FAIL",
                "lines": measurements,
            }
        )
    if (OUT / "D03-app-text.json").exists():
        app_text = json.loads((OUT / "D03-app-text.json").read_text(encoding="utf-8"))
        result["app_OCR_copy"] = []
        for i, p in enumerate(truth["pages"]):
            a, b = norm(p["text"]), norm(app_text[i])
            result["app_OCR_copy"].append(
                {
                    "page": i + 1,
                    "language": p["language"],
                    "split": p["split"],
                    "CER": distance(a, b) / len(a),
                }
            )
    for lang, threshold in [("jpn", 0.02), ("eng", 0.01)]:
        selected = [
            r for r in rows if r["language"] == lang and r["split"] == "evaluation"
        ]
        cer = sum(r["edit_distance"] for r in selected) / sum(
            r["reference_characters"] for r in selected
        )
        found = []
        for term in truth["search_terms"][lang]:
            locations = []
            for row in selected:
                i = row["page"] - 1
                tp = doc[i].get_textpage()
                search = tp.search(term)
                hit = search.get_next()
                if hit:
                    start, count = hit
                    ocr_boxes = [tp.get_charbox(j) for j in range(start, start + count)]
                    sp = source[i].get_textpage()
                    source_hit = sp.search(term).get_next()
                    delta = None
                    if source_hit:
                        st, ct = source_hit
                        source_boxes = [sp.get_charbox(j) for j in range(st, st + ct)]

                        def union(boxes):
                            return [
                                min(x[0] for x in boxes),
                                min(x[1] for x in boxes),
                                max(x[2] for x in boxes),
                                max(x[3] for x in boxes),
                            ]

                        ob, sb = union(ocr_boxes), union(source_boxes)
                        delta = max(abs(x - y) for x, y in zip(ob, sb)) * 25.4 / 72
                    locations.append({"page": i + 1, "position_delta_mm": delta})
            found.append(
                {"term": term, "found": bool(locations), "locations": locations}
            )
        result["OCR_evaluation"][lang] = {
            "CER": cer,
            "CER_status": "PASS" if cer <= threshold else "FAIL",
            "searches": found,
            "search_found": sum(x["found"] for x in found),
            "search_status": "PASS" if sum(x["found"] for x in found) >= 19 else "FAIL",
        }
    result["OCR_visible_preservation"] = []
    original = pdfium.PdfDocument(FX / "D03.pdf")
    for i in range(len(doc)):
        a = original[i].render(scale=1).to_pil().convert("RGB")
        b = doc[i].render(scale=1).to_pil().convert("RGB")
        diff = ImageChops.difference(a, b)
        bbox = diff.getbbox()
        result["OCR_visible_preservation"].append(
            {
                "page": i + 1,
                "changed_bbox": bbox,
                "status": "PASS" if bbox is None else "FAIL",
            }
        )
    doc[2].render(scale=1.5).to_pil().save(OUT / "D03-pdfium.png")
result["mixed_visible_preservation"] = []
for before, after in [
    (FX / "D04.pdf", OUT / "D04-ocr.pdf"),
    (OUT / "D05-before.pdf", OUT / "D05-ocr.pdf"),
]:
    if not after.exists():
        continue
    a, b = pdfium.PdfDocument(before), pdfium.PdfDocument(after)
    for i in range(len(a)):
        diff = ImageChops.difference(
            a[i].render(scale=1).to_pil().convert("RGB"),
            b[i].render(scale=1).to_pil().convert("RGB"),
        )
        result["mixed_visible_preservation"].append(
            {
                "input": before.name,
                "page": i + 1,
                "changed_bbox": diff.getbbox(),
                "status": "PASS" if diff.getbbox() is None else "FAIL",
            }
        )
if (OUT / "combined.pdf").exists():
    combined = pdfium.PdfDocument(OUT / "combined.pdf")
    reference = pdfium.PdfDocument(FX / "D03-digital-source.pdf")
    actual_lines = [
        row
        for row in text_lines(combined[0])
        if "DIGITAL" not in row["text"] and "PAGE" not in row["text"]
    ]
    source_lines = text_lines(reference[0])
    measured = []
    for line, (a, b) in enumerate(zip(source_lines, actual_lines), 1):
        measured.append(
            {
                "line": line,
                "max_edge_delta_mm": max(
                    abs(x - y) for x, y in zip(a["bbox"], b["bbox"])
                )
                * 25.4
                / 72,
            }
        )
    result["rotated_OCR_alignment"] = {
        "rotation": 90,
        "reference_lines": len(source_lines),
        "output_lines": len(actual_lines),
        "lines": measured,
        "max_mm": max((m["max_edge_delta_mm"] for m in measured), default=None),
        "coordinates": "PDFium text boxes in unrotated PDF page coordinates",
    }
if (OUT / "signature.pdf").exists():
    for name in [
        "signature",
        "signature-reedited",
        "coordinates",
        "combined",
        "form-preserved",
    ]:
        pdf = OUT / f"{name}.pdf"
        if not pdf.exists():
            continue
        subprocess.run(
            [
                str(PDFTOPPM),
                "-f",
                "1",
                "-singlefile",
                "-scale-to",
                "1400",
                "-png",
                str(pdf),
                str(OUT / f"{name}-poppler"),
            ],
            check=True,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.PIPE,
        )
        p = pdfium.PdfDocument(pdf)
        p.init_forms()
        p[0].render(scale=1.5).to_pil().save(OUT / f"{name}-pdfium.png")
    result["signature"] = {
        "annotations": [],
        "engines_rendered": ["PDFium", "Poppler"],
        "printing": "未実行",
    }
    r = PdfReader(OUT / "signature.pdf")
    for page in r.pages:
        for a in page.get("/Annots", []):
            d = a.get_object()
            result["signature"]["annotations"].append(
                {
                    "subtype": str(d.get("/Subtype")),
                    "contents": str(d.get("/Contents")),
                    "appearance": bool(d.get("/AP")),
                    "print_flag": bool(d.get("/F", 0) & 4),
                    "embedded_edit_metadata": bool(d.get("/Tatsujin")),
                }
            )
if (OUT / "form-preserved.pdf").exists():
    a, b = PdfReader(FX / "D07.pdf"), PdfReader(OUT / "form-preserved.pdf")
    values = lambda r: {k: str(v.get("/V")) for k, v in r.get_fields().items()}
    subtypes = lambda r: [
        str(x.get_object().get("/Subtype"))
        for p in r.pages
        for x in p.get("/Annots", [])
    ]
    result["form_preservation"] = {
        "before_values": values(a),
        "after_values": values(b),
        "values_equal": values(a) == values(b),
        "before_annotations": subtypes(a),
        "after_annotations": subtypes(b),
        "outline_before": str(a.outline),
        "outline_after": str(b.outline),
    }

    def outline(r):
        return [
            {"title": str(d.get("/Title")), "page": r.get_destination_page_number(d)}
            for d in r.outline
            if isinstance(d, dict)
        ]

    def links(r):
        return [
            str(x.get_object().get("/A", {}).get("/URI"))
            for page in r.pages
            for x in page.get("/Annots", [])
            if x.get_object().get("/Subtype") == "/Link"
        ]

    def notes(r):
        return [
            str(x.get_object().get("/Contents"))
            for page in r.pages
            for x in page.get("/Annots", [])
            if x.get_object().get("/Subtype") == "/Text"
        ]

    result["form_preservation"].update(
        outline_equal=outline(a) == outline(b),
        links_equal=links(a) == links(b),
        comments_equal=notes(a) == notes(b),
    )
(OUT / "independent-verification.json").write_text(
    json.dumps(result, ensure_ascii=False, indent=2), encoding="utf-8"
)
print(
    json.dumps(
        {
            "OCR_evaluation": {
                k: {
                    x: v[x]
                    for x in ["CER", "CER_status", "search_found", "search_status"]
                }
                for k, v in result.get("OCR_evaluation", {}).items()
            },
            "line_alignment": [
                {k: v for k, v in row.items() if k != "lines"}
                for row in result.get("OCR_line_alignment", [])
            ],
            "rotated_alignment": {
                k: v
                for k, v in result.get("rotated_OCR_alignment", {}).items()
                if k != "lines"
            },
            "form_values_equal": result.get("form_preservation", {}).get(
                "values_equal"
            ),
        },
        ensure_ascii=True,
        indent=2,
    )
)
