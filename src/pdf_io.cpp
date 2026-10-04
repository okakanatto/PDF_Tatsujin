#include "document.h"
#include "pdfdocumentreader.h"
#include "pdfdocumentwriter.h"

namespace tatsu
{
using namespace pdf;
void fail(const QString& text)
{
    throw std::runtime_error(text.toUtf8().constData());
}
bool sameFilePath(const QString& left, const QString& right)
{
    if (left.isEmpty() || right.isEmpty())
        return false;
    auto normalized = [](const QString& path)
    {
        const QFileInfo info(path);
        const auto canonical = info.canonicalFilePath();
        return canonical.isEmpty() ? QDir::cleanPath(info.absoluteFilePath()) : canonical;
    };
    // Windows paths may address the same file with different case or through
    // directory aliases. Keep the original conflict baseline in either case.
    return normalized(left).compare(normalized(right), Qt::CaseInsensitive) == 0;
}
QByteArray fileHash(const QString& path)
{
    QFile f(path);
    if (!f.exists())
        return {};
    if (!f.open(QIODevice::ReadOnly))
        fail("ファイルを読み取れません: " + f.errorString());
    QCryptographicHash h(QCryptographicHash::Sha256);
    if (!h.addData(&f))
        fail("ファイル照合に失敗しました。");
    return h.result();
}
PDFDocument readPdf(const QString& path, const QString& password)
{
    int attempts = 0;
    PDFDocumentReader reader(
        nullptr,
        [&](bool* ok)
        {
            *ok = !password.isEmpty() && attempts++ == 0;
            return password;
        },
        false, false);
    auto doc = reader.readFromFile(path);
    if (reader.getReadingResult() != PDFDocumentReader::Result::OK ||
        !doc.getCatalog()->getPageCount())
        fail("PDFを開けません。パスワードまたは文書を確認してください。 " +
             reader.getErrorMessage());
    return doc;
}
QByteArray encodePdf(const PDFDocument& doc)
{
    QBuffer b;
    b.open(QIODevice::WriteOnly);
    PDFDocumentWriter w(nullptr);
    auto r = w.write(&b, &doc);
    if (!r)
        fail("PDF出力に失敗しました。");
    return b.data();
}
void writeCandidate(const PDFDocument& doc, const QString& path)
{
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate))
        fail(f.errorString());
    auto data = encodePdf(doc);
    if (f.write(data) != data.size() || !f.flush())
        fail(f.errorString());
    f.close();
    auto check = readPdf(path);
    if (check.getCatalog()->getPageCount() != doc.getCatalog()->getPageCount())
        fail("保存候補のページ数が一致しません。");
}
} // namespace tatsu
