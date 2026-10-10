#include "office_import.h"
#include "office_pdf_compat.h"
#include "owned_process.h"
#include "private_temp.h"
#include <QCoreApplication>
#include <QTemporaryDir>
#include <QUrl>
#include <QXmlStreamReader>
#include <QtCore/private/qzipreader_p.h>
#include <optional>
#include <zlib.h>

namespace tatsu
{
namespace
{
void checkCancel(const std::function<bool()>& cancelled)
{
    if (cancelled && cancelled())
        fail("変換を取り消しました。");
}
void writeFile(const QString& path, const QByteArray& bytes)
{
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::NewOnly) ||
        file.write(bytes) != bytes.size() || !file.flush())
        fail("変換用の一時ファイルを書き込めません。");
}
} // namespace
QString officeConverterPath()
{
    const auto configured = qEnvironmentVariable("TATSU_OFFICE_CONVERTER");
    if (!configured.isEmpty())
        return QFileInfo(configured).absoluteFilePath();
    const auto saved = QSettings().value("office/converter").toString();
    if (!saved.isEmpty())
        return saved;
    return QCoreApplication::applicationDirPath() + "/office/LibreOffice/program/soffice.com";
}
OfficeInput validatedOffice(const QString& path, const std::function<bool()>& cancelled)
{
    checkCancel(cancelled);
    QFile source(path);
    if (!source.open(QIODevice::ReadOnly) || source.size() <= 0 || source.size() > 64 * 1024 * 1024)
        fail("Office文書を読み取れません。入力は64MiBまでです。");
    const auto snapshot = source.readAll();
    if (snapshot.size() != source.size())
        fail("Office文書の読み取りが完了しませんでした。");
    QBuffer buffer;
    buffer.setData(snapshot);
    buffer.open(QIODevice::ReadOnly);
    QZipReader zip(&buffer);
    const auto entries = zip.fileInfoList();
    if (!zip.isReadable() || zip.status() != QZipReader::NoError || entries.isEmpty() ||
        entries.size() > 4096)
        fail("有効なOffice文書ではありません。ZIP内の項目は4096個までです。");
    QSet<QString> seen;
    qint64 expanded = 0;
    QSet<QString> parts;
    std::optional<OfficeKind> kind;
    QString mainPart, suffix;
    for (const auto& entry : entries)
    {
        checkCancel(cancelled);
        const auto name = entry.filePath;
        const auto lower = name.toLower();
        if (!entry.isValid() || entry.isSymLink || name.startsWith('/') || name.contains('\\') ||
            name.contains(':') || name.split('/').contains("..") || seen.contains(lower) ||
            entry.size < 0 || entry.size > 256 * 1024 * 1024)
            fail("Office文書に不正なZIP項目があります。");
        seen.insert(lower);
        expanded += entry.size;
        if (expanded > 256 * 1024 * 1024)
            fail("Office文書の展開内容は合計256MiBまでです。");
        if (lower.contains("vbaproject") || lower.contains("/activex/") ||
            lower.contains("/embeddings/"))
            fail("マクロ・ActiveX・埋込みオブジェクトを含む文書は変換できません。");
        if (lower.startsWith("xl/externallinks/") || lower == "xl/connections.xml")
            fail("外部ブック・データ接続を含む表は変換できません。");
        if (!entry.isFile)
            continue;
        const bool xmlPart = lower.endsWith(".xml") || lower.endsWith(".rels");
        if (xmlPart && entry.size > 16 * 1024 * 1024)
            fail("Office文書のXML項目は16MiBまでです。");
        const auto data = zip.fileData(name);
        if (zip.status() != QZipReader::NoError || data.size() != entry.size ||
            crc32(0, reinterpret_cast<const Bytef*>(data.constData()), data.size()) != entry.crc)
            fail("Office文書のZIP項目が破損または暗号化されています。");
        parts.insert(name);
        if (!xmlPart)
            continue;
        QXmlStreamReader xml(data);
        while (!xml.atEnd())
        {
            xml.readNext();
            if (xml.isDTD() || xml.isEntityReference())
                fail("Office文書のDTD・外部実体は利用できません。");
            if (!xml.isStartElement())
                continue;
            const auto attributes = xml.attributes();
            if (lower == "[content_types].xml" && xml.name() == "Override")
            {
                const auto type = attributes.value("ContentType").toString();
                if (type.contains("macroEnabled", Qt::CaseInsensitive))
                    fail("マクロ有効文書は変換できません。");
                const auto declared = attributes.value("PartName").toString();
                std::optional<OfficeKind> found;
                QString extension;
                if (declared == "/word/document.xml" &&
                    type == "application/"
                            "vnd.openxmlformats-officedocument.wordprocessingml.document.main+xml")
                {
                    found = OfficeKind::Document;
                    extension = "docx";
                }
                else if (declared == "/xl/workbook.xml" &&
                         type == "application/"
                                 "vnd.openxmlformats-officedocument.spreadsheetml.sheet.main+xml")
                {
                    found = OfficeKind::Spreadsheet;
                    extension = "xlsx";
                }
                else if (declared == "/ppt/presentation.xml" &&
                         type == "application/"
                                 "vnd.openxmlformats-officedocument.presentationml.presentation."
                                 "main+xml")
                {
                    found = OfficeKind::Presentation;
                    extension = "pptx";
                }
                if (found)
                {
                    if (kind)
                        fail("Office文書に複数の本文形式があります。");
                    kind = found;
                    mainPart = declared.mid(1);
                    suffix = extension;
                }
            }
            if (lower.endsWith(".rels") && xml.name() == "Relationship" &&
                attributes.value("TargetMode")
                        .toString()
                        .compare("External", Qt::CaseInsensitive) == 0 &&
                !attributes.value("Type").toString().endsWith("/hyperlink"))
                fail("画像などを外部から取得する文書は変換できません。");
            if (lower == "word/document.xml" &&
                (xml.name() == "altChunk" || xml.name() == "object" || xml.name() == "OLEObject"))
                fail("外部内容・埋込みオブジェクトは変換できません。");
            if (lower.startsWith("xl/") && xml.name() == "f")
            {
                const auto formula = xml.readElementText();
                static const QRegularExpression external(
                    R"((?:^|[^A-Za-z0-9_])(WEBSERVICE|DDE|RTD|CALL|REGISTER|IMAGE|STOCKHISTORY)\s*\()",
                    QRegularExpression::CaseInsensitiveOption);
                if (formula.contains('[') || external.match(formula).hasMatch())
                    fail("外部処理の数式を含む表は変換できません。");
            }
        }
        if (xml.hasError())
            fail("Office文書のXMLが破損しています。");
    }
    if (!kind || !parts.contains(mainPart) || QFileInfo(path).suffix().toLower() != suffix)
        fail("Office文書の本文形式と拡張子が一致しません。");
    checkCancel(cancelled);
    return {snapshot, *kind};
}
QByteArray validatedDocx(const QString& path, const std::function<bool()>& cancelled)
{
    auto input = validatedOffice(path, cancelled);
    if (input.kind != OfficeKind::Document)
        fail("WordのDOCX本文がありません。");
    return std::move(input.bytes);
}
PDFDocument importOffice(const QString& path, const QString& converter,
                         const std::function<bool()>& cancelled, bool suppressAsianSpacing)
{
    const auto input = validatedOffice(path, cancelled);
    if (suppressAsianSpacing && input.kind != OfficeKind::Document)
        fail("字間抑制はWord文書で利用できます。");
    if (!QFileInfo(converter).isFile())
        fail("Office変換エンジンが見つかりません。LibreOfficeの設定を確認してください。");
    auto temporary = privateTemporaryDirectory(QDir::tempPath() + "/PDFTatsujin-office-XXXXXX");
    if (!temporary->isValid())
        fail("変換用の一時フォルダを作成できません。");
    const auto root = temporary->path();
    QDir dir(root);
    if (!dir.mkpath("profile/user") || !dir.mkdir("output"))
        fail("変換用の一時フォルダを作成できません。");
    const auto suffix = input.kind == OfficeKind::Document      ? "docx"
                        : input.kind == OfficeKind::Spreadsheet ? "xlsx"
                                                                : "pptx";
    const auto source = root + "/input." + suffix;
    writeFile(source, input.bytes);
    writeFile(root + "/profile/user/registrymodifications.xcu",
              R"xml(<?xml version="1.0" encoding="UTF-8"?>
<oor:items xmlns:oor="http://openoffice.org/2001/registry">
<item oor:path="/org.openoffice.Office.Common/Security/Scripting"><prop oor:name="MacroSecurityLevel" oor:op="fuse"><value>3</value></prop></item>
<item oor:path="/org.openoffice.Office.Common/Misc"><prop oor:name="FirstRun" oor:op="fuse"><value>false</value></prop></item>
<item oor:path="/org.openoffice.Office.Jobs/Jobs/org.openoffice.Office.Jobs:Job['UpdateCheck']/Arguments"><prop oor:name="AutoCheckEnabled" oor:op="fuse"><value>false</value></prop></item>
</oor:items>)xml");
    const auto python = QFileInfo(converter).absolutePath() + "/python.exe";
    if (!QFileInfo(python).isFile())
        fail("LibreOfficeのPython実行環境が見つかりません。");
    const auto result = runOwnedProcess(python,
                                        {asset("office/import_document.py"),
                                         QFileInfo(converter).absoluteFilePath(), source,
                                         root + "/output/input.pdf", root + "/profile",
                                         suppressAsianSpacing ? "true" : "false", suffix},
                                        root, 120000, cancelled);
    checkCancel(cancelled);
    if (result.exitCode != 0 || !QFileInfo::exists(root + "/output/input.pdf"))
        fail("Office文書からPDFへの変換に失敗しました。 " + QString::fromUtf8(result.output));
    auto candidate =
        normalizeOfficeClosingAdjustments(readPdf(root + "/output/input.pdf"), cancelled);
    checkCancel(cancelled);
    return candidate;
}
PDFDocument importDocx(const QString& path, const QString& converter,
                       const std::function<bool()>& cancelled, bool suppressAsianSpacing)
{
    if (QFileInfo(path).suffix().compare("docx", Qt::CaseInsensitive) != 0)
        fail("WordのDOCXを選んでください。");
    return importOffice(path, converter, cancelled, suppressAsianSpacing);
}
} // namespace tatsu
