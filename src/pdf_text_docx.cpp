#include "pdf_text_docx.h"
#include "office_package.h"
#include "pdfsecurityhandler.h"
#include <QXmlStreamWriter>

namespace tatsu
{
namespace
{
void beginParagraph(QXmlStreamWriter& xml)
{
    xml.writeStartElement("w:p");
    xml.writeStartElement("w:pPr");
    for (const auto& property : {"w:autoSpaceDE", "w:autoSpaceDN"})
    {
        xml.writeEmptyElement(property);
        xml.writeAttribute("w:val", "0");
    }
    xml.writeEndElement();
}
void validateText(const QString& text)
{
    for (int i = 0; i < text.size(); ++i)
    {
        const auto character = text[i];
        if (character.isHighSurrogate())
        {
            if (++i >= text.size() || !text[i].isLowSurrogate())
                fail("Word本文に不正な文字があります。");
        }
        else if (character.isLowSurrogate() || character.unicode() == 0xfffe ||
                 character.unicode() == 0xffff ||
                 (character.unicode() < 0x20 && character != '\n' && character != '\r' &&
                  character != '\t'))
            fail("Word本文に保存できない制御文字があります。");
    }
}
} // namespace
QStringList extractWordText(PDFDocument document, const QVector<int>& pages,
                            const std::function<bool()>& cancelled)
{
    if (!document.getStorage().getSecurityHandler()->isAllowed(
            PDFSecurityHandler::Permission::CopyContent))
        fail("このPDFでは本文のコピーが許可されていません。");
    if (pages.isEmpty() || pages.size() > 100)
        fail("Wordへ取り出すページは1〜100ページを指定してください。");
    QStringList result;
    qsizetype count = 0;
    for (const int page : pages)
    {
        if (cancelled && cancelled())
            fail("本文の取り出しを取り消しました。");
        if (page < 0 || page >= int(document.getCatalog()->getPageCount()))
            fail("本文のページがありません。");
        auto text = pageText(document, page);
        text.replace("\r\n", "\n").replace('\r', '\n');
        // PDFTextFlow appends a layout separator after the page's final line.
        // Word's explicit page boundary replaces that single separator. Keep
        // internal empty lines and whitespace in the actual extracted content.
        if (text.endsWith('\n'))
            text.chop(1);
        if (text.trimmed().isEmpty())
            fail(
                QString("%1ページに取り出せる文字がありません。画像のページは先にOCRしてください。")
                    .arg(page + 1));
        validateText(text);
        if ((count += text.size()) > 1000000)
            fail("Wordへ取り出す本文は100万文字までです。");
        result << text;
    }
    return result;
}
void exportWordText(const QStringList& pages, const QString& path, const QString& fontFamily,
                    const std::function<bool()>& cancelled)
{
    if (pages.isEmpty() || pages.size() > 100 || fontFamily.trimmed().isEmpty() ||
        fontFamily.size() > 128)
        fail("Word本文と書体を確認してください。");
    validateText(fontFamily);
    QByteArray content;
    QXmlStreamWriter xml(&content);
    xml.writeStartDocument();
    xml.writeStartElement("w:document");
    xml.writeNamespace("http://schemas.openxmlformats.org/wordprocessingml/2006/main", "w");
    xml.writeStartElement("w:body");
    qsizetype count = 0;
    for (int page = 0; page < pages.size(); ++page)
    {
        if (cancelled && cancelled())
            fail("Word本文の出力を取り消しました。");
        validateText(pages[page]);
        if (pages[page].trimmed().isEmpty() || (count += pages[page].size()) > 1000000)
            fail("Word本文は空でない100万文字以内の内容を指定してください。");
        if (page)
        {
            beginParagraph(xml);
            xml.writeStartElement("w:r");
            xml.writeEmptyElement("w:br");
            xml.writeAttribute("w:type", "page");
            xml.writeEndElement();
            xml.writeEndElement();
        }
        for (const auto& line : pages[page].split('\n', Qt::KeepEmptyParts))
        {
            beginParagraph(xml);
            xml.writeStartElement("w:r");
            xml.writeStartElement("w:rPr");
            xml.writeEmptyElement("w:rFonts");
            for (const auto& script : {"w:ascii", "w:hAnsi", "w:eastAsia", "w:cs"})
                xml.writeAttribute(script, fontFamily);
            xml.writeEmptyElement("w:sz");
            xml.writeAttribute("w:val", "22");
            xml.writeEndElement();
            const auto segments = line.split('\t', Qt::KeepEmptyParts);
            for (int segment = 0; segment < segments.size(); ++segment)
            {
                if (segment)
                    xml.writeEmptyElement("w:tab");
                xml.writeStartElement("w:t");
                xml.writeAttribute("xml:space", "preserve");
                xml.writeCharacters(segments[segment]);
                xml.writeEndElement();
            }
            xml.writeEndElement();
            xml.writeEndElement();
        }
    }
    xml.writeStartElement("w:sectPr");
    xml.writeEmptyElement("w:pgSz");
    xml.writeAttribute("w:w", "11906");
    xml.writeAttribute("w:h", "16838");
    xml.writeEmptyElement("w:pgMar");
    for (const auto& side : {"w:top", "w:right", "w:bottom", "w:left"})
        xml.writeAttribute(side, "1134");
    xml.writeEndElement();
    xml.writeEndElement();
    xml.writeEndElement();
    xml.writeEndDocument();
    if (xml.hasError())
        fail("Word本文をXMLへ記録できません。");
    writeOfficePackage(
        {{"[Content_Types].xml",
          R"xml(<?xml version="1.0" encoding="UTF-8"?><Types xmlns="http://schemas.openxmlformats.org/package/2006/content-types"><Default Extension="rels" ContentType="application/vnd.openxmlformats-package.relationships+xml"/><Default Extension="xml" ContentType="application/xml"/><Override PartName="/word/document.xml" ContentType="application/vnd.openxmlformats-officedocument.wordprocessingml.document.main+xml"/></Types>)xml"},
         {"_rels/.rels",
          R"xml(<?xml version="1.0" encoding="UTF-8"?><Relationships xmlns="http://schemas.openxmlformats.org/package/2006/relationships"><Relationship Id="rId1" Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/officeDocument" Target="word/document.xml"/></Relationships>)xml"},
         {"word/document.xml", content}},
        path, "docx", cancelled);
}
} // namespace tatsu
