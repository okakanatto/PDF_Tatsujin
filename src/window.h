#pragma once
#include "annotation_panel.h"
#include "bookmarks_panel.h"
#include "canvas.h"
#include "ocr_job.h"
#include "page_control.h"
#include "page_decoration.h"
#include "page_organizer.h"
#include "search_panel.h"
#include "view_history.h"
#include "worker_channels.h"
#include "writing_panel.h"
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
    TextFontPicker* signatureFontPicker;
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
    std::unique_ptr<OcrJob> work;
    std::unique_ptr<WorkerChannels> workerChannels;
    QList<QAction*> edits;
    QAction* undoAction;
    QAction* redoAction;
    QAction* signatureAction;
    QAction* ocrAction;
    QByteArray progressBuffer;
    explicit Window();
    ~Window() override;
    void openFile(const QString& path);
    void createFromImages(QStringList paths = {});
    void exportDocumentImages();
    void editPageDecoration(DecorationKind kind);
    void editBookmarks();
    void editLinks();
    void optimizeDocument();
    void exportEncryptedCopy();
    void createEditableCopy();
    void compareWithDocument();
    void processMultipleDocuments();
    void designForms();
    void manageFormData(bool importing);
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
    QToolButton* referenceControl = nullptr;
    QAction* referenceAction = nullptr;
    QAction* imageExportAction = nullptr;
    QAction* editableCopyAction = nullptr;
    QAction* comparisonAction = nullptr;
    bool navigationRequested = false;
    ViewHistory viewHistory;
    QAction* printAction;
    QAction* formDataExportAction;
    QAction* formDataImportAction;
    quint64 formDataRevision = 0;
    bool formDataChecked = false, formDataAvailable = false;
    QString formDataNotice;
    QAction* selectToolAction;
    QAction* handToolAction;
    QToolBar* documentToolbar;
    QToolBar* workToolbar;
    QAction* writingAction = nullptr;
    WritingPanel *writingPanel, *imageSignaturePanel;
    void setupWriting();
    void saveSignatureTemplate(SignatureTemplate item);
    void openSignatureLibrary();
    PageOrganizer* organizer;
    QAction* organizeAction;
    bool organizing = false;
    ViewState organizerReadingState;
    void setupOrganizer();
    void setOrganizing(bool enabled);
    void mergeFiles(QStringList paths = {});
    AnnotationPanel* annotationPanel;
    QAction* annotationAction;
    void setupAnnotations();
    PageControl* pageControl = nullptr;
    QAction* readingAction;
    bool readingMode = false, restoreProperties = false;
    bool initialPlacement = true;
    quint64 layoutGeneration = 0;
    bool initialPagePending = false;
    void setReadingMode(bool enabled);
    void syncReadingLayout();
    void showNavigation(int index);
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
