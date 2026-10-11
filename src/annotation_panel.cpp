#include "annotation_panel.h"

namespace tatsu
{
namespace
{
QString name(OverlayKind kind)
{
    return QStringList{"コメント", "矩形", "直線", "矢印", "ハイライト"}.value(
        int(kind) - int(OverlayKind::Comment));
}
} // namespace
AnnotationPanel::AnnotationPanel(Document* doc, Canvas* view, QWidget* parent)
    : QWidget(parent), document(doc), canvas(view)
{
    setObjectName("annotationPanel");
    auto layout = new QVBoxLayout(this);
    layout->setContentsMargins(18, 16, 18, 18);
    auto title = new QLabel("注釈");
    title->setProperty("sectionTitle", true);
    layout->addWidget(title);
    kind = new QComboBox;
    kind->setObjectName("annotationKind");
    kind->addItems({"コメント", "矩形", "直線", "矢印", "文字ハイライト"});
    kind->setAccessibleName("注釈の種類");
    layout->addWidget(new QLabel("新規作成する種類"));
    layout->addWidget(kind);
    contents = new QPlainTextEdit;
    contents->setObjectName("annotationContents");
    contents->setAccessibleName("コメント・注釈の内容");
    contents->setMaximumHeight(100);
    layout->addWidget(new QLabel("内容（コメントは必須）"));
    layout->addWidget(contents);
    auto appearance = new QHBoxLayout;
    lineWidth = new QDoubleSpinBox;
    lineWidth->setObjectName("annotationLineWidth");
    lineWidth->setRange(.5, 24);
    lineWidth->setValue(1.5);
    lineWidth->setEnabled(false);
    lineWidth->setSuffix(" pt");
    lineWidth->setAccessibleName("注釈の線幅");
    auto chooseColor = new QPushButton("色");
    appearance->addWidget(lineWidth);
    appearance->addWidget(chooseColor);
    layout->addLayout(appearance);
    connect(chooseColor, &QPushButton::clicked, this,
            [this]
            {
                auto chosen = QColorDialog::getColor(color, this);
                if (chosen.isValid())
                    color = chosen;
            });
    auto dimensions = new QHBoxLayout;
    width = new QDoubleSpinBox;
    height = new QDoubleSpinBox;
    width->setObjectName("annotationWidth");
    height->setObjectName("annotationHeight");
    width->setPrefix("幅 ");
    height->setPrefix("高さ ");
    width->setAccessibleName("注釈の幅");
    height->setAccessibleName("注釈の高さ");
    for (auto spin : {width, height})
    {
        spin->setRange(1, 3000);
        spin->setSuffix(" pt");
        dimensions->addWidget(spin);
    }
    layout->addLayout(dimensions);
    width->setValue(100);
    height->setValue(60);
    width->setEnabled(false);
    height->setEnabled(false);
    auto place = new QPushButton("クリックしてコメントを配置");
    place->setObjectName("placeAnnotation");
    place->setProperty("primary", true);
    layout->addWidget(place);
    connect(kind, &QComboBox::currentIndexChanged, this,
            [this, place]
            {
                auto type = currentKind();
                place->setText(type == OverlayKind::Highlight ? "選択した文字をハイライト"
                               : type == OverlayKind::Comment ? "クリックしてコメントを配置"
                                                              : "ドラッグして注釈を配置");
                lineWidth->setEnabled(type != OverlayKind::Comment &&
                                      type != OverlayKind::Highlight);
                if (type == OverlayKind::Highlight)
                    color = QColor("#ffdb55");
            });
    connect(place, &QPushButton::clicked, this,
            [this]
            {
                guard(
                    [&]
                    {
                        document->editable();
                        if (currentKind() == OverlayKind::Highlight)
                        {
                            auto selectedText = canvas->selectedTextRects();
                            auto references = putHighlights(*document, selectedText, color);
                            if (changed)
                                changed(references.front());
                        }
                        else
                        {
                            if (currentKind() == OverlayKind::Comment &&
                                contents->toPlainText().trimmed().isEmpty())
                                fail("コメントを入力してください。");
                            if (requestPlacement)
                                requestPlacement(currentKind() != OverlayKind::Comment);
                        }
                    });
            });
    auto update = new QPushButton("選択した注釈を更新");
    update->setObjectName("updateAnnotation");
    update->setEnabled(false);
    layout->addWidget(update);
    connect(update, &QPushButton::clicked, this,
            [this]
            {
                guard(
                    [&]
                    {
                        const auto mark = selected();
                        const double unit =
                            document->pdf().getCatalog()->getPage(canvas->page)->getUserUnit();
                        auto rectangle = mark.rect;
                        if (mark.kind != OverlayKind::Highlight &&
                            mark.kind != OverlayKind::Comment)
                            rectangle.setSize({width->value() / unit, height->value() / unit});
                        auto geometry = mark.geometry;
                        if ((mark.kind == OverlayKind::Line || mark.kind == OverlayKind::Arrow) &&
                            mark.rect.isValid())
                            for (auto& point : geometry)
                                point = rectangle.topLeft() +
                                        QPointF((point.x() - mark.rect.left()) * rectangle.width() /
                                                    mark.rect.width(),
                                                (point.y() - mark.rect.top()) * rectangle.height() /
                                                    mark.rect.height());
                        auto updated = putAnnotation(*document, canvas->page, mark.kind, rectangle,
                                                     contents->toPlainText(), color,
                                                     lineWidth->value(), geometry, mark.ref);
                        if (changed)
                            changed(updated.ref);
                    });
            });
    auto remove = new QPushButton("選択した注釈を削除");
    remove->setObjectName("removeAnnotation");
    remove->setEnabled(false);
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
    layout->addWidget(new QLabel("このページの追加済み注釈"));
    marks = new QListWidget;
    marks->setObjectName("ownedAnnotationList");
    marks->setMaximumHeight(130);
    layout->addWidget(marks);
    connect(marks, &QListWidget::currentRowChanged, this,
            [this](int row)
            {
                if (row < 0 || !document->loaded())
                    return;
                const auto items = signatures(document->pdf(), canvas->page);
                const int index = marks->item(row)->data(Qt::UserRole).toInt();
                if (index < 0 || index >= items.size())
                    return;
                canvas->refresh(items[index].ref);
                setSelection(items[index]);
            });
    auto hint = new QLabel(
        "ハイライトは本文の文字選択から作ります。矩形などを重ねても墨消しにはなりません。");
    hint->setWordWrap(true);
    layout->addWidget(hint);
    layout->addStretch();
}
void AnnotationPanel::guard(const std::function<void()>& operation)
{
    try
    {
        canvas->finishFormEdit();
        operation();
    }
    catch (const std::exception& error)
    {
        QMessageBox::warning(this, "注釈を操作できません", QString::fromUtf8(error.what()));
    }
}
OverlayKind AnnotationPanel::currentKind() const
{
    return OverlayKind(int(OverlayKind::Comment) + kind->currentIndex());
}
Signature AnnotationPanel::selected() const
{
    document->editable();
    auto items = signatures(document->pdf(), canvas->page);
    if (canvas->selected < 0 || canvas->selected >= items.size() ||
        !isAnnotation(items[canvas->selected].kind))
        fail("注釈の枠か一覧から対象を選択してください。");
    return items[canvas->selected];
}
void AnnotationPanel::setSelection(const Signature& mark)
{
    kind->setCurrentIndex(int(mark.kind) - int(OverlayKind::Comment));
    contents->setPlainText(mark.text);
    lineWidth->setValue(mark.size);
    color = mark.color;
    auto unit = document->pdf().getCatalog()->getPage(canvas->page)->getUserUnit();
    width->setValue(mark.rect.width() * unit);
    height->setValue(mark.rect.height() * unit);
    width->setEnabled(mark.kind != OverlayKind::Highlight && mark.kind != OverlayKind::Comment);
    height->setEnabled(width->isEnabled());
}
void AnnotationPanel::refresh()
{
    if (listRevision == document->revision && listPage == canvas->page &&
        listSelection == canvas->selected)
        return;
    listRevision = document->revision;
    listPage = canvas->page;
    listSelection = canvas->selected;
    QSignalBlocker block(marks);
    marks->clear();
    auto update = findChild<QPushButton*>("updateAnnotation");
    auto remove = findChild<QPushButton*>("removeAnnotation");
    update->setEnabled(false);
    remove->setEnabled(false);
    if (!document->loaded() || canvas->page < 0 || canvas->page >= document->pages())
        return;
    const auto items = signatures(document->pdf(), canvas->page);
    for (int i = 0; i < items.size(); ++i)
        if (isAnnotation(items[i].kind))
        {
            marks->addItem(name(items[i].kind) +
                           (items[i].text.isEmpty() ? "" : " — " + items[i].text.left(24)));
            marks->item(marks->count() - 1)->setData(Qt::UserRole, i);
            if (canvas->selected == i)
            {
                marks->setCurrentRow(marks->count() - 1);
                update->setEnabled(true);
                remove->setEnabled(true);
            }
        }
}
PDFObjectReference AnnotationPanel::place(QPointF point)
{
    const double unit = document->pdf().getCatalog()->getPage(canvas->page)->getUserUnit();
    return putAnnotation(*document, canvas->page, OverlayKind::Comment,
                         QRectF(point, QSizeF(24 / unit, 24 / unit)), contents->toPlainText(),
                         color, lineWidth->value())
        .ref;
}
PDFObjectReference AnnotationPanel::draw(QPointF start, QPointF finish)
{
    const auto type = currentKind();
    auto rectangle = QRectF(start, finish).normalized();
    QPolygonF points;
    if (type == OverlayKind::Line || type == OverlayKind::Arrow)
    {
        points << start << finish;
        const double margin = (type == OverlayKind::Arrow ? 8 : 2) * lineWidth->value() /
                              document->pdf().getCatalog()->getPage(canvas->page)->getUserUnit();
        rectangle.adjust(-margin, -margin, margin, margin);
    }
    return putAnnotation(*document, canvas->page, type, rectangle, contents->toPlainText(), color,
                         lineWidth->value(), points)
        .ref;
}
} // namespace tatsu
