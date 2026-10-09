#include "page_crop_dialog.h"
#include <memory>

namespace tatsu
{
class CropPreview : public QWidget
{
public:
    QImage image;
    QSizeF physical;
    QMarginsF margins;
    explicit CropPreview(QWidget* parent = nullptr) : QWidget(parent)
    {
        setObjectName("cropPreview");
        setMinimumSize(280, 220);
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
        setAccessibleName("残す用紙範囲のプレビュー。青枠内を残し、灰色の範囲を表示から除きます");
    }
    void paintEvent(QPaintEvent*) override
    {
        QPainter painter(this);
        painter.fillRect(rect(), palette().alternateBase());
        if (image.isNull())
        {
            painter.setPen(palette().text().color());
            painter.drawText(rect(), Qt::AlignCenter, "プレビューを準備しています…");
            return;
        }
        const auto size = image.size().scaled(width() - 24, height() - 24, Qt::KeepAspectRatio);
        const QRectF paper((width() - size.width()) / 2.0, (height() - size.height()) / 2.0,
                           size.width(), size.height());
        painter.drawImage(paper, image);
        const double x = paper.width() / physical.width(), y = paper.height() / physical.height();
        const double mm = 72.0 / 25.4;
        const auto keep =
            paper.marginsRemoved({margins.left() * mm * x, margins.top() * mm * y,
                                  margins.right() * mm * x, margins.bottom() * mm * y});
        QPainterPath removed;
        removed.setFillRule(Qt::OddEvenFill);
        removed.addRect(paper);
        if (keep.width() > 0 && keep.height() > 0)
            removed.addRect(keep);
        painter.fillPath(removed, QColor(32, 45, 65, 135));
        painter.setPen(QPen(QColor("#2563eb"), 2));
        if (keep.width() > 0 && keep.height() > 0)
            painter.drawRect(keep);
    }
};
PageCropDialog::PageCropDialog(PDFDocument document, QVector<int> selected, QWidget* parent)
    : QDialog(parent), snapshot(std::move(document)), pages(std::move(selected))
{
    cropPages(snapshot, pages, {});
    restriction = editingRestriction(snapshot);
    setObjectName("pageCropDialog");
    setWindowTitle("余白・表示範囲を調整");
    resize(680, 490);
    setMinimumSize(600, 450);
    auto layout = new QVBoxLayout(this);
    auto note = new QLabel("見える範囲だけを狭めます。範囲外の文字・画像もPDF内に残ります。"
                           "墨消しではありません。Undoで戻せます。");
    note->setWordWrap(true);
    layout->addWidget(note);
    auto body = new QHBoxLayout;
    preview = new CropPreview;
    body->addWidget(preview, 1);
    auto controls = new QVBoxLayout;
    controls->addWidget(new QLabel(QString("選択した%1ページへ適用").arg(pages.size())));
    page = new QComboBox;
    page->setObjectName("cropPreviewPage");
    page->setAccessibleName("プレビューする物理ページ");
    for (int number : pages)
        page->addItem(QString("プレビュー：%1ページ").arg(number + 1), number);
    controls->addWidget(page);
    auto form = new QFormLayout;
    auto input = [&](const QString& label, const QString& name)
    {
        auto field = new QDoubleSpinBox;
        field->setObjectName(name);
        field->setRange(0, 500);
        field->setDecimals(2);
        field->setSuffix(" mm");
        field->setAccessibleName("見た目の" + label + "から除く長さ");
        form->addRow(label, field);
        connect(field, &QDoubleSpinBox::valueChanged, this, [this] { updateSettings(); });
        return field;
    };
    top = input("上", "cropTop");
    right = input("右", "cropRight");
    bottom = input("下", "cropBottom");
    left = input("左", "cropLeft");
    controls->addLayout(form);
    auto reset = new QPushButton("入力を0に戻す");
    controls->addWidget(reset);
    connect(reset, &QPushButton::clicked, this,
            [this]
            {
                for (auto field : {top, right, bottom, left})
                    field->setValue(0);
            });
    dimensions = new QLabel;
    dimensions->setObjectName("cropDimensions");
    dimensions->setWordWrap(true);
    controls->addWidget(dimensions);
    controls->addStretch();
    body->addLayout(controls);
    layout->addLayout(body, 1);
    message = new QLabel;
    message->setObjectName("cropMessage");
    message->setWordWrap(true);
    layout->addWidget(message);
    auto buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    apply = buttons->button(QDialogButtonBox::Ok);
    apply->setText("選択ページへ適用");
    apply->setObjectName("applyPageCrop");
    buttons->button(QDialogButtonBox::Cancel)->setText("キャンセル");
    connect(buttons, &QDialogButtonBox::accepted, this,
            [this]
            {
                if (apply->isEnabled())
                    accept();
            });
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addWidget(buttons);
    connect(page, &QComboBox::currentIndexChanged, this, [this] { loadPreview(); });
    updateSettings();
    loadPreview();
}
PageCropDialog::~PageCropDialog()
{
    if (job)
    {
        disconnect(job, nullptr, this, nullptr);
        job->wait();
    }
}
QMarginsF PageCropDialog::margins() const
{
    return {left->value(), top->value(), right->value(), bottom->value()};
}
void PageCropDialog::updateSettings()
{
    preview->margins = margins();
    preview->update();
    apply->setEnabled(false);
    try
    {
        if (!restriction.isEmpty())
            fail(restriction);
        for (int number : pages)
            croppedPageBox(snapshot.getCatalog()->getPage(number), margins());
        const auto physical = pageSize(snapshot.getCatalog()->getPage(page->currentData().toInt()));
        const auto remaining = physical / (72.0 / 25.4) - QSizeF(left->value() + right->value(),
                                                                 top->value() + bottom->value());
        const auto current = physical / (72.0 / 25.4);
        dimensions->setText(QString("現在：%1 × %2 mm\n変更後：%3 × %4 mm")
                                .arg(current.width(), 0, 'f', 1)
                                .arg(current.height(), 0, 'f', 1)
                                .arg(remaining.width(), 0, 'f', 1)
                                .arg(remaining.height(), 0, 'f', 1));
        message->setText(margins().isNull()
                             ? "四辺の除く長さを指定してください。"
                             : "青枠内を残します。選択した全ページへ同じmmを適用します。");
        apply->setEnabled(!margins().isNull() && !preview->image.isNull() &&
                          shownPage == page->currentData().toInt());
    }
    catch (const std::exception& error)
    {
        message->setText(QString::fromUtf8(error.what()));
    }
}
void PageCropDialog::loadPreview()
{
    updateSettings();
    if (job)
        return;
    const int number = page->currentData().toInt();
    preview->image = {};
    preview->update();
    struct Result
    {
        QImage image;
        QSizeF physical;
        QString error;
    };
    auto result = std::make_shared<Result>();
    job = QThread::create(
        [document = snapshot, number, result]() mutable
        {
            try
            {
                result->physical = pageSize(document.getCatalog()->getPage(number));
                const double scale =
                    qMin(1.0, 500.0 / qMax(result->physical.width(), result->physical.height()));
                result->image = renderPage(document, number, scale);
            }
            catch (const std::exception& error)
            {
                result->error = QString::fromUtf8(error.what());
            }
            catch (...)
            {
                result->error = "プレビューを描画できません。文書は変更していません。";
            }
        });
    auto launched = job;
    launched->setParent(this);
    connect(launched, &QThread::finished, this,
            [this, launched, number, result]
            {
                job = nullptr;
                launched->deleteLater();
                if (number != page->currentData().toInt())
                {
                    loadPreview();
                    return;
                }
                shownPage = number;
                preview->physical = result->physical;
                preview->image = result->image;
                updateSettings();
                if (!result->error.isEmpty())
                    message->setText(result->error);
            });
    launched->start();
}
} // namespace tatsu
