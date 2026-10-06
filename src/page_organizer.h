#pragma once
#include "page_operations.h"
#include "page_previews.h"
#include <QtWidgets>

namespace tatsu
{
class OrganizerPreviews : public PagePreviews
{
public:
    using PagePreviews::PagePreviews;
    std::function<void(QVector<int>)> reorder;

protected:
    void dropEvent(QDropEvent* event) override;
};
class PageOrganizer : public QWidget
{
public:
    explicit PageOrganizer(Document* document, QWidget* parent = nullptr);
    QWidget* settings() const
    {
        return options;
    }
    void refresh();
    std::function<void(QVector<int>)> changed;
    std::function<void()> returnToDocument;
    OrganizerPreviews* previews;

private:
    Document* document;
    QWidget* options;
    QLabel* summary;
    QLineEdit* ranges;
    QSpinBox* before;
    QComboBox* insertion;
    QVector<int> selected() const;
    void guard(const std::function<void()>& operation);
    void applyOrder(const QVector<int>& order);
    void insert();
    void extract(bool split, bool eachPage = false);
};
} // namespace tatsu
