#pragma once
#include <QtWidgets>

namespace tatsu
{
// Keep full plain text for accessibility and tooltips, even in a narrow status bar.
class ElidedLabel : public QLabel
{
public:
    explicit ElidedLabel(const QString& text = {}, QWidget* parent = nullptr);

protected:
    void paintEvent(QPaintEvent*) override;
    bool event(QEvent*) override;
};

QScrollArea* scrollableSettings(QWidget* content);
} // namespace tatsu
