#include "redaction_dialog.h"
#include "ui_widgets.h"
#include <optional>

namespace tatsu
{
namespace
{
constexpr double mm = 72.0 / 25.4;
}
RedactionDialog::RedactionDialog(PDFDocument document, int currentPage,
                                 QVector<QPair<QString, QByteArray>> inputs, QString suggestedPath,
                                 QWidget* parent)
    : QDialog(parent), snapshot(std::move(document)), originals(std::move(inputs))
{
    setObjectName("redactionDialog");
    setWindowTitle("墨消ししたコピー");
    resize(1024, 700);
    setMinimumSize(760, 480);
    auto layout = new QVBoxLayout(this);
    auto note = new QLabel(
        "指定範囲の内容と文書内の隠れた情報（文書情報・添付等）を削除して、新しいPDFへ保存します。"
        "元のPDFとUndoは保持します。対応できないPDFは保存せず理由を表示します。");
    note->setWordWrap(true);
    note->setTextFormat(Qt::PlainText);
    layout->addWidget(note);
    settings = new QWidget;
    auto body = new QHBoxLayout(settings);
    body->setContentsMargins(0, 0, 0, 0);
    preview = new PageRegionPreview;
    preview->setObjectName("redactionPreview");
    preview->navigationEnabled = true;
    preview->regionColor = QColor("#b45309");
    preview->setToolTip(
        "Ctrl＋ホイールで拡大。ホイールで上下移動、中央ボタンのドラッグで移動できます。");
    preview->setAccessibleName(
        "墨消し範囲のプレビュー。範囲を描いて追加、中央で移動、右下でサイズ変更");
    auto viewer = new QWidget;
    auto viewerLayout = new QVBoxLayout(viewer);
    viewerLayout->setContentsMargins(0, 0, 0, 0);
    auto zoomTools = new QHBoxLayout;
    auto zoomButton = [&](QString text, QString name, std::function<void()> call)
    {
        auto button = new QPushButton(text);
        button->setObjectName(name);
        button->setAutoDefault(false);
        connect(button, &QPushButton::clicked, this, std::move(call));
        zoomTools->addWidget(button);
    };
    zoomButton("縮小", "redactionZoomOut",
               [this] { preview->setZoom(preview->zoom() / 1.5, preview->rect().center()); });
    zoomButton("拡大", "redactionZoomIn",
               [this] { preview->setZoom(preview->zoom() * 1.5, preview->rect().center()); });
    zoomButton("全体", "redactionFit", [this] { preview->fitPage(); });
    zoomTools->addStretch();
    viewerLayout->addLayout(zoomTools);
    viewerLayout->addWidget(preview, 1);
    body->addWidget(viewer, 1);
    auto controls = new QWidget;
    auto column = new QVBoxLayout(controls);
    page = new QComboBox;
    page->setObjectName("redactionPage");
    page->setAccessibleName("範囲を指定する物理ページ");
    for (int index = 0; index < int(snapshot.getCatalog()->getPageCount()); ++index)
        page->addItem(QString("%1ページ").arg(index + 1));
    page->setCurrentIndex(qBound(0, currentPage, page->count() - 1));
    column->addWidget(page);
    auto tools = new QHBoxLayout;
    draw = new QPushButton("範囲を描いて追加");
    draw->setCheckable(true);
    draw->setObjectName("redactionDraw");
    remove = new QPushButton("範囲を削除");
    remove->setObjectName("redactionDelete");
    tools->addWidget(draw);
    tools->addWidget(remove);
    column->addLayout(tools);
    auto wholePage = new QPushButton("ページ全体を指定");
    wholePage->setObjectName("redactionWholePage");
    wholePage->setAutoDefault(false);
    column->addWidget(wholePage);
    connect(
        wholePage, &QPushButton::clicked, this,
        [this] {
            add(QRectF(QPointF(), pageSize(snapshot.getCatalog()->getPage(page->currentIndex()))));
        });
    list = new QListWidget;
    list->setObjectName("redactionRegions");
    list->setAccessibleName("全ページの墨消し範囲");
    list->setMinimumHeight(90);
    list->setMaximumHeight(160);
    auto erase = new QShortcut(QKeySequence::Delete, list);
    erase->setContext(Qt::WidgetShortcut);
    connect(erase, &QShortcut::activated, remove, &QPushButton::click);
    column->addWidget(list);
    auto geometry = new QGridLayout;
    auto field = [&](QString label, QString name, int row, int col)
    {
        auto value = new QDoubleSpinBox;
        value->setObjectName(name);
        value->setDecimals(4);
        value->setRange((name == "redactionWidth" || name == "redactionHeight") ? 0.1 : 0, 100000);
        value->setSuffix(" mm");
        value->setAccessibleName("墨消し範囲の" + label);
        geometry->addWidget(new QLabel(label), row * 2, col);
        geometry->addWidget(value, row * 2 + 1, col);
        connect(value, &QDoubleSpinBox::valueChanged, this,
                [this]
                {
                    if (loading || saveJob || selected < 0)
                        return;
                    regions[selected].rectangle = {x->value() * mm, y->value() * mm,
                                                   width->value() * mm, height->value() * mm};
                    invalidate();
                    updateSelection();
                });
        return value;
    };
    x = field("左から", "redactionX", 0, 0);
    y = field("上から", "redactionY", 0, 1);
    width = field("幅", "redactionWidth", 1, 0);
    height = field("高さ", "redactionHeight", 1, 1);
    column->addLayout(geometry);
    auto help = new QLabel(
        "範囲は保存まで取り消せます。拡大して位置を確認でき、移動・サイズ変更は数値でも指定できます"
        "。"
        "文字列の途中や文字層の一部だけの指定はまだ保存できません。範囲外の文字が同じ埋め込み書体を"
        "使う場合も中止します。対象の文字層は、ページ外へ続く同じ層の内容も除去します。");
    help->setWordWrap(true);
    help->setTextFormat(Qt::PlainText);
    column->addWidget(help);
    column->addWidget(new QLabel("新しいPDFの保存先"));
    destination = new QLineEdit(suggestedPath);
    destination->setObjectName("redactionDestination");
    destination->setAccessibleName("墨消ししたコピーの新しいPDF保存先");
    column->addWidget(destination);
    auto browse = new QPushButton("保存先…");
    browse->setObjectName("redactionBrowse");
    column->addWidget(browse);
    connect(browse, &QPushButton::clicked, this,
            [this]
            {
                const auto path = QFileDialog::getSaveFileName(
                    this, "墨消ししたコピーの新しい保存先", destination->text(), "PDF (*.pdf)",
                    nullptr, QFileDialog::DontConfirmOverwrite);
                if (!path.isEmpty())
                    destination->setText(path);
            });
    column->addStretch();
    auto scroll = scrollableSettings(controls);
    scroll->setMinimumWidth(280);
    scroll->setMaximumWidth(340);
    body->addWidget(scroll);
    layout->addWidget(settings, 1);
    consent = new QCheckBox("指定した範囲と隠れた情報を削除したコピーを作る");
    consent->setObjectName("redactionConsent");
    layout->addWidget(consent);
    progress = new QProgressBar;
    progress->setObjectName("redactionProgress");
    progress->setRange(0, 0);
    progress->hide();
    layout->addWidget(progress);
    message = new QLabel("ページ上で範囲を指定してください。既存のPDFは上書きしません。");
    message->setObjectName("redactionMessage");
    message->setTextFormat(Qt::PlainText);
    message->setWordWrap(true);
    layout->addWidget(message);
    auto actions = new QDialogButtonBox;
    save = actions->addButton("コピーを保存して開く", QDialogButtonBox::AcceptRole);
    save->setObjectName("saveRedactedCopy");
    save->setProperty("primary", true);
    save->setAutoDefault(false);
    save->setEnabled(false);
    cancel = actions->addButton("キャンセル", QDialogButtonBox::RejectRole);
    cancel->setObjectName("cancelRedaction");
    layout->addWidget(actions);
    connect(save, &QPushButton::clicked, this, &RedactionDialog::accept);
    connect(cancel, &QPushButton::clicked, this, &RedactionDialog::reject);
    connect(consent, &QCheckBox::toggled, this,
            [this](bool checked)
            {
                save->setEnabled(checked && !regions.isEmpty() &&
                                 !destination->text().trimmed().isEmpty() && !saveJob);
            });
    connect(destination, &QLineEdit::textChanged, this, [this] { invalidate(); });
    connect(page, &QComboBox::currentIndexChanged, this,
            [this]
            {
                draw->setChecked(false);
                if (selected >= 0 && regions[selected].page != page->currentIndex())
                {
                    selected = -1;
                    const QSignalBlocker blocker(list);
                    list->setCurrentRow(-1);
                }
                updateSelection();
                loadPreview();
            });
    connect(draw, &QPushButton::toggled, this,
            [this](bool checked)
            {
                preview->drawing = checked;
                if (checked)
                    preview->setFocus();
            });
    connect(remove, &QPushButton::clicked, this,
            [this]
            {
                if (selected >= 0)
                {
                    regions.removeAt(selected);
                    invalidate();
                    rebuild();
                }
            });
    connect(list, &QListWidget::currentRowChanged, this,
            [this](int row)
            {
                selected = row;
                if (row >= 0)
                    page->setCurrentIndex(regions[row].page);
                updateSelection();
            });
    preview->choose = [this](int index) { list->setCurrentRow(index); };
    preview->create = [this](QRectF rectangle) { add(rectangle); };
    preview->move = [this](int index, QRectF rectangle)
    {
        if (index >= 0 && index < regions.size())
        {
            regions[index].rectangle = rectangle;
            invalidate();
            rebuild(index);
        }
    };
    preview->cancelDrawing = [this] { draw->setChecked(false); };
    rebuild();
    loadPreview();
}
RedactionDialog::~RedactionDialog()
{
    cancelled = true;
    for (auto thread : {renderJob, saveJob})
        if (thread)
        {
            disconnect(thread, nullptr, this, nullptr);
            thread->wait();
        }
}
void RedactionDialog::invalidate()
{
    consent->setChecked(false);
    save->setEnabled(false);
}
void RedactionDialog::rebuild(int selection)
{
    const QSignalBlocker blocker(list);
    list->clear();
    for (int index = 0; index < regions.size(); ++index)
        list->addItem(QString("%1：%2ページの範囲").arg(index + 1).arg(regions[index].page + 1));
    selected = qBound(-1, selection, int(regions.size()) - 1);
    list->setCurrentRow(selected);
    updateSelection();
}
void RedactionDialog::updateSelection()
{
    loading = true;
    const bool active = selected >= 0;
    for (auto field : {x, y, width, height})
        field->setEnabled(active);
    remove->setEnabled(active);
    if (active)
    {
        const auto rectangle = regions[selected].rectangle;
        x->setValue(rectangle.x() / mm);
        y->setValue(rectangle.y() / mm);
        width->setValue(rectangle.width() / mm);
        height->setValue(rectangle.height() / mm);
    }
    loading = false;
    preview->regions.clear();
    for (int index = 0; index < regions.size(); ++index)
        if (regions[index].page == page->currentIndex())
            preview->regions << qMakePair(index, regions[index].rectangle);
    preview->selected = selected;
    preview->update();
}
void RedactionDialog::add(QRectF rectangle)
{
    draw->setChecked(false);
    if (regions.size() >= 1000)
    {
        message->setText("範囲はコピー全体で1000個までです。");
        return;
    }
    regions << Region{page->currentIndex(), rectangle};
    invalidate();
    rebuild(regions.size() - 1);
}
void RedactionDialog::loadPreview()
{
    preview->fitPage();
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
                result->error = "ページを描画できません。";
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
                preview->physical = result->physical;
                preview->image = result->image;
                preview->setProperty("shownPage", number);
                draw->setEnabled(!saveJob && !result->image.isNull());
                updateSelection();
                if (!result->error.isEmpty())
                    message->setText(result->error);
            });
    launched->start();
}
QMap<int, QVector<QRectF>> RedactionDialog::rawRegions() const
{
    QMap<int, QVector<QRectF>> result;
    for (const auto& region : regions)
    {
        const auto sourcePage = snapshot.getCatalog()->getPage(region.page);
        if (!QRectF(QPointF(), pageSize(sourcePage)).contains(region.rectangle))
            fail("範囲をページの内側へ移動してください。");
        bool invertible = false;
        const auto inverse = pageMatrix(sourcePage).inverted(&invertible);
        if (!invertible)
            fail("ページの座標変換を確認できません。");
        result[region.page] << inverse.mapRect(region.rectangle);
    }
    return result;
}
void RedactionDialog::setBusy(bool busy)
{
    settings->setEnabled(!busy);
    consent->setEnabled(!busy);
    progress->setVisible(busy);
    cancel->setEnabled(true);
    cancel->setText(busy ? "処理を中止" : "キャンセル");
    save->setEnabled(!busy && consent->isChecked() && !regions.isEmpty());
}
void RedactionDialog::accept()
{
    if (saveJob || !consent->isChecked() || regions.isEmpty())
        return;
    QMap<int, QVector<QRectF>> plan;
    try
    {
        plan = rawRegions();
    }
    catch (const std::exception& error)
    {
        message->setText(QString::fromUtf8(error.what()));
        invalidate();
        return;
    }
    const auto output = QFileInfo(destination->text()).absoluteFilePath();
    draw->setChecked(false);
    cancelled = false;
    closeRequested = false;
    setBusy(true);
    struct Result
    {
        std::optional<RedactedCopy> copy;
        QString error;
    };
    auto result = std::make_shared<Result>();
    saveJob = QThread::create(
        [this, plan, output, result]
        {
            try
            {
                result->copy = exportRedactedPdf(
                    snapshot, plan, output, [this] { return cancelled.load(); },
                    [this](QString status)
                    {
                        QMetaObject::invokeMethod(
                            this,
                            [this, status]
                            {
                                if (saveJob && !cancelled)
                                    message->setText(status);
                            },
                            Qt::QueuedConnection);
                    },
                    [this]
                    {
                        for (const auto& original : originals)
                            if (fileHash(original.first) != original.second)
                                fail("元のPDFファイルが更新されました。開き直してください。");
                    });
            }
            catch (const std::exception& error)
            {
                result->error = QString::fromUtf8(error.what());
            }
            catch (...)
            {
                result->error = "墨消ししたコピーを作成できません。元の文書は保持しています。";
            }
        });
    auto launched = saveJob;
    launched->setParent(this);
    connect(launched, &QThread::finished, this,
            [this, launched, output, result]
            {
                saveJob = nullptr;
                launched->deleteLater();
                setBusy(false);
                if (result->copy)
                {
                    completedPath = output;
                    completedHash = result->copy->hash;
                    QDialog::accept();
                }
                else
                {
                    invalidate();
                    message->setText(cancelled ? "墨消しを中止しました。元の文書は保持しています。"
                                               : result->error);
                    if (closeRequested)
                        QDialog::reject();
                }
            });
    launched->start();
}
void RedactionDialog::reject()
{
    if (saveJob)
    {
        cancelled = true;
        closeRequested = true;
        cancel->setEnabled(false);
        message->setText("処理を中止して閉じています…");
    }
    else
        QDialog::reject();
}
void RedactionDialog::closeEvent(QCloseEvent* event)
{
    if (saveJob)
    {
        event->ignore();
        reject();
    }
    else
        QDialog::closeEvent(event);
}
} // namespace tatsu
