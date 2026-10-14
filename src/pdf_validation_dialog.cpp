#include "pdf_validation_dialog.h"

namespace tatsu
{
PdfValidationDialog::PdfValidationDialog(PdfValidationInput value, std::function<void()> check,
                                         QWidget* parent)
    : QDialog(parent), input(std::move(value)), validate(std::move(check))
{
    setObjectName("pdfValidationDialog");
    setWindowTitle("PDF規格の検証");
    resize(980, 650);
    setMinimumSize(720, 480);
    auto layout = new QVBoxLayout(this);
    auto note =
        new QLabel("現在のPDFをローカルで検証します。原本・未保存変更・Undoは変更しません。\n"
                   "申告だけで適合とせず、指定規格の検査結果を表示します。変換・修復はしません。");
    note->setWordWrap(true);
    layout->addWidget(note);
    auto form = new QFormLayout;
    const auto configured = [](const QString& environment, const QString& setting)
    {
        const auto value = qEnvironmentVariable(environment.toLatin1().constData());
        return value.isEmpty() ? QSettings().value(setting).toString() : value;
    };
    java = new QLineEdit(configured("TATSU_PDFA_JAVA", "pdfa/java"));
    jar = new QLineEdit(configured("TATSU_PDFA_JAR", "pdfa/jar"));
    java->setObjectName("pdfValidationJava");
    jar->setObjectName("pdfValidationJar");
    java->setPlaceholderText("Javaのjava.exeを指定してください");
    jar->setPlaceholderText("veraPDFのCLI JARを指定してください");
    for (const auto& item :
         {std::pair{java, QString("Javaの実行先")}, std::pair{jar, QString("veraPDF CLI")}})
    {
        auto row = new QHBoxLayout;
        row->addWidget(item.first, 1);
        auto browse = new QPushButton("選択…");
        engineBrowsers.push_back(browse);
        browse->setAutoDefault(false);
        row->addWidget(browse);
        form->addRow(item.second, row);
        connect(browse, &QPushButton::clicked, this,
                [this, field = item.first]
                {
                    const auto path = QFileDialog::getOpenFileName(
                        this, "検証器を指定", field->text(),
                        field == java ? "Java (*.exe)" : "veraPDF CLI (*.jar)");
                    if (!path.isEmpty())
                        field->setText(path);
                });
    }
    profile = new QComboBox;
    profile->setObjectName("pdfValidationProfile");
    for (const auto& format : pdfValidationProfiles())
        profile->addItem(format.title, format.id);
    connect(
        profile, &QComboBox::currentIndexChanged, note,
        [this, note]
        {
            const bool machine =
                pdfValidationProfile(profile->currentData().toString()).machineOnly;
            note->setText(
                machine
                    ? "PDF/UA-1の機械検査を行います。原本・未保存変更・Undoは変更しません。\n"
                      "機械検査に適合しても、全体の適合は未判定です。読み順・代替説明・表や見出し・"
                      "実際の読み上げなど、人による確認が必要です。"
                    : "現在のPDFをローカルで検証します。原本・未保存変更・Undoは変更しません。\n"
                      "申告だけで適合とせず、指定規格の検査結果を表示します。変換・修復はしません"
                      "。");
        });
    form->addRow("検証する形式", profile);
    layout->addLayout(form);
    details = new QPlainTextEdit;
    details->setObjectName("pdfValidationDetails");
    details->setReadOnly(true);
    details->setAccessibleName("PDF規格の検査結果。選択してコピーできます");
    layout->addWidget(details, 1);
    progress = new QProgressBar;
    progress->setRange(0, 0);
    progress->hide();
    layout->addWidget(progress);
    message = new QLabel("形式と検証器を確認して検証してください。結果はまだありません。");
    message->setObjectName("pdfValidationMessage");
    message->setTextFormat(Qt::PlainText);
    message->setWordWrap(true);
    layout->addWidget(message);
    auto buttons = new QHBoxLayout;
    start = new QPushButton("検証する");
    start->setObjectName("pdfValidationStart");
    start->setProperty("primary", true);
    copy = new QPushButton("結果をコピー");
    copy->setObjectName("pdfValidationCopy");
    copy->setEnabled(false);
    output = new QLineEdit;
    output->setObjectName("pdfValidationOutput");
    output->setPlaceholderText("新しい結果.jsonの保存先");
    save = new QPushButton("結果を保存");
    save->setObjectName("pdfValidationSave");
    save->setEnabled(false);
    auto destination = new QHBoxLayout;
    destination->addWidget(output, 1);
    auto browseOutput = new QPushButton("保存先…");
    browseOutput->setAutoDefault(false);
    destination->addWidget(browseOutput);
    destination->addWidget(save);
    layout->addLayout(destination);
    connect(browseOutput, &QPushButton::clicked, this,
            [this]
            {
                const auto path = QFileDialog::getSaveFileName(
                    this, "新しい検査結果へ保存", output->text(), "検査結果 (*.json)", nullptr,
                    QFileDialog::DontConfirmOverwrite);
                if (!path.isEmpty())
                    output->setText(path);
            });
    connect(save, &QPushButton::clicked, this,
            [this]
            {
                try
                {
                    if (validate)
                        validate();
                    if (!result)
                        fail("検査結果がありません。");
                    exportPdfValidationResult(*result, output->text());
                    message->setText("結果を保存しました：" +
                                     QFileInfo(output->text()).absoluteFilePath());
                }
                catch (const std::exception& error)
                {
                    if (validate)
                    {
                        try
                        {
                            validate();
                        }
                        catch (...)
                        {
                            result.reset();
                            copy->setEnabled(false);
                            save->setEnabled(false);
                            details->clear();
                        }
                    }
                    message->setText(QString::fromUtf8(error.what()));
                }
            });
    close = new QPushButton("閉じる");
    close->setObjectName("pdfValidationClose");
    for (auto button : {start, copy, close, save})
        button->setAutoDefault(false);
    buttons->addWidget(start);
    buttons->addWidget(copy);
    buttons->addStretch();
    buttons->addWidget(close);
    layout->addLayout(buttons);
    connect(start, &QPushButton::clicked, this, [this] { verify(); });
    connect(close, &QPushButton::clicked, this, [this] { reject(); });
    connect(copy, &QPushButton::clicked, this,
            [this]
            {
                if (!result)
                    return;
                try
                {
                    if (validate)
                        validate();
                    QApplication::clipboard()->setText(details->toPlainText());
                    message->setText("結果をコピーしました。");
                }
                catch (const std::exception& error)
                {
                    result.reset();
                    copy->setEnabled(false);
                    save->setEnabled(false);
                    details->clear();
                    message->setText(QString::fromUtf8(error.what()));
                }
            });
    const auto invalidated = [this]
    {
        if (!job)
        {
            result.reset();
            copy->setEnabled(false);
            save->setEnabled(false);
            details->clear();
            message->setText("設定を変更しました。再検証してください。");
        }
    };
    connect(java, &QLineEdit::textChanged, this, invalidated);
    connect(jar, &QLineEdit::textChanged, this, invalidated);
    connect(profile, &QComboBox::currentIndexChanged, this, invalidated);
}
PdfValidationDialog::~PdfValidationDialog()
{
    cancelled = true;
    if (job)
    {
        disconnect(job, nullptr, this, nullptr);
        job->wait();
    }
}
void PdfValidationDialog::verify()
{
    if (job)
        return;
    try
    {
        if (validate)
            validate();
    }
    catch (const std::exception& error)
    {
        result.reset();
        copy->setEnabled(false);
        save->setEnabled(false);
        details->clear();
        message->setText(QString::fromUtf8(error.what()));
        return;
    }
    result.reset();
    details->clear();
    copy->setEnabled(false);
    save->setEnabled(false);
    cancelled = false;
    start->setEnabled(false);
    java->setEnabled(false);
    jar->setEnabled(false);
    for (auto browse : engineBrowsers)
        browse->setEnabled(false);
    profile->setEnabled(false);
    close->setText("検証を中止");
    progress->show();
    message->setText("指定形式に対する適合性を検査しています…");
    struct Work
    {
        std::optional<PdfValidationResult> result;
        QString error;
    };
    const auto work = std::make_shared<Work>();
    const auto javaPath = java->text(), jarPath = jar->text(),
               format = profile->currentData().toString();
    job = QThread::create(
        [this, work, javaPath, jarPath, format]
        {
            try
            {
                work->result = validatePdfConformance(input, javaPath, jarPath, format,
                                                      [this] { return cancelled.load(); });
            }
            catch (const std::exception& error)
            {
                work->error = QString::fromUtf8(error.what());
            }
            catch (...)
            {
                work->error = "検査を完了できませんでした。適合性は未判定です。";
            }
        });
    auto launched = job;
    launched->setParent(this);
    connect(
        launched, &QThread::finished, this,
        [this, launched, work, javaPath, jarPath]
        {
            job = nullptr;
            launched->deleteLater();
            start->setEnabled(true);
            java->setEnabled(true);
            jar->setEnabled(true);
            for (auto browse : engineBrowsers)
                browse->setEnabled(true);
            profile->setEnabled(true);
            progress->hide();
            close->setEnabled(true);
            close->setText("閉じる");
            try
            {
                if (validate)
                    validate();
                if (cancelled || !work->result)
                    fail(cancelled ? "検査を中止しました。適合性は未判定です。" : work->error);
                result = std::move(work->result);
                const auto& format = pdfValidationProfile(result->profile);
                const QString judgment =
                    format.machineOnly
                        ? (result->checksPassed ? "機械検査に適合（全体の適合は未判定）"
                                                : "機械検査で不適合")
                        : (result->checksPassed ? "適合" : "不適合");
                QStringList lines{QString("検証形式：%1").arg(format.title),
                                  QString("判定：%1").arg(judgment),
                                  QString("検証器：veraPDF %1").arg(result->engineVersion),
                                  QString("検査対象のSHA-256：%1").arg(result->snapshotHash),
                                  QString("失敗：%1規則・%2検査")
                                      .arg(result->failedRules)
                                      .arg(result->failedChecks)};
                if (format.machineOnly)
                    lines << "全体の適合は未判定です。読み順・代替説明・表や見出し・実際の読み上げ"
                             "など、"
                             "人による確認が必要です。";
                for (const auto& rule : result->rules)
                    lines << QString("\n%1  %2（検査%3）\n%4")
                                 .arg(rule.specification, rule.clause, rule.test, rule.description);
                details->setPlainText(lines.join('\n'));
                message->setText(
                    format.machineOnly
                        ? judgment + "。読み順・代替説明・読み上げなど、人による確認が必要です。"
                    : result->checksPassed
                        ? "指定したPDF/A形式への適合を確認しました。"
                        : QString("指定したPDF/"
                                  "A形式に不適合です。%1規則・%2検査の失敗を確認してください。")
                              .arg(result->failedRules)
                              .arg(result->failedChecks));
                copy->setEnabled(true);
                save->setEnabled(true);
                QSettings().setValue("pdfa/java", javaPath);
                QSettings().setValue("pdfa/jar", jarPath);
            }
            catch (const std::exception& error)
            {
                result.reset();
                copy->setEnabled(false);
                save->setEnabled(false);
                details->clear();
                message->setText(QString::fromUtf8(error.what()));
            }
            if (closing)
                QDialog::reject();
        });
    launched->start();
}
void PdfValidationDialog::reject()
{
    if (!job)
        QDialog::reject();
    else
    {
        closing = true;
        cancelled = true;
        close->setEnabled(false);
        message->setText("検査を中止しています…");
    }
}
void PdfValidationDialog::closeEvent(QCloseEvent* event)
{
    event->ignore();
    reject();
}
} // namespace tatsu
