#pragma once
#include "pdfa_validation.h"
#include <QtWidgets>
#include <atomic>
#include <optional>

namespace tatsu
{
class PdfaDialog : public QDialog
{
public:
    PdfaDialog(PdfaInput input, std::function<void()> validate, QWidget* parent = nullptr);
    ~PdfaDialog() override;
    void reject() override;
    void closeEvent(QCloseEvent* event) override;

private:
    PdfaInput input;
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
    std::optional<PdfaValidation> result;
    void verify();
};
} // namespace tatsu
