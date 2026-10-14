#pragma once
#include "page_region_preview.h"
#include "redaction_copy.h"
#include <atomic>

namespace tatsu
{
class RedactionDialog : public QDialog
{
public:
    RedactionDialog(PDFDocument document, int currentPage,
                    QVector<QPair<QString, QByteArray>> originals, QString suggestedPath,
                    QWidget* parent = nullptr);
    ~RedactionDialog() override;
    void accept() override;
    void reject() override;
    QString savedPath() const
    {
        return completedPath;
    }
    QByteArray savedHash() const
    {
        return completedHash;
    }

protected:
    void closeEvent(QCloseEvent* event) override;

private:
    struct Region
    {
        int page;
        QRectF rectangle;
    };
    PDFDocument snapshot;
    QVector<QPair<QString, QByteArray>> originals;
    QVector<Region> regions;
    QWidget* settings;
    QComboBox* page;
    QListWidget* list;
    QDoubleSpinBox *x, *y, *width, *height;
    QLineEdit* destination;
    QCheckBox* consent;
    QLabel* message;
    QPushButton *draw, *remove, *save, *cancel;
    QProgressBar* progress;
    PageRegionPreview* preview;
    QThread *renderJob = nullptr, *saveJob = nullptr;
    int selected = -1;
    bool loading = false, closeRequested = false;
    std::atomic_bool cancelled = false;
    QString completedPath;
    QByteArray completedHash;
    void invalidate();
    void rebuild(int selection = -1);
    void updateSelection();
    void add(QRectF rectangle);
    void loadPreview();
    void setBusy(bool busy);
    QMap<int, QVector<QRectF>> rawRegions() const;
};
} // namespace tatsu
