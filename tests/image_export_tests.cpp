#include "image_export_tests.h"
#include "image_export_dialog.h"
#include "pdfdocumentbuilder.h"
#include "window.h"
#include <QtTest/QTest>
#include <cstring>

namespace tatsu
{
namespace
{
void check(bool value, const QString& message)
{
    if (!value)
        fail(message);
}
QString folder(const QString& output, const QString& name)
{
    const auto path = output + "/" + name;
    check(QDir().mkpath(path), "create owned image output directory");
    return path;
}
void write(const QString& path, const QByteArray& data)
{
    QFile file(path);
    check(file.open(QIODevice::WriteOnly) && file.write(data) == data.size(),
          "write owned conflict fixture");
}
bool samePixels(QImage first, QImage second)
{
    if (first.size() != second.size())
        return false;
    first = first.convertToFormat(QImage::Format_RGBA8888);
    second = second.convertToFormat(QImage::Format_RGBA8888);
    for (int y = 0; y < first.height(); ++y)
        if (memcmp(first.constScanLine(y), second.constScanLine(y), size_t(first.width()) * 4))
            return false;
    return true;
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
    fail("Invalid export unexpectedly succeeded");
}
} // namespace
QJsonObject testImageExportGeometry(const QString& fixtures, const QString& output)
{
    Document source;
    source.open(fixtures + "/D02.pdf");
    for (int page = 0; page < source.pages(); ++page)
    {
        const auto crop = source.pdf().getCatalog()->getPage(page)->getCropBox();
        source.putSignature(page, "画像出力の署名", crop.topLeft() + QPointF(20, 30), 12,
                            Qt::black);
    }
    const auto before = encodePdf(source.pdf()), sourceHash = fileHash(source.source);
    source.save(output + "/m4-export-reference.pdf");
    const auto target = folder(output, "m4-export-png");
    auto result = exportImages(source.pdf(), {0, 1, 2, 3}, {target, "画像", "png", 96});
    check(!result.cancelled && result.files.size() == 4, "four physical pages exported");
    QJsonArray pages;
    for (int i = 0; i < result.files.size(); ++i)
    {
        check(result.files[i].success, "PNG published without overwriting");
        QImage actual(result.files[i].path), expected = renderPage(source.pdf(), i, 96. / 72.);
        check(samePixels(actual, expected), "PNG retains every rendered pixel including signature");
        check(qRound(actual.dotsPerMeterX() * .0254) == 96 &&
                  qRound(actual.dotsPerMeterY() * .0254) == 96,
              "PNG dpi metadata");
        const auto reference = output + QString("/m4-export-expected-%1.png").arg(i + 1);
        check(expected.save(reference),
              "rendered reference saved before independent pixel extraction");
        pages.append(QJsonObject{{"page", i + 1},
                                 {"path", QFileInfo(result.files[i].path).fileName()},
                                 {"reference", QFileInfo(reference).fileName()},
                                 {"width", actual.width()},
                                 {"height", actual.height()},
                                 {"dpi", 96}});
    }
    auto digital = readPdf(fixtures + "/D01.pdf");
    const auto jpegFolder = folder(output, "m4-export-jpeg");
    auto jpeg = exportImages(digital, {0}, {jpegFolder, "文字", "jpeg", 150});
    check(jpeg.files.size() == 1 && jpeg.files[0].success, "JPEG published");
    auto expected = renderPage(digital, 0, 150. / 72.).convertToFormat(QImage::Format_RGB888);
    check(expected.save(output + "/m4-jpeg-reference.png"), "lossy JPEG reference pixels");
    auto actual = QImage(jpeg.files[0].path).convertToFormat(QImage::Format_RGB888);
    check(actual.size() == expected.size(), "JPEG dimensions retained");
    qint64 difference = 0;
    for (int y = 0; y < expected.height(); ++y)
        for (int x = 0; x < expected.width() * 3; ++x)
            difference += qAbs(int(actual.constScanLine(y)[x]) - expected.constScanLine(y)[x]);
    const double mean = double(difference) / (expected.width() * qint64(expected.height()) * 3);
    check(mean <= 3, "fixed digital JPEG has mean channel difference at most 3");
    check(encodePdf(source.pdf()) == before && fileHash(source.source) == sourceHash,
          "image output preserves original PDF and source file");
    QFile manifest(output + "/m4-export-images.json");
    check(manifest.open(QIODevice::WriteOnly), "output reference manifest");
    manifest.write(
        QJsonDocument(
            QJsonObject{{"PNG", pages},
                        {"JPEG", QJsonObject{{"path", "m4-export-jpeg/文字-0001.jpg"},
                                             {"reference", "m4-jpeg-reference.png"},
                                             {"dpi", 150},
                                             {"mean_absolute_channel_difference", mean}}}})
            .toJson());
    return {{"PNG_pages", 4},
            {"all_PNG_pixels_equal", true},
            {"JPEG_mean_difference", mean},
            {"original_unchanged", true}};
}
QJsonObject testImageExportFailures(const QString& fixtures, const QString& output)
{
    auto source = readPdf(fixtures + "/D02.pdf");
    const auto before = encodePdf(source);
    const auto target = folder(output, "m4-export-failures");
    const ImageExportOptions valid{target, "安全", "png", 96};
    QJsonArray cases;
    auto reject = [&](const QString& name, const std::function<void()>& operation)
    { cases.append(QJsonObject{{"case", name}, {"error", rejects(operation)}}); };
    reject("empty", [&] { exportImages(source, {}, valid); });
    reject("129 pages", [&] { exportImages(source, QVector<int>(129, 0), valid); });
    reject("duplicate", [&] { exportImages(source, {0, 0}, valid); });
    reject("invalid page", [&] { exportImages(source, {5}, valid); });
    for (const auto& name :
         QStringList{"../outside", "bad/name", "bad\\name", "bad:", "", "end.", "end "})
    {
        auto options = valid;
        options.prefix = name;
        reject("invalid prefix", [&] { exportImages(source, {0}, options); });
    }
    for (int dpi : {74, 601})
    {
        auto options = valid;
        options.dpi = dpi;
        reject("invalid dpi", [&] { exportImages(source, {0}, options); });
    }
    auto wrongFormat = valid;
    wrongFormat.format = "bmp";
    reject("format", [&] { exportImages(source, {0}, wrongFormat); });
    auto missing = valid;
    missing.directory += "/missing";
    reject("missing directory", [&] { exportImages(source, {0}, missing); });
    auto restricted = readPdf(fixtures + "/viewer-navigation-restricted.pdf", "navigation-user");
    reject("copy forbidden", [&] { exportImages(restricted, {0}, valid); });
    pdf::PDFDocumentBuilder builder;
    builder.appendPage({0, 0, 4000, 4000});
    auto oversized = builder.build();
    reject("64 megapixel limit",
           [&] { exportImages(oversized, {0}, {target, "large", "png", 600}); });
    const auto existing = target + "/安全-0001.png";
    write(existing, "existing output");
    const auto existingHash = fileHash(existing);
    reject("preexisting output", [&] { exportImages(source, {0, 1}, valid); });
    check(fileHash(existing) == existingHash && !QFileInfo::exists(target + "/安全-0002.png"),
          "preflight does not overwrite or partly export");
    const auto cancelFolder = folder(output, "m4-export-cancel");
    bool cancelled = false;
    auto cancel = exportImages(
        source, {0, 1, 2}, {cancelFolder, "cancel", "png", 96}, [&] { return cancelled; },
        [&](const QString& phase, int page, int)
        {
            if (phase == "描画" && page == 1)
                cancelled = true;
        });
    check(
        cancel.cancelled && !cancel.files[0].success &&
            QDir(cancelFolder).entryList(QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot).isEmpty(),
        "render cancellation leaves no public image or temporary folder");
    const auto raceFolder = folder(output, "m4-export-race");
    QByteArray concurrentHash;
    auto raced = exportImages(source, {0, 1, 2}, {raceFolder, "race", "png", 96}, {},
                              [&](const QString& phase, int page, int)
                              {
                                  if (phase == "保存" && page == 1)
                                  {
                                      const auto path = raceFolder + "/race-0002.png";
                                      write(path, "concurrent writer");
                                      concurrentHash = fileHash(path);
                                  }
                              });
    check(raced.files[0].success && !raced.files[1].success && !raced.files[2].success &&
              !raced.files[1].error.isEmpty(),
          "publish race reports exact partial outcome");
    check(fileHash(raceFolder + "/race-0002.png") == concurrentHash &&
              !QFileInfo::exists(raceFolder + "/race-0003.png"),
          "concurrent output retained and remaining pages not published");
    check(QDir(raceFolder)
              .entryList({"pdf-tatsujin-image-export-*"}, QDir::Dirs | QDir::NoDotAndDotDot)
              .isEmpty(),
          "normal failure cleans private staging");
    check(encodePdf(source) == before, "errors preserve PDF objects");
    return {{"rejected", cases},
            {"cancel_before_publication", true},
            {"concurrent_file_retained", true},
            {"partial_outcome_reported", true}};
}
QJsonObject testImageExportUi(const QString& fixtures, const QString& output)
{
    Window original;
    original.resize(800, 480);
    original.show();
    original.openFile(fixtures + "/D02.pdf");
    check(QTest::qWaitFor([&] { return original.canvas->pageReady(0); }, 15000),
          "initial document display settled before export setup");
    original.canvas->goToPage(1);
    original.canvas->setZoom(.5);
    check(QTest::qWaitFor([&] { return original.canvas->pageReady(1); }, 15000),
          "original page settled");
    const auto before = encodePdf(original.doc.pdf()), hash = fileHash(original.doc.source);
    const auto anchor = original.canvas->anchor();
    const int cursor = original.doc.cursor, saved = original.doc.saved;
    auto action = original.findChild<QAction*>("exportPdfImages");
    check(action && action->isEnabled(), "real export action reachable");
    const auto target = folder(output, "m4-export-ui");
    int state = 0, ticks = 0;
    QString error;
    QTimer automation;
    QElapsedTimer deadline;
    deadline.start();
    QObject::connect(
        &automation, &QTimer::timeout,
        [&]
        {
            auto dialog = dynamic_cast<ImageExportDialog*>(QApplication::activeModalWidget());
            if (!dialog)
                return;
            ++ticks;
            if (deadline.elapsed() > 30000 && error.isEmpty())
                error = "image export UI timed out";
            if (!error.isEmpty())
            {
                dialog->reject();
                return;
            }
            try
            {
                auto run = dialog->findChild<QPushButton*>("imageExportRun");
                if (state == 0)
                {
                    dialog->resize(580, 420);
                    dialog->findChild<QLineEdit*>("imageExportDirectory")->setText(target);
                    dialog->findChild<QLineEdit*>("imageExportPrefix")->setText("指定ページ");
                    dialog->findChild<QComboBox*>("imageExportScope")->setCurrentIndex(2);
                    dialog->findChild<QLineEdit*>("imageExportRange")->setText("2,4");
                    dialog->findChild<QSpinBox*>("imageExportDpi")->setValue(96);
                    dialog->grab().save(output + "/m4-image-export-dialog.png");
                    state = 1;
                    QTest::mouseClick(run, Qt::LeftButton);
                }
                else if (state == 1 && run->isEnabled())
                {
                    auto results =
                        dialog->findChild<QPlainTextEdit*>("imageExportResults")->toPlainText();
                    check(results.contains("指定ページ-0002.png") &&
                              results.contains("指定ページ-0004.png") &&
                              results.count("保存済み") == 2,
                          "actual selected range and per-file results");
                    check(ticks > 2, "GUI handles events while rendering");
                    dialog->grab().save(output + "/m4-image-export-results.png");
                    state = 2;
                    dialog->reject();
                }
            }
            catch (const std::exception& e)
            {
                error = QString::fromUtf8(e.what());
                dialog->reject();
            }
        });
    automation.start(10);
    original.exportDocumentImages();
    automation.stop();
    check(error.isEmpty(), error);
    check(state == 2, "export result shown before close");
    const auto afterAnchor = original.canvas->anchor();
    check(encodePdf(original.doc.pdf()) == before && fileHash(original.doc.source) == hash &&
              original.doc.cursor == cursor && original.doc.saved == saved,
          "export preserves original, Undo and saved status");
    check(afterAnchor.page == anchor.page && QLineF(afterAnchor.point, anchor.point).length() < 2,
          "export preserves reading position");
    Window protectedWindow;
    protectedWindow.doc.open(fixtures + "/viewer-navigation-restricted.pdf", "navigation-user");
    protectedWindow.refresh(true);
    check(!protectedWindow.findChild<QAction*>("exportPdfImages")->isEnabled(),
          "copy-forbidden document disables image export");
    return {{"requested_physical_pages", QJsonArray{2, 4}},
            {"event_loop_ticks", ticks},
            {"original_Undo_position_unchanged", true},
            {"native_UI", "未実行"}};
}
} // namespace tatsu
