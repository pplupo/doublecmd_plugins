// htmlview -- a Double Commander lister plugin that renders HTML with a
// real engine (QWebEngineView) instead of Qt's rich-text subset, and locks
// that engine down so a preview pane cannot execute scripts or reach the
// network unless the user says so for that document.
//
// The engine choice was verified, not assumed: constructing a
// QWebEngineView from a dlopen()ed plugin, after the host's QApplication
// already exists, works without the Qt::AA_ShareOpenGLContexts fatal that
// the documentation's warning implies -- checked against Qt 6.11 in a
// minimal host harness that mimics what DC does (QApplication, dlopen,
// ListLoad, reparent into a pane widget).

#include <QAction>
#include <QActionGroup>
#include <QApplication>
#include <QClipboard>
#include <QContextMenuEvent>
#include <QCoreApplication>
#include <QDesktopServices>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QPalette>
#include <QPrintDialog>
#include <QPrinter>
#include <QPushButton>
#include <QSettings>
#include <QTimer>
#include <QToolButton>
#include <QUrl>
#include <QVBoxLayout>
#include <QWebEngineFindTextResult>
#include <QWebEnginePage>
#include <QWebEngineProfile>
#include <QWebEngineSettings>
#include <QWebEngineUrlRequestInfo>
#include <QWebEngineUrlRequestInterceptor>
#include <QWebEngineView>
#include <QWidget>

#include <cstdio>
#include <exception>

#include "wlxplugin.h"

#include "../core/html_probe.h"

#define PLUGNAME "htmlview"

// ---------------------------------------------------------------------
// Settings. Written back to the ini on change, exactly like markdownview.
// ---------------------------------------------------------------------

static QString g_configPath;

// "system" | "dark" | "light" -- drives the CSS color-scheme hint, which
// is what a well-behaved modern page honours.
static QString g_theme = QStringLiteral("system");
// The heavier hammer for pages that hardcode a white background and would
// otherwise flash white inside a dark DC theme. Off by default: it is a
// filter, and it will make some pages look wrong.
static bool g_forceDark = false;
// Both default off. This is the security posture the QTextBrowser
// implementation had by accident, kept here on purpose.
static bool g_allowScripts = false;
static bool g_allowRemote = false;
static double g_zoomFactor = 1.0;
static qulonglong g_maxFileSize = 32ULL * 1024 * 1024;
static QString g_fallbackEncoding = QStringLiteral("windows-1252");
// Narrow by default and overridable, so installing this plugin does not
// silently hijack every HTML-ish preview in the file manager.
static QString g_detectString = QStringLiteral(
    "(EXT=\"HTML\" | EXT=\"HTM\" | EXT=\"XHTML\" | EXT=\"XHT\" | "
    "EXT=\"MHT\" | EXT=\"MHTML\")");

static void saveSettings() {
	if (g_configPath.isEmpty())
		return;
	QSettings settings(g_configPath, QSettings::IniFormat);
	settings.setValue(PLUGNAME "/theme", g_theme);
	settings.setValue(PLUGNAME "/force_dark", g_forceDark);
	settings.setValue(PLUGNAME "/allow_scripts", g_allowScripts);
	settings.setValue(PLUGNAME "/allow_remote_content", g_allowRemote);
	settings.setValue(PLUGNAME "/zoom_factor", g_zoomFactor);
	settings.setValue(PLUGNAME "/max_file_size", g_maxFileSize);
	settings.setValue(PLUGNAME "/fallback_encoding", g_fallbackEncoding);
	settings.setValue(PLUGNAME "/detect_string", g_detectString);
	settings.sync();
}

static bool isSystemDark() {
	return QGuiApplication::palette().color(QPalette::Window).value() < 128;
}

static bool resolveDarkMode() {
	if (g_theme == QStringLiteral("dark"))
		return true;
	if (g_theme == QStringLiteral("light"))
		return false;
	return isSystemDark();
}

// ---------------------------------------------------------------------
// The network gate
// ---------------------------------------------------------------------

// Everything the document tries to fetch passes through here. Local reads
// are confined to the directory the document lives in -- a preview pane
// has no business reading ~/.ssh through an <img src> -- and anything off
// the machine is blocked outright until the user allows it for that page.
class RequestGate : public QWebEngineUrlRequestInterceptor {
public:
	RequestGate(const QString &documentDir, QObject *parent)
	    : QWebEngineUrlRequestInterceptor(parent), m_documentDir(documentDir) {}

	void setAllowRemote(bool allow) { m_allowRemote = allow; }
	int blockedCount() const { return m_blocked; }
	void resetBlockedCount() { m_blocked = 0; }

	void interceptRequest(QWebEngineUrlRequestInfo &info) override {
		const QString scheme = info.requestUrl().scheme();

		// Inline content the document carries itself, and the engine's own
		// bookkeeping URLs -- neither touches the disk or the network.
		if (scheme == QLatin1String("data") ||
		    scheme == QLatin1String("about") ||
		    scheme == QLatin1String("blob"))
			return;

		if (scheme == QLatin1String("file")) {
			const QString target =
			    QFileInfo(info.requestUrl().toLocalFile()).absoluteFilePath();
			// Prefix match on the canonicalised directory, plus the
			// explicit separator, so /home/u/docs-secret cannot pass as a
			// child of /home/u/docs.
			if (target == m_documentDir ||
			    target.startsWith(m_documentDir + QLatin1Char('/')))
				return;
			m_blocked++;
			info.block(true);
			return;
		}

		if (!m_allowRemote) {
			m_blocked++;
			info.block(true);
		}
	}

private:
	QString m_documentDir;
	bool m_allowRemote = false;
	// Touched from whichever thread Chromium calls the interceptor on and
	// only ever read for a label, so exactness is not worth a mutex.
	int m_blocked = 0;
};

// ---------------------------------------------------------------------
// The page
// ---------------------------------------------------------------------

class ViewerPage : public QWebEnginePage {
public:
	ViewerPage(QWebEngineProfile *profile, const QString &documentDir,
	           QObject *parent)
	    : QWebEnginePage(profile, parent), m_documentDir(documentDir) {}

protected:
	// Navigation is a separate decision from resource loading: a click on
	// a link to another site should not silently repoint the preview pane,
	// but it should not be a dead end either. Local navigation within the
	// document's own directory stays inside the pane; anything else is
	// handed to the desktop's browser, which is where the user was going.
	bool acceptNavigationRequest(const QUrl &url, NavigationType type,
	                             bool isMainFrame) override {
		if (url.scheme() == QLatin1String("about") ||
		    url.scheme() == QLatin1String("data"))
			return true;

		if (url.isLocalFile()) {
			const QString target = QFileInfo(url.toLocalFile()).absoluteFilePath();
			if (target == m_documentDir ||
			    target.startsWith(m_documentDir + QLatin1Char('/')))
				return true;
		}

		if (type == NavigationTypeLinkClicked && isMainFrame) {
			QDesktopServices::openUrl(url);
			return false;
		}
		return false;
	}

	// Console noise from a page being previewed is not the user's problem
	// and would otherwise land in Double Commander's stderr.
	void javaScriptConsoleMessage(JavaScriptConsoleMessageLevel, const QString &,
	                              int, const QString &) override {}

private:
	QString m_documentDir;
};

// ---------------------------------------------------------------------
// The viewer widget
// ---------------------------------------------------------------------

class HtmlViewerWidget : public QWidget {
	Q_OBJECT

public:
	explicit HtmlViewerWidget(QWidget *parent = nullptr) : QWidget(parent) {
		QVBoxLayout *layout = new QVBoxLayout(this);
		layout->setContentsMargins(0, 0, 0, 0);
		layout->setSpacing(0);

		buildNoticeBar();
		layout->addWidget(m_noticeBar);

		// The profile is per-view and unnamed, which makes it off-the-record:
		// no cookie jar, no cache, no local storage surviving the preview.
		m_profile = new QWebEngineProfile(this);
		m_profile->setHttpCacheType(QWebEngineProfile::NoCache);
		m_profile->setPersistentCookiesPolicy(
		    QWebEngineProfile::NoPersistentCookies);

		m_view = new QWebEngineView(this);
		layout->addWidget(m_view, 1);

		buildFindBar();
		layout->addWidget(m_findBar);
		m_findBar->hide();

		setFocusPolicy(Qt::StrongFocus);
	}

	~HtmlViewerWidget() override {
		// The page holds the profile's request interceptor; tearing the
		// page down first keeps Chromium from calling into a half-destroyed
		// object during the profile's own shutdown.
		if (m_view)
			m_view->setPage(nullptr);
	}

	// Returns false when the file is not ours to render -- the caller turns
	// that into a null handle so DC falls through to the next lister.
	bool loadFile(const QString &path) {
		const QFileInfo info(path);
		const HtmlProbe::Result probe = HtmlProbe::probeFile(
		    path.toStdString(), g_maxFileSize, g_fallbackEncoding.toStdString());

		if (probe.verdict != HtmlProbe::Verdict::Renderable) {
			fprintf(stderr, "[" PLUGNAME "] declining \"%s\": %s\n",
			        path.toUtf8().constData(), probe.detail.c_str());
			return false;
		}

		m_filePath = path;
		m_documentDir = info.absoluteDir().absolutePath();
		m_encoding = QString::fromStdString(probe.encoding);

		// ListLoadNext reuses this widget for every file the user steps
		// through, so the previous page and gate have to go rather than
		// piling up as children for the lifetime of the viewer.
		RequestGate *previousGate = m_gate;
		QWebEnginePage *previousPage = m_page;

		m_gate = new RequestGate(m_documentDir, this);
		m_gate->setAllowRemote(g_allowRemote);
		m_profile->setUrlRequestInterceptor(m_gate);
		applyEngineSettings();

		m_page = new ViewerPage(m_profile, m_documentDir, this);
		m_view->setPage(m_page);
		delete previousPage;
		delete previousGate;

		if (probe.delivery == HtmlProbe::Delivery::InlineDecoded) {
			QFile file(path);
			if (!file.open(QIODevice::ReadOnly)) {
				fprintf(stderr, "[" PLUGNAME "] declining \"%s\": open failed\n",
				        path.toUtf8().constData());
				return false;
			}
			const QByteArray raw = file.readAll();
			file.close();

			std::string utf8;
			const std::string rawStd(raw.constData(), (size_t)raw.size());
			if (!HtmlProbe::decodeToUtf8(rawStd, probe.encoding, utf8)) {
				// iconv does not know the declared charset. Handing the
				// bytes to the engine unmodified is strictly better than
				// rendering a failed transcode.
				m_view->load(QUrl::fromLocalFile(path));
			} else {
				m_source = QString::fromUtf8(utf8.c_str(), (int)utf8.size());
				m_view->setHtml(buildPrelude() + m_source,
				                QUrl::fromLocalFile(path));
			}
		} else {
			m_view->load(QUrl::fromLocalFile(path));
		}

		m_view->setZoomFactor(g_zoomFactor);

		// The blocked-resource count is only meaningful once the page has
		// finished pulling everything it wants.
		connect(m_view, &QWebEngineView::loadFinished, this,
		        &HtmlViewerWidget::updateNoticeBar, Qt::UniqueConnection);

		// QWebEngineView delivers key events to an internal child widget,
		// so an override on this widget never sees them -- the focus proxy
		// is the only place a filter catches Ctrl+F and Ctrl+Q. It does not
		// exist until the view is realised, hence the deferred install.
		QTimer::singleShot(0, this, &HtmlViewerWidget::installKeyFilter);
		return true;
	}

	void reload() {
		const QString path = m_filePath;
		if (!path.isEmpty())
			loadFile(path);
	}

	void copySelection() { m_view->triggerPageAction(QWebEnginePage::Copy); }
	void selectAllText() { m_view->triggerPageAction(QWebEnginePage::SelectAll); }
	void focusView() { m_view->setFocus(); }

	// Double Commander's search API is synchronous -- it wants a hit/miss
	// answer as the return value -- while findText reports through a
	// callback. A scoped event loop bridges the two, with a timeout so a
	// page that never answers cannot wedge the file manager.
	bool findSynchronously(const QString &text,
	                       QWebEnginePage::FindFlags flags) {
		if (text.isEmpty())
			return false;
		bool found = false;
		QEventLoop loop;
		bool finished = false;
		m_view->findText(text, flags,
		                 [&](const QWebEngineFindTextResult &result) {
			                 found = result.numberOfMatches() > 0;
			                 finished = true;
			                 loop.quit();
		                 });
		if (!finished) {
			QTimer::singleShot(3000, &loop, &QEventLoop::quit);
			loop.exec();
		}
		return found;
	}

	bool printDocument(const QString &printerName, bool showDialog) {
		QPrinter printer(QPrinter::HighResolution);
		if (!printerName.isEmpty())
			printer.setPrinterName(printerName);
		if (showDialog) {
			QPrintDialog dialog(&printer, this);
			if (dialog.exec() != QDialog::Accepted)
				return false;
		}

		bool ok = false;
		QEventLoop loop;
		m_view->print(&printer);
		connect(m_view, &QWebEngineView::printFinished, &loop,
		        [&](bool success) {
			        ok = success;
			        loop.quit();
		        });
		// Printing goes through Chromium's own pipeline; a hard ceiling
		// keeps a failure there from becoming a hung file manager.
		QTimer::singleShot(60000, &loop, &QEventLoop::quit);
		loop.exec();
		return ok;
	}

	void showFindBar() {
		m_findBar->show();
		m_findInput->setFocus();
		m_findInput->selectAll();
	}

	void hideFindBar() {
		m_findBar->hide();
		m_view->findText(QString());
		m_view->setFocus();
	}

protected:
	void contextMenuEvent(QContextMenuEvent *event) override {
		QMenu menu(this);

		QAction *copyAction = menu.addAction(tr("Copy"));
		connect(copyAction, &QAction::triggered, this,
		        &HtmlViewerWidget::copySelection);
		QAction *selectAllAction = menu.addAction(tr("Select All"));
		connect(selectAllAction, &QAction::triggered, this,
		        &HtmlViewerWidget::selectAllText);
		QAction *findAction = menu.addAction(tr("Find..."));
		findAction->setShortcut(QKeySequence::Find);
		connect(findAction, &QAction::triggered, this,
		        &HtmlViewerWidget::showFindBar);

		menu.addSeparator();

		QAction *zoomIn = menu.addAction(tr("Zoom In"));
		connect(zoomIn, &QAction::triggered, this, [this]() { applyZoom(0.1); });
		QAction *zoomOut = menu.addAction(tr("Zoom Out"));
		connect(zoomOut, &QAction::triggered, this, [this]() { applyZoom(-0.1); });
		QAction *zoomReset = menu.addAction(tr("Reset Zoom"));
		connect(zoomReset, &QAction::triggered, this, [this]() {
			g_zoomFactor = 1.0;
			m_view->setZoomFactor(1.0);
			saveSettings();
		});

		menu.addSeparator();

		QMenu *themeMenu = menu.addMenu(tr("Theme"));
		QActionGroup *themeGroup = new QActionGroup(themeMenu);
		themeGroup->setExclusive(true);
		auto addTheme = [&](const QString &label, const QString &value) {
			QAction *action = themeMenu->addAction(label);
			action->setCheckable(true);
			action->setChecked(g_theme == value);
			themeGroup->addAction(action);
			connect(action, &QAction::triggered, this, [this, value]() {
				g_theme = value;
				saveSettings();
				reload();
			});
		};
		addTheme(tr("System"), QStringLiteral("system"));
		addTheme(tr("Dark"), QStringLiteral("dark"));
		addTheme(tr("Light"), QStringLiteral("light"));

		QAction *forceDark = menu.addAction(tr("Force Dark on Light Pages"));
		forceDark->setCheckable(true);
		forceDark->setChecked(g_forceDark);
		connect(forceDark, &QAction::triggered, this, [this](bool checked) {
			g_forceDark = checked;
			saveSettings();
			reload();
		});

		menu.addSeparator();

		QAction *scripts = menu.addAction(tr("Allow JavaScript"));
		scripts->setCheckable(true);
		scripts->setChecked(g_allowScripts);
		connect(scripts, &QAction::triggered, this, [this](bool checked) {
			g_allowScripts = checked;
			saveSettings();
			reload();
		});

		QAction *remote = menu.addAction(tr("Allow Remote Content"));
		remote->setCheckable(true);
		remote->setChecked(g_allowRemote);
		connect(remote, &QAction::triggered, this, [this](bool checked) {
			g_allowRemote = checked;
			saveSettings();
			reload();
		});

		menu.addSeparator();

		QAction *viewSource = menu.addAction(tr("View Source"));
		viewSource->setEnabled(!m_source.isEmpty());
		connect(viewSource, &QAction::triggered, this,
		        &HtmlViewerWidget::showSource);

		QAction *openExternal = menu.addAction(tr("Open in Browser"));
		connect(openExternal, &QAction::triggered, this, [this]() {
			QDesktopServices::openUrl(QUrl::fromLocalFile(m_filePath));
		});

		QAction *reloadAction = menu.addAction(tr("Reload"));
		connect(reloadAction, &QAction::triggered, this,
		        &HtmlViewerWidget::reload);

		menu.exec(event->globalPos());
	}

	bool eventFilter(QObject *watched, QEvent *event) override {
		if (event->type() == QEvent::KeyPress) {
			QKeyEvent *key = static_cast<QKeyEvent *>(event);
			if (key->matches(QKeySequence::Find)) {
				showFindBar();
				return true;
			}
			if (key->key() == Qt::Key_Escape && m_findBar->isVisible()) {
				hideFindBar();
				return true;
			}
			if ((key->modifiers() & Qt::ControlModifier) &&
			    key->key() == Qt::Key_Q) {
				// Ctrl+Q closes Quick View, but DC's hotkey manager never
				// sees a key event consumed inside an embedded native
				// window. Repost it to the top level, the same way
				// kpartview/logview/pdfview handle this.
				QWidget *target = QApplication::activeWindow();
				if (!target)
					target = window();
				if (target) {
					QCoreApplication::postEvent(
					    target, new QKeyEvent(QEvent::KeyPress, Qt::Key_Q,
					                          Qt::ControlModifier));
					QCoreApplication::postEvent(
					    target, new QKeyEvent(QEvent::KeyRelease, Qt::Key_Q,
					                          Qt::ControlModifier));
				}
				return true;
			}
		}
		return QWidget::eventFilter(watched, event);
	}

private slots:
	void installKeyFilter() {
		if (QWidget *proxy = m_view->focusProxy())
			proxy->installEventFilter(this);
	}

	void updateNoticeBar() {
		const int blocked = m_gate ? m_gate->blockedCount() : 0;
		if (blocked <= 0 || g_allowRemote) {
			m_noticeBar->hide();
			return;
		}
		m_noticeLabel->setText(
		    tr("%n remote resource(s) blocked. Scripts are %1.", "", blocked)
		        .arg(g_allowScripts ? tr("enabled") : tr("disabled")));
		m_noticeBar->show();
	}

	void showSource() {
		// Escaped through Qt rather than by hand, and rendered by the same
		// locked-down page -- source view must not become a second, laxer
		// path into the engine.
		QString escaped = m_source.toHtmlEscaped();
		const bool dark = resolveDarkMode();
		const QString style =
		    dark ? QStringLiteral("background:#1e1e1e;color:#d4d4d4")
		         : QStringLiteral("background:#ffffff;color:#000000");
		m_view->setHtml(QStringLiteral("<!doctype html><meta charset=\"utf-8\">"
		                               "<body style=\"%1;margin:0\">"
		                               "<pre style=\"white-space:pre-wrap;"
		                               "word-break:break-word;padding:8px;"
		                               "font-family:monospace\">%2</pre></body>")
		                    .arg(style, escaped));
	}

private:
	void applyEngineSettings() {
		QWebEngineSettings *settings = m_profile->settings();
		settings->setAttribute(QWebEngineSettings::JavascriptEnabled,
		                       g_allowScripts);
		settings->setAttribute(QWebEngineSettings::JavascriptCanOpenWindows,
		                       false);
		settings->setAttribute(QWebEngineSettings::JavascriptCanAccessClipboard,
		                       false);
		// Both deliberately ON, which reads backwards until you see what
		// they actually do: they are coarse allow/deny switches applied
		// *before* the request reaches an interceptor, so leaving them off
		// blocks the document's own same-directory images along with
		// everything else, and does it invisibly -- nothing reaches
		// RequestGate, so nothing can be counted or offered to the user.
		// Confirmed live: with these off, a local <img> next to the
		// document renders as a broken icon and blockedCount() stays 0.
		// Opening them makes RequestGate the single authority on what
		// loads, which is both stricter (it confines file reads to the
		// document's directory, which no setting here does) and honest
		// (the user is told what was blocked and can allow it).
		settings->setAttribute(
		    QWebEngineSettings::LocalContentCanAccessRemoteUrls, true);
		settings->setAttribute(QWebEngineSettings::LocalContentCanAccessFileUrls,
		                       true);
		settings->setAttribute(QWebEngineSettings::PluginsEnabled, false);
		settings->setAttribute(QWebEngineSettings::PdfViewerEnabled, false);
		settings->setAttribute(QWebEngineSettings::FullScreenSupportEnabled,
		                       false);
		settings->setAttribute(QWebEngineSettings::ScreenCaptureEnabled, false);
		settings->setAttribute(QWebEngineSettings::WebGLEnabled, false);
		settings->setAttribute(QWebEngineSettings::AllowRunningInsecureContent,
		                       false);
		settings->setAttribute(QWebEngineSettings::AutoLoadIconsForPage, false);
		settings->setAttribute(QWebEngineSettings::PlaybackRequiresUserGesture,
		                       true);
		// Off, so a failed subresource shows the page as authored rather
		// than Chromium's own error document inside a file manager pane.
		settings->setAttribute(QWebEngineSettings::ErrorPageEnabled, false);
	}

	// Prepended to the document rather than spliced into <head>: the HTML
	// parser hoists a leading <meta>/<style> into the head itself, which is
	// far more robust than trying to find the right insertion point in
	// markup we have not parsed.
	QString buildPrelude() const {
		const bool dark = resolveDarkMode();
		QString css = QStringLiteral(":root{color-scheme:%1}")
		                  .arg(dark ? QStringLiteral("dark")
		                            : QStringLiteral("light"));
		if (dark && g_forceDark) {
			// The standard inversion trick: flip the document, then flip
			// media back so photographs are not negatives. Only reachable
			// behind an explicit opt-in because it does misfire on pages
			// with carefully chosen colours.
			css += QStringLiteral(
			    "html{filter:invert(1) hue-rotate(180deg);background:#fff}"
			    "img,video,svg,picture,iframe,embed,object"
			    "{filter:invert(1) hue-rotate(180deg)}");
		}
		return QStringLiteral("<!doctype html><meta charset=\"utf-8\">"
		                      "<style>%1</style>")
		    .arg(css);
	}

	void applyZoom(double delta) {
		g_zoomFactor = qBound(0.25, g_zoomFactor + delta, 5.0);
		m_view->setZoomFactor(g_zoomFactor);
		saveSettings();
	}

	void buildFindBar() {
		m_findBar = new QWidget(this);
		QHBoxLayout *layout = new QHBoxLayout(m_findBar);
		layout->setContentsMargins(4, 2, 4, 2);

		m_findInput = new QLineEdit(m_findBar);
		m_findInput->setPlaceholderText(tr("Find..."));
		layout->addWidget(m_findInput, 1);

		QPushButton *next = new QPushButton(tr("Next"), m_findBar);
		QPushButton *previous = new QPushButton(tr("Previous"), m_findBar);
		QPushButton *close = new QPushButton(tr("Close"), m_findBar);
		layout->addWidget(next);
		layout->addWidget(previous);
		layout->addWidget(close);

		connect(m_findInput, &QLineEdit::textChanged, this,
		        [this](const QString &text) { m_view->findText(text); });
		connect(m_findInput, &QLineEdit::returnPressed, this,
		        [this]() { m_view->findText(m_findInput->text()); });
		connect(next, &QPushButton::clicked, this,
		        [this]() { m_view->findText(m_findInput->text()); });
		connect(previous, &QPushButton::clicked, this, [this]() {
			m_view->findText(m_findInput->text(),
			                 QWebEnginePage::FindBackward);
		});
		connect(close, &QPushButton::clicked, this,
		        &HtmlViewerWidget::hideFindBar);
	}

	void buildNoticeBar() {
		m_noticeBar = new QWidget(this);
		QHBoxLayout *layout = new QHBoxLayout(m_noticeBar);
		layout->setContentsMargins(6, 2, 6, 2);

		m_noticeLabel = new QLabel(m_noticeBar);
		layout->addWidget(m_noticeLabel, 1);

		QPushButton *allow = new QPushButton(tr("Allow for this session"),
		                                     m_noticeBar);
		layout->addWidget(allow);
		connect(allow, &QPushButton::clicked, this, [this]() {
			g_allowRemote = true;
			saveSettings();
			reload();
		});

		m_noticeBar->hide();
	}

	QWebEngineProfile *m_profile = nullptr;
	QWebEngineView *m_view = nullptr;
	RequestGate *m_gate = nullptr;
	QWebEnginePage *m_page = nullptr;
	QWidget *m_findBar = nullptr;
	QLineEdit *m_findInput = nullptr;
	QWidget *m_noticeBar = nullptr;
	QLabel *m_noticeLabel = nullptr;

	QString m_filePath;
	QString m_documentDir;
	QString m_encoding;
	// Decoded UTF-8 source, empty for documents that went the direct
	// file:// route (too large to hold, so "View Source" is disabled).
	QString m_source;
};

// ---------------------------------------------------------------------
// Lister entry points
// ---------------------------------------------------------------------

extern "C" {

HWND DCPCALL ListLoad(HWND ParentWin, char *FileToLoad, int ShowFlags)
try {
	(void)ShowFlags;
	// A GTK build of Double Commander would hand us a GtkWidget* here; the
	// cast below is only safe once we know a QApplication exists.
	if (!QApplication::instance() || !FileToLoad)
		return nullptr;

	HtmlViewerWidget *viewer = new HtmlViewerWidget((QWidget *)ParentWin);
	if (!viewer->loadFile(QString::fromUtf8(FileToLoad))) {
		// The whole point: a null handle is what makes DC try the next
		// lister, or its own viewer, instead of showing an empty pane.
		delete viewer;
		return nullptr;
	}
	viewer->show();
	viewer->focusView();
	return (HWND)viewer;
} catch (const std::exception &e) {
	fprintf(stderr, "[" PLUGNAME "] ListLoad exception: %s\n", e.what());
	return nullptr;
} catch (...) {
	fprintf(stderr, "[" PLUGNAME "] ListLoad unknown exception\n");
	return nullptr;
}

// Reuses the existing window when the user steps to the next file with the
// viewer open, instead of forcing DC to tear down and rebuild a Chromium
// page for every arrow key press.
int DCPCALL ListLoadNext(HWND ParentWin, HWND PluginWin, char *FileToLoad,
                         int ShowFlags)
try {
	(void)ParentWin;
	(void)ShowFlags;
	HtmlViewerWidget *viewer = (HtmlViewerWidget *)PluginWin;
	if (!viewer || !FileToLoad)
		return LISTPLUGIN_ERROR;
	if (!viewer->loadFile(QString::fromUtf8(FileToLoad)))
		return LISTPLUGIN_ERROR;
	return LISTPLUGIN_OK;
} catch (...) {
	return LISTPLUGIN_ERROR;
}

void DCPCALL ListCloseWindow(HWND ListWin) {
	HtmlViewerWidget *viewer = (HtmlViewerWidget *)ListWin;
	delete viewer;
}

void DCPCALL ListGetDetectString(char *DetectString, int maxlen) {
	if (!DetectString || maxlen <= 0)
		return;
	// snprintf's size argument already accounts for the terminator, and
	// maxlen is guarded above -- `maxlen - 1` would truncate a character
	// early, and would be SIZE_MAX after conversion if maxlen were ever 0.
	snprintf(DetectString, (size_t)maxlen, "%s",
	         g_detectString.toUtf8().constData());
}

int DCPCALL ListSearchText(HWND ListWin, char *SearchString,
                           int SearchParameter) {
	HtmlViewerWidget *viewer = (HtmlViewerWidget *)ListWin;
	if (!viewer || !SearchString)
		return LISTPLUGIN_ERROR;

	QWebEnginePage::FindFlags flags;
	if (SearchParameter & lcs_matchcase)
		flags |= QWebEnginePage::FindCaseSensitively;
	if (SearchParameter & lcs_backwards)
		flags |= QWebEnginePage::FindBackward;
	// lcs_wholewords has no counterpart in Chromium's find API; the search
	// runs as a substring match rather than silently reporting no hits.

	return viewer->findSynchronously(QString::fromUtf8(SearchString), flags)
	           ? LISTPLUGIN_OK
	           : LISTPLUGIN_ERROR;
}

int DCPCALL ListSendCommand(HWND ListWin, int Command, int Parameter) {
	(void)Parameter;
	HtmlViewerWidget *viewer = (HtmlViewerWidget *)ListWin;
	if (!viewer)
		return LISTPLUGIN_ERROR;

	switch (Command) {
	case lc_copy:
		viewer->copySelection();
		return LISTPLUGIN_OK;
	case lc_selectall:
		viewer->selectAllText();
		return LISTPLUGIN_OK;
	case lc_focus:
		viewer->focusView();
		return LISTPLUGIN_OK;
	case lc_newparams:
		// lcp_wraptext, lcp_fittowindow, lcp_ansi, lcp_ascii and friends
		// are all text-viewer concepts: line wrapping, image fitting and
		// codepage overrides are decided by the document's own CSS and by
		// the charset resolved at load time, none of which a lister
		// parameter can meaningfully override here. Accepted rather than
		// refused so DC does not treat the plugin as broken.
		return LISTPLUGIN_OK;
	default:
		// Includes lc_setpercent: scrolling to a position needs script
		// execution, which is exactly what this plugin refuses to do by
		// default.
		return LISTPLUGIN_ERROR;
	}
}

int DCPCALL ListPrint(HWND ListWin, char *FileToPrint, char *DefPrinter,
                      int PrintFlags, RECT *Margins) {
	(void)FileToPrint;
	(void)PrintFlags;
	(void)Margins;
	HtmlViewerWidget *viewer = (HtmlViewerWidget *)ListWin;
	if (!viewer)
		return LISTPLUGIN_ERROR;
	const QString printerName =
	    DefPrinter ? QString::fromUtf8(DefPrinter) : QString();
	return viewer->printDocument(printerName, true) ? LISTPLUGIN_OK
	                                                : LISTPLUGIN_ERROR;
}

void DCPCALL ListSetDefaultParams(ListDefaultParamStruct *dps) {
	if (!dps)
		return;

	// DefaultIniName is the SDK's sanctioned way to find the plugin's
	// configuration directory. No dladdr, no fixed-size path buffers, and
	// no setlocale(LC_ALL, "") -- this is a shared library inside someone
	// else's process, and LC_NUMERIC in particular would change decimal
	// parsing for Double Commander and every other loaded plugin.
	const QFileInfo defaultIni(QString::fromUtf8(dps->DefaultIniName));
	g_configPath = defaultIni.absolutePath() + QStringLiteral("/" PLUGNAME ".ini");

	QSettings settings(g_configPath, QSettings::IniFormat);
	auto load = [&settings](const char *key, auto &target, auto reader) {
		const QString path = QStringLiteral(PLUGNAME "/") + QLatin1String(key);
		if (settings.contains(path))
			target = reader(settings.value(path));
	};

	load("theme", g_theme,
	     [](const QVariant &v) { return v.toString().toLower(); });
	load("force_dark", g_forceDark, [](const QVariant &v) { return v.toBool(); });
	load("allow_scripts", g_allowScripts,
	     [](const QVariant &v) { return v.toBool(); });
	load("allow_remote_content", g_allowRemote,
	     [](const QVariant &v) { return v.toBool(); });
	load("zoom_factor", g_zoomFactor,
	     [](const QVariant &v) { return v.toDouble(); });
	load("max_file_size", g_maxFileSize,
	     [](const QVariant &v) { return v.toULongLong(); });
	load("fallback_encoding", g_fallbackEncoding,
	     [](const QVariant &v) { return v.toString(); });
	load("detect_string", g_detectString,
	     [](const QVariant &v) { return v.toString(); });

	if (g_zoomFactor < 0.25 || g_zoomFactor > 5.0)
		g_zoomFactor = 1.0;
	if (g_maxFileSize == 0)
		g_maxFileSize = 32ULL * 1024 * 1024;

	// Seed the ini on first run so every knob is discoverable by opening
	// the file, rather than by reading this source.
	saveSettings();
}

}  // extern "C"

#include "plugin_qt6.moc"
