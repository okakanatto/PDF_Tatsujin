#include "form_data_dialog.h"
#include "pdfexception.h"
#include <memory>

namespace tatsu
{
FormDataDialog::FormDataDialog(PDFDocument document, bool importValues, QString suggestedPath,
                               QWidget* parent)
    : QDialog(parent), snapshot(std::move(document)), importing(importValues)
{
    setObjectName("formDataDialog");
    setWindowTitle(importing ? "フォーム入力データを読み込む" : "フォーム入力データを書き出す");
    resize(740, 620);
    setMinimumSize(640, 480);
    auto layout = new QVBoxLayout(this);
    auto note = new QLabel(
        importing ? "現在のPDFへ読み込む入力値を確認し、明示的に適用します。すべての項目が検証でき"
                    "た候補だけを反映し、1回のUndoで戻せます。"
                  : "入力値をXFDFへ書き出します。このファイルには値が平文で含まれ、PDFの画像や署名"
                    "・保護設定は含みません。現在のPDFと保存先・Undoは保持します。");
    note->setWordWrap(true);
    layout->addWidget(note);
    settings = new QWidget;
    auto controls = new QHBoxLayout(settings);
    controls->setContentsMargins(0, 0, 0, 0);
    path = new QLineEdit(suggestedPath);
    path->setObjectName("formDataPath");
    path->setAccessibleName(importing ? "読み込むXFDF入力データ" : "新しいXFDF保存先");
    controls->addWidget(path, 1);
    auto browse = new QPushButton(importing ? "読み込み元…" : "保存先…");
    browse->setObjectName("formDataBrowse");
    controls->addWidget(browse);
    connect(browse, &QPushButton::clicked, this,
            [this]
            {
                auto selected =
                    importing ? QFileDialog::getOpenFileName(this, "入力データを選ぶ", path->text(),
                                                             "XFDF (*.xfdf *.xml)")
                              : QFileDialog::getSaveFileName(this, "新しい入力データの保存先",
                                                             path->text(), "XFDF (*.xfdf)", nullptr,
                                                             QFileDialog::DontConfirmOverwrite);
                if (!selected.isEmpty())
                    path->setText(selected);
            });
    load = new QPushButton("内容を読み込む");
    load->setObjectName("formDataLoad");
    load->setVisible(importing);
    controls->addWidget(load);
    connect(load, &QPushButton::clicked, this, &FormDataDialog::start);
    layout->addWidget(settings);
    table = new QTableWidget;
    table->setObjectName("formDataValues");
    table->setAccessibleName("フォーム入力値の一覧");
    table->setColumnCount(importing ? 3 : 2);
    table->setHorizontalHeaderLabels(importing ? QStringList{"項目名", "現在の値", "読み込む値"}
                                               : QStringList{"項目名", "出力する値"});
    table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table->setSelectionBehavior(QAbstractItemView::SelectRows);
    table->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    table->verticalHeader()->hide();
    table->setWordWrap(true);
    layout->addWidget(table, 1);
    message = new QLabel(
        importing ? "読み込み元を選び、項目と値を確認してください。PDFやURLの参照情報は開きません。"
                  : "書き出す入力値を確認し、新しい保存先を選んでください。");
    message->setObjectName("formDataMessage");
    message->setWordWrap(true);
    message->setTextFormat(Qt::PlainText);
    layout->addWidget(message);
    auto actions = new QDialogButtonBox;
    apply = actions->addButton(importing ? "確認した値を適用" : "入力データを書き出す",
                               QDialogButtonBox::AcceptRole);
    apply->setObjectName("formDataApply");
    apply->setProperty("primary", true);
    apply->setEnabled(!importing);
    cancel = actions->addButton("キャンセル", QDialogButtonBox::RejectRole);
    cancel->setObjectName("formDataCancel");
    layout->addWidget(actions);
    connect(apply, &QPushButton::clicked, this, &FormDataDialog::accept);
    connect(cancel, &QPushButton::clicked, this, &FormDataDialog::reject);
    connect(path, &QLineEdit::textChanged, this,
            [this]
            {
                if (importing)
                {
                    result = {};
                    loadedHash.clear();
                    table->setRowCount(0);
                    apply->setEnabled(false);
                }
            });
    if (!importing)
        showExportValues();
}
FormDataDialog::~FormDataDialog()
{
    cancelled = true;
    if (job)
    {
        disconnect(job, nullptr, this, nullptr);
        job->wait();
    }
}
void FormDataDialog::showExportValues()
{
    try
    {
        const auto data = formDataValues(snapshot);
        table->setRowCount(data.fields.size());
        for (int i = 0; i < data.fields.size(); ++i)
        {
            table->setItem(i, 0, new QTableWidgetItem(data.fields[i].name));
            table->setItem(i, 1, new QTableWidgetItem(data.fields[i].values.join('\n')));
        }
        table->resizeRowsToContents();
        message->setText(QString("出力 %1項目。対象外 %2項目。%3")
                             .arg(data.fields.size())
                             .arg(data.unsupported.size())
                             .arg(data.unsupported.join("、")));
    }
    catch (const std::exception& error)
    {
        apply->setEnabled(false);
        message->setText(QString::fromUtf8(error.what()));
    }
}
void FormDataDialog::start()
{
    if (job)
        return;
    const auto selected = QFileInfo(path->text()).absoluteFilePath();
    struct WorkResult
    {
        bool success = false;
        FormDataImport candidate;
        QByteArray hash;
        QString error;
    };
    auto work = std::make_shared<WorkResult>();
    settings->setEnabled(false);
    apply->setEnabled(false);
    cancelled = false;
    message->setText(importing ? "入力値と外観を検証しています…"
                               : "新しいXFDFファイルを書き出しています…");
    job = QThread::create(
        [this, selected, work, document = snapshot]
        {
            try
            {
                if (importing)
                {
                    QFile file(selected);
                    if (!file.open(QIODevice::ReadOnly) || file.size() > 4 * 1024 * 1024)
                        fail("4MiB以内のXFDF入力データを選んでください。");
                    const auto data = file.readAll();
                    work->hash = QCryptographicHash::hash(data, QCryptographicHash::Sha256);
                    work->candidate = importFormData(
                        document, decodeXfdf(data), [this] { return cancelled.load(); },
                        [this](int index, int total)
                        {
                            QMetaObject::invokeMethod(
                                this,
                                [this, index, total]
                                {
                                    if (job && !cancelled)
                                        message->setText(
                                            QString("入力値と外観を検証しています… %1 / %2")
                                                .arg(index)
                                                .arg(total));
                                },
                                Qt::QueuedConnection);
                        });
                    if (fileHash(selected) != work->hash)
                        fail("読み込み元が処理中に変更されました。文書は変更していません。");
                }
                else
                    exportFormData(document, selected, [this] { return cancelled.load(); });
                work->success = true;
            }
            catch (const pdf::PDFException&)
            {
                work->error = "PDFのフォーム処理に失敗しました。文書は変更していません。";
            }
            catch (const std::exception& error)
            {
                work->error = QString::fromUtf8(error.what());
            }
            catch (...)
            {
                work->error = "入力データを処理できません。文書は変更していません。";
            }
        });
    auto launched = job;
    launched->setParent(this);
    connect(launched, &QThread::finished, this,
            [this, launched, work, selected]
            {
                job = nullptr;
                launched->deleteLater();
                if (work->success && !importing)
                {
                    completedPath = selected;
                    QDialog::accept();
                    return;
                }
                if (cancelled)
                {
                    QDialog::reject();
                    return;
                }
                settings->setEnabled(true);
                if (!work->success)
                {
                    message->setText(work->error);
                    apply->setEnabled(!importing);
                    return;
                }
                result = work->candidate;
                loadedHash = work->hash;
                table->setRowCount(result.changes.size());
                for (int i = 0; i < result.changes.size(); ++i)
                {
                    const auto& change = result.changes[i];
                    table->setItem(i, 0, new QTableWidgetItem(change.name));
                    table->setItem(i, 1, new QTableWidgetItem(change.before.join('\n')));
                    table->setItem(i, 2, new QTableWidgetItem(change.after.join('\n')));
                }
                table->resizeRowsToContents();
                apply->setEnabled(!result.changes.isEmpty());
                message->setText(
                    result.changes.isEmpty()
                        ? "現在の入力値と同じです。適用する変更はありません。"
                        : QString("%1項目の変更を確認してください。まだ文書へ適用していません。")
                              .arg(result.changes.size()));
            });
    launched->start();
}
void FormDataDialog::accept()
{
    if (job)
        return;
    if (!importing)
    {
        start();
        return;
    }
    if (result.changes.isEmpty())
        return;
    try
    {
        if (loadedHash.isEmpty() ||
            fileHash(QFileInfo(path->text()).absoluteFilePath()) != loadedHash)
            fail("読み込み元が更新されました。内容を読み込み直してください。");
    }
    catch (const std::exception& error)
    {
        apply->setEnabled(false);
        message->setText(QString::fromUtf8(error.what()));
        return;
    }
    QDialog::accept();
}
void FormDataDialog::reject()
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
void FormDataDialog::closeEvent(QCloseEvent* event)
{
    if (job)
    {
        event->ignore();
        reject();
    }
    else
        QDialog::closeEvent(event);
}
FormDataImport FormDataDialog::candidate() const
{
    return result;
}
QString FormDataDialog::savedPath() const
{
    return completedPath;
}
} // namespace tatsu
