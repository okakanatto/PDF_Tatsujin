#include "private_temp.h"
#include "table_extraction.h"
#include "windows_path.h"
#include <QXmlStreamWriter>
#include <QtCore/private/qzipwriter_p.h>
#include <windows.h>

namespace tatsu
{
namespace
{
void stop(const std::function<bool()>& cancelled)
{
    if (cancelled && cancelled())
        fail("表の取り出しを取り消しました。");
}
QString columnName(int index)
{
    QString result;
    for (++index; index; index = (index - 1) / 26)
        result.prepend(QChar('A' + (index - 1) % 26));
    return result;
}
QByteArray worksheet(const TableCells& cells)
{
    QByteArray bytes;
    QXmlStreamWriter xml(&bytes);
    xml.writeStartDocument();
    xml.writeStartElement("worksheet");
    xml.writeDefaultNamespace("http://schemas.openxmlformats.org/spreadsheetml/2006/main");
    xml.writeStartElement("cols");
    xml.writeEmptyElement("col");
    xml.writeAttribute("min", "1");
    xml.writeAttribute("max", QString::number(cells.first().size()));
    xml.writeAttribute("width", "20");
    xml.writeAttribute("customWidth", "1");
    xml.writeEndElement();
    xml.writeStartElement("sheetData");
    qsizetype characters = 0;
    for (int row = 0; row < cells.size(); ++row)
    {
        xml.writeStartElement("row");
        xml.writeAttribute("r", QString::number(row + 1));
        int lines = 1;
        for (const auto& text : cells[row])
            lines = qMax(lines, int(text.count('\n') + 1));
        xml.writeAttribute("ht", QString::number(lines * 17 + 10));
        xml.writeAttribute("customHeight", "1");
        for (int column = 0; column < cells[row].size(); ++column)
        {
            const auto& text = cells[row][column];
            characters += text.size();
            if (text.size() > 32767 || characters > 1000000)
                fail("Excelのセル文字数の上限を超えています。");
            for (int i = 0; i < text.size(); ++i)
            {
                const auto value = text[i].unicode();
                if ((value < 32 && value != 9 && value != 10 && value != 13) || value == 0xFFFE ||
                    value == 0xFFFF || QChar::isLowSurrogate(value))
                    fail("Excelへ保存できない文字が含まれています。");
                if (QChar::isHighSurrogate(value))
                {
                    if (i + 1 == text.size() || !text[i + 1].isLowSurrogate())
                        fail("Excelへ保存できない文字が含まれています。");
                    ++i;
                }
            }
            xml.writeStartElement("c");
            xml.writeAttribute("r", columnName(column) + QString::number(row + 1));
            xml.writeAttribute("t", "inlineStr");
            xml.writeStartElement("is");
            xml.writeStartElement("t");
            xml.writeAttribute("xml:space", "preserve");
            xml.writeCharacters(text);
            xml.writeEndElement();
            xml.writeEndElement();
            xml.writeEndElement();
        }
        xml.writeEndElement();
    }
    xml.writeEndElement();
    xml.writeEndElement();
    xml.writeEndDocument();
    if (xml.hasError())
        fail("表のExcelデータを作成できません。");
    return bytes;
}
} // namespace
void exportTableXlsx(const TableCells& cells, const QString& path,
                     const std::function<bool()>& cancelled)
{
    stop(cancelled);
    if (cells.isEmpty() || cells.size() > 100 || cells.first().isEmpty() ||
        cells.first().size() > 100)
        fail("表は1〜100行・列を指定してください。");
    for (const auto& row : cells)
        if (row.size() != cells.first().size())
            fail("表の列数が一致しません。");
    const auto destination = QFileInfo(path);
    if (destination.suffix().compare("xlsx", Qt::CaseInsensitive) != 0 || destination.exists() ||
        destination.isSymLink())
        fail("新しい.xlsxファイルを指定してください。既存ファイルは上書きしません。");
    auto temporary =
        privateTemporaryDirectory(destination.absolutePath() + "/PDFTatsujin-table-XXXXXX");
    if (!temporary->isValid())
        fail("Excel出力の作業フォルダを作成できません。");
    const auto candidate = temporary->filePath("table.xlsx");
    QZipWriter zip(candidate);
    zip.addFile(
        "[Content_Types].xml",
        R"xml(<?xml version="1.0" encoding="UTF-8"?><Types xmlns="http://schemas.openxmlformats.org/package/2006/content-types"><Default Extension="rels" ContentType="application/vnd.openxmlformats-package.relationships+xml"/><Default Extension="xml" ContentType="application/xml"/><Override PartName="/xl/workbook.xml" ContentType="application/vnd.openxmlformats-officedocument.spreadsheetml.sheet.main+xml"/><Override PartName="/xl/worksheets/sheet1.xml" ContentType="application/vnd.openxmlformats-officedocument.spreadsheetml.worksheet+xml"/><Override PartName="/xl/styles.xml" ContentType="application/vnd.openxmlformats-officedocument.spreadsheetml.styles+xml"/></Types>)xml");
    zip.addFile(
        "_rels/.rels",
        R"xml(<?xml version="1.0" encoding="UTF-8"?><Relationships xmlns="http://schemas.openxmlformats.org/package/2006/relationships"><Relationship Id="rId1" Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/officeDocument" Target="xl/workbook.xml"/></Relationships>)xml");
    zip.addFile(
        "xl/workbook.xml",
        R"xml(<?xml version="1.0" encoding="UTF-8"?><workbook xmlns="http://schemas.openxmlformats.org/spreadsheetml/2006/main" xmlns:r="http://schemas.openxmlformats.org/officeDocument/2006/relationships"><sheets><sheet name="取り出した表" sheetId="1" r:id="rId1"/></sheets></workbook>)xml");
    zip.addFile(
        "xl/_rels/workbook.xml.rels",
        R"xml(<?xml version="1.0" encoding="UTF-8"?><Relationships xmlns="http://schemas.openxmlformats.org/package/2006/relationships"><Relationship Id="rId1" Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/worksheet" Target="worksheets/sheet1.xml"/><Relationship Id="rId2" Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/styles" Target="styles.xml"/></Relationships>)xml");
    zip.addFile(
        "xl/styles.xml",
        R"xml(<?xml version="1.0" encoding="UTF-8"?><styleSheet xmlns="http://schemas.openxmlformats.org/spreadsheetml/2006/main"><fonts count="1"><font><sz val="11"/><name val="Calibri"/><family val="2"/></font></fonts><fills count="2"><fill><patternFill patternType="none"/></fill><fill><patternFill patternType="gray125"/></fill></fills><borders count="1"><border><left/><right/><top/><bottom/><diagonal/></border></borders><cellStyleXfs count="1"><xf numFmtId="0" fontId="0" fillId="0" borderId="0"/></cellStyleXfs><cellXfs count="1"><xf numFmtId="49" fontId="0" fillId="0" borderId="0" xfId="0" applyNumberFormat="1" applyAlignment="1"><alignment wrapText="1" vertical="top"/></xf></cellXfs><cellStyles count="1"><cellStyle name="Normal" xfId="0" builtinId="0"/></cellStyles></styleSheet>)xml");
    zip.addFile("xl/worksheets/sheet1.xml", worksheet(cells));
    zip.close();
    if (zip.status() != QZipWriter::NoError)
        fail("表のExcelファイルを作成できません。");
    stop(cancelled);
    const auto from = extendedWindowsPath(candidate),
               to = extendedWindowsPath(destination.absoluteFilePath());
    if (!MoveFileExW(reinterpret_cast<LPCWSTR>(from.utf16()), reinterpret_cast<LPCWSTR>(to.utf16()),
                     MOVEFILE_WRITE_THROUGH))
        fail(QString("Excelファイルを確定できません（Windows %1）。既存ファイルは保持します。")
                 .arg(GetLastError()));
}
} // namespace tatsu
