#include "certificate_signing_dialog.h"
#include "pdfexception.h"
#include "ui_widgets.h"

namespace tatsu
{
CertificateSigningDialog::CertificateSigningDialog(PDFDocument document,
                                                   QVector<QPair<QString, QByteArray>> inputs,
                                                   QString suggestedPath, QWidget* parent)
    : QDialog(parent), snapshot(std::move(document)), originals(std::move(inputs))
{
    setObjectName("certificateSigningDialog");
    setWindowTitle("証明書で署名したコピー");
    resize(760, 700);
    setMinimumSize(620, 440);
    auto layout = new QVBoxLayout(this);
    auto note = new QLabel(
        "現在の変更を含むコピーに証明書で署名し、別ウィンドウで開きます。元のPDF・"
        "未保存状態・Undoは保持します。証明書署名はページに表示されません。名前や画像の署名"
        "は、元文書へ既存の「署名」ツールで配置できます。");
    note->setWordWrap(true);
    note->setTextFormat(Qt::PlainText);
    layout->addWidget(note);
    settings = new QWidget;
    auto controls = new QVBoxLayout(settings);
    auto keyRow = new QHBoxLayout;
    keyPath = new QLineEdit;
    keyPath->setObjectName("signingKeyPath");
    keyPath->setAccessibleName("ローカルのPFXまたはP12ファイル");
    keyRow->addWidget(keyPath, 1);
    auto keyBrowse = new QPushButton("証明書ファイル…");
    keyBrowse->setObjectName("signingKeyBrowse");
    keyRow->addWidget(keyBrowse);
    controls->addWidget(new QLabel("秘密鍵を含むPFX／P12ファイル"));
    controls->addLayout(keyRow);
    connect(keyBrowse, &QPushButton::clicked, this,
            [this]
            {
                const auto path = QFileDialog::getOpenFileName(
                    this, "署名に使うPFX／P12", keyPath->text(), "PKCS#12 (*.pfx *.p12)");
                if (!path.isEmpty())
                    keyPath->setText(path);
            });
    auto credentials = new QHBoxLayout;
    credentials->addWidget(new QLabel("P12のパスワード"));
    password = new QLineEdit;
    password->setObjectName("signingPassword");
    password->setEchoMode(QLineEdit::Password);
    password->setMaxLength(1024);
    credentials->addWidget(password, 1);
    inspect = new QPushButton("証明書を確認");
    inspect->setObjectName("inspectSigningCertificate");
    credentials->addWidget(inspect);
    controls->addLayout(credentials);
    auto reveal = new QCheckBox("パスワードを表示");
    reveal->setObjectName("signingShowPassword");
    connect(reveal, &QCheckBox::toggled, this, [this](bool checked)
            { password->setEchoMode(checked ? QLineEdit::Normal : QLineEdit::Password); });
    controls->addWidget(reveal);
    identity = new QPlainTextEdit;
    identity->setObjectName("signingCertificateIdentity");
    identity->setReadOnly(true);
    identity->setAccessibleName("署名に使う証明書の名前・期間・SHA-256指紋");
    identity->setMinimumHeight(120);
    controls->addWidget(identity, 1);
    auto scope = new QLabel(
        "証明書・パスワードはこの操作内だけで使います。アプリ設定やOSストアへ保存"
        "せず、通信もしません。署名作成はRSA／ECとSHA-256、AES保護の対応P12を扱います。"
        "信頼・失効・署名時点の有効性・タイムスタンプ・認証署名・LTVの総合評価は未対応です。");
    scope->setWordWrap(true);
    scope->setTextFormat(Qt::PlainText);
    controls->addWidget(scope);
    controls->addWidget(new QLabel("署名理由（任意・1000文字まで）"));
    reason = new QPlainTextEdit;
    reason->setObjectName("signingReason");
    reason->setMaximumHeight(90);
    controls->addWidget(reason);
    controls->addWidget(new QLabel("署名したコピーの新しいPDF保存先"));
    auto output = new QHBoxLayout;
    destination = new QLineEdit(suggestedPath);
    destination->setObjectName("signingDestination");
    destination->setAccessibleName("署名したコピーの新しいPDF保存先");
    output->addWidget(destination, 1);
    auto browse = new QPushButton("保存先…");
    browse->setObjectName("signingDestinationBrowse");
    output->addWidget(browse);
    controls->addLayout(output);
    connect(browse, &QPushButton::clicked, this,
            [this]
            {
                const auto path = QFileDialog::getSaveFileName(
                    this, "署名したコピーの新しい保存先", destination->text(), "PDF (*.pdf)",
                    nullptr, QFileDialog::DontConfirmOverwrite);
                if (!path.isEmpty())
                    destination->setText(path);
            });
    consent = new QCheckBox("表示した証明書で、このコピーに署名する");
    consent->setObjectName("signingConsent");
    consent->setEnabled(false);
    controls->addWidget(consent);
    layout->addWidget(scrollableSettings(settings), 1);
    progress = new QProgressBar;
    progress->setObjectName("signingProgress");
    progress->setRange(0, 0);
    progress->hide();
    layout->addWidget(progress);
    message = new QLabel("証明書を確認してから署名してください。既存ファイルは上書きしません。");
    message->setObjectName("signingMessage");
    message->setTextFormat(Qt::PlainText);
    message->setWordWrap(true);
    layout->addWidget(message);
    auto actions = new QDialogButtonBox;
    save = actions->addButton("署名したコピーを保存して開く", QDialogButtonBox::AcceptRole);
    save->setObjectName("saveSignedCertificateCopy");
    save->setProperty("primary", true);
    save->setAutoDefault(false);
    save->setEnabled(false);
    cancel = actions->addButton("キャンセル", QDialogButtonBox::RejectRole);
    cancel->setObjectName("cancelCertificateSigning");
    layout->addWidget(actions);
    connect(keyPath, &QLineEdit::textChanged, this, &CertificateSigningDialog::invalidate);
    connect(password, &QLineEdit::textChanged, this, &CertificateSigningDialog::invalidate);
    connect(reason, &QPlainTextEdit::textChanged, this, [this] { consent->setChecked(false); });
    connect(destination, &QLineEdit::textChanged, this, [this] { consent->setChecked(false); });
    connect(consent, &QCheckBox::toggled, this,
            [this](bool checked) { save->setEnabled(checked && !approvedP12.isEmpty() && !job); });
    connect(inspect, &QPushButton::clicked, this, &CertificateSigningDialog::inspectKey);
    connect(save, &QPushButton::clicked, this, &CertificateSigningDialog::accept);
    connect(cancel, &QPushButton::clicked, this, &CertificateSigningDialog::reject);
}
CertificateSigningDialog::~CertificateSigningDialog()
{
    cancelled = true;
    if (job)
    {
        disconnect(job, nullptr, this, nullptr);
        job->wait();
    }
    password->clear();
}
QString CertificateSigningDialog::savedPath() const
{
    return completedPath;
}
QByteArray CertificateSigningDialog::savedHash() const
{
    return completedHash;
}
void CertificateSigningDialog::invalidate()
{
    approvedP12.clear();
    approvedHash.clear();
    approvedPath.clear();
    identity->clear();
    consent->setChecked(false);
    consent->setEnabled(false);
    save->setEnabled(false);
    message->setText("証明書ファイルとパスワードを確認してください。");
}
void CertificateSigningDialog::setBusy(bool enabled)
{
    settings->setEnabled(!enabled);
    inspect->setEnabled(!enabled);
    save->setEnabled(!enabled && consent->isChecked() && !approvedP12.isEmpty());
    progress->setVisible(enabled);
    cancel->setEnabled(true);
    cancel->setText(enabled ? "処理を中止" : "キャンセル");
}
void CertificateSigningDialog::inspectKey()
{
    if (job)
        return;
    invalidate();
    setBusy(true);
    cancelled = false;
    closeRequested = false;
    const auto path = QFileInfo(keyPath->text()).absoluteFilePath();
    const auto pass = password->text();
    struct Work
    {
        QByteArray bytes, hash;
        CertificateIdentity identity;
        QString error;
    };
    auto work = std::make_shared<Work>();
    message->setText("証明書と秘密鍵・有効期間・用途を確認しています…");
    job = QThread::create(
        [this, path, pass, work]
        {
            try
            {
                QFile file(path);
                if (!file.open(QIODevice::ReadOnly) || file.size() <= 0 ||
                    file.size() > 4 * 1024 * 1024)
                    fail("4MiBまでの対応PFX／P12ファイルを選んでください。");
                work->bytes = file.read(4 * 1024 * 1024 + 1);
                if (file.error() != QFile::NoError || work->bytes.size() != file.size())
                    fail("P12を読み取れません。");
                work->hash = QCryptographicHash::hash(work->bytes, QCryptographicHash::Sha256);
                if (cancelled)
                    return;
                work->identity = inspectSigningCertificate(work->bytes, pass);
                if (fileHash(path) != work->hash)
                    fail("確認中にP12が更新されました。再確認してください。");
            }
            catch (const std::exception& error)
            {
                work->error = QString::fromUtf8(error.what());
            }
            catch (...)
            {
                work->error = "証明書を確認できません。";
            }
        });
    auto launched = job;
    launched->setParent(this);
    connect(launched, &QThread::finished, this,
            [this, launched, path, work]
            {
                job = nullptr;
                launched->deleteLater();
                setBusy(false);
                if (!cancelled && work->error.isEmpty() && !work->identity.fingerprint.isEmpty())
                {
                    approvedP12 = work->bytes;
                    approvedHash = work->hash;
                    approvedPath = path;
                    const auto& cert = work->identity;
                    identity->setPlainText(
                        QString("Subject: %1\nIssuer: %2\n期間（UTC）: %3 ～ %4\n公開鍵: "
                                "%5\nSHA-256指紋: %6\n信頼・失効・文書署名用途: 未評価")
                            .arg(cert.subject, cert.issuer, cert.notBefore.toString(Qt::ISODate),
                                 cert.notAfter.toString(Qt::ISODate), cert.key, cert.fingerprint));
                    consent->setEnabled(true);
                    message->setText(
                        "表示した証明書の名前・指紋と、理由・保存先を確認してください。");
                }
                else
                    message->setText(cancelled ? "証明書の確認を中止しました。" : work->error);
                if (closeRequested)
                    QDialog::reject();
            });
    launched->start();
}
void CertificateSigningDialog::accept()
{
    if (job || approvedP12.isEmpty() || !consent->isChecked())
        return;
    setBusy(true);
    cancelled = false;
    closeRequested = false;
    const auto p12 = approvedP12, p12Hash = approvedHash;
    const auto p12Path = approvedPath, pass = password->text(), text = reason->toPlainText();
    const auto output = QFileInfo(destination->text()).absoluteFilePath();
    struct Work
    {
        QByteArray hash;
        QString error;
    };
    auto work = std::make_shared<Work>();
    job = QThread::create(
        [this, p12, p12Hash, p12Path, pass, text, output, work]
        {
            try
            {
                work->hash = exportSignedPdf(
                    snapshot, p12, pass, text, output, [this] { return cancelled.load(); },
                    [this](QString status)
                    {
                        QMetaObject::invokeMethod(
                            this,
                            [this, status]
                            {
                                if (job && !cancelled)
                                    message->setText(status);
                            },
                            Qt::QueuedConnection);
                    },
                    [this, p12Path, p12Hash]
                    {
                        if (fileHash(p12Path) != p12Hash)
                            fail("P12が更新されました。証明書を再確認してください。");
                        for (const auto& original : originals)
                            if (!original.first.isEmpty() &&
                                fileHash(original.first) != original.second)
                                fail("元のPDFファイルが更新されました。開き直してから署名してくださ"
                                     "い。");
                    });
            }
            catch (const pdf::PDFException&)
            {
                work->error = "署名付きPDFを検証できません。元の文書は保持しています。";
            }
            catch (const std::exception& error)
            {
                work->error = QString::fromUtf8(error.what());
            }
            catch (...)
            {
                work->error = "署名したコピーを作成できません。";
            }
        });
    auto launched = job;
    launched->setParent(this);
    connect(launched, &QThread::finished, this,
            [this, launched, output, work]
            {
                job = nullptr;
                launched->deleteLater();
                setBusy(false);
                if (!work->hash.isEmpty())
                {
                    completedPath = output;
                    completedHash = work->hash;
                    password->clear();
                    QDialog::accept();
                }
                else
                {
                    invalidate();
                    message->setText(cancelled
                                         ? "署名の作成を中止しました。元の文書は保持しています。"
                                         : work->error);
                    if (closeRequested)
                        QDialog::reject();
                }
            });
    launched->start();
}
void CertificateSigningDialog::reject()
{
    if (job)
    {
        cancelled = true;
        closeRequested = true;
        cancel->setEnabled(false);
        message->setText("処理を中止して閉じています…");
    }
    else
        QDialog::reject();
}
void CertificateSigningDialog::closeEvent(QCloseEvent* event)
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
