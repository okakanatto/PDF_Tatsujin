#pragma once
#include "page_decoration.h"
#include "text_font_picker.h"
#include <QtWidgets>
#include <atomic>
#include <optional>

namespace tatsu
{
class PageDecorationDialog : public QDialog
{
public:
    PageDecorationDialog(PDFDocument document, int currentPage, DecorationKind kind,
                         QWidget* parent = nullptr);
    ~PageDecorationDialog() override;
    PDFDocument takeDocument();
    void accept() override;
    void reject() override;

protected:
    void closeEvent(QCloseEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;

private:
    enum class Task
    {
        Preview,
        Apply,
        Remove
    };
    PDFDocument snapshot;
    DecorationKind kind;
    int current;
    QVector<DecorationGroup> groups;
    QWidget* settings;
    QComboBox *group, *scope, *page;
    QLineEdit *range, *watermark;
    std::array<QLineEdit*, 6> entries{};
    TextFontPicker* font;
    QDoubleSpinBox *size, *left, *top, *right, *bottom, *angle;
    QSpinBox *start, *opacity;
    QPushButton *apply, *remove, *cancel;
    QLabel *message, *preview;
    QImage previewImage;
    QTimer timer;
    QThread* job = nullptr;
    std::shared_ptr<std::atomic_bool> cancelled;
    std::optional<PDFDocument> candidate;
    quint64 generation = 0, ready = 0;
    bool working = false, closing = false;
    QVector<int> selectedPages() const;
    DecorationOptions options() const;
    void selectGroup();
    void changed();
    void launch(Task task);
    void setWorking(bool value);
};
} // namespace tatsu
