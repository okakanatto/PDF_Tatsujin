#include "pdf_optimization_dialog.h"

namespace tatsu
{
namespace
{
QString bytes(qint64 value)
{
    return QLocale().toString(value) + " bytes（" + QString::number(value / 1048576.0, 'f', 2) +
           " MiB）";
}
} // namespace
PdfOptimizationDialog::PdfOptimizationDialog(PDFDocument document, QWidget* parent)
    : QDialog(parent), snapshot(std::move(document))
{
    setObjectName("pdfOptimizationDialog");
    setWindowTitle("PDFの容量を最適化");
    resize(640, 350);
    setMinimumSize(520, 300);
    auto layout = new QVBoxLayout(this);
    auto note = new QLabel("文字と画像の画質、フォーム・注釈の再編集を保ったまま、重複・未使用デー"
                           "タを整理します。ファイルはまだ保存しません。");
    note->setWordWrap(true);
    layout->addWidget(note);
    auto form = new QFormLayout;
    before = new QLabel("確認中");
    before->setObjectName("optimizationBefore");
    after = new QLabel("確認中");
    after->setObjectName("optimizationAfter");
    form->addRow("通常保存した場合", before);
    form->addRow("最適化した場合", after);
    layout->addLayout(form);
    progress = new QProgressBar;
    progress->setRange(0, 0);
    progress->setAccessibleName("容量最適化の処理中");
    layout->addWidget(progress);
    message = new QLabel;
    message->setObjectName("optimizationMessage");
    message->setTextFormat(Qt::PlainText);
    message->setWordWrap(true);
    layout->addWidget(message);
    layout->addStretch();
    auto actions = new QDialogButtonBox;
    retry = actions->addButton("再試行", QDialogButtonBox::ActionRole);
    retry->setObjectName("optimizationRetry");
    retry->hide();
    apply = actions->addButton("文書へ適用", QDialogButtonBox::AcceptRole);
    apply->setObjectName("optimizationApply");
    apply->setProperty("primary", true);
    apply->setEnabled(false);
    cancel = actions->addButton("キャンセル", QDialogButtonBox::RejectRole);
    cancel->setObjectName("optimizationCancel");
    layout->addWidget(actions);
    connect(apply, &QPushButton::clicked, this, &PdfOptimizationDialog::accept);
    connect(cancel, &QPushButton::clicked, this, &PdfOptimizationDialog::reject);
    connect(retry, &QPushButton::clicked, this, [this] { start(); });
    QTimer::singleShot(0, this, [this] { start(); });
}
PdfOptimizationDialog::~PdfOptimizationDialog()
{
    cancelled = true;
    if (job)
    {
        disconnect(job, nullptr, this, nullptr);
        job->wait();
    }
}
void PdfOptimizationDialog::start()
{
    if (job)
        return;
    cancelled = false;
    candidate.reset();
    apply->setEnabled(false);
    retry->hide();
    cancel->setEnabled(true);
    cancel->setText("キャンセル");
    progress->show();
    message->setText("最適化の候補を準備しています…");
    struct Result
    {
        std::optional<OptimizationResult> document;
        QString error;
    };
    auto result = std::make_shared<Result>();
    job = QThread::create(
        [this, document = snapshot, result]
        {
            try
            {
                result->document = optimizePdf(
                    document, [this] { return cancelled.load(); },
                    [this](QString text)
                    {
                        QMetaObject::invokeMethod(
                            this,
                            [this, text]
                            {
                                if (job && !cancelled)
                                    message->setText(text);
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
                result->error = "容量を最適化できません。文書は変更していません。";
            }
        });
    auto launched = job;
    launched->setParent(this);
    connect(launched, &QThread::finished, this,
            [this, launched, result]
            {
                job = nullptr;
                launched->deleteLater();
                progress->hide();
                if (cancelled)
                {
                    QDialog::reject();
                    return;
                }
                if (result->document)
                {
                    candidate = std::move(result->document);
                    before->setText(bytes(candidate->beforeBytes));
                    after->setText(bytes(candidate->afterBytes));
                    setProperty("optimizationSmaller", candidate->smaller());
                    if (candidate->smaller())
                    {
                        message->setText(
                            QString("%1 "
                                    "削減できます（%2%"
                                    "）。文書へ適用してから、通常の保存でPDFを保存してください。")
                                .arg(bytes(candidate->beforeBytes - candidate->afterBytes))
                                .arg(100.0 * (candidate->beforeBytes - candidate->afterBytes) /
                                         candidate->beforeBytes,
                                     0, 'f', 1));
                        apply->setEnabled(true);
                    }
                    else
                        message->setText(
                            "今回の方法ではこれ以上小さくなりません。文書は変更していません。");
                }
                else
                {
                    message->setText(result->error);
                    retry->show();
                }
                cancel->setText("閉じる");
            });
    launched->start();
}
void PdfOptimizationDialog::accept()
{
    if (!job && candidate && candidate->smaller() && apply->isEnabled())
        QDialog::accept();
}
void PdfOptimizationDialog::reject()
{
    if (job)
    {
        cancelled = true;
        cancel->setEnabled(false);
        message->setText("中止しています。元の文書は変更していません。");
    }
    else
        QDialog::reject();
}
void PdfOptimizationDialog::closeEvent(QCloseEvent* event)
{
    if (job)
    {
        event->ignore();
        reject();
    }
    else
        QDialog::closeEvent(event);
}
PDFDocument PdfOptimizationDialog::takeDocument()
{
    if (job || !candidate || !candidate->smaller() || result() != QDialog::Accepted)
        fail("適用できる最適化候補がありません。");
    auto result = std::move(candidate->document);
    candidate.reset();
    return result;
}
} // namespace tatsu
