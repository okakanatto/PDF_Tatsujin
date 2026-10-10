#include "batch_dialog.h"
#include "ocr_language.h"
#include "pdfexception.h"
#include <memory>

namespace tatsu
{
BatchDialog::BatchDialog(QWidget* parent) : QDialog(parent)
{
    setObjectName("batchDialog");
    setWindowTitle("複数PDFをまとめて処理");
    resize(1050, 700);
    setMinimumSize(800, 480);
    auto layout = new QVBoxLayout(this);
    auto note = new QLabel("保存済みのPDFを順番に処理し、新しいPDFへ出力します。現在の未保存変更は"
                           "入力に含みません。原本と、途中までに保存した出力は保持します。");
    note->setWordWrap(true);
    layout->addWidget(note);
    settings = new QWidget;
    auto controls = new QGridLayout(settings);
    controls->setContentsMargins(0, 0, 0, 0);
    auto add = new QPushButton("PDFを追加…");
    add->setObjectName("batchAdd");
    controls->addWidget(add, 0, 0);
    auto remove = new QPushButton("選択を削除");
    remove->setObjectName("batchRemove");
    controls->addWidget(remove, 0, 1);
    auto up = new QPushButton("上へ");
    up->setObjectName("batchUp");
    controls->addWidget(up, 0, 2);
    auto down = new QPushButton("下へ");
    down->setObjectName("batchDown");
    controls->addWidget(down, 0, 3);
    controls->setColumnStretch(1, 1);
    for (auto button : {add, remove, up, down})
        button->setSizePolicy(QSizePolicy::Maximum, QSizePolicy::Fixed);
    operation = new QComboBox;
    operation->setObjectName("batchOperation");
    operation->addItems({"文字を認識（OCR）", "画質を保つ容量最適化"});
    controls->addWidget(new QLabel("処理"), 1, 0);
    controls->addWidget(operation, 1, 1, 1, 2);
    language = new QComboBox;
    language->setObjectName("batchLanguage");
    language->addItems(ocrLanguageLabels());
    controls->addWidget(language, 1, 3, 1, 2);
    directory = new QLineEdit;
    directory->setObjectName("batchDirectory");
    directory->setPlaceholderText("新しいPDFを保存するフォルダ");
    controls->addWidget(new QLabel("出力先"), 2, 0);
    controls->addWidget(directory, 2, 1, 1, 3);
    auto browse = new QPushButton("フォルダを選ぶ…");
    browse->setObjectName("batchBrowse");
    controls->addWidget(browse, 2, 4);
    layout->addWidget(settings);
    table = new QTableWidget(0, 4);
    table->setObjectName("batchFiles");
    table->setHorizontalHeaderLabels({"入力PDF", "新しい出力", "状態", "処理結果・進捗"});
    table->setSelectionBehavior(QAbstractItemView::SelectRows);
    table->setSelectionMode(QAbstractItemView::SingleSelection);
    table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table->setWordWrap(false);
    table->verticalHeader()->setVisible(false);
    table->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    table->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    table->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    table->horizontalHeader()->setSectionResizeMode(3, QHeaderView::Stretch);
    layout->addWidget(table, 1);
    auto scope =
        new QLabel("OCRは各PDFの全ページが対象です。最大100入力、入力ごと500ページ・256MiB。既存出"
                   "力と名前の衝突は拒否します。処理不要な入力には出力を作りません。");
    scope->setWordWrap(true);
    layout->addWidget(scope);
    message = new QLabel("入力の順序・処理・出力先を確認してください。");
    message->setObjectName("batchMessage");
    message->setWordWrap(true);
    message->setTextFormat(Qt::PlainText);
    layout->addWidget(message);
    progress = new QProgressBar;
    progress->setObjectName("batchProgress");
    progress->hide();
    layout->addWidget(progress);
    auto buttons = new QHBoxLayout;
    buttons->addStretch();
    start = new QPushButton("一括処理を開始");
    start->setObjectName("batchStart");
    start->setDefault(true);
    buttons->addWidget(start);
    cancel = new QPushButton("閉じる");
    cancel->setObjectName("batchCancel");
    buttons->addWidget(cancel);
    layout->addLayout(buttons);
    connect(add, &QPushButton::clicked, this,
            [this]
            {
                const auto selected =
                    QFileDialog::getOpenFileNames(this, "処理するPDFを追加", {}, "PDF (*.pdf)");
                if (selected.isEmpty())
                    return;
                QStringList next = inputs;
                for (const auto& path : selected)
                {
                    bool duplicate = false;
                    for (const auto& current : next)
                        duplicate |= sameFilePath(path, current);
                    if (!duplicate)
                        next << path;
                }
                setInputs(next);
            });
    connect(remove, &QPushButton::clicked, this,
            [this]
            {
                const auto row = table->currentRow();
                if (row >= 0)
                {
                    inputs.removeAt(row);
                    rebuild();
                }
            });
    auto move = [this](int delta)
    {
        const auto row = table->currentRow(), target = row + delta;
        if (row >= 0 && target >= 0 && target < inputs.size())
        {
            inputs.move(row, target);
            rebuild();
            table->selectRow(target);
        }
    };
    connect(up, &QPushButton::clicked, this, [move] { move(-1); });
    connect(down, &QPushButton::clicked, this, [move] { move(1); });
    connect(browse, &QPushButton::clicked, this,
            [this]
            {
                const auto path =
                    QFileDialog::getExistingDirectory(this, "新しいPDFの出力先", directory->text());
                if (!path.isEmpty())
                    directory->setText(path);
            });
    connect(operation, qOverload<int>(&QComboBox::currentIndexChanged), this,
            [this]
            {
                language->setEnabled(operation->currentIndex() == 0);
                rebuild();
            });
    connect(language, qOverload<int>(&QComboBox::currentIndexChanged), this, [this] { rebuild(); });
    connect(directory, &QLineEdit::textChanged, this,
            [this]
            {
                directory->setToolTip(directory->text());
                rebuild();
            });
    connect(start, &QPushButton::clicked, this, &BatchDialog::run);
    connect(cancel, &QPushButton::clicked, this, &BatchDialog::reject);
}
BatchDialog::~BatchDialog()
{
    cancelled = true;
    if (job)
    {
        disconnect(job, nullptr, this, nullptr);
        job->wait();
    }
}
void BatchDialog::setInputs(const QStringList& paths)
{
    if (job)
        return;
    if (paths.size() > 100)
    {
        message->setText("最大100件まで追加できます。入力は変更していません。");
        return;
    }
    inputs = paths;
    rebuild();
}
const QVector<BatchItemResult>& BatchDialog::results() const
{
    return completed;
}
void BatchDialog::rebuild()
{
    if (job)
        return;
    completed.clear();
    table->setRowCount(inputs.size());
    for (int index = 0; index < inputs.size(); ++index)
    {
        const auto suffix = operation->currentIndex() == 0 ? "_ocr.pdf" : "_optimized.pdf";
        const auto output = QFileInfo(inputs[index]).completeBaseName() + suffix;
        const QStringList values{QFileInfo(inputs[index]).fileName(), output, "待機", ""};
        for (int column = 0; column < 4; ++column)
        {
            auto item = new QTableWidgetItem(values[column]);
            item->setToolTip(column == 0   ? inputs[index]
                             : column == 1 ? QDir(directory->text()).filePath(output)
                                           : values[column]);
            table->setItem(index, column, item);
        }
    }
    start->setEnabled(!inputs.isEmpty());
}
void BatchDialog::busy(bool value)
{
    settings->setEnabled(!value);
    table->setEnabled(!value);
    start->setEnabled(!value && !inputs.isEmpty());
    progress->setVisible(value);
    cancel->setText(value ? "処理を中止" : "閉じる");
}
void BatchDialog::updateRow(int index, const BatchItemResult& row)
{
    if (index < 0 || index >= table->rowCount())
        return;
    table->item(index, 1)->setToolTip(row.destination);
    table->item(index, 2)->setText(batchStatusText(row.status));
    table->item(index, 3)->setText(row.message);
    table->item(index, 3)->setToolTip(row.message);
    for (int column = 0; column < 4; ++column)
        table->item(index, column)
            ->setBackground(row.status == BatchStatus::Failed  ? QColor("#fff0ec")
                            : row.status == BatchStatus::Saved ? QColor("#eaf6ed")
                                                               : QColor(Qt::white));
    if (row.status == BatchStatus::Running)
    {
        table->scrollToItem(table->item(index, 0));
        message->setText(QString("%1 / %2：%3").arg(index + 1).arg(inputs.size()).arg(row.message));
    }
    progress->setRange(0, inputs.size());
    progress->setValue(index + (row.status == BatchStatus::Running ? 0 : 1));
}
void BatchDialog::run()
{
    if (job)
        return;
    const auto previousResults = completed;
    rebuild();
    const auto sources = inputs;
    const auto output = directory->text();
    const auto kind =
        operation->currentIndex() == 0 ? BatchOperation::Ocr : BatchOperation::Optimize;
    const auto lang = ocrLanguageCodes()[language->currentIndex()];
    const auto expectedGeneration = ++generation;
    cancelled = false;
    closeRequested = false;
    busy(true);
    progress->setRange(0, 0);
    message->setText("入力と新しい出力先を照合しています…");
    struct Work
    {
        QVector<BatchItemResult> result;
        QString error;
    };
    auto work = std::make_shared<Work>();
    job = QThread::create(
        [this, sources, output, kind, lang, expectedGeneration, work]
        {
            try
            {
                const auto plan =
                    prepareBatch(sources, output, kind, lang, [this] { return cancelled.load(); });
                work->result = processBatch(
                    plan, [this] { return cancelled.load(); },
                    [this, expectedGeneration](int index, const BatchItemResult& row)
                    {
                        QMetaObject::invokeMethod(
                            this,
                            [this, expectedGeneration, index, row]
                            {
                                if (job && generation == expectedGeneration)
                                    updateRow(index, row);
                            },
                            Qt::QueuedConnection);
                    });
            }
            catch (const pdf::PDFException&)
            {
                work->error = "入力PDFを処理できません。";
            }
            catch (const std::exception& error)
            {
                work->error = QString::fromUtf8(error.what()).left(1000);
            }
            catch (...)
            {
                work->error = "一括処理を開始できません。入力は保持しています。";
            }
        });
    auto launched = job;
    launched->setParent(this);
    connect(
        launched, &QThread::finished, this,
        [this, launched, work, previousResults]
        {
            job = nullptr;
            launched->deleteLater();
            completed = std::move(work->result);
            if (!work->error.isEmpty() && !cancelled && completed.isEmpty())
                completed = previousResults;
            if (cancelled && completed.isEmpty())
                for (const auto& source : inputs)
                {
                    BatchItemResult row;
                    row.status = BatchStatus::NotProcessed;
                    row.source = source;
                    row.destination =
                        QDir(directory->text())
                            .absoluteFilePath(
                                QFileInfo(source).completeBaseName() +
                                (operation->currentIndex() == 0 ? "_ocr.pdf" : "_optimized.pdf"));
                    row.message = "開始前の中止により未処理です。";
                    completed << row;
                }
            for (int index = 0; index < completed.size(); ++index)
                updateRow(index, completed[index]);
            busy(false);
            if (!work->error.isEmpty())
                message->setText(work->error);
            else
            {
                int saved = 0, failed = 0, unneeded = 0, unfinished = 0;
                for (const auto& row : completed)
                {
                    saved += row.status == BatchStatus::Saved;
                    failed += row.status == BatchStatus::Failed;
                    unneeded += row.status == BatchStatus::Unneeded;
                    unfinished += row.status == BatchStatus::Cancelled ||
                                  row.status == BatchStatus::NotProcessed;
                }
                message->setText(QString("処理終了：保存済み%1、処理不要%2、失敗%3、取消・未処理%"
                                         "4。保存済みの出力と原本は保持しています。")
                                     .arg(saved)
                                     .arg(unneeded)
                                     .arg(failed)
                                     .arg(unfinished));
            }
            if (closeRequested)
                QDialog::reject();
        });
    launched->start();
}
void BatchDialog::reject()
{
    if (job)
    {
        cancelled = true;
        closeRequested = sender() != cancel;
        message->setText("中止しています… 保存済みの出力は保持します。");
        return;
    }
    QDialog::reject();
}
} // namespace tatsu
