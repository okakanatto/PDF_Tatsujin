#pragma once
#include <QComboBox>

namespace tatsu
{
class TextFontPicker : public QComboBox
{
public:
    explicit TextFontPicker(QWidget* parent = nullptr);
    QString family() const;
    void setFamily(const QString& family);
};
} // namespace tatsu
