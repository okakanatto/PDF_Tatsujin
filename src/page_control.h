#pragma once
#include <QtWidgets>

namespace tatsu
{
// Physical page numbers are deliberately independent of PDF PageLabels.
class PageControl : public QWidget
{
    Q_OBJECT
public:
    explicit PageControl(QWidget* parent = nullptr);
    void setPage(int page, int count);
    void focusNumber();
    QString validationMessage() const;
signals:
    void pageRequested(int page);
    void returnToDocument();
    void validationChanged();

protected:
    bool eventFilter(QObject*, QEvent*) override;

private:
    void resetNumber();
    void validateNumber();
    void setError(const QString& message);
    QLineEdit* number;
    QLabel* total;
    int current = 0, pageCount = 0;
    bool composing = false;
    QString error;
};
} // namespace tatsu
