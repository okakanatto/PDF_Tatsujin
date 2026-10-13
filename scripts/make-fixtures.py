"""Synthetic, public-data-free acceptance inputs. Ground truth is fixed before OCR.
Run once before evaluation. Do not change corpus/search terms in response to results.
"""

from pathlib import Path
import io, json, hashlib, textwrap, subprocess
from reportlab.pdfgen import canvas
from reportlab.pdfbase import pdfmetrics
from reportlab.pdfbase.ttfonts import TTFont
from reportlab.lib.utils import ImageReader
from pypdf import PdfReader, PdfWriter
from pypdf.generic import (
    NameObject,
    NumberObject,
    ArrayObject,
    FloatObject,
    DictionaryObject,
    TextStringObject,
    DecodedStreamObject,
)
from PIL import Image, ImageDraw
from fontTools.ttLib import TTFont as Font
from fontTools.varLib.instancer import instantiateVariableFont

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / "fixtures"
OUT.mkdir(exist_ok=True)
from test_tools import poppler_tool

POPPLER = poppler_tool("pdftoppm")
font = OUT / "NotoSansJP-fixture.ttf"
if not font.exists():
    f = Font(ROOT / "assets/fonts/NotoSansJP.ttf")
    instantiateVariableFont(f, {"wght": 400}, inplace=True)
    f.save(font)
pdfmetrics.registerFont(TTFont("NotoFixture", str(font)))

JP = [
    """地域の図書館では、来館者が安心して資料を探せるように案内を整えています。入口の地図には、新聞、雑誌、児童書、参考図書の場所を示しました。受付では貸出カードの作り方を説明し、返却期限を印刷した紙を渡します。予約した本が届いた場合は、事前に選んだ連絡方法でお知らせします。
読書室の机と椅子は毎朝点検します。照明の明るさを調整し、窓からの日差しが強い時間帯にはカーテンを閉めます。静かな席を希望する方と、グループで相談する方が同じ場所で困らないように、利用区域を分けています。わからないことがあれば係員に声をかけてください。
資料の保存には温度と湿度の管理が欠かせません。古い写真や地図は専用の箱に入れ、閲覧するときは汚れを防ぐ敷物を使います。破れや変色を見つけた場合は、その場所を記録して担当者へ報告します。修理中の資料には返却予定日を表示します。
新しい展示は毎月最初の火曜日に始まります。今月のテーマは町の交通と暮らしです。昔の駅舎、商店街、公園の写真を年代順に並べました。展示の説明文は大きな文字で印刷しています。来館者の感想は専用の箱で受け付け、次回の企画を考える参考にします。
閉館前には館内放送で時間を案内します。忘れ物がないかを確認し、借りた資料の冊数を確かめてからお帰りください。貸出中の資料は返却ポストでも受け付けます。ただし付属品のある資料は窓口へお持ちください。""",
    """工場の設備点検は、作業開始前の確認から始まります。担当者は手順書を読み、工具と保護具がそろっていることを確かめます。機械の周囲に障害物がないか、床が濡れていないかを確認します。異常を見つけた場合は運転を始めず、責任者へ報告してください。
点検表には設備番号、実施日時、担当者名、確認結果を記入します。測定値は単位を付けて記録し、前回の値と比較します。数値が許容範囲内でも、変化が急な場合は原因を調べます。記録を後から書き直すときは、変更した理由がわかるように残します。
部品の交換は指定された順序で進めます。取り外したねじや留め具は箱にまとめ、別の設備の部品と混ざらないようにします。新しい部品の型式を確認してから取り付けます。交換後は工具の置き忘れがないことを確認し、保護カバーを元の位置に戻します。
作業を引き継ぐ際には、完了した項目と未完了の項目を分けて説明します。特に注意が必要な箇所は、口頭だけでなく書面でも伝えます。判断に迷う場合は、経験だけで進めずに手順書と責任者の指示を確認します。安全のための停止をためらわないことが大切です。
月末には点検記録を集計します。故障の回数だけでなく、停止時間、交換部品、発見のきっかけも調べます。同じ異常が繰り返されている設備は、保全計画を見直します。改善した内容は次の担当者にも共有し、毎日の確認に反映します。""",
    """市民公園の管理事務所では、季節に合わせて花壇の植え替えを計画しています。春は入口付近に明るい色の花を植え、夏は木陰の休憩場所を整えます。秋には落ち葉を集め、冬には通路の凍結を確認します。訪れる人が歩きやすいように、段差や水たまりも点検します。
遊具の点検では、金属部分のひび、ねじの緩み、地面の状態を調べます。修理が必要な遊具は使用を停止し、理由を示した案内を取り付けます。修理が終わるまで立ち入れないように囲いを設けます。点検した日時と担当者は記録簿に残し、次の確認に役立てます。
池の周囲では水質と生き物の様子を観察します。えさの与えすぎは水を汚す原因になるため、決められたルールを掲示します。水鳥が休む場所には近づきすぎないように案内します。清掃の際は、ごみだけでなく危険な落とし物がないかも確認します。
休日の催しでは、会場の配置図を入口に掲示します。受付、休憩所、救護所の位置をわかりやすく示し、通路には物を置きません。雨天時の対応は開催前に決め、変更があれば早めに知らせます。片付けが終わった後は、忘れ物と設備の状態を確認します。
公園を長く気持ちよく使うために、利用者からの意見を集めています。ベンチの場所、案内板の読みやすさ、夜間の明るさなど、具体的な気づきをお寄せください。寄せられた意見は定期的に整理し、対応した内容と今後の予定を掲示します。""",
    """観測所では、毎日同じ時刻に気温、湿度、風向、降水量を記録します。測定器の設置場所には周囲の建物や樹木の影響があります。そのため、機器を移動する場合は位置と理由を記録し、以前の観測値と単純に比較しないように注意します。
雨量計の入口に落ち葉や虫が入ると、正しい値を測れないことがあります。担当者は定期的に内部を確認し、必要な清掃を行います。清掃した日時も観測記録に残します。強い雨や風が予想される日は、無理に屋外へ出ず、安全を確保してから点検します。
収集したデータは、まず欠けている時刻や異常な値がないかを確認します。異常が疑われる値はすぐに削除せず、周辺の観測点や機器の状態と照らし合わせます。補正を行う場合は、元の値と補正後の値を区別して保存します。判断の根拠も担当者が読める形で残します。
月ごとの報告書には、平均値だけでなく最高値と最低値も掲載します。観測できなかった日がある場合は、その日数と理由を明記します。表とグラフでは同じ単位を使い、比較する期間をそろえます。長期的な傾向と一日の変動を混同しない説明を心がけます。
観測の結果は防災や農業の計画にも利用されます。ただし、一つの地点の数値だけで広い地域全体の状態を決めることはできません。利用する目的と測定条件を確認し、必要に応じて複数の資料を組み合わせます。わからない点は推測で埋めず、未確認として示します。""",
]
EN = [
    """The community workshop opens at nine each morning. Visitors register at the front desk and receive a short explanation of the available equipment. Staff check that tools are clean, guards are secure, and emergency exits are clear. Each participant reads the instructions before beginning a project. Questions are welcome at every stage of the work.
Materials are stored by type and size. Wood belongs on the lower shelves, while small metal parts are kept in labeled drawers. A separate cabinet holds protective glasses and gloves. Borrowed tools must be returned to their marked places. This arrangement makes missing items easier to notice and helps the next visitor prepare without delay.
At the end of a session, participants remove waste from their benches and report any damage. Staff record repairs in a shared notebook. A broken tool stays out of service until a qualified person has inspected it. The monthly review considers both accidents and situations that almost caused an accident. The purpose is to improve the workspace for everyone.""",
    """The museum prepares a new exhibition every season. Curators begin by selecting objects that tell a clear story. They check the condition of each object and confirm that it can be displayed safely. Labels explain where the object came from, how it was used, and why it matters. Dates and measurements are checked against the collection records.
Lighting is adjusted before the public enters the gallery. Fragile paper needs lower light levels than stone or metal. Display cases must allow visitors to see important details without touching the objects. Staff walk through the room to check the route, the signs, and the space around each case. They also test the reading height of the labels.
After the exhibition closes, every object is examined again. Changes in condition are photographed and described. Packing materials are selected to protect the surfaces during transport. The final report records what worked well and what should change next time. Visitor comments help the team understand which explanations were clear and which needed more context.""",
    """The coastal research team surveys the same beach throughout the year. Members record the date, tide level, weather, and starting position before collecting samples. They follow a marked route so that observations from different visits can be compared. Equipment is checked before departure, and each container receives a unique label.
Samples are handled carefully to avoid mixing material from separate locations. The team uses clean instruments and records the depth of each collection. Photographs show the surrounding area as well as the sample point. If a location cannot be reached safely, the team records the reason and leaves the entry incomplete. Safety takes priority over finishing every planned measurement.
Back at the laboratory, staff compare the labels with the field notes. Unexpected results are checked against the original observations. The report presents uncertainty alongside the measurements and explains any gaps in the record. Repeated surveys reveal gradual changes that a single visit cannot show. The collected evidence supports careful decisions about the coastline.""",
    """The railway archive contains maps, timetables, photographs, and maintenance records. Researchers request materials using the reference numbers in the catalog. Staff retrieve the requested items and explain any handling restrictions. Large maps are supported on a clean table, and loose photographs remain in their protective sleeves.
Digital copies make frequently requested records easier to consult. Before scanning, staff check page order and note any missing sections. Images are reviewed for focus, alignment, and complete margins. Text recognition helps users search the collection, but the original image remains available for comparison. Uncertain words are not silently replaced with guesses.
Each completed record includes the source, date of capture, equipment settings, and review status. A second staff member checks a sample of the results. Files are stored in more than one location, and restoration tests confirm that the backups can be used. The archive publishes clear descriptions of its coverage so that researchers understand both the strengths and the limits of the collection.""",
]
TERMS_JP = [
    "市民公園",
    "管理事務所",
    "花壇",
    "植え替え",
    "落ち葉",
    "遊具",
    "記録簿",
    "水質",
    "水鳥",
    "救護所",
    "観測所",
    "気温",
    "湿度",
    "風向",
    "降水量",
    "雨量計",
    "観測点",
    "補正",
    "防災",
    "農業",
]
TERMS_EN = [
    "coastal",
    "surveys",
    "beach",
    "tide",
    "containers" if False else "container",
    "instruments",
    "laboratory",
    "uncertainty",
    "coastline",
    "departure",
    "railway",
    "archive",
    "timetables",
    "photographs",
    "catalog",
    "scanning",
    "recognition",
    "comparison",
    "restoration",
    "backups",
]


def digital(path, texts, size=(595.276, 841.89), invisible=False):
    c = canvas.Canvas(str(path), pagesize=size, pageCompression=1)
    truths = []
    for lang, text in texts:
        lines = []
        for para in text.splitlines():
            lines.extend(
                textwrap.wrap(para, 42 if lang == "jpn" else 83, break_long_words=False)
                if lang == "eng"
                else [para[i : i + 42] for i in range(0, len(para), 42)]
            )
        t = c.beginText(48, size[1] - 72)
        t.setFont("NotoFixture", 11)
        t.setLeading(18)
        if invisible:
            t.setTextRenderMode(3)
        for line in lines:
            t.textLine(line)
        c.drawText(t)
        c.showPage()
        truths.append("\n".join(lines))
    c.save()
    return truths


def scan(source, output):
    subprocess.run(
        [str(POPPLER), "-r", "300", "-png", str(source), str(OUT / "scan")],
        check=True,
        stdout=subprocess.DEVNULL,
        stderr=subprocess.PIPE,
    )
    c = canvas.Canvas(str(output), pagesize=(595.276, 841.89))
    imgs = sorted(OUT.glob("scan-*.png"))
    for img in imgs:
        c.drawImage(str(img), 0, 0, width=595.276, height=841.89)
        c.showPage()
    c.save()
    return imgs


digital(
    OUT / "D01.pdf",
    [
        (
            "jpn",
            "日本語とEnglishの文書です。番号12345、句読点。山田 太郎、髙橋。\nPDFの検索とコピー、署名の保存を確認します。",
        )
    ],
)
writer = PdfWriter()
for i, rot in enumerate([0, 90, 180, 270]):
    page = writer.add_page(PdfReader(OUT / "D01.pdf").pages[0])
    page[NameObject("/Rotate")] = NumberObject(rot)
    page[NameObject("/CropBox")] = ArrayObject(
        [FloatObject(x) for x in [20 + i * 5, 35, 570, 800 - i * 20]]
    )
    page[NameObject("/UserUnit")] = NumberObject(2 if i == 3 else 1)
    if i == 2:
        page[NameObject("/MediaBox")] = ArrayObject(
            [NumberObject(x) for x in [0, 0, 650, 850]]
        )
writer.write(OUT / "D02.pdf")
truths = digital(
    OUT / "D03-digital-source.pdf", [("jpn", s) for s in JP] + [("eng", s) for s in EN]
)
ground = {
    "normalization": "NFC; collapse whitespace only",
    "frozen_before_ocr": True,
    "settings": {"dpi": 300, "psm": 3, "model": "tessdata_best", "oem": 1},
    "search_terms": {"jpn": TERMS_JP, "eng": TERMS_EN},
    "pages": [
        {
            "page": i + 1,
            "language": "jpn" if i < 4 else "eng",
            "split": "tune" if i % 4 < 2 else "evaluation",
            "text": t,
            "font_pt": 11,
            "dpi": 300,
            "direction": "horizontal",
            "line_origin_pt": [48, 769.89],
            "leading_pt": 18,
        }
        for i, t in enumerate(truths)
    ],
}
(OUT / "ground-truth.json").write_text(
    json.dumps(ground, ensure_ascii=False, indent=2), encoding="utf-8"
)
for lang in ["jpn", "eng"]:
    assert sum(len(p["text"]) for p in ground["pages"] if p["language"] == lang) >= 2000
images = scan(OUT / "D03-digital-source.pdf", OUT / "D03.pdf")
writer = PdfWriter()
writer.append(OUT / "D01.pdf")
writer.append(OUT / "D03.pdf", pages=(0, 2))
writer.write(OUT / "D04.pdf")
buf = io.BytesIO()
c = canvas.Canvas(buf, pagesize=(595.276, 841.89))
c.setFont("Helvetica", 11)
c.drawString(48, 815, "DIGITAL HEADING 2026")
c.drawString(500, 25, "PAGE 1")
c.save()
writer = PdfWriter()
page = writer.add_page(PdfReader(OUT / "D03.pdf").pages[0])
page.merge_page(PdfReader(buf).pages[0])
comment = DictionaryObject(
    {
        NameObject("/Type"): NameObject("/Annot"),
        NameObject("/Subtype"): NameObject("/Text"),
        NameObject("/Rect"): ArrayObject(
            [NumberObject(x) for x in [550, 760, 570, 780]]
        ),
        NameObject("/Contents"): TextStringObject(
            "Existing note: retain this annotation"
        ),
        NameObject("/F"): NumberObject(4),
    }
)
page[NameObject("/Annots")] = ArrayObject([writer._add_object(comment)])
writer.write(OUT / "D05.pdf")
writer = PdfWriter()
page = writer.add_page(PdfReader(OUT / "D03.pdf").pages[0])
hidden = io.BytesIO()
digital(hidden, [("jpn", JP[0])], invisible=True) if False else None
hidden_path = OUT / "hidden-source.pdf"
digital(hidden_path, [("jpn", JP[0])], invisible=True)
page.merge_page(PdfReader(hidden_path).pages[0])
writer.add_blank_page(595.276, 841.89)
photo = Image.new("RGB", (600, 800))
draw = ImageDraw.Draw(photo)
for y in range(800):
    draw.line((0, y, 600, y), fill=(int(70 + y / 8), 140, int(200 - y / 10)))
draw.ellipse((90, 220, 500, 640), fill=(70, 110, 65))
buf = io.BytesIO()
c = canvas.Canvas(buf, pagesize=(595.276, 841.89))
c.drawImage(ImageReader(photo), 0, 0, 595.276, 841.89)
c.save()
writer.add_page(PdfReader(buf).pages[0])
writer.write(OUT / "D06.pdf")
c = canvas.Canvas(str(OUT / "D07.pdf"), pagesize=(595.276, 841.89))
c.setFont("Helvetica", 14)
c.drawString(40, 800, "Form preservation fixture")
a = c.acroForm
a.textfield(name="name", value="Yamada Taro", x=45, y=730, width=220, height=25)
a.textfield(
    name="notes",
    value="Line one\nLine two",
    x=45,
    y=645,
    width=220,
    height=65,
    fieldFlags="multiline",
)
a.checkbox(name="agree", x=45, y=600, checked=True)
a.radio(name="choice", value="A", x=45, y=550, selected=True)
a.radio(name="choice", value="B", x=85, y=550, selected=False)
a.choice(
    name="combo",
    options=["Red", "Blue"],
    value="Blue",
    x=45,
    y=500,
    width=200,
    height=22,
)
a.listbox(
    name="list",
    options=["North", "South", "West"],
    value="South",
    x=45,
    y=420,
    width=200,
    height=60,
)
c.textAnnotation("Existing comment retained", (350, 650, 370, 670))
c.linkURL("https://example.org/", (40, 350, 200, 375), relative=0)
c.bookmarkPage("first")
c.addOutlineEntry("First page", "first")
c.showPage()
c.save()
writer = PdfWriter(clone_from=OUT / "D07.pdf")
writer.get_fields()["name"]
field = writer._root_object["/AcroForm"]["/Fields"][0].get_object()
field[NameObject("/V")] = TextStringObject("山田 太郎")
ap_path = OUT / "form-appearance-source.pdf"
c = canvas.Canvas(str(ap_path), pagesize=(220, 25))
c.setFillColorRGB(0.8, 0.84, 1)
c.rect(0, 0, 220, 25, fill=1, stroke=1)
c.setFillColorRGB(0, 0, 0)
c.setFont("NotoFixture", 12)
c.drawString(4, 7, "山田 太郎")
c.showPage()
c.save()
ap = PdfReader(ap_path).pages[0]
stream = DecodedStreamObject()
stream.set_data(ap.get_contents().get_data())
stream[NameObject("/Type")] = NameObject("/XObject")
stream[NameObject("/Subtype")] = NameObject("/Form")
stream[NameObject("/BBox")] = ArrayObject([NumberObject(x) for x in [0, 0, 220, 25]])
stream[NameObject("/Resources")] = ap["/Resources"].clone(writer)
field[NameObject("/AP")] = DictionaryObject(
    {NameObject("/N"): writer._add_object(stream)}
)
writer.write(OUT / "D07.pdf")
writer = PdfWriter(clone_from=OUT / "D01.pdf")
writer.encrypt("correct-password", "owner-password", algorithm="AES-256")
writer.write(OUT / "D08-encrypted.pdf")
writer = PdfWriter(clone_from=OUT / "D01.pdf")
sig = DictionaryObject(
    {
        NameObject("/Type"): NameObject("/Sig"),
        NameObject("/ByteRange"): ArrayObject(
            [NumberObject(0), NumberObject(1), NumberObject(2), NumberObject(3)]
        ),
    }
)
writer._root_object[NameObject("/TestSignature")] = writer._add_object(sig)
writer.write(OUT / "D08-signature-structure.pdf")
writer = PdfWriter(clone_from=OUT / "D07.pdf")
writer._root_object["/AcroForm"][NameObject("/XFA")] = TextStringObject(
    "synthetic unsupported XFA marker"
)
writer.write(OUT / "D08-xfa.pdf")
(OUT / "D08-broken.pdf").write_bytes(b"%PDF-1.7\n1 0 obj << /Type /Catalog\n")
(OUT / "D09_日本語のパス").mkdir(exist_ok=True)
(OUT / "D09_日本語のパス/入力 文書.pdf").write_bytes((OUT / "D01.pdf").read_bytes())
for name, count, src in [
    ("D10-digital-100.pdf", 100, "D01.pdf"),
    ("D10-image-50.pdf", 50, "D03.pdf"),
]:
    writer = PdfWriter()
    p = PdfReader(OUT / src).pages[0]
    for i in range(count):
        writer.add_page(p)
    writer.write(OUT / name)
manifest = []
for path in sorted(OUT.rglob("*.pdf")):
    try:
        reader = PdfReader(path)
        n = None if reader.is_encrypted else len(reader.pages)
    except Exception:
        n = None
    manifest.append(
        {
            "id": path.stem.split("-")[0],
            "file": str(path.relative_to(OUT)),
            "sha256": hashlib.sha256(path.read_bytes()).hexdigest(),
            "pages": n,
            "origin": "scripts/make-fixtures.py; synthetic original corpus",
            "license": "CC0-1.0 synthetic text and graphics; embedded Noto font under OFL-1.1",
            "language": "jpn+eng",
            "dpi": 300 if path.name == "D03.pdf" else None,
            "direction": "horizontal",
            "ground_truth": "ground-truth.json" if path.name == "D03.pdf" else None,
            "purpose": path.stem,
        }
    )
(OUT / "manifest.json").write_text(
    json.dumps(manifest, ensure_ascii=False, indent=2), encoding="utf-8"
)
print(
    json.dumps(
        {
            "pdf_count": len(manifest),
            "japanese_characters": sum(map(len, JP)),
            "english_characters": sum(map(len, EN)),
            "ground_truth_sha256": hashlib.sha256(
                (OUT / "ground-truth.json").read_bytes()
            ).hexdigest(),
        }
    )
)
