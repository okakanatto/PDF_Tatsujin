#include "link_edit_dialog.h"
#include <memory>

namespace tatsu
{
namespace
{
constexpr double mm = 72.0 / 25.4;
}
LinkEditDialog::LinkEditDialog(PDFDocument document, int currentPage, QWidget* parent)
    : QDialog(parent), snapshot(std::move(document)), values(editableLinks(snapshot))
{
    setObjectName("linkEditDialog");
    setWindowTitle("リンクを編集");
    resize(1000, 680);
    setMinimumSize(760, 480);
    auto layout = new QVBoxLayout(this);
    auto note = new QLabel("範囲を描いて追加。中央をドラッグして移動、右下でサイズ変更できます。最"
                           "後に文書へ適用します。");
    note->setWordWrap(true);
    layout->addWidget(note);
    settings = new QWidget;
    auto body = new QHBoxLayout(settings);
    body->setContentsMargins(0, 0, 0, 0);
    auto controls = new QWidget;
    auto column = new QVBoxLayout(controls);
    page = new QComboBox;
    page->setObjectName("linkEditPage");
    page->setAccessibleName("編集する物理ページ");
    for (int i = 0; i < int(snapshot.getCatalog()->getPageCount()); ++i)
        page->addItem(QString("%1ページ").arg(i + 1), i);
    page->setCurrentIndex(qBound(0, currentPage, page->count() - 1));
    column->addWidget(page);
    auto buttons = new QHBoxLayout;
    draw = new QPushButton("範囲を描いて追加");
    draw->setObjectName("linkDraw");
    draw->setCheckable(true);
    remove = new QPushButton("削除");
    remove->setObjectName("linkDelete");
    buttons->addWidget(draw);
    buttons->addWidget(remove);
    column->addLayout(buttons);
    list = new QListWidget;
    list->setObjectName("linkEditList");
    list->setAccessibleName("選択ページのリンク");
    list->setMinimumHeight(90);
    list->setMaximumHeight(140);
    column->addWidget(list);
    auto form = new QFormLayout;
    form->setRowWrapPolicy(QFormLayout::WrapAllRows);
    description = new QLineEdit;
    description->setMaxLength(500);
    description->setObjectName("linkDescription");
    form->addRow("説明（任意）", description);
    target = new QComboBox;
    target->setObjectName("linkTarget");
    target->addItems({"元の宛先を保持", "文書内ページ", "Web URL"});
    form->addRow("移動先", target);
    destination = new QSpinBox;
    destination->setObjectName("linkDestination");
    destination->setRange(1, page->count());
    destination->setSuffix(" ページ");
    form->addRow("物理ページ", destination);
    url = new QLineEdit;
    url->setMaxLength(4096);
    url->setObjectName("linkUrl");
    url->setPlaceholderText("https://example.com/");
    form->addRow("http / https", url);
    auto geometry = new QWidget;
    auto grid = new QGridLayout(geometry);
    grid->setContentsMargins(0, 0, 0, 0);
    auto field = [&](QString label, QString name, int row, int col)
    {
        auto widget = new QDoubleSpinBox;
        widget->setObjectName(name);
        widget->setRange(-100000, 100000);
        widget->setDecimals(4);
        widget->setSuffix(" mm");
        widget->setAccessibleName("リンクの" + label);
        grid->addWidget(new QLabel(label), row * 2, col);
        grid->addWidget(widget, row * 2 + 1, col);
        connect(widget, &QDoubleSpinBox::valueChanged, this,
                [this, name, widget]
                {
                    if (loading || selected < 0 || applyJob)
                        return;
                    auto box = values[selected].rectangle;
                    double left = box.x(), top = box.y(), w = box.width(), h = box.height();
                    if (name == "linkX")
                        left = widget->value() * mm;
                    else if (name == "linkY")
                        top = widget->value() * mm;
                    else if (name == "linkWidth")
                        w = widget->value() * mm;
                    else
                        h = widget->value() * mm;
                    values[selected].rectangle = {left, top, w, h};
                    updateSettings();
                });
        return widget;
    };
    x = field("左", "linkX", 0, 0);
    y = field("上", "linkY", 0, 1);
    width = field("幅", "linkWidth", 1, 0);
    height = field("高さ", "linkHeight", 1, 1);
    form->addRow(geometry);
    column->addLayout(form);
    auto hint = new QLabel("青い範囲は編集用の表示です。新しいリンクの枠はPDFに印刷されません。URL"
                           "を自動で開きません。");
    hint->setWordWrap(true);
    column->addWidget(hint);
    column->addStretch();
    auto scroll = new QScrollArea;
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setWidget(controls);
    scroll->setMinimumWidth(280);
    scroll->setMaximumWidth(330);
    body->addWidget(scroll);
    preview = new LinkPreview;
    preview->setObjectName("linkPreview");
    preview->setAccessibleName(
        "リンク範囲のプレビュー。中央で移動、右下でサイズ変更。数値欄でも変更できます");
    body->addWidget(preview, 1);
    layout->addWidget(settings, 1);
    message = new QLabel("候補を編集しています。元の文書はまだ変更していません。");
    message->setObjectName("linkMessage");
    message->setTextFormat(Qt::PlainText);
    message->setWordWrap(true);
    layout->addWidget(message);
    auto actions = new QDialogButtonBox;
    apply = actions->addButton("文書へ適用", QDialogButtonBox::AcceptRole);
    apply->setObjectName("linkApply");
    apply->setProperty("primary", true);
    cancel = actions->addButton("キャンセル", QDialogButtonBox::RejectRole);
    cancel->setObjectName("linkCancel");
    layout->addWidget(actions);
    connect(page, &QComboBox::currentIndexChanged, this,
            [this]
            {
                draw->setChecked(false);
                rebuild();
                loadPreview();
            });
    connect(list, &QListWidget::currentItemChanged, this, [this](QListWidgetItem* item)
            { select(item ? item->data(Qt::UserRole).toInt() : -1); });
    connect(description, &QLineEdit::textChanged, this, [this] { updateSettings(); });
    connect(target, &QComboBox::currentIndexChanged, this, [this] { updateSettings(); });
    connect(destination, &QSpinBox::valueChanged, this, [this] { updateSettings(); });
    connect(url, &QLineEdit::textChanged, this, [this] { updateSettings(); });
    connect(draw, &QPushButton::toggled, this,
            [this](bool value)
            {
                preview->drawing = value;
                preview->setCursor(value ? Qt::CrossCursor : Qt::ArrowCursor);
            });
    connect(
        remove, &QPushButton::clicked, this,
        [this]
        {
            if (selected >= 0)
            {
                values.removeAt(selected);
                rebuild();
                message->setText(
                    "リンクを候補から削除しました。文書へ適用するまで元のリンクは保持しています。");
            }
        });
    connect(apply, &QPushButton::clicked, this, &LinkEditDialog::accept);
    connect(cancel, &QPushButton::clicked, this, &LinkEditDialog::reject);
    preview->choose = [this](int index)
    {
        select(index);
        for (int row = 0; row < list->count(); ++row)
            if (list->item(row)->data(Qt::UserRole).toInt() == index)
                list->setCurrentRow(row);
    };
    preview->create = [this](QRectF rectangle) { add(rectangle); };
    preview->cancelDrawing = [this] { draw->setChecked(false); };
    preview->move = [this](int index, QRectF rectangle)
    {
        if (index >= 0 && index < values.size())
        {
            values[index].rectangle = rectangle;
            select(index);
            updatePreview();
            message->setText("範囲を変更しました。文書へ適用して確定できます。");
        }
    };
    rebuild();
    loadPreview();
}
LinkEditDialog::~LinkEditDialog()
{
    cancelled = true;
    for (auto job : {renderJob, applyJob})
        if (job)
        {
            disconnect(job, nullptr, this, nullptr);
            job->wait();
        }
}
void LinkEditDialog::rebuild(int selection)
{
    QSignalBlocker blocker(list);
    list->clear();
    QListWidgetItem* chosen = nullptr;
    for (int i = 0; i < values.size(); ++i)
        if (values[i].page == page->currentIndex())
        {
            auto item = new QListWidgetItem(values[i].description.isEmpty()
                                                ? QString("リンク %1").arg(i + 1)
                                                : values[i].description,
                                            list);
            item->setData(Qt::UserRole, i);
            if (i == selection)
                chosen = item;
        }
    if (!chosen && list->count())
        chosen = list->item(0);
    list->setCurrentItem(chosen);
    select(chosen ? chosen->data(Qt::UserRole).toInt() : -1);
    updatePreview();
}
void LinkEditDialog::select(int index)
{
    selected = index;
    loading = true;
    for (auto widget : QList<QWidget*>{description, target, x, y, width, height, remove})
        widget->setEnabled(index >= 0);
    destination->setEnabled(false);
    url->setEnabled(false);
    if (index >= 0)
    {
        const auto& item = values[index];
        description->setText(item.description);
        target->setCurrentIndex(int(item.target));
        destination->setValue(item.destination + 1);
        url->setText(item.url);
        x->setValue(item.rectangle.x() / mm);
        y->setValue(item.rectangle.y() / mm);
        width->setValue(item.rectangle.width() / mm);
        height->setValue(item.rectangle.height() / mm);
        destination->setEnabled(item.target == LinkTarget::Page);
        url->setEnabled(item.target == LinkTarget::Web);
    }
    else
    {
        description->clear();
        url->clear();
    }
    loading = false;
    updatePreview();
}
void LinkEditDialog::updateSettings()
{
    if (loading || selected < 0 || applyJob)
        return;
    auto& item = values[selected];
    item.description = description->text();
    item.target = LinkTarget(target->currentIndex());
    item.destination = destination->value() - 1;
    item.url = url->text();
    destination->setEnabled(item.target == LinkTarget::Page);
    url->setEnabled(item.target == LinkTarget::Web);
    if (list->currentItem())
        list->currentItem()->setText(
            item.description.isEmpty() ? QString("リンク %1").arg(selected + 1) : item.description);
    message->setText("候補を編集しています。文書へ適用して確定できます。");
    updatePreview();
}
void LinkEditDialog::updatePreview()
{
    preview->regions.clear();
    for (int i = 0; i < values.size(); ++i)
        if (values[i].page == page->currentIndex())
            preview->regions << qMakePair(i, values[i].rectangle);
    preview->selected = selected;
    preview->update();
}
void LinkEditDialog::add(QRectF rectangle)
{
    draw->setChecked(false);
    if (values.size() >= 1000)
    {
        message->setText("リンクは1000項目までです。既存の一覧は保持しています。");
        return;
    }
    values << LinkEntry{page->currentIndex(), -1, rectangle, {}, LinkTarget::Page,
                        page->currentIndex(), {}};
    rebuild(values.size() - 1);
    description->setFocus();
    message->setText("リンクの範囲を追加しました。移動先と説明を指定してください。");
}
void LinkEditDialog::loadPreview()
{
    preview->image = {};
    preview->update();
    draw->setEnabled(false);
    if (renderJob)
        return;
    const int number = page->currentIndex();
    struct Result
    {
        QImage image;
        QSizeF physical;
        QString error;
    };
    auto result = std::make_shared<Result>();
    renderJob = QThread::create(
        [document = snapshot, number, result]() mutable
        {
            try
            {
                result->physical = pageSize(document.getCatalog()->getPage(number));
                result->image = renderPage(
                    document, number,
                    qMin(1.5, 1000.0 / qMax(result->physical.width(), result->physical.height())));
            }
            catch (const std::exception& error)
            {
                result->error = QString::fromUtf8(error.what());
            }
            catch (...)
            {
                result->error = "ページのプレビューを描画できません。";
            }
        });
    auto launched = renderJob;
    launched->setParent(this);
    connect(launched, &QThread::finished, this,
            [this, launched, number, result]
            {
                renderJob = nullptr;
                launched->deleteLater();
                if (number != page->currentIndex())
                {
                    loadPreview();
                    return;
                }
                shownPage = number;
                preview->physical = result->physical;
                preview->image = result->image;
                preview->setProperty("shownPage", number);
                draw->setEnabled(!applyJob && !result->image.isNull());
                updatePreview();
                if (!result->error.isEmpty())
                    message->setText(result->error);
            });
    launched->start();
}
void LinkEditDialog::accept()
{
    if (applyJob)
        return;
    draw->setChecked(false);
    settings->setEnabled(false);
    apply->setEnabled(false);
    cancelled = false;
    message->setText("リンクの候補を確認しています。元の文書は変更していません。");
    struct Result
    {
        std::optional<PDFDocument> document;
        QString error;
    };
    auto result = std::make_shared<Result>();
    applyJob = QThread::create(
        [this, document = snapshot, edited = values, result]
        {
            try
            {
                result->document =
                    replaceLinks(document, edited, [this] { return cancelled.load(); });
            }
            catch (const std::exception& error)
            {
                result->error = QString::fromUtf8(error.what());
            }
            catch (...)
            {
                result->error = "リンクを変更できません。元の文書は保持しています。";
            }
        });
    auto launched = applyJob;
    launched->setParent(this);
    connect(launched, &QThread::finished, this,
            [this, launched, result]
            {
                applyJob = nullptr;
                launched->deleteLater();
                if (cancelled)
                    QDialog::reject();
                else if (result->document)
                {
                    candidate = std::move(result->document);
                    QDialog::accept();
                }
                else
                {
                    settings->setEnabled(true);
                    apply->setEnabled(true);
                    message->setText(result->error);
                }
            });
    launched->start();
}
void LinkEditDialog::reject()
{
    if (applyJob)
    {
        cancelled = true;
        cancel->setEnabled(false);
        message->setText("中止しています。元の文書は変更していません。");
    }
    else
        QDialog::reject();
}
void LinkEditDialog::closeEvent(QCloseEvent* event)
{
    if (applyJob)
    {
        event->ignore();
        reject();
    }
    else
        QDialog::closeEvent(event);
}
PDFDocument LinkEditDialog::takeDocument()
{
    if (!candidate || applyJob || result() != QDialog::Accepted)
        fail("完成したリンクの候補がありません。");
    auto result = std::move(*candidate);
    candidate.reset();
    return result;
}
} // namespace tatsu
