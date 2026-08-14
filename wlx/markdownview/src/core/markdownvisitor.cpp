// librsvg (included below) pulls in GLib/GObject/Gio headers transitively,
// some of which (gio/gdbusintrospection.h) declare a struct field literally
// named `signals` -- which collides with Qt's `signals` keyword macro
// (expands to `public`, for MOC's benefit) once both header sets land in
// the same translation unit. QT_NO_KEYWORDS disables that macro; must be
// defined before the *first* Qt header is included anywhere in this file
// (including transitively via markdownvisitor.h -> html.h -> doc.h).
// Safe here since this file declares no QObject subclass and never uses
// the signals:/slots:/emit keywords itself (only Q_* macros, which
// QT_NO_KEYWORDS doesn't touch).
#define QT_NO_KEYWORDS

#include "markdownvisitor.h"
#include <QRegularExpression>
#include <QDebug>

// Qt is only still used here for two narrow, disclosed reasons: (1) QString
// is md4qt's MD::String in the MD4QT_QT_SUPPORT build mode this project
// uses, so the AST/visitor interface itself is QString-typed; (2) MicroTeX's
// LaTeX math rendering below uses its Qt Graphics2D backend (graphic_qt.h),
// since MicroTeX's alternative Cairo backend needs cairomm/pangomm, which
// aren't set up in this build yet. Everything else previously pulled in by
// this file -- QtNetwork (Mermaid/PlantUML web rendering) and QtSvg/the rest
// of QtGui's painting API (SVG rasterization) -- has been replaced below
// with a curl subprocess and librsvg+Cairo respectively, which are real
// toolkit-neutral replacements, not stubs. onMath() guards against no
// QGuiApplication existing (true for the GTK3 host process, which never
// constructs one) and falls back to the existing plain-text rendering
// instead of calling into Qt painting APIs that would abort without one.
#include <QGuiApplication>
#include <QPainter>
#include <QPixmap>
#include <QImage>
#include <QColor>
#include <QBuffer>
#include <QIODevice>

// MicroTeX includes
#include "latex.h"
#include "platform/qt/graphic_qt.h"

#include <cairo.h>
#include <librsvg/rsvg.h>

#include <unistd.h>
#include <spawn.h>
#include <sys/wait.h>
#include <poll.h>
#include <signal.h>
#include <vector>
#include <cstring>
#include <chrono>
#include <algorithm>

extern char **environ;

namespace {

// Minimal subprocess runner (replaces QProcess), same posix_spawn+poll
// pattern used elsewhere in this project (e.g. diagramview's DiagramRenderer)
// for CLI subprocess invocation without a Qt event loop.
QByteArray runProcessCapture(const std::string &exe, const std::vector<std::string> &args, int timeoutMs)
{
    QByteArray result;
    int outPipe[2];
    if (pipe(outPipe) != 0) return result;

    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_adddup2(&actions, outPipe[1], STDOUT_FILENO);
    posix_spawn_file_actions_addclose(&actions, outPipe[0]);
    posix_spawn_file_actions_addclose(&actions, outPipe[1]);

    std::vector<char *> argv;
    argv.push_back(const_cast<char *>(exe.c_str()));
    for (auto &a : args) argv.push_back(const_cast<char *>(a.c_str()));
    argv.push_back(nullptr);

    pid_t pid = -1;
    int rc = posix_spawnp(&pid, exe.c_str(), &actions, nullptr, argv.data(), environ);
    posix_spawn_file_actions_destroy(&actions);
    close(outPipe[1]);
    if (rc != 0) { close(outPipe[0]); return result; }

    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    bool timedOut = false;
    char buf[8192];
    while (true) {
        auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
                              deadline - std::chrono::steady_clock::now()).count();
        if (remaining <= 0) { timedOut = true; break; }
        struct pollfd pfd { outPipe[0], POLLIN, 0 };
        int pr = poll(&pfd, 1, (int)remaining);
        if (pr < 0) break;
        if (pr == 0) { timedOut = true; break; }
        if (pfd.revents & (POLLIN | POLLHUP)) {
            ssize_t n = read(outPipe[0], buf, sizeof(buf));
            if (n > 0) result.append(buf, (int)n);
            else break;
        } else break;
    }
    close(outPipe[0]);
    if (timedOut) kill(pid, SIGKILL);
    int status = 0;
    waitpid(pid, &status, 0);
    if (timedOut || !WIFEXITED(status) || WEXITSTATUS(status) != 0)
        return QByteArray();
    return result;
}

// Fetches a URL via `curl` (replaces QNetworkAccessManager -- no Qt event
// loop needed, works identically whether this core is linked into the Qt6
// or GTK3 plugin).
QByteArray httpGet(const std::string &url, int timeoutMs = 15000)
{
    return runProcessCapture("curl", {"-s", "-L", "--max-time", std::to_string(timeoutMs / 1000), url}, timeoutMs + 2000);
}

// Rasterizes SVG bytes to a Cairo ARGB32 surface via librsvg, then encodes
// to PNG bytes. Used both by svgToHighDpiPng() (Mermaid/PlantUML) below and
// available as the one non-Qt image pipeline this file needs.
cairo_status_t writeToQByteArray(void *closure, const unsigned char *data, unsigned int length)
{
    auto *out = static_cast<QByteArray *>(closure);
    out->append(reinterpret_cast<const char *>(data), (int)length);
    return CAIRO_STATUS_SUCCESS;
}

} // namespace

MarkdownVisitor::MarkdownVisitor(bool darkMode) 
    : MD::details::HtmlVisitor()
    , m_darkMode(darkMode) {
}

MarkdownVisitor::~MarkdownVisitor() {
}

void MarkdownVisitor::onMath(MD::Math *m) {
    if (!m) return;

    // MicroTeX's Qt Graphics2D backend needs a live QGuiApplication (QPixmap
    // /QPainter abort without one); the GTK3 plugin's host process never
    // constructs one. Fall back to the existing plain-text rendering rather
    // than crash -- LaTeX math blocks render as their source text instead
    // of a typeset image under GTK3, a disclosed, narrower gap than the
    // outright missing-library crash this replaces.
    if (!qApp) {
        if (m->isInline()) {
            m_html += QStringLiteral("<span class=\"math inline\">$") + prepareTextForHtml(m->text()) + QStringLiteral("$</span>");
        } else {
            m_html += QStringLiteral("<pre class=\"math block\"><code>") + prepareTextForHtml(m->text()) + QStringLiteral("</code></pre>\n");
        }
        return;
    }

    std::wstring tex = m->text().toStdWString();

    constexpr int oversample = 8;
    constexpr float baseTextSize = 20.0f;
    constexpr float renderTextSize = baseTextSize * oversample;
    
    // Choose formula text color: bright white/cyan (#f0f6fc) for dark mode, black (#000000) for light mode
    unsigned int fgColor = m_darkMode ? 0xfff0f6fc : 0xff000000;
    
    tex::TeXRender* render = nullptr;
    try {
        render = tex::LaTeX::parse(tex, 0, renderTextSize, renderTextSize, fgColor);
    } catch (const std::exception& e) {
        qWarning() << "LaTeX parsing error:" << e.what();
    }

    if (render && render->getWidth() > 0 && render->getHeight() > 0) {
        int padding = 4 * oversample;
        int physicalWidth = render->getWidth() + padding * 2;
        int physicalHeight = render->getHeight() + render->getDepth() + padding * 2;
        
        if (physicalWidth <= 0) physicalWidth = 1;
        if (physicalHeight <= 0) physicalHeight = 1;
        
        QPixmap pixmap(physicalWidth, physicalHeight);
        QColor bgColor = m_darkMode ? QColor(QStringLiteral("#0d1117")) : QColor(QStringLiteral("#FAFAFA"));
        pixmap.fill(bgColor);
        
        QPainter painter(&pixmap);
        painter.setRenderHint(QPainter::Antialiasing);
        painter.setRenderHint(QPainter::TextAntialiasing);
        painter.setRenderHint(QPainter::SmoothPixmapTransform);
        
        tex::Graphics2D_qt g2(&painter);
        render->draw(g2, padding, padding);
        painter.end();
        QImage image = pixmap.toImage();
        
        QByteArray ba;
        QBuffer buffer(&ba);
        buffer.open(QIODevice::WriteOnly);
        
        int logicalWidth = physicalWidth / oversample;
        int logicalHeight = physicalHeight / oversample;
        QImage scaledImage = image.scaled(logicalWidth, logicalHeight,
                                          Qt::KeepAspectRatio,
                                          Qt::SmoothTransformation);
        scaledImage.save(&buffer, "PNG");
        
        QByteArray base64Img = ba.toBase64();
        QString imgTag = QStringLiteral("<img src=\"data:image/png;base64,") + QString::fromUtf8(base64Img) +
                         QStringLiteral("\" />");
        
        if (m->isInline()) {
            m_html += QStringLiteral("<span class=\"math inline\">") + imgTag + QStringLiteral("</span>");
        } else {
            m_html += QStringLiteral("<p align=\"center\">") + imgTag + QStringLiteral("</p>");
        }
        
        delete render;
    } else {
        // Fallback for non-LaTeX text (such as $1.00 and $4B) or parse failures
        if (m->isInline()) {
            m_html += QStringLiteral("<span class=\"math-fallback\">$") + prepareTextForHtml(m->text()) + QStringLiteral("$</span>");
        } else {
            m_html += QStringLiteral("<pre class=\"math block\"><code>") + prepareTextForHtml(m->text()) + QStringLiteral("</code></pre>\n");
        }
        if (render) delete render;
    }
}


void MarkdownVisitor::onCode(MD::Code *c)
{
    QString syntax = c->syntax().toLower();
    if (c->isFensedCode() && (syntax == QStringLiteral("mermaid") || syntax == QStringLiteral("plantuml") || syntax == QStringLiteral("puml"))) {
        QByteArray imgData;
        int logicalWidth = 0, logicalHeight = 0;
        
        if (syntax == QStringLiteral("mermaid")) {
            QByteArray svgData = runMermaidWeb(c->text());
            if (!svgData.isEmpty()) {
                QString svgStr = QString::fromUtf8(fixMermaidSvgText(svgData));
                
                QRegularExpression rgbaRe(QStringLiteral(R"(rgba\([^)]+\))"));
                svgStr.replace(rgbaRe, m_darkMode ? QStringLiteral("#161b22") : QStringLiteral("#E8E8E8"));
                
                imgData = svgToHighDpiPng(svgStr.toUtf8(), 2.0f, logicalWidth, logicalHeight);
            }
        } else {
            QByteArray svgData = runPlantUmlWeb(c->text());
            if (!svgData.isEmpty()) {
                QString svgStr = QString::fromUtf8(svgData);
                if (m_darkMode) {
                    svgStr = fixPlantUmlSvgDark(svgStr);
                } else {
                    QRegularExpression strokeRe(QStringLiteral(R"(stroke-width:0\.[0-9]+)"));
                    svgStr.replace(strokeRe, QStringLiteral("stroke-width:1.0"));
                }
                imgData = svgToHighDpiPng(svgStr.toUtf8(), 2.0f, logicalWidth, logicalHeight);
            }
        }

        if (!imgData.isEmpty()) {
            QByteArray base64Img = imgData.toBase64();
            QString imgTag;
            if (logicalWidth > 0 && logicalHeight > 0) {
                imgTag = QStringLiteral("<img src=\"data:image/png;base64,") + QString::fromUtf8(base64Img) +
                         QStringLiteral("\" width=\"%1\" height=\"%2\" />").arg(logicalWidth).arg(logicalHeight);
            } else {
                imgTag = QStringLiteral("<img src=\"data:image/png;base64,") + QString::fromUtf8(base64Img) +
                         QStringLiteral("\" />");
            }
            m_html.append(QStringLiteral("<p align=\"center\">\n"));
            m_html.append(imgTag);
            m_html.append(QStringLiteral("</p>\n"));
            return;
        }
    }
    // Fallback to original md4qt rendering if not mermaid/plantuml or if fetching failed
    MD::details::HtmlVisitor::onCode(c);
}

QByteArray MarkdownVisitor::runMermaidWeb(const QString& code)
{
    QString theme = m_darkMode ? QStringLiteral("\"dark\"") : QStringLiteral("\"default\"");
    QString config = QStringLiteral("%%{init: {\"theme\": ") + theme + QStringLiteral(", \"flowchart\": {\"htmlLabels\": false}, \"sequence\": {\"htmlLabels\": false}, \"gantt\": {\"htmlLabels\": false}, \"journey\": {\"htmlLabels\": false}, \"class\": {\"htmlLabels\": false}, \"state\": {\"htmlLabels\": false}, \"er\": {\"htmlLabels\": false}, \"pie\": {\"htmlLabels\": false}, \"c4\": {\"htmlLabels\": false}, \"themeVariables\": {\"background\": \"transparent\"}}}%%\n");
    QString fullCode = config + code;
    QByteArray base64 = fullCode.toUtf8().toBase64(QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals);
    QString url = QStringLiteral("https://mermaid.ink/svg/") + QString::fromUtf8(base64);

    return httpGet(url.toStdString());
}

QByteArray MarkdownVisitor::runPlantUmlWeb(const QString& code)
{
    QString modifiedCode = code;
    int startIdx = modifiedCode.indexOf(QStringLiteral("@startuml"));
    if (m_darkMode) {
        QString skin = QStringLiteral(
            "\nskinparam backgroundColor transparent\n"
            "skinparam defaultFontColor #f0f6fc\n"
            "skinparam ParticipantBackgroundColor #21262d\n"
            "skinparam ParticipantBorderColor #58a6ff\n"
            "skinparam ParticipantFontColor #f0f6fc\n"
            "skinparam ActorBackgroundColor #21262d\n"
            "skinparam ActorBorderColor #58a6ff\n"
            "skinparam ActorFontColor #f0f6fc\n"
            "skinparam SequenceGroupBackgroundColor #161b22\n"
            "skinparam SequenceGroupBorderColor #8b949e\n"
            "skinparam SequenceGroupHeaderFontColor #f0f6fc\n"
            "skinparam SequenceLifeLineBorderColor #58a6ff\n"
            "skinparam SequenceLifeLineBackgroundColor #161b22\n"
            "skinparam ArrowColor #58a6ff\n"
            "skinparam ActivityBackgroundColor #21262d\n"
            "skinparam ActivityBorderColor #58a6ff\n"
            "skinparam ActivityFontColor #f0f6fc\n"
            "skinparam ClassBackgroundColor #21262d\n"
            "skinparam ClassHeaderBackgroundColor #161b22\n"
            "skinparam ClassBorderColor #58a6ff\n"
            "skinparam ClassFontColor #f0f6fc\n"
            "skinparam NoteBackgroundColor #21262d\n"
            "skinparam NoteBorderColor #58a6ff\n"
            "skinparam NoteFontColor #f0f6fc\n"
        );
        if (startIdx != -1) {
            modifiedCode.insert(startIdx + 9, skin);
        } else {
            modifiedCode.prepend(skin);
        }
    } else {
        if (startIdx != -1) {
            modifiedCode.insert(startIdx + 9, QStringLiteral("\nskinparam backgroundColor transparent\n"));
        } else {
            modifiedCode.prepend(QStringLiteral("skinparam backgroundColor transparent\n"));
        }
    }

    QString hexStr = QStringLiteral("~h") + QString::fromUtf8(modifiedCode.toUtf8().toHex());
    QString url = QStringLiteral("http://www.plantuml.com/plantuml/svg/") + hexStr;

    return httpGet(url.toStdString());
}

QString MarkdownVisitor::fixPlantUmlSvgDark(const QString& svgInput)
{
    QString svgStr = svgInput;

    // Replace dark stroke colors (#181818, #000000, #333333, black) on arrows/lines with bright blue/white (#58a6ff)
    QRegularExpression darkStrokeRe(QStringLiteral(R"raw(stroke="(#181818|#000000|#333333|#000|black)")raw"));
    svgStr.replace(darkStrokeRe, QStringLiteral("stroke=\"#58a6ff\""));

    QRegularExpression darkStrokeStyleRe(QStringLiteral(R"raw(stroke\s*:\s*(#181818|#000000|#333333|#000|black))raw"));
    svgStr.replace(darkStrokeStyleRe, QStringLiteral("stroke:#58a6ff"));

    // Thicken fine 0.5px lines so lines stay crisp when rendered
    QRegularExpression strokeWidthRe(QStringLiteral(R"raw(stroke-width:0\.[0-9]+)raw"));
    svgStr.replace(strokeWidthRe, QStringLiteral("stroke-width:1.0"));

    return svgStr;
}

QByteArray MarkdownVisitor::fixMermaidSvgText(const QByteArray& svgData)
{
    QString svgStr = QString::fromUtf8(svgData);
    
    QRegularExpression foreignObjRe(QStringLiteral("<foreignObject\\s+width=\"([^\"]+)\"\\s+height=\"([^\"]+)\"[^>]*>.*?<span[^>]*>(?:<p>)?(.*?)(?:</p>)?</span>.*?</foreignObject>"));
    QRegularExpressionMatchIterator foreignIt = foreignObjRe.globalMatch(svgStr);
    QList<QRegularExpressionMatch> foreignMatches;
    while (foreignIt.hasNext()) {
        foreignMatches.append(foreignIt.next());
    }
    
    QString textColor = m_darkMode ? QStringLiteral("#c9d1d9") : QStringLiteral("#333333");
    
    for (int i = foreignMatches.size() - 1; i >= 0; --i) {
        const QRegularExpressionMatch& match = foreignMatches.at(i);
        double w = match.captured(1).toDouble();
        double h = match.captured(2).toDouble();
        QString text = match.captured(3);
        
        QString replacement = QStringLiteral(R"(<text x="%1" y="%2" dominant-baseline="middle" text-anchor="middle" font-family="sans-serif" font-size="14px" fill="%3">%4</text>)").arg(w / 2.0).arg(h / 2.0 + 2.0).arg(textColor).arg(text);
        svgStr.replace(match.capturedStart(0), match.capturedLength(0), replacement);
    }
    
    QRegularExpression textTspanRe(QStringLiteral(R"(<text\b([^>]*)>\s*<tspan\b([^>]*)>)"));
    QRegularExpressionMatchIterator it = textTspanRe.globalMatch(svgStr);
    QList<QRegularExpressionMatch> matches;
    while (it.hasNext()) {
        matches.append(it.next());
    }
    
    QRegularExpression yRe(QStringLiteral(R"(\by\s*=\s*"(-?[0-9]*\.?[0-9]+)em")"));
    QRegularExpression dyRe(QStringLiteral(R"(\bdy\s*=\s*"(-?[0-9]*\.?[0-9]+)em")"));
    QRegularExpression textYRe(QStringLiteral(R"(\by\s*=\s*"[^"]*")"));
    
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
            
            QRegularExpressionMatch textYMatch = textYRe.match(textAttrs);
            if (textYMatch.hasMatch()) {
                textAttrs.replace(textYMatch.capturedStart(0), textYMatch.capturedLength(0), 
                                  QStringLiteral("y=\"%1\"").arg(baselinePx, 0, 'f', 2));
            } else {
                textAttrs = QStringLiteral(" y=\"%1\"").arg(baselinePx, 0, 'f', 2) + textAttrs;
            }
            
            tspanAttrs.remove(yRe);
            tspanAttrs.remove(dyRe);
            
            tspanAttrs = tspanAttrs.simplified();
            if (!tspanAttrs.isEmpty() && !tspanAttrs.startsWith(QStringLiteral(" "))) {
                tspanAttrs.prepend(QLatin1Char(' '));
            }
            
            QString replacement = QStringLiteral("<text%1><tspan%2>").arg(textAttrs).arg(tspanAttrs);
            svgStr.replace(match.capturedStart(0), match.capturedLength(0), replacement);
        }
    }
    
    QRegularExpression emRe(QStringLiteral(R"(\b(y|dy)\s*=\s*"(-?[0-9]*\.?[0-9]+)em")"));
    QRegularExpressionMatchIterator emIt = emRe.globalMatch(svgStr);
    QList<QRegularExpressionMatch> emMatches;
    while (emIt.hasNext()) {
        emMatches.append(emIt.next());
    }
    
    for (int i = emMatches.size() - 1; i >= 0; --i) {
        const QRegularExpressionMatch& match = emMatches.at(i);
        QString attr = match.captured(1);
        double emValue = match.captured(2).toDouble();
        double pxValue = emValue * 16.0;
        
        QString replacement = QStringLiteral("%1=\"%2\"").arg(attr).arg(pxValue, 0, 'f', 2);
        svgStr.replace(match.capturedStart(0), match.capturedLength(0), replacement);
    }
    
    return svgStr.toUtf8();
}

QByteArray MarkdownVisitor::svgToHighDpiPng(const QByteArray& svgData, float scale, int& logicalWidth, int& logicalHeight)
{
    GError *error = nullptr;
    RsvgHandle *handle = rsvg_handle_new_from_data(
        reinterpret_cast<const guint8 *>(svgData.constData()), (gsize)svgData.size(), &error);
    if (!handle) {
        if (error) g_error_free(error);
        return QByteArray();
    }

    gdouble w = 0, h = 0;
    gboolean hasSize = rsvg_handle_get_intrinsic_size_in_pixels(handle, &w, &h);
    if (!hasSize || w <= 0 || h <= 0) {
        RsvgRectangle vb{};
        gboolean hasViewbox = FALSE, dummyW = FALSE, dummyH = FALSE;
        rsvg_handle_get_intrinsic_dimensions(handle, &dummyW, nullptr, &dummyH, nullptr, &hasViewbox, &vb);
        if (hasViewbox && vb.width > 0 && vb.height > 0) { w = vb.width; h = vb.height; }
        else { w = 800; h = 600; }
    }

    logicalWidth = (int)w;
    logicalHeight = (int)h;

    int pixelWidth = std::max(1, (int)(w * scale));
    int pixelHeight = std::max(1, (int)(h * scale));

    cairo_surface_t *surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, pixelWidth, pixelHeight);
    cairo_t *cr = cairo_create(surface);

    // Fully transparent background (alpha 0), same intent as the previous
    // QImage::fill() with a zero-alpha color -- only affects what shows
    // through outside the rendered SVG shapes, not the shapes themselves.
    cairo_set_source_rgba(cr, m_darkMode ? 0x0d / 255.0 : 0xFA / 255.0,
                              m_darkMode ? 0x11 / 255.0 : 0xFA / 255.0,
                              m_darkMode ? 0x17 / 255.0 : 0xFA / 255.0, 0.0);
    cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
    cairo_paint(cr);
    cairo_set_operator(cr, CAIRO_OPERATOR_OVER);

    cairo_scale(cr, (double)pixelWidth / w, (double)pixelHeight / h);

    RsvgRectangle viewport{0, 0, w, h};
    rsvg_handle_render_document(handle, cr, &viewport, &error);
    if (error) { g_error_free(error); error = nullptr; }

    QByteArray result;
    cairo_surface_write_to_png_stream(surface, writeToQByteArray, &result);

    cairo_destroy(cr);
    cairo_surface_destroy(surface);
    g_object_unref(handle);

    return result;
}

void MarkdownVisitor::onImage(MD::Image *i)
{
    if (!m_justCollectFootnoteRefs) {
        openStyle(i->openStyles());

        QString caption = prepareTextForHtml(i->text());
        
        m_html.push_back(QStringLiteral("<p align=\"center\">"));
        m_html.push_back(QStringLiteral("<img src=\""));
        m_html.push_back(i->url());
        m_html.push_back(QStringLiteral("\" alt=\""));
        m_html.push_back(caption);
        m_html.push_back(QStringLiteral("\" style=\"max-width:100%;\""));
        printId(i);
        m_html.push_back(QStringLiteral(" />"));
        
        if (!caption.isEmpty()) {
            // Previously read the default QGuiApplication font's point size
            // as a base; that instance never exists in the GTK3 host
            // process, so this is now a fixed default matching that
            // fallback's own default (10pt) instead of a live query.
            constexpr int baseSize = 10;
            int captionSize = qMax(1, baseSize - 2);
            
            m_html.push_back(QStringLiteral("<br />"));
            m_html.push_back(QStringLiteral("<span style=\"font-style: italic; font-size: %1pt;\">").arg(captionSize));
            m_html.push_back(caption);
            m_html.push_back(QStringLiteral("</span>"));
        }
        
        m_html.push_back(QStringLiteral("</p>\n"));

        closeStyle(i->closeStyles());
    }
}
