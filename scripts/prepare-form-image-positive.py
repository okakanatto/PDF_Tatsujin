import argparse, hashlib, json
from pathlib import Path
from pypdf import PdfReader


def main():
    parser = argparse.ArgumentParser(
        description="Freeze physical image coordinates from the source UserUnit before operations"
    )
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    base = root / "fixtures/existing-image-edit/forms"
    original = base / "criteria.json"
    expected = "a060299145df5301d76491f31b5306c2e767608702620a2f4d7b39d2f662d04b"
    assert hashlib.sha256(original.read_bytes()).hexdigest() == expected
    fixed = json.loads(original.read_text(encoding="utf-8"))
    source = base / "shared-forms.pdf"
    assert (
        hashlib.sha256(source.read_bytes()).hexdigest() == fixed["files"][source.name]
    )
    page = PdfReader(source).pages[0]
    assert (
        int(page.get("/Rotate", 0)) == 0
        and float(page.get("/UserUnit", 1)) == 2
        and page.cropbox == page.mediabox
    )
    x, y, w, h = fixed["first_pdf_bounds"]
    unit = float(page.get("/UserUnit", 1))
    physical = [
        (x - float(page.cropbox.left)) * unit,
        (float(page.cropbox.top) - y - h) * unit,
        w * unit,
        h * unit,
    ]
    fixed.update(
        first_physical=physical,
        target_physical=[v * unit for v in fixed["target_physical"]],
        page_user_unit=unit,
        page_media_box=list(page.mediabox),
        page_crop_box=list(page.cropbox),
        original_proposal_sha256=expected,
        scope="Separate positive geometry fixed from the original PDF raw bounds and pypdf page boxes before its first product operation. Original wrong unscaled physical coordinate proposal and failure retained; input, raw image bounds and tolerances unchanged.",
    )
    with args.output.open("x", encoding="utf-8") as out:
        json.dump(fixed, out, ensure_ascii=False, indent=2)
    print(
        json.dumps(
            dict(
                first_physical=physical,
                target_physical=fixed["target_physical"],
                sha256=hashlib.sha256(args.output.read_bytes()).hexdigest(),
            )
        )
    )


if __name__ == "__main__":
    main()
