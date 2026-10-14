#pragma once
#include "canvas.h"
#include "signature_library.h"
#include "text_font_picker.h"

namespace tatsu
{
class WritingPanel : public QWidget
{
public:
    WritingPanel(Document* document, Canvas* canvas, bool imageSignature,
                 QWidget* parent = nullptr);
    std::function<void(PDFObjectReference)> changed;
    std::function<void()> requestPlacement;
    std::function<void(SignatureTemplate)> saveTemplate;
    void setImage(const QString& path);
    void setSelection(const Signature& item);
    void setTemplate(const SignatureTemplate& item);
    PDFObjectReference place(QPointF point);

private:
    Document* document;
    Canvas* canvas;
    bool imageSignature;
    QComboBox* kind;
    TextFontPicker* fontPicker;
    QPlainTextEdit* text;
    QDoubleSpinBox *size, *width;
    QLabel *preview, *fontLabel, *sizeLabel, *widthLabel;
    QPushButton *imageButton, *colorButton;
    QColor ink = Qt::black;
    QImage image;
    bool replacement = false;
    OverlayKind currentKind() const;
    Signature selected() const;
    SignatureTemplate currentTemplate() const;
    PDFObjectReference put(QPointF point, PDFObjectReference old = {});
    void syncKind(bool initializeDate);
    void guard(const std::function<void()>& operation);
};
} // namespace tatsu
