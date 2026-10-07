#include "font_tests.h"
#include "pdf_objects.h"
#include "pdfdocumentbuilder.h"
#include "text_font.h"
#include "window.h"
#include <QtEndian>
#include <QtTest/QTest>

namespace tatsu
{
namespace
{
void check(bool value, const QString& message)
{
    if (!value)
        fail(message);
}
void ready(Window& window)
{
    check(QTest::qWaitFor([&] { return window.canvas->pageReady(0); }, 10000),
          "font test PDF ready");
    QTest::qWait(70);
}
Signature item(const Document& document)
{
    const auto items = signatures(document.pdf(), 0);
    check(items.size() == 1, "one editable text overlay");
    return items[0];
}
void replaceFamilyMetadata(Document& document, const QString& family)
{
    const auto ref = item(document).ref;
    PDFDocumentBuilder builder(&document.pdf());
    auto dictionary = *builder.getObjectByReference(ref).getDictionary();
    auto meta = QJsonDocument::fromJson(dictionary.get("Tatsujin").getString()).object();
    if (family.isEmpty())
        meta.remove("fontFamily");
    else
        meta["fontFamily"] = family;
    detail::set(dictionary, "Tatsujin",
                PDFObject::createString(QJsonDocument(meta).toJson(QJsonDocument::Compact)));
    builder.setObject(ref, detail::dictObject(dictionary));
    document.commit(builder.build());
}
} // namespace
QJsonObject testFontRoundtrip(const QString& fixtures, const QString& output)
{
    const auto families = textFontFamilies();
    check(families.contains(signatureFont()), "bundled fallback always available");
    if (QFileInfo::exists(qEnvironmentVariable("SystemRoot", "C:/Windows") + "/Fonts/meiryo.ttc"))
        check(families.contains("Meiryo UI"), "installed Meiryo UI is offered");
    QJsonArray rows;
    int index = 0;
    for (const auto& family : families)
    {
        Document document;
        document.open(fixtures + "/D01.pdf");
        const auto originalHash = fileHash(document.source);
        const auto body = pageText(document.pdf(), 0);
        const bool latin = family == "Arial" || family == "Times New Roman";
        const auto text = latin ? QString("PDF text ABC 123\nEditable font")
                                : QString("山田 太郎 髙橋\n申込書への追記 ABC 123");
        auto signature =
            document.putText(0, OverlayKind::Text, text, {55, 130}, 18, Qt::black, {}, family);
        const auto stem = QString("/font-%1").arg(index++);
        const auto path = output + stem + ".pdf";
        auto before = renderPage(document.pdf(), 0, 1.3);
        document.save(path);
        Document reopened;
        reopened.open(path);
        check(item(reopened).fontFamily == family && item(reopened).text == text,
              "family and multiline text survive PDF reopen");
        check(renderPage(reopened.pdf(), 0, 1.3) == before, "appearance exactly preserved");
        check(pageText(reopened.pdf(), 0) == body && fileHash(document.source) == originalHash,
              "existing body and input unchanged");
        check(before.save(output + stem + ".png"), "font visual evidence");
        const auto resources = reopened.pdf().getObject(
            reopened.pdf().getObjectByReference(item(reopened).ref).getDictionary()->get("AP"));
        check(resources.isDictionary(), "standard appearance dictionary present");
        const auto editedText = text + " !";
        reopened.putText(0, OverlayKind::Text, editedText, signature.rect.topLeft(), 18, Qt::black,
                         item(reopened).ref);
        check(item(reopened).fontFamily == family, "edit retains chosen family by default");
        reopened.undo();
        check(renderPage(reopened.pdf(), 0, 1.3) == before, "Undo restores exact appearance");
        reopened.redo();
        reopened.save(output + stem + "-edited.pdf");
        rows.append(QJsonObject{
            {"family", family},
            {"file", QFileInfo(path).fileName()},
            {"text", text},
            {"edited_text", editedText},
            {"fsType",
             int(qFromBigEndian<quint16>(
                 QRawFont::fromFont(textFont(family)).fontTable("OS/2").constData() + 8))}});
    }
    return {{"families", rows},
            {"Meiryo_UI_available", families.contains("Meiryo UI")},
            {"saved_appearance_exact", true},
            {"reedit_and_undo", true}};
}
QJsonObject testFontFailures(const QString& fixtures, const QString& output)
{
    auto permission = [](quint16 flags)
    {
        QByteArray os2(10, 0);
        qToBigEndian(flags, os2.data() + 8);
        return allowsEditableFontEmbedding(os2);
    };
    check(permission(0) && permission(8), "installable and editable embedding supported");
    check(!permission(2) && !permission(4) && !permission(0x108) && !permission(0x208) &&
              !allowsEditableFontEmbedding({}),
          "restricted, preview-only, no-subsetting, bitmap-only, unknown fonts rejected");
    Document document;
    document.open(fixtures + "/D01.pdf");
    const auto sourceHash = fileHash(document.source);
    document.putText(0, OverlayKind::Text, "以前の版の文字", {70, 140}, 18, Qt::black);
    const auto appearance = renderPage(document.pdf(), 0, 1.1);
    replaceFamilyMetadata(document, {});
    document.save(output + "/font-legacy.pdf");
    document.open(output + "/font-legacy.pdf");
    check(item(document).fontFamily == signatureFont(), "old PDFs retain Noto Sans JP");
    replaceFamilyMetadata(document, "Unavailable-Tatsujin-Test-Font");
    document.save(output + "/font-unavailable.pdf");
    document.open(output + "/font-unavailable.pdf");
    check(renderPage(document.pdf(), 0, 1.1) == appearance,
          "saved appearance works even with unavailable source font");
    const auto revision = document.revision;
    const auto snapshot = encodePdf(document.pdf());
    bool missing = false;
    try
    {
        document.putText(0, OverlayKind::Text, "変更", {70, 140}, 18, Qt::black,
                         item(document).ref);
    }
    catch (const std::exception& error)
    {
        missing = QString::fromUtf8(error.what()).contains("別の書体");
    }
    check(missing && document.revision == revision && !document.dirty() &&
              encodePdf(document.pdf()) == snapshot,
          "missing font refuses silent substitution without partial change");
    bool unsupported = false;
    try
    {
        document.putText(0, OverlayKind::Text, QString::fromUcs4(U"\U0010ffff"), {70, 140}, 18,
                         Qt::black, item(document).ref, signatureFont());
    }
    catch (const std::exception&)
    {
        unsupported = true;
    }
    check(unsupported && document.revision == revision && encodePdf(document.pdf()) == snapshot,
          "unsupported glyph refuses without replacing the existing item");
    document.putText(0, OverlayKind::Text, "書体を選び直して再編集", {70, 140}, 18, Qt::black,
                     item(document).ref, signatureFont());
    document.save(output + "/font-fallback.pdf");
    check(fileHash(fixtures + "/D01.pdf") == sourceHash, "font failures preserve original");
    SignatureTemplate signature;
    signature.name = "書体付き署名";
    signature.text = "山田 太郎";
    signature.fontFamily = textFontFamilies().value(0);
    SignatureLibrary library(output + "/font-templates.json");
    library.add(signature);
    check(library.load()[0].fontFamily == signature.fontFamily, "template stores font choice");
    TextFontPicker picker;
    picker.setFamily("Unavailable-Tatsujin-Test-Font");
    check(picker.family() == "Unavailable-Tatsujin-Test-Font" &&
              picker.currentText().contains("利用できません"),
          "UI shows missing family without changing it");
    return {{"embedding_permissions", true},
            {"legacy_pdf", true},
            {"missing_font_preserves_state", true},
            {"unsupported_glyph_preserves_state", true},
            {"explicit_fallback", true},
            {"template_family", true}};
}
QJsonObject testFontUi(const QString& fixtures, const QString& output)
{
    Window window;
    window.resize(1024, 720);
    window.show();
    window.openFile(fixtures + "/D01.pdf");
    window.findChild<QAction*>("writingAction")->trigger();
    auto panel = static_cast<WritingPanel*>(window.findChild<QWidget*>("writingPanel"));
    auto picker = static_cast<TextFontPicker*>(panel->findChild<QComboBox*>("writingFont"));
    const auto family =
        textFontFamilies().contains("Meiryo UI") ? QString("Meiryo UI") : signatureFont();
    picker->setFamily(family);
    panel->findChild<QPlainTextEdit*>("writingText")->setPlainText("山田 太郎\n書体を選んで入力");
    window.canvas->fitPage();
    ready(window);
    panel->findChild<QPushButton*>("placeWriting")->click();
    check(window.canvas->viewport()->rect().contains(
              window.canvas->pdfToViewport(0, {80, 240}).toPoint()),
          "placement pointer is inside the visible viewport");
    QTest::mouseClick(window.canvas->viewport(), Qt::LeftButton, Qt::NoModifier,
                      window.canvas->pdfToViewport(0, {80, 240}).toPoint());
    check(item(window.doc).fontFamily == family, "UI selected family used by placed PDF text");
    window.doc.save(output + "/font-ui.pdf");
    window.openFile(output + "/font-ui.pdf");
    window.canvas->fitPage();
    ready(window);
    const auto center = item(window.doc).rect.center();
    QTest::mouseClick(window.canvas->viewport(), Qt::LeftButton, Qt::NoModifier,
                      window.canvas->pdfToViewport(0, center).toPoint());
    check(picker->family() == family, "reopened selection restores font picker");
    check(window.grab().save(output + "/font-meiryo-ui-1024.png"), "selected face UI evidence");
    picker->setFamily(signatureFont());
    panel->findChild<QPushButton*>("updateWriting")->click();
    check(item(window.doc).fontFamily == signatureFont(), "UI changes existing text family");
    window.undoAction->trigger();
    check(item(window.doc).fontFamily == family, "UI Undo restores original family");
    window.redoAction->trigger();
    check(item(window.doc).fontFamily == signatureFont(), "UI Redo restores changed family");
    window.doc.save(output + "/font-ui-edited.pdf");
    ready(window);
    check(window.grab().save(output + "/font-ui-1024.png"), "font UI screenshot saved");
    for (auto widget : panel->findChildren<QWidget*>())
        if (widget->isVisible() &&
            (qobject_cast<QPushButton*>(widget) || qobject_cast<QComboBox*>(widget) ||
             qobject_cast<QAbstractSpinBox*>(widget)))
        {
            check(window.rect().contains(QRect(widget->mapTo(&window, QPoint()), widget->size())),
                  "font controls fit minimum window");
            check(widget->visibleRegion().contains(widget->rect()),
                  "font controls are not clipped by their panel");
        }
    window.signatureAction->trigger();
    window.signatureFontPicker->setFamily(family);
    window.signature->setPlainText("山田 太郎");
    window.canvas->beginPlacement();
    QTest::mouseClick(window.canvas->viewport(), Qt::LeftButton, Qt::NoModifier,
                      window.canvas->pdfToViewport(0, {80, 390}).toPoint());
    auto items = signatures(window.doc.pdf(), 0);
    check(items.size() == 2 && items[1].fontFamily == family,
          "signature picker uses the same PDF font pipeline");
    window.doc.save(output + "/font-ui-with-signature.pdf");
    return {{"family", family},
            {"actual_pointer_placement", true},
            {"reopen_family_change_undo_redo", true},
            {"signature_picker", true},
            {"minimum_window", true},
            {"native_IME", "未実行"}};
}
} // namespace tatsu
