#pragma once
#include "pdfdocument.h"
#include "pdftextlayout.h"
#include <QPrinter>
#include <QtCore>
#include <QtGui>
#include <stdexcept>

namespace tatsu
{
using namespace pdf;
class PdfPasswordRequired final : public std::runtime_error
{
public:
    using std::runtime_error::runtime_error;
};
[[noreturn]] void fail(const QString& text);
QByteArray fileHash(const QString& path);
bool sameFilePath(const QString& left, const QString& right);
QString asset(const QString& relative);
QString signatureFont();
PDFDocument correctFontUnicode(const PDFDocument& document, const QString& text,
                               const QRawFont& font);
PDFDocument readPdf(const QString& path, const QString& password = {});
QByteArray encodePdf(const PDFDocument& doc);
void writeCandidate(const PDFDocument& doc, const QString& path);
QTransform pageMatrix(const PDFPage* page, double scale = 1, bool rotate = true);
QSizeF pageSize(const PDFPage* page, bool rotate = true);
enum class RenderPurpose
{
    View,
    Print
};
QImage renderPage(PDFDocument& doc, int page, double scale, bool annotations = true,
                  bool rotate = true, RenderPurpose purpose = RenderPurpose::View);
PDFTextLayout textLayout(PDFDocument& doc, int page, const QTransform& matrix = {});
QString pageText(PDFDocument& doc, int page);
void printDocument(PDFDocument& doc, QPrinter& printer, int currentPage = 0);
struct Signature
{
    PDFObjectReference ref;
    QRectF rect;
    QString text;
    double size = 20;
    QColor color = Qt::black;
};
QVector<Signature> signatures(const PDFDocument& doc, int page);
class Document
{
public:
    std::vector<PDFDocument> history;
    int cursor = 0, saved = 0;
    quint64 revision = 0;
    QString source, target, readOnly;
    QByteArray sourceHash, targetHash;
    bool busy = false;
    bool copyAllowed = true;
    bool loaded() const
    {
        return !history.empty();
    }
    bool dirty() const
    {
        return loaded() && cursor != saved;
    }
    PDFDocument& pdf()
    {
        return history.at(cursor);
    }
    const PDFDocument& pdf() const
    {
        return history.at(cursor);
    }
    int pages() const
    {
        return loaded() ? int(pdf().getCatalog()->getPageCount()) : 0;
    }
    void open(const QString& path, const QString& password = {});
    void editable() const;
    void commit(PDFDocument doc);
    void undo();
    void redo();
    Signature putSignature(int page, const QString& text, QPointF point, double size, QColor color,
                           PDFObjectReference old = {});
    void moveSignature(int page, const Signature& sig, QPointF delta);
    void eraseSignature(int page, const Signature& sig);
    void rotate(int page);
    void save(const QString& path, const QByteArray& expected = {});
};
QVector<int> parsePages(QString text, int count);
} // namespace tatsu
