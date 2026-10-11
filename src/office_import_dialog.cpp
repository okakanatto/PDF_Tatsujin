#include "office_import_dialog.h"
#include "office_import.h"

namespace tatsu
{
OfficeImportDialog::OfficeImportDialog(const QString& path, QWidget* parent)
    : QDialog(parent), worker(this), source(path)
{
    setObjectName("officeImportDialog");
    setWindowTitle("Office文書からPDFを作成");
    resize(780, 680);
    auto layout = new QVBoxLayout(this);
    auto note =
        new QLabel(QFileInfo(path).fileName() +
                   "\n変換した配置を確認して、新しいPDFを開きます。元の文書は変更しません。");
    note->setTextFormat(Qt::PlainText);
    note->setWordWrap(true);
    layout->addWidget(note);
    spacing = new QCheckBox("日英・数字の間の自動字間を抑える（配置が変わります）");
    spacing->setObjectName("officeImportSuppressSpacing");
    spacing->setVisible(QFileInfo(path).suffix().compare("docx", Qt::CaseInsensitive) == 0);
    spacing->setToolTip("入力した空白は保持します。日本語と英数字の境界の自動字間だけを抑えます。");
    layout->addWidget(spacing);
    preview = new PageRegionPreview;
    preview->setObjectName("officeImportPreview");
    preview->navigationEnabled = true;
    preview->setAccessibleName("変換したPDFのプレビュー。Ctrlとマウスホイールで拡大・縮小");
    layout->addWidget(preview, 1);
    auto navigation = new QHBoxLayout;
    auto label = new QLabel("ページ");
    page = new QSpinBox;
    page->setObjectName("officeImportPage");
    page->setRange(1, 1);
    page->setEnabled(false);
    label->setBuddy(page);
    navigation->addWidget(label);
    navigation->addWidget(page);
    for (const auto& item : {qMakePair(QString("拡大＋"), 1.25), qMakePair(QString("縮小−"), .8),
                             qMakePair(QString("全体"), 0.0)})
    {
        auto button = new QPushButton(item.first);
        button->setAutoDefault(false);
        connect(button, &QPushButton::clicked, this,
                [this, factor = item.second]
                {
                    if (factor == 0)
                        preview->fitPage();
                    else
                        preview->setZoom(preview->zoom() * factor, preview->rect().center());
                });
        navigation->addWidget(button);
    }
    navigation->addStretch();
    engine = new QPushButton("変換エンジンを選ぶ…");
    engine->setObjectName("officeImportEngine");
    navigation->addWidget(engine);
    connect(engine, &QPushButton::clicked, this,
            [this]
            {
                if (worker.running())
                    return;
                const auto chosen = QFileDialog::getOpenFileName(
                    this, "LibreOfficeのsoffice.comを選ぶ", {}, "LibreOffice (soffice.com)");
                if (chosen.isEmpty())
                    return;
                QSettings().setValue("office/converter", QFileInfo(chosen).absoluteFilePath());
                convert();
            });
    layout->addLayout(navigation);
    message = new QLabel("変換しています…");
    message->setObjectName("officeImportStatus");
    message->setTextFormat(Qt::PlainText);
    message->setWordWrap(true);
    message->setTextInteractionFlags(Qt::TextSelectableByMouse);
    layout->addWidget(message);
    auto buttons = new QDialogButtonBox;
    apply = buttons->addButton("新しいPDFを開く", QDialogButtonBox::AcceptRole);
    apply->setObjectName("officeImportApply");
    apply->setProperty("primary", true);
    apply->setEnabled(false);
    auto cancel = buttons->addButton("キャンセル", QDialogButtonBox::RejectRole);
    cancel->setObjectName("officeImportCancel");
    layout->addWidget(buttons);
    connect(apply, &QPushButton::clicked, this, &OfficeImportDialog::accept);
    connect(cancel, &QPushButton::clicked, this, &OfficeImportDialog::reject);
    worker.ready = [this](CandidatePreviewResult result)
    {
        engine->setEnabled(true);
        if (!result.error.isEmpty())
        {
            message->setText(result.error);
            apply->setEnabled(false);
            return;
        }
        candidate = std::move(result.document);
        preview->image = std::move(result.image);
        preview->physical = result.dimensions;
        preview->fitPage();
        preview->update();
        page->setRange(1, candidate->getCatalog()->getPageCount());
        page->setEnabled(true);
        apply->setEnabled(true);
        message->setText(QString("%1ページを変換しました。配置を確認してください。")
                             .arg(candidate->getCatalog()->getPageCount()));
    };
    connect(page, &QSpinBox::valueChanged, this,
            [this](int number)
            {
                if (!candidate)
                    return;
                apply->setEnabled(false);
                message->setText("ページを表示しています…");
                const auto document = *candidate;
                worker.request([document](const auto&) { return document; }, number - 1, 1100);
            });
    connect(spacing, &QCheckBox::toggled, this, [this] { convert(); });
    convert();
}
void OfficeImportDialog::convert()
{
    candidate.reset();
    apply->setEnabled(false);
    engine->setEnabled(false);
    page->setEnabled(false);
    const QSignalBlocker blocker(page);
    page->setValue(1);
    message->setText("変換しています…");
    const auto converter = officeConverterPath();
    const auto path = source;
    const auto suppress = spacing->isChecked();
    worker.request([path, converter, suppress](const auto& cancelled)
                   { return importOffice(path, converter, cancelled, suppress); }, 0, 1100);
}
void OfficeImportDialog::accept()
{
    if (candidate && apply->isEnabled())
        QDialog::accept();
}
PDFDocument OfficeImportDialog::takeDocument()
{
    if (!candidate)
        fail("変換したPDFがありません。");
    return std::move(*candidate);
}
void OfficeImportDialog::reject()
{
    apply->setEnabled(false);
    message->setText("変換を取り消しています…");
    worker.cancel([this] { QDialog::reject(); });
}
void OfficeImportDialog::closeEvent(QCloseEvent* event)
{
    event->ignore();
    reject();
}
} // namespace tatsu
