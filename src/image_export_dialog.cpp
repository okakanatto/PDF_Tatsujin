#include "image_export_dialog.h"
#include <optional>

namespace tatsu
{
ImageExportDialog::ImageExportDialog(PDFDocument document, int currentPage, const QString& name,
                                     QWidget* parent)
    : QDialog(parent), snapshot(std::move(document)), current(currentPage)
{
    setObjectName("imageExportDialog");
    setWindowTitle("PDFを画像として出力");
    resize(620, 440);
    setMinimumSize(580, 420);
    auto layout = new QVBoxLayout(this);
    auto note = new QLabel("PDFの見た目をPNG／JPEGへ保存します。文字情報・フォーム・証明書は画像へ"
                           "引き継がれません。元のPDFは変更しません。");
    note->setWordWrap(true);
    layout->addWidget(note);
    settings = new QWidget;
    auto form = new QFormLayout(settings);
    form->setContentsMargins(0, 0, 0, 0);
    auto folderRow = new QHBoxLayout;
    directory = new QLineEdit;
    directory->setObjectName("imageExportDirectory");
    auto browse = new QPushButton("フォルダを選ぶ");
    connect(browse, &QPushButton::clicked, this,
            [this]
            {
                auto selected = QFileDialog::getExistingDirectory(this, "画像の出力フォルダ",
                                                                  directory->text());
                if (!selected.isEmpty())
                    directory->setText(selected);
            });
    folderRow->addWidget(directory, 1);
    folderRow->addWidget(browse);
    form->addRow("出力フォルダ", folderRow);
    prefix = new QLineEdit(name.isEmpty() ? "PDF画像" : name);
    prefix->setObjectName("imageExportPrefix");
    prefix->setMaxLength(80);
    form->addRow("画像の名前", prefix);
    auto rangeRow = new QHBoxLayout;
    scope = new QComboBox;
    scope->setObjectName("imageExportScope");
    scope->addItems({"現在ページ", "全ページ", "指定ページ"});
    range = new QLineEdit;
    range->setObjectName("imageExportRange");
    range->setPlaceholderText("例: 1-3, 5");
    range->setEnabled(false);
    connect(scope, &QComboBox::currentIndexChanged, this,
            [this] { range->setEnabled(scope->currentIndex() == 2); });
    rangeRow->addWidget(scope);
    rangeRow->addWidget(range, 1);
    form->addRow("出力するページ", rangeRow);
    auto encoding = new QHBoxLayout;
    format = new QComboBox;
    format->setObjectName("imageExportFormat");
    format->addItems({"PNG（画素を可逆保存）", "JPEG（品質95・白い背景）"});
    dpi = new QSpinBox;
    dpi->setObjectName("imageExportDpi");
    dpi->setRange(75, 600);
    dpi->setValue(150);
    dpi->setSuffix(" dpi");
    encoding->addWidget(format, 1);
    encoding->addWidget(dpi);
    form->addRow("形式・解像度", encoding);
    layout->addWidget(settings);
    message = new QLabel("名前に物理ページ番号を付けます。既存ファイルは上書きしません。");
    message->setObjectName("imageExportProgress");
    message->setTextFormat(Qt::PlainText);
    message->setWordWrap(true);
    layout->addWidget(message);
    results = new QPlainTextEdit;
    results->setObjectName("imageExportResults");
    results->setReadOnly(true);
    results->setMinimumHeight(70);
    results->setPlaceholderText("確定した画像・失敗・未出力の結果を表示します");
    layout->addWidget(results, 1);
    auto buttons = new QDialogButtonBox;
    run = buttons->addButton("この設定で画像出力", QDialogButtonBox::AcceptRole);
    run->setObjectName("imageExportRun");
    run->setProperty("primary", true);
    cancel = buttons->addButton("閉じる", QDialogButtonBox::RejectRole);
    cancel->setObjectName("imageExportCancel");
    layout->addWidget(buttons);
    connect(run, &QPushButton::clicked, this, &ImageExportDialog::start);
    connect(cancel, &QPushButton::clicked, this, &ImageExportDialog::reject);
}
ImageExportDialog::~ImageExportDialog()
{
    cancelled = true;
    if (job)
        job->wait();
}
void ImageExportDialog::setWorking(bool working)
{
    settings->setEnabled(!working);
    run->setEnabled(!working);
    cancel->setEnabled(true);
    cancel->setText(working ? "出力を中止" : "閉じる");
}
void ImageExportDialog::start()
{
    if (job)
        return;
    QVector<int> selected;
    try
    {
        if (scope->currentIndex() == 0)
            selected = {current};
        else if (scope->currentIndex() == 1)
            for (int i = 0; i < int(snapshot.getCatalog()->getPageCount()); ++i)
                selected.append(i);
        else
            selected = parsePages(range->text(), int(snapshot.getCatalog()->getPageCount()));
    }
    catch (const std::exception& error)
    {
        message->setText(QString::fromUtf8(error.what()));
        return;
    }
    const ImageExportOptions options{
        directory->text(), prefix->text(),
        format->currentIndex() == 0 ? QByteArray("png") : QByteArray("jpeg"), dpi->value()};
    cancelled = false;
    setWorking(true);
    results->clear();
    message->setText("出力先とページを確認しています。");
    struct Outcome
    {
        std::optional<ImageExportOutcome> value;
        QString error;
    };
    auto outcome = std::make_shared<Outcome>();
    job = QThread::create(
        [this, selected, options, outcome]
        {
            try
            {
                outcome->value = exportImages(
                    snapshot, selected, options, [this] { return cancelled.load(); },
                    [this](const QString& phase, int page, int total)
                    {
                        QMetaObject::invokeMethod(
                            this,
                            [this, phase, page, total]
                            {
                                if (!cancelled)
                                    message->setText(QString("%1 %2 / %3 ページ")
                                                         .arg(phase)
                                                         .arg(page + 1)
                                                         .arg(total));
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
                outcome->error = "画像出力を完了できませんでした。元の文書は変更していません。";
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
                if (!outcome->value)
                {
                    message->setText(outcome->error);
                    return;
                }
                QStringList lines;
                int completed = 0;
                for (const auto& file : outcome->value->files)
                {
                    if (file.success)
                        ++completed;
                    lines.append(QString("%1：%2%3")
                                     .arg(file.success ? "保存済み" : "未出力",
                                          QFileInfo(file.path).fileName(),
                                          file.error.isEmpty() ? QString() : " — " + file.error));
                }
                results->setPlainText(lines.join('\n'));
                const bool stopped = cancelled || outcome->value->cancelled;
                message->setText(
                    QString("%1%2 / %3 画像を保存しました。確定した画像は出力フォルダに残ります。")
                        .arg(stopped ? "中止しました。" : "")
                        .arg(completed)
                        .arg(outcome->value->files.size()));
            });
    launched->start();
}
void ImageExportDialog::reject()
{
    if (job)
    {
        cancelled = true;
        cancel->setEnabled(false);
        message->setText("中止しています。処理の終了後、確定した画像と未出力の結果を表示します。");
    }
    else
        QDialog::reject();
}
void ImageExportDialog::closeEvent(QCloseEvent* event)
{
    if (job)
    {
        event->ignore();
        reject();
    }
    else
        QDialog::closeEvent(event);
}
} // namespace tatsu
