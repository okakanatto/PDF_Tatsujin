#pragma once
#include "batch_processing.h"
#include <QtWidgets>
#include <atomic>
#include <optional>

namespace tatsu
{
class BatchDialog final : public QDialog
{
public:
    explicit BatchDialog(QWidget* parent = nullptr);
    ~BatchDialog() override;
    void setInputs(const QStringList& paths);
    const QVector<BatchItemResult>& results() const;

protected:
    void reject() override;

private:
    QStringList inputs;
    QVector<BatchItemResult> completed;
    QWidget* settings;
    QTableWidget* table;
    QComboBox *operation, *language;
    QLineEdit* directory;
    QPushButton *start, *cancel;
    QLabel* message;
    QProgressBar* progress;
    QThread* job = nullptr;
    std::atomic_bool cancelled{false};
    bool closeRequested = false;
    quint64 generation = 0;
    void rebuild();
    void run();
    void updateRow(int index, const BatchItemResult& result);
    void busy(bool value);
};
} // namespace tatsu
