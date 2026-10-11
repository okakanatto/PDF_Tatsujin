#include "form_tests.h"
#include "form_fields.h"
#include "pdf_objects.h"
#include "pdfdocumentbuilder.h"
#include "window.h"
#include <QInputMethodEvent>
#include <QtTest/QTest>

namespace tatsu
{
namespace
{
void check(bool value, const QString& message)
{
    if (!value)
        fail(message);
}
FormField named(const Document& document, const QString& name, QString state = {})
{
    for (const auto& field : formFields(document.pdf()))
        if (field.name == name && (state.isEmpty() || field.onState == state))
            return field;
    fail("form field not found: " + name);
}
} // namespace
QJsonObject testFormValues(const QString& fixtures, const QString& output)
{
    Document document;
    document.open(fixtures + "/D07.pdf");
    const auto sourceHash = fileHash(document.source);
    document.putSignature(0, "署名は保持", {60, 50}, 16, Qt::black);
    const QString japanese = "髙橋 香織";
    const QString multiline = "東京都\n申請内容の追記";
    putFormValue(document, named(document, "name").widget, {japanese});
    putFormValue(document, named(document, "notes").widget, {multiline});
    putFormValue(document, named(document, "agree").widget, {"Off"});
    auto radio = named(document, "choice", "B");
    putFormValue(document, radio.widget, {"B"});
    putFormValue(document, named(document, "combo").widget, {"Red"});
    const auto beforeList = document.pdf();
    putFormValue(document, named(document, "list").widget, {"West"});
    document.undo();
    check(document.pdf() == beforeList, "form edit is one Undo unit");
    document.redo();
    document.save(output + "/form-input.pdf");
    auto image = renderPage(document.pdf(), 0, 1.5);
    image.save(output + "/form-input.png");
    Document reopened;
    reopened.open(output + "/form-input.pdf");
    check(renderPage(reopened.pdf(), 0, 1.5) == image,
          "saved form appearance matches including embedded Japanese");
    check(named(reopened, "name").values == QStringList{japanese}, "Japanese field value retained");
    check(named(reopened, "notes").values == QStringList{multiline}, "multiline value retained");
    check(named(reopened, "agree").values == QStringList{"Off"}, "checkbox value retained");
    check(named(reopened, "choice", "B").values == QStringList{"B"}, "radio group value retained");
    check(named(reopened, "combo").values == QStringList{"Red"} &&
              named(reopened, "list").values == QStringList{"West"},
          "combo and list values retained");
    check(signatures(reopened.pdf(), 0).at(0).text == "署名は保持",
          "form editing preserves existing signature");
    const auto before = reopened.pdf();
    bool refused = false;
    try
    {
        putFormValue(reopened, named(reopened, "combo").widget, {"invalid choice"});
    }
    catch (const std::exception&)
    {
        refused = true;
    }
    check(refused && reopened.pdf() == before, "invalid choice rejected before document mutation");
    refused = false;
    const char32_t unsupported = 0x10ffff;
    try
    {
        putFormValue(reopened, named(reopened, "name").widget,
                     {QString::fromUcs4(&unsupported, 1)});
    }
    catch (const std::exception&)
    {
        refused = true;
    }
    check(refused && reopened.pdf() == before,
          "unsupported glyph rejected without silent substitution or partial commit");
    putFormValue(reopened, named(reopened, "name").widget, {"再編集した氏名"});
    reopened.save(output + "/form-reedited.pdf");
    QPrinter printer(QPrinter::HighResolution);
    printer.setOutputFormat(QPrinter::PdfFormat);
    printer.setOutputFileName(output + "/form-print.pdf");
    printDocument(document.pdf(), printer);
    check(fileHash(document.source) == sourceHash, "form input protects original");
    return {{"field_types", 6},
            {"Japanese_AP_embedded", true},
            {"save_reedit", true},
            {"undo", true},
            {"invalid_input_atomic", true}};
}
QJsonObject testFormInput(const QString& fixtures, const QString& output)
{
    Window window;
    window.show();
    window.openFile(fixtures + "/D07.pdf");
    check(QTest::qWaitFor([&] { return window.canvas->pageReady(0); }, 10000),
          "real form page ready");
    auto field = named(window.doc, "name");
    const auto point = window.canvas->pdfToViewport(0, field.rectangle.center()).toPoint();
    QTest::mouseClick(window.canvas->viewport(), Qt::LeftButton, Qt::NoModifier, point);
    auto editor = window.canvas->viewport()->findChild<QLineEdit*>("activeFormEditor");
    check(editor && editor->isVisible(), "native Qt single-line input over actual PDF field");
    editor->selectAll();
    QInputMethodEvent input;
    input.setCommitString("髙橋 香織");
    QApplication::sendEvent(editor, &input);
    check(editor->text() == "髙橋 香織" && window.doc.dirty() && window.doc.pendingInput,
          "committed Japanese input marks unsaved draft");
    const auto beforeDraftUndo = window.doc.cursor;
    QTest::keyClick(editor, Qt::Key_Z, Qt::ControlModifier);
    check(window.doc.cursor == beforeDraftUndo && editor->text() == "山田 太郎",
          "Ctrl+Z in a live form edits the local draft without committing the document");
    QTest::keyClick(editor, Qt::Key_Z, Qt::ControlModifier | Qt::ShiftModifier);
    check(editor->text() == "髙橋 香織" && window.doc.cursor == beforeDraftUndo,
          "Ctrl+Shift+Z restores local field input");
    QTest::keyClick(editor, Qt::Key_Tab);
    check(named(window.doc, "name").values == QStringList{"髙橋 香織"},
          "Tab commits previous field");
    auto multiline = window.canvas->viewport()->findChild<QPlainTextEdit*>("activeFormEditor");
    check(multiline && multiline->isVisible() && multiline->accessibleName() == "notes",
          "Tab reaches multiline field");
    multiline->setPlainText("日本語のフォーム\n複数行入力");
    window.canvas->finishFormEdit();
    check(!window.doc.pendingInput &&
              named(window.doc, "notes").values == QStringList{"日本語のフォーム\n複数行入力"},
          "multiline edit committed");
    const auto cursor = window.doc.cursor;
    window.undoAction->trigger();
    check(window.doc.cursor == cursor - 1 &&
              named(window.doc, "name").values == QStringList{"髙橋 香織"},
          "toolbar Undo preserves previous form field");
    window.redoAction->trigger();
    window.doc.save(output + "/form-ui.pdf");
    check(QTest::qWaitFor([&] { return window.canvas->pageReady(0); }, 10000),
          "edited PDF appearance ready");
    window.grab().save(output + "/form-ui.png");
    window.openFile(output + "/form-ui.pdf");
    check(QTest::qWaitFor([&] { return window.canvas->pageReady(0); }, 10000),
          "reopened form ready");
    field = named(window.doc, "name");
    QTest::mouseClick(window.canvas->viewport(), Qt::LeftButton, Qt::NoModifier,
                      window.canvas->pdfToViewport(0, field.rectangle.center()).toPoint());
    editor = window.canvas->viewport()->findChild<QLineEdit*>("activeFormEditor");
    check(editor && editor->text() == "髙橋 香織", "reopened form editable with Japanese value");
    editor->setText("取り消す下書き");
    QTest::keyClick(editor, Qt::Key_Escape);
    check(!window.doc.dirty() && named(window.doc, "name").values == QStringList{"髙橋 香織"},
          "Esc discards only uncommitted field draft");
    return {{"Qt_input_method_event", true},
            {"native_OS_IME", "未実行"},
            {"Tab_navigation", true},
            {"draft_dirty", true},
            {"save_reopen_edit", true}};
}
QJsonObject testFormKeyboard(const QString& fixtures, const QString& output)
{
    Window window;
    window.show();
    window.openFile(fixtures + "/D07.pdf");
    check(QTest::qWaitFor([&] { return window.canvas->pageReady(0); }, 10000), "form page ready");
    QTest::keyClick(window.canvas->viewport(), Qt::Key_Tab);
    auto line = window.canvas->viewport()->findChild<QLineEdit*>("activeFormEditor");
    check(line && line->accessibleName() == "name", "Tab enters first form from PDF viewer");
    QTest::keyClick(line, Qt::Key_Tab);
    auto multi = window.canvas->viewport()->findChild<QPlainTextEdit*>("activeFormEditor");
    check(multi, "keyboard reaches multiline");
    QTest::keyClick(multi, Qt::Key_Tab);
    auto checkBox = window.canvas->viewport()->findChild<QCheckBox*>("activeFormEditor");
    check(checkBox, "keyboard reaches checkbox");
    QTest::keyClick(checkBox, Qt::Key_Space);
    check(named(window.doc, "agree").values == QStringList{"Off"}, "Space toggles checkbox");
    // Return to the checkbox and Tab through the unselected radio without changing the group.
    auto fields = formFields(window.doc.pdf());
    auto agree = named(window.doc, "agree");
    QTest::mouseClick(
        window.canvas->viewport(), Qt::LeftButton, Qt::NoModifier,
        window.canvas->pdfToViewport(0, named(window.doc, "notes").rectangle.center()).toPoint());
    window.canvas->finishFormEdit();
    QTest::keyClick(window.canvas->viewport(), Qt::Key_Tab);
    line = window.canvas->viewport()->findChild<QLineEdit*>("activeFormEditor");
    check(line, "viewer Tab starts standard field sequence");
    QTest::keyClick(line, Qt::Key_Tab);
    multi = window.canvas->viewport()->findChild<QPlainTextEdit*>("activeFormEditor");
    QTest::keyClick(multi, Qt::Key_Tab);
    checkBox = window.canvas->viewport()->findChild<QCheckBox*>("activeFormEditor");
    QTest::keyClick(checkBox, Qt::Key_Tab);
    auto radio = window.canvas->viewport()->findChild<QRadioButton*>("activeFormEditor");
    check(radio, "Tab reaches first radio");
    QTest::keyClick(radio, Qt::Key_Tab);
    radio = window.canvas->viewport()->findChild<QRadioButton*>("activeFormEditor");
    check(radio, "Tab reaches second radio");
    QTest::keyClick(radio, Qt::Key_Space);
    check(named(window.doc, "choice", "B").values == QStringList{"B"},
          "Space selects second radio");
    QTest::keyClick(window.canvas->viewport(), Qt::Key_Tab);
    line = window.canvas->viewport()->findChild<QLineEdit*>("activeFormEditor");
    QTest::keyClick(line, Qt::Key_Tab);
    multi = window.canvas->viewport()->findChild<QPlainTextEdit*>("activeFormEditor");
    QTest::keyClick(multi, Qt::Key_Tab);
    checkBox = window.canvas->viewport()->findChild<QCheckBox*>("activeFormEditor");
    QTest::keyClick(checkBox, Qt::Key_Tab);
    radio = window.canvas->viewport()->findChild<QRadioButton*>("activeFormEditor");
    QTest::keyClick(radio, Qt::Key_Tab);
    radio = window.canvas->viewport()->findChild<QRadioButton*>("activeFormEditor");
    QTest::keyClick(radio, Qt::Key_Tab);
    check(named(window.doc, "choice", "B").values == QStringList{"B"},
          "Tab past unselected radio never clears group");
    auto combo = window.canvas->viewport()->findChild<QComboBox*>("activeFormEditor");
    check(combo, "keyboard reaches combo");
    QTest::keyClick(combo, Qt::Key_Home);
    QTest::keyClick(combo, Qt::Key_Tab);
    auto list = window.canvas->viewport()->findChild<QListWidget*>("activeFormEditor");
    check(list && named(window.doc, "combo").values == QStringList{"Red"},
          "Tab commits combo and reaches list");
    QTest::keyClick(list, Qt::Key_End);
    check(list->currentRow() == 2 && list->item(2)->isSelected(),
          QString("End selects last list item (current=%1, selected=%2)")
              .arg(list->currentRow())
              .arg(list->item(2)->isSelected()));
    QTest::keyClick(list, Qt::Key_Return);
    check(named(window.doc, "list").values == QStringList{"West"}, "Enter commits list");
    window.doc.save(output + "/form-keyboard.pdf");
    return {{"keyboard_types", 6}, {"radio_Tab_preserves_value", true}};
}
QJsonObject testWindowPanelLifetime(const QString& fixtures, const QString& output)
{
    Q_UNUSED(output);
    const auto sourceHash = fileHash(fixtures + "/D07.pdf");
    for (int panel = 0; panel < 6; ++panel)
    {
        auto window = std::make_unique<Window>();
        window->show();
        window->openFile(fixtures + "/D07.pdf");
        if (panel == 4)
            window->findChild<QAction*>("organizeAction")->trigger();
        else if (panel == 0)
            window->signatureAction->trigger();
        else if (panel == 1)
            window->ocrAction->trigger();
        else if (panel == 2)
            window->findChild<QAction*>("writingAction")->trigger();
        else if (panel == 3)
            window->signatureAction->menu()->actions().at(1)->trigger();
        else
            window->findChild<QAction*>("annotationAction")->trigger();
        QTest::qWait(20);
        check(window->properties->isVisible(), "panel visible before window destruction");
        window.reset();
        QCoreApplication::processEvents();
    }
    check(fileHash(fixtures + "/D07.pdf") == sourceHash,
          "window destruction never saves over source");
    return {{"visible_panel_cases", 6}, {"shutdown_completed", true}, {"original_unchanged", true}};
}
} // namespace tatsu
