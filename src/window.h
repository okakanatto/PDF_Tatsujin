#pragma once
#include "bookmarks_panel.h"
#include "canvas.h"
#include "page_control.h"
#include "search_panel.h"
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
    void refresh(bool rebuildPages = false, PDFObjectReference selection = {});
    bool saveFile(bool choose);
    void startOcr();
    void stopOcr();
    void guard(const std::function<void()>& call);

protected:
    void closeEvent(QCloseEvent*) override;
    void dragEnterEvent(QDragEnterEvent*) override;
    void dropEvent(QDropEvent*) override;
    void resizeEvent(QResizeEvent*) override;
    void showEvent(QShowEvent*) override;

private:
    QStackedWidget* documentArea;
    QDockWidget* navigation;
    QComboBox* zoomControl;
    QTabWidget* navigationTabs;
    SearchPanel* searchPanel;
    BookmarksPanel* bookmarksPanel;
    QStringList pageLabels;
    quint64 navigationRevision = std::numeric_limits<quint64>::max();
    QAction* backView;
    QAction* forwardView;
    QShortcut* backShortcut;
    QShortcut* forwardShortcut;
    struct HistoryEntry
    {
        ViewState view;
        QString query;
        quint64 revision;
    };
    QVector<HistoryEntry> backHistory, forwardHistory;
    QAction* printAction;
    QAction* selectToolAction;
    QAction* handToolAction;
    QToolBar* documentToolbar;
    PageControl* pageControl = nullptr;
    QAction* readingAction;
    bool readingMode = false, restoreProperties = false;
    bool initialPlacement = true;
    quint64 layoutGeneration = 0;
    bool initialPagePending = false;
    void setReadingMode(bool enabled);
    void syncReadingLayout();
    void preserveLayoutAnchor(const ViewAnchor& anchor);
    void refreshStatus();
    void rememberView();
    void rememberView(const ViewState& state);
    void navigateTarget(const NavigationTarget& target);
    void moveHistory(bool forward);
    void updateHistoryActions();
    void showPanel(int index);
    bool safeToClose();
    void finishOcr(int code, QProcess::ExitStatus exitStatus);
    QColor ink = Qt::black;
};
} // namespace tatsu
