#pragma once
#include "form_data.h"
#include <QtWidgets>
#include <atomic>

namespace tatsu
{
class FormDataDialog : public QDialog
{
public:
    FormDataDialog(PDFDocument document, bool importing, QString suggestedPath,
                   QWidget* parent = nullptr);
    ~FormDataDialog() override;
    FormDataImport candidate() const;
    QString savedPath() const;
    void accept() override;
    void reject() override;

protected:
    void closeEvent(QCloseEvent*) override;

private:
    PDFDocument snapshot;
    bool importing;
    QWidget* settings;
    QLineEdit* path;
    QTableWidget* table;
    QLabel* message;
    QPushButton *load, *apply, *cancel;
    QThread* job = nullptr;
    std::atomic_bool cancelled = false;
    FormDataImport result;
    QByteArray loadedHash;
    QString completedPath;
    void start();
    void showExportValues();
};
} // namespace tatsu
