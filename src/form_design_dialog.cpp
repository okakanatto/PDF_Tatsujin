#include "form_design_dialog.h"
#include <memory>

namespace tatsu
{
namespace
{
constexpr double mm = 72.0 / 25.4;
QString id()
{
    return QUuid::createUuid().toString(QUuid::WithoutBraces);
}
QString kindName(FormKind kind)
{
    const QStringList names{"単行テキスト",   "複数行テキスト", "チェックボックス",
                            "ラジオボタン群", "コンボボックス", "リスト"};
    return names.value(int(kind), "非対応");
}
} // namespace
FormDesignDialog::FormDesignDialog(PDFDocument document, int currentPage, QWidget* parent)
    : QDialog(parent), snapshot(std::move(document)), values(formDesign(snapshot))
{
    setObjectName("formDesignDialog");
    setWindowTitle("フォームを設計");
    resize(1100, 700);
    setMinimumSize(800, 480);
    for (const auto& field : formFields(snapshot))
    {
        const auto owner = snapshot.getObjectByReference(field.field);
        if (!owner.isDictionary() || !owner.getDictionary()->get("TatsujinForm").isString())
            foreign << field;
    }
    auto layout = new QVBoxLayout(this);
    auto note = new QLabel("種類を選んで範囲を描くと追加できます。中央で移動、右下でサイズ変更。最"
                           "後に文書へ適用します。");
    note->setWordWrap(true);
    layout->addWidget(note);
    settings = new QWidget;
    auto body = new QHBoxLayout(settings);
    body->setContentsMargins(0, 0, 0, 0);
    auto navigator = new QWidget;
    navigator->setMinimumWidth(165);
    navigator->setMaximumWidth(210);
    auto navigation = new QVBoxLayout(navigator);
    navigation->setContentsMargins(0, 0, 0, 0);
    page = new QComboBox;
    page->setObjectName("formDesignPage");
    page->setAccessibleName("フォームを配置するページ");
    for (int i = 0; i < int(snapshot.getCatalog()->getPageCount()); ++i)
        page->addItem(QString("%1ページ").arg(i + 1));
    page->setCurrentIndex(qBound(0, currentPage, page->count() - 1));
    navigation->addWidget(page);
    navigation->addWidget(new QLabel("追加する種類"));
    kind = new QComboBox;
    kind->setObjectName("formDesignKind");
    for (int i = 0; i < 6; ++i)
        kind->addItem(kindName(FormKind(i)));
    navigation->addWidget(kind);
    draw = new QPushButton("範囲を描いて追加");
    draw->setObjectName("formDesignDraw");
    draw->setCheckable(true);
    navigation->addWidget(draw);
    list = new QListWidget;
    list->setObjectName("formDesignList");
    list->setAccessibleName("選択ページのフォーム項目");
    navigation->addWidget(list, 1);
    remove = new QPushButton("選択した項目を削除");
    remove->setObjectName("formDesignDelete");
    navigation->addWidget(remove);
    auto hint =
        new QLabel("既存の他社フォームは保持します。ラジオ群の削除は選択肢全体へ適用します。");
    hint->setWordWrap(true);
    navigation->addWidget(hint);
    body->addWidget(navigator);
    preview = new PageRegionPreview;
    preview->setObjectName("formDesignPreview");
    preview->minimumSide = 6;
    preview->setAccessibleName(
        "フォーム配置のプレビュー。中央で移動、右下でサイズ変更。数値欄でも変更できます");
    body->addWidget(preview, 1);
    properties = new QWidget;
    auto column = new QVBoxLayout(properties);
    auto form = new QFormLayout;
    form->setRowWrapPolicy(QFormLayout::WrapAllRows);
    name = new QLineEdit;
    name->setObjectName("formDesignName");
    name->setMaxLength(256);
    form->addRow("内部名（文書内で一意）", name);
    caption = new QLineEdit;
    caption->setObjectName("formDesignCaption");
    caption->setMaxLength(1024);
    form->addRow("表示ラベル（入力欄の説明）", caption);
    initial = new QPlainTextEdit;
    initial->setObjectName("formDesignInitial");
    initial->setMinimumHeight(62);
    initial->setMaximumHeight(95);
    form->addRow("値（複数選択は1行1値）", initial);
    maxLength = new QSpinBox;
    maxLength->setObjectName("formDesignMaxLength");
    maxLength->setRange(0, 65536);
    maxLength->setSpecialValueText("制限なし");
    form->addRow("最大文字数", maxLength);
    column->addLayout(form);
    readOnly = new QCheckBox("入力は読取専用");
    readOnly->setObjectName("formDesignReadOnly");
    required = new QCheckBox("必須の項目");
    required->setObjectName("formDesignRequired");
    editableChoice = new QCheckBox("選択肢以外も入力可");
    editableChoice->setObjectName("formDesignEditableChoice");
    multiple = new QCheckBox("複数の選択を許可");
    multiple->setObjectName("formDesignMultiple");
    for (auto control : {readOnly, required, editableChoice, multiple})
        column->addWidget(control);
    column->addWidget(new QLabel("選択肢（値と表示を分けます）"));
    options = new QTableWidget(0, 2);
    options->setObjectName("formDesignOptions");
    options->setAccessibleName("フォームの選択値と表示ラベル");
    options->setHorizontalHeaderLabels({"保存する値", "表示ラベル"});
    options->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    options->verticalHeader()->hide();
    options->setMinimumHeight(90);
    options->setMaximumHeight(150);
    column->addWidget(options);
    auto buttons = new QHBoxLayout;
    addOption = new QPushButton("選択肢を追加");
    addOption->setObjectName("formDesignAddOption");
    removeOption = new QPushButton("選択肢を削除");
    removeOption->setObjectName("formDesignRemoveOption");
    buttons->addWidget(addOption);
    buttons->addWidget(removeOption);
    column->addLayout(buttons);
    auto geometry = new QGridLayout;
    auto coordinate = [&](const QString& label, const QString& key, int row, int col)
    {
        auto control = new QDoubleSpinBox;
        control->setObjectName(key);
        control->setRange(-100000, 100000);
        control->setDecimals(4);
        control->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
        control->setMinimumWidth(70);
        control->setSuffix(" mm");
        control->setAccessibleName("フォームの" + label);
        geometry->addWidget(new QLabel(label), row * 2, col);
        geometry->addWidget(control, row * 2 + 1, col);
        connect(control, &QDoubleSpinBox::valueChanged, this, [this] { updateSettings(); });
        return control;
    };
    x = coordinate("左", "formDesignX", 0, 0);
    y = coordinate("上", "formDesignY", 0, 1);
    width = coordinate("幅", "formDesignWidth", 1, 0);
    height = coordinate("高さ", "formDesignHeight", 1, 1);
    column->addLayout(geometry);
    column->addStretch();
    auto scroll = new QScrollArea;
    scroll->setObjectName("formDesignProperties");
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    scroll->setWidget(properties);
    scroll->setMinimumWidth(245);
    scroll->setMaximumWidth(290);
    body->addWidget(scroll);
    layout->addWidget(settings, 1);
    message = new QLabel("候補を編集しています。元の文書はまだ変更していません。");
    message->setObjectName("formDesignMessage");
    message->setTextFormat(Qt::PlainText);
    message->setWordWrap(true);
    layout->addWidget(message);
    auto actions = new QDialogButtonBox;
    apply = actions->addButton("文書へ適用", QDialogButtonBox::AcceptRole);
    apply->setObjectName("formDesignApply");
    apply->setProperty("primary", true);
    cancel = actions->addButton("キャンセル", QDialogButtonBox::RejectRole);
    cancel->setObjectName("formDesignCancel");
    layout->addWidget(actions);
    debounce = new QTimer(this);
    debounce->setSingleShot(true);
    debounce->setInterval(180);
    connect(debounce, &QTimer::timeout, this, &FormDesignDialog::loadPreview);
    connect(page, &QComboBox::currentIndexChanged, this,
            [this]
            {
                draw->setChecked(false);
                selected = -1;
                preview->image = {};
                rebuild();
                schedulePreview();
            });
    connect(list, &QListWidget::currentItemChanged, this, [this](QListWidgetItem* item)
            { select(item ? item->data(Qt::UserRole).toInt() : -1); });
    connect(draw, &QPushButton::toggled, this,
            [this](bool value)
            {
                preview->drawing = value;
                preview->setCursor(value ? Qt::CrossCursor : Qt::ArrowCursor);
            });
    connect(remove, &QPushButton::clicked, this,
            [this]
            {
                if (selected < 0 || selected >= positions.size())
                    return;
                values.removeAt(positions[selected].first);
                rebuild();
                schedulePreview();
            });
    connect(name, &QLineEdit::textChanged, this, [this] { updateSettings(); });
    connect(caption, &QLineEdit::textChanged, this, [this] { updateSettings(); });
    connect(initial, &QPlainTextEdit::textChanged, this, [this] { updateSettings(); });
    connect(maxLength, &QSpinBox::valueChanged, this, [this] { updateSettings(); });
    for (auto control : {readOnly, required, editableChoice, multiple})
        connect(control, &QCheckBox::toggled, this, [this] { updateSettings(); });
    connect(options, &QTableWidget::itemChanged, this, [this] { updateSettings(); });
    connect(addOption, &QPushButton::clicked, this,
            [this]
            {
                if (options->rowCount() >= 1000)
                    return;
                loading = true;
                int row = options->rowCount();
                options->insertRow(row);
                options->setItem(row, 0, new QTableWidgetItem(QString("choice%1").arg(row + 1)));
                options->setItem(row, 1, new QTableWidgetItem(QString("選択肢 %1").arg(row + 1)));
                loading = false;
                updateSettings();
            });
    connect(
        removeOption, &QPushButton::clicked, this,
        [this]
        {
            if (options->currentRow() < 0)
                return;
            if (options->rowCount() <= 1)
            {
                message->setText(
                    "選択肢は1件以上必要です。項目全体を消す場合は左側の削除を使ってください。");
                return;
            }
            loading = true;
            options->removeRow(options->currentRow());
            loading = false;
            updateSettings();
        });
    connect(apply, &QPushButton::clicked, this, &FormDesignDialog::accept);
    connect(cancel, &QPushButton::clicked, this, &FormDesignDialog::reject);
    preview->choose = [this](int index)
    {
        for (int row = 0; row < list->count(); ++row)
            if (list->item(row)->data(Qt::UserRole).toInt() == index)
            {
                list->setCurrentRow(row);
                return;
            }
        list->setCurrentRow(-1);
    };
    preview->create = [this](QRectF box) { add(box); };
    preview->cancelDrawing = [this] { draw->setChecked(false); };
    preview->move = [this](int index, QRectF box)
    {
        if (index < 0 || index >= positions.size())
            return;
        const auto [field, widget] = positions[index];
        values[field].widgets[widget].rectangle = box;
        select(index);
        updatePreview();
        schedulePreview();
    };
    rebuild();
    loadPreview();
}
FormDesignDialog::~FormDesignDialog()
{
    cancelled = true;
    for (auto job : {renderJob, applyJob})
        if (job)
        {
            disconnect(job, nullptr, this, nullptr);
            job->wait();
        }
}
void FormDesignDialog::rebuild(int selection)
{
    QSignalBlocker blocker(list);
    list->clear();
    positions.clear();
    QListWidgetItem* chosen = nullptr;
    for (int field = 0; field < values.size(); ++field)
        for (int widget = 0; widget < values[field].widgets.size(); ++widget)
        {
            const int index = positions.size();
            positions << qMakePair(field, widget);
            if (values[field].widgets[widget].page != page->currentIndex())
                continue;
            auto item = new QListWidgetItem(values[field].name +
                                                (values[field].kind == FormKind::Radio
                                                     ? " / " + values[field].labels.value(widget)
                                                     : ""),
                                            list);
            item->setToolTip(kindName(values[field].kind));
            item->setData(Qt::UserRole, index);
            if (index == selection)
                chosen = item;
        }
    for (int i = 0; i < foreign.size(); ++i)
        if (foreign[i].page == page->currentIndex())
        {
            auto item = new QListWidgetItem(foreign[i].qualifiedName + "（既存）", list);
            item->setData(Qt::UserRole, 100000 + i);
            item->setToolTip("設計は保持します。通常の入力画面から入力できます。");
        }
    if (!chosen && list->count())
        chosen = list->item(0);
    list->setCurrentItem(chosen);
    select(chosen ? chosen->data(Qt::UserRole).toInt() : -1);
}
void FormDesignDialog::select(int index)
{
    selected = index >= 0 && index < positions.size() ? index : -1;
    properties->setEnabled(selected >= 0);
    remove->setEnabled(selected >= 0);
    loading = true;
    if (selected >= 0)
    {
        const auto [field, widget] = positions[selected];
        const auto& value = values[field];
        const auto& box = value.widgets[widget].rectangle;
        name->setText(value.name);
        caption->setText(value.caption);
        initial->setPlainText(value.values.join('\n'));
        maxLength->setValue(value.maxLength);
        maxLength->setEnabled(value.kind == FormKind::Text || value.kind == FormKind::Multiline);
        readOnly->setChecked(value.readOnly);
        required->setChecked(value.required);
        editableChoice->setChecked(value.editableChoice);
        editableChoice->setEnabled(value.kind == FormKind::Combo);
        multiple->setChecked(value.multiple);
        multiple->setEnabled(value.kind == FormKind::List);
        const bool choices = value.kind == FormKind::Combo || value.kind == FormKind::List ||
                             value.kind == FormKind::Checkbox || value.kind == FormKind::Radio;
        options->setEnabled(choices);
        addOption->setEnabled(choices && value.kind != FormKind::Checkbox);
        removeOption->setEnabled(choices && value.kind != FormKind::Checkbox);
        options->setRowCount(value.exports.size());
        for (int i = 0; i < value.exports.size(); ++i)
        {
            options->setItem(i, 0, new QTableWidgetItem(value.exports[i]));
            options->setItem(i, 1, new QTableWidgetItem(value.labels[i]));
        }
        x->setValue(box.x() / mm);
        y->setValue(box.y() / mm);
        width->setValue(box.width() / mm);
        height->setValue(box.height() / mm);
    }
    loading = false;
    updatePreview();
}
void FormDesignDialog::updateSettings()
{
    if (loading || selected < 0 || applyJob)
        return;
    const auto [field, widget] = positions[selected];
    auto& value = values[field];
    const auto selectedId = value.widgets[widget].id;
    const int oldWidgetCount = value.widgets.size();
    value.name = name->text();
    value.caption = caption->text();
    const auto text = initial->toPlainText();
    value.values = value.kind == FormKind::List
                       ? (text.isEmpty() ? QStringList{} : text.split('\n'))
                       : QStringList{text};
    value.readOnly = readOnly->isChecked();
    value.required = required->isChecked();
    value.editableChoice = editableChoice->isChecked();
    value.multiple = multiple->isChecked();
    value.maxLength = maxLength->value();
    value.exports.clear();
    value.labels.clear();
    for (int row = 0; row < options->rowCount(); ++row)
    {
        value.exports << (options->item(row, 0) ? options->item(row, 0)->text() : "");
        value.labels << (options->item(row, 1) ? options->item(row, 1)->text() : "");
    }
    // Text editing must not round untouched PDF coordinates or reset the editor
    // caret. Update only the numeric control that actually changed.
    if (auto coordinate = qobject_cast<QDoubleSpinBox*>(sender()))
    {
        auto& box = value.widgets[widget].rectangle;
        if (coordinate == x)
            box.moveLeft(x->value() * mm);
        else if (coordinate == y)
            box.moveTop(y->value() * mm);
        else if (coordinate == width)
            box.setWidth(width->value() * mm);
        else if (coordinate == height)
            box.setHeight(height->value() * mm);
    }
    if (value.kind == FormKind::Radio)
    {
        while (value.widgets.size() < value.exports.size())
        {
            const auto base = value.widgets.front();
            value.widgets << FormDesignWidget{
                id(), base.page,
                base.rectangle.translated(0, value.widgets.size() * (base.rectangle.height() + 8))};
        }
        while (value.widgets.size() > value.exports.size())
            value.widgets.removeLast();
    }
    int next = -1, index = 0;
    for (const auto& item : values)
        for (const auto& placement : item.widgets)
        {
            if (placement.id == selectedId)
                next = index;
            ++index;
        }
    if (value.widgets.size() != oldWidgetCount)
        rebuild(next);
    else
        for (int row = 0; row < list->count(); ++row)
        {
            auto item = list->item(row);
            const int index = item->data(Qt::UserRole).toInt();
            if (index >= 0 && index < positions.size() && positions[index].first == field)
                item->setText(value.name +
                              (value.kind == FormKind::Radio
                                   ? " / " + value.labels.value(positions[index].second)
                                   : ""));
        }
    schedulePreview();
}
void FormDesignDialog::updatePreview()
{
    preview->regions.clear();
    for (int i = 0; i < positions.size(); ++i)
    {
        const auto [field, widget] = positions[i];
        const auto& placement = values[field].widgets[widget];
        if (placement.page == page->currentIndex())
            preview->regions << qMakePair(i, placement.rectangle);
    }
    preview->selected = selected;
    preview->update();
}
void FormDesignDialog::add(QRectF box)
{
    draw->setChecked(false);
    if (positions.size() >= 1000)
    {
        message->setText("フォームは1000widgetまでです。");
        return;
    }
    FormDesignEntry value;
    value.id = id();
    value.kind = FormKind(kind->currentIndex());
    QSet<QString> names;
    for (const auto& entry : values)
        names.insert(entry.name);
    for (const auto& entry : foreign)
        names.insert(entry.qualifiedName);
    int number = 1;
    do
    {
        value.name = QString("field%1").arg(number++);
    } while (names.contains(value.name));
    value.caption = kindName(value.kind);
    value.widgets << FormDesignWidget{id(), page->currentIndex(), box};
    value.values = {""};
    if (value.kind == FormKind::Checkbox || value.kind == FormKind::Radio ||
        value.kind == FormKind::Combo || value.kind == FormKind::List)
    {
        value.exports = {"choice1"};
        value.labels = {"選択肢 1"};
        value.values = (value.kind == FormKind::Checkbox || value.kind == FormKind::Radio)
                           ? QStringList{"Off"}
                           : QStringList{"choice1"};
        if (value.kind != FormKind::Checkbox)
        {
            value.exports << "choice2";
            value.labels << "選択肢 2";
            if (value.kind == FormKind::Radio)
                value.widgets << FormDesignWidget{id(), page->currentIndex(),
                                                  box.translated(0, box.height() + 8)};
        }
    }
    if (positions.size() + value.widgets.size() > 1000)
    {
        message->setText("フォームは1000widgetまでです。");
        return;
    }
    values << value;
    rebuild(positions.size());
    name->setFocus();
    name->selectAll();
    schedulePreview();
}
void FormDesignDialog::schedulePreview()
{
    ++generation;
    message->setText("候補を確認しています。直近の有効な候補を表示しています。文書へ適用するまで原"
                     "本は保持します。");
    updatePreview();
    debounce->start();
}
void FormDesignDialog::loadPreview()
{
    if (renderJob || applyJob || cancelled)
        return;
    struct Result
    {
        QImage image;
        QSizeF physical;
        QString error;
    };
    auto result = std::make_shared<Result>();
    const int number = page->currentIndex();
    const auto launchedGeneration = generation;
    renderJob = QThread::create(
        [this, document = snapshot, edited = values, number, result]() mutable
        {
            try
            {
                auto candidate =
                    replaceFormDesign(document, edited, [this] { return cancelled.load(); });
                result->physical = pageSize(candidate.getCatalog()->getPage(number));
                result->image = renderPage(
                    candidate, number,
                    qMin(1.5, 1000.0 / qMax(result->physical.width(), result->physical.height())));
            }
            catch (const std::exception& error)
            {
                result->error = QString::fromUtf8(error.what());
            }
            catch (...)
            {
                result->error = "フォームの候補を表示できません。元の文書は保持しています。";
            }
        });
    auto launched = renderJob;
    launched->setParent(this);
    connect(launched, &QThread::finished, this,
            [this, launched, number, launchedGeneration, result]
            {
                renderJob = nullptr;
                launched->deleteLater();
                if (cancelled || applyJob)
                    return;
                if (launchedGeneration != generation || number != page->currentIndex())
                {
                    loadPreview();
                    return;
                }
                if (!result->image.isNull())
                {
                    preview->image = result->image;
                    preview->physical = result->physical;
                    preview->setProperty("shownPage", number);
                    preview->setProperty("generation", QVariant::fromValue<qulonglong>(generation));
                    draw->setEnabled(true);
                    message->setText(
                        "候補を表示しています。文書へ適用すると、まとめて1回でUndoできます。");
                }
                else
                    message->setText(result->error + " 表示は直近の有効な候補です。");
                updatePreview();
            });
    draw->setEnabled(!preview->image.isNull());
    launched->start();
}
void FormDesignDialog::accept()
{
    if (applyJob)
        return;
    if (auto input = QGuiApplication::inputMethod())
        input->commit();
    if (auto focused = focusWidget())
        focused->clearFocus();
    draw->setChecked(false);
    debounce->stop();
    settings->setEnabled(false);
    apply->setEnabled(false);
    cancelled = false;
    message->setText("フォームの候補を確認しています。元の文書は変更していません。");
    struct Result
    {
        std::optional<PDFDocument> document;
        QString error;
    };
    auto result = std::make_shared<Result>();
    const auto attempt = ++generation;
    applyJob = QThread::create(
        [this, document = snapshot, edited = values, result, attempt]
        {
            try
            {
                result->document = replaceFormDesign(
                    document, edited, [this] { return cancelled.load(); },
                    [this, attempt](int completed, int total)
                    {
                        if (completed != 1 && completed != total && completed % 25)
                            return;
                        QMetaObject::invokeMethod(
                            this,
                            [this, attempt, completed, total]
                            {
                                if (applyJob && !cancelled && generation == attempt)
                                    message->setText(QString("フォームの候補を作成しています… %1 / "
                                                             "%2。元の文書は保持しています。")
                                                         .arg(completed)
                                                         .arg(total));
                            },
                            Qt::QueuedConnection);
                    });
            }
            catch (const std::exception& error)
            {
                result->error = QString::fromUtf8(error.what());
            }
            catch (...)
            {
                result->error = "フォームを設計できません。元の文書は保持しています。";
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
void FormDesignDialog::reject()
{
    cancelled = true;
    debounce->stop();
    if (applyJob)
    {
        cancel->setEnabled(false);
        message->setText("中止しています。元の文書は変更していません。");
    }
    else
        QDialog::reject();
}
void FormDesignDialog::closeEvent(QCloseEvent* event)
{
    if (applyJob)
    {
        event->ignore();
        reject();
    }
    else
        QDialog::closeEvent(event);
}
PDFDocument FormDesignDialog::takeDocument()
{
    if (!candidate || applyJob || result() != QDialog::Accepted)
        fail("完成したフォームの候補がありません。");
    auto value = std::move(*candidate);
    candidate.reset();
    return value;
}
} // namespace tatsu
