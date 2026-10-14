#pragma once
#include "certificate_signing.h"
#include <QtWidgets>
#include <atomic>

namespace tatsu
{
class CertificateSigningDialog : public QDialog
{
public:
    CertificateSigningDialog(PDFDocument document, QVector<QPair<QString, QByteArray>> originals,
                             QString suggestedPath, QWidget* parent = nullptr);
    ~CertificateSigningDialog() override;
    void accept() override;
    void reject() override;
    QString savedPath() const;
    QByteArray savedHash() const;

protected:
    void closeEvent(QCloseEvent* event) override;

private:
    PDFDocument snapshot;
    QVector<QPair<QString, QByteArray>> originals;
    QWidget* settings;
    QLineEdit *keyPath, *password, *destination;
    QPlainTextEdit *reason, *identity;
    QCheckBox* consent;
    QLabel* message;
    QPushButton *inspect, *save, *cancel;
    QProgressBar* progress;
    QThread* job = nullptr;
    std::atomic_bool cancelled = false;
    bool closeRequested = false;
    QByteArray approvedP12, approvedHash, completedHash;
    QString approvedPath, completedPath;
    void invalidate();
    void inspectKey();
    void setBusy(bool enabled);
};
} // namespace tatsu
