#include "form_data.h"
#include "pdfform.h"
#include "pdfsecurityhandler.h"
#include "save_candidate.h"
#include "windows_path.h"
#include <QXmlStreamReader>
#include <QXmlStreamWriter>
#include <windows.h>

namespace tatsu
{
namespace
{
constexpr qsizetype byteLimit = 4 * 1024 * 1024;
constexpr int fieldLimit = 500, depthLimit = 16;
const QString space = "http://ns.adobe.com/xfdf/";
void stop(const std::function<bool()>& cancelled)
{
    if (cancelled && cancelled())
        fail("フォーム入力データの処理を中止しました。文書は変更していません。");
}
void xmlText(const QString& text)
{
    if (text.toUtf8().size() > 65536 || QString::fromUtf8(text.toUtf8()) != text)
        fail("入力データの文字または長さが不正です。");
    for (uint value : text.toUcs4())
        if (!(value == 9 || value == 10 || value == 13 || (value >= 0x20 && value <= 0xd7ff) ||
              (value >= 0xe000 && value <= 0xfffd) || (value >= 0x10000 && value <= 0x10ffff)))
            fail("XMLへ保存できない文字が含まれています。");
}
void valuesValid(const QVector<FormDataValue>& values)
{
    if (values.isEmpty() || values.size() > fieldLimit)
        fail("入力データは1〜500項目で指定してください。");
    QSet<QString> seen;
    qsizetype size = 0;
    for (const auto& field : values)
    {
        xmlText(field.name);
        if (field.name.isEmpty() || field.name.toUtf8().size() > 2048 ||
            seen.contains(field.name) || field.values.size() > fieldLimit)
            fail("入力データの項目名・重複・値の数が不正です。");
        seen.insert(field.name);
        size += field.name.toUtf8().size();
        for (const auto& value : field.values)
        {
            xmlText(value);
            size += value.toUtf8().size();
        }
        if (size > byteLimit)
            fail("入力データが4MiBの処理上限を超えています。");
    }
}
QMap<QString, QVector<FormField>> groups(const PDFDocument& document)
{
    QMap<QString, QVector<FormField>> result;
    for (const auto& field : formFields(document))
    {
        const auto& name = field.qualifiedName;
        if (name.isEmpty())
            fail("完全修飾名を持たないフォーム項目には対応していません。");
        auto& group = result[name];
        if (!group.isEmpty() && group[0].field != field.field)
            fail("同じ完全修飾名を持つ別項目を区別できません。");
        group << field;
        if (result.size() > fieldLimit)
            fail("フォームが500項目の処理上限を超えています。");
    }
    return result;
}
void element(const QXmlStreamReader& reader, const QString& name)
{
    if (reader.name() != name || reader.namespaceUri() != space)
        fail("対応しないXFDF要素が含まれています。");
}
void readField(QXmlStreamReader& reader, QString prefix, int depth, QVector<FormDataValue>& output)
{
    element(reader, "field");
    if (depth > depthLimit || !reader.attributes().hasAttribute("name") ||
        reader.attributes().size() != 1)
        fail("XFDFの項目名または階層が不正です。");
    const auto local = reader.attributes().value("name").toString();
    if (local.isEmpty())
        fail("XFDFの項目名が空です。");
    const auto name = prefix.isEmpty() ? local : prefix + "." + local;
    QStringList values;
    bool children = false;
    while (reader.readNextStartElement())
    {
        if (reader.name() == "field")
        {
            if (!values.isEmpty())
                fail("同じXFDF項目に値と子項目は指定できません。");
            children = true;
            readField(reader, name, depth + 1, output);
        }
        else
        {
            element(reader, "value");
            if (children || !reader.attributes().isEmpty() || values.size() >= fieldLimit)
                fail("XFDFの項目値が不正です。");
            values << reader.readElementText(QXmlStreamReader::ErrorOnUnexpectedElement);
        }
    }
    if (!children)
    {
        output << FormDataValue{name, values};
        if (output.size() > fieldLimit)
            fail("XFDFが500項目の処理上限を超えています。");
    }
}
} // namespace
FormDataValues formDataValues(const PDFDocument& document)
{
    if (!document.getCatalog() || !document.getStorage().getSecurityHandler())
        fail("PDFを開いてください。");
    if (!document.getStorage().getSecurityHandler()->isAllowed(
            PDFSecurityHandler::Permission::CopyContent))
        fail("この文書では入力値のコピーが許可されていません。");
    auto form = PDFForm::parse(&document, document.getCatalog()->getFormObject());
    if (!form.isAcroForm())
        fail("標準AcroFormの入力値だけを扱います。");
    const auto fields = groups(document);
    FormDataValues result;
    for (auto it = fields.cbegin(); it != fields.cend(); ++it)
    {
        const auto& field = it.value()[0];
        if (field.kind == FormKind::Unsupported)
            result.unsupported << it.key();
        else
            result.fields << FormDataValue{it.key(), field.values};
    }
    std::function<void(const PDFFormFields&, int)> hidden =
        [&](const PDFFormFields& nodes, int depth)
    {
        if (depth > 32)
            fail("PDFフォームの階層が処理上限を超えています。");
        for (const auto& node : nodes)
        {
            if (node->getChildFields().empty())
            {
                const auto name = node->getName(PDFFormField::FullyQualified);
                if (!fields.contains(name) && !result.unsupported.contains(name))
                    result.unsupported << name;
            }
            else
                hidden(node->getChildFields(), depth + 1);
        }
    };
    hidden(form.getFormFields(), 0);
    if (result.fields.isEmpty())
        fail("対応する入力欄がありません。署名・XFA・表示欄のない項目は対象外です。");
    valuesValid(result.fields);
    return result;
}
QByteArray encodeXfdf(const QVector<FormDataValue>& values)
{
    valuesValid(values);
    QByteArray output;
    QXmlStreamWriter writer(&output);
    writer.setAutoFormatting(true);
    writer.writeStartDocument();
    writer.writeStartElement("xfdf");
    writer.writeDefaultNamespace(space);
    writer.writeAttribute("xml:space", "preserve");
    writer.writeStartElement("fields");
    for (const auto& field : values)
    {
        writer.writeStartElement("field");
        writer.writeAttribute("name", field.name);
        for (const auto& value : field.values)
        {
            writer.writeStartElement("value");
            // XML normalizes literal CR. Preserve it through a character reference.
            auto parts = value.split('\r');
            for (int i = 0; i < parts.size(); ++i)
            {
                if (i)
                    writer.writeEntityReference("#xD");
                writer.writeCharacters(parts[i]);
            }
            writer.writeEndElement();
        }
        writer.writeEndElement();
    }
    writer.writeEndElement();
    writer.writeEndElement();
    writer.writeEndDocument();
    if (writer.hasError() || output.size() > byteLimit)
        fail("XFDFの出力に失敗したか、4MiBの処理上限を超えています。");
    if (decodeXfdf(output) != values)
        fail("XFDFの入力値の一致を確認できません。");
    return output;
}
QVector<FormDataValue> decodeXfdf(const QByteArray& data)
{
    if (data.isEmpty() || data.size() > byteLimit)
        fail("XFDFは4MiB以内の入力データで指定してください。");
    QXmlStreamReader scan(data);
    while (!scan.atEnd())
    {
        scan.readNext();
        if (scan.isDTD() || scan.isEntityReference())
            fail("DTD・エンティティを使うXFDFは読み込みません。");
    }
    if (scan.hasError())
        fail("XFDFのXML形式が不正です。");
    QXmlStreamReader reader(data);
    if (!reader.readNextStartElement())
        fail("XFDFがありません。");
    element(reader, "xfdf");
    QVector<FormDataValue> output;
    bool hasFields = false;
    while (reader.readNextStartElement())
    {
        if (reader.name() == "fields")
        {
            element(reader, "fields");
            if (hasFields || !reader.attributes().isEmpty())
                fail("XFDFのfieldsは1つだけ指定してください。");
            hasFields = true;
            while (reader.readNextStartElement())
                readField(reader, {}, 1, output);
        }
        else if (reader.name() == "f" || reader.name() == "ids")
        {
            element(reader, reader.name().toString());
            // Reference information is inert. Never open its file/URL.
            if (reader.readNextStartElement())
                fail("XFDFの参照情報に変更要素を指定できません。");
        }
        else
            fail("入力値以外のXFDF変更要素には対応していません。");
    }
    if (reader.hasError() || !hasFields)
        fail("XFDFの入力値を確認できません。");
    valuesValid(output);
    return output;
}
FormDataImport importFormData(const PDFDocument& document, const QVector<FormDataValue>& values,
                              const std::function<bool()>& cancelled,
                              const std::function<void(int, int)>& progress)
{
    stop(cancelled);
    if (!document.getCatalog() || !document.getStorage().getSecurityHandler())
        fail("PDFを開いてください。");
    const auto restriction = editingRestriction(document);
    if (!restriction.isEmpty())
        fail(restriction);
    valuesValid(values);
    const auto fields = groups(document);
    FormDataImport result{document, {}};
    Document candidate;
    candidate.history = {document};
    int index = 0;
    for (const auto& value : values)
    {
        stop(cancelled);
        if (!fields.contains(value.name))
            fail("このPDFに対応する入力欄がない項目が含まれています。");
        const auto& group = fields[value.name];
        auto selected = group[0];
        auto data = value.values;
        if ((selected.kind == FormKind::Checkbox || selected.kind == FormKind::Radio) &&
            data.size() > 1)
            fail("チェック・ラジオ項目は1つの値で指定してください。");
        if (data.isEmpty() && selected.kind != FormKind::List)
            data << (selected.kind == FormKind::Checkbox || selected.kind == FormKind::Radio ? "Off"
                                                                                             : "");
        if (selected.kind == FormKind::Unsupported)
            fail("対応しないフォーム項目が含まれています。");
        if (selected.values != data)
        {
            if (selected.readOnly)
                fail("読取専用の項目へ値は読み込めません。");
            if (selected.kind == FormKind::Radio)
                for (const auto& radio : group)
                    if (radio.onState == data.value(0))
                        selected = radio;
            putFormValue(candidate, selected.widget, data);
            result.changes << FormDataChange{value.name, selected.name, selected.values, data};
            // Retain one private snapshot rather than a history per imported field.
            auto current = candidate.pdf();
            candidate.history = {current};
            candidate.cursor = candidate.saved = 0;
        }
        if (progress)
            progress(++index, values.size());
    }
    stop(cancelled);
    result.document = candidate.pdf();
    return result;
}
void exportFormData(const PDFDocument& document, const QString& destination,
                    const std::function<bool()>& cancelled)
{
    stop(cancelled);
    if (destination.isEmpty() || !destination.endsWith(".xfdf", Qt::CaseInsensitive) ||
        QFileInfo::exists(destination) || !QFileInfo(destination).dir().exists())
        fail("既存ファイルを使わず、新しい.xfdfの保存先を指定してください。");
    const auto values = formDataValues(document).fields;
    const auto data = encodeXfdf(values);
    SaveCandidate staged(QFileInfo(destination).absolutePath());
    QFile file(staged.filePath());
    if (!file.open(QIODevice::WriteOnly | QIODevice::NewOnly) || file.write(data) != data.size() ||
        !file.flush())
        fail("フォーム入力データを書き込めません。");
    file.close();
    stop(cancelled);
    if (!file.open(QIODevice::ReadOnly) || decodeXfdf(file.readAll()) != values)
        fail("保存候補の入力値を検証できません。");
    file.close();
    stop(cancelled);
    const auto from = extendedWindowsPath(staged.filePath()),
               to = extendedWindowsPath(QFileInfo(destination).absoluteFilePath());
    if (!MoveFileExW(reinterpret_cast<LPCWSTR>(from.utf16()), reinterpret_cast<LPCWSTR>(to.utf16()),
                     MOVEFILE_WRITE_THROUGH))
        fail("入力データを確定できません。既存ファイルと文書は変更していません。");
}
} // namespace tatsu
