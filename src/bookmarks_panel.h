#pragma once
#include "navigation.h"
#include <QtWidgets>
#include <functional>

namespace tatsu
{
class BookmarksPanel final : public QWidget
{
public:
    explicit BookmarksPanel(QWidget* parent = nullptr);
    void reset();
    void setDocument(const PDFDocument* document, quint64 revision, const QStringList& labels);
    std::function<void(const NavigationTarget&)> activated;

private:
    QTreeWidget* tree;
    QLabel* notice;
    QMap<QTreeWidgetItem*, NavigationTarget> targets;
    quint64 version = std::numeric_limits<quint64>::max();
    void activate(QTreeWidgetItem* item);
};
} // namespace tatsu
