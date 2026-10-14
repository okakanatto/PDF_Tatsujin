#pragma once
#include "annotation_operations.h"
#include "canvas.h"
namespace tatsu
{
class AnnotationPanel : public QWidget
{
public:
    AnnotationPanel(Document* document, Canvas* canvas, QWidget* parent = nullptr);
    std::function<void(PDFObjectReference)> changed;
    std::function<void(bool)> requestPlacement;
    void refresh();
    void setSelection(const Signature& mark);
    PDFObjectReference place(QPointF point);
    PDFObjectReference draw(QPointF start, QPointF finish);

private:
    Document* document;
    Canvas* canvas;
    QComboBox* kind;
    QPlainTextEdit* contents;
    QDoubleSpinBox *lineWidth, *width, *height;
    QListWidget* marks;
    QColor color = QColor("#d14b28");
    quint64 listRevision = std::numeric_limits<quint64>::max();
    int listPage = -1, listSelection = -2;
    OverlayKind currentKind() const;
    Signature selected() const;
    void guard(const std::function<void()>& operation);
};
} // namespace tatsu
