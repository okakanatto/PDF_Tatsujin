#pragma once
#include "bookmark_edit.h"
#include <QtWidgets>
#include <atomic>
#include <optional>

namespace tatsu
{
class BookmarkEditDialog : public QDialog
{
public:
    BookmarkEditDialog(PDFDocument document, int currentPage, QWidget* parent = nullptr);
    ~BookmarkEditDialog() override;
    PDFDocument takeDocument();
    void accept() override;
    void reject() override;

protected:
    void closeEvent(QCloseEvent* event) override;

private:
    PDFDocument snapshot;
    int current;
    QVector<BookmarkEntry> values;
    QWidget* settings;
    QTreeWidget* tree;
    QLineEdit* title;
    QCheckBox* destination;
    QSpinBox* page;
    QLabel* message;
    QPushButton *remove, *apply, *cancel;
    QThread* job = nullptr;
    std::atomic_bool cancelled = false;
    std::optional<PDFDocument> candidate;
    bool loading = false;
    void select();
    void update();
    void add();
    void erase();
    void move(int offset);
    void indent(bool deeper);
    QVector<BookmarkEntry> entries() const;
};
} // namespace tatsu
