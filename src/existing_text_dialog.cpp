#include "existing_text_dialog.h"

namespace tatsu
{
namespace
{
constexpr double mm = 72.0 / 25.4;
}
ExistingTextDialog::ExistingTextDialog(PDFDocument document, int currentPage,
                                       std::function<void()> validate, QWidget* parent)
    : QDialog(parent), snapshot(std::move(document)), validate(std::move(validate)), worker(this)
{
    setObjectName("existingTextDialog");
    setWindowTitle("PDF本文の文字を編集");
    resize(1024, 700);
    setMinimumSize(760, 480);
    auto layout = new QVBoxLayout(this);
    auto note = new QLabel("本文の文字ブロックを選び、文字の置換・位置とサイズの変更・削除を実ペー"
                           "ジで確認して適用します。元の字体で表現できる横書きに対応します。");
    note->setWordWrap(true);
    layout->addWidget(note);
    auto body = new QHBoxLayout;
    auto left = new QVBoxLayout;
    auto zoom = new QHBoxLayout;
    preview = new PageRegionPreview;
    preview->setObjectName("existingTextPreview");
    preview->setAccessibleName("本文編集のページプレビュー");
    preview->navigationEnabled = true;
    preview->zoomChanged = [this](double) { schedule(); };
    for (const auto& label : {QString("縮小"), QString("拡大"), QString("全体")})
    {
        auto button = new QPushButton(label);
        button->setAutoDefault(false);
        zoom->addWidget(button);
        connect(button, &QPushButton::clicked, this,
                [this, label]
                {
                    if (label == "全体")
                        preview->fitPage();
                    else
                        preview->setZoom(preview->zoom() * (label == "拡大" ? 1.25 : .8),
                                         preview->rect().center());
                });
    }
    zoom->addStretch();
    left->addLayout(zoom);
    left->addWidget(preview, 1);
    body->addLayout(left, 1);
    auto settings = new QWidget;
    auto column = new QVBoxLayout(settings);
    page = new QComboBox;
    page->setObjectName("existingTextPage");
    page->setAccessibleName("本文を編集するページ");
    for (int i = 0; i < int(snapshot.getCatalog()->getPageCount()); ++i)
        page->addItem(QString("%1ページ").arg(i + 1));
    page->setCurrentIndex(qBound(0, currentPage, page->count() - 1));
    column->addWidget(page);
    list = new QListWidget;
    list->setObjectName("existingTextList");
    list->setAccessibleName("本文の文字ブロック一覧");
    list->setMaximumHeight(130);
    column->addWidget(list);
    fontInfo = new QLabel;
    fontInfo->setObjectName("existingTextFontInfo");
    fontInfo->setWordWrap(true);
    column->addWidget(fontInfo);
    operation = new QComboBox;
    operation->setObjectName("existingTextOperation");
    operation->setAccessibleName("本文の変更内容");
    operation->addItems({"文字を置換", "位置・サイズを変更", "文字を削除"});
    column->addWidget(operation);
    text = new QPlainTextEdit;
    text->setObjectName("existingTextInput");
    text->setAccessibleName("置換する本文の文字");
    text->setMaximumHeight(100);
    column->addWidget(text);
    auto form = new QFormLayout;
    auto number = [&](QString name, QString label)
    {
        auto value = new QDoubleSpinBox;
        value->setObjectName(name);
        value->setAccessibleName(label);
        value->setDecimals(4);
        value->setRange(-1000000, 1000000);
        value->setSuffix(" mm");
        form->addRow(label, value);
        return value;
    };
    x = number("existingTextX", "左から");
    y = number("existingTextY", "上から");
    width = number("existingTextWidth", "幅");
    height = number("existingTextHeight", "高さ");
    column->addLayout(form);
    consent = new QCheckBox("この文字描画を削除する");
    consent->setObjectName("existingTextDeleteConsent");
    column->addWidget(consent);
    auto help = new QLabel(
        "表示する文字と字体を確認してください。文字列の長さで表示幅が変わります。字体の変更・複数行"
        "・Form内部・OCR層などは未対応です。文字の削除は墨消しではありません。");
    help->setWordWrap(true);
    column->addWidget(help);
    column->addStretch();
    auto scroll = new QScrollArea;
    scroll->setWidgetResizable(true);
    scroll->setWidget(settings);
    scroll->setMinimumWidth(290);
    scroll->setMaximumWidth(340);
    body->addWidget(scroll);
    layout->addLayout(body, 1);
    message = new QLabel;
    message->setObjectName("existingTextMessage");
    message->setWordWrap(true);
    message->setAccessibleName("本文編集の確認結果");
    layout->addWidget(message);
    auto footer = new QHBoxLayout;
    footer->addStretch();
    apply = new QPushButton("変更を適用");
    apply->setObjectName("applyExistingText");
    apply->setAutoDefault(false);
    auto cancel = new QPushButton("キャンセル");
    cancel->setAutoDefault(false);
    footer->addWidget(apply);
    footer->addWidget(cancel);
    layout->addLayout(footer);
    connect(apply, &QPushButton::clicked, this, &ExistingTextDialog::accept);
    connect(cancel, &QPushButton::clicked, this, &ExistingTextDialog::reject);
    connect(page, &QComboBox::currentIndexChanged, this, [this] { loadPage(); });
    connect(list, &QListWidget::currentRowChanged, this, [this](int index) { select(index); });
    connect(operation, &QComboBox::currentIndexChanged, this,
            [this]
            {
                consent->setChecked(false);
                schedule();
            });
    connect(text, &QPlainTextEdit::textChanged, this, [this] { schedule(); });
    connect(consent, &QCheckBox::toggled, this, [this] { updateApply(); });
    for (auto field : {x, y, width, height})
        connect(field, &QDoubleSpinBox::valueChanged, this,
                [this, field](double value)
                {
                    if (field == x)
                        geometry.moveLeft(value * mm);
                    else if (field == y)
                        geometry.moveTop(value * mm);
                    else if (field == width)
                        geometry.setWidth(value * mm);
                    else
                        geometry.setHeight(value * mm);
                    preview->regions.clear();
                    for (int i = 0; i < blocks.size(); ++i)
                        preview->regions << qMakePair(
                            i, i == list->currentRow() ? geometry : blocks[i].physical);
                    preview->update();
                    schedule();
                });
    preview->choose = [this](int index) { list->setCurrentRow(index); };
    preview->move = [this](int index, QRectF bounds)
    {
        if (index == list->currentRow() && operation->currentIndex() == 1)
        {
            setGeometry(bounds);
            schedule();
        }
    };
    worker.ready = [this](CandidatePreviewResult result)
    {
        valid = result.error.isEmpty() && !result.image.isNull();
        preview->setProperty("renderReady", true);
        if (valid)
        {
            candidate = std::move(result.document);
            preview->physical = result.dimensions;
            preview->image = std::move(result.image);
            const int row = list->currentRow();
            preview->regions.clear();
            for (int i = 0; i < blocks.size(); ++i)
                if (i != row || operation->currentIndex() != 2)
                    preview->regions << qMakePair(i, i == row && operation->currentIndex() == 1
                                                         ? geometry
                                                         : blocks[i].physical);
            preview->update();
            message->setText(!pageError.isEmpty() ? pageError
                             : row >= 0 && !blocks[row].restriction.isEmpty()
                                 ? blocks[row].restriction
                                 : QString("プレビューを確認して適用してください。変更はまだ文書へ"
                                           "反映していません。"));
        }
        else
            message->setText(result.error);
        updateApply();
    };
    loadPage();
}
void ExistingTextDialog::loadPage()
{
    blocks.clear();
    pageError.clear();
    try
    {
        blocks = existingTextBlocks(snapshot, page->currentIndex());
    }
    catch (const std::exception& error)
    {
        pageError = QString::fromUtf8(error.what());
        message->setText(pageError);
    }
    QSignalBlocker blocker(list);
    list->clear();
    for (const auto& block : blocks)
    {
        auto item = new QListWidgetItem(block.text.left(60), list);
        item->setToolTip(block.font + "\n" + block.restriction);
    }
    preview->fitPage();
    preview->physical = pageSize(snapshot.getCatalog()->getPage(page->currentIndex()));
    list->setCurrentRow(blocks.isEmpty() ? -1 : 0);
    select(list->currentRow());
}
void ExistingTextDialog::select(int index)
{
    const QSignalBlocker blocker(text);
    text->setPlainText(index >= 0 ? blocks[index].text : QString());
    fontInfo->setText(index >= 0 ? "元の字体：" + blocks[index].font : QString());
    consent->setChecked(false);
    setGeometry(index >= 0 ? blocks[index].physical : QRectF());
    preview->selected = index;
    schedule();
}
void ExistingTextDialog::setGeometry(QRectF value)
{
    geometry = value;
    for (const auto& pair : QVector<QPair<QDoubleSpinBox*, double>>{
             {x, value.x()}, {y, value.y()}, {width, value.width()}, {height, value.height()}})
    {
        QSignalBlocker blocker(pair.first);
        pair.first->setValue(pair.second / mm);
    }
    preview->regions.clear();
    for (int i = 0; i < blocks.size(); ++i)
        preview->regions << qMakePair(i, i == list->currentRow() ? geometry : blocks[i].physical);
    preview->update();
}
void ExistingTextDialog::updateApply()
{
    const int index = list->currentRow(), op = operation->currentIndex();
    const bool selected = index >= 0 && index < blocks.size();
    const bool supported = selected && blocks[index].restriction.isEmpty();
    const bool changed =
        selected && (op == 2 || (op == 1 ? geometry != blocks[index].physical
                                         : text->toPlainText() != blocks[index].text));
    apply->setEnabled(!closing && valid && supported && changed &&
                      (op != 2 || consent->isChecked()));
    for (auto field : {x, y, width, height})
        field->setEnabled(supported && op == 1 && !closing);
    text->setEnabled(supported && op == 0 && !closing);
    consent->setVisible(op == 2);
    preview->selected = op == 1 ? index : -1;
}
void ExistingTextDialog::schedule()
{
    if (closing)
        return;
    valid = false;
    preview->setProperty("renderReady", false);
    updateApply();
    const int index = list->currentRow(), number = page->currentIndex(),
              op = operation->currentIndex();
    const auto rows = blocks;
    const auto source = snapshot;
    const auto bounds = geometry;
    const auto replacement = text->toPlainText();
    message->setText("プレビューを更新しています…");
    worker.request(
        [source, rows, index, number, op, bounds, replacement](const CandidatePreview::Cancel& stop)
        {
            if (index < 0 || index >= rows.size())
                return source;
            if (!rows[index].restriction.isEmpty())
                return source;
            if (op == 0 && replacement == rows[index].text)
                return source;
            if (op == 1 && bounds == rows[index].physical)
                return source;
            const auto change = op == 0   ? ExistingTextChange::Replace
                                : op == 1 ? ExistingTextChange::Geometry
                                          : ExistingTextChange::Remove;
            return editExistingText(source, number, rows[index].occurrence, change, replacement,
                                    bounds, stop);
        },
        number, 1000.0 * preview->zoom());
}
void ExistingTextDialog::accept()
{
    if (!apply->isEnabled())
        return;
    try
    {
        if (validate)
            validate();
        QDialog::accept();
    }
    catch (const std::exception& error)
    {
        valid = false;
        message->setText(QString::fromUtf8(error.what()));
        updateApply();
    }
}
void ExistingTextDialog::reject()
{
    if (closing)
        return;
    closing = true;
    updateApply();
    worker.cancel([this] { QDialog::reject(); });
}
} // namespace tatsu
