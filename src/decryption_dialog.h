#pragma once
#include "unprotected_pdf.h"
#include <QtWidgets>
#include <atomic>

namespace tatsu
{
class DecryptionDialog : public QDialog
{
public:
    DecryptionDialog(PDFDocument document, QString suggestedPath, QWidget* parent = nullptr);
    ~DecryptionDialog() override;
    QString savedPath() const;
    QByteArray savedHash() const;
    void accept() override;
    void reject() override;

protected:
    void closeEvent(QCloseEvent*) override;

private:
    PDFDocument snapshot;
    QWidget* settings;
    QLineEdit *password, *path;
    QCheckBox* consent;
    QLabel* message;
    QPushButton *save, *cancel;
    QThread* job = nullptr;
    std::atomic_bool cancelled = false;
    QString completedPath;
    QByteArray completedHash;
};
} // namespace tatsu
