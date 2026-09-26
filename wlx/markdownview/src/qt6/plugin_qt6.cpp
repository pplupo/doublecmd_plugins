#include <QFile>
#include <QFileInfo>
#include <QSettings>
#include <QTextBrowser>
#include <QTextDocument>
#include <QScrollBar>
#include <QApplication>
#include <QCoreApplication>
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
#include <QPrinter>
#include <QPrintDialog>
#include <QPainter>
#include <QAbstractTextDocumentLayout>
#include <QLineEdit>
#include <QPushButton>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QResizeEvent>
#include <QTextBlock>
#include <QTextImageFormat>
#include <QTextFrame>
#include <QTextTable>
#include <QImage>
#include <QImageReader>
#include <QPixmap>
#include <QRegularExpression>
#include <QDir>
#include <QVariant>
#include <QSize>
#include <QSet>
#include <dlfcn.h>
#include <cmath>

#include "wlxplugin.h"
#include "../core/markdown_engine.h"

#define PLUGNAME "markdownview"

static bool g_autoReloadEnabled = true;
static QString g_mode = QStringLiteral("system"); // "system", "dark", "light"
static QString g_themeFilePath;
static QString g_configPath;
// Persisted "Save Zoom" font-size multiplier (1.0 = no change) -- distinct
// from m_zoomLevel below, which is QTextBrowser's own transient in-view
// zoom that resets on reload/reopen. "Save Zoom" bakes the current
// transient zoom into this instead, via a body { font-size: N% } CSS rule
// (see markdown_engine.cpp's postProcessHtml), so it survives reloads.
static double g_zoomMultiplier = 1.0;
// Empty = MarkdownEngine's own default (Latin Modern Math, the closest
// visual match to what MicroTeX rendered before this feature existed).
// Otherwise a .clm1 path from MarkdownEngine::availableMathFonts() (or one
// the user set in the ini by hand, pointing at a font of their own) --
// selection is by path, not display name, since MicroTeX's own internal
// font name frequently isn't the same string we'd show in a menu; see
// markdown_engine.cpp's resolveMathFontCanonicalName().
static QString g_mathFontClmPath;
// Figure rendering: "off" leaves ```vegalite blocks as plain text;
// anything else renders them -- see MarkdownEngine::setChartRendererMode().
// The menu writes "on"/"off"; a legacy "cairo"/"auto" left in an existing
// ini by an older build still reads as "render".
//
// In the vlcharts build figures render locally (the vendored vl-convert);
// in the light build they are POSTed to g_krokiUrl. Either way this is the
// only switch -- there is no second, local chart backend to fall back to.
static QString g_chartRenderer = QStringLiteral("on");
static bool g_mermaidEnabled = true;
static bool g_plantUmlEnabled = true;
static bool g_latexEnabled = true;
// Each notation's render service, overridable so a self-hosted instance
// can be used instead of the public one. Ini-only, no menu entries -- a
// URL isn't something to type into a context menu, and repointing one is
// a one-time setup step. Only g_krokiUrl matters in the vlcharts build,
// whose figures never leave the machine.
static QString g_mermaidUrl = QStringLiteral("https://mermaid.ink");
static QString g_plantUmlUrl = QStringLiteral("http://www.plantuml.com/plantuml");
static QString g_krokiUrl = QStringLiteral("https://kroki.io");

// Pushes the current g_chartRenderer/g_*Enabled/g_*Url globals into the
// engine's own process-global state (see
// MarkdownEngine::setChartRendererMode/setDiagramEnabled/
// setDiagramServiceUrl) -- these aren't per-render-call parameters like
// g_mathFontClmPath, so they need an explicit push after every ini load
// and every menu toggle, before the next reloadContent().
static void applyEngineRenderSettings() {
    MarkdownEngine::setChartRendererMode(g_chartRenderer.toStdString());
    MarkdownEngine::setDiagramEnabled("mermaid", g_mermaidEnabled);
    MarkdownEngine::setDiagramEnabled("plantuml", g_plantUmlEnabled);
    MarkdownEngine::setDiagramEnabled("latex", g_latexEnabled);
    MarkdownEngine::setDiagramServiceUrl("mermaid", g_mermaidUrl.toStdString());
    MarkdownEngine::setDiagramServiceUrl("plantuml", g_plantUmlUrl.toStdString());
    MarkdownEngine::setDiagramServiceUrl("vegalite", g_krokiUrl.toStdString());
}

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
    settings.setValue(PLUGNAME "/zoom_multiplier", g_zoomMultiplier);
    settings.setValue(PLUGNAME "/math_font", g_mathFontClmPath);
    settings.setValue(PLUGNAME "/chart_renderer", g_chartRenderer);
    settings.setValue(PLUGNAME "/enable_mermaid", g_mermaidEnabled);
    settings.setValue(PLUGNAME "/enable_plantuml", g_plantUmlEnabled);
    settings.setValue(PLUGNAME "/enable_latex", g_latexEnabled);
    settings.setValue(PLUGNAME "/mermaid_url", g_mermaidUrl);
    settings.setValue(PLUGNAME "/plantuml_url", g_plantUmlUrl);
    settings.setValue(PLUGNAME "/kroki_url", g_krokiUrl);
    // Explicit sync rather than relying solely on ~QSettings() to flush --
    // no known bug requires this (QSettings' destructor already syncs),
    // but it removes any doubt while diagnosing zoom persistence.
    settings.sync();
}

class MarkdownViewerWidget : public QTextBrowser {
private:
    QString m_filePath;
    QFileSystemWatcher m_watcher;
    QTimer m_debounceTimer;
    QTimer m_imageFitTimer;
    int m_zoomLevel = 0;

    // In-document incremental search (Ctrl+F), matching kpartview's
    // markdownpart backend -- this searches the loaded document's own
    // text, not the same thing as Double Commander's own Ctrl+F (that's
    // ListSearchText below, DC's own lister search UI driving find()
    // directly). A small floating bar overlaid on the top-right corner,
    // shown/hidden on demand, rather than restructuring this widget into a
    // container+child layout -- keeps ListLoad's HWND contract (this
    // widget itself) unchanged.
    QWidget* m_findBar = nullptr;
    QLineEdit* m_findEdit = nullptr;

    void ensureFindBar() {
        if (m_findBar) return;
        m_findBar = new QWidget(this);
        m_findBar->setAutoFillBackground(true);
        m_findBar->setStyleSheet("QWidget { background-color: palette(window); border: 1px solid palette(mid); }");

        m_findEdit = new QLineEdit(m_findBar);
        m_findEdit->setPlaceholderText(tr("Find in document..."));
        m_findEdit->setClearButtonEnabled(true);

        QPushButton* prevBtn = new QPushButton(tr("Prev"), m_findBar);
        QPushButton* nextBtn = new QPushButton(tr("Next"), m_findBar);
        QPushButton* closeBtn = new QPushButton(tr("✕"), m_findBar);
        closeBtn->setFixedWidth(24);

        auto* layout = new QHBoxLayout(m_findBar);
        layout->setContentsMargins(6, 4, 6, 4);
        layout->addWidget(m_findEdit);
        layout->addWidget(prevBtn);
        layout->addWidget(nextBtn);
        layout->addWidget(closeBtn);

        connect(m_findEdit, &QLineEdit::textChanged, this, [this](const QString&) { findIncremental(false); });
        connect(m_findEdit, &QLineEdit::returnPressed, this, [this]() { findIncremental(false); });
        connect(nextBtn, &QPushButton::clicked, this, [this]() { findIncremental(false); });
        connect(prevBtn, &QPushButton::clicked, this, [this]() { findIncremental(true); });
        connect(closeBtn, &QPushButton::clicked, this, &MarkdownViewerWidget::hideFindBar);

        m_findBar->hide();
    }

    void positionFindBar() {
        if (!m_findBar) return;
        int w = qMin(320, width() - 16);
        m_findBar->setFixedWidth(w);
        m_findBar->adjustSize();
        m_findBar->move(width() - w - 8, 8);
    }

    void findIncremental(bool backward) {
        if (!m_findEdit) return;
        QString text = m_findEdit->text();
        if (text.isEmpty()) return;

        QTextDocument::FindFlags flags;
        if (backward) flags |= QTextDocument::FindBackward;

        if (find(text, flags)) return;

        // Wrap around: jump the cursor to the start (forward search) or
        // end (backward search) of the document and retry once, same
        // "wraps instead of stopping at the edge" behavior as a normal
        // incremental search bar.
        QTextCursor cursor = textCursor();
        cursor.movePosition(backward ? QTextCursor::End : QTextCursor::Start);
        setTextCursor(cursor);
        find(text, flags);
    }

    void showFindBar() {
        ensureFindBar();
        positionFindBar();
        m_findBar->show();
        m_findBar->raise();
        m_findEdit->setFocus();
        m_findEdit->selectAll();
    }

    void hideFindBar() {
        if (m_findBar) m_findBar->hide();
        setFocus();
    }

    // Absolute font-size approach (not Qt's own relative zoomIn/zoomOut)
    // so it's idempotent no matter how many times reloadContent() runs --
    // no bookkeeping needed to "undo" a previous call before reapplying.
    // Confirmed live that a CSS `body { font-size: N%; }` rule has zero
    // effect on Qt's QTextDocument (measured identical rendered text width
    // at 50%/100%/182%), so this widget's own font is the only thing that
    // actually works here.
    qreal m_baseFontPointSize = 0;

    // Confirmed live that QTextBrowser::zoomIn()/zoomOut() (and, by
    // extension, the font-scaling applyZoom() does above) never touch
    // <img> sizing at all -- an image's width/height in QTextImageFormat
    // has to be scaled explicitly. Captured once right after each
    // setHtml() (see reloadContent()), before any zoom is applied, so
    // repeated applyZoom() calls (every wheel notch) always scale from the
    // true natural/unscaled size instead of compounding on top of an
    // already-scaled one.
    QVector<QSize> m_imageNaturalSizes;

    // src attribute values of <img> tags injectPlainImageSizes() sized on
    // the MOST RECENT call -- captureImageNaturalSizes() below uses this to
    // recognize a plain image (however explicitly-sized it now looks in
    // the format) and deliberately record an invalid size for it, so
    // scaleImages() -- see its own comment on why mutating a plain image's
    // format in place is unsafe -- skips it via the same "no explicit size
    // to scale from" check it already had. A diagram/equation image (never
    // in this set) is still tracked normally and still zoom-scales via the
    // cursor-mutation path, which is fine for those -- they've always been
    // explicitly sized from the moment they were first laid out, never
    // resized in place after the fact the way a plain image would be here.
    QSet<QString> m_plainImageSrcs;

    void captureImageNaturalSizes() {
        m_imageNaturalSizes.clear();
        QTextDocument *doc = document();
        if (!doc) return;
        for (QTextBlock block = doc->begin(); block.isValid(); block = block.next()) {
            for (auto it = block.begin(); !it.atEnd(); ++it) {
                QTextFragment frag = it.fragment();
                if (!frag.isValid()) continue;
                QTextCharFormat fmt = frag.charFormat();
                if (fmt.isImageFormat()) {
                    QTextImageFormat imgFmt = fmt.toImageFormat();
                    if (m_plainImageSrcs.contains(imgFmt.name())) {
                        m_imageNaturalSizes.append(QSize()); // invalid: scaleImages() skips it
                    } else {
                        m_imageNaturalSizes.append(QSize(qRound(imgFmt.width()), qRound(imgFmt.height())));
                    }
                }
            }
        }
    }

    // Qt's rich-text CSS subset doesn't support `max-width`/`max-height` on
    // <img> at all (only explicit width/height, which is why
    // markdownview.css's `img { max-width: 100%; }` is a no-op here despite
    // doing real work in the GTK3/WebKit variant) -- confirmed by
    // scaleImages()'s own "no explicit size to scale from" comment below: a
    // plain user image with no width/height HTML attribute lays out at its
    // full natural pixel size, unconstrained, however large that is.
    //
    // Two earlier approaches to fixing that both corrupted real documents
    // (a chunk of body text going invisible, with the final image
    // duplicated in its place) on any file that also has tables,
    // blockquotes, or code blocks (all three render as QTextTable frames --
    // see postProcessHtml() in markdown_engine.cpp): first, reading a
    // resource via doc->resource(QTextDocument::ImageResource, ...) mid-
    // layout; then, mutating an already-laid-out plain image's
    // QTextImageFormat via QTextCursor::setCharFormat() (even a
    // doc->markContentsDirty() covering the whole document afterwards
    // didn't fix it). scaleImages() below does that same cursor-mutation
    // pattern too and is fine -- but only because it only ever touches
    // images that ALREADY had an explicit width/height from the moment
    // they were first laid out (diagrams/equations, sized by
    // renderDiagramImgTag()/replaceMathTags() in markdown_engine.cpp);
    // it always skips plain images (natural.width() <= 0). So a plain
    // image is what specifically breaks when its format is mutated
    // in-place after the fact.
    //
    // This sidesteps the whole class of bug: the fitted width/height for
    // a plain image is computed and spliced directly into the <img> tag
    // in the HTML STRING, before it's ever handed to setHtml() at all --
    // by the time Qt lays the document out for the first time, a plain
    // image already carries an explicit size, exactly like a diagram
    // always has. No live document mutation, so nothing to corrupt.
    QString m_rawHtml; // last renderFileToHtml() output, before per-size <img> injection

    // Splices width="W" height="H" into any <img> tag that doesn't already
    // have one (a diagram/equation image always does; a plain markdown
    // image never does), sized to fit this pane (per the four rules
    // fitImageSize() implements) at the given zoom multiplier. Called
    // fresh from reapplyImageSizing() every time -- recomputing from the
    // raw string each time rather than caching results, since the "fits
    // the pane" answer depends on the pane's current size AND zoom.
    // Repopulates m_plainImageSrcs with every src this call sized, for
    // captureImageNaturalSizes() to recognize afterwards.
    QString injectPlainImageSizes(const QString &html, double zoomMultiplier) {
        m_plainImageSrcs.clear();
        QTextDocument *doc = document();
        qreal margin = doc ? doc->documentMargin() : 4.0;
        qreal pageW = (viewport()->width() - 2 * margin) / zoomMultiplier;
        qreal pageH = (viewport()->height() - 2 * margin) / zoomMultiplier;
        bool paneUsablySized = pageW >= 50 && pageH >= 50;
        QDir baseDir = QFileInfo(m_filePath).absoluteDir();

        QString out;
        out.reserve(html.size());
        static const QRegularExpression imgTagRe(QStringLiteral("<img\\b[^>]*>"));
        static const QRegularExpression srcRe(QStringLiteral("src=\"([^\"]*)\""));
        static const QRegularExpression widthRe(QStringLiteral("\\bwidth="));
        int lastEnd = 0;
        auto it = imgTagRe.globalMatch(html);
        while (it.hasNext()) {
            QRegularExpressionMatch m = it.next();
            out += html.mid(lastEnd, m.capturedStart() - lastEnd);
            QString tag = m.captured(0);
            lastEnd = m.capturedEnd();

            // Diagrams/equations already declare width/height (device-pixel
            // correct, set in markdown_engine.cpp) -- leave those alone;
            // they zoom-scale separately via scaleImages().
            if (widthRe.match(tag).hasMatch() || !paneUsablySized) {
                out += tag;
                continue;
            }
            QRegularExpressionMatch srcMatch = srcRe.match(tag);
            if (!srcMatch.hasMatch()) {
                out += tag;
                continue;
            }
            QString src = srcMatch.captured(1);
            QString localPath = baseDir.filePath(src);
            QImageReader reader(localPath);
            QSize natural = reader.size();
            if (!natural.isValid() || natural.isEmpty()) {
                out += tag;
                continue;
            }
            QSizeF fitted = fitImageSize(natural, pageW, pageH);
            tag.insert(tag.size() - 1, // just before the closing '>'
                QStringLiteral(" width=\"%1\" height=\"%2\"")
                    .arg(qRound(fitted.width() * zoomMultiplier))
                    .arg(qRound(fitted.height() * zoomMultiplier)));
            out += tag;
            m_plainImageSrcs.insert(src);
        }
        out += html.mid(lastEnd);
        return out;
    }

    // Shared by injectPlainImageSizes() above: an image that's already
    // smaller than the pane in both dimensions displays at its real size
    // (never upscaled); one too tall but not too wide fits to the pane's
    // height; one too wide but not too tall fits to the pane's width; one
    // exceeding both fits to whichever dimension is more constraining (the
    // smaller of the two scale factors), so it lands fully inside the pane
    // either way. Aspect ratio is always preserved.
    static QSizeF fitImageSize(QSize natural, qreal pageW, qreal pageH) {
        qreal w = natural.width();
        qreal h = natural.height();
        bool fitsW = w <= pageW;
        bool fitsH = h <= pageH;
        double scale;
        if (fitsW && fitsH) scale = 1.0;
        else if (fitsW && !fitsH) scale = pageH / h;
        else if (!fitsW && fitsH) scale = pageW / w;
        else scale = qMin(pageW / w, pageH / h);
        return QSizeF(w * scale, h * scale);
    }

    double effectiveZoomMultiplier() const {
        if (m_baseFontPointSize <= 0) return 1.0;
        return g_zoomMultiplier * (1.0 + 0.1 * m_zoomLevel);
    }

    // The single entry point for "make the pane match its current size AND
    // zoom level" -- called from reloadContent(), (debounced) resizeEvent(),
    // and every zoom change (wheel, Save Zoom, Reset Zoom). Re-derives the
    // fitted HTML from m_rawHtml fresh each time and does a full, ordinary
    // setHtml() with it, rather than caching/mutating -- the pane's real
    // size may not exist yet the first time reloadContent() runs (Double
    // Commander resizes this widget into its quick-view panel after
    // construction, not before). A plain setHtml() call is a full, clean
    // re-layout every time, same as any ordinary reload -- no live
    // mutation of an already-laid-out plain image, which is what corrupted
    // real documents before (see injectPlainImageSizes()'s comment above).
    // Diagram/equation images still zoom-scale via scaleImages()'s
    // existing cursor-mutation, which is fine for those -- see
    // captureImageNaturalSizes()'s comment on why plain images are
    // deliberately excluded from it.
    void reapplyImageSizing() {
        if (m_rawHtml.isEmpty()) return;
        double multiplier = effectiveZoomMultiplier();
        int currentScrollX = horizontalScrollBar() ? horizontalScrollBar()->value() : 0;
        int currentScrollY = verticalScrollBar() ? verticalScrollBar()->value() : 0;

        setHtml(injectPlainImageSizes(m_rawHtml, multiplier));
        document()->setBaseUrl(QUrl::fromLocalFile(m_filePath).adjusted(QUrl::RemoveFilename));

        QFont f = font();
        f.setPointSizeF(m_baseFontPointSize * multiplier);
        setFont(f);
        // setFont() alone does NOT retroactively rescale content already
        // loaded via setHtml() -- confirmed live via idealWidth() staying
        // identical before/after a setFont()-only call once content
        // exists. QTextDocument::setDefaultFont() is what actually forces
        // the relayout against the new base size.
        if (document()) document()->setDefaultFont(f);

        captureImageNaturalSizes();
        scaleImages(multiplier);

        if (horizontalScrollBar()) horizontalScrollBar()->setValue(currentScrollX);
        if (verticalScrollBar()) verticalScrollBar()->setValue(currentScrollY);
    }

    // Confirmed live that QTextBrowser::zoomIn()/zoomOut() never touch
    // <img> sizing at all -- an image's width/height in QTextImageFormat
    // has to be scaled explicitly. Only ever touches images NOT in
    // m_plainImageSrcs (diagrams/equations) -- see
    // captureImageNaturalSizes()'s comment for why.
    void scaleImages(double multiplier) {
        if (m_imageNaturalSizes.isEmpty()) return;
        QTextDocument *doc = document();
        if (!doc) return;
        QTextCursor cursor(doc);
        int idx = 0;
        for (QTextBlock block = doc->begin(); block.isValid(); block = block.next()) {
            for (auto it = block.begin(); !it.atEnd(); ++it) {
                QTextFragment frag = it.fragment();
                if (!frag.isValid()) continue;
                QTextCharFormat fmt = frag.charFormat();
                if (!fmt.isImageFormat()) continue;
                if (idx >= m_imageNaturalSizes.size()) break;
                QSize natural = m_imageNaturalSizes[idx++];
                if (natural.width() <= 0 || natural.height() <= 0) continue; // no explicit size to scale from
                QTextImageFormat imgFmt = fmt.toImageFormat();
                imgFmt.setWidth(natural.width() * multiplier);
                imgFmt.setHeight(natural.height() * multiplier);
                cursor.setPosition(frag.position());
                cursor.setPosition(frag.position() + frag.length(), QTextCursor::KeepAnchor);
                cursor.setCharFormat(imgFmt);
            }
        }
    }

public:
    MarkdownViewerWidget(QWidget* parent = nullptr) : QTextBrowser(parent) {
        setOpenExternalLinks(true);
        setOpenLinks(true);
        // Qt::ScrollBarAsNeeded (the default) toggles the vertical
        // scrollbar's visibility based on content height -- but showing/
        // hiding it changes the viewport's WIDTH, which rewraps the text,
        // which changes the total height, which can toggle the scrollbar
        // again: a classic reflow oscillation. Confirmed live: a document
        // made of a few long, densely-wrapped paragraphs reflows a lot
        // from a ~15-20px width change and can land right on that
        // threshold, visibly flickering the scrollbar between two states
        // from the moment the document opens (nothing to do with theme,
        // scrolling, or the print-path work elsewhere in this file --
        // this is a plain QAbstractScrollArea/QTextEdit internal
        // mechanism). A table/bullet/code-heavy document barely reflows
        // from that same width delta, which is why this was never
        // visible on those. Always reserving the scrollbar's width makes
        // the viewport width constant regardless of content height,
        // removing the oscillation entirely.
        setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOn);
        m_baseFontPointSize = font().pointSizeF();

        m_debounceTimer.setSingleShot(true);
        m_debounceTimer.setInterval(200);

        // Debounced re-fit on resize (see resizeEvent()) -- coalesces a
        // drag-resize into one re-render instead of one per intermediate
        // frame.
        m_imageFitTimer.setSingleShot(true);
        m_imageFitTimer.setInterval(50);
        connect(&m_imageFitTimer, &QTimer::timeout, this, [this]() {
            reapplyImageSizing();
        });

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
        std::string html;
        try {
            // md4c parsing, MicroTeX LaTeX rendering, and diagram
            // (mermaid/plantuml) rendering all run synchronously in here --
            // any C++ exception thrown anywhere in that chain would unwind
            // straight across this extern "C"-adjacent call into DC's
            // Pascal caller, which is undefined behavior. Confirmed live:
            // DC's own recovery from that manifests as silently falling
            // back to the next WLX plugin registered for .md (kpartview
            // here) on the NEXT open, not a visible crash -- exactly why
            // markdownview_gtk3's ListLoad already wraps this same call the
            // same way.
            // Render diagrams and figures at this widget's actual device
            // pixel ratio. An image produced at one scale and displayed at
            // another is what makes an otherwise clean render look soft or
            // aliased -- QTextDocument rescales image data without
            // smoothing. Generating at device resolution while declaring
            // the logical size in the HTML makes the mapping 1:1.
            //
            // MarkdownEngine's setter, not VegaLite's: the latter is a
            // no-op stub in the build without vl-convert compiled in, so
            // calling it there silently dropped the ratio and left every
            // web-rendered image hardcoded to 2x.
            MarkdownEngine::setDisplayScale(devicePixelRatioF());
            html = MarkdownEngine::renderFileToHtml(
                m_filePath.toStdString(),
                activeDarkMode,
                g_themeFilePath.toStdString(),
                g_mathFontClmPath.toStdString()
            );
        } catch (const std::exception &e) {
            setHtml(QStringLiteral("<p>markdownview: failed to render this file: %1</p>").arg(QString::fromUtf8(e.what())));
            return;
        } catch (...) {
            setHtml(QStringLiteral("<p>markdownview: failed to render this file (unknown error).</p>"));
            return;
        }
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

        m_rawHtml = QString::fromStdString(html);
        reapplyImageSizing(); // does its own setHtml() + scroll preservation

        if (QFile::exists(m_filePath) && !m_watcher.files().contains(m_filePath)) {
            m_watcher.addPath(m_filePath);
        }
    }

    void copySelection() {
        if (textCursor().hasSelection()) {
            copy();
        }
    }

    // Prints the SAME QTextDocument this widget already has on screen --
    // whatever theme/CSS reloadContent() most recently applied (dark or
    // light, default or custom) is exactly what ends up on the page, with
    // no separate print stylesheet involved. NOT via QTextDocument::print()
    // directly, though: that call only paints the document's own laid-out
    // content, never the physical page itself -- on the dark theme the
    // printer's native white paper showed through as a border around the
    // (still dark-colored) text. Paint each page's background first, then
    // draw the document's content on top of it.
    // A plain image's width/height (injectPlainImageSizes()) is baked in
    // to fit the SCREEN viewport, in that viewport's own device-pixel
    // scale. printer.pageRect(QPrinter::DevicePixel) below is in the
    // PRINTER's device-pixel scale, which is a different number of pixels
    // per inch -- so an image already fitted to (say) an 900px-wide screen
    // pane can be wider than a print page whose printable area happens to
    // be fewer device pixels across at the printer/PDF-writer's resolution.
    // Nothing upstream re-fits it for that new width, so it bleeds past
    // the right margin exactly like body text doesn't (text reflows to
    // whatever width doc->setPageSize() below gives it; a fixed-size image
    // format does not). Clamped here, once, against the real print content
    // width, same cursor-mutation pattern as scaleImages() above -- safe
    // here specifically because this is a clone (see the big comment on
    // m_rawHtml above for why mutating a *live* image format was the thing
    // that corrupted real documents; this document is thrown away after
    // this function returns).
    void clampImagesToWidth(QTextDocument *doc, qreal availableWidth) {
        if (availableWidth <= 0) return;
        QTextCursor cursor(doc);
        for (QTextBlock block = doc->begin(); block.isValid(); block = block.next()) {
            for (auto it = block.begin(); !it.atEnd(); ++it) {
                QTextFragment frag = it.fragment();
                if (!frag.isValid()) continue;
                QTextCharFormat fmt = frag.charFormat();
                if (!fmt.isImageFormat()) continue;
                QTextImageFormat imgFmt = fmt.toImageFormat();
                if (imgFmt.width() <= availableWidth) continue;
                qreal scale = availableWidth / imgFmt.width();
                imgFmt.setWidth(imgFmt.width() * scale);
                imgFmt.setHeight(imgFmt.height() * scale);
                cursor.setPosition(frag.position());
                cursor.setPosition(frag.position() + frag.length(), QTextCursor::KeepAnchor);
                cursor.setCharFormat(imgFmt);
            }
        }
    }

    void printDocument() {
        QPrinter printer;
        QPrintDialog dialog(&printer, this);
        if (dialog.exec() != QDialog::Accepted) return;

        QColor pageColor = resolveDarkMode() ? QColor("#0d1117") : QColor("#ffffff");

        // setFullPage(true), instead of the default printer.pageRect()
        // this used to use: by default a QPrinter's
        // paint-device origin is already inset from the physical paper
        // edge by the driver's hardware margins, so a QPainter drawing at
        // (0,0) is drawing at the inside edge of that margin, not the
        // paper's actual corner -- fillRect() below could only ever reach
        // as far as that inset boundary, leaving the driver's own margin
        // strip outside the painter's addressable space entirely (shows
        // as a plain white/default border on a dark-theme print, no
        // matter what color is asked for). Painting the full physical
        // page requires opting into that coordinate space explicitly.
        printer.setFullPage(true);
        // Not pageLayout.fullRectPixels(): that converts the page's
        // physical size to pixels itself (point size * resolution / 72,
        // rounded into an integer QRect), which can come out fractionally
        // smaller than what the paint device actually reports -- and
        // since painting starts at (0,0), any such shortfall only shows
        // up on the far edges (right/bottom), never the near ones
        // (confirmed live: left/top were flush, right/bottom had a
        // shrunk-but-nonzero sliver of the old white border left). Asking
        // the printer's own paint-device metrics directly guarantees this
        // matches the exact coordinate space the QPainter below draws in.
        QSizeF fullPageSize(printer.width(), printer.height());
        // NOT printer.setPageMargins()/the printer's own margin rect:
        // setFullPage(true) above makes the printable area equal the
        // whole physical page, which makes that margin concept
        // moot in that mode -- confirmed live, setPageMargins() here had
        // no visible effect at all. Computing the inset ourselves, in the
        // same full-page coordinate space fullPageSize already uses,
        // keeps the two consistent by construction and can't reintroduce
        // the original white-border bug: contentRect is still entirely
        // inside the area fillRect() below paints solid, nothing is ever
        // drawn outside the painter's addressable full-page space.
        qreal marginPx = 0.5 * printer.resolution();
        QRect contentRect(qRound(marginPx), qRound(marginPx),
                           qRound(fullPageSize.width() - 2 * marginPx),
                           qRound(fullPageSize.height() - 2 * marginPx));

        QTextDocument *doc = document()->clone();
        QSizeF pageSize = contentRect.size();
        clampImagesToWidth(doc, pageSize.width() - 2 * doc->documentMargin());
        doc->setPageSize(pageSize);
        for (QTextFrame::iterator it = doc->rootFrame()->begin(); !it.atEnd(); ++it) {
            QTextTable *table = qobject_cast<QTextTable *>(it.currentFrame());
            if (!table) continue;
            QTextTableFormat fmt = table->format();
            if (fmt.width().type() != QTextLength::PercentageLength) continue;
            fmt.setWidth(QTextLength(QTextLength::FixedLength, pageSize.width()));
            table->setFormat(fmt);
        }

        QPainter painter(&printer);
        int pageCount = doc->pageCount();
        for (int page = 0; page < pageCount; ++page) {
            if (page > 0) printer.newPage();
            // Measured directly against real print output: a fillRect
            // sized to EXACTLY fullPageSize leaves a literal 1-device-pixel
            // white line uncovered along the right and bottom edges (never
            // left/top) on every page -- a sub-pixel rounding shortfall
            // between what printer.width()/height() report and where the
            // PDF backend's own page boundary actually falls. Padding the
            // fill a couple pixels past the reported size in every
            // direction closes that gap; the backend clips anything past
            // the real page edge on its own, so overshooting here is safe.
            painter.fillRect(QRectF(QPointF(-2, -2), fullPageSize + QSizeF(4, 4)), pageColor);

            QAbstractTextDocumentLayout::PaintContext ctx;
            // A default-constructed PaintContext's palette is the AMBIENT
            // system palette, not this widget's own -- reloadContent()
            // already sets this widget's QPalette::Text/WindowText to
            // match the active theme (see its comment on why a text run
            // with no explicit CSS color falls back to the paint
            // context's palette). Without this, a light document on a
            // dark-system machine printed with the system's light/white
            // Text color on this print path's explicitly light page
            // background -- invisible text, only visible once selected
            // (a different color pair). Reusing the already-correct
            // widget palette here keeps print consistent with the screen.
            ctx.palette = palette();
            ctx.clip = QRectF(0, page * pageSize.height(), pageSize.width(), pageSize.height());
            painter.save();
            painter.translate(contentRect.topLeft());
            painter.translate(0, -page * pageSize.height());
            doc->documentLayout()->draw(&painter, ctx);
            painter.restore();
        }
        delete doc;
    }

    // Back to the factory default size -- clears BOTH the transient
    // in-view zoom and any persisted "Save Zoom" multiplier, unlike
    // saveZoom() which folds the transient zoom into the persisted one.
    void resetZoom() {
        m_zoomLevel = 0;
        bool wasPersisted = (g_zoomMultiplier != 1.0);
        g_zoomMultiplier = 1.0;
        reapplyImageSizing();
        if (wasPersisted) saveSettings();
    }

    // Persists the CURRENT transient zoom (each wheel notch = 10%) into
    // g_zoomMultiplier, then clears the transient zoom back to neutral --
    // the total effective zoom (persisted * transient) stays exactly the
    // same, but it's now entirely in g_zoomMultiplier and survives a
    // reload/reopen, unlike m_zoomLevel alone. Reset Zoom (above) discards
    // this back to the factory default instead.
    void saveZoom() {
        if (m_zoomLevel == 0) return;
        g_zoomMultiplier *= (1.0 + 0.1 * m_zoomLevel);
        if (g_zoomMultiplier < 0.1) g_zoomMultiplier = 0.1;
        m_zoomLevel = 0; // the delta is now folded into g_zoomMultiplier; don't double-apply it
        reapplyImageSizing();
        saveSettings();
    }

protected:
    void resizeEvent(QResizeEvent* event) override {
        QTextBrowser::resizeEvent(event);
        positionFindBar();
        // Re-fit plain images against the pane's new size -- the pane's
        // real size (Double Commander resizing this widget into its
        // quick-view panel) may not exist yet the first time reloadContent()
        // runs. Debounced via m_imageFitTimer so a drag-resize coalesces
        // into one re-render instead of one per intermediate frame.
        //
        // Only worth doing at all if there's a plain <img> to fit --
        // reapplyImageSizing() otherwise does a full setHtml() reload for
        // zero benefit (ordinary text reflow on resize is already Qt's
        // own doing, not something this timer drives). Double Commander
        // settling its quick-view panel into its final size fires several
        // resizeEvent()s in quick succession right on load; an image-less
        // document used to pay for a full document rebuild on every one
        // of them, which is real, visible work landing in the same
        // fraction of a second the scrollbar was still settling --
        // confirmed live as the remaining "resizes a few times on load"
        // quirk on exactly the same files that had the reflow-oscillation
        // bug fixed by setVerticalScrollBarPolicy() above.
        if (!m_rawHtml.isEmpty() && m_rawHtml.contains(QLatin1String("<img"))) {
            m_imageFitTimer.start();
        }
    }

    void keyPressEvent(QKeyEvent* event) override {
        if (event->matches(QKeySequence::Find)) {
            showFindBar();
            event->accept();
            return;
        }
        if (event->key() == Qt::Key_Escape && m_findBar && m_findBar->isVisible()) {
            hideFindBar();
            event->accept();
            return;
        }
        if (event->modifiers() & Qt::ControlModifier) {
            if (event->key() == Qt::Key_Q) {
                // DC's own hotkey manager (Ctrl+Q closes Quick View) never
                // sees key events this widget handles locally -- it's a
                // real embedded QWidget, but across a native-window
                // boundary, so key events consumed here don't reach DC's
                // top-level window. Repost it there explicitly, matching
                // the pattern used by kpartview/logview/pdfview for the
                // same problem.
                QWidget* target = QApplication::activeWindow();
                if (!target) target = window();
                if (target) {
                    QCoreApplication::postEvent(target, new QKeyEvent(QEvent::KeyPress, Qt::Key_Q, Qt::ControlModifier));
                    QCoreApplication::postEvent(target, new QKeyEvent(QEvent::KeyRelease, Qt::Key_Q, Qt::ControlModifier));
                }
                return;
            }
        }
        QTextBrowser::keyPressEvent(event);
    }

    void wheelEvent(QWheelEvent* event) override {
        if (event->modifiers() & Qt::ControlModifier) {
            if (event->angleDelta().y() > 0) {
                m_zoomLevel++;
                reapplyImageSizing();
            } else if (event->angleDelta().y() < 0) {
                m_zoomLevel--;
                reapplyImageSizing();
            }
            event->accept();
        } else {
            QTextBrowser::wheelEvent(event);
        }
    }

    void contextMenuEvent(QContextMenuEvent* event) override {
        QMenu menu(this);

        QAction* copyAction = menu.addAction(tr("Copy"));
        copyAction->setEnabled(textCursor().hasSelection());
        connect(copyAction, &QAction::triggered, this, &MarkdownViewerWidget::copySelection);

        QAction* selectAllAction = menu.addAction(tr("Select All"));
        connect(selectAllAction, &QAction::triggered, this, &MarkdownViewerWidget::selectAll);

        QAction* findAction = menu.addAction(tr("Find in Document..."));
        findAction->setShortcut(QKeySequence::Find);
        connect(findAction, &QAction::triggered, this, &MarkdownViewerWidget::showFindBar);

        QAction* printAction = menu.addAction(tr("Print..."));
        connect(printAction, &QAction::triggered, this, &MarkdownViewerWidget::printDocument);

        menu.addSeparator();

        QAction* saveZoomAction = menu.addAction(tr("Save Zoom"));
        saveZoomAction->setEnabled(m_zoomLevel != 0);
        connect(saveZoomAction, &QAction::triggered, this, &MarkdownViewerWidget::saveZoom);

        QAction* resetZoomAction = menu.addAction(tr("Reset Zoom"));
        resetZoomAction->setEnabled(m_zoomLevel != 0 || g_zoomMultiplier != 1.0);
        connect(resetZoomAction, &QAction::triggered, this, &MarkdownViewerWidget::resetZoom);

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

        QMenu* mathFontMenu = menu.addMenu(tr("Math Font"));
        QActionGroup* mathFontGroup = new QActionGroup(mathFontMenu);
        mathFontGroup->setExclusive(true);

        QAction* fontDefault = mathFontMenu->addAction(tr("Default"));
        fontDefault->setCheckable(true);
        fontDefault->setChecked(g_mathFontClmPath.isEmpty());
        mathFontGroup->addAction(fontDefault);
        connect(fontDefault, &QAction::triggered, this, [this]() {
            g_mathFontClmPath.clear();
            saveSettings();
            reloadContent();
        });

        mathFontMenu->addSeparator();

        // MarkdownEngine::init() must have already run (reloadContent()
        // above always calls it) before availableMathFonts() has anything
        // to report -- true here since this is a context menu on an
        // already-loaded document.
        for (const MarkdownEngine::MathFontInfo &font : MarkdownEngine::availableMathFonts()) {
            QString qDisplayName = QString::fromStdString(font.displayName);
            QString qClmPath = QString::fromStdString(font.clmPath);
            QAction* fontAction = mathFontMenu->addAction(qDisplayName);
            fontAction->setCheckable(true);
            fontAction->setChecked(g_mathFontClmPath == qClmPath);
            mathFontGroup->addAction(fontAction);
            connect(fontAction, &QAction::triggered, this, [this, qClmPath]() {
                g_mathFontClmPath = qClmPath;
                saveSettings();
                reloadContent();
            });
        }

        auto addDiagramToggle = [&](const QString &label, bool &flag) {
            QAction* a = menu.addAction(label);
            a->setCheckable(true);
            a->setChecked(flag);
            // Captures a pointer, not a reference to the `flag` parameter
            // itself -- that parameter is a local binding that would be
            // dangling by the time this lambda actually fires (Qt's
            // QAction::triggered is asynchronous, long after
            // addDiagramToggle() returns).
            bool *flagPtr = &flag;
            connect(a, &QAction::triggered, this, [this, flagPtr](bool checked) {
                *flagPtr = checked;
                saveSettings();
                applyEngineRenderSettings();
                reloadContent();
            });
        };
        // Plain on/off, where a three-way "Chart Renderer" submenu used to
        // offer Matplot++/Cairo/Disabled. Those backends are gone -- one
        // renderer remains, so a backend choice would be a menu of one.
        QAction* chartAction = menu.addAction(tr("Render Vega-Lite Charts"));
        chartAction->setCheckable(true);
        chartAction->setChecked(g_chartRenderer != QStringLiteral("off"));
        connect(chartAction, &QAction::triggered, this, [this](bool checked) {
            g_chartRenderer = checked ? QStringLiteral("on") : QStringLiteral("off");
            saveSettings();
            applyEngineRenderSettings();
            reloadContent();
        });

        addDiagramToggle(tr("Render Mermaid Diagrams"), g_mermaidEnabled);
        addDiagramToggle(tr("Render PlantUML Diagrams"), g_plantUmlEnabled);
        addDiagramToggle(tr("Render LaTeX Math"), g_latexEnabled);

        menu.exec(event->globalPos());
    }
};

extern "C" {

// Function-try-block: reloadContent() (called from loadFile() below) is
// itself exception-safe now, but widget construction/show() could still
// throw in principle -- same defensive boundary markdownview_gtk3's
// ListLoad already has, for the same reason (see reloadContent()'s comment
// above).
HWND DCPCALL ListLoad(HWND ParentWin, char* FileToLoad, int ShowFlags)
try {
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
} catch (const std::exception &e) {
    fprintf(stderr, "[markdownview_qt6] ListLoad EXCEPTION: %s\n", e.what());
    return nullptr;
} catch (...) {
    fprintf(stderr, "[markdownview_qt6] ListLoad UNKNOWN EXCEPTION\n");
    return nullptr;
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

    if (!settings.contains(PLUGNAME "/zoom_multiplier"))
        settings.setValue(PLUGNAME "/zoom_multiplier", g_zoomMultiplier);
    else
        g_zoomMultiplier = settings.value(PLUGNAME "/zoom_multiplier").toDouble();

    if (!settings.contains(PLUGNAME "/math_font"))
        settings.setValue(PLUGNAME "/math_font", g_mathFontClmPath);
    else
        g_mathFontClmPath = settings.value(PLUGNAME "/math_font").toString();

    if (!settings.contains(PLUGNAME "/chart_renderer"))
        settings.setValue(PLUGNAME "/chart_renderer", g_chartRenderer);
    else
        g_chartRenderer = settings.value(PLUGNAME "/chart_renderer").toString().toLower();

    if (!settings.contains(PLUGNAME "/enable_mermaid"))
        settings.setValue(PLUGNAME "/enable_mermaid", g_mermaidEnabled);
    else
        g_mermaidEnabled = settings.value(PLUGNAME "/enable_mermaid").toBool();

    if (!settings.contains(PLUGNAME "/enable_plantuml"))
        settings.setValue(PLUGNAME "/enable_plantuml", g_plantUmlEnabled);
    else
        g_plantUmlEnabled = settings.value(PLUGNAME "/enable_plantuml").toBool();

    if (!settings.contains(PLUGNAME "/enable_latex"))
        settings.setValue(PLUGNAME "/enable_latex", g_latexEnabled);
    else
        g_latexEnabled = settings.value(PLUGNAME "/enable_latex").toBool();

    if (!settings.contains(PLUGNAME "/mermaid_url"))
        settings.setValue(PLUGNAME "/mermaid_url", g_mermaidUrl);
    else
        g_mermaidUrl = settings.value(PLUGNAME "/mermaid_url").toString();

    if (!settings.contains(PLUGNAME "/plantuml_url"))
        settings.setValue(PLUGNAME "/plantuml_url", g_plantUmlUrl);
    else
        g_plantUmlUrl = settings.value(PLUGNAME "/plantuml_url").toString();

    if (!settings.contains(PLUGNAME "/kroki_url"))
        settings.setValue(PLUGNAME "/kroki_url", g_krokiUrl);
    else
        g_krokiUrl = settings.value(PLUGNAME "/kroki_url").toString();

    applyEngineRenderSettings();
}

} // extern "C"
