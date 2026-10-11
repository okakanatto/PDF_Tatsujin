#include "ui_widgets.h"
#include "window.h"
namespace tatsu
{
void Window::setupAnnotations()
{
    annotationPanel = new AnnotationPanel(&doc, canvas);
    panels->addWidget(scrollableSettings(annotationPanel));
    annotationPanel->changed = [this](PDFObjectReference reference) { refresh(false, reference); };
    annotationPanel->requestPlacement = [this](bool region)
    {
        if (region)
        {
            auto kind = annotationPanel->findChild<QComboBox*>("annotationKind");
            canvas->beginDrawing(kind->currentIndex() == 2 || kind->currentIndex() == 3);
            status->setText("始点から終点へドラッグして配置。Escで解除。");
        }
        else
        {
            canvas->beginPlacement();
            status->setText("コメントの位置をクリック。Escで解除。");
        }
    };
}
} // namespace tatsu
