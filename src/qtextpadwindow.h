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

#ifndef QTEXTPAD_WINDOW_H
#define QTEXTPAD_WINDOW_H

#include <QMainWindow>
#include <QDateTime>
#include <QLocale>

#include "filetypeinfo.h"

class SyntaxTextEdit;
class SearchWidget;
class ActivationLabel;

class QToolButton;
class QMenu;
class QActionGroup;
class QUndoStack;
class QUndoCommand;
class QFileSystemWatcher;

namespace KSyntaxHighlighting
{
    class Definition;
    class Theme;
}

class QTextPadWindow : public QMainWindow
{
    Q_OBJECT

public:
    explicit QTextPadWindow(QWidget *parent = Q_NULLPTR);

    SyntaxTextEdit *editor() { return m_editor; }

    void setSyntax(const KSyntaxHighlighting::Definition &syntax);
    void setEditorTheme(const KSyntaxHighlighting::Theme &theme);
    void setDefaultEditorTheme();
    void setEncoding(const QString &codecName);

    QString textEncoding() const { return m_textEncoding; }

    bool utfBOM() const;
    void setUtfBOM(bool bom);

    void setOverwriteMode(bool overwrite);
    void setAutoIndent(bool ai);

    void setLineEndingMode(FileTypeInfo::LineEndingType mode);
    FileTypeInfo::LineEndingType lineEndingMode() const { return m_lineEndingMode; }

    bool saveDocumentTo(const QString &filename);
    bool loadDocumentFrom(const QString &filename,
                          const QString &textEncoding = QString());
    bool isDocumentModified() const;
    bool documentExists() const;

    void gotoLine(int line, int column = 0);

public slots:
    void checkForModifications();
    bool promptForSave();
    bool promptForDiscard();
    void newDocument();
    void resetEditor();
    bool saveDocument();
    bool saveDocumentAs();
    bool saveDocumentCopy();
    bool loadDocument();
    bool reloadDocument();
    void reloadDocumentEncoding(const QString &textEncoding);
    void printDocument();
    void printPreviewDocument();

    void editorContextMenu(const QPoint& pos);
    void updateCursorPosition();
    void nextInsertMode();
    void nextLineEndingMode();
    void updateIndentStatus();
    void chooseEditorFont();
    void promptIndentSettings();
    void promptLongLineWidth();
    void navigateToLine();
    void toggleFilePath(bool show);

    void insertDateTime(QLocale::FormatType type);
    void upcaseSelection();
    void downcaseSelection();
    void joinLines();

    void showAbout();
    void toggleFullScreen(bool fullScreen);
    void showSearchBar(bool show);

    // User-triggered actions that store commands in the Undo stack
    void changeEncoding(const QString &encoding);
    void changeLineEndingMode(FileTypeInfo::LineEndingType mode);
    void changeUtfBOM();

protected:
    void resizeEvent(QResizeEvent *event) Q_DECL_OVERRIDE;
    void showEvent(QShowEvent *event) Q_DECL_OVERRIDE;
    void closeEvent(QCloseEvent *event) Q_DECL_OVERRIDE;

    bool eventFilter(QObject *, QEvent *event) Q_DECL_OVERRIDE;

private:
    enum FileState
    {
        FS_New = 0x01,
        FS_OutOfDate = 0x02,
    };

    SyntaxTextEdit *m_editor;
    SearchWidget *m_searchWidget;
    QString m_textEncoding;

    QString m_openFilename;
    unsigned int m_fileState;
    QFileSystemWatcher *m_fileWatcher;
    QDateTime m_cachedModTime;
    void setOpenFilename(const QString &filename);

    QToolBar *m_toolBar;
    QMenu *m_recentFiles;
    QMenu *m_themeMenu;
    QMenu *m_syntaxMenu;
    QMenu *m_setEncodingMenu;
    bool m_showFilePath;

    // QAction caches
    QAction *m_reloadAction;
    QAction *m_overwriteModeAction;
    QAction *m_utfBOMAction;
    QAction *m_autoIndentAction;
    QAction *m_fullScreenAction;
    QActionGroup *m_themeActions;
    QAction *m_defaultThemeAction;
    QActionGroup *m_syntaxActions;
    QActionGroup *m_setEncodingActions;
    QActionGroup *m_lineEndingActions;
    QActionGroup *m_tabWidthActions;
    QActionGroup *m_indentWidthActions;
    QActionGroup *m_indentModeActions;
    QList<QAction *> m_editorContextActions;

    ActivationLabel *m_positionLabel;
    ActivationLabel *m_crlfLabel;
    ActivationLabel *m_insertLabel;
    QToolButton *m_indentButton;
    QToolButton *m_encodingButton;
    QToolButton *m_syntaxButton;
    FileTypeInfo::LineEndingType m_lineEndingMode;

    // Custom Undo Stack for adding non-editor undo items
    QUndoStack *m_undoStack;

    QString documentTitle();
    void updateTitle();
    void populateRecentFiles();
    void populateThemeMenu();
    void populateSyntaxMenu();
    void populateEncodingMenu();
    void populateIndentButtonMenu();
};

#endif // QTEXTPAD_WINDOW_H
