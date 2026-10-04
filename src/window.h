#pragma once
#include "canvas.h"
#include <QtWidgets>
#include <functional>

namespace tatsu
{
class Window : public QMainWindow
{
public:
    Document doc;
    Canvas* canvas;
    QListWidget* pages;
    QPlainTextEdit* signature;
    QDoubleSpinBox* size;
    QComboBox* language;
    QComboBox* scope;
    QLineEdit* range;
    QDockWidget* properties;
    QStackedWidget* panels;
    QLabel* status;
    QLabel* progress;
    QPushButton* cancel;
    QLineEdit* query;
    QProcess* worker = nullptr;
    std::unique_ptr<QTemporaryDir> work;
    std::unique_ptr<QLockFile> workLock;
    QList<QAction*> edits;
    QAction* undoAction;
    QAction* redoAction;
    QAction* signatureAction;
    QAction* ocrAction;
    quint64 startRevision = 0;
    QByteArray progressBuffer;
    explicit Window();
    ~Window() override;
    void openFile(const QString& path);
    void refresh(bool rebuildPages = false);
    bool saveFile(bool choose);
    void startOcr();
    void stopOcr();
    void guard(const std::function<void()>& call);

protected:
    void closeEvent(QCloseEvent*) override;
    void dragEnterEvent(QDragEnterEvent*) override;
    void dropEvent(QDropEvent*) override;

private:
    QStackedWidget* documentArea;
    QDockWidget* navigation;
    QComboBox* zoomControl;
    QAction* printAction;
    void showPanel(int index);
    bool safeToClose();
    void finishOcr(int code, QProcess::ExitStatus exitStatus);
    QColor ink = Qt::black;
};
} // namespace tatsu
