#include "document_comparison.h"
#include "form_fields.h"
#include "navigation.h"
#include "pdfcms.h"
#include "pdffont.h"
#include "pdfsecurityhandler.h"
#include "pdftextlayoutgenerator.h"
#include "save_candidate.h"
#include "windows_path.h"
#include <cmath>
#include <windows.h>

namespace tatsu
{
namespace
{
constexpr double coordinateTolerance = 1e-6;
void stop(const std::function<bool()>& cancelled)
{
    if (cancelled && cancelled())
        fail("比較を中止しました。文書は変更していません。");
}
class CharacterCounter final : public PDFTextLayoutGenerator
{
public:
    using PDFTextLayoutGenerator::PDFTextLayoutGenerator;
    qint64 count = 0, maximum = 0;
    std::function<bool()> cancelled;

protected:
    void performOutputCharacter(const PDFTextCharacterInfo& info) override
    {
        if (isContentSuppressed() || info.character.isSpace())
            return;
        if (++count > maximum)
            fail("抽出文字が比較上限を超えています。部分結果は表示しません。");
        if (count % 256 == 0)
            stop(cancelled);
        // Count the same non-space characters as the SDK layout, without
        // constructing its potentially expensive spatial reading order.
    }
};
void preflightText(PDFDocument& document, qint64& total, const std::function<bool()>& cancelled,
                   const std::function<void(QString, int, int)>& progress)
{
    struct CountedPage
    {
        PDFObject page, resources, contents;
        QRectF media, crop;
        double unit;
        PageRotation rotation;
        qint64 count;
    };
    QVector<CountedPage> cache;
    PDFFontCache fonts(128, 128);
    fonts.setDocument(PDFModifiedDocument(&document, nullptr));
    PDFCMSManager manager(nullptr);
    auto cms = manager.getCurrentCMS();
    for (int index = 0; index < int(document.getCatalog()->getPageCount()); ++index)
    {
        stop(cancelled);
        if (progress)
            progress("文字数の上限を確認しています…", 0, 0);
        const auto* page = document.getCatalog()->getPage(index);
        const auto object = document.getStorage().getObjectByReference(page->getPageReference());
        const auto cached = std::find_if(cache.begin(), cache.end(),
                                         [&](const CountedPage& entry)
                                         {
                                             return entry.page == object &&
                                                    entry.resources == page->getResources() &&
                                                    entry.contents == page->getContents() &&
                                                    entry.media == page->getMediaBox() &&
                                                    entry.crop == page->getCropBox() &&
                                                    entry.unit == page->getUserUnit() &&
                                                    entry.rotation == page->getPageRotation();
                                         });
        qint64 count;
        if (cached != cache.end())
            count = cached->count;
        else
        {
            CharacterCounter counter(PDFRenderer::getDefaultFeatures(), page, &document, &fonts,
                                     cms.data(), nullptr, {}, {});
            counter.maximum = std::min(qint64(200000), qint64(10000000) - total);
            counter.cancelled = cancelled;
            const auto errors = counter.processContents();
            for (const auto& error : errors)
                if (error.type != RenderErrorType::Information)
                    fail("文字を完全に解析できません。比較結果は表示しません。");
            count = counter.count;
            cache << CountedPage{object,
                                 page->getResources(),
                                 page->getContents(),
                                 page->getMediaBox(),
                                 page->getCropBox(),
                                 page->getUserUnit(),
                                 page->getPageRotation(),
                                 count};
        }
        total += count;
        if (total > 10000000)
            fail("抽出文字が比較上限を超えています。部分結果は表示しません。");
    }
}
double coordinate(double value)
{
    if (!std::isfinite(value))
        fail("比較できない座標が含まれています。");
    return std::round(value / coordinateTolerance) * coordinateTolerance;
}
QJsonArray rectangle(QRectF value)
{
    return {coordinate(value.left()), coordinate(value.top()), coordinate(value.right()),
            coordinate(value.bottom())};
}
QString bounded(QString text)
{
    if (text.size() > 65536)
        fail("文書情報または注釈の文字列が65536文字を超えています。");
    return text;
}
QJsonObject target(const NavigationTarget& value)
{
    const auto& d = value.destination;
    QJsonObject result{{"page", value.page},
                       {"notice", bounded(value.notice)},
                       {"type", int(d.getDestinationType())}};
    if (d.hasLeft())
        result["left"] = coordinate(d.getLeft());
    if (d.hasTop())
        result["top"] = coordinate(d.getTop());
    if (d.hasRight())
        result["right"] = coordinate(d.getRight());
    if (d.hasBottom())
        result["bottom"] = coordinate(d.getBottom());
    if (d.hasZoom())
        result["zoom"] = coordinate(d.getZoom());
    return result;
}
QJsonObject properties(const PDFDocument& document)
{
    qint64 propertyBytes = 0;
    auto account = [&](QJsonObject item)
    {
        propertyBytes += QJsonDocument(item).toJson(QJsonDocument::Compact).size();
        if (propertyBytes > 4 * 1024 * 1024)
            fail("文書情報が4MiBの比較上限を超えています。");
        return item;
    };
    const auto info = document.getInfo();
    QJsonObject result{{"title", bounded(info->title)},
                       {"author", bounded(info->author)},
                       {"subject", bounded(info->subject)},
                       {"keywords", bounded(info->keywords)},
                       {"creator", bounded(info->creator)},
                       {"producer", bounded(info->producer)},
                       {"creationDate", info->creationDate.toUTC().toString(Qt::ISODateWithMs)},
                       {"modifiedDate", info->modifiedDate.toUTC().toString(Qt::ISODateWithMs)}};
    QJsonArray bookmarks;
    account(result);
    const auto outlines = readBookmarks(document);
    if (outlines.limited)
        fail("しおりが比較上限を超えています。部分結果は表示しません。");
    for (const auto& item : outlines.items)
        bookmarks << account(QJsonObject{{"title", bounded(item.title)},
                                         {"parent", item.parent},
                                         {"expanded", item.expanded},
                                         {"target", target(item.target)}});
    result["bookmarks"] = bookmarks;
    QJsonObject extra;
    for (const auto& [key, value] : info->extra)
    {
        const auto text = bounded(value.toString());
        account(QJsonObject{{QString::fromLatin1(key), text}});
        extra[QString::fromLatin1(key)] = text;
    }
    result["extraInfo"] = extra;
    return result;
}
QJsonArray annotations(const PDFDocument& document, int page, qint64& semanticBytes,
                       const std::function<bool()>& cancelled)
{
    const auto object =
        document.getObjectByReference(document.getCatalog()->getPage(page)->getPageReference());
    const auto list = document.getObject(object.getDictionary()->get("Annots"));
    if (list.isNull())
        return {};
    if (!list.isArray() || list.getArray()->getCount() > 10000)
        fail("注釈の形式または件数が比較上限を超えています。");
    PDFDocumentDataLoaderDecorator loader(&document);
    QJsonArray result;
    for (const auto& reference : *list.getArray())
    {
        stop(cancelled);
        const auto annotation = document.getObject(reference);
        if (!annotation.isDictionary())
            fail("比較できない注釈が含まれています。");
        const auto dictionary = annotation.getDictionary();
        QJsonObject row{
            {"type", QString::fromLatin1(loader.readNameFromDictionary(dictionary, "Subtype"))},
            {"rect", rectangle(loader.readRectangle(dictionary->get("Rect"), {}))},
            {"flags", double(loader.readInteger(dictionary->get("F"), 0))}};
        for (auto key : {"Contents", "T", "Subj"})
            row[key] = bounded(loader.readTextString(dictionary->get(key), {}));
        const auto managed = document.getObject(dictionary->get("Tatsujin"));
        if (managed.isString())
        {
            if (managed.getString().size() > 1024 * 1024)
                fail("注釈の再編集情報が比較上限を超えています。");
            row["managed"] = QJsonDocument::fromJson(managed.getString()).object();
        }
        if (dictionary->hasKey("A") || dictionary->hasKey("Dest"))
        {
            const auto action = document.getObject(dictionary->get("A"));
            row["target"] = target(resolveActionObject(document, action, dictionary->get("Dest")));
            if (action.isDictionary())
                row["URI"] = bounded(loader.readTextString(action.getDictionary()->get("URI"), {}));
        }
        semanticBytes += QJsonDocument(row).toJson(QJsonDocument::Compact).size();
        if (semanticBytes > 32 * 1024 * 1024)
            fail("フォーム・注釈が32MiBの比較上限を超えています。");
        result << row;
    }
    return result;
}
struct PageMeasure
{
    QByteArray pixels, key;
    QSizeF physical;
    QRectF media, crop;
    double unit = 1;
    PageRotation rotation;
    QString text;
    QJsonArray forms, notes;
};
bool sameCoordinate(double a, double b)
{
    return std::abs(a - b) <= coordinateTolerance;
}
bool sameCoordinate(QRectF a, QRectF b)
{
    return sameCoordinate(a.left(), b.left()) && sameCoordinate(a.top(), b.top()) &&
           sameCoordinate(a.right(), b.right()) && sameCoordinate(a.bottom(), b.bottom());
}
bool sameGeometry(const PageMeasure& a, const PageMeasure& b)
{
    return sameCoordinate(a.media, b.media) && sameCoordinate(a.crop, b.crop) &&
           sameCoordinate(a.unit, b.unit) && a.rotation == b.rotation &&
           sameCoordinate(a.physical.width(), b.physical.width()) &&
           sameCoordinate(a.physical.height(), b.physical.height());
}
QByteArray pixelHash(const QImage& image)
{
    QCryptographicHash hash(QCryptographicHash::Sha256);
    hash.addData(QByteArray::number(image.width()) + ":" + QByteArray::number(image.height()) +
                 ":");
    hash.addData(
        QByteArrayView(reinterpret_cast<const char*>(image.constBits()), image.sizeInBytes()));
    return hash.result();
}
QVector<PageMeasure> measure(PDFDocument& document, qint64& characters, qint64& semanticBytes,
                             const std::function<bool()>& cancelled,
                             const std::function<void(QString, int, int)>& progress, int offset,
                             int total)
{
    const auto fields = formFields(document);
    if (fields.size() > 10000)
        fail("フォームの件数が比較上限を超えています。");
    QVector<PageMeasure> result;
    for (int index = 0; index < int(document.getCatalog()->getPageCount()); ++index)
    {
        stop(cancelled);
        if (progress)
            progress("ページの表示・文字・入力値を比較しています…", offset + index, total);
        const auto page = document.getCatalog()->getPage(index);
        PageMeasure row;
        row.physical = pageSize(page);
        row.media = page->getMediaBox();
        row.crop = page->getCropBox();
        row.unit = page->getUserUnit();
        row.rotation = page->getPageRotation();
        row.text = pageText(document, index);
        characters += row.text.size();
        if (row.text.size() > 200000 || characters > 10000000)
            fail("抽出文字が比較上限を超えています。部分結果は表示しません。");
        for (const auto& field : fields)
            if (field.page == index && field.kind != FormKind::Unsupported)
            {
                QJsonArray values;
                for (const auto& value : field.values)
                    values << bounded(value);
                const QJsonObject item{{"name", bounded(field.qualifiedName)},
                                       {"kind", int(field.kind)},
                                       {"values", values},
                                       {"rect", rectangle(field.rectangle)}};
                semanticBytes += QJsonDocument(item).toJson(QJsonDocument::Compact).size();
                if (semanticBytes > 32 * 1024 * 1024)
                    fail("フォーム・注釈が32MiBの比較上限を超えています。");
                row.forms << item;
            }
        row.notes = annotations(document, index, semanticBytes, cancelled);
        row.pixels = pixelHash(renderComparisonPage(document, index));
        stop(cancelled);
        const auto content =
            QJsonDocument(
                QJsonObject{{"text", row.text}, {"forms", row.forms}, {"annotations", row.notes}})
                .toJson(QJsonDocument::Compact);
        row.key = QCryptographicHash::hash(row.pixels + content, QCryptographicHash::Sha256);
        result << std::move(row);
    }
    return result;
}
QRect changedBounds(const QImage& left, const QImage& right, const std::function<bool()>& cancelled)
{
    if (left.size() != right.size())
        return QRect(QPoint(), left.size().expandedTo(right.size()));
    int x0 = left.width(), y0 = left.height(), x1 = -1, y1 = -1;
    for (int y = 0; y < left.height(); ++y)
    {
        if (y % 64 == 0)
            stop(cancelled);
        const auto a = reinterpret_cast<const QRgb*>(left.constScanLine(y)),
                   b = reinterpret_cast<const QRgb*>(right.constScanLine(y));
        for (int x = 0; x < left.width(); ++x)
            if (a[x] != b[x])
            {
                x0 = std::min(x0, x);
                x1 = std::max(x1, x);
                y0 = std::min(y0, y);
                y1 = y;
            }
    }
    return x1 < 0 ? QRect() : QRect(QPoint(x0, y0), QPoint(x1, y1));
}
} // namespace
bool ComparedPage::changed() const
{
    return left < 0 || right < 0 || geometryChanged || textChanged || pixelsChanged ||
           formsChanged || annotationsChanged;
}
QString ComparedPage::description() const
{
    if (left < 0)
        return "右に追加";
    if (right < 0)
        return "右で削除";
    QStringList kinds;
    if (geometryChanged)
        kinds << "寸法・表示範囲・回転";
    if (textChanged)
        kinds << "文字";
    if (pixelsChanged)
        kinds << "見た目";
    if (formsChanged)
        kinds << "フォーム値";
    if (annotationsChanged)
        kinds << "注釈・リンク";
    return kinds.isEmpty() ? QString("比較範囲に差なし") : kinds.join("・");
}
int ComparisonResult::changedPages() const
{
    return int(std::count_if(pages.begin(), pages.end(),
                             [](const ComparedPage& row) { return row.changed(); }));
}
bool ComparisonResult::same() const
{
    return !changedPages() && propertyChanges.isEmpty();
}
QImage renderComparisonPage(PDFDocument& document, int page)
{
    const auto size = pageSize(document.getCatalog()->getPage(page));
    if (!std::isfinite(size.width()) || !std::isfinite(size.height()) || size.width() < 1 ||
        size.height() < 1 || std::ceil(size.width()) * std::ceil(size.height()) > 16000000)
        fail("比較するページの描画サイズが1600万画素の上限を超えているか、不正です。");
    QStringList diagnostics;
    auto image = renderPage(document, page, 1, true, true, RenderPurpose::View, &diagnostics);
    if (!diagnostics.isEmpty())
        fail("ページを完全に描画できません。比較結果は表示しません。");
    return image;
}
ComparisonResult compareDocuments(PDFDocument left, PDFDocument right,
                                  const ComparisonOptions& options,
                                  const std::function<bool()>& cancelled,
                                  const std::function<void(QString, int, int)>& progress)
{
    stop(cancelled);
    for (const auto* document : {&left, &right})
    {
        if (!document->getCatalog() || !document->getCatalog()->getPageCount() ||
            document->getCatalog()->getPageCount() > 500)
            fail("比較するPDFは1〜500ページで指定してください。");
        if (!document->getStorage().getSecurityHandler()->isAllowed(
                PDFSecurityHandler::Permission::CopyContent))
            fail("文字や画像のコピーが許可されていないPDFは比較できません。");
    }
    ComparisonResult result;
    result.options = options;
    result.leftPages = int(left.getCatalog()->getPageCount());
    result.rightPages = int(right.getCatalog()->getPageCount());
    result.leftProperties = properties(left);
    result.rightProperties = properties(right);
    for (const auto& key : result.leftProperties.keys())
        if (result.leftProperties[key] != result.rightProperties[key])
            result.propertyChanges << key;
    qint64 minimumCharacters = 0;
    preflightText(left, minimumCharacters, cancelled, progress);
    preflightText(right, minimumCharacters, cancelled, progress);
    qint64 characters = 0, semanticBytes = 0;
    const auto a = measure(left, characters, semanticBytes, cancelled, progress, 0,
                           result.leftPages + result.rightPages);
    const auto b = measure(right, characters, semanticBytes, cancelled, progress, result.leftPages,
                           result.leftPages + result.rightPages);
    auto append = [&](int i, int j)
    {
        stop(cancelled);
        if (progress)
            progress(QString("変更箇所を照合しています… %1対").arg(result.pages.size() + 1), 0, 0);
        ComparedPage row;
        row.left = i;
        row.right = j;
        if (i >= 0)
        {
            row.leftPixels = a[i].pixels;
            row.leftText = a[i].text;
            row.leftForms = a[i].forms;
            row.leftAnnotations = a[i].notes;
        }
        if (j >= 0)
        {
            row.rightPixels = b[j].pixels;
            row.rightText = b[j].text;
            row.rightForms = b[j].forms;
            row.rightAnnotations = b[j].notes;
        }
        if (i >= 0 && j >= 0)
        {
            row.geometryChanged = !sameGeometry(a[i], b[j]);
            row.textChanged = a[i].text != b[j].text;
            row.pixelsChanged = a[i].pixels != b[j].pixels;
            row.formsChanged = a[i].forms != b[j].forms;
            row.annotationsChanged = a[i].notes != b[j].notes;
            if (row.pixelsChanged)
                row.pixelBounds = changedBounds(renderComparisonPage(left, i),
                                                renderComparisonPage(right, j), cancelled);
        }
        result.pages << std::move(row);
    };
    if (!options.matchCommonPages)
    {
        for (int i = 0; i < std::max(a.size(), b.size()); ++i)
            append(i < a.size() ? i : -1, i < b.size() ? i : -1);
    }
    else
    {
        const int n = int(a.size()), m = int(b.size()), stride = m + 1;
        std::vector<quint16> lengths(size_t(n + 1) * size_t(m + 1), 0);
        auto at = [&](int i, int j) -> quint16& { return lengths[size_t(i) * stride + j]; };
        auto common = [&](int i, int j)
        { return a[i].key == b[j].key && sameGeometry(a[i], b[j]); };
        for (int i = n - 1; i >= 0; --i)
        {
            stop(cancelled);
            for (int j = m - 1; j >= 0; --j)
                at(i, j) = common(i, j) ? quint16(at(i + 1, j + 1) + 1)
                                        : std::max(at(i + 1, j), at(i, j + 1));
        }
        int i = 0, j = 0, previousLeft = 0, previousRight = 0;
        auto gap = [&](int toLeft, int toRight)
        {
            while (previousLeft < toLeft || previousRight < toRight)
                append(previousLeft < toLeft ? previousLeft++ : -1,
                       previousRight < toRight ? previousRight++ : -1);
        };
        while (i < n && j < m)
        {
            if (common(i, j))
            {
                gap(i, j);
                append(i, j);
                previousLeft = ++i;
                previousRight = ++j;
            }
            else if (at(i + 1, j) >= at(i, j + 1))
                ++i;
            else
                ++j;
        }
        gap(n, m);
    }
    stop(cancelled);
    if (progress)
        progress("比較が完了しました。", result.leftPages + result.rightPages,
                 result.leftPages + result.rightPages);
    return result;
}
QJsonObject comparisonJson(const ComparisonResult& result)
{
    QJsonArray pages;
    for (const auto& row : result.pages)
        pages << QJsonObject{
            {"leftPage", row.left + 1},
            {"rightPage", row.right + 1},
            {"changed", row.changed()},
            {"geometryChanged", row.geometryChanged},
            {"textChanged", row.textChanged},
            {"pixelsChanged", row.pixelsChanged},
            {"formsChanged", row.formsChanged},
            {"annotationsChanged", row.annotationsChanged},
            {"pixelBounds", QJsonArray{row.pixelBounds.x(), row.pixelBounds.y(),
                                       row.pixelBounds.width(), row.pixelBounds.height()}},
            {"leftPixelSHA256", QString::fromLatin1(row.leftPixels.toHex())},
            {"rightPixelSHA256", QString::fromLatin1(row.rightPixels.toHex())},
            {"leftText", row.leftText},
            {"rightText", row.rightText},
            {"leftForms", row.leftForms},
            {"rightForms", row.rightForms},
            {"leftAnnotations", row.leftAnnotations},
            {"rightAnnotations", row.rightAnnotations}};
    return {{"format", "PDFTatsujin-comparison"},
            {"version", 1},
            {"dpi", 72},
            {"coordinateTolerancePoints", coordinateTolerance},
            {"matching", result.options.matchCommonPages ? "common-pages" : "page-number"},
            {"leftPages", result.leftPages},
            {"rightPages", result.rightPages},
            {"sameInComparedScope", result.same()},
            {"changedPages", result.changedPages()},
            {"pages", pages},
            {"leftProperties", result.leftProperties},
            {"rightProperties", result.rightProperties},
            {"propertyChanges", QJsonArray::fromStringList(result.propertyChanges)},
            {"excluded", "Attachments, scripts, tag structure, layer visibility settings, XFA and "
                         "unsupported or widgetless "
                         "fields; not a byte or print-resolution identity guarantee"}};
}
void exportComparison(const ComparisonResult& result, const QString& destination,
                      const std::function<bool()>& cancelled)
{
    stop(cancelled);
    if (destination.isEmpty() || !destination.endsWith(".json", Qt::CaseInsensitive) ||
        QFileInfo::exists(destination) || !QFileInfo(destination).dir().exists())
        fail("まだ使われていない.jsonの出力先を指定してください。");
    const auto object = comparisonJson(result);
    const auto data = QJsonDocument(object).toJson();
    if (data.size() > 64 * 1024 * 1024)
        fail("比較結果のJSONが64MiBを超えています。");
    SaveCandidate staged(QFileInfo(destination).absolutePath());
    QFile file(staged.filePath());
    if (!file.open(QIODevice::WriteOnly | QIODevice::NewOnly) || file.write(data) != data.size() ||
        !file.flush())
        fail("比較結果を書き込めません。");
    file.close();
    stop(cancelled);
    QFile check(staged.filePath());
    if (!check.open(QIODevice::ReadOnly) ||
        QJsonDocument::fromJson(check.readAll()).object() != object)
        fail("比較結果を検証できません。");
    check.close();
    stop(cancelled);
    const auto from = extendedWindowsPath(staged.filePath()),
               to = extendedWindowsPath(QFileInfo(destination).absoluteFilePath());
    if (!MoveFileExW(reinterpret_cast<LPCWSTR>(from.utf16()), reinterpret_cast<LPCWSTR>(to.utf16()),
                     MOVEFILE_WRITE_THROUGH))
        fail(QString("比較結果の確定に失敗しました（Windows %1）。既存ファイルは保持しています。")
                 .arg(GetLastError()));
}
} // namespace tatsu
