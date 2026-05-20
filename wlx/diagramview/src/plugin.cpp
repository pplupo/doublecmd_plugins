#include <QFile>
#include <QFileInfo>
#include <QSettings>
#include <QRegularExpression>
#include <QtWidgets>
#include <QClipboard>
#include <QApplication>
#include <QMessageBox>
#include <QProcess>
#include <QTemporaryFile>
#include <QFileSystemWatcher>
#include <QTimer>
#include <QMenu>
#include <QAction>
#include <QContextMenuEvent>
#include <QFileDialog>
#include <QGuiApplication>
#include <QPainter>
#include <QPalette>
#include <QSvgRenderer>
#include <QGraphicsSvgItem>
#include <QWheelEvent>
#include <QResizeEvent>
#include <cmath>
#include <dlfcn.h>
#include <libintl.h>
#include <locale.h>

#define _(STRING) gettext(STRING)
#define GETTEXT_PACKAGE "plugins"

#include "wlxplugin.h"

// Settings
static bool g_autoReloadEnabled = true;
static bool g_darkMode = false;
static bool g_useSystemDarkMode = true;
static QString g_configPath;

static QString g_renderer = "java";           // "java" or "web" (for PlantUML)
static QString g_mermaidRenderer = "local";   // "local" or "web" (for Mermaid)
static QString g_plantumlPath = "plantuml";
static QString g_javaPath = "java";
static QString g_plantumlServerUrl = "http://www.plantuml.com/plantuml";
static QString g_mmdcPath = "mmdc";
static QString g_mermaidServerUrl = "https://mermaid.ink";
static int g_timeoutMs = 15000;

static void saveSettings() {
	if (g_configPath.isEmpty()) return;
	QSettings settings(g_configPath, QSettings::IniFormat);
	settings.setValue(PLUGNAME "/auto_reload", g_autoReloadEnabled);
	settings.setValue(PLUGNAME "/dark_mode", g_darkMode);
	settings.setValue(PLUGNAME "/use_system_dark_mode", g_useSystemDarkMode);
	settings.setValue(PLUGNAME "/renderer", g_renderer);
	settings.setValue(PLUGNAME "/mermaid_renderer", g_mermaidRenderer);
	settings.setValue(PLUGNAME "/plantuml_path", g_plantumlPath);
	settings.setValue(PLUGNAME "/java_path", g_javaPath);
	settings.setValue(PLUGNAME "/plantuml_server_url", g_plantumlServerUrl);
	settings.setValue(PLUGNAME "/mmdc_path", g_mmdcPath);
	settings.setValue(PLUGNAME "/mermaid_server_url", g_mermaidServerUrl);
	settings.setValue(PLUGNAME "/timeout_ms", g_timeoutMs);
}

static QString getPluginDir() {
	Dl_info dlinfo;
	if (dladdr((void*)getPluginDir, &dlinfo) != 0) {
		QString path = QString::fromUtf8(dlinfo.dli_fname);
		int idx = path.lastIndexOf('/');
		if (idx != -1) {
			return path.left(idx);
		}
	}
	return QString();
}

// Helper to check system dark mode
static bool isSystemDark() {
	QPalette pal = QGuiApplication::palette();
	return pal.color(QPalette::Window).value() < 128;
}

static bool runMermaidWeb(const QString& inputPath, const QString& outputPath, bool darkMode) {
	QFile file(inputPath);
	if (!file.open(QIODevice::ReadOnly)) {
		return false;
	}
	QByteArray code = file.readAll();
	file.close();

	QByteArray base64 = code.toBase64();
	QString url = g_mermaidServerUrl + "/svg/" + QString::fromUtf8(base64.toPercentEncoding());

	QProcess proc;
	QStringList args;
	args << "-s" << "-f" << "--max-time" << QString::number(g_timeoutMs / 1000) << url;
	proc.start("curl", args);
	if (proc.waitForStarted() && proc.waitForFinished(g_timeoutMs)) {
		if (proc.exitCode() == 0) {
			QByteArray outData = proc.readAllStandardOutput();
			if (!outData.isEmpty()) {
				QFile outFile(outputPath);
				if (outFile.open(QIODevice::WriteOnly)) {
					outFile.write(outData);
					outFile.close();
					return true;
				}
			}
		}
	}
	return false;
}

static bool runMermaidLocal(const QString& inputPath, const QString& outputPath, bool darkMode) {
	QTemporaryFile configFile;
	QString configPath;
	if (configFile.open()) {
		configPath = configFile.fileName() + ".json";
		configFile.close();
		QFile tempConf(configPath);
		if (tempConf.open(QIODevice::WriteOnly)) {
			tempConf.write(R"({
  "htmlLabels": false,
  "flowchart": { "htmlLabels": false },
  "sequence": { "htmlLabels": false },
  "gantt": { "htmlLabels": false },
  "journey": { "htmlLabels": false },
  "class": { "htmlLabels": false },
  "state": { "htmlLabels": false },
  "er": { "htmlLabels": false },
  "pie": { "htmlLabels": false },
  "c4": { "htmlLabels": false }
})");
			tempConf.close();
		}
	}

	QStringList args;
	args << "-i" << inputPath << "-o" << outputPath << "-e" << "svg";
	if (darkMode) {
		args << "--theme" << "dark";
	}
	
	if (!configPath.isEmpty() && QFile::exists(configPath)) {
		args << "-c" << configPath;
	}
	
	// 1. Try running configured mmdc_path
	QProcess proc;
	proc.start(g_mmdcPath, args);
	if (proc.waitForStarted() && proc.waitForFinished(g_timeoutMs)) {
		if (proc.exitCode() == 0 && QFile::exists(outputPath)) {
			if (!configPath.isEmpty()) {
				QFile::remove(configPath);
			}
			return true;
		}
	}

	// 2. Try looking for "mmdc" in the Double Commander config directory
	QString configDir = g_configPath.isEmpty() ? QString() : QFileInfo(g_configPath).absolutePath();
	if (!configDir.isEmpty()) {
		QString configMmdc = configDir + "/mmdc";
		if (QFile::exists(configMmdc)) {
			QProcess procConfig;
			procConfig.start(configMmdc, args);
			if (procConfig.waitForStarted() && procConfig.waitForFinished(g_timeoutMs)) {
				if (procConfig.exitCode() == 0 && QFile::exists(outputPath)) {
					if (!configPath.isEmpty()) {
						QFile::remove(configPath);
					}
					return true;
				}
			}
		}
	}

	// 3. Try looking for "mmdc" in the plugin directory
	QString pluginMmdc = getPluginDir() + "/mmdc";
	if (!pluginMmdc.isEmpty() && QFile::exists(pluginMmdc)) {
		QProcess procPlugin;
		procPlugin.start(pluginMmdc, args);
		if (procPlugin.waitForStarted() && procPlugin.waitForFinished(g_timeoutMs)) {
			if (procPlugin.exitCode() == 0 && QFile::exists(outputPath)) {
				if (!configPath.isEmpty()) {
					QFile::remove(configPath);
				}
				return true;
			}
		}
	}

	// 4. Try running raw "mmdc" as fallback if g_mmdcPath is different
	if (g_mmdcPath != "mmdc") {
		QProcess procMmdc;
		procMmdc.start("mmdc", args);
		if (procMmdc.waitForStarted() && procMmdc.waitForFinished(g_timeoutMs)) {
			if (procMmdc.exitCode() == 0 && QFile::exists(outputPath)) {
				if (!configPath.isEmpty()) {
					QFile::remove(configPath);
				}
				return true;
			}
		}
	}

	// 5. Fall back to "npx"
	QProcess procNpx;
	QStringList npxArgs;
	npxArgs << "-y" << "@mermaid-js/mermaid-cli" << "-i" << inputPath << "-o" << outputPath << "-e" << "svg";
	if (darkMode) {
		npxArgs << "--theme" << "dark";
	}
	if (!configPath.isEmpty() && QFile::exists(configPath)) {
		npxArgs << "-c" << configPath;
	}
	procNpx.start("npx", npxArgs);
	if (procNpx.waitForStarted() && procNpx.waitForFinished(g_timeoutMs * 3)) {
		if (procNpx.exitCode() == 0 && QFile::exists(outputPath)) {
			if (!configPath.isEmpty()) {
				QFile::remove(configPath);
			}
			return true;
		}
	}

	if (!configPath.isEmpty()) {
		QFile::remove(configPath);
	}
	return false;
}

static bool runMermaid(const QString& inputPath, const QString& outputPath, bool darkMode) {
	if (g_mermaidRenderer == "web") {
		if (runMermaidWeb(inputPath, outputPath, darkMode)) {
			return true;
		}
	}
	if (runMermaidLocal(inputPath, outputPath, darkMode)) {
		return true;
	}
	if (g_mermaidRenderer != "web") {
		return runMermaidWeb(inputPath, outputPath, darkMode);
	}
	return false;
}

static QByteArray runPlantUmlWeb(const QString& inputPath, bool darkMode) {
	QFile file(inputPath);
	if (!file.open(QIODevice::ReadOnly)) {
		return QByteArray();
	}
	QByteArray code = file.readAll();
	file.close();

	QString hexStr = "~h" + QString::fromUtf8(code).toUtf8().toHex();
	QString url = g_plantumlServerUrl + "/svg/" + hexStr;

	QProcess proc;
	QStringList args;
	args << "-s" << "-f" << "--max-time" << QString::number(g_timeoutMs / 1000) << url;
	proc.start("curl", args);
	if (proc.waitForStarted() && proc.waitForFinished(g_timeoutMs)) {
		if (proc.exitCode() == 0) {
			return proc.readAllStandardOutput();
		}
	}
	return QByteArray();
}

static QByteArray runPlantUmlLocal(const QString& inputPath, bool darkMode) {
	QStringList exeOptions;
	QStringList jarOptions;

	if (g_plantumlPath.endsWith(".jar", Qt::CaseInsensitive)) {
		jarOptions << g_plantumlPath;
	} else {
		exeOptions << g_plantumlPath;
	}

	if (!exeOptions.contains("plantuml")) {
		exeOptions << "plantuml";
	}
	if (!exeOptions.contains("/usr/bin/plantuml")) {
		exeOptions << "/usr/bin/plantuml";
	}

	jarOptions << "/usr/share/java/plantuml/plantuml.jar";
	jarOptions << "/usr/share/plantuml/plantuml.jar";
	QString localJar = getPluginDir() + "/plantuml.jar";
	if (!localJar.isEmpty()) {
		jarOptions << localJar;
	}
	QString configDir = g_configPath.isEmpty() ? QString() : QFileInfo(g_configPath).absolutePath();
	if (!configDir.isEmpty()) {
		jarOptions << configDir + "/plantuml.jar";
	}

	for (const QString& exe : exeOptions) {
		QProcess proc;
		QStringList pumlArgs;
		pumlArgs << "-tsvg" << "-pipe";
		if (darkMode) {
			pumlArgs << "--dark-mode";
		}
		proc.start(exe, pumlArgs);
		if (proc.waitForStarted()) {
			QFile inFile(inputPath);
			if (inFile.open(QIODevice::ReadOnly)) {
				proc.write(inFile.readAll());
				proc.closeWriteChannel();
			}
			if (proc.waitForFinished(g_timeoutMs)) {
				if (proc.exitCode() == 0) {
					QByteArray res = proc.readAllStandardOutput();
					if (!res.isEmpty()) {
						return res;
					}
				}
			}
		}
	}

	for (const QString& jar : jarOptions) {
		if (!QFile::exists(jar)) {
			continue;
		}
		QProcess proc;
		QStringList pumlArgs;
		pumlArgs << "-jar" << jar << "-tsvg" << "-pipe";
		if (darkMode) {
			pumlArgs << "--dark-mode";
		}
		proc.start(g_javaPath, pumlArgs);
		if (proc.waitForStarted()) {
			QFile inFile(inputPath);
			if (inFile.open(QIODevice::ReadOnly)) {
				proc.write(inFile.readAll());
				proc.closeWriteChannel();
			}
			if (proc.waitForFinished(g_timeoutMs)) {
				if (proc.exitCode() == 0) {
					QByteArray res = proc.readAllStandardOutput();
					if (!res.isEmpty()) {
						return res;
					}
				}
			}
		}
		if (g_javaPath != "java") {
			QProcess procJava;
			procJava.start("java", pumlArgs);
			if (procJava.waitForStarted()) {
				QFile inFile(inputPath);
				if (inFile.open(QIODevice::ReadOnly)) {
					procJava.write(inFile.readAll());
					procJava.closeWriteChannel();
				}
				if (procJava.waitForFinished(g_timeoutMs)) {
					if (procJava.exitCode() == 0) {
						QByteArray res = procJava.readAllStandardOutput();
						if (!res.isEmpty()) {
							return res;
						}
					}
				}
			}
		}
	}

	return QByteArray();
}

static QByteArray fixMermaidSvgText(const QByteArray& svgData) {
	QString svgStr = QString::fromUtf8(svgData);
	
	// Phase 1: Match <text ...> followed by <tspan ...> and move the combined y+dy baseline to the parent <text> element.
	QRegularExpression textTspanRe(R"(<text\b([^>]*)>\s*<tspan\b([^>]*)>)");
	QRegularExpressionMatchIterator it = textTspanRe.globalMatch(svgStr);
	QList<QRegularExpressionMatch> matches;
	while (it.hasNext()) {
		matches.append(it.next());
	}
	
	QRegularExpression yRe(R"(\by\s*=\s*"(-?[0-9]*\.?[0-9]+)em")");
	QRegularExpression dyRe(R"(\bdy\s*=\s*"(-?[0-9]*\.?[0-9]+)em")");
	QRegularExpression textYRe(R"(\by\s*=\s*"[^"]*")");
	
	for (int i = matches.size() - 1; i >= 0; --i) {
		const QRegularExpressionMatch& match = matches.at(i);
		QString textAttrs = match.captured(1);
		QString tspanAttrs = match.captured(2);
		
		QRegularExpressionMatch yMatch = yRe.match(tspanAttrs);
		QRegularExpressionMatch dyMatch = dyRe.match(tspanAttrs);
		
		if (yMatch.hasMatch() && dyMatch.hasMatch()) {
			double yEm = yMatch.captured(1).toDouble();
			double dyEm = dyMatch.captured(1).toDouble();
			double baselinePx = (yEm + dyEm) * 16.0 - 2.0;
			
			// Replace or insert y attribute in parent <text>
			QRegularExpressionMatch textYMatch = textYRe.match(textAttrs);
			if (textYMatch.hasMatch()) {
				textAttrs.replace(textYMatch.capturedStart(0), textYMatch.capturedLength(0), 
				                  QString("y=\"%1\"").arg(baselinePx, 0, 'f', 2));
			} else {
				textAttrs = QString(" y=\"%1\"").arg(baselinePx, 0, 'f', 2) + textAttrs;
			}
			
			// Remove y and dy attributes from first <tspan> so it inherits parent's y
			tspanAttrs.remove(yRe);
			tspanAttrs.remove(dyRe);
			
			// Clean up extra whitespaces inside tspanAttrs
			tspanAttrs = tspanAttrs.simplified();
			if (!tspanAttrs.isEmpty() && !tspanAttrs.startsWith(" ")) {
				tspanAttrs = " " + tspanAttrs;
			}
			
			QString replacement = QString("<text%1><tspan%2>").arg(textAttrs).arg(tspanAttrs);
			svgStr.replace(match.capturedStart(0), match.capturedLength(0), replacement);
		}
	}
	
	// Phase 2: Convert any remaining em values in y/dy attributes to pixel values
	QRegularExpression emRe(R"(\b(y|dy)\s*=\s*"(-?[0-9]*\.?[0-9]+)em")");
	QRegularExpressionMatchIterator emIt = emRe.globalMatch(svgStr);
	QList<QRegularExpressionMatch> emMatches;
	while (emIt.hasNext()) {
		emMatches.append(emIt.next());
	}
	
	for (int i = emMatches.size() - 1; i >= 0; --i) {
		const QRegularExpressionMatch& match = emMatches.at(i);
		QString attr = match.captured(1);
		double emValue = match.captured(2).toDouble();
		double pxValue = emValue * 16.0; // 1em = 16px
		
		QString replacement = QString("%1=\"%2\"").arg(attr).arg(pxValue, 0, 'f', 2);
		svgStr.replace(match.capturedStart(0), match.capturedLength(0), replacement);
	}
	
	return svgStr.toUtf8();
}

class SvgDiagramViewer : public QGraphicsView {
private:
	QString m_currentFilePath;
	QFileSystemWatcher m_watcher;
	QTimer m_debounceTimer;
	QByteArray m_lastSvgData;

public:
	SvgDiagramViewer(QWidget* parent = nullptr) : QGraphicsView(parent) {
		setScene(new QGraphicsScene(this));
		setDragMode(QGraphicsView::ScrollHandDrag);
		setRenderHint(QPainter::Antialiasing);
		setRenderHint(QPainter::SmoothPixmapTransform);
		setTransformationAnchor(QGraphicsView::AnchorUnderMouse);
		
		setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
		setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
		setFocusPolicy(Qt::NoFocus);
		
		m_debounceTimer.setSingleShot(true);
		m_debounceTimer.setInterval(200);

		connect(&m_debounceTimer, &QTimer::timeout, this, &SvgDiagramViewer::executeRender);

		connect(&m_watcher, &QFileSystemWatcher::fileChanged, this, [this](const QString&) {
			if (g_autoReloadEnabled) {
				m_debounceTimer.start();
			}
		});
	}

	void loadFile(const QString& filePath) {
		m_currentFilePath = filePath;
		
		if (!m_watcher.files().isEmpty()) {
			m_watcher.removePaths(m_watcher.files());
		}
		
		if (QFile::exists(filePath)) {
			m_watcher.addPath(filePath);
		}
		
		executeRender();
	}

	void loadSvgData(const QByteArray& svgData) {
		scene()->clear();
		
		QGraphicsSvgItem* svgItem = new QGraphicsSvgItem();
		QSvgRenderer* renderer = new QSvgRenderer(svgData, svgItem);
		if (!renderer->isValid()) {
			delete svgItem;
			return;
		}
		svgItem->setSharedRenderer(renderer);
		scene()->addItem(svgItem);
		
		QRectF bounds = svgItem->boundingRect();
		if (bounds.isEmpty() || !bounds.isValid()) {
			QSize defSize = renderer->defaultSize();
			if (defSize.isValid() && !defSize.isEmpty()) {
				bounds = QRectF(0, 0, defSize.width(), defSize.height());
			} else {
				bounds = renderer->viewBoxF();
			}
		}
		scene()->setSceneRect(bounds);
		
		if (width() > 10 && height() > 10) {
			fitInView(bounds, Qt::KeepAspectRatio);
		}
	}

	void executeRender() {
		if (m_currentFilePath.isEmpty())
			return;

		QByteArray svgData;
		QFileInfo fi(m_currentFilePath);
		QString ext = fi.suffix().toLower();
		
		bool activeDarkMode = g_useSystemDarkMode ? isSystemDark() : g_darkMode;

		if (ext == "mmd" || ext == "mermaid") {
			QTemporaryFile tempFile;
			QString tempPath;
			if (tempFile.open()) {
				tempPath = tempFile.fileName() + ".svg";
				tempFile.close();
			} else {
				QMessageBox::critical(this, _("Diagram Viewer Error"), _("Failed to create temporary file."));
				return;
			}

			if (runMermaid(m_currentFilePath, tempPath, activeDarkMode)) {
				QFile outFile(tempPath);
				if (outFile.open(QIODevice::ReadOnly)) {
					svgData = outFile.readAll();
					outFile.close();
				}
				QFile::remove(tempPath);
			} else {
				QMessageBox::critical(this, _("Diagram Viewer Error"), 
					_("Failed to render Mermaid diagram.\n"
					  "Please ensure '@mermaid-js/mermaid-cli' is installed, 'npx' is available, or internet connection is active."));
				return;
			}
		} else if (ext == "puml" || ext == "plantuml") {
			if (g_renderer == "web") {
				svgData = runPlantUmlWeb(m_currentFilePath, activeDarkMode);
				if (svgData.isEmpty()) {
					svgData = runPlantUmlLocal(m_currentFilePath, activeDarkMode);
				}
			} else {
				svgData = runPlantUmlLocal(m_currentFilePath, activeDarkMode);
				if (svgData.isEmpty()) {
					svgData = runPlantUmlWeb(m_currentFilePath, activeDarkMode);
				}
			}

			if (svgData.isEmpty()) {
				QMessageBox::critical(this, _("Diagram Viewer Error"), 
					_("Failed to render PlantUML diagram.\n"
					  "Please ensure Java/PlantUML is installed locally, or internet connection is active."));
				return;
			}
		} else {
			QMessageBox::critical(this, _("Diagram Viewer Error"), _("Unsupported file extension: ") + ext);
			return;
		}

		if (!svgData.isEmpty()) {
			if (ext == "mmd" || ext == "mermaid") {
				svgData = fixMermaidSvgText(svgData);
			}
			m_lastSvgData = svgData;
			loadSvgData(svgData);
		}

		// Re-add to watcher in case editor used atomic save (delete/rename)
		if (QFile::exists(m_currentFilePath) && !m_watcher.files().contains(m_currentFilePath)) {
			m_watcher.addPath(m_currentFilePath);
		}
	}

	void saveAsSvg() {
		if (m_lastSvgData.isEmpty())
			return;
		QString filePath = QFileDialog::getSaveFileName(this, _("Save as SVG"), QString(), _("SVG Files (*.svg)"));
		if (!filePath.isEmpty()) {
			QFile file(filePath);
			if (file.open(QIODevice::WriteOnly)) {
				file.write(m_lastSvgData);
				file.close();
			} else {
				QMessageBox::warning(this, _("Error"), _("Could not open file for writing."));
			}
		}
	}

	void saveAsPng() {
		if (m_lastSvgData.isEmpty())
			return;
		QSvgRenderer renderer(m_lastSvgData);
		QSize size = renderer.defaultSize();
		if (!size.isValid() || size.isEmpty()) {
			size = QSize(800, 600);
		}
		QImage image(size, QImage::Format_ARGB32);
		image.fill(Qt::transparent);
		QPainter painter(&image);
		renderer.render(&painter);
		painter.end();

		QString filePath = QFileDialog::getSaveFileName(this, _("Save as PNG"), QString(), _("PNG Files (*.png)"));
		if (!filePath.isEmpty()) {
			if (!image.save(filePath, "PNG")) {
				QMessageBox::warning(this, _("Error"), _("Could not save PNG file."));
			}
		}
	}

	void copyToClipboard() {
		if (m_lastSvgData.isEmpty())
			return;
		QSvgRenderer renderer(m_lastSvgData);
		QSize size = renderer.defaultSize();
		if (!size.isValid() || size.isEmpty()) {
			size = QSize(800, 600);
		}
		QImage image(size, QImage::Format_ARGB32);
		image.fill(Qt::white);
		QPainter painter(&image);
		renderer.render(&painter);
		painter.end();

		QGuiApplication::clipboard()->setImage(image);
	}

protected:
	void wheelEvent(QWheelEvent* event) override {
		const double scaleFactor = 1.15;
		if (event->angleDelta().y() > 0) {
			scale(scaleFactor, scaleFactor);
		} else {
			scale(1.0 / scaleFactor, 1.0 / scaleFactor);
		}
	}

	void resizeEvent(QResizeEvent* event) override {
		QGraphicsView::resizeEvent(event);
		if (scene() && !scene()->sceneRect().isEmpty()) {
			fitInView(scene()->sceneRect(), Qt::KeepAspectRatio);
		}
	}

	void drawBackground(QPainter* painter, const QRectF& rect) override {
		bool activeDarkMode = g_useSystemDarkMode ? isSystemDark() : g_darkMode;
		QColor bgColor = activeDarkMode ? QColor(30, 30, 46) : QColor(248, 249, 250);
		QColor gridColor = activeDarkMode ? QColor(45, 45, 68) : QColor(226, 232, 240);
		
		painter->fillRect(rect, bgColor);

		painter->save();
		painter->setPen(QPen(gridColor, 1, Qt::DotLine));
		
		qreal gridSpacing = 20.0;
		qreal left = std::floor(rect.left() / gridSpacing) * gridSpacing;
		qreal top = std::floor(rect.top() / gridSpacing) * gridSpacing;
		
		for (qreal x = left; x < rect.right(); x += gridSpacing) {
			painter->drawLine(QPointF(x, rect.top()), QPointF(x, rect.bottom()));
		}
		for (qreal y = top; y < rect.bottom(); y += gridSpacing) {
			painter->drawLine(QPointF(rect.left(), y), QPointF(rect.right(), y));
		}
		painter->restore();
	}

	void contextMenuEvent(QContextMenuEvent* event) override {
		QMenu menu(this);

		QAction* reloadAction = menu.addAction(_("Reload Diagram"));
		connect(reloadAction, &QAction::triggered, this, &SvgDiagramViewer::executeRender);

		menu.addSeparator();

		QAction* saveAsSvgAction = menu.addAction(_("Save as SVG..."));
		connect(saveAsSvgAction, &QAction::triggered, this, &SvgDiagramViewer::saveAsSvg);

		QAction* saveAsPngAction = menu.addAction(_("Save as PNG..."));
		connect(saveAsPngAction, &QAction::triggered, this, &SvgDiagramViewer::saveAsPng);

		QAction* copyAction = menu.addAction(_("Copy Image to Clipboard"));
		connect(copyAction, &QAction::triggered, this, &SvgDiagramViewer::copyToClipboard);

		menu.addSeparator();

		QAction* toggleAutoAction = menu.addAction(_("Auto-Reload on Save"));
		toggleAutoAction->setCheckable(true);
		toggleAutoAction->setChecked(g_autoReloadEnabled);
		connect(toggleAutoAction, &QAction::triggered, this, [](bool checked) {
			g_autoReloadEnabled = checked;
			saveSettings();
		});

		QAction* toggleSystemDarkAction = menu.addAction(_("Use System Dark Mode"));
		toggleSystemDarkAction->setCheckable(true);
		toggleSystemDarkAction->setChecked(g_useSystemDarkMode);
		connect(toggleSystemDarkAction, &QAction::triggered, this, [this](bool checked) {
			g_useSystemDarkMode = checked;
			saveSettings();
			executeRender();
		});

		QAction* toggleDarkAction = menu.addAction(_("Force Dark Mode"));
		toggleDarkAction->setCheckable(true);
		toggleDarkAction->setChecked(g_darkMode);
		toggleDarkAction->setEnabled(!g_useSystemDarkMode);
		connect(toggleDarkAction, &QAction::triggered, this, [this](bool checked) {
			g_darkMode = checked;
			saveSettings();
			executeRender();
		});

		menu.addSeparator();

		QMenu* mermaidMenu = menu.addMenu(_("Mermaid Renderer"));
		QAction* mermaidLocalAction = mermaidMenu->addAction(_("Local (mmdc/npx)"));
		mermaidLocalAction->setCheckable(true);
		mermaidLocalAction->setChecked(g_mermaidRenderer == "local");
		connect(mermaidLocalAction, &QAction::triggered, this, [this]() {
			g_mermaidRenderer = "local";
			saveSettings();
			executeRender();
		});

		QAction* mermaidWebAction = mermaidMenu->addAction(_("Web (mermaid.ink)"));
		mermaidWebAction->setCheckable(true);
		mermaidWebAction->setChecked(g_mermaidRenderer == "web");
		connect(mermaidWebAction, &QAction::triggered, this, [this]() {
			g_mermaidRenderer = "web";
			saveSettings();
			executeRender();
		});

		QMenu* plantumlMenu = menu.addMenu(_("PlantUML Renderer"));
		QAction* pumlLocalAction = plantumlMenu->addAction(_("Local (native/java)"));
		pumlLocalAction->setCheckable(true);
		pumlLocalAction->setChecked(g_renderer == "java");
		connect(pumlLocalAction, &QAction::triggered, this, [this]() {
			g_renderer = "java";
			saveSettings();
			executeRender();
		});

		QAction* pumlWebAction = plantumlMenu->addAction(_("Web (plantuml.com)"));
		pumlWebAction->setCheckable(true);
		pumlWebAction->setChecked(g_renderer == "web");
		connect(pumlWebAction, &QAction::triggered, this, [this]() {
			g_renderer = "web";
			saveSettings();
			executeRender();
		});

		menu.exec(event->globalPos());
	}
};

HANDLE DCPCALL ListLoad(HANDLE ParentWin, char* FileToLoad, int ShowFlags)
{
	if (!QApplication::instance())
		return nullptr;

	QFileInfo fi(FileToLoad);
	QString ext = fi.suffix().toLower();
	if (ext != "mmd" && ext != "mermaid" && ext != "puml" && ext != "plantuml") {
		return nullptr;
	}

	SvgDiagramViewer* viewer = new SvgDiagramViewer((QWidget*)ParentWin);
	viewer->loadFile(QString(FileToLoad));
	viewer->show();

	return viewer;
}

void DCPCALL ListCloseWindow(HANDLE ListWin)
{
	SvgDiagramViewer* viewer = (SvgDiagramViewer*)ListWin;
	delete viewer;
}

int DCPCALL ListSendCommand(HWND ListWin, int Command, int Parameter)
{
	SvgDiagramViewer* viewer = (SvgDiagramViewer*)ListWin;

	if (Command == lc_newparams) {
		viewer->executeRender();
		return LISTPLUGIN_OK;
	}
	
	if (Command == lc_copy) {
		viewer->copyToClipboard();
		return LISTPLUGIN_OK;
	}

	return LISTPLUGIN_ERROR;
}

int DCPCALL ListSearchText(HWND ListWin, char* SearchString, int SearchParameter)
{
	return LISTPLUGIN_ERROR;
}

void DCPCALL ListGetDetectString(char* DetectString, int maxlen)
{
	snprintf(DetectString, maxlen - 1, "(EXT=\"PUML\" | EXT=\"PLANTUML\" | EXT=\"MMD\" | EXT=\"MERMAID\") & SIZE<30000000");
}

void DCPCALL ListSetDefaultParams(ListDefaultParamStruct* dps)
{
	QFileInfo defini(QString::fromStdString(dps->DefaultIniName));
	g_configPath = defini.absolutePath() + "/diagramview.ini";
	QSettings settings(g_configPath, QSettings::IniFormat);

	if (!settings.contains(PLUGNAME "/auto_reload"))
		settings.setValue(PLUGNAME "/auto_reload", g_autoReloadEnabled);
	else
		g_autoReloadEnabled = settings.value(PLUGNAME "/auto_reload").toBool();

	if (!settings.contains(PLUGNAME "/dark_mode"))
		settings.setValue(PLUGNAME "/dark_mode", g_darkMode);
	else
		g_darkMode = settings.value(PLUGNAME "/dark_mode").toBool();

	if (!settings.contains(PLUGNAME "/use_system_dark_mode"))
		settings.setValue(PLUGNAME "/use_system_dark_mode", g_useSystemDarkMode);
	else
		g_useSystemDarkMode = settings.value(PLUGNAME "/use_system_dark_mode").toBool();

	if (!settings.contains(PLUGNAME "/renderer"))
		settings.setValue(PLUGNAME "/renderer", g_renderer);
	else
		g_renderer = settings.value(PLUGNAME "/renderer").toString();

	if (!settings.contains(PLUGNAME "/mermaid_renderer"))
		settings.setValue(PLUGNAME "/mermaid_renderer", g_mermaidRenderer);
	else
		g_mermaidRenderer = settings.value(PLUGNAME "/mermaid_renderer").toString();

	if (!settings.contains(PLUGNAME "/plantuml_path"))
		settings.setValue(PLUGNAME "/plantuml_path", g_plantumlPath);
	else
		g_plantumlPath = settings.value(PLUGNAME "/plantuml_path").toString();

	if (!settings.contains(PLUGNAME "/java_path"))
		settings.setValue(PLUGNAME "/java_path", g_javaPath);
	else
		g_javaPath = settings.value(PLUGNAME "/java_path").toString();

	if (!settings.contains(PLUGNAME "/plantuml_server_url"))
		settings.setValue(PLUGNAME "/plantuml_server_url", g_plantumlServerUrl);
	else
		g_plantumlServerUrl = settings.value(PLUGNAME "/plantuml_server_url").toString();

	if (!settings.contains(PLUGNAME "/mmdc_path"))
		settings.setValue(PLUGNAME "/mmdc_path", g_mmdcPath);
	else
		g_mmdcPath = settings.value(PLUGNAME "/mmdc_path").toString();

	if (!settings.contains(PLUGNAME "/mermaid_server_url"))
		settings.setValue(PLUGNAME "/mermaid_server_url", g_mermaidServerUrl);
	else
		g_mermaidServerUrl = settings.value(PLUGNAME "/mermaid_server_url").toString();

	if (!settings.contains(PLUGNAME "/timeout_ms"))
		settings.setValue(PLUGNAME "/timeout_ms", g_timeoutMs);
	else
		g_timeoutMs = settings.value(PLUGNAME "/timeout_ms").toInt();

	Dl_info dlinfo;
	static char plg_path[PATH_MAX];
	const char* loc_dir = "langs";

	memset(&dlinfo, 0, sizeof(dlinfo));

	if (dladdr(plg_path, &dlinfo) != 0)
	{
		strncpy(plg_path, dlinfo.dli_fname, PATH_MAX);
		char *pos = strrchr(plg_path, '/');

		if (pos)
			strcpy(pos + 1, loc_dir);

		setlocale(LC_ALL, "");
		bindtextdomain(GETTEXT_PACKAGE, plg_path);
		textdomain(GETTEXT_PACKAGE);
	}
}
