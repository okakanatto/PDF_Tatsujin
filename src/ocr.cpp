#include "ocr.h"
#include "document.h"
#include "ocr_job.h"
#include "ocr_jobs.h"
#include "ocr_language.h"
#include "ocr_preprocess.h"
#include "pdfcms.h"
#include "pdfdocumentbuilder.h"
#include "pdffont.h"
#include "pdfrenderer.h"
#include "pdftextlayoutgenerator.h"
#include "vertical_ocr_layer.h"
#include <cstdio>
#include <tesseract/baseapi.h>
#include <tesseract/resultiterator.h>

namespace tatsu
{
static bool readOcrModel(const char* filename, std::vector<char>* data)
{
    if (!filename || !data)
        return false;
    const auto name = QFileInfo(QString::fromUtf8(filename)).fileName();
    if (name != "jpn.traineddata" && name != "eng.traineddata" && name != "jpn_vert.traineddata")
        return false;
    // Read the same bundled bytes through Qt's Unicode/long-path filesystem
    // support. Do not let model names address files supplied by a PDF.
    QFile file(asset("tessdata/" + name));
    if (!file.open(QIODevice::ReadOnly) || file.size() <= 0 || file.size() > 128 * 1024 * 1024)
        return false;
    data->resize(size_t(file.size()));
    return file.read(data->data(), qint64(data->size())) == qint64(data->size()) && file.atEnd();
}
static void put(PDFDictionary& d, const char* k, PDFObject v)
{
    d.setEntry(PDFInplaceOrMemoryString(k), std::move(v));
}
static PDFObject dictionary(PDFDictionary d)
{
    return PDFObject::createDictionary(std::make_shared<PDFDictionary>(std::move(d)));
}
static PDFObject array(std::vector<PDFObject> v)
{
    return PDFObject::createArray(std::make_shared<PDFArray>(std::move(v)));
}
static PDFObject stream(PDFDictionary d, QByteArray bytes)
{
    put(d, "Length", PDFObject::createInteger(bytes.size()));
    return PDFObject::createStream(std::make_shared<PDFStream>(std::move(d), std::move(bytes)));
}
static QByteArray contentBytes(const PDFDocument& d, PDFObject o)
{
    o = d.getObject(o);
    QByteArray data;
    if (o.isArray())
    {
        for (auto& v : *o.getArray())
            data += contentBytes(d, v) + '\n';
    }
    else if (o.isStream())
        data = d.getDecodedStream(o.getStream());
    return data;
}
class Probe : public PDFTextLayoutGenerator
{
public:
    using PDFTextLayoutGenerator::PDFTextLayoutGenerator;
    bool invisible = false, images = false;
    QVector<QRectF> visible;
    QVector<QRectF> blackBlocks;

protected:
    void performPathPainting(const QPainterPath& path, bool stroke, bool fill, bool text,
                             Qt::FillRule) override
    {
        if (!stroke && fill && !text && blackBlocks.size() < 1000 &&
            getGraphicState()->getFillColorWithAlpha() == QColor(Qt::black))
            if (const auto box = axisAlignedRectangle(getCurrentWorldMatrix().map(path)))
                blackBlocks << *box;
    }
    bool isContentKindSuppressed(ContentKind k) const override
    {
        if (k == ContentKind::Images || k == ContentKind::Shapes)
            return false;
        return PDFTextLayoutGenerator::isContentKindSuppressed(k);
    }
    void performImagePainting(const QImage&) override
    {
        images = true;
    }
    void performOutputCharacter(const PDFTextCharacterInfo& info) override
    {
        if (isContentSuppressed())
            return;
        auto mode = getGraphicState()->getTextRenderingMode();
        if (mode == TextRenderingMode::Invisible)
            invisible = true;
        else
            visible << getPagePointToDevicePointMatrix()
                           .map(info.matrix.map(info.outline))
                           .boundingRect();
        PDFTextLayoutGenerator::performOutputCharacter(info);
    }
};
static PDFDocument recognizedLayer(tesseract::TessBaseAPI& api, QSizeF points)
{
    // Preserve Tesseract's line text, including its language-aware spacing.
    // Its stock PDF renderer appends a space to every word, including Japanese
    // segmentation units. Real embedded glyphs also give readers usable geometry.
    PDFContentStreamBuilder streamBuilder(points, PDFContentStreamBuilder::CoordinateSystem::Qt);
    auto painter = streamBuilder.begin();
    QFont font(signatureFont());
    font.setPixelSize(100);
    font.setStyleStrategy(QFont::NoFontMerging);
    painter->setFont(font);
    painter->setPen(Qt::black);
    QFontMetricsF metrics(font);
    auto raw = QRawFont::fromFont(font);
    QString allText;
    std::unique_ptr<tesseract::ResultIterator> iterator(api.GetIterator());
    if (iterator)
        do
        {
            std::unique_ptr<char[]> utf8(iterator->GetUTF8Text(tesseract::RIL_TEXTLINE));
            QString text = QString::fromUtf8(utf8.get()).trimmed();
            int left, top, right, bottom;
            if (text.isEmpty() ||
                !iterator->BoundingBox(tesseract::RIL_TEXTLINE, &left, &top, &right, &bottom))
                continue;
            for (auto cp : text.toUcs4())
                if (!raw.supportsCharacter(cp))
                    fail(QString("OCR文字層のフォントが U+%1 に対応していません。")
                             .arg(cp, 4, 16, QChar('0')));
            allText += text;
            auto bounds = metrics.tightBoundingRect(text);
            if (bounds.isEmpty())
                continue;
            QRectF target(left * 72.0 / 300, top * 72.0 / 300, (right - left) * 72.0 / 300,
                          (bottom - top) * 72.0 / 300);
            painter->save();
            painter->translate(target.topLeft());
            painter->scale(target.width() / bounds.width(), target.height() / bounds.height());
            painter->translate(-bounds.topLeft());
            painter->drawText(QPointF(), text);
            painter->restore();
        } while (iterator->Next(tesseract::RIL_TEXTLINE));
    auto generated = streamBuilder.end(painter);
    auto page = generated.document.getCatalog()->getPage(0);
    PDFDocumentBuilder builder(&generated.document);
    QByteArray commands = "BT 3 Tr ET\n" + contentBytes(generated.document, page->getContents());
    auto pd = *builder.getObjectByReference(page->getPageReference()).getDictionary();
    put(pd, "Contents", PDFObject::createReference(builder.addObject(stream({}, commands))));
    builder.setObject(page->getPageReference(), dictionary(pd));
    return correctFontUnicode(builder.build(), allText, raw);
}
static void mergeLayer(PDFDocument& doc, int page, const PDFDocument& layer, bool vertical)
{
    auto p = doc.getCatalog()->getPage(page);
    auto overlay = layer.getCatalog()->getPage(0);
    auto crop = p->getCropBox();
    auto size = overlay->getMediaBox().size();
    PDFDocumentBuilder b(&doc);
    PDFDictionary resources;
    auto r = doc.getObject(p->getResources());
    if (r.isDictionary())
        resources = *r.getDictionary();
    QByteArray name = "TatsuOCR_" + QUuid::createUuid().toByteArray(QUuid::Id128);
    QByteArray command = QString("Q\nq %1 0 0 %2 %3 %4 cm\n")
                             .arg(crop.width() / size.width(), 0, 'g', 15)
                             .arg(crop.height() / size.height(), 0, 'g', 15)
                             .arg(crop.x(), 0, 'g', 15)
                             .arg(crop.y(), 0, 'g', 15)
                             .toLatin1();
    if (vertical)
    {
        const auto generatedResources = layer.getObject(overlay->getResources());
        const auto generatedFonts =
            layer.getObject(generatedResources.getDictionary()->get("Font"));
        if (!generatedFonts.isDictionary() || generatedFonts.getDictionary()->getCount() < 1 ||
            generatedFonts.getDictionary()->getCount() > 2)
            fail("縦書き文字層の字体を安全に統合できません。");
        PDFDictionary fonts;
        const auto existing = doc.getObject(resources.get("Font"));
        if (existing.isDictionary())
            fonts = *existing.getDictionary();
        // Top-level glyph objects let readers detect vertical page flow. Keeping
        // them in a Form causes some readers to infer horizontal column order.
        auto glyphs = contentBytes(layer, overlay->getContents());
        std::vector<PDFObject> sourceFonts;
        for (size_t i = 0; i < generatedFonts.getDictionary()->getCount(); ++i)
            sourceFonts.push_back(generatedFonts.getDictionary()->getValue(i));
        // One import preserves shared embedded programs across CID chunks.
        const auto importedFonts = b.copyFrom(sourceFonts, layer.getStorage(), true);
        for (size_t i = 0; i < generatedFonts.getDictionary()->getCount(); ++i)
        {
            const auto sourceName = generatedFonts.getDictionary()->getKey(i).getString();
            if (sourceName != "TatsujinVertical" + QByteArray::number(i))
                fail("縦書き文字層の字体名を確認できません。");
            const auto targetName = name + '_' + QByteArray::number(i);
            fonts.setEntry(PDFInplaceOrMemoryString(targetName), PDFObject(importedFonts.at(i)));
            glyphs.replace('/' + sourceName + ' ', '/' + targetName + ' ');
        }
        put(resources, "Font", dictionary(fonts));
        command += glyphs + "Q\n";
    }
    else
    {
        auto imported = b.copyFrom({overlay->getResources()}, layer.getStorage(), true).at(0);
        PDFDictionary form;
        put(form, "Type", PDFObject::createName("XObject"));
        put(form, "Subtype", PDFObject::createName("Form"));
        put(form, "BBox",
            array({PDFObject::createInteger(0), PDFObject::createInteger(0),
                   PDFObject::createReal(size.width()), PDFObject::createReal(size.height())}));
        put(form, "Resources", imported);
        auto formRef = b.addObject(stream(form, contentBytes(layer, overlay->getContents())));
        PDFDictionary xo;
        const auto x = doc.getObject(resources.get("XObject"));
        if (x.isDictionary())
            xo = *x.getDictionary();
        xo.setEntry(PDFInplaceOrMemoryString(name), PDFObject::createReference(formRef));
        put(resources, "XObject", dictionary(xo));
        command += "/" + name + " Do Q\n";
    }
    std::vector<PDFObject> contents{PDFObject::createReference(b.addObject(stream({}, "q\n")))};
    // PDFPage caches resolved streams. PDF syntax requires every stream in /Contents
    // to be an indirect object; preserve the original dictionary's references.
    auto original =
        doc.getObjectByReference(p->getPageReference()).getDictionary()->get("Contents");
    auto old = doc.getObject(original);
    if (old.isArray())
    {
        for (auto& o : *old.getArray())
            contents.push_back(o.isStream() ? PDFObject::createReference(b.addObject(o)) : o);
    }
    else if (!old.isNull())
        contents.push_back(original.isStream() ? PDFObject::createReference(b.addObject(original))
                                               : original);
    contents.push_back(PDFObject::createReference(b.addObject(stream({}, command))));
    auto pd = *b.getObjectByReference(p->getPageReference()).getDictionary();
    put(pd, "Contents", array(contents));
    put(pd, "Resources", dictionary(resources));
    put(pd, "TatsujinOCR", PDFObject::createBool(true));
    b.setObject(p->getPageReference(), dictionary(pd));
    doc = b.build();
}
int ocrWorker(const QStringList& args)
{
    // A separate process owns the complete operation; the UI only imports its validated final PDF.
    try
    {
        if (args.size() != 5)
            fail("OCR引数が不正です。");
        Document session;
        auto jobLock = lockOwnedOcrWorker(args[1]);
        session.open(args[1]);
        session.editable();
        auto doc = session.pdf();
        QFile settings(args[3]);
        if (!settings.open(QIODevice::ReadOnly))
            fail("OCR設定を読めません。");
        auto config = QJsonDocument::fromJson(settings.readAll()).object();
        QString lang = config["language"].toString("jpn+eng");
        if (!ocrLanguageCodes().contains(lang))
            fail("OCR言語が不正です。");
        auto pages = parsePages(config["pages"].toString(), session.pages());
        validateOcrPageGeometry(doc, lang, pages);
        QJsonArray results;
        tesseract::TessBaseAPI api;
        const auto models = asset("tessdata").toUtf8();
        if (api.Init(models.constData(), 0, lang.toLatin1().constData(), tesseract::OEM_LSTM_ONLY,
                     nullptr, 0, nullptr, nullptr, false, readOcrModel) != 0)
            fail("OCRモデルの初期化に失敗しました。");
        std::vector<std::string> loaded;
        api.GetLoadedLanguagesAsVector(&loaded);
        for (const auto& requested : lang.split('+'))
            if (std::find(loaded.begin(), loaded.end(), requested.toStdString()) == loaded.end())
                fail("指定したOCR言語のモデルを読み込めません。結果は反映しません。");
        api.SetPageSegMode(tesseract::PSM_AUTO);
        api.SetVariable("user_defined_dpi", "300");
        api.SetVariable("preserve_interword_spaces", "1");
        int n = 0;
        for (int index : pages)
        {
            auto p = doc.getCatalog()->getPage(index);
            PDFFontCache fonts(128, 128);
            fonts.setDocument(PDFModifiedDocument(&doc, nullptr));
            PDFCMSManager cms(nullptr);
            auto color = cms.getCurrentCMS();
            auto matrix = pageMatrix(p, 300.0 / 72, false);
            Probe probe(PDFRenderer::getDefaultFeatures(), p, &doc, &fonts, color.data(), nullptr,
                        matrix, {});
            auto errors = probe.processContents();
            if (!errors.isEmpty() && !p->getContents().isNull())
            {
                QStringList details;
                for (auto error : errors)
                    details << error.message;
                fail(QString("%1ページの内容を安全に解析できません: %2")
                         .arg(index + 1)
                         .arg(details.join("; ")));
            }
            QString status;
            if (probe.invisible)
                status = "既存OCR保持";
            else if (!probe.images)
                status = probe.visible.isEmpty() ? "文字未検出" : "処理不要（既存文字）";
            else
            {
                auto image = renderPage(doc, index, 300.0 / 72, false, false);
                QPainter mask(&image);
                for (auto box : probe.visible)
                    mask.fillRect(box.adjusted(-2, -2, 2, 2), Qt::white);
                mask.end();
                image = image.convertToFormat(QImage::Format_Grayscale8);
                omitSolidBlackOcrBlocks(image, probe.blackBlocks);
                api.SetImage(image.constBits(), image.width(), image.height(), 1,
                             image.bytesPerLine());
                api.SetSourceResolution(300);
                if (api.Recognize(nullptr) != 0)
                    fail(QString("%1ページのOCRが失敗しました。").arg(index + 1));
                std::unique_ptr<char[]> text(api.GetUTF8Text());
                QString recognized = QString::fromUtf8(text.get());
                if (recognized.trimmed().isEmpty())
                    status = "文字未検出";
                else
                {
                    auto layer = lang == "jpn_vert"
                                     ? verticalOcrLayer(api, pageSize(p, false), image)
                                     : recognizedLayer(api, pageSize(p, false));
                    mergeLayer(doc, index, layer, lang == "jpn_vert");
                    status = "処理済み";
                }
                api.Clear();
            }
            results.append(QJsonObject{{"page", index + 1}, {"status", status}});
            auto progress = QJsonDocument(QJsonObject{{"done", ++n},
                                                      {"total", pages.size()},
                                                      {"page", index + 1},
                                                      {"status", status}})
                                .toJson(QJsonDocument::Compact);
            fwrite(progress.constData(), 1, progress.size(), stdout);
            fputc('\n', stdout);
            fflush(stdout);
        }
        writeCandidate(doc, args[2]);
        QFile report(args[4]);
        if (!report.open(QIODevice::WriteOnly))
            fail("OCR結果を記録できません。");
        const auto bytes =
            QJsonDocument(QJsonObject{{"pages", results}, {"language", lang}}).toJson();
        if (report.write(bytes) != bytes.size() || !report.flush())
            fail("OCR結果を記録できません。");
        return 0;
    }
    catch (const std::exception& e)
    {
        auto message = QByteArray(e.what());
        fwrite(message.constData(), 1, message.size(), stderr);
        return 2;
    }
}
} // namespace tatsu
