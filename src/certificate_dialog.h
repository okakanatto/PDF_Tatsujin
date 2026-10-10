#pragma once
#include "certificate_verification.h"
#include <QtWidgets>
#include <atomic>
#include <optional>

namespace tatsu
{
class CertificateDialog : public QDialog
{
public:
    CertificateDialog(QString path, QByteArray hash, QWidget* parent = nullptr);
    ~CertificateDialog() override;
    void reject() override;
    const CertificateVerification* verification() const;

protected:
    void closeEvent(QCloseEvent* event) override;

private:
    QString path;
    QByteArray hash;
    QTableWidget* table;
    QPlainTextEdit* details;
    QLabel* message;
    QProgressBar* progress;
    QPushButton *start, *close;
    QThread* job = nullptr;
    std::atomic_bool cancelled = false;
    bool closeRequested = false;
    std::optional<CertificateVerification> result;
    void verify();
    void showSelected();
};
} // namespace tatsu
