#include "image_pdf_tests.h"
#include "image_pdf_dialog.h"
#include "private_temp.h"
#include "window.h"
#include <QtEndian>
#include <QtTest/QTest>
#include <limits>

namespace tatsu
{
namespace
{
void check(bool condition, const QString& message)
{
    if (!condition)
        fail(message);
}
QByteArray bytes(const QString& path)
{
    QFile file(path);
    check(file.open(QIODevice::ReadOnly), "read test input");
    return file.readAll();
}
void write(const QString& path, const QByteArray& value)
{
    QFile file(path);
    check(file.open(QIODevice::WriteOnly) && file.write(value) == value.size(),
          "write owned test input");
}
QStringList synthetic(const QString& output, const QString& suffix)
{
    auto dir = output + "/m4-inputs-" + suffix;
    check(QDir().mkpath(dir), "create synthetic image directory");
    QStringList paths;
    for (int i = 0; i < 3; ++i)
    {
        const QSize size = i == 1 ? QSize(270, 180) : QSize(180, 270);
        QImage image(size, QImage::Format_ARGB32);
        for (int y = 0; y < size.height(); ++y)
            for (int x = 0; x < size.width(); ++x)
            {
                const QColor color =
                    y < size.height() / 2
                        ? (x < size.width() / 2 ? QColor(230, 30, 40) : QColor(30, 210, 60))
                        : (x < size.width() / 2 ? QColor(30, 50, 230) : QColor(230, 210, 30));
                image.setPixelColor(
                    x, y,
                    i == 2 ? QColor(color.red(), color.green(), color.blue(), x < 20 ? 0 : 128)
                           : color);
            }
        auto path = dir + QString("/画像-%1.png").arg(i + 1);
        check(image.save(path), "save PNG fixture");
        paths.append(path);
    }
    QImage jpeg(180, 120, QImage::Format_RGB32);
    jpeg.fill(QColor(30, 50, 230));
    QPainter painter(&jpeg);
    painter.fillRect(QRect(0, 0, 90, 60), QColor(230, 30, 40));
    painter.end();
    QByteArray encoded;
    QBuffer buffer(&encoded);
    buffer.open(QIODevice::WriteOnly);
    check(jpeg.save(&buffer, "JPEG", 95), "encode JPEG fixture");
    // One TIFF orientation entry: EXIF value 6 means 90 degrees clockwise.
    const auto exif =
        QByteArray::fromHex("45786966000049492a0008000000010012010300010000000600000000000000");
    check(exif.size() == 32 && encoded.startsWith(QByteArray::fromHex("ffd8")),
          "EXIF fixture structure");
    const auto path = dir + "/回転.jpg";
    write(path, encoded.left(2) + QByteArray::fromHex("ffe10022") + exif + encoded.mid(2));
    paths.append(path);
    return paths;
}
QByteArray pngChunk(const QByteArray& type, const QByteArray& data)
{
    QByteArray chunk(4, '\0');
    qToBigEndian<quint32>(quint32(data.size()), chunk.data());
    chunk += type + data;
    quint32 crc = 0xffffffff;
    for (char value : type + data)
    {
        crc ^= quint8(value);
        for (int bit = 0; bit < 8; ++bit)
            crc = (crc >> 1) ^ (0xedb88320 & (0u - (crc & 1)));
    }
    QByteArray checksum(4, '\0');
    qToBigEndian<quint32>(~crc, checksum.data());
    return chunk + checksum;
}
QByteArray pngImageData(const QByteArray& png)
{
    QByteArray result;
    for (qsizetype offset = 8; offset <= png.size() - 12;)
    {
        const auto count = qFromBigEndian<quint32>(png.constData() + offset);
        check(count <= quint64(png.size() - offset - 12), "synthetic PNG chunk bounds");
        if (png.mid(offset + 4, 4) == "IDAT")
            result += png.mid(offset + 8, count);
        offset += qsizetype(count) + 12;
    }
    check(!result.isEmpty(), "synthetic PNG has compressed image data");
    return result;
}
QVector<ImagePdfInput> inputs(const QStringList& paths)
{
    QVector<ImagePdfInput> result;
    for (const auto& path : paths)
        result.append({path, fileHash(path)});
    return result;
}
QPushButton* button(ImagePdfDialog& dialog, const char* name)
{
    auto result = dialog.findChild<QPushButton*>(name);
    check(result, "dialog has a named real button");
    return result;
}
void completed(ImagePdfDialog& dialog)
{
    check(QTest::qWaitFor([&] { return dialog.result() == QDialog::Accepted; }, 30000),
          "image PDF creation completes");
}
template <typename F> QString rejects(F operation)
{
    try
    {
        operation();
    }
    catch (const std::exception& error)
    {
        return QString::fromUtf8(error.what());
    }
    fail("Invalid/cancelled creation unexpectedly returned a PDF");
}
} // namespace
QJsonObject testImagePdfGeometry(const QString& output)
{
    const auto paths = synthetic(output, "geometry");
    const auto baseline = inputs(paths);
    Document created;
    created.history = {createImagePdf(baseline, {})};
    created.saved = -1;
    check(created.pages() == 4 && created.dirty(), "four new unsaved pages");
    QJsonArray references;
    for (int i = 0; i < paths.size(); ++i)
    {
        QImageReader reader(paths[i]);
        reader.setAutoTransform(true);
        auto original = reader.read();
        check(!original.isNull(),
              "decode frozen synthetic reference before independent verification");
        check(i != 3 || original.size() == QSize(120, 180), "EXIF rotates the JPEG dimensions");
        if (i == 3)
            check(original.pixelColor(90, 30).red() > 200,
                  "EXIF rotates the red top-left quadrant clockwise");
        const bool landscape = original.width() > original.height();
        const auto media = created.pdf().getCatalog()->getPage(i)->getMediaBox();
        check(qAbs(media.width() - (landscape ? 297 : 210) * 72.0 / 25.4) < .01 &&
                  qAbs(media.height() - (landscape ? 210 : 297) * 72.0 / 25.4) < .01,
              "A4 physical size and direction");
        const auto reference = output + QString("/m4-reference-%1.png").arg(i + 1);
        check(original.save(reference), "save decoded reference pixels");
        references.append(
            QJsonObject{{"input", QFileInfo(paths[i]).fileName()},
                        {"input_sha256", QString::fromLatin1(baseline[i].expectedHash.toHex())},
                        {"reference", QFileInfo(reference).fileName()},
                        {"width", original.width()},
                        {"height", original.height()},
                        {"alpha", original.hasAlphaChannel()}});
        check(fileHash(paths[i]) == baseline[i].expectedHash, "source image unchanged");
    }
    created.save(output + "/m4-images-a4.pdf");
    Document physical;
    physical.history = {createImagePdf(baseline, {ImagePdfOptions::PageMode::ImageSize, 150})};
    physical.saved = -1;
    physical.save(output + "/m4-images-150dpi.pdf");
    check(qAbs(physical.pdf().getCatalog()->getPage(0)->getMediaBox().width() - 86.4) < .0001,
          "180 pixels / 150 dpi gives 86.4 points");
    write(output + "/m4-image-references.json",
          QJsonDocument(QJsonObject{{"inputs_directory", QFileInfo(paths[0]).absolutePath()},
                                    {"references", references}})
              .toJson());
    return {{"pages", 4},
            {"EXIF_clockwise", true},
            {"inputs_unchanged", true},
            {"independent_pixel_verification", "separate evaluator"}};
}
QJsonObject testImagePdfFailures(const QString& output)
{
    const auto paths = synthetic(output, "failures");
    const auto baseline = inputs(paths);
    QJsonArray cases;
    auto reject = [&](const QString& name, const std::function<void()>& operation)
    {
        const auto error = rejects(operation);
        check(!error.isEmpty(), "failure has a visible reason");
        cases.append(QJsonObject{{"case", name}, {"error", error}});
    };
    reject("empty", [] { createImagePdf({}, {}); });
    reject("missing", [&] { createImagePdf({{output + "/missing-image.png", {}}}, {}); });
    QVector<ImagePdfInput> tooMany(129, baseline[0]);
    reject("129 images", [&] { createImagePdf(tooMany, {}); });
    const auto bad = output + "/m4-broken.png";
    write(bad, "not a PNG");
    reject("broken last input", [&] { createImagePdf({baseline[0], {bad, {}}}, {}); });
    const auto bmp = output + "/m4-not-png.png";
    QImage bitmap(30, 30, QImage::Format_RGB32);
    bitmap.fill(Qt::red);
    check(bitmap.save(bmp, "BMP"), "write content-format fixture");
    reject("BMP with PNG extension", [&] { createImagePdf({{bmp, {}}}, {}); });
    const auto animated = output + "/m4-two-frames.png";
    QByteArray animation;
    QDataStream control(&animation, QIODevice::WriteOnly);
    control << quint32(2) << quint32(0);
    auto frameControl = [](quint32 sequence)
    {
        QByteArray data;
        QDataStream stream(&data, QIODevice::WriteOnly);
        stream << sequence << quint32(180) << quint32(270) << quint32(0) << quint32(0) << quint16(1)
               << quint16(10) << quint8(0) << quint8(0);
        return data;
    };
    QByteArray sequence(4, '\0');
    qToBigEndian<quint32>(2, sequence.data());
    const auto firstPng = bytes(paths[0]), secondPng = bytes(paths[2]);
    check(firstPng.mid(8, 25) == secondPng.mid(8, 25), "two APNG frames use the same IHDR");
    write(animated,
          firstPng.left(33) + pngChunk("acTL", animation) + pngChunk("fcTL", frameControl(0)) +
              pngChunk("IDAT", pngImageData(firstPng)) + pngChunk("fcTL", frameControl(1)) +
              pngChunk("fdAT", sequence + pngImageData(secondPng)) + pngChunk("IEND", {}));
    const auto animationError = rejects([&] { createImagePdf({{animated, {}}}, {}); });
    check(animationError.contains("アニメーションPNG"),
          "APNG explicitly rejected rather than silently losing a frame");
    cases.append(QJsonObject{{"case", "two-frame APNG"}, {"error", animationError}});
    for (double dpi : {74., 601., std::numeric_limits<double>::infinity(),
                       std::numeric_limits<double>::quiet_NaN()})
        reject("invalid dpi",
               [&] { createImagePdf(baseline, {ImagePdfOptions::PageMode::ImageSize, dpi}); });
    auto temp = privateTemporaryDirectory(output + "/m4-boundaries-XXXXXX");
    check(temp->isValid(), "owned boundary directory");
    auto large = temp->filePath("36MP.png");
    QImage oversized(6000, 6000, QImage::Format_Grayscale8);
    oversized.fill(200);
    check(oversized.save(large), "small compressed but oversized image");
    oversized = {};
    reject("36 megapixels", [&] { createImagePdf({{large, {}}}, {}); });
    QImage fourMP(2000, 2000, QImage::Format_Grayscale8);
    fourMP.fill(180);
    const auto four = temp->filePath("4MP.png");
    check(fourMP.save(four), "aggregate limit fixture");
    fourMP = {};
    reject("132 megapixel aggregate",
           [&] { createImagePdf(QVector<ImagePdfInput>(33, {four, {}}), {}); });
    const auto huge = temp->filePath("too-large.png");
    QFile file(huge);
    check(file.open(QIODevice::WriteOnly) && file.resize(64LL * 1024 * 1024 + 1),
          "owned file-size boundary");
    file.close();
    reject("64MiB plus one", [&] { createImagePdf({{huge, {}}}, {}); });
    auto changed = baseline;
    changed[0].expectedHash = QByteArray(32, 'x');
    reject("preview conflict", [&] { createImagePdf(changed, {}); });
    reject("oversized physical page",
           [] { imagePdfLayout({32000000, 1}, {ImagePdfOptions::PageMode::ImageSize, 75}); });
    int published = 0;
    reject("cancel after first page",
           [&]
           {
               createImagePdf(
                   baseline, {}, [&] { return published > 0; },
                   [&](int n, int, const QString&) { published = n; });
           });
    check(published == 1, "cancel actually follows one generated page");
    const auto clone1 = temp->filePath("first.png"), clone2 = temp->filePath("second.png");
    write(clone1, bytes(paths[0]));
    write(clone2, bytes(paths[1]));
    reject("future input changed during build",
           [&]
           {
               createImagePdf(inputs({clone1, clone2}), {}, {},
                              [&](int n, int, const QString&)
                              {
                                  if (n == 1)
                                      write(clone2, bytes(paths[0]));
                              });
           });
    write(clone1, bytes(paths[0]));
    write(clone2, bytes(paths[1]));
    reject("past input changed before publication",
           [&]
           {
               createImagePdf(inputs({clone1, clone2}), {}, {},
                              [&](int n, int, const QString&)
                              {
                                  if (n == 1)
                                      write(clone1, bytes(paths[1]));
                              });
           });
    for (int i = 0; i < paths.size(); ++i)
        check(fileHash(paths[i]) == baseline[i].expectedHash, "original failure fixture retained");
    return {{"rejected_cases", cases},
            {"cancelled_after_completed_pages", published},
            {"no_partial_document_returned", true}};
}
QJsonObject testImagePdfWindow(const QString& fixtures, const QString& output)
{
    const auto paths = synthetic(output, "window");
    Window original;
    original.resize(800, 480);
    original.show();
    original.openFile(fixtures + "/D01.pdf");
    original.doc.putSignature(0, "元の未保存署名", {45, 55}, 16, Qt::black);
    original.refresh();
    const auto before = encodePdf(original.doc.pdf()), source = fileHash(original.doc.source);
    const auto cursor = original.doc.cursor, saved = original.doc.saved;
    QString error;
    QTimer automation;
    QObject::connect(
        &automation, &QTimer::timeout,
        [&]
        {
            auto dialog = dynamic_cast<ImagePdfDialog*>(QApplication::activeModalWidget());
            if (!dialog)
                return;
            automation.stop();
            try
            {
                dialog->resize(620, 420);
                auto list = dialog->findChild<QListWidget*>("imagePdfFiles");
                check(list && list->count() == 4, "actual confirmation list");
                list->setCurrentRow(1);
                QTest::mouseClick(button(*dialog, "imagePdfMoveUp"), Qt::LeftButton);
                check(sameFilePath(list->item(0)->data(Qt::UserRole).toString(), paths[1]),
                      "up button changes the order");
                dialog->grab().save(output + "/m4-image-dialog.png");
                check(button(*dialog, "imagePdfCreate")->isVisible() &&
                          dialog->rect().contains(
                              button(*dialog, "imagePdfCreate")->mapTo(dialog, QPoint())),
                      "create reachable at compact size");
                QTest::mouseClick(button(*dialog, "imagePdfCreate"), Qt::LeftButton);
            }
            catch (const std::exception& e)
            {
                error = QString::fromUtf8(e.what());
                dialog->reject();
            }
        });
    automation.start(10);
    QTimer::singleShot(30000, &original,
                       [&]
                       {
                           if (auto dialog =
                                   dynamic_cast<ImagePdfDialog*>(QApplication::activeModalWidget()))
                           {
                               error = "creation dialog did not complete";
                               dialog->reject();
                           }
                       });
    original.createFromImages(paths);
    check(error.isEmpty(), error);
    Window* created = nullptr;
    for (auto widget : QApplication::topLevelWidgets())
        if (widget->objectName() == "imageCreatedDocument")
            created = dynamic_cast<Window*>(widget);
    check(created && created->doc.pages() == 4 && created->doc.dirty() &&
              created->doc.source.isEmpty(),
          "separate unsaved document window");
    std::unique_ptr<Window> owner(created);
    check(created->doc.pdf().getCatalog()->getPage(0)->getMediaBox().width() >
              created->doc.pdf().getCatalog()->getPage(0)->getMediaBox().height(),
          "UI order is actual PDF order");
    check(encodePdf(original.doc.pdf()) == before && original.doc.cursor == cursor &&
              original.doc.saved == saved && fileHash(original.doc.source) == source,
          "original PDF, signature, Undo and file preserved");
    auto added = created->doc.putSignature(0, "山田 太郎", {45, 35}, 16, Qt::black);
    created->doc.moveSignature(0, added, {10, 5});
    created->doc.undo();
    created->doc.save(output + "/m4-window-signed.pdf");
    Document reopened;
    reopened.open(output + "/m4-window-signed.pdf");
    check(reopened.pages() == 4 && signatures(reopened.pdf(), 0).size() == 1,
          "save and reopen created PDF with editable signature");
    const auto signature = signatures(reopened.pdf(), 0).first();
    reopened.moveSignature(0, signature, {6, 4});
    reopened.undo();
    check(signatures(reopened.pdf(), 0).first().rect == signature.rect,
          "reopened signature edit and Undo");
    created->refresh(true);
    QTest::qWait(80);
    created->grab().save(output + "/m4-created-window.png");
    return {{"new_document_pages", 4},
            {"original_unchanged", true},
            {"ordered_creation_save_reedit_Undo", true},
            {"native_UI", "未実行"}};
}
QJsonObject testImagePdfCancelRetry(const QString& output)
{
    const auto dir = output + "/m4-inputs-cancel";
    check(QDir().mkpath(dir), "cancel input directory");
    QImage image(1000, 1000, QImage::Format_RGB32);
    quint32 state = 0x12345678;
    for (int y = 0; y < image.height(); ++y)
        for (int x = 0; x < image.width(); ++x)
        {
            state ^= state << 13;
            state ^= state >> 17;
            state ^= state << 5;
            reinterpret_cast<QRgb*>(image.scanLine(y))[x] = 0xff000000 | (state & 0xffffff);
        }
    const auto path = dir + "/noise.png";
    check(image.save(path), "bounded real workload PNG");
    image = {};
    const auto baseline = fileHash(path);
    QStringList paths;
    for (int i = 0; i < 20; ++i)
        paths.append(path);
    ImagePdfDialog dialog(paths);
    dialog.show();
    int ticks = 0;
    QTimer tick;
    QObject::connect(&tick, &QTimer::timeout, [&] { ++ticks; });
    tick.start(5);
    QTest::mouseClick(button(dialog, "imagePdfCreate"), Qt::LeftButton);
    auto progress = dialog.findChild<QLabel*>("imagePdfProgress");
    check(QTest::qWaitFor([&] { return progress->text().startsWith("作成 "); }, 15000),
          "real job publishes page progress");
    check(!button(dialog, "imagePdfCreate")->isEnabled() &&
              button(dialog, "imagePdfCancel")->isEnabled(),
          "cancel available during actual creation");
    dialog.grab().save(output + "/m4-create-running.png");
    QTest::mouseClick(button(dialog, "imagePdfCancel"), Qt::LeftButton);
    check(QTest::qWaitFor([&] { return button(dialog, "imagePdfCreate")->isEnabled(); }, 15000),
          "cancel joins owned worker and allows retry");
    check(dialog.result() != QDialog::Accepted && progress->text().contains("中止しました") &&
              ticks > 2,
          "no partial PDF and GUI event loop responsive");
    auto list = dialog.findChild<QListWidget*>("imagePdfFiles");
    while (list->count() > 1)
    {
        list->setCurrentRow(list->count() - 1);
        QTest::mouseClick(button(dialog, "imagePdfRemove"), Qt::LeftButton);
    }
    const auto bad = dir + "/broken.png";
    write(bad, "broken image");
    dialog.appendFiles({bad});
    QTest::mouseClick(button(dialog, "imagePdfCreate"), Qt::LeftButton);
    check(QTest::qWaitFor([&] { return button(dialog, "imagePdfCreate")->isEnabled(); }, 15000),
          "failed job allows fixing the list");
    check(dialog.result() != QDialog::Accepted && progress->text().contains("broken.png"),
          "failure identifies input and creates no document");
    list->setCurrentRow(1);
    QTest::mouseClick(button(dialog, "imagePdfRemove"), Qt::LeftButton);
    QTest::mouseClick(button(dialog, "imagePdfCreate"), Qt::LeftButton);
    completed(dialog);
    auto result = dialog.takeDocument();
    check(result.getCatalog()->getPageCount() == 1 && fileHash(path) == baseline,
          "cancel and failure followed by successful retry, source retained");
    return {
        {"event_loop_ticks", ticks}, {"cancel_failure_retry", true}, {"source_unchanged", true}};
}
QJsonObject testImagePdfOcr(const QString& fixtures, const QString& output)
{
    const auto sourceHash = fileHash(fixtures + "/D03.pdf");
    auto source = readPdf(fixtures + "/D03.pdf");
    QStringList paths;
    for (int page : {2, 6})
    {
        const auto path = output + QString("/m4-scan-page-%1.png").arg(page + 1);
        check(renderPage(source, page, 300. / 72., false).save(path),
              "derive scan without changing frozen D03");
        paths.append(path);
    }
    Window window;
    window.show();
    window.doc.history = {createImagePdf(inputs(paths), {})};
    window.doc.saved = -1;
    ++window.doc.revision;
    window.doc.putSignature(0, "画像から作った文書", {40, 30}, 14, Qt::black);
    window.refresh(true);
    window.doc.save(output + "/m4-scan-before.pdf");
    QString unexpected;
    QTimer dialogs;
    QObject::connect(&dialogs, &QTimer::timeout,
                     [&]
                     {
                         if (auto message =
                                 qobject_cast<QMessageBox*>(QApplication::activeModalWidget()))
                         {
                             if (message->windowTitle() != "OCR結果")
                                 unexpected = message->text();
                             message->accept();
                         }
                     });
    dialogs.start(20);
    window.ocrAction->trigger();
    window.language->setCurrentIndex(0);
    window.scope->setCurrentIndex(0);
    window.startOcr();
    check(QTest::qWaitFor([&] { return !window.doc.busy && !window.worker; }, 90000),
          "real bilingual OCR on image-created document");
    check(unexpected.isEmpty(), unexpected);
    check(pageText(window.doc.pdf(), 0).contains("市民公園") &&
              pageText(window.doc.pdf(), 1).contains("coastal", Qt::CaseInsensitive),
          "fixed Japanese and English search terms survive image creation and OCR");
    check(signatures(window.doc.pdf(), 0).size() == 1, "signature retained by OCR");
    window.canvas->setFocus();
    QTest::keyClick(window.canvas->viewport(), Qt::Key_F, Qt::ControlModifier);
    window.query->setText("市民公園");
    QTest::keyClick(window.query, Qt::Key_Return);
    auto search = window.findChild<SearchPanel*>("searchPanel");
    check(QTest::qWaitFor(
              [&]
              { return search->session()->complete() && !search->session()->matches().isEmpty(); },
              10000),
          "actual search after OCR");
    window.canvas->setZoom(.5);
    window.canvas->goToPage(0);
    check(QTest::qWaitFor([&] { return window.canvas->pageReady(0); }, 15000),
          "created OCR page renders for text selection");
    const auto dimensions = pageSize(window.doc.pdf().getCatalog()->getPage(0));
    const auto first = window.canvas->mapFromScene({1, 1});
    const auto last =
        window.canvas->mapFromScene({dimensions.width() - 1, dimensions.height() - 1});
    QTest::mousePress(window.canvas->viewport(), Qt::LeftButton, Qt::NoModifier, first);
    QTest::mouseMove(window.canvas->viewport(), last, 80);
    QTest::mouseRelease(window.canvas->viewport(), Qt::LeftButton, Qt::NoModifier, last);
    check(QTest::qWaitFor([&] { return window.canvas->selectionReady(); }, 15000),
          "created OCR text selection completes");
    QTest::keyClick(window.canvas, Qt::Key_C, Qt::ControlModifier);
    const auto text = QApplication::clipboard()->text();
    check(text.size() > 500 && text.contains("市民公園"),
          "actual Qt copy after image creation and OCR");
    window.doc.save(output + "/m4-scan-ocr-signed.pdf");
    Document reopened;
    reopened.open(output + "/m4-scan-ocr-signed.pdf");
    check(pageText(reopened.pdf(), 1).contains("coastal", Qt::CaseInsensitive) &&
              signatures(reopened.pdf(), 0).size() == 1,
          "saved OCR text and signature reopen");
    check(fileHash(fixtures + "/D03.pdf") == sourceHash, "fixed scan PDF remains unchanged");
    return {{"derived_pages", QJsonArray{3, 7}},
            {"search_terms", QJsonArray{"市民公園", "coastal"}},
            {"real_bilingual_OCR", true},
            {"signature_search_save_reopen", true},
            {"Japanese_copy_characters", text.size()}};
}
} // namespace tatsu
