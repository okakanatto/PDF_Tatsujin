#include "encryption_dialog.h"
#include "pdfexception.h"
#include <memory>

namespace tatsu
{
EncryptionDialog::EncryptionDialog(PDFDocument document, QString suggestedPath, QWidget* parent)
    : QDialog(parent), snapshot(std::move(document))
{
    setObjectName("encryptionDialog");
    setWindowTitle("パスワードで保護したコピー");
    resize(680, 650);
    setMinimumSize(600, 480);
    auto layout = new QVBoxLayout(this);
    auto note = new QLabel("現在の作業内容を、パスワードで開くPDFコピーへ保存します。閲覧用と変更権"
                           "限用の2つを設定します。作業中の文書の保存先は切り替えません。");
    note->setWordWrap(true);
    layout->addWidget(note);
    settings = new QWidget;
    auto controls = new QVBoxLayout(settings);
    auto passwords = new QFormLayout;
    auto field = [&](QString label, QString name)
    {
        auto widget = new QLineEdit;
        widget->setObjectName(name);
        widget->setEchoMode(QLineEdit::Password);
        widget->setMaxLength(1024);
        passwords->addRow(label, widget);
        return widget;
    };
    user = field("閲覧パスワード", "encryptionUser");
    userConfirm = field("閲覧パスワードを再入力", "encryptionUserConfirm");
    owner = field("変更権限パスワード", "encryptionOwner");
    ownerConfirm = field("変更権限パスワードを再入力", "encryptionOwnerConfirm");
    controls->addLayout(passwords);
    auto show = new QCheckBox("パスワードを表示");
    show->setObjectName("encryptionShowPasswords");
    controls->addWidget(show);
    connect(show, &QCheckBox::toggled, this,
            [this](bool visible)
            {
                for (auto widget : {user, userConfirm, owner, ownerConfirm})
                    widget->setEchoMode(visible ? QLineEdit::Normal : QLineEdit::Password);
            });
    auto group = new QGroupBox("閲覧パスワードで開いた人に許可する操作");
    auto permissions = new QGridLayout(group);
    auto permit = [&](QString label, QString name, bool value, int row, int column)
    {
        auto widget = new QCheckBox(label);
        widget->setObjectName(name);
        widget->setChecked(value);
        permissions->addWidget(widget, row, column);
        return widget;
    };
    print = permit("印刷", "encryptionPrint", true, 0, 0);
    copy = permit("文字・画像のコピー", "encryptionCopy", true, 0, 1);
    forms = permit("フォームへの入力", "encryptionForms", true, 1, 0);
    annotations = permit("注釈の変更", "encryptionAnnotations", false, 1, 1);
    assemble = permit("ページの整理", "encryptionAssemble", false, 2, 0);
    modify = permit("その他の文書変更", "encryptionModify", false, 2, 1);
    controls->addWidget(group);
    auto hint = new QLabel("許可の適用は受取人のPDFビューアに依存します。アクセシビリティの利用は許"
                           "可します。本アプリでは、保護したPDFを開き直すと閲覧用になります。標準PD"
                           "Fの規則で全角英数字などのパスワードは正規化されます。");
    hint->setWordWrap(true);
    controls->addWidget(hint);
    auto destination = new QHBoxLayout;
    path = new QLineEdit(suggestedPath);
    path->setObjectName("encryptionPath");
    path->setAccessibleName("保護したコピーの新しいPDF保存先");
    destination->addWidget(path, 1);
    auto browse = new QPushButton("保存先…");
    browse->setObjectName("encryptionBrowse");
    destination->addWidget(browse);
    connect(browse, &QPushButton::clicked, this,
            [this]
            {
                const auto selected = QFileDialog::getSaveFileName(
                    this, "保護したコピーの新しい保存先", path->text(), "PDF (*.pdf)", nullptr,
                    QFileDialog::DontConfirmOverwrite);
                if (!selected.isEmpty())
                    path->setText(selected);
            });
    controls->addWidget(new QLabel("新しいPDFの保存先"));
    controls->addLayout(destination);
    controls->addStretch();
    auto scroll = new QScrollArea;
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setWidget(settings);
    layout->addWidget(scroll, 1);
    message = new QLabel("保護したコピーを保存します。作業中の未保存変更とUndo履歴は保持します。");
    message->setObjectName("encryptionMessage");
    message->setTextFormat(Qt::PlainText);
    message->setWordWrap(true);
    layout->addWidget(message);
    auto actions = new QDialogButtonBox;
    save = actions->addButton("保護したコピーを保存", QDialogButtonBox::AcceptRole);
    save->setObjectName("encryptionSave");
    save->setProperty("primary", true);
    cancel = actions->addButton("キャンセル", QDialogButtonBox::RejectRole);
    cancel->setObjectName("encryptionCancel");
    layout->addWidget(actions);
    connect(save, &QPushButton::clicked, this, &EncryptionDialog::accept);
    connect(cancel, &QPushButton::clicked, this, &EncryptionDialog::reject);
}
EncryptionDialog::~EncryptionDialog()
{
    cancelled = true;
    if (job)
    {
        disconnect(job, nullptr, this, nullptr);
        job->wait();
    }
    for (auto widget : {user, userConfirm, owner, ownerConfirm})
        widget->clear();
}
EncryptionOptions EncryptionDialog::options() const
{
    if (user->text() != userConfirm->text() || owner->text() != ownerConfirm->text())
        fail("再入力したパスワードが一致していません。");
    return {user->text(),          owner->text(),      print->isChecked(),
            copy->isChecked(),     forms->isChecked(), annotations->isChecked(),
            assemble->isChecked(), modify->isChecked()};
}
void EncryptionDialog::accept()
{
    if (job)
        return;
    EncryptionOptions selected;
    try
    {
        selected = options();
        const auto first = preparePdfPassword(selected.userPassword),
                   second = preparePdfPassword(selected.ownerPassword);
        if (first == second)
            fail("閲覧用と変更権限用には異なるパスワードを指定してください。");
    }
    catch (const std::exception& error)
    {
        message->setText(QString::fromUtf8(error.what()));
        return;
    }
    const auto destination = QFileInfo(path->text()).absoluteFilePath();
    settings->setEnabled(false);
    save->setEnabled(false);
    cancelled = false;
    struct Result
    {
        bool success = false;
        QString error;
    };
    auto result = std::make_shared<Result>();
    job = QThread::create(
        [this, document = snapshot, selected, destination, result]
        {
            try
            {
                exportProtectedPdf(
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
                result->success = true;
            }
            catch (const pdf::PDFException&)
            {
                result->error = "PDFの暗号化処理に失敗しました。文書は変更していません。";
            }
            catch (const std::exception& error)
            {
                result->error = QString::fromUtf8(error.what());
            }
            catch (...)
            {
                result->error = "保護したコピーを保存できません。文書は変更していません。";
            }
        });
    auto launched = job;
    launched->setParent(this);
    connect(launched, &QThread::finished, this,
            [this, launched, result, destination]
            {
                job = nullptr;
                launched->deleteLater();
                // Publication wins a cancellation arriving after the atomic move.
                if (result->success)
                {
                    completedPath = destination;
                    QDialog::accept();
                }
                else if (cancelled)
                    QDialog::reject();
                else
                {
                    settings->setEnabled(true);
                    save->setEnabled(true);
                    message->setText(result->error);
                }
            });
    launched->start();
}
void EncryptionDialog::reject()
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
void EncryptionDialog::closeEvent(QCloseEvent* event)
{
    if (job)
    {
        event->ignore();
        reject();
    }
    else
        QDialog::closeEvent(event);
}
QString EncryptionDialog::savedPath() const
{
    return completedPath;
}
} // namespace tatsu
