// LaTeX math rendering for the Qt6 plugin: MicroTeX's Qt Graphics2D
// backend. Only compiled into markdownview_qt6, which already links
// Qt6::Gui for its own UI -- this costs it nothing extra. See
// src/gtk3/latex_render_cairo.cpp for the GTK3 target's equivalent (Cairo
// backend, no Qt).
//
// Vendored MicroTeX is upstream's "openmath" branch, not master -- see the
// comment on renderMathTag() in markdown_engine.cpp for why. The API here
// (microtex::MicroTeX::init/addFont/parse, PlatformFactory registration)
// is a real API, not a drop-in replacement for the old tex::LaTeX one.

#include "core/latex_render.h"

#include "microtex.h"
#include "graphic_qt.h"

#include <QGuiApplication>
#include <QPainter>
#include <QPixmap>
#include <QImage>
#include <QColor>
#include <QBuffer>
#include <QIODevice>

void initLatexFonts(std::vector<LatexFontEntry> &fonts)
{
    static bool inited = false;
    if (inited || fonts.empty()) return;
    inited = true;

    microtex::PlatformFactory::registerFactory("qt", std::make_unique<microtex::PlatformFactory_qt>());
    microtex::PlatformFactory::activate("qt");

    microtex::FontSrcFile first(fonts.front().clmPath, fonts.front().otfPath);
    fonts.front().canonicalName = microtex::MicroTeX::init(first).name;
    for (size_t i = 1; i < fonts.size(); ++i) {
        microtex::FontSrcFile src(fonts[i].clmPath, fonts[i].otfPath);
        fonts[i].canonicalName = microtex::MicroTeX::addFont(src).name;
    }
}

std::string addLatexFont(const std::string &clmPath, const std::string &otfPath)
{
    if (!microtex::MicroTeX::isInited()) return "";
    try {
        microtex::FontSrcFile src(clmPath, otfPath);
        return microtex::MicroTeX::addFont(src).name;
    } catch (const std::exception &) {
        // Same validation MicroTeX applies to the embedded fonts at
        // startup (rejected the real newpxmath CTAN package's incomplete
        // MATH table with ex_invalid_param, for instance) can just as well
        // reject an arbitrary user-supplied ini path -- that must fall
        // back to the default font, not crash or propagate.
        return "";
    }
}

std::vector<uint8_t> renderLatexToPng(const std::string &tex, bool darkMode,
                                       const std::string &mathFontName,
                                       int &logicalWidth, int &logicalHeight)
{
    // QPixmap/QPainter need a live QGuiApplication; the Qt6 plugin always
    // has one running (it's a Qt Lister plugin, hosted inside DC's Qt
    // event loop), so this is just a defensive guard, not an expected path.
    if (!qApp) return {};
    if (!microtex::MicroTeX::isInited()) return {};

    constexpr int oversample = 8;
    constexpr float baseTextSize = 20.0f;
    constexpr float renderTextSize = baseTextSize * oversample;
    unsigned int fgColor = darkMode ? 0xfff0f6fc : 0xff000000;

    microtex::Render *render = nullptr;
    try {
        render = microtex::MicroTeX::parse(
            tex, 0, renderTextSize, renderTextSize / 3.f, fgColor,
            true, {false, microtex::TexStyle::text}, mathFontName
        );
    } catch (const std::exception &) {
        return {};
    }
    if (!render || render->getWidth() <= 0 || render->getHeight() <= 0) {
        delete render;
        return {};
    }

    int padding = 4 * oversample;
    int physicalWidth = std::max(1, render->getWidth() + padding * 2);
    int physicalHeight = std::max(1, render->getHeight() + render->getDepth() + padding * 2);

    QPixmap pixmap(physicalWidth, physicalHeight);
    QColor bgColor = darkMode ? QColor(QStringLiteral("#0d1117")) : QColor(QStringLiteral("#FAFAFA"));
    pixmap.fill(bgColor);

    {
        QPainter painter(&pixmap);
        painter.setRenderHint(QPainter::Antialiasing);
        painter.setRenderHint(QPainter::TextAntialiasing);
        painter.setRenderHint(QPainter::SmoothPixmapTransform);

        microtex::Graphics2D_qt g2(&painter);
        render->draw(g2, padding, padding);
    }
    delete render;

    QImage image = pixmap.toImage();
    logicalWidth = physicalWidth / oversample;
    logicalHeight = physicalHeight / oversample;
    QImage scaledImage = image.scaled(logicalWidth, logicalHeight, Qt::KeepAspectRatio, Qt::SmoothTransformation);

    QByteArray ba;
    QBuffer buffer(&ba);
    buffer.open(QIODevice::WriteOnly);
    scaledImage.save(&buffer, "PNG");

    return std::vector<uint8_t>(ba.begin(), ba.end());
}
