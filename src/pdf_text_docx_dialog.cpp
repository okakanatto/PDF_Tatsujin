#include "pdf_text_docx_dialog.h"
#include "pdf_text_docx.h"

namespace tatsu
{
PdfTextDocxDialog::PdfTextDocxDialog(PDFDocument document, int currentPage,
                                     std::function<void()> validate, QWidget* parent)
    : QDialog(parent), snapshot(std::move(document)), validate(std::move(validate))
{
    setObjectName("wordTextDialog");
    setWindowTitle("PDFの本文をWordへ取り出す");
    resize(1080, 720);
    setMinimumSize(800, 520);
    auto layout = new QVBoxLayout(this);
    auto note =
        new QLabel("本文を確認・修正し、編集できるWord文書へ保存します。元のレイアウト・"
                   "画像・表は復元せず、署名・注釈・フォーム入力は含めません。原PDFは保持します。");
    note->setWordWrap(true);
    layout->addWidget(note);
    auto controls = new QHBoxLayout;
    controls->addWidget(new QLabel("取り出すページ"));
    range = new QLineEdit(QString::number(currentPage + 1));
    range->setObjectName("wordTextRange");
    range->setAccessibleName("本文を取り出すページ範囲");
    range->setPlaceholderText("例: 1,3-5 （100ページまで）");
    controls->addWidget(range, 1);
    extract = new QPushButton("本文を取り出す");
    extract->setObjectName("wordTextExtract");
    controls->addWidget(extract);
    page = new QComboBox;
    page->setObjectName("wordTextPage");
    page->setAccessibleName("本文を確認するページ");
    controls->addWidget(page);
    controls->addWidget(new QLabel("Wordの書体"));
    font = new QFontComboBox;
    font->setObjectName("wordTextFont");
    font->setAccessibleName("Word本文の書体");
    font->setMaximumWidth(200);
    font->setEditable(false);
    if (QFontDatabase::families().contains("Meiryo UI"))
        font->setCurrentFont(QFont("Meiryo UI"));
    controls->addWidget(font);
    layout->addLayout(controls);
    auto split = new QSplitter;
    preview = new PageRegionPreview;
    preview->setObjectName("wordTextPreview");
    preview->navigationEnabled = true;
    preview->setAccessibleName("元のPDFページ");
    preview->setToolTip("Ctrl＋ホイールで拡大・縮小できます。");
    editor = new QPlainTextEdit;
    editor->setObjectName("wordTextEditor");
    editor->setAccessibleName("Wordへ保存する本文。編集できます");
    split->addWidget(preview);
    split->addWidget(editor);
    split->setSizes({500, 500});
    layout->addWidget(split, 1);
    message = new QLabel("本文を確認しています…");
    message->setObjectName("wordTextMessage");
    message->setWordWrap(true);
    layout->addWidget(message);
    auto fontNote =
        new QLabel("書体名を指定します。相手のPCに書体がない場合は代替書体で表示されます。");
    fontNote->setWordWrap(true);
    layout->addWidget(fontNote);
    auto output = new QHBoxLayout;
    path = new QLineEdit;
    path->setObjectName("wordTextPath");
    path->setAccessibleName("新しいWord文書の保存先");
    path->setPlaceholderText("新しい.docxファイル。既存ファイルは上書きしません");
    auto browse = new QPushButton("保存先…");
    browse->setObjectName("wordTextBrowse");
    save = new QPushButton("Wordへ保存");
    save->setObjectName("wordTextSave");
    save->setEnabled(false);
    auto close = new QPushButton("閉じる");
    close->setObjectName("wordTextClose");
    output->addWidget(path, 1);
    output->addWidget(browse);
    output->addWidget(save);
    output->addWidget(close);
    layout->addLayout(output);
    for (auto button : {extract, browse, save, close})
        button->setAutoDefault(false);
    connect(browse, &QPushButton::clicked, this,
            [this]
            {
                const auto chosen = QFileDialog::getSaveFileName(
                    this, "新しいWord文書へ保存", path->text(), "Word文書 (*.docx)", nullptr,
                    QFileDialog::DontConfirmOverwrite);
                if (!chosen.isEmpty())
                    path->setText(chosen);
            });
    connect(extract, &QPushButton::clicked, this, [this] { requestTexts(); });
    connect(range, &QLineEdit::returnPressed, this, [this] { requestTexts(); });
    connect(range, &QLineEdit::textChanged, this,
            [this]
            {
                if (available && !extracting)
                {
                    const bool applied = range->text() == appliedRange;
                    save->setEnabled(applied);
                    if (!applied)
                        message->setText("ページ範囲は未適用です。本文を取り出してください。");
                }
            });
    connect(page, &QComboBox::currentIndexChanged, this, [this](int index) { changePage(index); });
    connect(editor, &QPlainTextEdit::textChanged, this,
            [this]
            {
                if (available && shown < texts.size())
                {
                    texts[shown] = editor->toPlainText();
                    edited = true;
                    message->setText("本文を編集しました。元のPDFは変更していません。");
                }
            });
    connect(font, &QFontComboBox::currentFontChanged, this,
            [this](const QFont& value)
            {
                editor->setFont(value);
                if (available)
                    edited = true;
            });
    connect(save, &QPushButton::clicked, this, [this] { saveDocument(); });
    connect(close, &QPushButton::clicked, this, [this] { reject(); });
    worker.ready = [this](CandidatePreviewResult result)
    {
        if (!result.error.isEmpty())
        {
            message->setText(result.error);
            if (extracting)
            {
                extracting = false;
                extract->setEnabled(true);
                range->setEnabled(true);
            }
            return;
        }
        preview->image = std::move(result.image);
        preview->physical = result.dimensions;
        preview->update();
        if (!extracting)
            return;
        texts = *prepared;
        appliedRange = range->text();
        extracting = false;
        available = !texts.isEmpty();
        extract->setEnabled(true);
        range->setEnabled(true);
        editor->setEnabled(available);
        page->setEnabled(available);
        save->setEnabled(available);
        QSignalBlocker choice(page), contents(editor);
        page->clear();
        for (const int number : selected)
            page->addItem(QString("%1ページ").arg(number + 1));
        shown = 0;
        editor->setPlainText(texts.value(0));
        editor->setFont(font->currentFont());
        edited = false;
        message->setText(QString("%1ページの本文を取り出しました。順序と改行を確認してください。")
                             .arg(texts.size()));
    };
    requestTexts();
}
bool PdfTextDocxDialog::discardEdits()
{
    if (edited && QMessageBox::question(
                      this, "本文の編集を保持",
                      "取り出し直すと、確認画面で編集した本文は失われます。取り出し直しますか。",
                      QMessageBox::Yes | QMessageBox::No, QMessageBox::No) != QMessageBox::Yes)
    {
        QSignalBlocker blocker(range);
        range->setText(appliedRange);
        save->setEnabled(available);
        message->setText("編集した本文とページ範囲を保持しました。");
        return false;
    }
    return true;
}
void PdfTextDocxDialog::requestTexts()
{
    try
    {
        auto numbers = parsePages(range->text(), int(snapshot.getCatalog()->getPageCount()));
        if (numbers.size() > 100)
            fail("一度に取り出せる本文は100ページまでです。");
        if (!discardEdits())
            return;
        if (validate)
            validate();
        selected = std::move(numbers);
        prepared = std::make_shared<QStringList>();
        const auto values = prepared;
        const auto source = snapshot;
        const auto pages = selected;
        extracting = true;
        available = false;
        save->setEnabled(false);
        page->setEnabled(false);
        editor->setEnabled(false);
        extract->setEnabled(false);
        range->setEnabled(false);
        message->setText("本文を取り出しています…閉じる操作で取り消せます。");
        worker.request(
            [source, pages, values](const auto& cancelled)
            {
                *values = extractWordText(source, pages, cancelled);
                return source;
            },
            selected.first(), 1200);
    }
    catch (const std::exception& error)
    {
        message->setText(QString::fromUtf8(error.what()));
    }
}
void PdfTextDocxDialog::changePage(int index)
{
    if (!available || index < 0 || index >= texts.size())
        return;
    shown = index;
    QSignalBlocker blocker(editor);
    editor->setPlainText(texts[index]);
    preview->image = {};
    preview->update();
    const auto source = snapshot;
    worker.request([source](const auto&) { return source; }, selected[index], 1200);
}
void PdfTextDocxDialog::saveDocument()
{
    if (!available)
        return;
    try
    {
        if (validate)
            validate();
        exportWordText(texts, path->text(), font->currentFont().family());
        edited = false;
        message->setText("保存しました：" + QFileInfo(path->text()).absoluteFilePath());
    }
    catch (const std::exception& error)
    {
        message->setText(QString::fromUtf8(error.what()));
    }
}
void PdfTextDocxDialog::reject()
{
    if (edited && QMessageBox::question(
                      this, "本文編集を保存", "編集した本文は保存されていません。閉じますか。",
                      QMessageBox::Yes | QMessageBox::No, QMessageBox::No) != QMessageBox::Yes)
        return;
    save->setEnabled(false);
    worker.cancel([this] { QDialog::reject(); });
}
void PdfTextDocxDialog::closeEvent(QCloseEvent* event)
{
    event->ignore();
    reject();
}
} // namespace tatsu
