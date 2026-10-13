"""Freeze synthetic navigation targets before application tests. Never overwrite."""

import hashlib
import io
import json
from pathlib import Path

from pypdf import PdfReader, PdfWriter
from pypdf.constants import UserAccessPermissions
from pypdf.generic import (
    ArrayObject,
    DictionaryObject,
    Fit,
    FloatObject,
    NameObject,
    NullObject,
    NumberObject,
    RectangleObject,
    TextStringObject,
)
from reportlab.pdfbase import pdfmetrics
from reportlab.pdfbase.ttfonts import TTFont
from reportlab.pdfgen.canvas import Canvas

ROOT = Path(__file__).resolve().parents[1]
target = ROOT / "fixtures/viewer-navigation.pdf"
restricted = ROOT / "fixtures/viewer-navigation-restricted.pdf"
manifest = ROOT / "fixtures/viewer-navigation-manifest.json"
assert not any(
    p.exists() for p in (target, restricted, manifest)
), "Preserve frozen input"
pdfmetrics.registerFont(
    TTFont("Fixture", str(ROOT / "fixtures/NotoSansJP-fixture.ttf"))
)
buffer = io.BytesIO()
canvas = Canvas(buffer, pagesize=(600, 800), invariant=1, pageCompression=1)
for page in range(6):
    canvas.setFont("Fixture", 18)
    canvas.drawString(90, 720, f"文書内の参照 — {page + 1}")
    canvas.drawString(90, 650, "交通費を申請します。 English navigation.")
    canvas.setStrokeColorRGB(0.15, 0.4, 0.7)
    canvas.rect(180, 250, 240, 300)
    canvas.line(280, 400, 320, 400)
    canvas.line(300, 380, 300, 420)
    canvas.setFont("Fixture", 12)
    canvas.drawString(90, 580, "LINK: 3ページへ移動")
    canvas.drawString(90, 540, "EXTERNAL: example.invalid")
    canvas.drawString(90, 500, "SCRIPT: 実行しない")
    canvas.showPage()
canvas.save()
writer = PdfWriter()
for index, page in enumerate(PdfReader(buffer).pages):
    writer.add_page(page)
    writer.pages[-1][NameObject("/CropBox")] = RectangleObject([20, 30, 580, 770])
    writer.pages[-1][NameObject("/Rotate")] = NumberObject((index % 4) * 90)
    if index == 5:
        writer.pages[-1][NameObject("/UserUnit")] = FloatObject(2)


def destination(page, style, *values):
    return ArrayObject(
        [writer.pages[page].indirect_reference, NameObject(style)]
        + [NullObject() if value is None else FloatObject(value) for value in values]
    )


named = destination(2, "/XYZ", 300, 400, 3)
writer._root_object[NameObject("/Names")] = DictionaryObject(
    {
        NameObject("/Dests"): DictionaryObject(
            {
                NameObject("/Names"): ArrayObject(
                    [
                        TextStringObject("cycle-a"),
                        DictionaryObject(
                            {NameObject("/D"): TextStringObject("cycle-b")}
                        ),
                        TextStringObject("cycle-b"),
                        DictionaryObject(
                            {NameObject("/D"): TextStringObject("cycle-a")}
                        ),
                        TextStringObject("named-target"),
                        DictionaryObject({NameObject("/D"): named}),
                    ]
                )
            }
        )
    }
)
writer._root_object[NameObject("/PageLabels")] = DictionaryObject(
    {
        NameObject("/Nums"): ArrayObject(
            [
                NumberObject(0),
                DictionaryObject({NameObject("/S"): NameObject("/r")}),
                NumberObject(2),
                DictionaryObject(
                    {
                        NameObject("/S"): NameObject("/D"),
                        NameObject("/P"): TextStringObject("本編-"),
                        NameObject("/St"): NumberObject(7),
                    }
                ),
                NumberObject(4),
                DictionaryObject(
                    {
                        NameObject("/S"): NameObject("/A"),
                        NameObject("/St"): NumberObject(27),
                    }
                ),
            ]
        )
    }
)
parent = writer.add_outline_item("回転・倍率の目次", None)
outline = []


def bookmark(title, page, dest=None, parent_item=None):
    item = writer.add_outline_item(
        title, page, parent=parent_item, fit=Fit.xyz(300, 400, 3)
    )
    if dest is not None:
        item.get_object()[NameObject("/A")] = DictionaryObject(
            {NameObject("/S"): NameObject("/GoTo"), NameObject("/D"): dest}
        )
    outline.append({"title": title, "page": None if page is None else page + 1})
    return item


for page in range(4):
    bookmark(f"回転{page * 90}°：中心へ", page, parent_item=parent)
bookmark(
    "名前付き宛先：日本語の長いしおりタイトルを省略せずツールチップで確認する",
    2,
    TextStringObject("named-target"),
)
bookmark("全体 Fit", 4, destination(4, "/Fit"))
bookmark("幅 FitH", 4, destination(4, "/FitH", 600))
bookmark("高さ FitV", 4, destination(4, "/FitV", 200))
bookmark("矩形 FitR", 4, destination(4, "/FitR", 180, 250, 420, 550))
bookmark("null XYZ：位置と倍率を保持", 4, destination(4, "/XYZ", None, None, None))
bookmark("UserUnit 2", 5)
bookmark("存在しない名前", 0, TextStringObject("missing-target"))
bookmark("循環した名前", 0, TextStringObject("cycle-a"))
non_page = writer._add_object(
    DictionaryObject({NameObject("/Synthetic"): TextStringObject("not a page")})
)
bookmark("無効なページ参照", 0, ArrayObject([non_page, NameObject("/Fit")]))
bookmark("BBox 未対応", 0, destination(0, "/FitB"))
chained = bookmark("複合アクション 未対応", 1)
chained.get_object()["/A"][NameObject("/Next")] = DictionaryObject(
    {
        NameObject("/S"): NameObject("/URI"),
        NameObject("/URI"): TextStringObject("https://example.invalid/chained"),
    }
)


def link(rect, action=None, dest=None, flags=0, quad=None):
    annotation = DictionaryObject(
        {
            NameObject("/Type"): NameObject("/Annot"),
            NameObject("/Subtype"): NameObject("/Link"),
            NameObject("/Rect"): RectangleObject(rect),
            NameObject("/Border"): ArrayObject(
                [NumberObject(0), NumberObject(0), NumberObject(1)]
            ),
            NameObject("/F"): NumberObject(flags),
        }
    )
    if action is not None:
        annotation[NameObject("/A")] = action
    if dest is not None:
        annotation[NameObject("/Dest")] = dest
    if quad is not None:
        annotation[NameObject("/QuadPoints")] = ArrayObject(
            [FloatObject(v) for v in quad]
        )
    annotation[NameObject("/P")] = writer.pages[0].indirect_reference
    if "/Annots" not in writer.pages[0]:
        writer.pages[0][NameObject("/Annots")] = ArrayObject()
    writer.pages[0]["/Annots"].append(writer._add_object(annotation))


link([85, 570, 300, 600], dest=TextStringObject("named-target"))
link(
    [85, 530, 360, 560],
    action=DictionaryObject(
        {
            NameObject("/S"): NameObject("/URI"),
            NameObject("/URI"): TextStringObject(
                "https://example.invalid/document?x=<text>"
            ),
        }
    ),
)
link(
    [85, 490, 330, 520],
    action=DictionaryObject(
        {
            NameObject("/S"): NameObject("/JavaScript"),
            NameObject("/JS"): TextStringObject("app.alert('synthetic-never-execute')"),
        }
    ),
)
link([85, 450, 300, 480], dest=destination(1, "/Fit"), flags=2)
link(
    [85, 610, 350, 640],
    dest=destination(1, "/Fit"),
    quad=[85, 630, 200, 630, 85, 615, 200, 615],
)
writer.write(target)
protected = PdfWriter(clone_from=target)
protected.encrypt(
    "navigation-user",
    "navigation-owner",
    permissions_flag=UserAccessPermissions.PRINT
    | UserAccessPermissions.PRINT_TO_REPRESENTATION,
    algorithm="AES-256",
)
protected.write(restricted)
manifest.write_text(
    json.dumps(
        {
            "file": target.name,
            "sha256": hashlib.sha256(target.read_bytes()).hexdigest(),
            "restricted_file": restricted.name,
            "restricted_sha256": hashlib.sha256(restricted.read_bytes()).hexdigest(),
            "origin": "Synthetic MIT text/graphics; embedded Noto Sans JP under OFL. Created before app tests.",
            "expected": {
                "pages": 6,
                "page_labels": ["i", "ii", "本編-7", "本編-8", "AA", "BB"],
                "outlines": outline,
                "root_count": 13,
                "total_count": 17,
                "XYZ": {"point": [300, 400], "zoom": 3, "max_axis_error_DIP": 2},
                "history_max_axis_error_DIP": 2,
                "FitR": {
                    "page": 5,
                    "rectangle": [180, 250, 420, 550],
                    "fully_visible": True,
                },
                "links": {
                    "internal": [85, 570, 300, 600],
                    "uri": [85, 530, 360, 560],
                    "script": [85, 490, 330, 520],
                    "hidden": [85, 450, 300, 480],
                    "quad": [85, 610, 350, 640],
                },
                "invalid_no_navigation": [
                    "存在しない名前",
                    "循環した名前",
                    "無効なページ参照",
                    "BBox 未対応",
                    "複合アクション 未対応",
                ],
                "text_selection_contains": "LINK:",
                "restricted_password": "navigation-user",
            },
        },
        ensure_ascii=False,
        indent=2,
    )
    + "\n",
    encoding="utf-8",
)
print(
    "Frozen navigation PDFs and expectations:",
    hashlib.sha256(manifest.read_bytes()).hexdigest(),
)
