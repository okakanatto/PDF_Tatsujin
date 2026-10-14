#include "page_decoration_dialog.h"

namespace tatsu
{
PageDecorationDialog::PageDecorationDialog(PDFDocument document, int currentPage,
                                           DecorationKind kind, QWidget* parent)
    : QDialog(parent), snapshot(std::move(document)), kind(kind), current(currentPage)
{
    setObjectName("pageDecorationDialog");
    setWindowTitle(kind == DecorationKind::HeaderFooter ? "ヘッダー／フッター・ページ番号"
                                                        : "透かし");
    resize(880, 640);
    setMinimumSize(760, 480);
    auto layout = new QVBoxLayout(this);
    auto note = new QLabel("表示中の向きに配置します。保存後もこの画面で変更・削除できます。"
                           "番号は保存時の固定値です。本文の編集や墨消しには使えません。");
    note->setWordWrap(true);
    layout->addWidget(note);
    auto columns = new QHBoxLayout;
    auto scroll = new QScrollArea;
    scroll->setWidgetResizable(true);
    scroll->setMinimumWidth(420);
    settings = new QWidget;
    auto form = new QFormLayout(settings);
    group = new QComboBox;
    group->setObjectName("decorationGroup");
    group->addItem("新しく追加する", "");
    groups = decorationGroups(snapshot);
    for (const auto& value : groups)
        if (value.options.kind == kind)
            group->addItem(QString("保存された設定 %1（%2ページ）")
                               .arg(group->count())
                               .arg(value.pages.size()),
                           value.id);
    form->addRow("編集する設定", group);
    scope = new QComboBox;
    scope->setObjectName("decorationScope");
    scope->addItems({"全ページ", "現在ページ", "指定ページ"});
    range = new QLineEdit;
    range->setObjectName("decorationRange");
    range->setPlaceholderText("例: 1-3, 5");
    form->addRow("対象ページ", scope);
    form->addRow("指定する範囲", range);
    if (kind == DecorationKind::HeaderFooter)
    {
        const QStringList labels{"ヘッダー左", "ヘッダー中央", "ヘッダー右",
                                 "フッター左", "フッター中央", "フッター右"};
        for (int i = 0; i < 6; ++i)
        {
            entries[i] = new QLineEdit;
            entries[i]->setObjectName(QString("decorationText%1").arg(i));
            entries[i]->setMaxLength(200);
            form->addRow(labels[i], entries[i]);
        }
        entries[4]->setText("{page} / {pages}");
        auto hint = new QLabel("{page}：開始番号＋物理ページ位置\n{pages}：文書全体のページ数");
        hint->setWordWrap(true);
        form->addRow(hint);
    }
    watermark = new QLineEdit(settings);
    watermark->setObjectName("decorationWatermark");
    watermark->setMaxLength(200);
    if (kind == DecorationKind::Watermark)
    {
        watermark->setText("社外秘");
        form->addRow("透かしの文字", watermark);
    }
    else
        watermark->hide();
    font = new TextFontPicker;
    font->setObjectName("decorationFont");
    font->setFamily(signatureFont());
    form->addRow("書体", font);
    auto numeric = [&](const QString& label, const QString& name, double minimum, double maximum,
                       double value, const QString& suffix)
    {
        auto control = new QDoubleSpinBox;
        control->setObjectName(name);
        control->setRange(minimum, maximum);
        control->setDecimals(2);
        control->setValue(value);
        control->setSuffix(suffix);
        form->addRow(label, control);
        connect(control, &QDoubleSpinBox::valueChanged, this, [this] { changed(); });
        return control;
    };
    size = numeric("文字サイズ", "decorationSize", 6, 144,
                   kind == DecorationKind::Watermark ? 36 : 12, " pt");
    auto color = new QComboBox;
    color->setObjectName("decorationColor");
    for (const auto& pair : {qMakePair("濃い灰色", "#444444"), qMakePair("黒", "#000000"),
                             qMakePair("青", "#0055bb"), qMakePair("赤", "#bb0000")})
        color->addItem(pair.first, pair.second);
    form->addRow("文字色", color);
    left = numeric("左からの距離", "decorationLeft", 0, 100, 10, " mm");
    top = numeric("上からの距離", "decorationTop", 0, 100, 10, " mm");
    right = numeric("右からの距離", "decorationRight", 0, 100, 10, " mm");
    bottom = numeric("下からの距離", "decorationBottom", 0, 100, 10, " mm");
    start = new QSpinBox(settings);
    start->setObjectName("decorationStartNumber");
    start->setRange(1, 999999);
    start->setValue(1);
    if (kind == DecorationKind::HeaderFooter)
        form->addRow("ページ開始番号", start);
    else
        start->hide();
    angle = numeric("透かしの角度", "decorationAngle", -90, 90, -30, " °");
    opacity = new QSpinBox(settings);
    opacity->setObjectName("decorationOpacity");
    opacity->setRange(5, 100);
    opacity->setValue(20);
    opacity->setSuffix(" %");
    if (kind == DecorationKind::Watermark)
        form->addRow("不透明度", opacity);
    else
    {
        form->labelForField(angle)->hide();
        angle->hide();
        opacity->hide();
    }
    scroll->setWidget(settings);
    columns->addWidget(scroll, 1);
    auto previewColumn = new QVBoxLayout;
    page = new QComboBox;
    page->setObjectName("decorationPreviewPage");
    page->setAccessibleName("プレビューするページ");
    for (int i = 0; i < int(snapshot.getCatalog()->getPageCount()); ++i)
        page->addItem(QString("プレビュー：%1ページ").arg(i + 1), i);
    page->setCurrentIndex(current);
    previewColumn->addWidget(page);
    preview = new QLabel("プレビューを準備しています…");
    preview->setObjectName("decorationPreview");
    preview->setAlignment(Qt::AlignCenter);
    preview->setMinimumSize(250, 220);
    preview->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Ignored);
    previewColumn->addWidget(preview, 1);
    columns->addLayout(previewColumn, 1);
    layout->addLayout(columns, 1);
    message = new QLabel;
    message->setObjectName("decorationMessage");
    message->setTextFormat(Qt::PlainText);
    message->setWordWrap(true);
    layout->addWidget(message);
    auto buttons = new QDialogButtonBox;
    remove = buttons->addButton("この設定を全削除", QDialogButtonBox::DestructiveRole);
    remove->setObjectName("decorationRemove");
    apply = buttons->addButton("対象ページへ適用", QDialogButtonBox::AcceptRole);
    apply->setObjectName("decorationApply");
    apply->setProperty("primary", true);
    cancel = buttons->addButton("キャンセル", QDialogButtonBox::RejectRole);
    cancel->setObjectName("decorationCancel");
    layout->addWidget(buttons);
    connect(apply, &QPushButton::clicked, this, &PageDecorationDialog::accept);
    connect(remove, &QPushButton::clicked, this, [this] { launch(Task::Remove); });
    connect(cancel, &QPushButton::clicked, this, &PageDecorationDialog::reject);
    connect(group, &QComboBox::currentIndexChanged, this, [this] { selectGroup(); });
    connect(scope, &QComboBox::currentIndexChanged, this, [this] { changed(); });
    connect(range, &QLineEdit::textChanged, this, [this] { changed(); });
    connect(page, &QComboBox::currentIndexChanged, this, [this] { changed(); });
    connect(font, &QComboBox::currentIndexChanged, this, [this] { changed(); });
    connect(color, &QComboBox::currentIndexChanged, this, [this] { changed(); });
    connect(watermark, &QLineEdit::textChanged, this, [this] { changed(); });
    connect(start, &QSpinBox::valueChanged, this, [this] { changed(); });
    connect(opacity, &QSpinBox::valueChanged, this, [this] { changed(); });
    for (auto entry : entries)
        if (entry)
            connect(entry, &QLineEdit::textChanged, this, [this] { changed(); });
    timer.setSingleShot(true);
    timer.setInterval(150);
    connect(&timer, &QTimer::timeout, this, [this] { launch(Task::Preview); });
    if (group->count() > 1)
        group->setCurrentIndex(1);
    else
        changed();
}
PageDecorationDialog::~PageDecorationDialog()
{
    if (cancelled)
        *cancelled = true;
    if (job)
        job->wait();
}
QVector<int> PageDecorationDialog::selectedPages() const
{
    const int count = int(snapshot.getCatalog()->getPageCount());
    if (scope->currentIndex() == 1)
        return {current};
    if (scope->currentIndex() == 2)
        return parsePages(range->text(), count);
    QVector<int> result;
    for (int i = 0; i < count; ++i)
        result << i;
    return result;
}
DecorationOptions PageDecorationDialog::options() const
{
    DecorationOptions value;
    value.kind = kind;
    if (kind == DecorationKind::HeaderFooter)
        for (int i = 0; i < 3; ++i)
        {
            value.header[i] = entries[i]->text();
            value.footer[i] = entries[i + 3]->text();
        }
    value.watermark = watermark->text();
    value.fontFamily = font->family();
    value.size = size->value();
    value.color = QColor(findChild<QComboBox*>("decorationColor")->currentData().toString());
    value.margins = {left->value(), top->value(), right->value(), bottom->value()};
    value.startNumber = start->value();
    value.angle = angle->value();
    value.opacity = opacity->value() / 100.0;
    return value;
}
void PageDecorationDialog::selectGroup()
{
    if (working)
        return;
    const auto id = group->currentData().toString();
    for (const auto& value : groups)
        if (value.id == id)
        {
            const auto& option = value.options;
            if (kind == DecorationKind::HeaderFooter)
                for (int i = 0; i < 3; ++i)
                {
                    entries[i]->setText(option.header[i]);
                    entries[i + 3]->setText(option.footer[i]);
                }
            watermark->setText(option.watermark);
            font->setFamily(option.fontFamily);
            size->setValue(option.size);
            auto color = findChild<QComboBox*>("decorationColor");
            int index = color->findData(option.color.name());
            if (index < 0)
            {
                color->addItem(option.color.name(), option.color.name());
                index = color->count() - 1;
            }
            color->setCurrentIndex(index);
            left->setValue(option.margins.left());
            top->setValue(option.margins.top());
            right->setValue(option.margins.right());
            bottom->setValue(option.margins.bottom());
            start->setValue(option.startNumber);
            angle->setValue(option.angle);
            opacity->setValue(qRound(option.opacity * 100));
            scope->setCurrentIndex(2);
            QStringList numbers;
            for (int number : value.pages)
                numbers << QString::number(number + 1);
            range->setText(numbers.join(','));
            page->setCurrentIndex(value.pages.front());
            break;
        }
    changed();
}
void PageDecorationDialog::changed()
{
    if (working)
        return;
    ++generation;
    if (cancelled)
        *cancelled = true;
    range->setEnabled(scope->currentIndex() == 2);
    apply->setEnabled(false);
    remove->setEnabled(!job && !group->currentData().toString().isEmpty());
    previewImage = {};
    preview->setPixmap({});
    preview->setText("プレビューを更新しています…");
    message->setText("設定を確認しています。文書にはまだ反映していません。");
    timer.start();
}
void PageDecorationDialog::setWorking(bool value)
{
    working = value;
    settings->setEnabled(!value);
    page->setEnabled(!value);
    apply->setEnabled(!value && ready == generation);
    remove->setEnabled(!value && !group->currentData().toString().isEmpty());
    cancel->setEnabled(true);
    cancel->setText(value ? "処理を中止" : "キャンセル");
}
void PageDecorationDialog::launch(Task task)
{
    if (job || working || closing)
        return;
    QVector<int> pages;
    try
    {
        if (task != Task::Remove)
            pages = selectedPages();
        if (task == Task::Preview && !pages.contains(page->currentData().toInt()))
        {
            page->setCurrentIndex(pages.front());
            return;
        }
    }
    catch (const std::exception& error)
    {
        message->setText(QString::fromUtf8(error.what()));
        return;
    }
    const auto option = options();
    const auto id = group->currentData().toString();
    const auto revision = generation;
    const int number = page->currentData().toInt();
    auto stop = std::make_shared<std::atomic_bool>(false);
    cancelled = stop;
    if (task != Task::Preview)
    {
        timer.stop();
        setWorking(true);
        message->setText("候補PDFを作成しています。完成するまで文書は変更しません。");
    }
    else
        remove->setEnabled(false);
    struct Result
    {
        std::optional<PDFDocument> document;
        QImage image;
        QString error;
    };
    auto result = std::make_shared<Result>();
    job = QThread::create(
        [this, document = snapshot, pages, option, id, number, revision, stop, task,
         result]() mutable
        {
            try
            {
                auto isCancelled = [stop] { return stop->load(); };
                if (task == Task::Remove)
                    result->document = removeDecoration(document, id, isCancelled);
                else if (task == Task::Apply)
                    result->document = putDecoration(
                        document, pages, option, id, isCancelled,
                        [this, revision](int completed, int total)
                        {
                            QMetaObject::invokeMethod(
                                this,
                                [this, revision, completed, total]
                                {
                                    if (revision == generation && working)
                                        message->setText(QString("作成 %1 / %2 ページ")
                                                             .arg(completed)
                                                             .arg(total));
                                },
                                Qt::QueuedConnection);
                        });
                else
                {
                    auto candidate =
                        previewDecoration(document, pages, number, option, id, isCancelled);
                    const auto physical = pageSize(candidate.getCatalog()->getPage(number));
                    result->image =
                        renderPage(candidate, number,
                                   qMin(1.0, 360.0 / qMax(physical.width(), physical.height())));
                }
            }
            catch (const std::exception& error)
            {
                result->error = QString::fromUtf8(error.what());
            }
            catch (...)
            {
                result->error = "ページ装飾を作成できません。文書は変更していません。";
            }
        });
    auto launched = job;
    launched->setParent(this);
    connect(launched, &QThread::finished, this,
            [this, launched, result, stop, revision, task]
            {
                job = nullptr;
                launched->deleteLater();
                if (closing)
                {
                    QDialog::reject();
                    return;
                }
                if (task == Task::Preview)
                {
                    if (revision != generation || *stop)
                    {
                        timer.start();
                        return;
                    }
                    remove->setEnabled(!group->currentData().toString().isEmpty());
                    if (!result->image.isNull())
                    {
                        previewImage = result->image;
                        preview->setPixmap(QPixmap::fromImage(result->image)
                                               .scaled(preview->size(), Qt::KeepAspectRatio,
                                                       Qt::SmoothTransformation));
                        ready = revision;
                        apply->setEnabled(true);
                        message->setText(
                            "全対象の配置を確認しました。適用すると1回のUndoで取り消せます。");
                    }
                    else
                    {
                        preview->setText("設定を確認してください");
                        message->setText(result->error);
                    }
                }
                else
                {
                    setWorking(false);
                    if (*stop || !result->document)
                        message->setText(
                            *stop ? "処理を中止しました。元の文書と未保存変更を保持しています。"
                                  : result->error);
                    else
                    {
                        candidate = std::move(result->document);
                        QDialog::accept();
                    }
                }
            });
    launched->start();
}
void PageDecorationDialog::accept()
{
    if (apply->isEnabled() && ready == generation)
        launch(Task::Apply);
}
void PageDecorationDialog::reject()
{
    timer.stop();
    if (job)
    {
        closing = true;
        *cancelled = true;
        cancel->setEnabled(false);
        message->setText("処理を中止しています。文書は変更していません。");
    }
    else
        QDialog::reject();
}
void PageDecorationDialog::closeEvent(QCloseEvent* event)
{
    if (job)
    {
        event->ignore();
        reject();
    }
    else
        QDialog::closeEvent(event);
}
void PageDecorationDialog::resizeEvent(QResizeEvent* event)
{
    QDialog::resizeEvent(event);
    if (!previewImage.isNull())
        preview->setPixmap(
            QPixmap::fromImage(previewImage)
                .scaled(preview->size(), Qt::KeepAspectRatio, Qt::SmoothTransformation));
}
PDFDocument PageDecorationDialog::takeDocument()
{
    if (!candidate || job || QDialog::result() != QDialog::Accepted)
        fail("完成した候補PDFがありません。");
    auto result = std::move(*candidate);
    candidate.reset();
    return result;
}
} // namespace tatsu
