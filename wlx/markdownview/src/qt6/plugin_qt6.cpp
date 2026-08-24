#include <QFile>
#include <QFileInfo>
#include <QSettings>
#include <QTextBrowser>
#include <QTextDocument>
#include <QScrollBar>
#include <QApplication>
#include <QClipboard>
#include <QGuiApplication>
#include <QPalette>
#include <QFileSystemWatcher>
#include <QTimer>
#include <QMenu>
#include <QAction>
#include <QActionGroup>
#include <QContextMenuEvent>
#include <QWheelEvent>
#include <QVBoxLayout>
#include <QUrl>
#include <dlfcn.h>
#include <cmath>

#include "wlxplugin.h"
#include "../core/markdown_engine.h"

#define PLUGNAME "markdownview"

static bool g_autoReloadEnabled = true;
static QString g_mode = QStringLiteral("system"); // "system", "dark", "light"
static QString g_themeFilePath;
static QString g_configPath;

static bool isSystemDark() {
    QPalette pal = QGuiApplication::palette();
    return pal.color(QPalette::Window).value() < 128;
}

static bool resolveDarkMode() {
    if (g_mode == QStringLiteral("dark")) return true;
    if (g_mode == QStringLiteral("light")) return false;
    return isSystemDark();
}

static void saveSettings() {
    if (g_configPath.isEmpty()) return;
    QSettings settings(g_configPath, QSettings::IniFormat);
    settings.setValue(PLUGNAME "/theme_file_path", g_themeFilePath);
    settings.setValue(PLUGNAME "/mode", g_mode);
    settings.setValue(PLUGNAME "/auto_reload", g_autoReloadEnabled);
}

class MarkdownViewerWidget : public QTextBrowser {
private:
    QString m_filePath;
    QFileSystemWatcher m_watcher;
    QTimer m_debounceTimer;
    int m_zoomLevel = 0;

public:
    MarkdownViewerWidget(QWidget* parent = nullptr) : QTextBrowser(parent) {
        setOpenExternalLinks(true);
        setOpenLinks(true);

        m_debounceTimer.setSingleShot(true);
        m_debounceTimer.setInterval(200);

        connect(&m_debounceTimer, &QTimer::timeout, this, &MarkdownViewerWidget::reloadContent);
        connect(&m_watcher, &QFileSystemWatcher::fileChanged, this, [this](const QString&) {
            if (g_autoReloadEnabled) {
                m_debounceTimer.start();
            }
        });
    }

    void loadFile(const QString& filePath) {
        m_filePath = filePath;

        if (!m_watcher.files().isEmpty()) {
            m_watcher.removePaths(m_watcher.files());
        }
        if (QFile::exists(filePath)) {
            m_watcher.addPath(filePath);
        }

        reloadContent();
    }

    void reloadContent() {
        if (m_filePath.isEmpty() || !QFile::exists(m_filePath))
            return;

        bool activeDarkMode = resolveDarkMode();
        std::string html = MarkdownEngine::renderFileToHtml(
            m_filePath.toStdString(),
            activeDarkMode,
            g_themeFilePath.toStdString()
        );
        QString autoResolvedCss = QString::fromStdString(MarkdownEngine::getLastAutoResolvedCssPath());
        if (!autoResolvedCss.isEmpty() && autoResolvedCss != g_themeFilePath) {
            g_themeFilePath = autoResolvedCss;
            saveSettings();
        }

        // QTextBrowser paints its own viewport background from
        // QPalette::Base BEFORE the document is drawn on top -- a `body {
        // background-color: ... }` CSS rule only colors the document's
        // root frame, not the surrounding widget/viewport, so without this
        // any area the document doesn't fully cover (margins, a
        // shorter-than-viewport document) shows through as whatever the
        // ambient/system palette's Base color is. On a dark system theme
        // that made the "light" markdown theme look mostly dark outside
        // the actual text blocks. Match the same body background colors
        // DEFAULT_CSS uses so the two stay in sync.
        //
        // QPalette::Text/WindowText matter too, not just Base/Window --
        // QTextDocument's CSS engine doesn't reliably cascade a
        // class-scoped `body.theme-light { color: ... }` rule down to
        // every paragraph the way a real browser would, so any text that
        // doesn't inherit it falls back to the palette's default text
        // color. On a dark system theme that default is light/white,
        // which on the light markdown theme's white background rendered
        // as invisible text -- visible only once selected, since selection
        // painting uses a different color pair.
        QPalette pal = palette();
        QColor bg = activeDarkMode ? QColor("#0d1117") : QColor("#ffffff");
        QColor fg = activeDarkMode ? QColor("#c9d1d9") : QColor("#24292e");
        pal.setColor(QPalette::Base, bg);
        pal.setColor(QPalette::Window, bg);
        pal.setColor(QPalette::Text, fg);
        pal.setColor(QPalette::WindowText, fg);
        setPalette(pal);

        int currentScrollX = horizontalScrollBar() ? horizontalScrollBar()->value() : 0;
        int currentScrollY = verticalScrollBar() ? verticalScrollBar()->value() : 0;

        setHtml(QString::fromStdString(html));
        document()->setBaseUrl(QUrl::fromLocalFile(m_filePath).adjusted(QUrl::RemoveFilename));

        if (horizontalScrollBar()) horizontalScrollBar()->setValue(currentScrollX);
        if (verticalScrollBar()) verticalScrollBar()->setValue(currentScrollY);

        if (QFile::exists(m_filePath) && !m_watcher.files().contains(m_filePath)) {
            m_watcher.addPath(m_filePath);
        }
    }

    void copySelection() {
        if (textCursor().hasSelection()) {
            copy();
        }
    }

protected:
    void wheelEvent(QWheelEvent* event) override {
        if (event->modifiers() & Qt::ControlModifier) {
            if (event->angleDelta().y() > 0) {
                zoomIn(1);
                m_zoomLevel++;
            } else if (event->angleDelta().y() < 0) {
                zoomOut(1);
                m_zoomLevel--;
            }
            event->accept();
        } else {
            QTextBrowser::wheelEvent(event);
        }
    }

    void contextMenuEvent(QContextMenuEvent* event) override {
        QMenu menu(this);

        QAction* copyAction = menu.addAction(tr("Copy Text"));
        copyAction->setEnabled(textCursor().hasSelection());
        connect(copyAction, &QAction::triggered, this, &MarkdownViewerWidget::copySelection);

        QAction* selectAllAction = menu.addAction(tr("Select All"));
        connect(selectAllAction, &QAction::triggered, this, &MarkdownViewerWidget::selectAll);

        menu.addSeparator();

        QAction* reloadAction = menu.addAction(tr("Reload Document"));
        connect(reloadAction, &QAction::triggered, this, &MarkdownViewerWidget::reloadContent);

        QAction* toggleAutoAction = menu.addAction(tr("Auto-Reload on Save"));
        toggleAutoAction->setCheckable(true);
        toggleAutoAction->setChecked(g_autoReloadEnabled);
        connect(toggleAutoAction, &QAction::triggered, this, [](bool checked) {
            g_autoReloadEnabled = checked;
            saveSettings();
        });

        menu.addSeparator();

        QMenu* modeMenu = menu.addMenu(tr("Theme Mode"));
        QActionGroup* modeGroup = new QActionGroup(modeMenu);
        modeGroup->setExclusive(true);

        QAction* modeSystem = modeMenu->addAction(tr("System"));
        modeSystem->setCheckable(true);
        modeSystem->setChecked(g_mode == QStringLiteral("system"));
        modeGroup->addAction(modeSystem);
        connect(modeSystem, &QAction::triggered, this, [this]() {
            g_mode = QStringLiteral("system");
            saveSettings();
            reloadContent();
        });

        QAction* modeDark = modeMenu->addAction(tr("Dark"));
        modeDark->setCheckable(true);
        modeDark->setChecked(g_mode == QStringLiteral("dark"));
        modeGroup->addAction(modeDark);
        connect(modeDark, &QAction::triggered, this, [this]() {
            g_mode = QStringLiteral("dark");
            saveSettings();
            reloadContent();
        });

        QAction* modeLight = modeMenu->addAction(tr("Light"));
        modeLight->setCheckable(true);
        modeLight->setChecked(g_mode == QStringLiteral("light"));
        modeGroup->addAction(modeLight);
        connect(modeLight, &QAction::triggered, this, [this]() {
            g_mode = QStringLiteral("light");
            saveSettings();
            reloadContent();
        });

        menu.exec(event->globalPos());
    }
};

extern "C" {

HWND DCPCALL ListLoad(HWND ParentWin, char* FileToLoad, int ShowFlags)
{
    if (!QApplication::instance())
        return nullptr;

    QFileInfo fi(FileToLoad);
    QString ext = fi.suffix().toLower();
    if (ext != "md" && ext != "markdown" && ext != "mdown" && ext != "mkd") {
        return nullptr;
    }

    MarkdownViewerWidget* viewer = new MarkdownViewerWidget((QWidget*)ParentWin);
    viewer->loadFile(QString::fromUtf8(FileToLoad));
    viewer->show();

    return (HWND)viewer;
}

void DCPCALL ListCloseWindow(HWND ListWin)
{
    MarkdownViewerWidget* viewer = (MarkdownViewerWidget*)ListWin;
    if (viewer) {
        delete viewer;
    }
}

int DCPCALL ListSendCommand(HWND ListWin, int Command, int Parameter)
{
    MarkdownViewerWidget* viewer = (MarkdownViewerWidget*)ListWin;
    if (!viewer) return LISTPLUGIN_ERROR;

    switch (Command) {
    case lc_copy:
        viewer->copySelection();
        return LISTPLUGIN_OK;
    case lc_selectall:
        viewer->selectAll();
        return LISTPLUGIN_OK;
    case lc_newparams:
        viewer->reloadContent();
        return LISTPLUGIN_OK;
    default:
        return LISTPLUGIN_ERROR;
    }
}

int DCPCALL ListSearchText(HWND ListWin, char* SearchString, int SearchParameter)
{
    MarkdownViewerWidget* viewer = (MarkdownViewerWidget*)ListWin;
    if (!viewer || !SearchString) return LISTPLUGIN_ERROR;

    QTextDocument::FindFlags flags;
    if (SearchParameter & lcs_matchcase)
        flags |= QTextDocument::FindCaseSensitively;
    if (SearchParameter & lcs_backwards)
        flags |= QTextDocument::FindBackward;
    if (SearchParameter & lcs_wholewords)
        flags |= QTextDocument::FindWholeWords;

    bool found = viewer->find(QString::fromUtf8(SearchString), flags);
    return found ? LISTPLUGIN_OK : LISTPLUGIN_ERROR;
}

void DCPCALL ListGetDetectString(char* DetectString, int maxlen)
{
    snprintf(DetectString, maxlen - 1, "(EXT=\"MD\" | EXT=\"MARKDOWN\" | EXT=\"MDOWN\" | EXT=\"MKD\") & SIZE<30000000");
}

void DCPCALL ListSetDefaultParams(ListDefaultParamStruct* dps)
{
    if (!dps) return;
    QFileInfo defini(QString::fromUtf8(dps->DefaultIniName));
    g_configPath = defini.absolutePath() + "/markdownview.ini";
    MarkdownEngine::setPluginConfigDir(defini.absolutePath().toStdString());
    QSettings settings(g_configPath, QSettings::IniFormat);

    if (!settings.contains(PLUGNAME "/theme_file_path"))
        settings.setValue(PLUGNAME "/theme_file_path", g_themeFilePath);
    else
        g_themeFilePath = settings.value(PLUGNAME "/theme_file_path").toString();

    if (!settings.contains(PLUGNAME "/mode"))
        settings.setValue(PLUGNAME "/mode", g_mode);
    else
        g_mode = settings.value(PLUGNAME "/mode").toString().toLower();

    if (!settings.contains(PLUGNAME "/auto_reload"))
        settings.setValue(PLUGNAME "/auto_reload", g_autoReloadEnabled);
    else
        g_autoReloadEnabled = settings.value(PLUGNAME "/auto_reload").toBool();
}

} // extern "C"
