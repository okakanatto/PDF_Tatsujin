#include "image_pdf_dialog.h"
#include "ui_widgets.h"

namespace tatsu
{
ImagePdfDialog::ImagePdfDialog(const QStringList& paths, QWidget* parent) : QDialog(parent)
{
    setObjectName("imagePdfDialog");
    setWindowTitle("画像からPDFを作成");
    resize(700, 440);
    setMinimumSize(620, 420);
    auto layout = new QVBoxLayout(this);
    auto note =
        new QLabel("一覧の順に、1画像を1ページにします。元の画像と開いているPDFは変更しません。");
    note->setWordWrap(true);
    layout->addWidget(note);
    settings = new QWidget;
    auto content = new QVBoxLayout(settings);
    content->setContentsMargins(0, 0, 0, 0);
    auto columns = new QHBoxLayout;
    files = new QListWidget;
    files->setObjectName("imagePdfFiles");
    files->setAccessibleName(
        "PDFにする画像一覧。上からページ順。ドラッグまたは上下ボタンで並べ替え");
    files->setDragDropMode(QAbstractItemView::InternalMove);
    files->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    files->setMinimumWidth(200);
    columns->addWidget(files, 1);
    auto detail = new QVBoxLayout;
    preview = new QLabel;
    preview->setObjectName("imagePdfPreview");
    preview->setAlignment(Qt::AlignCenter);
    preview->setFixedSize(240, 160);
    caption = new ElidedLabel("画像を選ぶとプレビューを表示します");
    caption->setObjectName("imagePdfCaption");
    caption->setFixedWidth(240);
    detail->addWidget(preview);
    detail->addWidget(caption);
    detail->addStretch();
    columns->addLayout(detail);
    content->addLayout(columns, 1);
    auto edits = new QHBoxLayout;
    for (const auto& pair : {qMakePair(QString("上へ"), -1), qMakePair(QString("下へ"), 1)})
    {
        auto button = new QPushButton(pair.first);
        button->setObjectName(pair.second < 0 ? "imagePdfMoveUp" : "imagePdfMoveDown");
        connect(button, &QPushButton::clicked, this,
                [this, offset = pair.second] { moveSelection(offset); });
        edits->addWidget(button);
    }
    auto add = new QPushButton("画像を追加");
    add->setObjectName("imagePdfAdd");
    connect(add, &QPushButton::clicked, this,
            [this]
            {
                appendFiles(QFileDialog::getOpenFileNames(this, "PDFにする画像を追加", {},
                                                          "PNG・JPEG (*.png *.jpg *.jpeg)"));
            });
    auto remove = new QPushButton("一覧から除く");
    remove->setObjectName("imagePdfRemove");
    connect(remove, &QPushButton::clicked, this,
            [this]
            {
                delete files->takeItem(files->currentRow());
                create->setEnabled(files->count() > 0);
            });
    edits->addWidget(add);
    edits->addWidget(remove);
    content->addLayout(edits);
    auto options = new QHBoxLayout;
    auto paperLabel = new QLabel("用紙");
    paper = new QComboBox;
    paper->setObjectName("imagePdfPaper");
    paper->addItems({"A4・自動の向き・余白10mm", "画像サイズに合わせる"});
    paperLabel->setBuddy(paper);
    options->addWidget(paperLabel);
    options->addWidget(paper, 1);
    auto dpiLabel = new QLabel("解像度");
    dpi = new QSpinBox;
    dpi->setObjectName("imagePdfDpi");
    dpi->setRange(75, 600);
    dpi->setValue(150);
    dpi->setSuffix(" dpi");
    dpiLabel->setBuddy(dpi);
    dpi->setEnabled(false);
    options->addWidget(dpiLabel);
    options->addWidget(dpi);
    content->addLayout(options);
    layout->addWidget(settings, 1);
    message =
        new QLabel("PNG・JPEG／128枚まで。1画像64MiB・32メガピクセル、合計128メガピクセルまで。");
    message->setObjectName("imagePdfProgress");
    message->setTextFormat(Qt::PlainText);
    message->setWordWrap(true);
    message->setTextInteractionFlags(Qt::TextSelectableByMouse);
    layout->addWidget(message);
    auto buttons = new QDialogButtonBox;
    create = buttons->addButton("この順でPDFを作成", QDialogButtonBox::AcceptRole);
    create->setObjectName("imagePdfCreate");
    create->setProperty("primary", true);
    cancel = buttons->addButton("キャンセル", QDialogButtonBox::RejectRole);
    cancel->setObjectName("imagePdfCancel");
    layout->addWidget(buttons);
    connect(create, &QPushButton::clicked, this, &ImagePdfDialog::accept);
    connect(cancel, &QPushButton::clicked, this,
            [this]
            {
                if (job)
                    requestCancel(false);
                else
                    QDialog::reject();
            });
    connect(paper, &QComboBox::currentIndexChanged, this,
            [this] { dpi->setEnabled(paper->currentIndex() == 1); });
    connect(files, &QListWidget::currentRowChanged, this, [this] { showPreview(); });
    appendFiles(paths);
}
ImagePdfDialog::~ImagePdfDialog()
{
    cancelled = true;
    if (job)
        job->wait();
}
void ImagePdfDialog::appendFiles(const QStringList& paths)
{
    if (job)
        return;
    for (const auto& path : paths)
    {
        auto item = new QListWidgetItem(QFileInfo(path).fileName(), files);
        item->setData(Qt::UserRole, QFileInfo(path).absoluteFilePath());
        item->setToolTip(Qt::convertFromPlainText(QFileInfo(path).absoluteFilePath()));
    }
    create->setEnabled(files->count() > 0);
    if (files->currentRow() < 0 && files->count())
        files->setCurrentRow(0);
}
void ImagePdfDialog::showPreview()
{
    preview->clear();
    auto item = files->currentItem();
    if (!item)
    {
        caption->setText("画像を追加してください");
        return;
    }
    try
    {
        QByteArray baseline;
        auto image = imagePdfPreview(item->data(Qt::UserRole).toString(), &baseline);
        item->setData(Qt::UserRole + 1, baseline);
        preview->setPixmap(QPixmap::fromImage(image).scaled(preview->size(), Qt::KeepAspectRatio,
                                                            Qt::SmoothTransformation));
        caption->setText(item->text());
    }
    catch (const std::exception& error)
    {
        caption->setText(QString::fromUtf8(error.what()));
    }
}
void ImagePdfDialog::moveSelection(int offset)
{
    const int from = files->currentRow(), to = from + offset;
    if (from < 0 || to < 0 || to >= files->count())
        return;
    auto item = files->takeItem(from);
    files->insertItem(to, item);
    files->setCurrentItem(item);
}
void ImagePdfDialog::setWorking(bool working)
{
    settings->setEnabled(!working);
    create->setEnabled(!working && files->count() > 0);
    cancel->setEnabled(true);
    cancel->setText(working ? "作成を中止" : "キャンセル");
}
void ImagePdfDialog::accept()
{
    if (job || !files->count())
        return;
    QVector<ImagePdfInput> inputs;
    for (int i = 0; i < files->count(); ++i)
        inputs.append({files->item(i)->data(Qt::UserRole).toString(),
                       files->item(i)->data(Qt::UserRole + 1).toByteArray()});
    const ImagePdfOptions options{paper->currentIndex() == 0 ? ImagePdfOptions::PageMode::AutoA4
                                                             : ImagePdfOptions::PageMode::ImageSize,
                                  double(dpi->value())};
    cancelled = false;
    closeAfterCancel = false;
    createdDocument.reset();
    setWorking(true);
    message->setText("画像を確認しています。完成するまで文書は追加しません。");
    const auto id = ++generation;
    struct Outcome
    {
        std::optional<PDFDocument> document;
        QString error;
    };
    auto outcome = std::make_shared<Outcome>();
    job = QThread::create(
        [this, inputs, options, outcome, id]
        {
            try
            {
                outcome->document = createImagePdf(
                    inputs, options, [this] { return cancelled.load(); },
                    [this, id](int completed, int total, const QString& name)
                    {
                        QMetaObject::invokeMethod(
                            this,
                            [this, id, completed, total, name]
                            {
                                if (id == generation && !cancelled)
                                    message->setText(QString("作成 %1 / %2 ページ — %3")
                                                         .arg(completed)
                                                         .arg(total)
                                                         .arg(name));
                            },
                            Qt::QueuedConnection);
                    });
            }
            catch (const std::exception& error)
            {
                outcome->error = QString::fromUtf8(error.what());
            }
            catch (...)
            {
                outcome->error = "画像からPDFを作成できませんでした。文書は追加していません。";
            }
        });
    auto launched = job;
    launched->setParent(this);
    connect(launched, &QThread::finished, this,
            [this, launched, outcome]
            {
                job = nullptr;
                launched->deleteLater();
                setWorking(false);
                if (cancelled)
                {
                    message->setText(
                        "作成を中止しました。元の画像と文書は変更していません。再試行できます。");
                    if (closeAfterCancel)
                        QDialog::reject();
                }
                else if (!outcome->document)
                    message->setText(outcome->error);
                else
                {
                    createdDocument = std::move(outcome->document);
                    QDialog::accept();
                }
            });
    launched->start();
}
void ImagePdfDialog::requestCancel(bool close)
{
    cancelled = true;
    closeAfterCancel = closeAfterCancel || close;
    cancel->setEnabled(false);
    message->setText("中止しています。現在の画像の処理が終わるまでお待ちください。");
}
void ImagePdfDialog::reject()
{
    if (job)
        requestCancel(true);
    else
        QDialog::reject();
}
void ImagePdfDialog::closeEvent(QCloseEvent* event)
{
    if (job)
    {
        event->ignore();
        requestCancel(true);
    }
    else
        QDialog::closeEvent(event);
}
PDFDocument ImagePdfDialog::takeDocument()
{
    if (!createdDocument || job || QDialog::result() != QDialog::Accepted)
        fail("完成したPDFがありません。");
    auto document = std::move(*createdDocument);
    createdDocument.reset();
    return document;
}
} // namespace tatsu
