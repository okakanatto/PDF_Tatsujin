#include "ocr.h"
#include "document.h"
#include "ocr_jobs.h"
#include "pdfcms.h"
#include "pdfdocumentbuilder.h"
#include "pdffont.h"
#include "pdfrenderer.h"
#include "pdftextlayoutgenerator.h"
#include <cstdio>
#include <tesseract/baseapi.h>
#include <tesseract/renderer.h>
#include <tesseract/resultiterator.h>

namespace tatsu
{
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

protected:
    bool isContentKindSuppressed(ContentKind k) const override
    {
        if (k == ContentKind::Images)
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
static void mergeLayer(PDFDocument& doc, int page, const PDFDocument& layer)
{
    auto p = doc.getCatalog()->getPage(page);
    auto overlay = layer.getCatalog()->getPage(0);
    auto crop = p->getCropBox();
    auto size = overlay->getMediaBox().size();
    PDFDocumentBuilder b(&doc);
    auto imported = b.copyFrom({overlay->getResources()}, layer.getStorage(), true).at(0);
    PDFDictionary form;
    put(form, "Type", PDFObject::createName("XObject"));
    put(form, "Subtype", PDFObject::createName("Form"));
    put(form, "BBox",
        array({PDFObject::createInteger(0), PDFObject::createInteger(0),
               PDFObject::createReal(size.width()), PDFObject::createReal(size.height())}));
    put(form, "Resources", imported);
    auto formRef = b.addObject(stream(form, contentBytes(layer, overlay->getContents())));
    PDFDictionary resources;
    auto r = doc.getObject(p->getResources());
    if (r.isDictionary())
        resources = *r.getDictionary();
    PDFDictionary xo;
    auto x = doc.getObject(resources.get("XObject"));
    if (x.isDictionary())
        xo = *x.getDictionary();
    QByteArray name = "TatsuOCR_" + QUuid::createUuid().toByteArray(QUuid::Id128);
    xo.setEntry(PDFInplaceOrMemoryString(name), PDFObject::createReference(formRef));
    put(resources, "XObject", dictionary(xo));
    QByteArray command = QString("Q\nq %1 0 0 %2 %3 %4 cm /%5 Do Q\n")
                             .arg(crop.width() / size.width(), 0, 'g', 15)
                             .arg(crop.height() / size.height(), 0, 'g', 15)
                             .arg(crop.x(), 0, 'g', 15)
                             .arg(crop.y(), 0, 'g', 15)
                             .arg(QString::fromLatin1(name))
                             .toLatin1();
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
        if (lang != "jpn+eng" && lang != "jpn" && lang != "eng")
            fail("OCR言語が不正です。");
        auto pages = parsePages(config["pages"].toString(), session.pages());
        QJsonArray results;
        QTemporaryDir temp(QFileInfo(args[1]).absolutePath() + "/worker-XXXXXX");
        if (!temp.isValid())
            fail("OCR一時領域を作成できません。");
        // Tesseract's C file interfaces require a locally representable path. Reject unsupported
        // paths explicitly.
        if (QString::fromLocal8Bit(temp.path().toLocal8Bit()) != temp.path())
            fail("OCR一時領域のパスを扱えません。");
        for (auto file :
             QStringList{"jpn.traineddata", "jpn_vert.traineddata", "eng.traineddata", "pdf.ttf"})
            if (!QFile::copy(asset("tessdata/" + file), temp.filePath(file)))
                fail("OCRモデルまたはPDFフォントが見つかりません。");
        tesseract::TessBaseAPI api;
        if (api.Init(temp.path().toLocal8Bit().constData(), lang.toLatin1().constData(),
                     tesseract::OEM_LSTM_ONLY) != 0)
            fail("OCRモデルの初期化に失敗しました。");
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
                    auto layer = recognizedLayer(api, pageSize(p, false));
                    mergeLayer(doc, index, layer);
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
        report.write(QJsonDocument(QJsonObject{{"pages", results}, {"language", lang}}).toJson());
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
