#include "document.h"
#include "form_design.h"
#include "form_fields.h"
#include <QGuiApplication>

// Optional owned diagnostic. The explicit re-edit mode creates only new output files.
int main(int argc, char** argv)
{
    QGuiApplication app(argc, argv);
    QJsonObject result{{"scope", "PDF4QT form-font diagnostic"}};
    const auto arguments = app.arguments();
    const bool reedit = arguments.size() == 4 && arguments[3] == "--reedit-forms";
    if (arguments.size() != 3 && !reedit)
        return 2;
    const QString input = arguments[1], output = arguments[2];
    try
    {
        if (QFileInfo::exists(output) || !QDir().mkpath(output))
            tatsu::fail("Cannot create owned diagnostic directory");
        auto pdf = tatsu::readPdf(input);
        QStringList errors;
        const auto image =
            tatsu::renderPage(pdf, 0, 1.5, true, true, tatsu::RenderPurpose::View, &errors);
        if (!image.save(output + "/sdk-view.png"))
            tatsu::fail("Cannot save render diagnostic");
        const auto print =
            tatsu::renderPage(pdf, 0, 1.5, true, true, tatsu::RenderPurpose::Print, &errors);
        if (!print.save(output + "/sdk-print.png"))
            tatsu::fail("Cannot save print diagnostic");
        QJsonArray fields;
        for (const auto& field : tatsu::formFields(pdf))
            fields.append(QJsonObject{{"name", field.qualifiedName},
                                      {"values", QJsonArray::fromStringList(field.values)}});
        result["body_text"] = tatsu::pageText(pdf, 0);
        result["fields"] = fields;
        result["render_errors"] = QJsonArray::fromStringList(errors);
        result["input_sha256"] = QString::fromLatin1(tatsu::fileHash(input).toHex());
        result["status"] = "EXECUTED";
        if (reedit)
        {
            tatsu::Document document;
            document.open(input);
            const auto sourceHash = tatsu::fileHash(input);
            const auto before = document.pdf();
            auto entries = tatsu::formDesign(before);
            if (entries.size() != 6 || !errors.isEmpty())
                tatsu::fail("Expected six app-created fields and successful SDK rendering");
            bool changed = false;
            for (auto& entry : entries)
                if (entry.name == "designed-1")
                {
                    if (entry.values != QStringList{"山田 太郎 髙橋 𠮷野"})
                        tatsu::fail("External Japanese and supplementary value was not preserved");
                    entry.caption = "外部入力後の再設計";
                    entry.widgets[0].rectangle.translate(12, 8);
                    changed = true;
                }
            if (!changed)
                tatsu::fail("Expected external-filled field");
            document.commit(tatsu::replaceFormDesign(document.pdf(), entries));
            document.undo();
            if (document.pdf() != before)
                tatsu::fail("Re-design Undo failed");
            document.redo();
            const auto candidate = document.pdf();
            document.save(output + "/external-reedited.pdf");
            document.open(output + "/external-reedited.pdf");
            if (tatsu::formDesign(document.pdf()) != tatsu::formDesign(candidate) ||
                tatsu::fileHash(input) != sourceHash)
                tatsu::fail("Saved external-input re-design was not preserved");
            tatsu::FormField text;
            bool found = false;
            for (const auto& field : tatsu::formFields(document.pdf()))
                if (field.qualifiedName == "designed-1")
                {
                    text = field;
                    found = true;
                }
            if (!found)
                tatsu::fail("Missing re-designed input field");
            tatsu::putFormValue(document, text.widget, {"外部入力後に再入力 髙橋 𠮷野"});
            document.save(output + "/external-reinput.pdf");
            auto saved = tatsu::readPdf(output + "/external-reinput.pdf");
            bool correct = false;
            for (const auto& field : tatsu::formFields(saved))
                if (field.qualifiedName == "designed-1")
                    correct = field.values == QStringList{"外部入力後に再入力 髙橋 𠮷野"};
            if (!correct || tatsu::fileHash(input) != sourceHash)
                tatsu::fail("Saved re-input or original preservation failed");
            result["status"] = "PASS";
            result["external_input_reedit_undo_redo_save_reinput"] = true;
            result["six_field_definitions_and_values_preserved"] = true;
        }
    }
    catch (const std::exception& error)
    {
        result["status"] = "FAIL";
        result["error"] = QString::fromUtf8(error.what());
    }
    QFile file(output + "/result.json");
    if (!file.open(QIODevice::WriteOnly) || file.write(QJsonDocument(result).toJson()) < 0)
        return 3;
    return result["status"] == "FAIL" ? 1 : 0;
}
