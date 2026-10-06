#include "writing_panel.h"
#include <QImageReader>

namespace tatsu
{
WritingPanel::WritingPanel(Document* doc, Canvas* view, bool signature, QWidget* parent)
    : QWidget(parent), document(doc), canvas(view), imageSignature(signature)
{
    setObjectName(signature ? "imageSignaturePanel" : "writingPanel");
    auto layout = new QVBoxLayout(this);
    layout->setContentsMargins(18, 16, 18, 18);
    auto title = new QLabel(signature ? "画像の署名" : "書き込み");
    title->setProperty("sectionTitle", true);
    layout->addWidget(title);
    auto note = new QLabel(signature ? "見た目の署名です。本人性の証明にはなりません。"
                                     : "文字・日付・画像を追加します。保存後も再編集できます。");
    note->setWordWrap(true);
    layout->addWidget(note);
    kind = new QComboBox;
    kind->setObjectName("writingKind");
    kind->addItems(signature ? QStringList{"画像署名"} : QStringList{"テキスト", "日付", "画像"});
    kind->setAccessibleName("書き込みの種類");
    layout->addWidget(kind);
    text = new QPlainTextEdit;
    text->setObjectName("writingText");
    text->setAccessibleName("追加する文字");
    text->setMaximumHeight(130);
    layout->addWidget(text);
    sizeLabel = new QLabel("文字サイズ（Noto Sans JP）");
    layout->addWidget(sizeLabel);
    size = new QDoubleSpinBox;
    size->setObjectName("writingSize");
    size->setRange(6, 144);
    size->setValue(20);
    size->setSuffix(" pt");
    size->setAccessibleName("追加文字のサイズ");
    layout->addWidget(size);
    colorButton = new QPushButton("文字色を選ぶ");
    layout->addWidget(colorButton);
    connect(colorButton, &QPushButton::clicked, this,
            [this]
            {
                const auto color = QColorDialog::getColor(ink, this);
                if (color.isValid())
                    ink = color;
            });
    imageButton = new QPushButton("PNG・JPEG画像を選ぶ");
    imageButton->setObjectName("chooseWritingImage");
    layout->addWidget(imageButton);
    connect(imageButton, &QPushButton::clicked, this,
            [this]
            {
                const auto path = QFileDialog::getOpenFileName(this, "画像を選ぶ", {},
                                                               "画像 (*.png *.jpg *.jpeg)");
                if (!path.isEmpty())
                    guard([&] { setImage(path); });
            });
    preview = new QLabel("画像を選んでください");
    preview->setAlignment(Qt::AlignCenter);
    preview->setMinimumHeight(70);
    preview->setMaximumHeight(105);
    layout->addWidget(preview);
    widthLabel = new QLabel("幅（縦横比を保持）");
    layout->addWidget(widthLabel);
    width = new QDoubleSpinBox;
    width->setObjectName("writingImageWidth");
    width->setRange(6, 3000);
    width->setValue(144);
    width->setSuffix(" pt");
    width->setAccessibleName("画像の幅");
    layout->addWidget(width);
    auto placeButton = new QPushButton("ページをクリックして配置");
    placeButton->setObjectName("placeWriting");
    placeButton->setProperty("primary", true);
    layout->addWidget(placeButton);
    connect(placeButton, &QPushButton::clicked, this,
            [this]
            {
                guard(
                    [&]
                    {
                        document->editable();
                        if (isImage(currentKind()) ? image.isNull()
                                                   : text->toPlainText().trimmed().isEmpty())
                            fail("配置する内容を指定してください。");
                        if (requestPlacement)
                            requestPlacement();
                    });
            });
    auto update = new QPushButton("選択した要素を更新");
    update->setObjectName("updateWriting");
    layout->addWidget(update);
    connect(update, &QPushButton::clicked, this,
            [this]
            {
                guard(
                    [&]
                    {
                        const auto item = selected();
                        const auto reference = put(item.rect.topLeft(), item.ref);
                        if (changed)
                            changed(reference);
                    });
            });
    auto remove = new QPushButton("選択した要素を削除");
    remove->setObjectName("removeWriting");
    layout->addWidget(remove);
    connect(remove, &QPushButton::clicked, this,
            [this]
            {
                guard(
                    [&]
                    {
                        document->eraseSignature(canvas->page, selected());
                        canvas->selected = -1;
                        if (changed)
                            changed({});
                    });
            });
    if (signature)
    {
        auto save = new QPushButton("この署名をPCに保存");
        layout->addWidget(save);
        connect(save, &QPushButton::clicked, this,
                [this]
                {
                    guard(
                        [&]
                        {
                            if (saveTemplate)
                                saveTemplate(currentTemplate());
                        });
                });
    }
    layout->addStretch();
    connect(kind, &QComboBox::currentIndexChanged, this, [this] { syncKind(true); });
    syncKind(false);
}
void WritingPanel::guard(const std::function<void()>& operation)
{
    try
    {
        operation();
    }
    catch (const std::exception& error)
    {
        QMessageBox::warning(this, "操作を完了できません", QString::fromUtf8(error.what()));
    }
}
OverlayKind WritingPanel::currentKind() const
{
    return imageSignature ? OverlayKind::SignatureImage
                          : QList<OverlayKind>{OverlayKind::Text, OverlayKind::Date,
                                               OverlayKind::Image}[kind->currentIndex()];
}
void WritingPanel::syncKind(bool initializeDate)
{
    const bool picture = isImage(currentKind());
    for (auto widget : QList<QWidget*>{text, sizeLabel, size, colorButton})
        widget->setVisible(!picture);
    for (auto widget : QList<QWidget*>{imageButton, preview, widthLabel, width})
        widget->setVisible(picture);
    if (initializeDate && currentKind() == OverlayKind::Date)
        text->setPlainText(QDate::currentDate().toString("yyyy年M月d日"));
}
void WritingPanel::setImage(const QString& path)
{
    QImageReader reader(path);
    const auto format = reader.format().toLower();
    if (format != "png" && format != "jpeg" && format != "jpg")
        fail("PNGまたはJPEG画像を選んでください。");
    reader.setAutoTransform(true);
    auto loaded = reader.read();
    if (loaded.isNull())
        fail("画像を読み込めません: " + reader.errorString());
    image = std::move(loaded);
    replacement = true;
    preview->setPixmap(
        QPixmap::fromImage(image).scaled(240, 100, Qt::KeepAspectRatio, Qt::SmoothTransformation));
    if (!imageSignature)
        kind->setCurrentIndex(2);
}
Signature WritingPanel::selected() const
{
    document->editable();
    const auto items = signatures(document->pdf(), canvas->page);
    if (canvas->selected < 0 || canvas->selected >= items.size())
        fail("要素の枠を選択してください。");
    return items[canvas->selected];
}
void WritingPanel::setSelection(const Signature& item)
{
    QSignalBlocker block(kind);
    if (!imageSignature)
        kind->setCurrentIndex(item.kind == OverlayKind::Date ? 1 : isImage(item.kind) ? 2 : 0);
    text->setPlainText(item.text);
    size->setValue(item.size ? item.size : 20);
    ink = item.color;
    image = {};
    replacement = false;
    if (isImage(item.kind))
    {
        width->setValue(item.rect.width() *
                        document->pdf().getCatalog()->getPage(canvas->page)->getUserUnit());
        preview->setPixmap({});
        preview->setText(QString("保存済み画像 %1 × %2 px\n幅を変えると高さも追従します")
                             .arg(item.imagePixels.width())
                             .arg(item.imagePixels.height()));
    }
    syncKind(false);
}
PDFObjectReference WritingPanel::put(QPointF point, PDFObjectReference old)
{
    if (!isImage(currentKind()))
        return document
            ->putText(canvas->page, currentKind(), text->toPlainText(), point, size->value(), ink,
                      old)
            .ref;
    if (old.isValid() && !replacement && isImage(selected().kind))
    {
        document->resizeImage(canvas->page, selected(), width->value());
        return old;
    }
    return document->putImage(canvas->page, currentKind(), image, point, width->value(), old).ref;
}
PDFObjectReference WritingPanel::place(QPointF point)
{
    return put(point);
}
SignatureTemplate WritingPanel::currentTemplate() const
{
    SignatureTemplate item;
    item.kind = OverlayKind::SignatureImage;
    item.width = width->value();
    item.image = image;
    if (item.image.isNull())
        item.image = overlayImage(document->pdf(), selected());
    return item;
}
void WritingPanel::setTemplate(const SignatureTemplate& item)
{
    image = item.image;
    replacement = true;
    width->setValue(item.width);
    preview->setPixmap(
        QPixmap::fromImage(image).scaled(240, 100, Qt::KeepAspectRatio, Qt::SmoothTransformation));
}
} // namespace tatsu
