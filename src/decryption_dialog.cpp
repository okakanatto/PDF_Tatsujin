#include "decryption_dialog.h"
#include "pdfexception.h"
#include <memory>

namespace tatsu
{
DecryptionDialog::DecryptionDialog(PDFDocument document, QString suggestedPath, QWidget* parent)
    : QDialog(parent), snapshot(std::move(document))
{
    setObjectName("decryptionDialog");
    setWindowTitle("保護を解除した編集用コピー");
    resize(660, 440);
    setMinimumSize(600, 400);
    auto layout = new QVBoxLayout(this);
    auto note = new QLabel("変更権限パスワードを確認し、新しいコピーを保存して別のウィンドウで開き"
                           "ます。元のPDFの保護と読取専用は保持します。");
    note->setWordWrap(true);
    layout->addWidget(note);
    settings = new QWidget;
    auto controls = new QVBoxLayout(settings);
    auto fields = new QFormLayout;
    password = new QLineEdit;
    password->setObjectName("decryptionPassword");
    password->setEchoMode(QLineEdit::Password);
    password->setMaxLength(1024);
    fields->addRow("変更権限パスワード", password);
    controls->addLayout(fields);
    auto reveal = new QCheckBox("パスワードを表示");
    reveal->setObjectName("decryptionShowPassword");
    connect(reveal, &QCheckBox::toggled, this, [this](bool visible)
            { password->setEchoMode(visible ? QLineEdit::Normal : QLineEdit::Password); });
    controls->addWidget(reveal);
    consent = new QCheckBox("コピーからパスワードと操作制限を取り除く");
    consent->setObjectName("decryptionConsent");
    controls->addWidget(consent);
    auto hint = new QLabel("コピーはパスワードなしで開け、内容のコピー・編集・印刷ができます。証明"
                           "書署名や非対応フォームがある文書は解除できません。");
    hint->setWordWrap(true);
    controls->addWidget(hint);
    controls->addWidget(new QLabel("編集用コピーの新しい保存先"));
    auto destination = new QHBoxLayout;
    path = new QLineEdit(suggestedPath);
    path->setObjectName("decryptionPath");
    path->setAccessibleName("保護を解除したコピーの新しいPDF保存先");
    destination->addWidget(path, 1);
    auto browse = new QPushButton("保存先…");
    browse->setObjectName("decryptionBrowse");
    destination->addWidget(browse);
    connect(browse, &QPushButton::clicked, this,
            [this]
            {
                const auto selected = QFileDialog::getSaveFileName(
                    this, "編集用コピーの新しい保存先", path->text(), "PDF (*.pdf)", nullptr,
                    QFileDialog::DontConfirmOverwrite);
                if (!selected.isEmpty())
                    path->setText(selected);
            });
    controls->addLayout(destination);
    controls->addStretch();
    auto scroll = new QScrollArea;
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setWidgetResizable(true);
    scroll->setWidget(settings);
    layout->addWidget(scroll, 1);
    message = new QLabel(
        "解除するコピーの意味を確認して選択してください。既存のファイルは上書きしません。");
    message->setObjectName("decryptionMessage");
    message->setTextFormat(Qt::PlainText);
    message->setWordWrap(true);
    layout->addWidget(message);
    auto actions = new QDialogButtonBox;
    save = actions->addButton("コピーを保存して開く", QDialogButtonBox::AcceptRole);
    save->setObjectName("decryptionSave");
    save->setProperty("primary", true);
    save->setEnabled(false);
    cancel = actions->addButton("キャンセル", QDialogButtonBox::RejectRole);
    cancel->setObjectName("decryptionCancel");
    layout->addWidget(actions);
    connect(consent, &QCheckBox::toggled, this,
            [this](bool checked) { save->setEnabled(checked && !job); });
    connect(save, &QPushButton::clicked, this, &DecryptionDialog::accept);
    connect(cancel, &QPushButton::clicked, this, &DecryptionDialog::reject);
}
DecryptionDialog::~DecryptionDialog()
{
    cancelled = true;
    if (job)
    {
        disconnect(job, nullptr, this, nullptr);
        job->wait();
    }
    password->clear();
}
void DecryptionDialog::accept()
{
    if (job || !consent->isChecked())
        return;
    const auto selected = password->text();
    const auto destination = QFileInfo(path->text()).absoluteFilePath();
    settings->setEnabled(false);
    save->setEnabled(false);
    cancelled = false;
    struct Result
    {
        QByteArray hash;
        QString error;
    };
    auto result = std::make_shared<Result>();
    job = QThread::create(
        [this, document = snapshot, selected, destination, result]
        {
            try
            {
                result->hash = exportUnprotectedPdf(
                    document, selected, destination, [this] { return cancelled.load(); },
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
            catch (const pdf::PDFException&)
            {
                result->error = "PDFの保護解除を確認できません。元の文書は変更していません。";
            }
            catch (const std::exception& error)
            {
                result->error = QString::fromUtf8(error.what());
            }
            catch (...)
            {
                result->error = "編集用コピーを作成できません。元の文書は変更していません。";
            }
        });
    auto launched = job;
    launched->setParent(this);
    connect(launched, &QThread::finished, this,
            [this, launched, result, destination]
            {
                job = nullptr;
                launched->deleteLater();
                if (!result->hash.isEmpty())
                {
                    completedPath = destination;
                    completedHash = result->hash;
                    QDialog::accept();
                }
                else if (cancelled)
                    QDialog::reject();
                else
                {
                    settings->setEnabled(true);
                    save->setEnabled(consent->isChecked());
                    message->setText(result->error);
                }
            });
    launched->start();
}
void DecryptionDialog::reject()
{
    if (job)
    {
        cancelled = true;
        cancel->setEnabled(false);
        message->setText("中止しています…");
    }
    else
        QDialog::reject();
}
void DecryptionDialog::closeEvent(QCloseEvent* event)
{
    if (job)
    {
        event->ignore();
        reject();
    }
    else
        QDialog::closeEvent(event);
}
QString DecryptionDialog::savedPath() const
{
    return completedPath;
}
QByteArray DecryptionDialog::savedHash() const
{
    return completedHash;
}
} // namespace tatsu
