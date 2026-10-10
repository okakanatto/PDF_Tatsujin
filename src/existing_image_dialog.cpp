#include "existing_image_dialog.h"
#include "ui_widgets.h"
#include <QImageReader>

namespace tatsu
{
namespace
{
constexpr double mm = 72.0 / 25.4;
}
ExistingImageDialog::ExistingImageDialog(PDFDocument document, int currentPage,
                                         std::function<void()> validate, QWidget* parent)
    : QDialog(parent), snapshot(std::move(document)), validate(std::move(validate))
{
    setObjectName("existingImageDialog");
    setWindowTitle("PDF内の画像を編集");
    resize(1024, 700);
    setMinimumSize(760, 480);
    auto layout = new QVBoxLayout(this);
    auto note = new QLabel("画像をページ上で選び、移動・サイズ変更・差替え・削除できます。実際の変"
                           "更をプレビューしてから、選んだ1箇所へ適用します。");
    note->setWordWrap(true);
    layout->addWidget(note);
    auto body = new QHBoxLayout;
    auto left = new QVBoxLayout;
    auto zoom = new QHBoxLayout;
    preview = new PageRegionPreview;
    preview->setObjectName("existingImagePreview");
    preview->navigationEnabled = true;
    preview->zoomChanged = [this](double) { schedule(); };
    preview->setAccessibleName("画像編集のページプレビュー");
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
    page->setObjectName("existingImagePage");
    page->setAccessibleName("画像を編集するページ");
    for (int index = 0; index < int(snapshot.getCatalog()->getPageCount()); ++index)
        page->addItem(QString("%1ページ").arg(index + 1));
    page->setCurrentIndex(qBound(0, currentPage, page->count() - 1));
    column->addWidget(page);
    list = new QListWidget;
    list->setObjectName("existingImageList");
    list->setAccessibleName("本文の画像一覧");
    list->setMaximumHeight(140);
    column->addWidget(list);
    operation = new QComboBox;
    operation->setObjectName("existingImageOperation");
    operation->setAccessibleName("画像の変更内容");
    operation->addItems({"位置・サイズを変更", "画像を差し替え", "画像を削除"});
    column->addWidget(operation);
    auto coordinates = new QFormLayout;
    auto number = [&](const QString& name, const QString& label)
    {
        auto value = new QDoubleSpinBox;
        value->setObjectName(name);
        value->setAccessibleName(label);
        value->setDecimals(4);
        value->setRange(-1000000, 1000000);
        value->setSuffix(" mm");
        coordinates->addRow(label, value);
        return value;
    };
    x = number("existingImageX", "左から");
    y = number("existingImageY", "上から");
    width = number("existingImageWidth", "幅");
    height = number("existingImageHeight", "高さ");
    width->setMinimum(.3528);
    height->setMinimum(.3528);
    column->addLayout(coordinates);
    aspect = new QCheckBox("縦横比を保つ");
    aspect->setObjectName("existingImageAspect");
    aspect->setChecked(true);
    column->addWidget(aspect);
    replacementPath = new QLineEdit;
    replacementPath->setObjectName("existingImageReplacement");
    replacementPath->setAccessibleName("差替えるPNG/JPEGのパス");
    column->addWidget(replacementPath);
    pick = new QPushButton("差替える画像を選ぶ…");
    pick->setAutoDefault(false);
    column->addWidget(pick);
    confirmDelete = new QCheckBox("選んだ画像を表示から削除する");
    confirmDelete->setObjectName("existingImageDeleteConsent");
    column->addWidget(confirmDelete);
    auto limits = new QLabel(
        "追加した画像は文書画面でも編集できます。Form内の画像・一部クリッピング等は未対応です。画像"
        "に対応するOCRがある場合は変更を中止します。削除は墨消しではありません。");
    limits->setWordWrap(true);
    column->addWidget(limits);
    column->addStretch();
    auto scroll = new QScrollArea;
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setMaximumWidth(340);
    scroll->setWidget(settings);
    body->addWidget(scroll);
    layout->addLayout(body, 1);
    message = new QLabel;
    message->setObjectName("existingImageMessage");
    message->setWordWrap(true);
    message->setTextFormat(Qt::PlainText);
    layout->addWidget(message);
    auto buttons = new QHBoxLayout;
    buttons->addStretch();
    apply = new QPushButton("変更を適用");
    apply->setObjectName("applyExistingImage");
    apply->setProperty("primary", true);
    apply->setAutoDefault(false);
    cancel = new QPushButton("キャンセル");
    cancel->setAutoDefault(false);
    buttons->addWidget(apply);
    buttons->addWidget(cancel);
    layout->addLayout(buttons);
    connect(apply, &QPushButton::clicked, this, &ExistingImageDialog::accept);
    connect(cancel, &QPushButton::clicked, this, &ExistingImageDialog::reject);
    connect(page, &QComboBox::currentIndexChanged, this, [this] { loadPage(); });
    connect(list, &QListWidget::currentRowChanged, this, &ExistingImageDialog::selectImage);
    connect(operation, &QComboBox::currentIndexChanged, this,
            [this]
            {
                confirmDelete->setChecked(false);
                schedule();
            });
    connect(confirmDelete, &QCheckBox::toggled, this, [this] { updateApply(); });
    connect(replacementPath, &QLineEdit::editingFinished, this,
            &ExistingImageDialog::loadReplacement);
    connect(pick, &QPushButton::clicked, this,
            [this]
            {
                const auto path = QFileDialog::getOpenFileName(this, "差替える画像", {},
                                                               "画像 (*.png *.jpg *.jpeg)");
                if (!path.isEmpty())
                {
                    replacementPath->setText(path);
                    loadReplacement();
                }
            });
    for (auto field : {x, y, width, height})
        connect(field, qOverload<double>(&QDoubleSpinBox::valueChanged), this,
                [this, field] { geometryChanged(field); });
    preview->choose = [this](int index) { list->setCurrentRow(index); };
    preview->move = [this](int index, QRectF rectangle)
    {
        if (index != list->currentRow())
            return;
        if (aspect->isChecked() && index >= 0)
        {
            const auto ratio = images[index].physical.width() / images[index].physical.height();
            if (qAbs(rectangle.width() - geometry.width()) >=
                qAbs(rectangle.height() - geometry.height()) * ratio)
                rectangle.setHeight(rectangle.width() / ratio);
            else
                rectangle.setWidth(rectangle.height() * ratio);
        }
        setGeometry(rectangle);
        schedule();
    };
    timer.setSingleShot(true);
    timer.setInterval(120);
    connect(&timer, &QTimer::timeout, this, &ExistingImageDialog::render);
    loadPage();
}
ExistingImageDialog::~ExistingImageDialog()
{
    stopped->store(true);
    if (job)
        job->wait();
}
void ExistingImageDialog::loadPage()
{
    {
        QSignalBlocker blocker(list);
        list->clear();
    }
    images.clear();
    pageError.clear();
    preview->image = {};
    preview->regions.clear();
    preview->selected = -1;
    preview->fitPage();
    try
    {
        images = existingImages(snapshot, page->currentIndex());
        for (int index = 0; index < images.size(); ++index)
            list->addItem(QString("画像 %1　%2 × %3 px")
                              .arg(index + 1)
                              .arg(images[index].pixels.width())
                              .arg(images[index].pixels.height()));
        if (images.isEmpty())
            selectImage(-1);
        else
            list->setCurrentRow(0);
    }
    catch (const std::exception& error)
    {
        pageError = QString::fromUtf8(error.what());
        message->setText(pageError);
        selectImage(-1);
    }
}
void ExistingImageDialog::selectImage(int index)
{
    replacement = {};
    replacementHash.clear();
    replacementPath->clear();
    confirmDelete->setChecked(false);
    {
        QSignalBlocker blocker(operation);
        operation->setCurrentIndex(0);
    }
    if (index >= 0 && index < images.size())
        setGeometry(images[index].physical);
    preview->selected = index;
    schedule();
}
void ExistingImageDialog::setGeometry(QRectF rectangle)
{
    geometry = rectangle;
    const QSignalBlocker bx(x), by(y), bw(width), bh(height);
    x->setValue(rectangle.x() / mm);
    y->setValue(rectangle.y() / mm);
    width->setValue(rectangle.width() / mm);
    height->setValue(rectangle.height() / mm);
}
void ExistingImageDialog::geometryChanged(QDoubleSpinBox* sender)
{
    const int index = list->currentRow();
    if (index < 0)
        return;
    auto rectangle = geometry;
    if (sender == x)
        rectangle.moveLeft(x->value() * mm);
    else if (sender == y)
        rectangle.moveTop(y->value() * mm);
    else if (sender == width)
        rectangle.setWidth(width->value() * mm);
    else if (sender == height)
        rectangle.setHeight(height->value() * mm);
    if (aspect->isChecked())
    {
        const double ratio = images[index].physical.width() / images[index].physical.height();
        if (sender == width)
            rectangle.setHeight(rectangle.width() / ratio);
        else if (sender == height)
            rectangle.setWidth(rectangle.height() * ratio);
    }
    geometry = rectangle;
    // Update only the coupled dimension. Reformatting the active editor here
    // interrupts decimal entry and rounds unrelated source coordinates.
    if (sender == width)
    {
        const QSignalBlocker blocker(height);
        height->setValue(rectangle.height() / mm);
    }
    else if (sender == height)
    {
        const QSignalBlocker blocker(width);
        width->setValue(rectangle.width() / mm);
    }
    schedule();
}
void ExistingImageDialog::loadReplacement()
{
    replacement = {};
    replacementHash.clear();
    {
        QSignalBlocker blocker(operation);
        operation->setCurrentIndex(1);
    }
    try
    {
        const auto path = replacementPath->text();
        QImageReader reader(path);
        reader.setAutoTransform(true);
        const auto size = reader.size();
        const auto format = reader.format();
        if ((format != "png" && format != "jpeg") || !size.isValid() || size.width() > 40000 ||
            size.height() > 40000 || qint64(size.width()) * size.height() > 160000000)
            fail("読めるPNG/JPEG画像を指定してください。");
        const auto before = fileHash(path);
        replacement = reader.read();
        if (replacement.isNull() || before != fileHash(path))
            fail("画像を読み込めないか、読み込み中に更新されました。");
        replacementHash = before;
        {
            QSignalBlocker blocker(operation);
            operation->setCurrentIndex(1);
        }
        schedule();
    }
    catch (const std::exception& error)
    {
        schedule();
        message->setText(QString::fromUtf8(error.what()));
    }
}
void ExistingImageDialog::updateApply()
{
    const int index = list->currentRow();
    const bool selected = index >= 0 && index < images.size();
    const bool deleting = operation->currentIndex() == 2;
    const bool changed =
        selected && (operation->currentIndex() != 0 || geometry != images[index].physical);
    apply->setEnabled(!closing && validPreview && changed &&
                      (!deleting || confirmDelete->isChecked()));
    for (auto field : {x, y, width, height})
        field->setEnabled(selected && !deleting && !closing);
    aspect->setEnabled(selected && !deleting && !closing);
    replacementPath->setEnabled(selected && operation->currentIndex() == 1 && !closing);
    pick->setEnabled(selected && !closing);
    confirmDelete->setVisible(deleting);
}
void ExistingImageDialog::schedule()
{
    ++generation;
    validPreview = false;
    preview->setProperty("renderReady", false);
    updateApply();
    timer.start();
}
void ExistingImageDialog::render()
{
    if (job || closing)
        return;
    const int number = page->currentIndex(), index = list->currentRow(),
              op = operation->currentIndex();
    const auto token = generation;
    const auto bounds = geometry;
    const auto pixels = replacement;
    const auto requestedPixels = qMin(3000.0, 1000.0 * preview->zoom());
    struct Result
    {
        PDFDocument document;
        QImage image;
        QSizeF size;
        QString error;
    };
    auto result = std::make_shared<Result>();
    const auto source = snapshot;
    const auto rows = images;
    const auto stop = stopped;
    message->setText("プレビューを更新しています…");
    job = QThread::create(
        [source, number, index, op, token, bounds, pixels, requestedPixels, rows, stop,
         result]() mutable
        {
            try
            {
                result->document = source;
                if (index >= 0 && index < rows.size() &&
                    (op != 0 || bounds != rows[index].physical))
                    result->document = editExistingImage(source, number, rows[index].occurrence,
                                                         ExistingImageChange(op), bounds, pixels,
                                                         [stop] { return stop->load(); });
                result->size = pageSize(source.getCatalog()->getPage(number));
                result->image =
                    renderPage(result->document, number,
                               requestedPixels / qMax(result->size.width(), result->size.height()));
            }
            catch (const std::exception& error)
            {
                result->error = QString::fromUtf8(error.what());
            }
            catch (...)
            {
                result->error = "画像の変更を確認できません。元の文書は保持しています。";
            }
        });
    auto launched = job;
    launched->setParent(this);
    connect(
        launched, &QThread::finished, this,
        [this, launched, token, number, index, op, result]
        {
            job = nullptr;
            launched->deleteLater();
            if (closing)
            {
                QDialog::reject();
                return;
            }
            if (token != generation)
            {
                timer.start(0);
                return;
            }
            preview->physical = result->size;
            if (!result->image.isNull())
                preview->image = result->image;
            preview->regions.clear();
            for (int row = 0; row < images.size(); ++row)
                preview->regions << qMakePair(row, row == index ? geometry : images[row].physical);
            preview->selected = index;
            preview->update();
            validPreview = result->error.isEmpty() && !result->image.isNull();
            candidate = result->document;
            preview->setProperty("shownPage", number);
            preview->setProperty("renderReady", validPreview);
            message->setText(
                !result->error.isEmpty() ? result->error
                : !pageError.isEmpty()   ? pageError
                : index < 0
                    ? "このページに対応する本文の画像はありません。Form内の画像等は未対応です。"
                    : "変更はまだ適用していません。画像・ページの切替では未適用の変更が戻ります。");
            updateApply();
        });
    launched->start();
}
void ExistingImageDialog::accept()
{
    if (!apply->isEnabled())
        return;
    try
    {
        if (validate)
            validate();
        if (operation->currentIndex() == 1 && fileHash(replacementPath->text()) != replacementHash)
            fail("差替え画像が更新されました。読み込み直してください。");
        QDialog::accept();
    }
    catch (const std::exception& error)
    {
        validPreview = false;
        updateApply();
        message->setText(QString::fromUtf8(error.what()));
    }
}
void ExistingImageDialog::reject()
{
    closing = true;
    stopped->store(true);
    timer.stop();
    updateApply();
    if (job)
    {
        message->setText("プレビューの処理を中止しています…");
        cancel->setEnabled(false);
    }
    else
        QDialog::reject();
}
} // namespace tatsu
