"""Derive fixed standard PDF font widths; no font outlines or runtime Python."""

import argparse
import hashlib
import importlib
from importlib.metadata import distribution
import json
from pathlib import Path
import subprocess

import reportlab
from reportlab.pdfbase import _fontdata

ROOT = Path(__file__).resolve().parents[1]
ENCODINGS = (
    "StandardEncoding",
    "WinAnsiEncoding",
    "MacRomanEncoding",
    "SymbolEncoding",
    "ZapfDingbatsEncoding",
)


def sha(data):
    return hashlib.sha256(data).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--clang-format", required=True, type=Path)
    parser.add_argument("--verify", action="store_true")
    args = parser.parse_args()
    if reportlab.Version != "4.4.9":
        raise RuntimeError("Use the pinned ReportLab 4.4.9 development dependency")
    package = distribution("reportlab")
    license_source = next(
        p for p in package.files if str(p).endswith("licenses/LICENSE")
    )
    notice = Path(package.locate_file(license_source)).read_bytes()
    lines = [
        "// SPDX-License-Identifier: BSD-3-Clause",
        "// Generated metric data from ReportLab 4.4.9. See scripts/standard-font-metrics-source.json.",
    ]
    lines += ["// " + line for line in notice.decode("utf-8").splitlines()]
    lines += [
        "#pragma once",
        "#include <QByteArray>",
        "#include <array>",
        "namespace tatsu::detail {",
        "struct StandardFontWidthTable { const char* font; const char* encoding; std::array<short, 256> widths; };",
        "inline constexpr StandardFontWidthTable standardFontWidths[] = {",
    ]
    for font in _fontdata.standardFonts:
        for encoding in ENCODINGS:
            widths = [
                _fontdata.widthsByFontGlyph[font].get(glyph, -1)
                for glyph in _fontdata.encodings[encoding]
            ]
            assert len(widths) == 256 and all(-1 <= value <= 32767 for value in widths)
            lines.append(
                '{"'
                + font
                + '","'
                + encoding
                + '",{{'
                + ",".join(str(value) for value in widths)
                + "}}},"
            )
    lines += [
        "};",
        "inline int standardFontGlyphWidth(const QByteArray& font, const QByteArray& encoding, unsigned int code) {",
        "if (code > 255) return -1;",
        "for (const auto& table : standardFontWidths) if (font == table.font && encoding == table.encoding) return table.widths[code];",
        "return -1;",
        "}",
        "} // namespace tatsu::detail",
        "",
    ]
    header = subprocess.check_output(
        [args.clang_format.resolve(), "--style=file"],
        input="\n".join(lines).encode("utf-8"),
        cwd=ROOT,
    )
    sources = {}
    for name in (
        "_fontdata",
        *[
            "_fontdata_widths_" + font.lower().replace("-", "")
            for font in _fontdata.standardFonts
        ],
        *[
            "_fontdata_enc_" + encoding.lower().removesuffix("encoding")
            for encoding in ENCODINGS
        ],
    ):
        module = importlib.import_module("reportlab.pdfbase." + name)
        sources[name + ".py"] = sha(Path(module.__file__).read_bytes())
    manifest = dict(
        reportlab_version=reportlab.Version,
        license="BSD-3-Clause",
        origin="https://docs.reportlab.com/developerfaqs/",
        source_files_sha256=sources,
        generated_header="src/standard_font_metrics.h",
        generated_header_sha256_LF=sha(header.replace(b"\r\n", b"\n")),
        license_file="licenses/REPORTLAB_METRICS_LICENSE.txt",
        license_sha256=sha(notice),
        font_outlines_included=False,
        runtime_python_required=False,
    )
    outputs = {
        ROOT / manifest["generated_header"]: header,
        ROOT / manifest["license_file"]: notice,
        ROOT
        / "scripts/standard-font-metrics-source.json": (
            json.dumps(manifest, indent=2) + "\n"
        ).encode("utf-8"),
    }
    for path, data in outputs.items():
        if args.verify:
            if not path.is_file() or path.read_bytes().replace(
                b"\r\n", b"\n"
            ) != data.replace(b"\r\n", b"\n"):
                raise RuntimeError("Generated metric evidence differs: " + str(path))
        else:
            if path.exists():
                raise FileExistsError("Preserve existing generated data: " + str(path))
            path.write_bytes(data)
    print(
        json.dumps(
            dict(
                fonts=14,
                encodings=len(ENCODINGS),
                source_version=reportlab.Version,
                generated_sha256=manifest["generated_header_sha256_LF"],
            )
        )
    )


if __name__ == "__main__":
    main()
