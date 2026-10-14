#include "comparison_dialog.h"
#include "pdfexception.h"
#include <cmath>
#include <memory>

namespace tatsu
{
namespace
{
class ComparisonView : public QGraphicsView
{
public:
    explicit ComparisonView(QWidget* parent = nullptr) : QGraphicsView(parent)
    {
        setScene(new QGraphicsScene(this));
        setBackgroundBrush(QColor("#eaf0f6"));
        setDragMode(QGraphicsView::ScrollHandDrag);
        setRenderHint(QPainter::SmoothPixmapTransform);
    }
    double zoom = 0;
    void paper(QImage image, QRect changed = {})
    {
        scene()->clear();
        if (!image.isNull())
        {
            const auto item = scene()->addPixmap(QPixmap::fromImage(image));
            if (!changed.isEmpty())
            {
                QPen pen(QColor("#d44e35"), 2, Qt::DashLine);
                pen.setCosmetic(true);
                scene()->addRect(QRectF(changed).intersected(item->boundingRect()), pen,
                                 QColor(235, 90, 60, 24));
            }
        }
        scene()->setSceneRect(scene()->itemsBoundingRect());
        fit();
    }
    void fit()
    {
        if (scene()->sceneRect().isEmpty())
            return;
        if (zoom == 0)
            fitInView(scene()->sceneRect(), Qt::KeepAspectRatio);
        else
        {
            resetTransform();
            scale(zoom, zoom);
        }
    }

protected:
    void resizeEvent(QResizeEvent* event) override
    {
        QGraphicsView::resizeEvent(event);
        fit();
    }
};
QString propertyName(QString key)
{
    const QMap<QString, QString> labels{
        {"title", "タイトル"},          {"author", "作成者"},         {"subject", "主題"},
        {"keywords", "キーワード"},     {"creator", "作成アプリ"},    {"producer", "PDF生成元"},
        {"creationDate", "作成日時"},   {"modifiedDate", "更新日時"}, {"bookmarks", "しおり"},
        {"extraInfo", "追加の文書情報"}};
    return labels.value(key, key);
}
QString valueText(const QJsonValue& value)
{
    if (value.isString())
        return value.toString();
    if (value.isArray())
    {
        QStringList lines;
        for (const auto& row : value.toArray())
        {
            const auto item = row.toObject(), target = item["target"].toObject();
            lines << item["title"].toString() + " → " +
                         (target["page"].toInt(-1) >= 0
                              ? QString::number(target["page"].toInt() + 1) + "ページ"
                              : target["notice"].toString());
        }
        return lines.join('\n');
    }
    if (value.isObject())
    {
        QStringList lines;
        const auto object = value.toObject();
        for (const auto& key : object.keys())
            lines << key + ": " + object[key].toVariant().toString();
        return lines.join('\n');
    }
    return value.toVariant().toString();
}
QString fieldText(QJsonArray values)
{
    QStringList lines;
    for (const auto& value : values)
    {
        const auto item = value.toObject();
        QStringList entries;
        for (const auto& entry : item["values"].toArray())
            entries << entry.toString();
        lines << item["name"].toString() + ": " + entries.join(" / ");
    }
    return lines.join('\n');
}
QString annotationText(QJsonArray values)
{
    QStringList lines;
    for (const auto& value : values)
    {
        const auto item = value.toObject();
        QString text = item["type"].toString();
        for (const auto* key : {"T", "Subj", "Contents", "URI"})
            if (!item[key].toString().isEmpty())
                text += "\n" + item[key].toString();
        if (item.contains("target"))
        {
            const auto destination = item["target"].toObject();
            const auto page = destination["page"].toInt(-1);
            text += page >= 0 ? QString("\n移動先：%1ページ").arg(page + 1)
                              : "\n" + destination["notice"].toString();
        }
        lines << text;
    }
    return lines.join("\n\n");
}
void clearView(QGraphicsView* view)
{
    static_cast<ComparisonView*>(view)->paper({});
}
QImage previewPage(PDFDocument& document, int page)
{
    const auto physical = pageSize(document.getCatalog()->getPage(page));
    const double scale = std::min(
        2.0, std::sqrt(16000000.0 / (std::ceil(physical.width()) * std::ceil(physical.height()))));
    QStringList diagnostics;
    auto image = renderPage(document, page, scale, true, true, RenderPurpose::View, &diagnostics);
    if (!diagnostics.isEmpty())
        fail("比較ページを完全に表示できません。");
    image.setDevicePixelRatio(scale);
    return image;
}
bool sameInputFiles(const QString& left, const QByteArray& leftHash, const QString& right,
                    const QByteArray& rightHash)
{
    try
    {
        return (left.isEmpty() || fileHash(left) == leftHash) && !right.isEmpty() &&
               fileHash(right) == rightHash;
    }
    catch (...)
    {
        return false;
    }
}
} // namespace
ComparisonDialog::ComparisonDialog(PDFDocument document, QString sourcePath, QByteArray sourceHash,
                                   QWidget* parent)
    : QDialog(parent), snapshot(std::move(document)), originalPath(std::move(sourcePath)),
      originalHash(std::move(sourceHash))
{
    setObjectName("comparisonDialog");
    setWindowTitle("PDFを比較");
    resize(1200, 800);
    setMinimumSize(800, 480);
    auto layout = new QVBoxLayout(this);
    auto note = new QLabel("現在のPDFと比較先を読み取り専用で比較します。変更を含む範囲を赤枠で示し"
                           "ます。元の文書・保存先・Undoは保持します。");
    note->setWordWrap(true);
    layout->addWidget(note);
    settings = new QWidget;
    auto controls = new QGridLayout(settings);
    controls->setContentsMargins(0, 0, 0, 0);
    controls->addWidget(new QLabel("比較先のPDF"), 0, 0);
    path = new QLineEdit;
    path->setObjectName("comparisonPath");
    controls->addWidget(path, 0, 1, 1, 3);
    auto browse = new QPushButton("選ぶ…");
    browse->setObjectName("comparisonBrowse");
    controls->addWidget(browse, 0, 4);
    connect(browse, &QPushButton::clicked, this,
            [this]
            {
                const auto selected =
                    QFileDialog::getOpenFileName(this, "比較先のPDF", path->text(), "PDF (*.pdf)");
                if (!selected.isEmpty())
                    path->setText(selected);
            });
    controls->addWidget(new QLabel("必要なパスワード"), 1, 0);
    password = new QLineEdit;
    password->setObjectName("comparisonPassword");
    password->setEchoMode(QLineEdit::Password);
    password->setMaxLength(1024);
    controls->addWidget(password, 1, 1);
    matching = new QComboBox;
    matching->setObjectName("comparisonMatching");
    matching->addItems({"共通ページに合わせる", "同じページ番号"});
    controls->addWidget(matching, 1, 2);
    start = new QPushButton("比較を開始");
    start->setObjectName("comparisonStart");
    start->setProperty("primary", true);
    controls->addWidget(start, 1, 3, 1, 2);
    layout->addWidget(settings);
    auto toolbar = new QHBoxLayout;
    zoom = new QComboBox;
    zoom->setObjectName("comparisonZoom");
    zoom->addItems({"全体表示", "100%", "150%", "200%"});
    toolbar->addWidget(new QLabel("左右の表示"));
    toolbar->addWidget(zoom);
    auto showText = new QCheckBox("文字・入力値・文書情報を表示");
    showText->setObjectName("comparisonShowDetails");
    toolbar->addWidget(showText);
    toolbar->addStretch();
    auto scope = new QPushButton("比較対象について");
    toolbar->addWidget(scope);
    layout->addLayout(toolbar);
    connect(scope, &QPushButton::clicked, this,
            [this]
            {
                QMessageBox box(QMessageBox::Information, "比較対象",
                                "72dpiの表示、抽出文字、表示widgetがある標準フォーム値、標準注釈の"
                                "内容、しおりと文書情報を比較します。添付内容・スクリプトの動作・タ"
                                "グ構造・レイヤー表示設定・XFA・非対応フォームは対象外です。差がな"
                                "い場合も、PDF全体の"
                                "完全同一や印刷解像度での同一を保証しません。大きく変わったページや"
                                "同じ見た目の繰り返しでは、左右のページ番号を確認してください。",
                                QMessageBox::Ok, this);
                box.setTextFormat(Qt::PlainText);
                box.exec();
            });
    auto body = new QSplitter;
    table = new QTableWidget(0, 3);
    table->setObjectName("comparisonPages");
    table->setHorizontalHeaderLabels({"左", "右", "変更"});
    table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table->setSelectionBehavior(QAbstractItemView::SelectRows);
    table->setSelectionMode(QAbstractItemView::SingleSelection);
    table->verticalHeader()->hide();
    table->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    table->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    table->horizontalHeader()->setSectionResizeMode(2, QHeaderView::Stretch);
    table->setMinimumWidth(200);
    table->setWordWrap(false);
    body->addWidget(table);
    auto previews = new QWidget;
    auto pair = new QHBoxLayout(previews);
    pair->setContentsMargins(0, 0, 0, 0);
    auto pane = [&](QString name, QLabel*& label, QGraphicsView*& view)
    {
        auto area = new QVBoxLayout;
        label = new QLabel(name);
        label->setTextFormat(Qt::PlainText);
        area->addWidget(label);
        view = new ComparisonView;
        view->setObjectName(name == "左" ? "comparisonLeftPreview" : "comparisonRightPreview");
        area->addWidget(view, 1);
        pair->addLayout(area, 1);
    };
    pane("左", leftLabel, leftView);
    pane("右", rightLabel, rightView);
    body->addWidget(previews);
    body->setSizes({280, 850});
    layout->addWidget(body, 1);
    details = new QPlainTextEdit;
    details->setObjectName("comparisonDetails");
    details->setReadOnly(true);
    details->setMaximumHeight(140);
    details->hide();
    layout->addWidget(details);
    connect(showText, &QCheckBox::toggled, details, &QWidget::setVisible);
    auto synchronize = [&](QScrollBar* from, QScrollBar* to)
    {
        connect(from, &QScrollBar::valueChanged, this,
                [from, to](int value)
                {
                    QSignalBlocker block(to);
                    if (from->maximum() > 0)
                        to->setValue(int(double(value) * to->maximum() / from->maximum()));
                });
    };
    synchronize(leftView->verticalScrollBar(), rightView->verticalScrollBar());
    synchronize(rightView->verticalScrollBar(), leftView->verticalScrollBar());
    connect(zoom, qOverload<int>(&QComboBox::currentIndexChanged), this,
            [this](int index)
            {
                for (auto view : {leftView, rightView})
                {
                    auto pane = static_cast<ComparisonView*>(view);
                    pane->zoom = QVector<double>{0, 1, 1.5, 2}[index];
                    pane->fit();
                }
            });
    progress = new QProgressBar;
    progress->setObjectName("comparisonProgress");
    progress->hide();
    layout->addWidget(progress);
    message = new QLabel("比較先を指定してください。比較範囲の違いを確認できます。");
    message->setObjectName("comparisonMessage");
    message->setTextFormat(Qt::PlainText);
    message->setWordWrap(true);
    layout->addWidget(message);
    auto destination = new QHBoxLayout;
    destination->addWidget(new QLabel("平文JSONの新しい出力先"));
    output = new QLineEdit;
    output->setObjectName("comparisonOutput");
    output->setText(originalPath.isEmpty()
                        ? QString()
                        : QFileInfo(originalPath)
                              .dir()
                              .absoluteFilePath(QFileInfo(originalPath).completeBaseName() +
                                                "-comparison.json"));
    destination->addWidget(output, 1);
    outputBrowse = new QPushButton("保存先…");
    outputBrowse->setObjectName("comparisonOutputBrowse");
    destination->addWidget(outputBrowse);
    connect(outputBrowse, &QPushButton::clicked, this,
            [this]
            {
                const auto selected = QFileDialog::getSaveFileName(
                    this, "比較結果の新しい出力先", output->text(), "JSON (*.json)", nullptr,
                    QFileDialog::DontConfirmOverwrite);
                if (!selected.isEmpty())
                    output->setText(selected);
            });
    save = new QPushButton("結果を出力");
    save->setObjectName("comparisonExport");
    save->setEnabled(false);
    destination->addWidget(save);
    cancel = new QPushButton("閉じる");
    cancel->setObjectName("comparisonCancel");
    destination->addWidget(cancel);
    layout->addLayout(destination);
    connect(path, &QLineEdit::textChanged, this, [this] { invalidate(); });
    connect(password, &QLineEdit::textChanged, this, [this] { invalidate(); });
    connect(matching, qOverload<int>(&QComboBox::currentIndexChanged), this,
            [this] { invalidate(); });
    connect(start, &QPushButton::clicked, this, &ComparisonDialog::compare);
    connect(save, &QPushButton::clicked, this, &ComparisonDialog::exportResult);
    connect(table, &QTableWidget::itemSelectionChanged, this, &ComparisonDialog::selectRow);
    connect(cancel, &QPushButton::clicked, this,
            [this]
            {
                if (job)
                    requestCancel();
                else
                    reject();
            });
}
ComparisonDialog::~ComparisonDialog()
{
    cancelled = true;
    for (auto thread : {job, previewJob})
        if (thread)
        {
            disconnect(thread, nullptr, this, nullptr);
            thread->wait();
        }
    password->clear();
}
const ComparisonResult* ComparisonDialog::comparison() const
{
    return result ? &*result : nullptr;
}
void ComparisonDialog::invalidate()
{
    ++generation;
    result.reset();
    desiredRow = -1;
    table->setRowCount(0);
    details->clear();
    clearView(leftView);
    clearView(rightView);
    leftLabel->setText("左");
    rightLabel->setText("右");
    save->setEnabled(false);
    if (job)
        cancelled = true;
    message->setText("比較先と対応方法を確認して、比較を開始してください。");
}
void ComparisonDialog::busy(bool enabled)
{
    settings->setEnabled(!enabled);
    table->setEnabled(!enabled);
    output->setEnabled(!enabled);
    outputBrowse->setEnabled(!enabled);
    save->setEnabled(!enabled && result.has_value());
    progress->setVisible(enabled);
    cancel->setEnabled(true);
    cancel->setText(enabled ? "処理を中止" : "閉じる");
}
bool ComparisonDialog::inputsUnchanged() const
{
    return sameInputFiles(originalPath, originalHash, referencePath, referenceHash);
}
void ComparisonDialog::compare()
{
    if (job)
        return;
    invalidate();
    busy(true);
    cancelled = false;
    closeRequested = false;
    const auto selectedPath = QFileInfo(path->text()).absoluteFilePath(),
               selectedPassword = password->text();
    const ComparisonOptions options{matching->currentIndex() == 0};
    struct Work
    {
        PDFDocument document = PDFDocument{};
        QByteArray hash;
        std::optional<ComparisonResult> result;
        QString error;
    };
    auto work = std::make_shared<Work>();
    progress->setRange(0, 0);
    job = QThread::create(
        [this, left = snapshot, selectedPath, selectedPassword, options, work]
        {
            try
            {
                if (!originalPath.isEmpty() && fileHash(originalPath) != originalHash)
                    fail("元のPDFファイルが更新されました。開き直してから比較してください。");
                work->hash = fileHash(selectedPath);
                work->document = readPdf(selectedPath, selectedPassword);
                auto compared = compareDocuments(
                    left, work->document, options, [this] { return cancelled.load(); },
                    [this](QString text, int done, int total)
                    {
                        QMetaObject::invokeMethod(
                            this,
                            [this, text, done, total]
                            {
                                if (job && !cancelled)
                                {
                                    message->setText(text);
                                    progress->setRange(0, total);
                                    progress->setValue(done);
                                }
                            },
                            Qt::QueuedConnection);
                    });
                if (fileHash(selectedPath) != work->hash ||
                    (!originalPath.isEmpty() && fileHash(originalPath) != originalHash))
                    fail("比較中にPDFファイルが更新されました。結果は反映しません。");
                work->result = std::move(compared);
            }
            catch (const pdf::PDFException&)
            {
                work->error = "PDFの内容を比較できません。文書は変更していません。";
            }
            catch (const std::exception& error)
            {
                work->error = QString::fromUtf8(error.what());
            }
            catch (...)
            {
                work->error = "比較に失敗しました。文書は変更していません。";
            }
        });
    auto launched = job;
    launched->setParent(this);
    connect(launched, &QThread::finished, this,
            [this, launched, work, selectedPath]
            {
                job = nullptr;
                launched->deleteLater();
                if (!cancelled && work->result)
                {
                    reference = std::move(work->document);
                    referenceHash = work->hash;
                    referencePath = selectedPath;
                    result = std::move(work->result);
                    showResult();
                }
                else
                    message->setText(cancelled ? "比較を中止しました。結果は反映していません。"
                                               : work->error);
                busy(false);
                if (closeRequested)
                    QDialog::reject();
            });
    launched->start();
}
void ComparisonDialog::showResult()
{
    const auto& data = *result;
    table->setRowCount(data.pages.size() + data.propertyChanges.size());
    for (int index = 0; index < table->rowCount(); ++index)
    {
        QStringList values;
        bool changed = true;
        if (index < data.pages.size())
        {
            const auto& row = data.pages[index];
            values = {row.left < 0 ? "—" : QString::number(row.left + 1),
                      row.right < 0 ? "—" : QString::number(row.right + 1), row.description()};
            changed = row.changed();
        }
        else
            values = {"文書", "全体",
                      propertyName(data.propertyChanges[index - data.pages.size()])};
        for (int column = 0; column < 3; ++column)
        {
            auto item = new QTableWidgetItem(values[column]);
            item->setToolTip(values[column]);
            if (changed)
                item->setBackground(QColor("#fff3ee"));
            table->setItem(index, column, item);
        }
    }
    message->setText(
        QString("比較完了：変更を含む%1対、文書情報%2項目。左右のページ番号を確認してください。")
            .arg(data.changedPages())
            .arg(data.propertyChanges.size()));
    table->selectRow(0);
}
void ComparisonDialog::selectRow()
{
    if (!result)
        return;
    desiredRow = table->currentRow();
    ++generation;
    clearView(leftView);
    clearView(rightView);
    if (desiredRow < 0)
        return;
    if (desiredRow >= result->pages.size())
    {
        const auto key = result->propertyChanges[desiredRow - result->pages.size()];
        leftLabel->setText("左：" + propertyName(key));
        rightLabel->setText("右：" + propertyName(key));
        details->setPlainText("左\n" + valueText(result->leftProperties[key]) + "\n\n右\n" +
                              valueText(result->rightProperties[key]));
        findChild<QCheckBox*>("comparisonShowDetails")->setChecked(true);
        return;
    }
    const auto& row = result->pages[desiredRow];
    leftLabel->setText(row.left < 0 ? "左：対応ページなし"
                                    : QString("左：%1ページ").arg(row.left + 1));
    rightLabel->setText(row.right < 0 ? "右：対応ページなし"
                                      : QString("右：%1ページ").arg(row.right + 1));
    details->setPlainText("左の抽出文字\n" + row.leftText + "\n\n右の抽出文字\n" + row.rightText +
                          "\n\n左の入力値\n" + fieldText(row.leftForms) + "\n\n右の入力値\n" +
                          fieldText(row.rightForms) + "\n\n左の注釈・リンク\n" +
                          annotationText(row.leftAnnotations) + "\n\n右の注釈・リンク\n" +
                          annotationText(row.rightAnnotations));
    loadPreview();
}
void ComparisonDialog::loadPreview()
{
    if (previewJob || !result || desiredRow < 0 || desiredRow >= result->pages.size())
        return;
    renderingRow = desiredRow;
    const auto expectedGeneration = generation;
    const auto row = result->pages[desiredRow];
    struct Preview
    {
        QImage left, right;
        QString error;
    };
    auto images = std::make_shared<Preview>();
    previewJob = QThread::create(
        [left = snapshot, right = reference, row, images]() mutable
        {
            try
            {
                if (row.left >= 0)
                    images->left = previewPage(left, row.left);
                if (row.right >= 0)
                    images->right = previewPage(right, row.right);
            }
            catch (...)
            {
                images->error = "比較ページを表示できません。";
            }
        });
    auto launched = previewJob;
    launched->setParent(this);
    connect(launched, &QThread::finished, this,
            [this, launched, expectedGeneration, row, images]
            {
                previewJob = nullptr;
                launched->deleteLater();
                if (expectedGeneration == generation && result && desiredRow == renderingRow)
                {
                    static_cast<ComparisonView*>(leftView)->paper(images->left, row.pixelBounds);
                    static_cast<ComparisonView*>(rightView)->paper(images->right, row.pixelBounds);
                    if (!images->error.isEmpty())
                        message->setText(images->error);
                }
                else
                    loadPreview();
            });
    launched->start();
}
void ComparisonDialog::exportResult()
{
    if (job || !result)
        return;
    const auto destination = QFileInfo(output->text()).absoluteFilePath();
    const auto data = *result;
    struct Export
    {
        bool success = false;
        QString error;
    };
    auto work = std::make_shared<Export>();
    cancelled = false;
    closeRequested = false;
    busy(true);
    progress->setRange(0, 0);
    message->setText("比較結果を平文JSONへ出力しています…");
    job = QThread::create(
        [this, data, destination, work, leftPath = originalPath, leftHash = originalHash,
         rightPath = referencePath, rightHash = referenceHash]
        {
            try
            {
                exportComparison(
                    data, destination,
                    [this, leftPath, leftHash, rightPath, rightHash]
                    {
                        if (!sameInputFiles(leftPath, leftHash, rightPath, rightHash))
                            fail("PDFファイルが更新されました。再比較してから出力してください。");
                        return cancelled.load();
                    });
                work->success = true;
            }
            catch (const pdf::PDFException&)
            {
                work->error = "比較結果を保存できません。";
            }
            catch (const std::exception& error)
            {
                work->error = QString::fromUtf8(error.what());
            }
            catch (...)
            {
                work->error = "比較結果を保存できません。";
            }
        });
    auto launched = job;
    launched->setParent(this);
    connect(launched, &QThread::finished, this,
            [this, launched, work]
            {
                job = nullptr;
                launched->deleteLater();
                busy(false);
                if (work->success)
                    message->setText(
                        "比較結果を保存しました。文字・入力値などを含む平文JSONです。");
                else
                {
                    if (!inputsUnchanged())
                    {
                        invalidate();
                        message->setText("PDFファイルが更新されました。再比較してください。");
                    }
                    else
                        message->setText(cancelled
                                             ? "出力を中止しました。既存ファイルは保持しています。"
                                             : work->error);
                }
                if (closeRequested)
                    QDialog::reject();
            });
    launched->start();
}
void ComparisonDialog::requestCancel()
{
    cancelled = true;
    cancel->setEnabled(false);
    message->setText("中止しています…");
}
void ComparisonDialog::reject()
{
    if (job)
    {
        closeRequested = true;
        requestCancel();
    }
    else
        QDialog::reject();
}
void ComparisonDialog::closeEvent(QCloseEvent* event)
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
