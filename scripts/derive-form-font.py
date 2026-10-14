"""Derive the complete, editable AcroForm font from the pinned OFL source.

Development tool only. The application reads the generated font and mapping;
it never requires Python or fontTools. Existing mismatched files are not replaced.
"""

import argparse
import hashlib
import io
import json
from pathlib import Path


SOURCE_SHA256 = "c2f3b4d463500a2ddcd3849cded1fceeb9fd6d1c32e6cbecd568453ba50fc68f"
FONTTOOLS_VERSION = "4.60.1"
ROOT = Path(__file__).resolve().parents[1]


def derive(source):
    import fontTools
    from fontTools.ttLib import TTFont
    from fontTools.varLib.instancer import instantiateVariableFont

    original = source.read_bytes()
    if hashlib.sha256(original).hexdigest() != SOURCE_SHA256:
        raise ValueError("Review the font recipe before changing the source")
    if fontTools.__version__ != FONTTOOLS_VERSION:
        raise ValueError("Use the pinned fontTools version")
    font = TTFont(io.BytesIO(original), recalcTimestamp=False)
    instantiateVariableFont(font, {"wght": 400}, inplace=True)
    font.recalcTimestamp = False
    if font["OS/2"].fsType != 0:
        raise ValueError("Review editable embedding permissions")
    for entry in font["name"].names:
        if entry.nameID not in (1, 2, 3, 4, 6, 16, 17, 18, 21, 22):
            continue
        name = "Tatsujin Sans JP Regular"
        if entry.nameID == 6:
            name = "TatsujinSansJP-Regular"
        elif entry.nameID in (1, 16, 21):
            name = "Tatsujin Sans JP"
        elif entry.nameID in (2, 17, 22):
            name = "Regular"
        entry.string = name.encode(entry.getEncoding())
    output = io.BytesIO()
    font.save(output)
    data = output.getvalue()
    scale = 1000 / font["head"].unitsPerEm
    cmap = font.getBestCmap()
    if not cmap or len(cmap) >= 65535:
        raise ValueError("Invalid complete CID repertoire")
    mapping = {
        "version": 1,
        "source_sha256": SOURCE_SHA256,
        "font_sha256": hashlib.sha256(data).hexdigest(),
        "font_bytes": len(data),
        "family": "Tatsujin Sans JP",
        "postscript_name": "TatsujinSansJP-Regular",
        "fontTools": FONTTOOLS_VERSION,
        "weight": 400,
        "bbox": [
            getattr(font["head"], name) * scale
            for name in ("xMin", "yMin", "xMax", "yMax")
        ],
        "ascent": font["hhea"].ascent * scale,
        "descent": font["hhea"].descent * scale,
        "cap_height": font["OS/2"].sCapHeight * scale,
        "notdef_width": font["hmtx"].metrics[font.getGlyphOrder()[0]][0] * scale,
        # CID is the one-based row index. Different Unicode aliases keep distinct
        # CIDs even when they share a GID, preserving input and copied characters.
        "characters": [
            [
                code,
                font.getGlyphID(cmap[code]),
                font["hmtx"].metrics[cmap[code]][0] * scale,
            ]
            for code in sorted(cmap)
        ],
    }
    return data, (json.dumps(mapping, separators=(",", ":")) + "\n").encode("utf8")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, default=ROOT / "assets/fonts")
    args = parser.parse_args()
    font, mapping = derive(ROOT / "assets/fonts/NotoSansJP.ttf")
    outputs = {
        args.output / "TatsujinSansJP-Regular.ttf": font,
        args.output / "TatsujinSansJP-Regular.json": mapping,
    }
    for path, data in outputs.items():
        if path.exists() and (not path.is_file() or path.read_bytes() != data):
            raise ValueError(f"Refusing to replace modified asset: {path}")
    args.output.mkdir(parents=True, exist_ok=True)
    for path, data in outputs.items():
        if not path.exists():
            with path.open("xb") as file:
                file.write(data)
        print(
            json.dumps(
                {
                    "file": path.name,
                    "bytes": len(data),
                    "sha256": hashlib.sha256(data).hexdigest(),
                }
            )
        )


if __name__ == "__main__":
    main()
