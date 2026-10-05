#include "window.h"
#include "page_previews.h"
#include "pdfsecurityhandler.h"
#include <QtPrintSupport>

namespace tatsu
{
Window::Window()
{
    setWindowTitle("PDF達人 — M1 試作");
    resize(1280, 850);
    setMinimumSize(1024, 720);
    setAcceptDrops(true);
    canvas = new Canvas(&doc, this);
    documentArea = new QStackedWidget;
    auto welcome = new QWidget;
    welcome->setObjectName("welcome");
    auto welcomeLayout = new QVBoxLayout(welcome);
    welcomeLayout->setAlignment(Qt::AlignCenter);
    auto welcomeTitle = new QLabel("PDFを、次の作業へ。");
    welcomeTitle->setObjectName("welcomeTitle");
    auto welcomeText =
        new QLabel("日本語の署名を加える。\nスキャンを、検索・コピーできる文書にする。");
    welcomeText->setObjectName("welcomeText");
    welcomeText->setAlignment(Qt::AlignCenter);
    auto openButton = new QPushButton("PDFを開く");
    openButton->setProperty("primary", true);
    openButton->setMinimumHeight(44);
    openButton->setMaximumWidth(240);
    auto dropHint = new QLabel("PDFをこのウィンドウへドラッグしても開けます");
    dropHint->setObjectName("dropHint");
    welcomeLayout->addWidget(welcomeTitle, 0, Qt::AlignHCenter);
    welcomeLayout->addWidget(welcomeText, 0, Qt::AlignHCenter);
    welcomeLayout->addSpacing(20);
    welcomeLayout->addWidget(openButton, 0, Qt::AlignHCenter);
    welcomeLayout->addSpacing(12);
    welcomeLayout->addWidget(dropHint, 0, Qt::AlignHCenter);
    documentArea->addWidget(welcome);
    documentArea->addWidget(canvas);
    setCentralWidget(documentArea);
    auto top = documentToolbar = addToolBar("文書操作");
    top->setObjectName("documentToolbar");
    top->setMovable(false);
    top->setIconSize(QSize(18, 18));
    top->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    auto action = [&](QString label, QKeySequence key, std::function<void()> f, bool edit = false)
    {
        auto a = top->addAction(label);
        // Window ownership of the shortcut context keeps document commands usable
        // while the toolbar is hidden for reading. Input widgets still get override.
        addAction(a);
        a->setShortcut(key);
        connect(a, &QAction::triggered, this, [this, f] { guard(f); });
        if (edit)
            edits << a;
        return a;
    };
    auto openAction =
        action("PDFを開く", QKeySequence::Open,
               [this]
               {
                   auto path = QFileDialog::getOpenFileName(this, "PDFを開く", {}, "PDF (*.pdf)");
                   if (!path.isEmpty())
                   {
                       if (doc.loaded())
                       {
                           auto w = new Window;
                           w->setAttribute(Qt::WA_DeleteOnClose);
                           w->show();
                           w->openFile(path);
                       }
                       else
                           openFile(path);
                   }
               });
    openAction->setIcon(style()->standardIcon(QStyle::SP_DialogOpenButton));
    connect(openButton, &QPushButton::clicked, openAction, &QAction::trigger);
    action(
        "保存", QKeySequence::Save, [this] { saveFile(false); }, true)
        ->setIcon(style()->standardIcon(QStyle::SP_DialogSaveButton));
    action("別名保存", QKeySequence::SaveAs, [this] { saveFile(true); }, true);
    top->addSeparator();
    printAction = action("印刷", QKeySequence::Print,
                         [this]
                         {
                             if (!doc.loaded())
                                 fail("PDFを開いてください。");
                             if (!doc.pdf().getStorage().getSecurityHandler()->isAllowed(
                                     PDFSecurityHandler::Permission::PrintHighResolution) &&
                                 !doc.pdf().getStorage().getSecurityHandler()->isAllowed(
                                     PDFSecurityHandler::Permission::PrintLowResolution))
                                 fail("この文書では印刷が許可されていません。");
                             QPrinter printer(QPrinter::HighResolution);
                             QPrintDialog dialog(&printer, this);
                             dialog.setMinMax(1, doc.pages());
                             dialog.setOption(QAbstractPrintDialog::PrintPageRange, true);
                             dialog.setOption(QAbstractPrintDialog::PrintCurrentPage, true);
                             dialog.setOption(QAbstractPrintDialog::PrintSelection, false);
                             if (dialog.exec() != QDialog::Accepted)
                                 return;
                             printDocument(doc.pdf(), printer, canvas->page);
                         });
    undoAction = action(
        "元に戻す", QKeySequence::Undo,
        [this]
        {
            doc.undo();
            refresh();
        },
        true);
    redoAction = action(
        "やり直す", QKeySequence::Redo,
        [this]
        {
            doc.redo();
            refresh();
        },
        true);
    redoAction->setShortcuts({QKeySequence::Redo, QKeySequence("Ctrl+Shift+Z")});
    top->addSeparator();
    signatureAction = action(
        "署名", {},
        [this]
        {
            showPanel(0);
            signature->setFocus();
        },
        true);
    action(
        "右へ90°回転", {},
        [this]
        {
            doc.rotate(canvas->page);
            refresh();
        },
        true);
    ocrAction = action("OCR", {}, [this] { showPanel(1); }, true);
    signatureAction->setCheckable(true);
    ocrAction->setCheckable(true);
    auto left = navigation = new QDockWidget("文書ナビゲーション", this);
    left->setFeatures(QDockWidget::NoDockWidgetFeatures);
    left->setObjectName("navigationDock");
    auto nav = new QWidget;
    auto nl = new QVBoxLayout(nav);
    auto historyRow = new QHBoxLayout;
    backView = new QAction("前の表示", this);
    backView->setObjectName("previousView");
    backView->setToolTip("前の表示へ戻る（Alt+←）。文書の編集は変わりません。");
    forwardView = new QAction("次の表示", this);
    forwardView->setObjectName("nextView");
    forwardView->setToolTip("次の表示へ進む（Alt+→）");
    backView->setIcon(style()->standardIcon(QStyle::SP_ArrowBack));
    forwardView->setIcon(style()->standardIcon(QStyle::SP_ArrowForward));
    for (auto a : {backView, forwardView})
    {
        auto button = new QToolButton;
        button->setDefaultAction(a);
        button->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
        button->setIcon(
            style()->standardIcon(a == backView ? QStyle::SP_ArrowBack : QStyle::SP_ArrowForward));
        historyRow->addWidget(button);
    }
    nl->addLayout(historyRow);
    auto toolRow = new QHBoxLayout;
    auto toolGroup = new QActionGroup(this);
    selectToolAction = new QAction("選択", toolGroup);
    selectToolAction->setObjectName("selectReadingTool");
    selectToolAction->setToolTip(
        "ドラッグで文字選択、ダブルクリックで単語選択、Shift+クリックで範囲を拡張。"
        "Ctrl+Cでコピー。署名の枠をドラッグして編集。");
    handToolAction = new QAction("手のひら", toolGroup);
    handToolAction->setObjectName("handReadingTool");
    handToolAction->setToolTip("PDFをつかんで表示を移動。本文ではSpace押下中だけ一時切替。");
    for (auto a : {selectToolAction, handToolAction})
    {
        a->setCheckable(true);
        auto button = new QToolButton;
        button->setDefaultAction(a);
        button->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
        button->setAccessibleName(a->text() + "ツール");
        toolRow->addWidget(button);
        connect(a, &QAction::triggered, this,
                [this, a]
                {
                    canvas->setHandTool(a == handToolAction);
                    canvas->setFocus();
                });
    }
    selectToolAction->setChecked(true);
    nl->addLayout(toolRow);
    connect(backView, &QAction::triggered, this, [this] { moveHistory(false); });
    connect(forwardView, &QAction::triggered, this, [this] { moveHistory(true); });
    backShortcut = new QShortcut(QKeySequence("Alt+Left"), this);
    forwardShortcut = new QShortcut(QKeySequence("Alt+Right"), this);
    connect(backShortcut, &QShortcut::activated, backView, &QAction::trigger);
    connect(forwardShortcut, &QShortcut::activated, forwardView, &QAction::trigger);
    connect(qApp, &QApplication::focusChanged, this, [this] { updateHistoryActions(); });
    navigationTabs = new QTabWidget;
    navigationTabs->setObjectName("navigationTabs");
    navigationTabs->setStyleSheet(
        "QTabWidget::pane { border: 0; }"
        "QTabBar::tab { background: transparent; color: #596779; padding: 9px 10px; border-bottom: "
        "2px solid transparent; }"
        "QTabBar::tab:selected { color: #154a88; border-bottom-color: #377ccf; }");
    pages = new PagePreviews;
    pages->setObjectName("pageList");
    pages->setSpacing(6);
    pages->setUniformItemSizes(true);
    navigationTabs->addTab(pages, "ページ");
    bookmarksPanel = new BookmarksPanel;
    navigationTabs->addTab(bookmarksPanel, "しおり");
    bookmarksPanel->activated = [this](const NavigationTarget& target) { navigateTarget(target); };
    canvas->navigate = bookmarksPanel->activated;
    searchPanel = new SearchPanel;
    navigationTabs->addTab(searchPanel, "検索");
    query = searchPanel->editor();
    nl->addWidget(navigationTabs);
    left->setWidget(nav);
    left->setMinimumWidth(210);
    left->setMaximumWidth(270);
    addDockWidget(Qt::LeftDockWidgetArea, left);
    auto findShortcut = new QShortcut(QKeySequence::Find, this);
    connect(findShortcut, &QShortcut::activated, this,
            [this]
            {
                if (doc.loaded())
                {
                    setReadingMode(false);
                    navigationTabs->setCurrentWidget(searchPanel);
                    syncReadingLayout();
                    query->setFocus();
                    query->selectAll();
                }
            });
    for (int direction : {-1, 1})
    {
        auto nextShortcut = new QShortcut(QKeySequence(direction > 0 ? "F3" : "Shift+F3"), this);
        connect(nextShortcut, &QShortcut::activated, this,
                [this, direction] { searchPanel->next(direction); });
    }
    connect(query, &QLineEdit::textChanged, this,
            [this](const QString& text)
            {
                if (!text.isEmpty())
                    navigationTabs->setCurrentWidget(searchPanel);
            });
    connect(searchPanel, &SearchPanel::returnToDocument, this, [this] { canvas->setFocus(); });
    connect(navigationTabs, &QTabWidget::currentChanged, this,
            [this]
            {
                if (pageControl)
                    syncReadingLayout();
            });
    connect(searchPanel, &SearchPanel::presentationChanged, this,
            [this] {
                canvas->setSearchResults(searchPanel->session()->matches(),
                                         searchPanel->activeMatch());
            });
    connect(searchPanel, &SearchPanel::matchActivated, this,
            [this](const SearchMatch& match)
            {
                initialPagePending = false;
                ++layoutGeneration;
                rememberView();
                canvas->showSearchMatch(match);
            });
    canvas->navigatePage = [this](int row)
    {
        if (row < 0 || row >= doc.pages())
            return;
        initialPagePending = false;
        ++layoutGeneration;
        const auto before = canvas->viewState();
        canvas->goToPage(row);
        const auto after = canvas->viewState();
        if (before.anchor.page != after.anchor.page ||
            QLineF(before.anchor.point, after.anchor.point).length() > .5 ||
            qAbs(before.zoom - after.zoom) > .001 || before.fitMode != after.fitMode)
            rememberView(before);
    };
    connect(pages, &QListWidget::currentRowChanged, this,
            [this](int row) { guard([&] { canvas->navigatePage(row); }); });
    properties = new QDockWidget("設定", this);
    properties->setObjectName("propertiesDock");
    properties->setFeatures(QDockWidget::DockWidgetClosable);
    properties->setMinimumWidth(280);
    properties->setMaximumWidth(380);
    panels = new QStackedWidget;
    properties->setWidget(panels);
    addDockWidget(Qt::RightDockWidgetArea, properties);
    auto sign = new QWidget;
    auto sl = new QVBoxLayout(sign);
    sl->setContentsMargins(18, 16, 18, 18);
    sl->setSpacing(10);
    auto signTitle = new QLabel("署名テキスト");
    signTitle->setProperty("sectionTitle", true);
    sl->addWidget(signTitle);
    auto note =
        new QLabel("氏名などを配置する見た目の署名です。\n証明書による本人性の保証はありません。");
    note->setWordWrap(true);
    sl->addWidget(note);
    auto signatureLabel = new QLabel("氏名・日付（複数行可）");
    sl->addWidget(signatureLabel);
    signature = new QPlainTextEdit;
    // A separate label stays readable while the Windows IME draws preedit text.
    // Qt's empty-document placeholder otherwise overlaps the first composition.
    signatureLabel->setBuddy(signature);
    signature->setAccessibleName("署名テキスト");
    signature->setMaximumHeight(160);
    sl->addWidget(signature);
    sl->addWidget(new QLabel("文字のサイズ"));
    size = new QDoubleSpinBox;
    size->setRange(6, 144);
    size->setValue(20);
    size->setSuffix(" pt");
    size->setButtonSymbols(QAbstractSpinBox::PlusMinus);
    size->setAccessibleName("署名サイズ");
    sl->addWidget(size);
    sl->addWidget(new QLabel("書体：Noto Sans JP"));
    auto color = new QPushButton("文字色を選ぶ");
    sl->addWidget(color);
    connect(color, &QPushButton::clicked, this,
            [this]
            {
                auto c = QColorDialog::getColor(ink, this);
                if (c.isValid())
                    ink = c;
            });
    auto place = new QPushButton("ページをクリックして配置");
    place->setProperty("primary", true);
    sl->addWidget(place);
    connect(place, &QPushButton::clicked, this,
            [this]
            {
                guard(
                    [&]
                    {
                        doc.editable();
                        if (signature->toPlainText().trimmed().isEmpty())
                            fail("署名を入力してください。");
                        canvas->beginPlacement();
                        status->setText("配置位置をクリック。Escで解除。");
                    });
            });
    auto update = new QPushButton("選択した署名を更新");
    sl->addWidget(update);
    connect(update, &QPushButton::clicked, this,
            [this]
            {
                guard(
                    [&]
                    {
                        auto ss = signatures(doc.pdf(), canvas->page);
                        if (canvas->selected < 0 || canvas->selected >= ss.size())
                            fail("署名の枠を選択してください。");
                        auto s = ss[canvas->selected];
                        auto updated =
                            doc.putSignature(canvas->page, signature->toPlainText(),
                                             s.rect.topLeft(), size->value(), ink, s.ref);
                        refresh(false, updated.ref);
                    });
            });
    auto remove = new QPushButton("選択した署名を削除");
    sl->addWidget(remove);
    connect(remove, &QPushButton::clicked, this,
            [this]
            {
                guard(
                    [&]
                    {
                        auto ss = signatures(doc.pdf(), canvas->page);
                        if (canvas->selected >= 0 && canvas->selected < ss.size())
                        {
                            doc.eraseSignature(canvas->page, ss[canvas->selected]);
                            canvas->selected = -1;
                            refresh();
                        }
                    });
            });
    sl->addStretch();
    panels->addWidget(sign);
    auto ocr = new QWidget;
    auto ol = new QVBoxLayout(ocr);
    ol->setContentsMargins(18, 16, 18, 18);
    ol->setSpacing(10);
    auto ocrTitle = new QLabel("文字を認識（OCR）");
    ocrTitle->setProperty("sectionTitle", true);
    ol->addWidget(ocrTitle);
    auto desc =
        new QLabel("スキャンを検索・コピーできるPDFにする\n元の画像・署名・注釈を保持します。");
    desc->setWordWrap(true);
    ol->addWidget(desc);
    ol->addWidget(new QLabel("文書の言語"));
    language = new QComboBox;
    language->addItems({"日本語＋英語", "日本語", "英語"});
    ol->addWidget(language);
    ol->addWidget(new QLabel("対象ページ"));
    scope = new QComboBox;
    scope->addItems({"全ページ", "現在ページ", "指定ページ"});
    ol->addWidget(scope);
    range = new QLineEdit;
    range->setPlaceholderText("物理ページ番号 例: 1,3-5");
    range->setEnabled(false);
    connect(scope, &QComboBox::currentIndexChanged, range,
            [this](int index) { range->setEnabled(index == 2); });
    ol->addWidget(range);
    auto run = new QPushButton("OCRを開始");
    run->setProperty("primary", true);
    ol->addWidget(run);
    connect(run, &QPushButton::clicked, this, [this] { guard([&] { startOcr(); }); });
    ol->addStretch();
    panels->addWidget(ocr);
    properties->hide();
    connect(properties, &QDockWidget::visibilityChanged, this,
            [this](bool visible)
            {
                signatureAction->setChecked(visible && panels->currentIndex() == 0);
                ocrAction->setChecked(visible && panels->currentIndex() == 1);
                if (pageControl)
                    syncReadingLayout();
            });
    canvas->place = [this](QPointF point)
    {
        guard(
            [&]
            {
                auto added = doc.putSignature(canvas->page, signature->toPlainText(), point,
                                              size->value(), ink);
                refresh(false, added.ref);
            });
    };
    canvas->select = [this](int i)
    {
        auto ss = signatures(doc.pdf(), canvas->page);
        if (i >= 0 && i < ss.size())
        {
            signature->setPlainText(ss[i].text);
            size->setValue(ss[i].size);
            ink = ss[i].color;
            showPanel(0);
        }
    };
    canvas->changed = [this] { refresh(); };
    status = new QLabel("PDFを開いてください");
    status->setTextFormat(Qt::PlainText);
    status->setMinimumWidth(0);
    status->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    canvas->interactionCancelled = [this] { refreshStatus(); };
    canvas->escapeReading = [this] { setReadingMode(false); };
    canvas->toolChanged = [this]
    {
        handToolAction->setChecked(canvas->handToolActive());
        selectToolAction->setChecked(!canvas->handToolActive());
        refreshStatus();
    };
    statusBar()->addWidget(status, 1);
    progress = new QLabel;
    statusBar()->addWidget(progress);
    cancel = new QPushButton("OCRを中止");
    statusBar()->addWidget(cancel);
    cancel->hide();
    connect(cancel, &QPushButton::clicked, this, &Window::stopOcr);
    // These controls remain reachable when both docks and the document toolbar are hidden.
    for (auto a : {backView, forwardView})
    {
        auto button = new QToolButton;
        button->setDefaultAction(a);
        button->setIcon(
            style()->standardIcon(a == backView ? QStyle::SP_ArrowBack : QStyle::SP_ArrowForward));
        button->setAccessibleName(a->text());
        statusBar()->addPermanentWidget(button);
    }
    pageControl = new PageControl;
    statusBar()->addPermanentWidget(pageControl);
    connect(pageControl, &PageControl::pageRequested, this,
            [this](int page) { canvas->navigatePage(page); });
    connect(pageControl, &PageControl::returnToDocument, this, [this] { canvas->setFocus(); });
    connect(pageControl, &PageControl::validationChanged, this, [this] { refreshStatus(); });
    auto pageShortcut = new QShortcut(QKeySequence("Ctrl+L"), this);
    connect(pageShortcut, &QShortcut::activated, pageControl, &PageControl::focusNumber);
    readingAction = new QAction("集中表示", this);
    readingAction->setObjectName("readingMode");
    readingAction->setCheckable(true);
    readingAction->setShortcut(QKeySequence("F8"));
    readingAction->setShortcutContext(Qt::WidgetWithChildrenShortcut);
    readingAction->setToolTip(
        "パネルを畳んで読む（本文でF8）。Escで解除。ページ・倍率・表示履歴は引き続き使えます。");
    canvas->addAction(readingAction);
    connect(readingAction, &QAction::triggered, this,
            [this](bool enabled) { setReadingMode(enabled); });
    auto readingButton = new QToolButton;
    readingButton->setDefaultAction(readingAction);
    readingButton->setToolButtonStyle(Qt::ToolButtonTextOnly);
    statusBar()->addPermanentWidget(readingButton);
    auto zoom = zoomControl = new QComboBox;
    zoom->setEditable(true);
    zoom->lineEdit()->setReadOnly(true);
    zoom->addItems(
        {"50%", "75%", "100%", "125%", "150%", "200%", "幅に合わせる", "全体に合わせる"});
    zoom->setCurrentIndex(2);
    statusBar()->addPermanentWidget(zoom);
    connect(zoom, &QComboBox::activated, this,
            [this](int i)
            {
                if (i >= 0 && i < 6)
                    canvas->setZoom(QList<double>{.5, .75, 1, 1.25, 1.5, 2}[i]);
                else if (doc.loaded() && i == 6)
                    canvas->fitWidth();
                else if (doc.loaded() && i == 7)
                    canvas->fitPage();
            });
    canvas->viewChanged = [this]
    {
        statusBar()->clearMessage();
        QSignalBlocker pageBlock(pages), zoomBlock(zoomControl);
        pages->setCurrentRow(canvas->page);
        if (canvas->fitMode())
            zoomControl->setCurrentIndex(canvas->fitMode() == 1 ? 6 : 7);
        else
            zoomControl->setEditText(QString("%1%").arg(qRound(canvas->zoom * 100)));
        searchPanel->setCurrentPage(canvas->page);
        refreshStatus();
    };
    setStyleSheet(R"(
        QMainWindow { background: #f4f6f9; color: #202b3c; }
        QToolBar { spacing: 5px; padding: 9px; background: white; border-bottom: 1px solid #d9e0e9; }
        QToolButton { padding: 7px 9px; border: 1px solid transparent; border-radius: 5px; }
        QToolButton:hover { background: #edf3fb; }
        QToolButton:checked { background: #e1ecfc; color: #155bb5; border-color: #b6cff0; }
        QPushButton { padding: 9px 12px; border: 1px solid #cbd5e1; border-radius: 5px; background: white; }
        QPushButton:hover { background: #f0f5fc; border-color: #a1b4ce; }
        QPushButton[primary="true"] { background: #185fc3; color: white; border-color: #185fc3; font-weight: 600; }
        QPushButton[primary="true"]:hover { background: #124da1; }
        QPushButton:disabled, QToolButton:disabled { color: #98a2b2; }
        QPushButton:focus, QToolButton:focus { border: 2px solid #6496db; }
        QDockWidget { font-weight: 600; }
        QDockWidget::title { padding: 10px; background: #f4f6f9; }
        QDockWidget > QWidget { background: white; }
        QLineEdit, QPlainTextEdit, QComboBox, QDoubleSpinBox { padding: 7px; background: white; border: 1px solid #cbd5e1; border-radius: 4px; selection-background-color: #d7e6fa; selection-color: #202b3c; }
        QLineEdit:focus, QPlainTextEdit:focus, QComboBox:focus, QDoubleSpinBox:focus { border-color: #377ccf; }
        QLineEdit[invalidPage="true"] { border-color: #b83a32; }
        QListWidget#pageList { background: #f4f6f9; border: 0; outline: 0; }
        QListWidget#pageList::item { padding: 10px 6px; border: 1px solid transparent; border-radius: 5px; }
        QListWidget#pageList::item:selected { background: #e2ecfa; color: #154a88; border-color: #9dbce6; }
        QLabel[sectionTitle="true"] { font-size: 17px; font-weight: 600; padding-bottom: 4px; }
        QStatusBar { padding: 4px; background: #f4f6f9; border-top: 1px solid #d9e0e9; }
        QWidget#welcome { background: #f4f6f9; }
        QLabel#welcomeTitle { font-size: 28px; font-weight: 600; color: #26364d; }
        QLabel#welcomeText { font-size: 15px; color: #56657a; margin-top: 14px; }
        QLabel#dropHint { color: #68778c; font-size: 12px; }
    )");
    refresh();
}
void Window::showPanel(int index)
{
    const auto anchor = canvas->anchor();
    setReadingMode(false);
    canvas->cancelInteraction();
    panels->setCurrentIndex(index);
    properties->setWindowTitle(index == 0 ? "署名" : "OCR");
    properties->show();
    signatureAction->setChecked(index == 0);
    ocrAction->setChecked(index == 1);
    syncReadingLayout();
    preserveLayoutAnchor(anchor);
}
void Window::preserveLayoutAnchor(const ViewAnchor& anchor)
{
    const auto generation = ++layoutGeneration;
    const auto revision = doc.revision;
    // Dock visibility and resize events can be delivered while Qt is laying out
    // the main window. Restore after that pass; never re-enter its layout here.
    QTimer::singleShot(0, canvas,
                       [this, generation, revision, anchor]
                       {
                           if (generation == layoutGeneration && revision == doc.revision)
                           {
                               if (initialPagePending)
                               {
                                   initialPagePending = false;
                                   canvas->goToPage(0);
                               }
                               else
                                   canvas->restoreAnchor(anchor);
                           }
                       });
}
void Window::syncReadingLayout()
{
    const bool narrowProperties = !properties->isHidden() && width() < 1200;
    const bool searching = navigationTabs->currentWidget() == searchPanel;
    const bool visible = doc.loaded() && !readingMode && (!narrowProperties || searching);
    if (!navigation->isHidden() != visible)
    {
        const auto anchor = canvas->anchor();
        navigation->setVisible(visible);
        preserveLayoutAnchor(anchor);
    }
}
void Window::setReadingMode(bool enabled)
{
    enabled = enabled && doc.loaded();
    if (enabled == readingMode)
        return;
    const auto anchor = canvas->anchor();
    canvas->cancelInteraction();
    if (enabled)
        restoreProperties = !properties->isHidden();
    readingMode = enabled;
    readingAction->setChecked(enabled);
    readingAction->setText(enabled ? "集中解除" : "集中表示");
    documentToolbar->setVisible(!enabled);
    properties->setVisible(!enabled && restoreProperties);
    syncReadingLayout();
    preserveLayoutAnchor(anchor);
    canvas->setFocus();
}
void Window::resizeEvent(QResizeEvent* event)
{
    QMainWindow::resizeEvent(event);
    if (pageControl)
        QTimer::singleShot(0, this, [this] { syncReadingLayout(); });
}
void Window::showEvent(QShowEvent* event)
{
    QMainWindow::showEvent(event);
    if (!initialPlacement)
        return;
    initialPlacement = false;
    // Account for the native title bar and taskbar without changing OS settings.
    // Offscreen test surfaces have no desktop placement contract.
    if (QGuiApplication::platformName() == "offscreen" || !screen())
        return;
    const auto available = screen()->availableGeometry();
    const auto extra = frameGeometry().size() - QWidget::size();
    resize(qMin(width(), available.width() - extra.width()),
           qMin(height(), available.height() - extra.height()));
    const auto frame = frameGeometry();
    const auto left =
        qMax(available.left(), qMin(frame.left(), available.right() - frame.width() + 1));
    const auto top =
        qMax(available.top(), qMin(frame.top(), available.bottom() - frame.height() + 1));
    move(pos() + QPoint(left - frame.left(), top - frame.top()));
}
Window::~Window()
{
    canvas->viewChanged = {};
    canvas->toolChanged = {};
    canvas->interactionCancelled = {};
    canvas->escapeReading = {};
    canvas->navigate = {};
    disconnect(pageControl, nullptr, this, nullptr);
    disconnect(qApp, nullptr, this, nullptr);
    bookmarksPanel->activated = {};
    delete searchPanel;
    searchPanel = nullptr;
    if (worker)
    {
        worker->kill();
        worker->waitForFinished(3000);
    }
}
void Window::guard(const std::function<void()>& f)
{
    try
    {
        f();
    }
    catch (const std::exception& e)
    {
        QMessageBox::warning(this, "操作を完了できません", QString::fromUtf8(e.what()));
    }
}
void Window::openFile(const QString& path)
{
    try
    {
        doc.open(path);
    }
    catch (const PdfPasswordRequired&)
    {
        bool ok = false;
        QString pwd =
            QInputDialog::getText(this, "PDFを開く", "このPDFのパスワードを入力してください。",
                                  QLineEdit::Password, {}, &ok);
        if (!ok)
            return;
        doc.open(path, pwd);
    }
    ++layoutGeneration;
    initialPagePending = true;
    setReadingMode(false);
    canvas->resetView();
    bookmarksPanel->reset();
    canvas->selected = -1;
    query->clear();
    backHistory.clear();
    forwardHistory.clear();
    updateHistoryActions();
    refresh(true);
    // Finish the first layout before choosing the initial reading position.
    const auto generation = ++layoutGeneration;
    QTimer::singleShot(0, canvas,
                       [this, generation]
                       {
                           if (generation == layoutGeneration && initialPagePending)
                           {
                               initialPagePending = false;
                               canvas->goToPage(0);
                           }
                       });
}
void Window::refresh(bool rebuild, PDFObjectReference selection)
{
    documentArea->setCurrentIndex(doc.loaded() ? 1 : 0);
    syncReadingLayout();
    readingAction->setEnabled(doc.loaded());
    zoomControl->setEnabled(doc.loaded());
    printAction->setEnabled(doc.loaded());
    selectToolAction->setEnabled(doc.loaded());
    handToolAction->setEnabled(doc.loaded());
    if (navigationRevision != doc.revision)
    {
        navigationRevision = doc.revision;
        pageLabels = doc.loaded() ? readPageLabels(doc.pdf()) : QStringList();
        bookmarksPanel->setDocument(doc.loaded() ? &doc.pdf() : nullptr, doc.revision, pageLabels);
        for (int i = 0; i < pages->count(); ++i)
            pages->item(i)->setText(pageDescription(i, pageLabels));
    }
    if (rebuild)
    {
        QSignalBlocker block(pages);
        pages->clear();
        for (int i = 0; i < doc.pages(); ++i)
            pages->addItem(pageDescription(i, pageLabels));
        pages->setCurrentRow(canvas->page);
    }
    canvas->refresh(selection);
    searchPanel->setDocument(doc.loaded() ? &doc.pdf() : nullptr, doc.revision, canvas->page);
    static_cast<PagePreviews*>(pages)->setDocument(doc.loaded() ? &doc.pdf() : nullptr,
                                                   doc.revision);
    setWindowTitle(
        QString("%1%2 — PDF達人 M1")
            .arg(doc.dirty() ? "● " : "")
            .arg(doc.loaded() ? QFileInfo(doc.target.isEmpty() ? doc.source : doc.target).fileName()
                              : "PDFを開く"));
    bool can = doc.loaded() && doc.readOnly.isEmpty() && !doc.busy;
    for (auto a : edits)
        a->setEnabled(can);
    undoAction->setEnabled(can && doc.cursor > 0);
    redoAction->setEnabled(can && doc.cursor + 1 < int(doc.history.size()));
    panels->setEnabled(can);
    refreshStatus();
}
void Window::rememberView()
{
    rememberView(canvas->viewState());
}
void Window::rememberView(const ViewState& state)
{
    if (state.anchor.page < 0)
        return;
    if (backHistory.isEmpty() || backHistory.back().view.anchor.page != state.anchor.page ||
        QLineF(backHistory.back().view.anchor.point, state.anchor.point).length() > .5 ||
        qAbs(backHistory.back().view.zoom - state.zoom) > .001 ||
        backHistory.back().view.fitMode != state.fitMode ||
        backHistory.back().view.activeSearch != state.activeSearch ||
        backHistory.back().query != query->text() || backHistory.back().revision != doc.revision)
        backHistory.append({state, query->text(), doc.revision});
    if (backHistory.size() > 200)
        backHistory.removeFirst();
    forwardHistory.clear();
    updateHistoryActions();
}
void Window::navigateTarget(const NavigationTarget& target)
{
    const auto resolved =
        doc.loaded() && target.valid() ? resolveDestination(doc.pdf(), target.destination) : target;
    if (!resolved.valid())
    {
        // Plain text keeps PDF-provided titles/URIs from becoming rich-text markup.
        QMessageBox message(QMessageBox::Information, "移動先について", resolved.notice,
                            QMessageBox::Ok, this);
        message.setTextFormat(Qt::PlainText);
        message.exec();
        return;
    }
    const auto before = canvas->viewState();
    initialPagePending = false;
    ++layoutGeneration;
    canvas->goToDestination(resolved);
    const auto after = canvas->viewState();
    if (before.anchor.page != after.anchor.page ||
        QLineF(before.anchor.point, after.anchor.point).length() > .5 ||
        qAbs(before.zoom - after.zoom) > .001 || before.fitMode != after.fitMode)
        rememberView(before);
    canvas->setFocus();
    statusBar()->showMessage(pageDescription(resolved.page, pageLabels) + "へ移動しました", 4000);
}
void Window::moveHistory(bool forward)
{
    auto& source = forward ? forwardHistory : backHistory;
    auto& target = forward ? backHistory : forwardHistory;
    if (!doc.loaded() || source.isEmpty())
        return;
    initialPagePending = false;
    ++layoutGeneration;
    target.append({canvas->viewState(), query->text(), doc.revision});
    auto destination = source.takeLast();
    canvas->restoreView(destination.view);
    searchPanel->restoreActive(destination.query == query->text() &&
                                       destination.revision == doc.revision
                                   ? destination.view.activeSearch
                                   : 0);
    updateHistoryActions();
}
void Window::updateHistoryActions()
{
    backView->setEnabled(doc.loaded() && !backHistory.isEmpty());
    forwardView->setEnabled(doc.loaded() && !forwardHistory.isEmpty());
    QWidget* focus = QApplication::focusWidget();
    const bool editing = qobject_cast<QLineEdit*>(focus) || qobject_cast<QPlainTextEdit*>(focus) ||
                         qobject_cast<QTextEdit*>(focus) || qobject_cast<QAbstractSpinBox*>(focus);
    backShortcut->setEnabled(backView->isEnabled() && !editing);
    forwardShortcut->setEnabled(forwardView->isEnabled() && !editing);
}
void Window::refreshStatus()
{
    if (pageControl)
        pageControl->setPage(canvas->page, doc.pages());
    status->setText(!doc.readOnly.isEmpty() ? doc.readOnly
                    : doc.loaded()          ? QString("%1 / %2 ページ%3")
                                         .arg(canvas->page + 1)
                                         .arg(doc.pages())
                                         .arg(doc.dirty() ? " • 未保存" : "")
                                   : "PDFを開いてください");
    if (doc.loaded() && canvas->page < pageLabels.size() && !pageLabels[canvas->page].isEmpty())
        status->setText(pageLabels[canvas->page] + " · " + status->text());
    if (doc.loaded() && canvas->handToolActive())
        status->setText(status->text() + " · 手のひら：ドラッグで表示を移動 · Escで選択に戻る");
    else if (doc.loaded() && !canvas->selectionMessage().isEmpty())
        status->setText(status->text() + " · " + canvas->selectionMessage());
    if (pageControl && !pageControl->validationMessage().isEmpty())
        status->setText(pageControl->validationMessage());
    status->setToolTip(status->text());
}
bool Window::saveFile(bool choose)
{
    doc.editable();
    QString path = doc.target;
    if (choose || path.isEmpty())
        path = QFileDialog::getSaveFileName(
            this, "PDFとして保存",
            path.isEmpty() ? QFileInfo(doc.source).absolutePath() + "/" +
                                 QFileInfo(doc.source).completeBaseName() + "_編集.pdf"
                           : path,
            "PDF (*.pdf)");
    if (path.isEmpty())
        return false;
    if (sameFilePath(path, doc.source) && !sameFilePath(doc.target, doc.source))
    {
        if (QMessageBox::question(
                this, "原本への上書き",
                "原本を上書きしますか。原本を残す場合は別名で保存してください。") !=
            QMessageBox::Yes)
            return false;
    }
    doc.save(path, fileHash(path));
    refresh();
    return true;
}
void Window::startOcr()
{
    doc.editable();
    QString selection = scope->currentIndex() == 0   ? QString("1-%1").arg(doc.pages())
                        : scope->currentIndex() == 1 ? QString::number(canvas->page + 1)
                                                     : range->text();
    parsePages(selection, doc.pages());
    work = std::make_unique<QTemporaryDir>(QDir::tempPath() + "/pdf-tatsujin-job-XXXXXX");
    if (!work->isValid())
        fail("OCR一時領域を作成できません。");
    QFile owner(work->filePath(".tatsujin-owner"));
    if (!owner.open(QIODevice::WriteOnly))
        fail(owner.errorString());
    owner.write("PDFTatsujin job v1");
    owner.close();
    workLock = std::make_unique<QLockFile>(work->filePath("job.lock"));
    workLock->setStaleLockTime(0);
    if (!workLock->tryLock())
        fail("OCR一時領域を確保できません。");
    writeCandidate(doc.pdf(), work->filePath("input.pdf"));
    QFile f(work->filePath("options.json"));
    if (!f.open(QIODevice::WriteOnly))
        fail(f.errorString());
    f.write(QJsonDocument(QJsonObject{{"language", QStringList{"jpn+eng", "jpn",
                                                               "eng"}[language->currentIndex()]},
                                      {"pages", selection}})
                .toJson());
    f.close();
    startRevision = doc.revision;
    doc.busy = true;
    refresh();
    progress->setText("OCRを開始しています…");
    cancel->show();
    progressBuffer.clear();
    worker = new QProcess(this);
    connect(worker, &QProcess::readyReadStandardOutput, this,
            [this]
            {
                progressBuffer += worker->readAllStandardOutput();
                while (progressBuffer.contains('\n'))
                {
                    auto line = progressBuffer.left(progressBuffer.indexOf('\n'));
                    progressBuffer.remove(0, line.size() + 1);
                    auto p = QJsonDocument::fromJson(line).object();
                    if (!p.isEmpty())
                        progress->setText(QString("OCR %1 / %2 ページ")
                                              .arg(p["done"].toInt())
                                              .arg(p["total"].toInt()));
                }
            });
    connect(worker, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), this,
            &Window::finishOcr);
    connect(worker, &QProcess::errorOccurred, this,
            [this](QProcess::ProcessError e)
            {
                if (e == QProcess::FailedToStart)
                    finishOcr(-1, QProcess::CrashExit);
            });
    worker->start(QCoreApplication::applicationFilePath(),
                  {"--ocr-worker", work->filePath("input.pdf"), work->filePath("result.pdf"),
                   work->filePath("options.json"), work->filePath("report.json")});
    QTimer::singleShot(15 * 60 * 1000, worker,
                       [this]
                       {
                           if (worker && worker->state() != QProcess::NotRunning)
                           {
                               worker->setProperty("timedOut", true);
                               worker->kill();
                           }
                       });
}
void Window::stopOcr()
{
    if (worker)
    {
        worker->setProperty("cancelled", true);
        worker->kill();
        progress->setText("OCRを中止しています…");
    }
}
void Window::finishOcr(int code, QProcess::ExitStatus exitStatus)
{
    if (!worker)
        return;
    bool cancelled = worker->property("cancelled").toBool();
    bool timedOut = worker->property("timedOut").toBool();
    auto err = QString::fromUtf8(worker->readAllStandardError());
    worker->deleteLater();
    worker = nullptr;
    doc.busy = false;
    cancel->hide();
    guard(
        [&]
        {
            if (cancelled)
            {
                progress->setText("OCR中止。開始前の変更を保持しました。");
                return;
            }
            if (code != 0 || exitStatus != QProcess::NormalExit)
                fail(timedOut
                         ? "OCRが時間制限を超えました。開始前の変更を保持しました。"
                         : "OCRに失敗しました。開始前の変更を保持しました。\n" + err.left(500));
            if (doc.revision != startRevision)
                fail("文書の版が変わったためOCRを反映しませんでした。");
            auto result = readPdf(work->filePath("result.pdf"));
            if (int(result.getCatalog()->getPageCount()) != doc.pages())
                fail("OCR結果のページ数が一致しません。");
            QFile rf(work->filePath("report.json"));
            rf.open(QIODevice::ReadOnly);
            auto report = QJsonDocument::fromJson(rf.readAll()).object();
            QStringList summary;
            bool changed = false;
            for (auto value : report["pages"].toArray())
            {
                auto o = value.toObject();
                auto s = o["status"].toString();
                changed |= s == "処理済み";
                summary << QString("%1ページ: %2").arg(o["page"].toInt()).arg(s);
            }
            if (changed)
                doc.commit(std::move(result));
            progress->setText(changed ? "OCR反映済み。元に戻すで取り消せます。"
                                      : "OCR終了。追加された文字はありません。");
            QMessageBox::information(this, "OCR結果", summary.join('\n'));
        });
    workLock.reset();
    work.reset();
    refresh();
}
bool Window::safeToClose()
{
    if (doc.busy)
    {
        if (QMessageBox::question(this, "OCR処理中",
                                  "OCRを中止して閉じますか。未保存変更は続けて確認します。") !=
            QMessageBox::Yes)
            return false;
        stopOcr();
        if (worker)
            worker->waitForFinished(3000);
    }
    if (!doc.dirty())
        return true;
    auto result =
        QMessageBox::warning(this, "未保存の変更", "変更を保存しますか。",
                             QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel);
    if (result == QMessageBox::Cancel)
        return false;
    if (result == QMessageBox::Save)
    {
        bool saved = false;
        guard([&] { saved = saveFile(false); });
        return saved;
    }
    return true;
}
void Window::closeEvent(QCloseEvent* e)
{
    if (safeToClose())
        e->accept();
    else
        e->ignore();
}
void Window::dragEnterEvent(QDragEnterEvent* e)
{
    if (e->mimeData()->hasUrls() && e->mimeData()->urls().size() == 1)
        e->acceptProposedAction();
}
void Window::dropEvent(QDropEvent* e)
{
    auto urls = e->mimeData()->urls();
    if (urls.size() != 1)
        return;
    guard(
        [&]
        {
            if (doc.loaded())
            {
                auto w = new Window;
                w->setAttribute(Qt::WA_DeleteOnClose);
                w->show();
                w->openFile(urls[0].toLocalFile());
            }
            else
                openFile(urls[0].toLocalFile());
        });
}
} // namespace tatsu
