#include "writing_tests.h"
#include "window.h"
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
QImage sampleImage()
{
    QImage image(240, 80, QImage::Format_ARGB32);
    image.fill(Qt::transparent);
    QPainter painter(&image);
    painter.fillRect(0, 0, 60, 80, QColor("#185fc3"));
    painter.fillRect(150, 10, 60, 60, QColor("#cc2020"));
    painter.fillRect(65, 0, 30, 20, QColor(30, 120, 80, 128));
    painter.setPen(Qt::black);
    painter.drawLine(65, 65, 140, 20);
    return image;
}
Signature byKind(const Document& document, OverlayKind kind, int page = 0)
{
    for (const auto& item : signatures(document.pdf(), page))
        if (item.kind == kind)
            return item;
    fail("expected overlay kind missing");
}
void ready(Window& window)
{
    check(QTest::qWaitFor([&] { return window.canvas->pageReady(window.canvas->page); }, 10000),
          "real PDF ready");
    QTest::qWait(70);
}
} // namespace
QJsonObject testWritingRoundtrip(const QString& fixtures, const QString& output)
{
    Document document;
    document.open(fixtures + "/D01.pdf");
    const auto originalHash = fileHash(document.source);
    const auto originalText = pageText(document.pdf(), 0);
    auto text =
        document.putText(0, OverlayKind::Text, "申込書への追記\n髙橋", {60, 100}, 16, Qt::black);
    document.putText(0, OverlayKind::Date, "2026年10月4日", {60, 160}, 16, Qt::black);
    auto image = document.putImage(0, OverlayKind::Image, sampleImage(), {60, 240}, 144);
    document.putImage(0, OverlayKind::SignatureImage, sampleImage(), {60, 330}, 120);
    check(signatures(document.pdf(), 0).size() == 4, "four editable overlays");
    document.moveSignature(0, image, {20, 10});
    const auto moved = byKind(document, OverlayKind::Image);
    check(moved.rect.topLeft() == image.rect.topLeft() + QPointF(20, 10),
          "move in PDF coordinates");
    document.undo();
    check(byKind(document, OverlayKind::Image).rect == image.rect, "one move Undo");
    document.redo();
    document.save(output + "/writing.pdf");
    check(!document.dirty() && fileHash(document.source) == originalHash, "save protects source");
    auto visual = renderPage(document.pdf(), 0, 1.4);
    check(visual.save(output + "/writing.png"), "writing render saved");
    Document reopened;
    reopened.open(output + "/writing.pdf");
    check(renderPage(reopened.pdf(), 0, 1.4) == visual, "saved appearance exactly matches");
    check(pageText(reopened.pdf(), 0) == originalText, "existing body retained");
    check(byKind(reopened, OverlayKind::Date).text == "2026年10月4日",
          "date never refreshed on open");
    const auto reimage = byKind(reopened, OverlayKind::Image);
    const auto decoded = overlayImage(reopened.pdf(), reimage);
    check(decoded.size() == sampleImage().size(),
          "original image dimensions available without sidecar");
    check(decoded.pixelColor(100, 0).alpha() == 0, "transparent PNG remains transparent");
    check(decoded.pixelColor(70, 5).alpha() == 128, "semitransparent PNG retains 8-bit alpha");
    check(decoded.pixelColor(20, 20).blue() > 150 && decoded.pixelColor(170, 20).red() > 150,
          "image colors retained");
    reopened.resizeImage(0, reimage, 90);
    auto resized = byKind(reopened, OverlayKind::Image);
    check(qAbs(resized.rect.width() / resized.rect.height() - 3) < 1e-10,
          "resize preserves aspect ratio");
    check(qAbs(resized.rect.width() - 90) < 1e-10, "requested width applied");
    text = byKind(reopened, OverlayKind::Text);
    reopened.putText(0, OverlayKind::Text, "追記を修正", text.rect.topLeft(), 18, Qt::blue,
                     text.ref);
    check(byKind(reopened, OverlayKind::Date).text == "2026年10月4日",
          "unrelated date retained during reedit");
    reopened.eraseSignature(0, byKind(reopened, OverlayKind::SignatureImage));
    reopened.undo();
    check(signatures(reopened.pdf(), 0).size() == 4, "delete Undo restores image signature");
    reopened.save(output + "/writing-reedited.pdf");
    const auto jpegPath = output + "/writing-source.jpg";
    check(sampleImage().convertToFormat(QImage::Format_RGB32).save(jpegPath, "JPEG", 90),
          "actual JPEG input written");
    const QImage jpeg(jpegPath);
    check(!jpeg.isNull(), "actual JPEG decoded");
    reopened.putImage(0, OverlayKind::Image, jpeg, {260, 240}, 120);
    reopened.save(output + "/writing-jpeg.pdf");
    Document jpegReopened;
    jpegReopened.open(output + "/writing-jpeg.pdf");
    check(overlayImage(jpegReopened.pdf(), signatures(jpegReopened.pdf(), 0).back())
                  .convertToFormat(QImage::Format_RGB32) ==
              jpeg.convertToFormat(QImage::Format_RGB32),
          "JPEG decoded pixels retained without recompression");
    return {{"types", 4},
            {"original_unchanged", true},
            {"reeditable", true},
            {"png_alpha", true},
            {"fixed_date", true}};
}
QJsonObject testWritingCoordinates(const QString& fixtures, const QString& output)
{
    Document document;
    document.open(fixtures + "/D02.pdf");
    QJsonArray rows;
    for (int page = 0; page < document.pages(); ++page)
    {
        auto p = document.pdf().getCatalog()->getPage(page);
        const auto unit = p->getUserUnit();
        const auto point = p->getCropBox().topLeft() + QPointF(12, 16);
        const double width = 90;
        auto item = document.putImage(page, OverlayKind::Image, sampleImage(), point, width);
        check(qAbs(item.rect.width() * unit - width) < 1e-10,
              "physical image width respects UserUnit");
        document.moveSignature(page, item, {4, 5});
        document.resizeImage(page, signatures(document.pdf(), page).at(0), 60);
        rows.append(QJsonObject{{"page", page + 1}, {"unit", unit}});
    }
    document.save(output + "/writing-coordinates.pdf");
    Document reopened;
    reopened.open(output + "/writing-coordinates.pdf");
    for (int page = 0; page < reopened.pages(); ++page)
    {
        auto p = reopened.pdf().getCatalog()->getPage(page);
        auto item = signatures(reopened.pdf(), page).at(0);
        check(QLineF(item.rect.topLeft(), p->getCropBox().topLeft() + QPointF(16, 21)).length() <=
                  .5,
              "saved position within fixed 0.5pt");
        check(qAbs(item.rect.width() * p->getUserUnit() - 60) < 1e-8, "saved physical width");
        renderPage(reopened.pdf(), page, 1.1)
            .save(output + QString("/writing-coordinate-%1.png").arg(page));
    }
    return {{"pages", rows}, {"rotation_crop_userunit", true}};
}
QJsonObject testWritingUi(const QString& fixtures, const QString& output)
{
    Window window;
    window.resize(1024, 720);
    window.show();
    window.openFile(fixtures + "/D01.pdf");
    window.findChild<QAction*>("writingAction")->trigger();
    auto panel = static_cast<WritingPanel*>(window.findChild<QWidget*>("writingPanel"));
    auto kind = panel->findChild<QComboBox*>("writingKind");
    auto text = panel->findChild<QPlainTextEdit*>("writingText");
    kind->setCurrentIndex(1);
    const auto date = QDate::currentDate().toString("yyyy年M月d日");
    check(text->toPlainText() == date, "date initialized once by choice");
    ready(window);
    panel->findChild<QPushButton*>("placeWriting")->click();
    const QPointF datePoint(90, 300);
    QTest::mouseClick(window.canvas->viewport(), Qt::LeftButton, Qt::NoModifier,
                      window.canvas->pdfToViewport(0, datePoint).toPoint());
    check(byKind(window.doc, OverlayKind::Date).text == date, "actual pointer places date");
    const auto imagePath = output + "/writing-source.png";
    check(sampleImage().save(imagePath), "PNG input");
    panel->setImage(imagePath);
    panel->findChild<QDoubleSpinBox*>("writingImageWidth")->setValue(120);
    ready(window);
    panel->findChild<QPushButton*>("placeWriting")->click();
    QTest::mouseClick(window.canvas->viewport(), Qt::LeftButton, Qt::NoModifier,
                      window.canvas->pdfToViewport(0, {100, 400}).toPoint());
    auto image = byKind(window.doc, OverlayKind::Image);
    check(QLineF(image.rect.topLeft(), {100, 400}).length() <= .5,
          "actual pointer image placement");
    ready(window);
    const auto start = window.canvas->pdfToViewport(0, image.rect.center()).toPoint();
    const auto finish =
        window.canvas->pdfToViewport(0, image.rect.center() + QPointF(30, 20)).toPoint();
    const auto cursor = window.doc.cursor;
    QTest::mousePress(window.canvas->viewport(), Qt::LeftButton, Qt::NoModifier, start);
    QTest::mouseMove(window.canvas->viewport(), finish);
    QTest::mouseRelease(window.canvas->viewport(), Qt::LeftButton, Qt::NoModifier, finish);
    check(window.doc.cursor == cursor + 1, "image drag is one Undo operation");
    window.undoAction->trigger();
    check(byKind(window.doc, OverlayKind::Image).rect == image.rect,
          "UI Undo restores image position");
    window.redoAction->trigger();
    ready(window);
    const auto moved = byKind(window.doc, OverlayKind::Image);
    QTest::mouseClick(window.canvas->viewport(), Qt::LeftButton, Qt::NoModifier,
                      window.canvas->pdfToViewport(0, moved.rect.center()).toPoint());
    panel->findChild<QDoubleSpinBox*>("writingImageWidth")->setValue(90);
    panel->findChild<QPushButton*>("updateWriting")->click();
    check(qAbs(byKind(window.doc, OverlayKind::Image).rect.width() - 90) < 1e-8,
          "UI resize saved image");
    window.doc.save(output + "/writing-ui.pdf");
    ready(window);
    window.grab().save(output + "/writing-ui.png");
    for (auto widget : panel->findChildren<QWidget*>())
        if (widget->isVisible() &&
            (qobject_cast<QPushButton*>(widget) || qobject_cast<QAbstractSpinBox*>(widget)))
            check(window.rect().contains(QRect(widget->mapTo(&window, QPoint()), widget->size())),
                  "writing controls fit 1024x720");
    window.openFile(output + "/writing-ui.pdf");
    ready(window);
    image = byKind(window.doc, OverlayKind::Image);
    QTest::mouseClick(window.canvas->viewport(), Qt::LeftButton, Qt::NoModifier,
                      window.canvas->pdfToViewport(0, image.rect.center()).toPoint());
    panel->findChild<QDoubleSpinBox*>("writingImageWidth")->setValue(72);
    panel->findChild<QPushButton*>("updateWriting")->click();
    check(qAbs(byKind(window.doc, OverlayKind::Image).rect.width() - 72) < 1e-8,
          "UI reedit after reopen without source image");
    panel->findChild<QPushButton*>("removeWriting")->click();
    check(signatures(window.doc.pdf(), 0).size() == 1, "UI deletes image only");
    return {{"pointer_place", true},
            {"drag_undo", true},
            {"reopen_resize_delete", true},
            {"minimum_window", true}};
}
QJsonObject testSignatureLibrary(const QString& fixtures, const QString& output)
{
    const auto path = output + "/signature-library.json";
    SignatureLibrary library(path);
    check(library.load().isEmpty() && !QFile::exists(path), "no implicit persistence");
    SignatureTemplate text;
    text.name = "試験用署名";
    text.text = "山田 太郎";
    auto textId = library.add(text);
    SignatureTemplate image;
    image.name = "試験用画像";
    image.kind = OverlayKind::SignatureImage;
    image.image = sampleImage();
    const auto imageId = library.add(image);
    auto items = library.load();
    check(items.size() == 2 && items[1].image == image.image,
          "text and exact PNG templates roundtrip locally");
    Document document;
    document.open(fixtures + "/D01.pdf");
    document.putSignature(0, items[0].text, {80, 100}, items[0].size, items[0].color);
    document.putImage(0, items[1].kind, items[1].image, {80, 200}, items[1].width);
    document.save(output + "/template-applied.pdf");
    const auto pdfHash = fileHash(output + "/template-applied.pdf");
    library.remove(textId);
    library.remove(imageId);
    check(library.load().isEmpty() && fileHash(output + "/template-applied.pdf") == pdfHash,
          "template deletion never changes placed PDFs");
    QFile corrupt(path);
    check(corrupt.open(QIODevice::WriteOnly), "corrupt template test setup");
    corrupt.write("{broken");
    corrupt.close();
    const auto badHash = fileHash(path);
    bool refused = false;
    try
    {
        library.add(text);
    }
    catch (const std::exception&)
    {
        refused = true;
    }
    check(refused && fileHash(path) == badHash, "corrupt library rejected without overwrite");
    return {{"local_only", true},
            {"explicit_save", true},
            {"deletion_independent", true},
            {"corruption_preserved", true}};
}
} // namespace tatsu
