#include "certificate_dialog.h"
#include "document.h"
#include "pdfexception.h"

namespace tatsu
{
CertificateDialog::CertificateDialog(QString source, QByteArray expected, QWidget* parent)
    : QDialog(parent), path(std::move(source)), hash(std::move(expected))
{
    setObjectName("certificateVerificationDialog");
    setWindowTitle("証明書署名を確認");
    resize(980, 650);
    setMinimumSize(640, 440);
    auto layout = new QVBoxLayout(this);
    auto heading = new QLabel("証明書署名のローカル検証");
    auto font = heading->font();
    font.setPointSizeF(font.pointSizeF() + 3);
    font.setBold(true);
    heading->setFont(font);
    layout->addWidget(heading);
    auto scope = new QLabel(
        "署名対象の改変・署名対象範囲・証明書チェーンを別々に確認します。通信は行いません。"
        "失効状態、署名時点の有効性、タイムスタンプ、文書署名用途と許容変更は未評価です。"
        "チェーンの確認だけで署名全体の有効性や本人性を保証するものではありません。");
    scope->setObjectName("certificateVerificationScope");
    scope->setTextFormat(Qt::PlainText);
    scope->setWordWrap(true);
    layout->addWidget(scope);
    auto sourceLabel = new QLabel(QFileInfo(path).fileName());
    sourceLabel->setTextFormat(Qt::PlainText);
    sourceLabel->setTextInteractionFlags(Qt::TextSelectableByMouse | Qt::TextSelectableByKeyboard);
    sourceLabel->setToolTip(path);
    layout->addWidget(sourceLabel);
    table = new QTableWidget(0, 4);
    table->setObjectName("certificateSignatures");
    table->setHorizontalHeaderLabels(
        {"署名欄", "署名対象の改変", "署名対象範囲", "現在のチェーン"});
    table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table->setSelectionBehavior(QAbstractItemView::SelectRows);
    table->setSelectionMode(QAbstractItemView::SingleSelection);
    table->verticalHeader()->hide();
    table->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    table->horizontalHeader()->setMinimumSectionSize(140);
    layout->addWidget(table, 1);
    details = new QPlainTextEdit;
    details->setObjectName("certificateDetails");
    details->setReadOnly(true);
    details->setAccessibleName("選択した証明書署名の詳細。選択してコピーできます。");
    layout->addWidget(details, 2);
    progress = new QProgressBar;
    progress->setObjectName("certificateVerificationProgress");
    progress->setRange(0, 0);
    progress->hide();
    layout->addWidget(progress);
    message = new QLabel("保存済みPDFを検証します。文書は変更しません。");
    message->setObjectName("certificateVerificationMessage");
    message->setTextFormat(Qt::PlainText);
    message->setWordWrap(true);
    layout->addWidget(message);
    auto buttons = new QHBoxLayout;
    start = new QPushButton("検証する");
    start->setObjectName("verifyCertificateSignatures");
    start->setProperty("primary", true);
    buttons->addWidget(start);
    buttons->addStretch();
    close = new QPushButton("閉じる");
    close->setObjectName("closeCertificateVerification");
    buttons->addWidget(close);
    layout->addLayout(buttons);
    connect(start, &QPushButton::clicked, this, &CertificateDialog::verify);
    connect(close, &QPushButton::clicked, this,
            [this]
            {
                if (!job)
                    reject();
                else
                {
                    cancelled = true;
                    close->setEnabled(false);
                    message->setText("検証を中止しています…");
                }
            });
    connect(table, &QTableWidget::itemSelectionChanged, this, &CertificateDialog::showSelected);
}
CertificateDialog::~CertificateDialog()
{
    cancelled = true;
    if (job)
    {
        disconnect(job, nullptr, this, nullptr);
        job->wait();
    }
}
const CertificateVerification* CertificateDialog::verification() const
{
    return result ? &*result : nullptr;
}
void CertificateDialog::verify()
{
    if (job)
        return;
    result.reset();
    table->setRowCount(0);
    details->clear();
    cancelled = false;
    closeRequested = false;
    start->setEnabled(false);
    close->setText("検証を中止");
    progress->show();
    message->setText("元のバイト列とローカル証明書を検証しています…");
    struct Work
    {
        std::optional<CertificateVerification> result;
        QString error;
    };
    auto work = std::make_shared<Work>();
    job = QThread::create(
        [this, work]
        {
            try
            {
                work->result = verifyCertificateSignatures(path, hash, {},
                                                           [this] { return cancelled.load(); });
            }
            catch (const pdf::PDFException&)
            {
                work->error = "PDFの署名情報を読み取れません。";
            }
            catch (const std::exception& error)
            {
                work->error = QString::fromUtf8(error.what());
            }
            catch (...)
            {
                work->error = "証明書署名の検証に失敗しました。";
            }
        });
    auto launched = job;
    launched->setParent(this);
    connect(launched, &QThread::finished, this,
            [this, launched, work]
            {
                job = nullptr;
                launched->deleteLater();
                progress->hide();
                start->setEnabled(true);
                start->setText("再検証する");
                close->setEnabled(true);
                close->setText("閉じる");
                if (!cancelled && work->result)
                {
                    result = std::move(work->result);
                    table->setRowCount(result->signatures.size());
                    for (int i = 0; i < result->signatures.size(); ++i)
                    {
                        const auto& row = result->signatures[i];
                        const auto coverage = row.signedBytes == 0 ? QString("未評価")
                                              : row.entireFile ? QString("現在のファイル全体")
                                                               : QString("署名後の追記あり");
                        const QStringList cells{row.field, signatureIntegrityText(row.integrity),
                                                coverage, certificateChainText(row.chain)};
                        for (int column = 0; column < cells.size(); ++column)
                        {
                            auto cell = new QTableWidgetItem(cells[column]);
                            cell->setToolTip(cells[column]);
                            table->setItem(i, column, cell);
                        }
                    }
                    if (!result->signatures.isEmpty())
                        table->selectRow(0);
                    message->setText(
                        result->signatures.isEmpty()
                            ? "証明書署名を検出しませんでした。見た目の名前・画像による署名とは別の"
                              "検査です。"
                            : "検証結果を表示しました。各列と詳細の評価範囲を確認してください。");
                }
                else
                    message->setText(cancelled ? "検証を中止しました。結果は反映していません。"
                                               : work->error);
                if (closeRequested)
                    QDialog::reject();
            });
    launched->start();
}
void CertificateDialog::showSelected()
{
    const auto index = table->currentRow();
    if (!result || index < 0 || index >= result->signatures.size())
        return;
    const auto& row = result->signatures[index];
    const auto& cert = row.certificate;
    QString text = QString("%1\n\n署名欄: %2\n形式: %3\nハッシュ: %4\n署名対象: %5 "
                           "bytes\n署名後の対象外追記: %6 bytes\n現在のチェーン: %7\n\n")
                       .arg(row.detail, row.field, row.format, row.digest)
                       .arg(row.signedBytes)
                       .arg(row.unsignedTail)
                       .arg(certificateChainText(row.chain));
    if (!cert.fingerprint.isEmpty())
        text +=
            QString("証明書Subject: %1\n証明書Issuer: %2\nSerial: %3\n有効期間（UTC）: %4 ～ "
                    "%5\n公開鍵: %6\nSHA-256指紋: %7\n\n")
                .arg(cert.subject, cert.issuer, cert.serial, cert.notBefore.toString(Qt::ISODate),
                     cert.notAfter.toString(Qt::ISODate), cert.key, cert.fingerprint);
    text +=
        QString("検証時刻（UTC）: %1\nファイルSHA-256: "
                "%2\n失効状態・署名時点の有効性・タイムスタンプ・文書署名用途・許容変更: 未評価")
            .arg(result->checkedAt.toString(Qt::ISODate),
                 QString::fromLatin1(result->fileHash.toHex()));
    details->setPlainText(text);
}
void CertificateDialog::reject()
{
    if (job)
    {
        closeRequested = true;
        cancelled = true;
        close->setEnabled(false);
        message->setText("検証を中止して閉じています…");
        return;
    }
    QDialog::reject();
}
void CertificateDialog::closeEvent(QCloseEvent* event)
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
