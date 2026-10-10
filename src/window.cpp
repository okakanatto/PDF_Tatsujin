#include "window.h"
#include "form_data.h"
#include "ocr_language.h"
#include "page_previews.h"
#include "pdfsecurityhandler.h"
#include "ui_icons.h"
#include "ui_widgets.h"
#include <QtPrintSupport>

namespace tatsu
{
Window::Window()
{
    setWindowTitle("PDF達人 — 評価版");
    resize(1280, 850);
    setMinimumSize(800, 480);
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
    openAction->setIcon(uiIcon(QStyle::SP_DialogOpenButton));
    connect(openButton, &QPushButton::clicked, openAction, &QAction::trigger);
    action(
        "保存", QKeySequence::Save, [this] { saveFile(false); }, true)
        ->setIcon(uiIcon(QStyle::SP_DialogSaveButton));
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
            progress->setText("元に戻しました。");
        },
        true);
    redoAction = action(
        "やり直す", QKeySequence::Redo,
        [this]
        {
            doc.redo();
            refresh();
            progress->setText("やり直しました。");
        },
        true);
    redoAction->setShortcuts({QKeySequence::Redo, QKeySequence("Ctrl+Shift+Z")});
    addToolBarBreak();
    top = workToolbar = addToolBar("作業");
    top->setObjectName("workToolbar");
    top->setMovable(false);
    top->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    writingAction = action("書き込み", {}, [this] { showPanel(2); }, true);
    writingAction->setObjectName("writingAction");
    writingAction->setCheckable(true);
    annotationAction = action("注釈", {}, [this] { showPanel(5); }, true);
    annotationAction->setObjectName("annotationAction");
    annotationAction->setCheckable(true);
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
    organizeAction = action("ページ整理", {}, [this] { setOrganizing(!organizing); }, true);
    organizeAction->setObjectName("organizeAction");
    organizeAction->setCheckable(true);
    auto mergeAction = action("PDFを結合", {}, [this] { mergeFiles(); });
    auto mergeButton = new QPushButton("PDFを結合");
    mergeButton->setMinimumHeight(44);
    mergeButton->setMaximumWidth(240);
    welcomeLayout->insertWidget(welcomeLayout->indexOf(openButton) + 1, mergeButton, 0,
                                Qt::AlignHCenter);
    connect(mergeButton, &QPushButton::clicked, mergeAction, &QAction::trigger);
    auto createAction = action("作成", QKeySequence::New, [this] { createFromImages(); });
    createAction->setObjectName("createPdfAction");
    auto createMenu = new QMenu(this);
    createMenu->addAction("画像からPDF…", this, [this] { guard([&] { createFromImages(); }); });
    auto officeImport = createMenu->addAction("Office文書からPDF…", this,
                                              [this] { guard([&] { createFromOffice(); }); });
    officeImport->setObjectName("importOfficeDocument");
    tableExportAction = createMenu->addAction("PDFの表をExcelへ…", this,
                                              [this] { guard([&] { extractDocumentTable(); }); });
    tableExportAction->setObjectName("extractPdfTable");
    imageExportAction = createMenu->addAction("PDFを画像として出力…", this,
                                              [this] { guard([&] { exportDocumentImages(); }); });
    imageExportAction->setObjectName("exportPdfImages");
    imageExportAction->setShortcut(QKeySequence("Ctrl+Shift+E"));
    addAction(imageExportAction);
    createMenu->addSeparator();
    auto headers = createMenu->addAction(
        "ヘッダー／フッター・ページ番号…", this,
        [this] { guard([&] { editPageDecoration(DecorationKind::HeaderFooter); }); });
    headers->setObjectName("editHeadersFooters");
    auto watermark = createMenu->addAction(
        "透かし…", this, [this] { guard([&] { editPageDecoration(DecorationKind::Watermark); }); });
    watermark->setObjectName("editWatermark");
    edits << headers << watermark;
    auto links =
        createMenu->addAction("リンクを編集…", this, [this] { guard([&] { editLinks(); }); });
    links->setObjectName("editDocumentLinks");
    edits << links;
    auto optimize = createMenu->addAction("PDFの容量を最適化…", this,
                                          [this] { guard([&] { optimizeDocument(); }); });
    optimize->setObjectName("optimizePdfDocument");
    edits << optimize;
    auto encrypt = createMenu->addAction("パスワードで保護したコピー…", this,
                                         [this] { guard([&] { exportEncryptedCopy(); }); });
    encrypt->setObjectName("exportEncryptedCopy");
    edits << encrypt;
    editableCopyAction = createMenu->addAction("保護を解除した編集用コピー…", this,
                                               [this] { guard([&] { createEditableCopy(); }); });
    editableCopyAction->setObjectName("createEditableCopy");
    certificateAction = createMenu->addAction("証明書署名を確認…", this, [this]
                                              { guard([&] { verifyDocumentCertificates(); }); });
    certificateAction->setObjectName("verifyDocumentCertificates");
    auto signCertificate = createMenu->addAction(
        "証明書で署名したコピー…", this, [this] { guard([&] { exportSignedCertificateCopy(); }); });
    signCertificate->setObjectName("exportSignedCertificateCopy");
    edits << signCertificate;
    auto redact = createMenu->addAction("墨消ししたコピー…", this,
                                        [this] { guard([&] { exportRedactedCopy(); }); });
    redact->setObjectName("exportRedactedCopy");
    edits << redact;
    auto existingImage = createMenu->addAction("PDF内の画像を編集…", this,
                                               [this] { guard([&] { editExistingImages(); }); });
    existingImage->setObjectName("editExistingImages");
    edits << existingImage;
    auto existingText = createMenu->addAction("PDF本文の文字を編集…", this,
                                              [this] { guard([&] { editExistingTextBlocks(); }); });
    existingText->setObjectName("editExistingTextBlocks");
    edits << existingText;
    comparisonAction = createMenu->addAction("PDFを比較…", this,
                                             [this] { guard([&] { compareWithDocument(); }); });
    comparisonAction->setObjectName("compareDocuments");
    auto batch = createMenu->addAction("複数PDFをまとめて処理…", this,
                                       [this] { guard([&] { processMultipleDocuments(); }); });
    batch->setObjectName("processMultipleDocuments");
    auto formData = createMenu->addMenu("フォーム入力データ");
    auto formDesign =
        createMenu->addAction("フォームを設計…", this, [this] { guard([&] { designForms(); }); });
    formDesign->setObjectName("designForms");
    edits << formDesign;
    formDataImportAction = formData->addAction("入力値を読み込む…", this,
                                               [this] { guard([&] { manageFormData(true); }); });
    formDataImportAction->setObjectName("importFormData");
    formDataExportAction = formData->addAction("入力値を書き出す…", this,
                                               [this] { guard([&] { manageFormData(false); }); });
    formDataExportAction->setObjectName("exportFormData");
    createAction->setMenu(createMenu);
    if (auto button = qobject_cast<QToolButton*>(top->widgetForAction(createAction)))
        button->setPopupMode(QToolButton::MenuButtonPopup);
    auto createButton = new QPushButton("画像からPDFを作成");
    createButton->setObjectName("imagePdfWelcomeButton");
    createButton->setMinimumHeight(44);
    createButton->setMaximumWidth(240);
    welcomeLayout->insertWidget(welcomeLayout->indexOf(mergeButton) + 1, createButton, 0,
                                Qt::AlignHCenter);
    connect(createButton, &QPushButton::clicked, createAction, &QAction::trigger);
    signatureAction->setCheckable(true);
    ocrAction->setCheckable(true);
    auto signatureMenu = new QMenu(this);
    signatureMenu->addAction("テキスト署名", this, [this] { showPanel(0); });
    signatureMenu->addAction("画像署名", this, [this] { showPanel(3); });
    signatureMenu->addSeparator();
    signatureMenu->addAction("保存済み署名から配置", this,
                             [this] { guard([&] { openSignatureLibrary(); }); });
    signatureAction->setMenu(signatureMenu);
    if (auto button = qobject_cast<QToolButton*>(top->widgetForAction(signatureAction)))
        button->setPopupMode(QToolButton::MenuButtonPopup);
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
    backView->setIcon(uiIcon(QStyle::SP_ArrowBack));
    forwardView->setIcon(uiIcon(QStyle::SP_ArrowForward));
    for (auto a : {backView, forwardView})
    {
        auto button = new QToolButton;
        button->setDefaultAction(a);
        button->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
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
    bookmarksPanel->editRequested = [this] { guard([&] { editBookmarks(); }); };
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
                    showNavigation(2);
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
            [this](int index)
            {
                if (pageControl)
                {
                    navigationRequested = true;
                    if (referenceControl)
                        for (int i = 0; i < referenceControl->menu()->actions().size(); ++i)
                            referenceControl->menu()->actions()[i]->setChecked(i == index);
                    syncReadingLayout();
                }
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
                const auto before = canvas->viewState();
                canvas->showSearchMatch(match);
                if (!before.samePosition(canvas->viewState()) || before.activeSearch != match.id)
                    rememberView(before);
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
        if (!before.samePosition(after))
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
    auto typography = new QHBoxLayout;
    auto familyColumn = new QVBoxLayout;
    signatureFontPicker = new TextFontPicker;
    signatureFontPicker->setObjectName("signatureFont");
    signatureFontPicker->setAccessibleName("署名の書体");
    auto fontLabel = new QLabel("書体");
    fontLabel->setBuddy(signatureFontPicker);
    familyColumn->addWidget(fontLabel);
    familyColumn->addWidget(signatureFontPicker);
    typography->addLayout(familyColumn, 2);
    auto sizeColumn = new QVBoxLayout;
    sizeColumn->addWidget(new QLabel("文字サイズ"));
    size = new QDoubleSpinBox;
    size->setRange(6, 144);
    size->setValue(20);
    size->setSuffix(" pt");
    size->setButtonSymbols(QAbstractSpinBox::PlusMinus);
    size->setAccessibleName("署名サイズ");
    sizeColumn->addWidget(size);
    typography->addLayout(sizeColumn, 1);
    sl->addLayout(typography);
    connect(signatureFontPicker, &QComboBox::currentIndexChanged, this,
            [this] { signature->setFont(QFont(signatureFontPicker->family(), 10)); });
    signature->setFont(QFont(signatureFontPicker->family(), 10));
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
    place->setObjectName("placeSignature");
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
                        auto updated = doc.putSignature(canvas->page, signature->toPlainText(),
                                                        s.rect.topLeft(), size->value(), ink, s.ref,
                                                        signatureFontPicker->family());
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
    auto reuse = new QToolButton;
    reuse->setText("署名をこのPCに保存・再利用");
    reuse->setToolButtonStyle(Qt::ToolButtonTextOnly);
    reuse->setPopupMode(QToolButton::InstantPopup);
    auto reuseMenu = new QMenu(reuse);
    reuseMenu->addAction("この署名をPCに保存", this,
                         [this]
                         {
                             guard(
                                 [&]
                                 {
                                     SignatureTemplate item;
                                     item.text = signature->toPlainText();
                                     item.size = size->value();
                                     item.color = ink;
                                     item.fontFamily = signatureFontPicker->family();
                                     saveSignatureTemplate(item);
                                 });
                         });
    reuseMenu->addAction("保存済み署名から配置", this,
                         [this] { guard([&] { openSignatureLibrary(); }); });
    reuse->setMenu(reuseMenu);
    sl->addWidget(reuse);
    sl->addStretch();
    panels->addWidget(scrollableSettings(sign));
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
    auto accuracy = new QLabel("横書きの鮮明な印刷文字を対象とします。縦書き・ルビ・段組みや不鮮明"
                               "な原稿は、認識結果を原文と照合してください。");
    accuracy->setObjectName("ocrAccuracy");
    accuracy->setWordWrap(true);
    ol->addWidget(accuracy);
    ol->addWidget(new QLabel("文書の言語"));
    language = new QComboBox;
    language->addItems(ocrLanguageLabels());
    language->setObjectName("ocrLanguage");
    connect(language, &QComboBox::currentIndexChanged, accuracy,
            [accuracy](int index)
            {
                accuracy->setText(
                    index == 3
                        ? "回転のない標準単位のページで、上から下、右から左の日本語縦書きを"
                          "対象とします。回転・特殊なページ単位は処理できません。ルビ・古い"
                          "字体・見開き・横書き混在は原文と照合してください。"
                        : "横書きの鮮明な印刷文字を対象とします。縦書き・ルビ・段組みや不鮮明"
                          "な原稿は、認識結果を原文と照合してください。");
            });
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
    panels->addWidget(scrollableSettings(ocr));
    setupWriting();
    setupOrganizer();
    setupAnnotations();
    properties->hide();
    connect(properties, &QDockWidget::visibilityChanged, this,
            [this](bool visible)
            {
                signatureAction->setChecked(
                    visible && (panels->currentIndex() == 0 || panels->currentIndex() == 3));
                writingAction->setChecked(visible && panels->currentIndex() == 2);
                annotationAction->setChecked(visible && panels->currentIndex() == 5);
                ocrAction->setChecked(visible && panels->currentIndex() == 1);
                if (pageControl)
                    syncReadingLayout();
            });
    canvas->place = [this](QPointF point)
    {
        guard(
            [&]
            {
                if (panels->currentIndex() == 5)
                    refresh(false, annotationPanel->place(point));
                else if (panels->currentIndex() == 2 || panels->currentIndex() == 3)
                {
                    auto editor = panels->currentIndex() == 2 ? writingPanel : imageSignaturePanel;
                    refresh(false, editor->place(point));
                }
                else
                {
                    auto added =
                        doc.putSignature(canvas->page, signature->toPlainText(), point,
                                         size->value(), ink, {}, signatureFontPicker->family());
                    refresh(false, added.ref);
                }
            });
    };
    canvas->select = [this](int i)
    {
        auto ss = signatures(doc.pdf(), canvas->page);
        if (i >= 0 && i < ss.size())
        {
            if (isAnnotation(ss[i].kind))
            {
                annotationPanel->setSelection(ss[i]);
                showPanel(5);
                return;
            }
            if (ss[i].kind != OverlayKind::SignatureText)
            {
                auto editor =
                    ss[i].kind == OverlayKind::SignatureImage ? imageSignaturePanel : writingPanel;
                editor->setSelection(ss[i]);
                showPanel(ss[i].kind == OverlayKind::SignatureImage ? 3 : 2);
                return;
            }
            signature->setPlainText(ss[i].text);
            signatureFontPicker->setFamily(ss[i].fontFamily);
            size->setValue(ss[i].size);
            ink = ss[i].color;
            showPanel(0);
        }
    };
    canvas->changed = [this] { refresh(); };
    canvas->draw = [this](QPointF start, QPointF finish)
    { guard([&] { refresh(false, annotationPanel->draw(start, finish)); }); };
    status = new ElidedLabel("PDFを開いてください");
    status->setObjectName("documentStatus");
    canvas->interactionCancelled = [this] { refreshStatus(); };
    canvas->escapeReading = [this] { setReadingMode(false); };
    canvas->toolChanged = [this]
    {
        handToolAction->setChecked(canvas->handToolActive());
        selectToolAction->setChecked(!canvas->handToolActive());
        refreshStatus();
    };
    statusBar()->addWidget(status, 1);
    progress = new ElidedLabel;
    progress->setObjectName("operationProgress");
    statusBar()->addWidget(progress, 1);
    cancel = new QPushButton("OCRを中止");
    cancel->setObjectName("ocrCancel");
    statusBar()->addWidget(cancel);
    cancel->hide();
    connect(cancel, &QPushButton::clicked, this, &Window::stopOcr);
    // These controls remain reachable when both docks and the document toolbar are hidden.
    for (auto a : {backView, forwardView})
    {
        auto button = new QToolButton;
        button->setDefaultAction(a);
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
    referenceAction = new QAction("参照", this);
    referenceAction->setObjectName("showReferences");
    referenceAction->setToolTip("ページ・しおり・検索を開く。矢印から参照先を選べます。");
    connect(referenceAction, &QAction::triggered, this,
            [this] { showNavigation(navigationTabs->currentIndex()); });
    referenceControl = new QToolButton;
    referenceControl->setObjectName("referenceControl");
    referenceControl->setDefaultAction(referenceAction);
    referenceControl->setAccessibleName("ページ・しおり・検索を開く");
    referenceControl->setPopupMode(QToolButton::MenuButtonPopup);
    auto referenceMenu = new QMenu(referenceControl);
    auto referenceGroup = new QActionGroup(referenceMenu);
    const QStringList referenceNames{"ページ", "しおり", "検索"};
    const QStringList referenceIds{"showPageReferences", "showBookmarkReferences",
                                   "showSearchReferences"};
    for (int i = 0; i < referenceNames.size(); ++i)
    {
        auto action = new QAction(referenceNames[i], referenceGroup);
        action->setObjectName(referenceIds[i]);
        action->setCheckable(true);
        action->setChecked(i == navigationTabs->currentIndex());
        referenceMenu->addAction(action);
        connect(action, &QAction::triggered, this, [this, i] { showNavigation(i); });
    }
    referenceControl->setMenu(referenceMenu);
    statusBar()->addPermanentWidget(referenceControl);
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
    zoom->setObjectName("zoomControl");
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
        annotationPanel->refresh();
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
    if (organizing && index != 4)
        setOrganizing(false);
    const auto anchor = canvas->anchor();
    navigationRequested = false;
    setReadingMode(false);
    if (index != 5 || canvas->placing)
        canvas->cancelInteraction();
    panels->setCurrentIndex(index);
    properties->setWindowTitle(
        QStringList{"署名", "OCR", "書き込み", "画像署名", "ページ整理", "注釈"}.value(index));
    properties->show();
    signatureAction->setChecked(index == 0 || index == 3);
    writingAction->setChecked(index == 2);
    annotationAction->setChecked(index == 5);
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
    const bool visible = doc.loaded() && !readingMode && !organizing &&
                         (!narrowProperties || searching || navigationRequested);
    if (!navigation->isHidden() != visible)
    {
        const auto anchor = canvas->anchor();
        navigation->setVisible(visible);
        preserveLayoutAnchor(anchor);
    }
}
void Window::showNavigation(int index)
{
    if (!doc.loaded() || index < 0 || index >= navigationTabs->count())
        return;
    if (organizing)
        setOrganizing(false);
    const auto anchor = canvas->anchor();
    navigationRequested = true;
    setReadingMode(false);
    navigationTabs->setCurrentIndex(index);
    syncReadingLayout();
    preserveLayoutAnchor(anchor);
    if (index == 2)
    {
        query->setFocus();
        query->selectAll();
    }
    else
    {
        auto target =
            index == 1
                ? static_cast<QWidget*>(bookmarksPanel->findChild<QTreeWidget*>("bookmarkTree"))
                : static_cast<QWidget*>(pages);
        (target->isVisible() ? target : static_cast<QWidget*>(navigationTabs->tabBar()))
            ->setFocus();
    }
}
void Window::setReadingMode(bool enabled)
{
    if (enabled && organizing)
        setOrganizing(false);
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
    workToolbar->setVisible(!enabled);
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
    // Worker completion and dock visibility signals must not refresh torn-down UI.
    for (auto child : findChildren<QObject*>())
        disconnect(child, nullptr, this, nullptr);
    delete searchPanel;
    searchPanel = nullptr;
    if (worker)
    {
        worker->kill();
        worker->waitForFinished(3000);
    }
    canvas->cancelFormEdit();
    delete canvas;
    canvas = nullptr;
}
void Window::guard(const std::function<void()>& f)
{
    try
    {
        canvas->finishFormEdit();
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
    navigationRequested = false;
    organizing = false;
    organizeAction->setChecked(false);
    initialPagePending = true;
    setReadingMode(false);
    canvas->resetView();
    bookmarksPanel->reset();
    canvas->selected = -1;
    query->clear();
    viewHistory.clear();
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
    rebuild = rebuild || pages->count() != doc.pages();
    documentArea->setCurrentIndex(doc.loaded() ? (organizing ? 2 : 1) : 0);
    organizer->refresh();
    syncReadingLayout();
    readingAction->setEnabled(doc.loaded());
    referenceAction->setEnabled(doc.loaded());
    imageExportAction->setEnabled(doc.loaded() && doc.copyAllowed);
    tableExportAction->setEnabled(doc.loaded() && doc.copyAllowed);
    comparisonAction->setEnabled(doc.loaded() && doc.copyAllowed && !doc.busy);
    certificateAction->setEnabled(doc.loaded() && !doc.busy && !doc.dirty());
    editableCopyAction->setEnabled(doc.loaded() && !doc.busy &&
                                   doc.pdf().getStorage().getSecurityHandler()->getMode() ==
                                       EncryptionMode::Standard);
    zoomControl->setEnabled(doc.loaded());
    printAction->setEnabled(doc.loaded());
    if (!doc.loaded())
    {
        formDataChecked = false;
        formDataAvailable = false;
        formDataNotice = "対応するフォーム入力欄を持つPDFを開いてください。";
    }
    else if (!formDataChecked || formDataRevision != doc.revision)
    {
        formDataRevision = doc.revision;
        formDataChecked = true;
        formDataAvailable = false;
        formDataNotice = "対応するフォーム入力欄がありません。";
        if (!doc.pdf().getCatalog()->getFormObject().isNull())
            try
            {
                formDataAvailable = !formDataValues(doc.pdf()).fields.isEmpty();
            }
            catch (const std::exception& error)
            {
                formDataNotice = QString::fromUtf8(error.what());
            }
            catch (...)
            {
                formDataNotice = "フォームの入力値を確認できません。";
            }
    }
    formDataImportAction->setEnabled(formDataAvailable && doc.readOnly.isEmpty() && !doc.busy);
    formDataExportAction->setEnabled(formDataAvailable && doc.copyAllowed && !doc.busy);
    formDataImportAction->setToolTip(!doc.readOnly.isEmpty() ? doc.readOnly
                                     : formDataAvailable
                                         ? "読み込む値を確認してから、まとめて適用します。"
                                         : formDataNotice);
    formDataExportAction->setToolTip(
        formDataAvailable ? "入力値を平文のXFDFファイルへ書き出します。" : formDataNotice);
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
    annotationPanel->refresh();
    searchPanel->setDocument(doc.loaded() ? &doc.pdf() : nullptr, doc.revision, canvas->page);
    static_cast<PagePreviews*>(pages)->setDocument(doc.loaded() ? &doc.pdf() : nullptr,
                                                   doc.revision);
    setWindowTitle(
        QString("%1%2 — PDF達人 評価版")
            .arg(doc.dirty() ? "● " : "")
            .arg(doc.loaded()
                     ? (doc.source.isEmpty() && doc.target.isEmpty()
                            ? "新しい文書"
                            : QFileInfo(doc.target.isEmpty() ? doc.source : doc.target).fileName())
                     : "PDFを開く"));
    bool can = doc.loaded() && doc.readOnly.isEmpty() && !doc.busy;
    bookmarksPanel->setEditable(can);
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
    viewHistory.remember({state, query->text(), doc.revision});
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
    if (!before.samePosition(after))
        rememberView(before);
    canvas->setFocus();
    statusBar()->showMessage(pageDescription(resolved.page, pageLabels) + "へ移動しました", 4000);
}
void Window::moveHistory(bool forward)
{
    if (!doc.loaded())
        return;
    const auto destination =
        viewHistory.move(forward ? ViewHistory::Direction::Forward : ViewHistory::Direction::Back,
                         {canvas->viewState(), query->text(), doc.revision});
    if (!destination)
        return;
    initialPagePending = false;
    ++layoutGeneration;
    canvas->restoreView(destination->view);
    searchPanel->restoreActive(destination->activeSearchFor(query->text(), doc.revision));
    updateHistoryActions();
}
void Window::updateHistoryActions()
{
    backView->setEnabled(doc.loaded() && viewHistory.canMove(ViewHistory::Direction::Back));
    forwardView->setEnabled(doc.loaded() && viewHistory.canMove(ViewHistory::Direction::Forward));
    QWidget* focus = QApplication::focusWidget();
    const bool editing = qobject_cast<QLineEdit*>(focus) || qobject_cast<QPlainTextEdit*>(focus) ||
                         qobject_cast<QTextEdit*>(focus) || qobject_cast<QAbstractSpinBox*>(focus);
    backShortcut->setEnabled(backView->isEnabled() && !editing);
    forwardShortcut->setEnabled(forwardView->isEnabled() && !editing);
}
void Window::refreshStatus()
{
    setWindowTitle(
        QString("%1%2 — PDF達人 評価版")
            .arg(doc.dirty() ? "● " : "")
            .arg(doc.loaded()
                     ? (doc.source.isEmpty() && doc.target.isEmpty()
                            ? "新しい文書"
                            : QFileInfo(doc.target.isEmpty() ? doc.source : doc.target).fileName())
                     : "PDFを開く"));
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
    canvas->finishFormEdit();
    doc.editable();
    QString path = doc.target;
    if (choose || path.isEmpty())
        path = QFileDialog::getSaveFileName(
            this, "PDFとして保存",
            path.isEmpty() ? (doc.source.isEmpty()
                                  ? QDir::homePath() + "/結合した文書.pdf"
                                  : QFileInfo(doc.source).absolutePath() + "/" +
                                        QFileInfo(doc.source).completeBaseName() + "_編集.pdf")
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
    canvas->finishFormEdit();
    doc.editable();
    QString selection = scope->currentIndex() == 0   ? QString("1-%1").arg(doc.pages())
                        : scope->currentIndex() == 1 ? QString::number(canvas->page + 1)
                                                     : range->text();
    auto nextJob = OcrJob::prepare(doc, {ocrLanguageCodes()[language->currentIndex()], selection});
    auto nextWorker = std::make_unique<QProcess>(this);
    auto nextChannels = std::make_unique<WorkerChannels>(*nextWorker, nextJob->path());
    work = std::move(nextJob);
    worker = nextWorker.release();
    workerChannels = std::move(nextChannels);
    doc.busy = true;
    refresh();
    progress->setText("OCRを開始しています…");
    cancel->show();
    progressBuffer.clear();
    auto updateProgress = [this, observed = worker]
    {
        if (worker != observed || !workerChannels)
            return;
        progressBuffer += workerChannels->progress();
        while (progressBuffer.contains('\n'))
        {
            auto line = progressBuffer.left(progressBuffer.indexOf('\n'));
            progressBuffer.remove(0, line.size() + 1);
            auto p = QJsonDocument::fromJson(line).object();
            if (!p.isEmpty())
                progress->setText(
                    QString("OCR %1 / %2 ページ").arg(p["done"].toInt()).arg(p["total"].toInt()));
        }
    };
    connect(worker, &QProcess::readyReadStandardOutput, this, updateProgress);
    if (workerChannels->usesFiles())
    {
        auto poll = new QTimer(worker);
        connect(poll, &QTimer::timeout, this, updateProgress);
        poll->start(100);
    }
    connect(worker, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), this,
            [this, observed = worker](int code, QProcess::ExitStatus exitStatus)
            {
                if (worker == observed)
                    finishOcr(code, exitStatus);
            });
    connect(worker, &QProcess::errorOccurred, this,
            [this, observed = worker](QProcess::ProcessError e)
            {
                if (worker == observed && e == QProcess::FailedToStart)
                    finishOcr(-1, QProcess::CrashExit);
            });
    QTimer::singleShot(15 * 60 * 1000, worker,
                       [this, observed = worker]
                       {
                           if (worker == observed && worker->state() != QProcess::NotRunning)
                           {
                               worker->setProperty("timedOut", true);
                               worker->kill();
                           }
                       });
    worker->start(QCoreApplication::applicationFilePath(), work->workerArguments());
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
    auto err = workerChannels->error();
    workerChannels.reset();
    worker->deleteLater();
    worker = nullptr;
    // A result dialog runs a nested event loop. Keep the completed job local so
    // its cleanup cannot release a later job started during that loop.
    auto finishedJob = std::move(work);
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
            auto result = finishedJob->readResult(doc);
            QStringList summary;
            const bool changed = result.changed();
            for (const auto& page : result.pages)
                summary << QString("%1ページ: %2").arg(page.page).arg(page.status);
            if (changed)
                doc.commit(std::move(result.document));
            progress->setText(changed ? "OCR反映済み。元に戻すで取り消せます。"
                                      : "OCR終了。追加された文字はありません。");
            QMessageBox::information(this, "OCR結果", summary.join('\n'));
        });
    finishedJob.reset();
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
    bool close = false;
    guard([&] { close = safeToClose(); });
    if (close)
        e->accept();
    else
        e->ignore();
}
void Window::dragEnterEvent(QDragEnterEvent* e)
{
    if (e->mimeData()->hasUrls() && !e->mimeData()->urls().isEmpty())
        e->acceptProposedAction();
}
void Window::dropEvent(QDropEvent* e)
{
    auto urls = e->mimeData()->urls();
    if (urls.isEmpty())
        return;
    guard(
        [&]
        {
            if (urls.size() > 1)
            {
                QStringList paths;
                for (const auto& url : urls)
                {
                    if (!url.isLocalFile())
                        fail("ローカルのPDFを指定してください。");
                    paths.append(url.toLocalFile());
                }
                const auto answer = QMessageBox::question(
                    this, "複数のPDF", "結合しますか？「いいえ」で個別の文書ウィンドウを開きます。",
                    QMessageBox::Yes | QMessageBox::No | QMessageBox::Cancel);
                if (answer == QMessageBox::Cancel)
                    return;
                if (answer == QMessageBox::Yes)
                {
                    mergeFiles(paths);
                    return;
                }
                for (const auto& path : paths)
                {
                    auto window = new Window;
                    window->setAttribute(Qt::WA_DeleteOnClose);
                    window->show();
                    window->openFile(path);
                }
                return;
            }
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
