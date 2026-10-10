#include "office_import.h"
#include "owned_process.h"
#include "private_temp.h"
#include <QCoreApplication>
#include <QTemporaryDir>
#include <QUrl>
#include <QXmlStreamReader>
#include <QtCore/private/qzipreader_p.h>
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
QByteArray validatedDocx(const QString& path, const std::function<bool()>& cancelled)
{
    checkCancel(cancelled);
    QFile source(path);
    if (!source.open(QIODevice::ReadOnly) || source.size() <= 0 || source.size() > 64 * 1024 * 1024)
        fail("DOCXを読み取れません。入力は64MiBまでです。");
    const auto snapshot = source.readAll();
    if (snapshot.size() != source.size())
        fail("DOCXの読み取りが完了しませんでした。");
    QBuffer buffer;
    buffer.setData(snapshot);
    buffer.open(QIODevice::ReadOnly);
    QZipReader zip(&buffer);
    const auto entries = zip.fileInfoList();
    if (!zip.isReadable() || zip.status() != QZipReader::NoError || entries.isEmpty() ||
        entries.size() > 4096)
        fail("有効なDOCXではありません。ZIP内の項目は4096個までです。");
    QSet<QString> seen;
    qint64 expanded = 0;
    bool document = false, contentType = false;
    for (const auto& entry : entries)
    {
        checkCancel(cancelled);
        const auto name = entry.filePath;
        const auto lower = name.toLower();
        if (!entry.isValid() || entry.isSymLink || name.startsWith('/') || name.contains('\\') ||
            name.contains(':') || name.split('/').contains("..") || seen.contains(lower) ||
            entry.size < 0 || entry.size > 256 * 1024 * 1024)
            fail("DOCXに不正なZIP項目があります。");
        seen.insert(lower);
        expanded += entry.size;
        if (expanded > 256 * 1024 * 1024)
            fail("DOCXの展開内容は合計256MiBまでです。");
        if (lower.contains("vbaproject") || lower.startsWith("word/activex/") ||
            lower.startsWith("word/embeddings/"))
            fail("マクロ・ActiveX・埋込みオブジェクトを含む文書は変換できません。");
        if (!entry.isFile)
            continue;
        const bool xmlPart = lower.endsWith(".xml") || lower.endsWith(".rels");
        if (xmlPart && entry.size > 16 * 1024 * 1024)
            fail("DOCXのXML項目は16MiBまでです。");
        const auto data = zip.fileData(name);
        if (zip.status() != QZipReader::NoError || data.size() != entry.size ||
            crc32(0, reinterpret_cast<const Bytef*>(data.constData()), data.size()) != entry.crc)
            fail("DOCXのZIP項目が破損または暗号化されています。");
        if (name == "word/document.xml")
            document = true;
        if (!xmlPart)
            continue;
        QXmlStreamReader xml(data);
        while (!xml.atEnd())
        {
            xml.readNext();
            if (xml.isDTD() || xml.isEntityReference())
                fail("DOCXのDTD・外部実体は利用できません。");
            if (!xml.isStartElement())
                continue;
            const auto attributes = xml.attributes();
            if (lower == "[content_types].xml" && xml.name() == "Override")
            {
                const auto type = attributes.value("ContentType").toString();
                if (type.contains("macroEnabled", Qt::CaseInsensitive))
                    fail("マクロ有効文書は変換できません。");
                if (attributes.value("PartName") == "/word/document.xml" &&
                    type == "application/"
                            "vnd.openxmlformats-officedocument.wordprocessingml.document.main+xml")
                    contentType = true;
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
        }
        if (xml.hasError())
            fail("DOCXのXMLが破損しています。");
    }
    if (!document || !contentType)
        fail("WordのDOCX本文がありません。");
    checkCancel(cancelled);
    return snapshot;
}
PDFDocument importDocx(const QString& path, const QString& converter,
                       const std::function<bool()>& cancelled, bool suppressAsianSpacing)
{
    const auto input = validatedDocx(path, cancelled);
    if (!QFileInfo(converter).isFile())
        fail("DOCX変換エンジンが見つかりません。LibreOfficeの設定を確認してください。");
    auto temporary = privateTemporaryDirectory(QDir::tempPath() + "/PDFTatsujin-office-XXXXXX");
    if (!temporary->isValid())
        fail("変換用の一時フォルダを作成できません。");
    const auto root = temporary->path();
    QDir dir(root);
    if (!dir.mkpath("profile/user") || !dir.mkdir("output"))
        fail("変換用の一時フォルダを作成できません。");
    writeFile(root + "/input.docx", input);
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
    const auto result =
        runOwnedProcess(python,
                        {asset("office/import_docx.py"), QFileInfo(converter).absoluteFilePath(),
                         root + "/input.docx", root + "/output/input.pdf", root + "/profile",
                         suppressAsianSpacing ? "true" : "false"},
                        root, 120000, cancelled);
    checkCancel(cancelled);
    if (result.exitCode != 0 || !QFileInfo::exists(root + "/output/input.pdf"))
        fail("DOCXからPDFへの変換に失敗しました。 " + QString::fromUtf8(result.output));
    auto candidate = readPdf(root + "/output/input.pdf");
    checkCancel(cancelled);
    return candidate;
}
} // namespace tatsu
