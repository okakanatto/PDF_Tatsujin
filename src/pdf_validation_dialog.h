#pragma once
#include "pdf_validation.h"
#include <QtWidgets>
#include <atomic>
#include <optional>

namespace tatsu
{
class PdfValidationDialog : public QDialog
{
public:
    PdfValidationDialog(PdfValidationInput input, std::function<void()> validate,
                        QWidget* parent = nullptr);
    ~PdfValidationDialog() override;
    void reject() override;
    void closeEvent(QCloseEvent* event) override;

private:
    PdfValidationInput input;
    std::function<void()> validate;
    QLineEdit *java, *jar;
    QComboBox* profile;
    QPlainTextEdit* details;
    QLabel* message;
    QPushButton *start, *close, *copy, *save;
    QList<QPushButton*> engineBrowsers;
    QLineEdit* output;
    QProgressBar* progress;
    QThread* job = nullptr;
    std::atomic_bool cancelled{false};
    bool closing = false;
    std::optional<PdfValidationResult> result;
    void verify();
};
} // namespace tatsu
