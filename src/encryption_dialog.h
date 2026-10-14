#pragma once
#include "encrypted_pdf.h"
#include <QtWidgets>
#include <atomic>

namespace tatsu
{
class EncryptionDialog : public QDialog
{
public:
    EncryptionDialog(PDFDocument document, QString suggestedPath, QWidget* parent = nullptr);
    ~EncryptionDialog() override;
    QString savedPath() const;
    void accept() override;
    void reject() override;

protected:
    void closeEvent(QCloseEvent*) override;

private:
    PDFDocument snapshot;
    QWidget* settings;
    QLineEdit *user, *userConfirm, *owner, *ownerConfirm, *path;
    QCheckBox *print, *copy, *forms, *annotations, *assemble, *modify;
    QLabel* message;
    QPushButton *save, *cancel;
    QThread* job = nullptr;
    std::atomic_bool cancelled = false;
    QString completedPath;
    EncryptionOptions options() const;
};
} // namespace tatsu
