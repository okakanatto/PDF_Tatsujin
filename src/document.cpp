#include "document.h"
#include "pdf_objects.h"
#include "pdfdocumentbuilder.h"
#include "pdfpainter.h"
#include "pdfsecurityhandler.h"
#include <windows.h>

namespace tatsu
{
using namespace pdf;
using namespace detail;
QVector<Signature> signatures(const PDFDocument& doc, int page)
{
    QVector<Signature> out;
    const auto& po = doc.getObjectByReference(doc.getCatalog()->getPage(page)->getPageReference());
    const auto& ann = doc.getObject(po.getDictionary()->get("Annots"));
    if (!ann.isArray())
        return out;
    for (const auto& ref : *ann.getArray())
    {
        auto o = doc.getObject(ref);
        if (!o.isDictionary())
            continue;
        auto j = o.getDictionary()->get("Tatsujin");
        if (!j.isString())
            continue;
        auto data = QJsonDocument::fromJson(j.getString()).object();
        if (data["version"].toInt() != 1)
            continue;
        auto a = doc.getObject(o.getDictionary()->get("Rect"));
        if (!a.isArray() || a.getArray()->getCount() != 4)
            continue;
        auto value = [&](int i)
        {
            auto v = a.getArray()->getItem(i);
            return v.isReal() ? v.getReal() : double(v.getInteger());
        };
        out.push_back(
            {ref.getReference(), QRectF(QPointF(value(0), value(1)), QPointF(value(2), value(3))),
             data["text"].toString(), data["size"].toDouble(), QColor(data["color"].toString())});
    }
    return out;
}
void Document::open(const QString& path, const QString& password)
{
    if (busy)
        fail("OCR処理中です。");
    auto doc = readPdf(path, password);
    QString restriction;
    if (doc.getStorage().getSecurityHandler()->getMode() != EncryptionMode::None)
        restriction = "暗号化されたPDF（読み取り専用）";
    for (const auto& entry : doc.getStorage().getObjects())
    {
        auto o = entry.object;
        if (!o.isDictionary())
            continue;
        auto d = o.getDictionary();
        auto ft = doc.getObject(d->get("FT"));
        auto type = doc.getObject(d->get("Type"));
        if ((ft.isName() && ft.getString() == "Sig" && !d->get("V").isNull()) ||
            (type.isName() && type.getString() == "Sig") || d->hasKey("ByteRange"))
            restriction = "証明書署名付きPDF（読み取り専用・証明書の有効性は未評価）";
        if (d->hasKey("XFA") || (d->hasKey("FT") && d->hasKey("AA")))
            restriction = "非対応フォーム／スクリプト付き文書（読み取り専用）";
    }
    copyAllowed = doc.getStorage().getSecurityHandler()->isAllowed(
        PDFSecurityHandler::Permission::CopyContent);
    history = {std::move(doc)};
    cursor = saved = 0;
    source = QFileInfo(path).absoluteFilePath();
    sourceHash = fileHash(source);
    target.clear();
    targetHash.clear();
    readOnly = restriction;
    ++revision;
}
void Document::editable() const
{
    if (!loaded())
        fail("PDFを開いてください。");
    if (busy)
        fail("OCR処理中は編集・保存を一時停止しています。");
    if (!readOnly.isEmpty())
        fail(readOnly);
}
void Document::commit(PDFDocument doc)
{
    editable();
    if (cursor + 1 < int(history.size()))
    {
        history.resize(cursor + 1);
        if (saved > cursor)
            saved = -1;
    }
    history.push_back(std::move(doc));
    ++cursor;
    ++revision;
}
void Document::undo()
{
    editable();
    if (cursor > 0)
    {
        --cursor;
        ++revision;
    }
}
void Document::redo()
{
    editable();
    if (cursor + 1 < int(history.size()))
    {
        ++cursor;
        ++revision;
    }
}
Signature Document::putSignature(int page, const QString& text, QPointF point, double size,
                                 QColor color, PDFObjectReference old)
{
    editable();
    if (text.trimmed().isEmpty() || text.size() > 2000 || size < 6 || size > 144)
        fail("署名は1～2000文字、サイズは6～144ptで指定してください。");
    const double unit = pdf().getCatalog()->getPage(page)->getUserUnit();
    const double fontScale = size / (100.0 * unit);
    QFont font(signatureFont());
    font.setPixelSize(100);
    font.setStyleStrategy(QFont::NoFontMerging);
    auto raw = QRawFont::fromFont(font);
    for (auto cp : text.toUcs4())
        if (cp != 10 && cp != 13 && !raw.supportsCharacter(cp))
            fail(QString("対応していない文字があります: U+%1").arg(cp, 4, 16, QChar('0')));
    QFontMetricsF metrics(font);
    auto lines = text.split('\n');
    double width = 0;
    for (auto& l : lines)
        width = std::max(width, metrics.horizontalAdvance(l));
    QSizeF dim(width * fontScale + 8 / unit,
               metrics.lineSpacing() * lines.size() * fontScale + 8 / unit);
    QRectF rect(point, dim);
    if (!pdf().getCatalog()->getPage(page)->getCropBox().contains(rect))
        fail("署名がページ範囲を超えます。位置かサイズを変更してください。");
    PDFDocumentBuilder b(&pdf());
    if (old.isValid())
        b.removeAnnotation(pdf().getCatalog()->getPage(page)->getPageReference(), old);
    PDFContentStreamBuilder sb(dim, PDFContentStreamBuilder::CoordinateSystem::Qt);
    auto painter = sb.begin();
    painter->setFont(font);
    painter->setPen(color);
    painter->translate(4 / unit, 4 / unit);
    painter->scale(fontScale, fontScale);
    for (int i = 0; i < lines.size(); ++i)
        painter->drawText(QPointF(0, metrics.ascent() + i * metrics.lineSpacing()), lines[i]);
    auto ap = sb.end(painter);
    ap.document = correctFontUnicode(ap.document, text, raw);
    auto resources = b.copyFrom({ap.resources}, ap.document.getStorage(), true).at(0);
    PDFDocumentBuilder apb(&ap.document);
    QByteArray commands;
    auto content = ap.document.getObject(ap.contents);
    if (content.isArray())
    {
        for (auto& o : *content.getArray())
            commands += apb.getDecodedStream(ap.document.getObject(o).getStream()) + '\n';
    }
    else
        commands = apb.getDecodedStream(content.getStream());
    PDFDictionary form;
    set(form, "Type", PDFObject::createName("XObject"));
    set(form, "Subtype", PDFObject::createName("Form"));
    set(form, "BBox", rectObject(QRectF(QPointF(), dim)));
    set(form, "Resources", resources);
    auto apRef = b.addObject(streamObject(form, commands));
    PDFDictionary appearance;
    set(appearance, "N", PDFObject::createReference(apRef));
    auto ref = b.createAnnotationStamp(pdf().getCatalog()->getPage(page)->getPageReference(), rect,
                                       Stamp::Approved, "PDF達人", "見た目の署名", text);
    auto d = *b.getObjectByReference(ref).getDictionary();
    set(d, "Rect", rectObject(rect));
    set(d, "AP", dictObject(appearance));
    set(d, "F", PDFObject::createInteger(4));
    QJsonObject meta{{"version", 1}, {"text", text}, {"size", size}, {"color", color.name()}};
    set(d, "Tatsujin", PDFObject::createString(QJsonDocument(meta).toJson(QJsonDocument::Compact)));
    set(d, "NM", PDFObject::createString(QUuid::createUuid().toByteArray()));
    b.setObject(ref, dictObject(d));
    commit(b.build());
    return {ref, rect, text, size, color};
}
void Document::moveSignature(int page, const Signature& sig, QPointF delta)
{
    editable();
    auto rect = sig.rect.translated(delta);
    if (!pdf().getCatalog()->getPage(page)->getCropBox().contains(rect))
        fail("署名をページ内に配置してください。");
    PDFDocumentBuilder b(&pdf());
    auto d = *b.getObjectByReference(sig.ref).getDictionary();
    set(d, "Rect", rectObject(rect));
    b.setObject(sig.ref, dictObject(d));
    commit(b.build());
}
void Document::eraseSignature(int page, const Signature& s)
{
    editable();
    PDFDocumentBuilder b(&pdf());
    b.removeAnnotation(pdf().getCatalog()->getPage(page)->getPageReference(), s.ref);
    commit(b.build());
}
void Document::rotate(int page)
{
    editable();
    auto p = pdf().getCatalog()->getPage(page);
    PDFDocumentBuilder b(&pdf());
    b.setPageRotation(p->getPageReference(), getPageRotationRotatedRight(p->getPageRotation()));
    commit(b.build());
}
void Document::save(const QString& path, const QByteArray& expected)
{
    editable();
    QString dest = QFileInfo(path).absoluteFilePath();
    QByteArray baseline = expected;
    if (sameFilePath(dest, target))
        baseline = targetHash;
    else if (sameFilePath(dest, source))
        baseline = sourceHash;
    if (fileHash(dest) != baseline)
        fail("保存先が外部で変更されたか、既に存在します。別の名前で保存してください。");
    QTemporaryDir temp(QFileInfo(dest).absolutePath() + "/.pdf-tatsujin-XXXXXX");
    if (!temp.isValid())
        fail("保存先に一時領域を作成できません。");
    QString tempPath = temp.filePath("candidate.pdf");
    writeCandidate(pdf(), tempPath);
    if (fileHash(dest) != baseline)
        fail("保存先の外部変更を検出しました。");
    BOOL ok = baseline.isEmpty() ? MoveFileExW((LPCWSTR)tempPath.utf16(), (LPCWSTR)dest.utf16(),
                                               MOVEFILE_WRITE_THROUGH)
                                 : ReplaceFileW((LPCWSTR)dest.utf16(), (LPCWSTR)tempPath.utf16(),
                                                nullptr, 0, nullptr, nullptr);
    if (!ok)
        fail(QString("保存の置換に失敗しました（Windowsエラー "
                     "%1）。元ファイルと未保存変更は保持しました。")
                 .arg(GetLastError()));
    target = dest;
    targetHash = fileHash(dest);
    if (sameFilePath(dest, source))
        sourceHash = targetHash;
    saved = cursor;
}
QVector<int> parsePages(QString s, int count)
{
    QVector<int> out;
    if (s.trimmed().isEmpty())
        fail("対象ページを指定してください。");
    for (auto part : s.split(','))
    {
        auto m = QRegularExpression("^\\s*(\\d+)(?:\\s*-\\s*(\\d+))?\\s*$").match(part);
        if (!m.hasMatch())
            fail("ページ範囲は 1,3-5 の形式です。");
        int a = m.captured(1).toInt(), b = m.captured(2).isEmpty() ? a : m.captured(2).toInt();
        if (a < 1 || b < a || b > count)
            fail("ページ範囲が不正です。");
        for (int i = a; i <= b; ++i)
        {
            if (out.contains(i - 1))
                fail("ページが重複しています。");
            out << i - 1;
        }
    }
    return out;
}
} // namespace tatsu
