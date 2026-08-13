#include "markdownvisitor.h"
#include <QRegularExpression>
#include <QProcess>
#include <QSvgRenderer>
#include <QDebug>
#include <QGuiApplication>
#include <QFont>

#include <QSvgGenerator>
#include <QBuffer>
#include <QPainter>
#include <QPixmap>
#include <QNetworkAccessManager>
#include <QNetworkRequest>
#include <QNetworkReply>
#include <QEventLoop>
#include <QUrl>

// MicroTeX includes
#include "latex.h"
#include "platform/qt/graphic_qt.h"

MarkdownVisitor::MarkdownVisitor(bool darkMode) 
    : MD::details::HtmlVisitor()
    , m_darkMode(darkMode) {
}

MarkdownVisitor::~MarkdownVisitor() {
}

void MarkdownVisitor::onMath(MD::Math *m) {
    if (!m) return;
    
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

    QNetworkAccessManager manager;
    QNetworkRequest request((QUrl(url)));
    QNetworkReply *reply = manager.get(request);
    
    QEventLoop loop;
    QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    loop.exec();
    
    QByteArray data;
    if (reply->error() == QNetworkReply::NoError) {
        data = reply->readAll();
    }
    reply->deleteLater();
    return data;
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

    QNetworkAccessManager manager;
    QNetworkRequest request((QUrl(url)));
    QNetworkReply *reply = manager.get(request);
    
    QEventLoop loop;
    QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    loop.exec();
    
    QByteArray data;
    if (reply->error() == QNetworkReply::NoError) {
        data = reply->readAll();
    }
    reply->deleteLater();
    return data;
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
    QSvgRenderer renderer(svgData);
    if (!renderer.isValid()) return QByteArray();
    
    QSize defaultSize = renderer.defaultSize();
    if (defaultSize.isEmpty()) {
        QRectF viewBox = renderer.viewBoxF();
        if (!viewBox.isEmpty()) {
            defaultSize = viewBox.size().toSize();
        } else {
            defaultSize = QSize(800, 600);
        }
    }
    
    logicalWidth = defaultSize.width();
    logicalHeight = defaultSize.height();
    
    QSize scaledSize = defaultSize * scale;
    QImage image(scaledSize, QImage::Format_ARGB32_Premultiplied);
    QColor fillCol = m_darkMode ? QColor(0x0d, 0x11, 0x17, 0) : QColor(0xFA, 0xFA, 0xFA, 0);
    image.fill(fillCol);
    
    QPainter painter(&image);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setRenderHint(QPainter::TextAntialiasing);
    painter.setRenderHint(QPainter::SmoothPixmapTransform);
    
    renderer.render(&painter);
    painter.end();
    
    QByteArray ba;
    QBuffer buffer(&ba);
    buffer.open(QIODevice::WriteOnly);
    image.save(&buffer, "PNG");
    
    return ba;
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
            int baseSize = QGuiApplication::font().pointSize();
            if (baseSize <= 0) baseSize = 10;
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
