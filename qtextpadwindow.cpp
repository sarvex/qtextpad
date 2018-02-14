/* This file is part of QTextPad.
 *
 * QTextPad is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * QTextPad is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with QTextPad.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "qtextpadwindow.h"

#include <QLabel>
#include <QToolButton>
#include <QMenuBar>
#include <QToolBar>
#include <QStatusBar>
#include <QFontDialog>
#include <QApplication>
#include <QWidgetAction>
#include <QMessageBox>
#include <QFileDialog>
#include <QClipboard>
#include <QUndoStack>
#include <QTextBlock>
#include <QTextCodec>
#include <QTimer>
#include <QFileInfo>

#include <KSyntaxHighlighting/Repository>
#include <KSyntaxHighlighting/Definition>
#include <KSyntaxHighlighting/Theme>
#include <KCharsets>

#include "activationlabel.h"
#include "syntaxtextedit.h"
#include "settingspopup.h"
#include "searchdialog.h"
#include "indentsettings.h"
#include "appsettings.h"
#include "undocommands.h"
#include "ftdetect.h"

#define LARGE_FILE_SIZE     (10*1024*1024)  // 10 MiB
#define DETECTION_SIZE      (1*1024)
#define DECODE_BLOCK_SIZE   (16*1024)

class EncodingPopupAction : public QWidgetAction
{
public:
    explicit EncodingPopupAction(QTextPadWindow *parent)
        : QWidgetAction(parent) { }

protected:
    QWidget *createWidget(QWidget *menuParent) Q_DECL_OVERRIDE
    {
        auto popup = new EncodingPopup(menuParent);
        connect(popup, &EncodingPopup::encodingSelected,
                [this](const QString &codecName) {
            auto window = qobject_cast<QTextPadWindow *>(parent());
            window->changeEncoding(codecName);

            // Don't close the popup right after clicking, so the user can
            // briefly see the visual feedback for the item they selected.
            QTimer::singleShot(100, []() {
                QApplication::activePopupWidget()->close();
            });
        });
        return popup;
    }
};

class SyntaxPopupAction : public QWidgetAction
{
public:
    explicit SyntaxPopupAction(QTextPadWindow *parent)
        : QWidgetAction(parent) { }

protected:
    QWidget *createWidget(QWidget *menuParent) Q_DECL_OVERRIDE
    {
        auto popup = new SyntaxPopup(menuParent);
        connect(popup, &SyntaxPopup::syntaxSelected,
                [this](const KSyntaxHighlighting::Definition &syntax) {
            auto window = qobject_cast<QTextPadWindow *>(parent());
            window->setSyntax(syntax);

            // Don't close the popup right after clicking, so the user can
            // briefly see the visual feedback for the item they selected.
            QTimer::singleShot(100, []() {
                QApplication::activePopupWidget()->close();
            });
        });
        return popup;
    }
};

QTextPadWindow::QTextPadWindow(QWidget *parent)
    : QMainWindow(parent)
{
    m_editor = new SyntaxTextEdit(this);
    setCentralWidget(m_editor);
    m_editor->setFrameStyle(QFrame::NoFrame);

    m_undoStack = new QUndoStack(this);
    connect(m_editor->document(), &QTextDocument::undoCommandAdded,
            [this]() { addUndoCommand(new TextEditorUndoCommand(m_editor)); });
    connect(m_editor, &SyntaxTextEdit::parentUndo, m_undoStack, &QUndoStack::undo);
    connect(m_editor, &SyntaxTextEdit::parentRedo, m_undoStack, &QUndoStack::redo);

    QMenu *fileMenu = menuBar()->addMenu(tr("&File"));
    auto newAction = fileMenu->addAction(ICON("document-new"), tr("&New"));
    newAction->setShortcut(QKeySequence::New);
    (void) fileMenu->addSeparator();
    auto openAction = fileMenu->addAction(ICON("document-open"), tr("&Open..."));
    openAction->setShortcut(QKeySequence::Open);
    m_recentFiles = fileMenu->addMenu(tr("Open &Recent"));
    populateRecentFiles();
    m_reloadAction = fileMenu->addAction(ICON("view-refresh"), tr("Re&load"));
    m_reloadAction->setShortcut(QKeySequence::Refresh);
    (void) fileMenu->addSeparator();
    auto saveAction = fileMenu->addAction(ICON("document-save"), tr("&Save"));
    saveAction->setShortcut(QKeySequence::Save);
    auto saveAsAction = fileMenu->addAction(ICON("document-save-as"), tr("Save &As..."));
    saveAsAction->setShortcut(QKeySequence::SaveAs);
    auto saveCopyAction = fileMenu->addAction(ICON("document-save-as"), tr("Save &Copy..."));
    (void) fileMenu->addSeparator();
    auto printAction = fileMenu->addAction(ICON("document-print"), tr("&Print..."));
    printAction->setShortcut(QKeySequence::Print);
    auto printPreviewAction = fileMenu->addAction(ICON("document-preview"), tr("Print Previe&w"));
    (void) fileMenu->addSeparator();
    auto quitAction = fileMenu->addAction(tr("&Quit"));
    quitAction->setShortcut(QKeySequence::Quit);

    connect(newAction, &QAction::triggered, this, &QTextPadWindow::newDocument);
    connect(openAction, &QAction::triggered, this, &QTextPadWindow::loadDocument);
    connect(m_reloadAction, &QAction::triggered, this, &QTextPadWindow::reloadDocument);
    connect(saveAction, &QAction::triggered, this, &QTextPadWindow::saveDocument);
    connect(saveAsAction, &QAction::triggered, this, &QTextPadWindow::saveDocumentAs);
    connect(saveCopyAction, &QAction::triggered, this, &QTextPadWindow::saveDocumentCopy);
    //connect(printAction, &QAction::triggered, ...);
    //connect(printPreviewAction, &QAction::triggered, ...);
    connect(quitAction, &QAction::triggered, this, &QWidget::close);

    QMenu *editMenu = menuBar()->addMenu(tr("&Edit"));
    auto undoAction = editMenu->addAction(ICON("edit-undo"), tr("&Undo"));
    undoAction->setShortcut(QKeySequence::Undo);
    m_editorContextActions << undoAction;
    auto redoAction = editMenu->addAction(ICON("edit-redo"), tr("&Redo"));
    redoAction->setShortcut(QKeySequence::Redo);
    m_editorContextActions << redoAction;
    m_editorContextActions << editMenu->addSeparator();
    auto cutAction = editMenu->addAction(ICON("edit-cut"), tr("Cu&t"));
    cutAction->setShortcut(QKeySequence::Cut);
    m_editorContextActions << cutAction;
    auto copyAction = editMenu->addAction(ICON("edit-copy"), tr("&Copy"));
    copyAction->setShortcut(QKeySequence::Copy);
    m_editorContextActions << copyAction;
    auto pasteAction = editMenu->addAction(ICON("edit-paste"), tr("&Paste"));
    pasteAction->setShortcut(QKeySequence::Paste);
    m_editorContextActions << pasteAction;
    auto clearAction = editMenu->addAction(ICON("edit-delete"), tr("&Delete"));
    clearAction->setShortcut(QKeySequence::Delete);
    m_editorContextActions << clearAction;
    m_editorContextActions << editMenu->addSeparator();
    auto selectAllAction = editMenu->addAction(tr("Select &All"));
    selectAllAction->setShortcut(QKeySequence::SelectAll);
    m_editorContextActions << selectAllAction;
    (void) editMenu->addSeparator();
    m_overwriteModeAction = editMenu->addAction(tr("&Overwrite Mode"));
    m_overwriteModeAction->setShortcut(Qt::Key_Insert);
    m_overwriteModeAction->setCheckable(true);
    (void) editMenu->addSeparator();
    auto findAction = editMenu->addAction(ICON("edit-find"), tr("&Find..."));
    findAction->setShortcut(QKeySequence::Find);
    auto findNextAction = editMenu->addAction(tr("Find &Next"));
    findNextAction->setShortcut(QKeySequence::FindNext);
    auto findPrevAction = editMenu->addAction(tr("Find &Previous"));
    findPrevAction->setShortcut(QKeySequence::FindPrevious);
    auto replaceAction = editMenu->addAction(ICON("edit-find-replace"), tr("&Replace..."));
    replaceAction->setShortcut(QKeySequence::Replace);

    connect(undoAction, &QAction::triggered, m_undoStack, &QUndoStack::undo);
    connect(redoAction, &QAction::triggered, m_undoStack, &QUndoStack::redo);
    connect(cutAction, &QAction::triggered, m_editor, &QPlainTextEdit::cut);
    connect(copyAction, &QAction::triggered, m_editor, &QPlainTextEdit::copy);
    connect(pasteAction, &QAction::triggered, m_editor, &QPlainTextEdit::paste);
    connect(clearAction, &QAction::triggered, m_editor, &SyntaxTextEdit::deleteSelection);
    connect(selectAllAction, &QAction::triggered, m_editor, &QPlainTextEdit::selectAll);
    connect(m_overwriteModeAction, &QAction::toggled,
            this, &QTextPadWindow::setOverwriteMode);

    connect(findAction, &QAction::triggered, [this]() { SearchDialog::create(this, false); });
    connect(findNextAction, &QAction::triggered, [this]() { SearchDialog::searchNext(this, false); });
    connect(findPrevAction, &QAction::triggered, [this]() { SearchDialog::searchNext(this, true); });
    connect(replaceAction, &QAction::triggered, [this]() { SearchDialog::create(this, true); });

    connect(m_undoStack, &QUndoStack::canUndoChanged, undoAction, &QAction::setEnabled);
    undoAction->setEnabled(false);
    connect(m_undoStack, &QUndoStack::canRedoChanged, redoAction, &QAction::setEnabled);
    redoAction->setEnabled(false);
    connect(m_editor, &QPlainTextEdit::copyAvailable, cutAction, &QAction::setEnabled);
    cutAction->setEnabled(false);
    connect(m_editor, &QPlainTextEdit::copyAvailable, copyAction, &QAction::setEnabled);
    copyAction->setEnabled(false);
    connect(m_editor, &QPlainTextEdit::copyAvailable, clearAction, &QAction::setEnabled);
    clearAction->setEnabled(false);

    connect(QApplication::clipboard(), &QClipboard::dataChanged, [this, pasteAction]() {
        pasteAction->setEnabled(m_editor->canPaste());
    });
    pasteAction->setEnabled(m_editor->canPaste());

    // The Editor's default context menu hooks directly into the QTextDocument's
    // undo stack, which won't see our custom actions.  We might as well just
    // use the app's actions for everything else while we're fixing that.
    m_editor->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(m_editor, &QPlainTextEdit::customContextMenuRequested,
            this, &QTextPadWindow::editorContextMenu);

    QMenu *viewMenu = menuBar()->addMenu(tr("&View"));
    auto fontAction = viewMenu->addAction(tr("Default &Font..."));
    m_themeMenu = viewMenu->addMenu(tr("&Theme"));
    populateThemeMenu();
    (void) viewMenu->addSeparator();
    auto wordWrapAction = viewMenu->addAction(tr("&Word Wrap"));
    wordWrapAction->setShortcut(Qt::CTRL | Qt::Key_W);
    wordWrapAction->setCheckable(true);
    auto longLineAction = viewMenu->addAction(tr("Long Line &Margin"));
    longLineAction->setShortcut(Qt::CTRL | Qt::SHIFT | Qt::Key_M);
    longLineAction->setCheckable(true);
    auto longLineSettingsAction = viewMenu->addAction(tr("Long Line Se&ttings..."));
    auto indentGuidesAction = viewMenu->addAction(tr("&Indentation Guides"));
    indentGuidesAction->setShortcut(Qt::CTRL | Qt::SHIFT | Qt::Key_T);
    indentGuidesAction->setCheckable(true);
    (void) viewMenu->addSeparator();
    auto showLineNumbersAction = viewMenu->addAction(tr("Line &Numbers"));
    showLineNumbersAction->setShortcut(Qt::CTRL | Qt::SHIFT | Qt::Key_N);
    showLineNumbersAction->setCheckable(true);
    auto showWhitespaceAction = viewMenu->addAction(tr("Show White&space"));
    showWhitespaceAction->setShortcut(Qt::CTRL | Qt::SHIFT | Qt::Key_W);
    showWhitespaceAction->setCheckable(true);
    (void) viewMenu->addSeparator();
    auto showCurrentLineAction = viewMenu->addAction(tr("Highlight &Current Line"));
    showCurrentLineAction->setCheckable(true);
    auto showMatchingBraces = viewMenu->addAction(tr("Match &Braces"));
    showMatchingBraces->setCheckable(true);
    (void) viewMenu->addSeparator();
    auto zoomInAction = viewMenu->addAction(ICON("zoom-in"), tr("Zoom &In"));
    zoomInAction->setShortcut(QKeySequence::ZoomIn);
    auto zoomOutAction = viewMenu->addAction(ICON("zoom-out"), tr("Zoom &Out"));
    zoomOutAction->setShortcut(QKeySequence::ZoomOut);
    auto zoomResetAction = viewMenu->addAction(ICON("zoom-original"), tr("Reset &Zoom"));
    zoomResetAction->setShortcut(Qt::CTRL | Qt::Key_0);

    connect(fontAction, &QAction::triggered, this, &QTextPadWindow::chooseEditorFont);
    connect(wordWrapAction, &QAction::toggled, m_editor, &SyntaxTextEdit::setWordWrap);
    connect(longLineAction, &QAction::toggled,
            m_editor, &SyntaxTextEdit::setShowLongLineEdge);
    //connect(longLineSettingsAction, &QAction::triggered, ...);
    connect(indentGuidesAction, &QAction::toggled,
            m_editor, &SyntaxTextEdit::setShowIndentGuides);
    connect(showLineNumbersAction, &QAction::toggled,
            m_editor, &SyntaxTextEdit::setShowLineNumbers);
    connect(showWhitespaceAction, &QAction::toggled,
            m_editor, &SyntaxTextEdit::setShowWhitespace);
    connect(showCurrentLineAction, &QAction::toggled,
            m_editor, &SyntaxTextEdit::setHighlightCurrentLine);
    connect(showMatchingBraces, &QAction::toggled,
            m_editor, &SyntaxTextEdit::setMatchBraces);
    connect(zoomInAction, &QAction::triggered, m_editor, &SyntaxTextEdit::zoomIn);
    connect(zoomOutAction, &QAction::triggered, m_editor, &SyntaxTextEdit::zoomOut);
    connect(zoomResetAction, &QAction::triggered, m_editor, &SyntaxTextEdit::zoomReset);

    QMenu *settingsMenu = menuBar()->addMenu(tr("&Settings"));
    m_syntaxMenu = settingsMenu->addMenu(tr("&Syntax"));
    populateSyntaxMenu();
    m_encodingMenu = settingsMenu->addMenu(tr("&Encoding"));
    populateEncodingMenu();
    auto lineEndingMenu = settingsMenu->addMenu(tr("&Line Endings"));
    m_lineEndingActions = new QActionGroup(this);
    auto crOnlyAction = lineEndingMenu->addAction(tr("Classic Mac (CR)"));
    crOnlyAction->setCheckable(true);
    crOnlyAction->setActionGroup(m_lineEndingActions);
    crOnlyAction->setData(static_cast<int>(CROnly));
    auto lfOnlyAction = lineEndingMenu->addAction(tr("UNIX (LF)"));
    lfOnlyAction->setCheckable(true);
    lfOnlyAction->setActionGroup(m_lineEndingActions);
    lfOnlyAction->setData(static_cast<int>(LFOnly));
    auto crlfAction = lineEndingMenu->addAction(tr("Windows/DOS (CRLF)"));
    crlfAction->setCheckable(true);
    crlfAction->setActionGroup(m_lineEndingActions);
    crlfAction->setData(static_cast<int>(CRLF));
    (void) settingsMenu->addSeparator();
    auto tabSettingsAction = settingsMenu->addAction(tr("&Tab Settings..."));
    m_autoIndentAction = settingsMenu->addAction(tr("&Auto Indent"));
    m_autoIndentAction->setShortcut(Qt::CTRL | Qt::SHIFT | Qt::Key_I);
    m_autoIndentAction->setCheckable(true);

    connect(crOnlyAction, &QAction::triggered, [this]() { changeLineEndingMode(CROnly); });
    connect(lfOnlyAction, &QAction::triggered, [this]() { changeLineEndingMode(LFOnly); });
    connect(crlfAction, &QAction::triggered, [this]() { changeLineEndingMode(CRLF); });
    connect(tabSettingsAction, &QAction::triggered, this, &QTextPadWindow::promptTabSettings);
    connect(m_autoIndentAction, &QAction::toggled, this, &QTextPadWindow::setAutoIndent);

    QMenu *helpMenu = menuBar()->addMenu(tr("&Help"));
    auto aboutAction = helpMenu->addAction(ICON("help-about"), tr("&About..."));
    aboutAction->setShortcut(QKeySequence::HelpContents);

    auto toolBar = addToolBar(tr("Toolbar"));
    toolBar->setIconSize(QSize(22, 22));
    toolBar->setMovable(false);
    toolBar->addAction(newAction);
    toolBar->addAction(openAction);
    toolBar->addAction(saveAction);
    (void) toolBar->addSeparator();
    toolBar->addAction(undoAction);
    toolBar->addAction(redoAction);
    (void) toolBar->addSeparator();
    toolBar->addAction(cutAction);
    toolBar->addAction(copyAction);
    toolBar->addAction(pasteAction);
    (void) toolBar->addSeparator();
    toolBar->addAction(findAction);
    toolBar->addAction(replaceAction);

    m_positionLabel = new ActivationLabel(this);
    statusBar()->addWidget(m_positionLabel, 1);
    m_insertLabel = new ActivationLabel(this);
    statusBar()->addPermanentWidget(m_insertLabel);
    m_crlfLabel = new ActivationLabel(this);
    statusBar()->addPermanentWidget(m_crlfLabel);
    m_indentButton = new QToolButton(this);
    m_indentButton->setAutoRaise(true);;
    m_indentButton->setPopupMode(QToolButton::InstantPopup);
    populateIndentButtonMenu();
    statusBar()->addPermanentWidget(m_indentButton);
    m_encodingButton = new QToolButton(this);
    m_encodingButton->setAutoRaise(true);
    m_encodingButton->setPopupMode(QToolButton::InstantPopup);
    m_encodingButton->addAction(new EncodingPopupAction(this));
    statusBar()->addPermanentWidget(m_encodingButton);
    m_syntaxButton = new QToolButton(this);
    m_syntaxButton->setAutoRaise(true);
    m_syntaxButton->setPopupMode(QToolButton::InstantPopup);
    m_syntaxButton->addAction(new SyntaxPopupAction(this));
    statusBar()->addPermanentWidget(m_syntaxButton);
    updateCursorPosition();

    //connect(m_positionLabel, &ActivationLabel::activated, ...);
    connect(m_insertLabel, &ActivationLabel::activated,
            this, &QTextPadWindow::nextInsertMode);
    connect(m_crlfLabel, &ActivationLabel::activated,
            this, &QTextPadWindow::nextLineEndingMode);

    wordWrapAction->setChecked(m_editor->wordWrap());
    longLineAction->setChecked(m_editor->showLongLineEdge());
    indentGuidesAction->setChecked(m_editor->showIndentGuides());
    showLineNumbersAction->setChecked(m_editor->showLineNumbers());
    showWhitespaceAction->setChecked(m_editor->showWhitespace());
    showCurrentLineAction->setChecked(m_editor->highlightCurrentLine());
    showMatchingBraces->setChecked(m_editor->matchBraces());
    m_autoIndentAction->setChecked(m_editor->autoIndent());

    QTextPadSettings settings;
    auto theme = (m_editor->palette().color(QPalette::Base).lightness() < 128)
                 ? SyntaxTextEdit::syntaxRepo()->defaultTheme(KSyntaxHighlighting::Repository::DarkTheme)
                 : SyntaxTextEdit::syntaxRepo()->defaultTheme(KSyntaxHighlighting::Repository::LightTheme);
    QString themeName = settings.editorTheme();
    if (!themeName.isEmpty()) {
        auto requestedTheme = SyntaxTextEdit::syntaxRepo()->theme(themeName);
        if (requestedTheme.isValid())
            theme = requestedTheme;
    }
    setTheme(theme);

    m_insertLabel->setMinimumWidth(QFontMetrics(m_insertLabel->font()).width("OVR") + 4);
    m_crlfLabel->setMinimumWidth(QFontMetrics(m_crlfLabel->font()).width("CRLF") + 4);
    setOverwriteMode(false);

    connect(m_editor, &SyntaxTextEdit::cursorPositionChanged,
            this, &QTextPadWindow::updateCursorPosition);
    connect(m_undoStack, &QUndoStack::cleanChanged, [this](bool) { updateTitle(); });

    // Set up the editor and status for a clean, empty document
    newDocument();

    resize(settings.windowSize());
}

void QTextPadWindow::setSyntax(const KSyntaxHighlighting::Definition &syntax)
{
    m_editor->setSyntax(syntax);
    if (syntax.isValid())
        m_syntaxButton->setText(syntax.translatedName());
    else
        m_syntaxButton->setText(tr("Plain Text"));

    // Update the menus when this is triggered via other callers
    for (const auto &action : m_syntaxActions->actions()) {
        const auto actionDef = action->data().value<KSyntaxHighlighting::Definition>();
        if (actionDef == syntax) {
            action->setChecked(true);
            break;
        }
    }
}

void QTextPadWindow::setTheme(const KSyntaxHighlighting::Theme &theme)
{
    m_editor->setTheme(theme);

    // Update the menus when this is triggered via other callers
    for (const auto &action : m_themeActions->actions()) {
        const auto actionTheme = action->data().value<KSyntaxHighlighting::Theme>();
        if (actionTheme.filePath() == theme.filePath()) {
            action->setChecked(true);
            break;
        }
    }
}

void QTextPadWindow::setEncoding(const QString &codecName)
{
    m_textEncoding = codecName;

    // We may not directly match the passed encoding, so don't show a
    // radio check at all if we can't find the encoding.
    if (m_encodingActions->checkedAction())
        m_encodingActions->checkedAction()->setChecked(false);

    bool ok = false;
    (void) KCharsets::charsets()->codecForName(codecName, ok);
    if (!ok) {
        qWarning(tr("Invalid codec selected").toLocal8Bit().constData());
        m_encodingButton->setText(tr("Invalid (%1)").arg(codecName));
    } else {
        // Use the passed name for UI consistency
        m_encodingButton->setText(codecName);
    }

    // Update the menus when this is triggered via other callers
    for (const auto &action : m_encodingActions->actions()) {
        const auto actionCodec = action->data().toString();
        if (actionCodec == codecName) {
            action->setChecked(true);
            break;
        }
    }
}

void QTextPadWindow::setOverwriteMode(bool overwrite)
{
    m_editor->setOverwriteMode(overwrite);
    m_overwriteModeAction->setChecked(overwrite);
    m_insertLabel->setText(overwrite ? tr("OVR") : tr("INS"));
}

void QTextPadWindow::setAutoIndent(bool ai)
{
    m_editor->setAutoIndent(ai);
    m_autoIndentAction->setChecked(ai);
    updateIndentStatus();
}

void QTextPadWindow::setLineEndingMode(LineEndingMode mode)
{
    m_lineEndingMode = mode;

    switch (mode) {
    case CROnly:
        m_crlfLabel->setText(QStringLiteral("CR"));
        break;
    case LFOnly:
        m_crlfLabel->setText(QStringLiteral("LF"));
        break;
    case CRLF:
        m_crlfLabel->setText(QStringLiteral("CRLF"));
        break;
    }

    // Update the menus when this is triggered via other callers
    for (const auto &action : m_lineEndingActions->actions()) {
        if (action->data().toInt() == static_cast<int>(mode)) {
            action->setChecked(true);
            break;
        }
    }
}

bool QTextPadWindow::saveDocumentTo(const QString &filename)
{
    // TODO: Encode and save to file
    return true;
}

bool QTextPadWindow::loadDocumentFrom(const QString &filename, const QString &textEncoding)
{
    QFile file(filename);
    if (!file.open(QIODevice::ReadOnly)) {
        QMessageBox::critical(this, QString::null,
                              tr("Cannot open file %1 for reading").arg(filename));
        return false;
    }

    if (file.size() > LARGE_FILE_SIZE) {
        int response = QMessageBox::question(this, QString::null,
                            tr("Warning: Are you sure you want to open this large file?"),
                            QMessageBox::Yes | QMessageBox::No);
        if (response == QMessageBox::No)
            return false;
    }

    auto detect = FileDetection::detect(file.read(DETECTION_SIZE), filename);
    setLineEndingMode(detect.lineEndings());

    QTextCodec *codec = Q_NULLPTR;
    if (!textEncoding.isEmpty()) {
        bool ok;
        codec = KCharsets::charsets()->codecForName(textEncoding, ok);
        if (!ok) {
            qDebug(tr("Invalid manually-specified encoding: %s").toLocal8Bit().constData(),
                   textEncoding.toLocal8Bit().constData());
            codec = Q_NULLPTR;
        }
    }
    if (!codec)
        codec = detect.textCodec();
    setEncoding(codec->name());

    file.seek(detect.bomOffset());
    auto decoder = codec->makeDecoder();
    QStringList pieces;
    for ( ;; ) {
        const auto buffer = file.read(DECODE_BLOCK_SIZE);
        if (buffer.size() == 0)
            break;
        pieces << decoder->toUnicode(buffer);
    }

    // Don't let the syntax highlighter hinder us while setting the new content
    m_editor->clear();
    setSyntax(SyntaxTextEdit::nullSyntax());
    m_editor->setPlainText(pieces.join(QString::null));
    m_editor->document()->clearUndoRedoStacks();

    // If libmagic finds a match, it's probably more likely to be correct
    // than the filename match, which is easily fooled
    // (e.g. idle3.6 is not a Troff Mandoc file)
    auto definition = FileDetection::definitionForFileMagic(filename);
    if (!definition.isValid())
        definition = SyntaxTextEdit::syntaxRepo()->definitionForFileName(filename);
    if (definition.isValid())
        setSyntax(definition);

    m_openFilename = filename;
    QFileInfo fi(m_openFilename);
    m_documentTitle = fi.fileName();
    m_undoStack->clear();
    m_undoStack->setClean();
    m_reloadAction->setEnabled(true);
    updateTitle();
    return true;
}

bool QTextPadWindow::isDocumentModified() const
{
    return !m_undoStack->isClean();
}

void QTextPadWindow::addUndoCommand(QUndoCommand *command)
{
    m_undoStack->push(command);
}

void QTextPadWindow::gotoLine(int line, int column)
{
    m_editor->moveCursorTo(line, column);
}

bool QTextPadWindow::promptForSave()
{
    if (isDocumentModified()) {
        int response = QMessageBox::question(this, QString::null,
                            tr("%1 has been modified.  Would you like to save your changes first?")
                            .arg(m_documentTitle), QMessageBox::Yes | QMessageBox::No | QMessageBox::Cancel);
        if (response == QMessageBox::Cancel)
            return false;
        else if (response == QMessageBox::Yes)
            return saveDocument();
    }
    return true;
}

void QTextPadWindow::newDocument()
{
    if (!promptForSave())
        return;

    m_editor->clear();
    m_editor->document()->clearUndoRedoStacks();

    setSyntax(SyntaxTextEdit::nullSyntax());
    setEncoding("UTF-8");
#ifdef _WIN32
    setLineEndingMode(CRLF);
#else
    // OSX uses LF as well, and we don't support building for classic MacOS
    setLineEndingMode(LFOnly);
#endif

    m_openFilename = QString::null;
    m_documentTitle = tr("Untitled");
    m_undoStack->clear();
    m_undoStack->setClean();
    m_reloadAction->setEnabled(false);
    updateTitle();
}

bool QTextPadWindow::saveDocument()
{
    QString path = m_openFilename;
    if (path.isEmpty()) {
        path = QFileDialog::getSaveFileName(this, tr("Save File"));
        if (path.isEmpty())
            return false;
    }
    if (!saveDocumentTo(path))
        return false;

    m_openFilename = path;
    QFileInfo fi(m_openFilename);
    m_documentTitle = fi.fileName();
    m_undoStack->setClean();
    m_editor->document()->clearUndoRedoStacks();
    updateTitle();
    return true;
}

bool QTextPadWindow::saveDocumentAs()
{
    QString path = QFileDialog::getSaveFileName(this, tr("Save File As"));
    if (path.isEmpty())
        return false;
    if (!saveDocumentTo(path))
        return false;

    m_openFilename = path;
    QFileInfo fi(m_openFilename);
    m_documentTitle = fi.fileName();
    m_undoStack->setClean();
    m_editor->document()->clearUndoRedoStacks();
    updateTitle();
    return true;
}

bool QTextPadWindow::saveDocumentCopy()
{
    QString path = QFileDialog::getSaveFileName(this, tr("Save Copy As"));
    if (path.isEmpty())
        return false;
    return saveDocumentTo(path);
}

bool QTextPadWindow::loadDocument()
{
    QString startPath;
    if (!m_openFilename.isEmpty()) {
        QFileInfo fi(m_openFilename);
        startPath = fi.absolutePath();
    }
    QString path = QFileDialog::getOpenFileName(this, tr("Load File"), startPath);
    if (path.isEmpty())
        return false;

    return loadDocumentFrom(path);
}

bool QTextPadWindow::reloadDocument()
{
    if (isDocumentModified()) {
        int response = QMessageBox::question(this, QString::null,
                            tr("%1 has been modified.  Are you sure you want to discard your changes?")
                            .arg(m_documentTitle), QMessageBox::Yes | QMessageBox::No);
        if (response == QMessageBox::No)
            return false;
    }
    return loadDocumentFrom(m_openFilename);
}

void QTextPadWindow::editorContextMenu(const QPoint &pos)
{
    std::unique_ptr<QMenu> menu(new QMenu(m_editor));
    for (auto action : m_editorContextActions)
        menu->addAction(action);

#if 0
    // TODO: Qt provides a control code menu in RTL text mode, but
    // does not provide any public API way to use it without instantiating
    // QPlainTextEdit's default context menu :(
    if (QGuiApplication::styleHints()->useRtlExtensions()) {
        menu->addSeparator();
        // Private API
        auto ctrlCharMenu = new QUnicodeControlCharacterMenu(m_editor, menu);
        menu->addMenu(ctrlCharMenu);
    }
#endif

    menu->exec(m_editor->viewport()->mapToGlobal(pos));
}

void QTextPadWindow::updateCursorPosition()
{
    const QTextCursor cursor = m_editor->textCursor();
    const int column = m_editor->textColumn(cursor.block().text(), cursor.positionInBlock());
    m_positionLabel->setText(QString("Line %1, Col %2")
                             .arg(cursor.blockNumber() + 1)
                             .arg(column + 1));
}

void QTextPadWindow::updateTitle()
{
    QString title = m_documentTitle + QStringLiteral(" \u2013 qtextpad");  // n-dash
    if (isDocumentModified())
        title = QStringLiteral("* ") + title;

    setWindowTitle(title);
}

void QTextPadWindow::nextInsertMode()
{
    setOverwriteMode(!m_editor->overwriteMode());
}

void QTextPadWindow::nextLineEndingMode()
{
    switch (m_lineEndingMode) {
    case CROnly:
        changeLineEndingMode(LFOnly);
        break;
    case LFOnly:
        changeLineEndingMode(CRLF);
        break;
    case CRLF:
        changeLineEndingMode(CROnly);
        break;
    }
}

void QTextPadWindow::updateIndentStatus()
{
    const int tabWidth = m_editor->tabWidth();
    const int indentWidth = m_editor->indentWidth();
    const auto indentMode = m_editor->indentationMode();

    QString description;
    switch (indentMode) {
    case SyntaxTextEdit::IndentSpaces:
        description = tr("Soft Tabs: %1").arg(indentWidth);
        if (tabWidth != indentWidth)
            description += tr(" (%1)").arg(tabWidth);
        break;
    case SyntaxTextEdit::IndentTabs:
        description = tr("Tab Size: %1").arg(tabWidth);
        break;
    case SyntaxTextEdit::IndentMixed:
        description = tr("Mixed Indent: %1").arg(indentWidth);
        if (tabWidth != indentWidth)
            description += tr(" (%1)").arg(tabWidth);
        break;
    default:
        description = tr("INVALID");
        break;
    }

    m_indentButton->setText(description);

    QAction *otherAction = Q_NULLPTR;
    bool haveMatch = false;
    for (auto action : m_tabWidthActions->actions()) {
        if (!action->data().isValid()) {
            otherAction = action;
        } else if (tabWidth == action->data().toInt()) {
            action->setChecked(true);
            haveMatch = true;
        }
    }
    if (!haveMatch && otherAction)
        otherAction->setChecked(true);

    otherAction = Q_NULLPTR;
    haveMatch = false;
    for (auto action : m_indentWidthActions->actions()) {
        if (!action->data().isValid()) {
            otherAction = action;
        } else if (indentWidth == action->data().toInt()) {
            action->setChecked(true);
            haveMatch = true;
        }
    }
    if (!haveMatch && otherAction)
        otherAction->setChecked(true);

    for (auto action : m_indentModeActions->actions()) {
        const auto actionMode = static_cast<SyntaxTextEdit::IndentationMode>(action->data().toInt());
        if (indentMode == actionMode)
            action->setChecked(true);
    }
}

void QTextPadWindow::chooseEditorFont()
{
    bool ok = false;
    QFont newFont = QFontDialog::getFont(&ok, m_editor->defaultFont(), this,
                                         tr("Default Editor Font"));
    if (ok)
        m_editor->setDefaultFont(newFont);
}

void QTextPadWindow::changeEncoding(const QString &encoding)
{
    if (m_openFilename.isEmpty()) {
        // Don't save changes in the undo stack if we are creating a new file
        setEncoding(encoding);
    } else {
        auto command = new ChangeEncodingCommand(this, encoding);
        addUndoCommand(command);
    }
}

void QTextPadWindow::changeLineEndingMode(LineEndingMode mode)
{
    if (m_openFilename.isEmpty()) {
        // Don't save changes in the undo stack if we are creating a new file
        setLineEndingMode(mode);
    } else {
        auto command = new ChangeLineEndingCommand(this, mode);
        addUndoCommand(command);
    }
}

void QTextPadWindow::promptTabSettings()
{
    auto dialog = new IndentSettingsDialog(this);
    dialog->loadSettings(m_editor);
    if (dialog->exec() == QDialog::Accepted) {
        dialog->applySettings(m_editor);
        updateIndentStatus();
    }
}

void QTextPadWindow::closeEvent(QCloseEvent *e)
{
    if (!promptForSave()) {
        e->ignore();
        return;
    }

    QTextPadSettings settings;
    settings.setWindowSize(size());
    e->accept();
}

void QTextPadWindow::populateRecentFiles()
{
    QStringList recentFiles = QTextPadSettings().recentFiles();
    for (const auto &filename : recentFiles) {
        QFileInfo info(filename);
        auto recentFileAction = m_recentFiles->addAction(info.fileName());
        recentFileAction->setData(info.absoluteFilePath());
        //connect(recentFileAction, &QAction::triggered, ...);
    }

    (void) m_recentFiles->addSeparator();
    auto clearListAction = m_recentFiles->addAction(tr("Clear List"));
    connect(clearListAction, &QAction::triggered, [this]() {
        m_recentFiles->clear();
        populateRecentFiles();
    });
}

void QTextPadWindow::populateSyntaxMenu()
{
    m_syntaxActions = new QActionGroup(this);

    auto plainText = m_syntaxMenu->addAction(tr("Plain Text"));
    plainText->setCheckable(true);
    plainText->setActionGroup(m_syntaxActions);
    plainText->setData(QVariant::fromValue(SyntaxTextEdit::nullSyntax()));
    connect(plainText, &QAction::triggered, [this]() {
        setSyntax(SyntaxTextEdit::nullSyntax());
    });

    KSyntaxHighlighting::Repository *syntaxRepo = SyntaxTextEdit::syntaxRepo();
    const auto syntaxDefs = syntaxRepo->definitions();
    QMap<QString, QMenu *> groupMenus;
    for (const auto def : syntaxDefs) {
        if (def.isHidden())
            continue;

        QMenu *parentMenu = groupMenus.value(def.translatedSection(), Q_NULLPTR);
        if (!parentMenu) {
            parentMenu = m_syntaxMenu->addMenu(def.translatedSection());
            groupMenus[def.translatedSection()] = parentMenu;
        }
        auto item = parentMenu->addAction(def.translatedName());
        item->setCheckable(true);
        item->setActionGroup(m_syntaxActions);
        item->setData(QVariant::fromValue(def));
        connect(item, &QAction::triggered, [this, def]() { setSyntax(def); });
    }
}

void QTextPadWindow::populateThemeMenu()
{
    m_themeActions = new QActionGroup(this);

    KSyntaxHighlighting::Repository *syntaxRepo = SyntaxTextEdit::syntaxRepo();
    const auto themeDefs = syntaxRepo->themes();
    for (const auto theme : themeDefs) {
        auto item = m_themeMenu->addAction(theme.translatedName());
        item->setCheckable(true);
        item->setActionGroup(m_themeActions);
        item->setData(QVariant::fromValue(theme));
        connect(item, &QAction::triggered, [this, theme]() { setTheme(theme); });
    }
}

void QTextPadWindow::populateEncodingMenu()
{
    m_encodingActions = new QActionGroup(this);
    auto charsets = KCharsets::charsets();
    auto encodingScripts = charsets->encodingsByScript();

    // Sort the lists by script/region name
    std::sort(encodingScripts.begin(), encodingScripts.end(),
              [](const QStringList &left, const QStringList &right)
    {
        return left.first() < right.first();
    });

    for (const auto encodingList : encodingScripts) {
        QMenu *parentMenu = m_encodingMenu->addMenu(encodingList.first());
        for (int i = 1; i < encodingList.size(); ++i) {
            QString codecName = encodingList.at(i);
            auto item = parentMenu->addAction(codecName);
            item->setCheckable(true);
            item->setActionGroup(m_encodingActions);
            item->setData(codecName);
            connect(item, &QAction::triggered, [this, codecName]() {
                changeEncoding(codecName);
            });
        }
    }
}

void QTextPadWindow::populateIndentButtonMenu()
{
    auto indentMenu = new QMenu(this);

    // Qt 5.1 has QMenu::addSection, but that results in a style hint that
    // is completely ignored by some platforms, including both GTK and Windows
    auto headerLabel = new QLabel(tr("Tab Width"), this);
    headerLabel->setAlignment(Qt::AlignCenter);
    headerLabel->setContentsMargins(4, 4, 4, 0);
    auto headerAction = new QWidgetAction(this);
    headerAction->setDefaultWidget(headerLabel);
    indentMenu->addAction(headerAction);
    (void) indentMenu->addSeparator();

    m_tabWidthActions = new QActionGroup(this);
    auto tabWidth8Action = indentMenu->addAction(QStringLiteral("8"));
    tabWidth8Action->setCheckable(true);
    tabWidth8Action->setActionGroup(m_tabWidthActions);
    tabWidth8Action->setData(8);
    auto tabWidth4Action = indentMenu->addAction(QStringLiteral("4"));
    tabWidth4Action->setCheckable(true);
    tabWidth4Action->setActionGroup(m_tabWidthActions);
    tabWidth4Action->setData(4);
    auto tabWidth2Action = indentMenu->addAction(QStringLiteral("2"));
    tabWidth2Action->setCheckable(true);
    tabWidth2Action->setActionGroup(m_tabWidthActions);
    tabWidth2Action->setData(2);
    auto tabWidthOtherAction = indentMenu->addAction(tr("Other..."));
    tabWidthOtherAction->setCheckable(true);
    tabWidthOtherAction->setActionGroup(m_tabWidthActions);

    for (auto action : m_tabWidthActions->actions()) {
        if (action == tabWidthOtherAction)
            continue;
        connect(action, &QAction::triggered, [this, action]() {
            m_editor->setTabWidth(action->data().toInt());
            updateIndentStatus();
        });
    }
    connect(tabWidthOtherAction, &QAction::triggered,
            this, &QTextPadWindow::promptTabSettings);

    headerLabel = new QLabel(tr("Indentation Width"), this);
    headerLabel->setAlignment(Qt::AlignCenter);
    headerLabel->setContentsMargins(4, 4, 4, 0);
    headerAction = new QWidgetAction(this);
    headerAction->setDefaultWidget(headerLabel);
    indentMenu->addAction(headerAction);
    (void) indentMenu->addSeparator();

    m_indentWidthActions = new QActionGroup(this);
    auto indentWidth8Action = indentMenu->addAction(QStringLiteral("8"));
    indentWidth8Action->setCheckable(true);
    indentWidth8Action->setActionGroup(m_indentWidthActions);
    indentWidth8Action->setData(8);
    auto indentWidth4Action = indentMenu->addAction(QStringLiteral("4"));
    indentWidth4Action->setCheckable(true);
    indentWidth4Action->setActionGroup(m_indentWidthActions);
    indentWidth4Action->setData(4);
    auto indentWidth2Action = indentMenu->addAction(QStringLiteral("2"));
    indentWidth2Action->setCheckable(true);
    indentWidth2Action->setActionGroup(m_indentWidthActions);
    indentWidth2Action->setData(2);
    auto indentWidthOtherAction = indentMenu->addAction(tr("Other..."));
    indentWidthOtherAction->setCheckable(true);
    indentWidthOtherAction->setActionGroup(m_indentWidthActions);

    for (auto action : m_indentWidthActions->actions()) {
        if (action == indentWidthOtherAction)
            continue;
        connect(action, &QAction::triggered, [this, action]() {
            m_editor->setIndentWidth(action->data().toInt());
            updateIndentStatus();
        });
    }
    connect(indentWidthOtherAction, &QAction::triggered,
            this, &QTextPadWindow::promptTabSettings);

    headerLabel = new QLabel(tr("Indentation Mode"), this);
    headerLabel->setAlignment(Qt::AlignCenter);
    headerLabel->setContentsMargins(4, 4, 4, 0);
    headerAction = new QWidgetAction(this);
    headerAction->setDefaultWidget(headerLabel);
    indentMenu->addAction(headerAction);
    (void) indentMenu->addSeparator();

    m_indentModeActions = new QActionGroup(this);
    auto indentSpacesAction = indentMenu->addAction(tr("&Spaces"));
    indentSpacesAction->setCheckable(true);
    indentSpacesAction->setActionGroup(m_indentModeActions);
    indentSpacesAction->setData(static_cast<int>(SyntaxTextEdit::IndentSpaces));
    auto indentTabsAction = indentMenu->addAction(tr("&Tabs"));
    indentTabsAction->setCheckable(true);
    indentTabsAction->setActionGroup(m_indentModeActions);
    indentTabsAction->setData(static_cast<int>(SyntaxTextEdit::IndentTabs));
    auto indentMixedAction = indentMenu->addAction(tr("&Mixed"));
    indentMixedAction->setCheckable(true);
    indentMixedAction->setActionGroup(m_indentModeActions);
    indentMixedAction->setData(static_cast<int>(SyntaxTextEdit::IndentMixed));

    for (auto action : m_indentModeActions->actions()) {
        connect(action, &QAction::triggered, [this, action]() {
            const auto mode = static_cast<SyntaxTextEdit::IndentationMode>(action->data().toInt());
            m_editor->setIndentationMode(mode);
            updateIndentStatus();
        });
    }

    (void) indentMenu->addSeparator();
    // Make a copy since we don't want to show the key shortcut here
    auto autoIndentAction = indentMenu->addAction(m_autoIndentAction->text());
    autoIndentAction->setCheckable(true);
    autoIndentAction->setChecked(m_autoIndentAction->isChecked());
    connect(m_autoIndentAction, &QAction::toggled,
            autoIndentAction, &QAction::setChecked);
    connect(autoIndentAction, &QAction::triggered,
            m_autoIndentAction, &QAction::trigger);

    m_indentButton->setMenu(indentMenu);
    updateIndentStatus();
}
