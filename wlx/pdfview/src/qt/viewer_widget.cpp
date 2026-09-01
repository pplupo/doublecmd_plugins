#include "viewer_widget.h"
#include <QPainter>
#include <QKeyEvent>
#include <QClipboard>
#include <QGuiApplication>
#include <QApplication>
#include <QCoreApplication>
#include <QPrinter>
#include <QPrintDialog>
#include <QMenu>
#include <QContextMenuEvent>
#include <QScrollBar>
#include <QLineEdit>
#include <QLabel>
#include <QToolButton>
#include <QHBoxLayout>
#include <algorithm>
#include <limits>

ViewerWidget::ViewerWidget(QWidget* parent) : QAbstractScrollArea(parent) {
    setFocusPolicy(Qt::StrongFocus);
    viewport()->setBackgroundRole(QPalette::Dark);
    viewport()->setAutoFillBackground(true);
    connect(horizontalScrollBar(), &QScrollBar::valueChanged, viewport(), qOverload<>(&QWidget::update));
    connect(verticalScrollBar(), &QScrollBar::valueChanged, viewport(), qOverload<>(&QWidget::update));
}

ViewerWidget::~ViewerWidget() = default;

void ViewerWidget::ensureFindBar() {
    if (findBar) return;

    findBar = new QWidget(viewport());
    findBar->setStyleSheet(
        "QWidget { background: #2b2b2b; border-bottom: 2px solid #3a8ee6; }"
        "QLineEdit { background: #1e1e1e; color: #eee; border: 1px solid #444; border-radius: 3px; padding: 3px 6px; }"
        "QLabel { color: #bbb; }"
        "QToolButton { color: #ddd; border: none; padding: 3px 8px; }"
        "QToolButton:hover { background: #444; border-radius: 3px; }");

    findEdit = new QLineEdit(findBar);
    findEdit->setPlaceholderText("Find in document...");
    findEdit->setFixedWidth(200);
    findEdit->installEventFilter(this);

    findStatusLabel = new QLabel(findBar);
    findPrevButton = new QToolButton(findBar);
    findPrevButton->setText("Prev");
    findNextButton = new QToolButton(findBar);
    findNextButton->setText("Next");
    QToolButton* closeButton = new QToolButton(findBar);
    closeButton->setText(QChar(0x00D7)); // ×

    QHBoxLayout* layout = new QHBoxLayout(findBar);
    layout->setContentsMargins(8, 6, 8, 6);
    layout->setSpacing(6);
    layout->addWidget(findEdit);
    layout->addWidget(findStatusLabel);
    layout->addWidget(findPrevButton);
    layout->addWidget(findNextButton);
    layout->addWidget(closeButton);

    connect(findEdit, &QLineEdit::textChanged, this, &ViewerWidget::onFindTextChanged);
    connect(findPrevButton, &QToolButton::clicked, this, &ViewerWidget::findPrevious);
    connect(findNextButton, &QToolButton::clicked, this, &ViewerWidget::findNext);
    connect(closeButton, &QToolButton::clicked, this, &ViewerWidget::hideFindBar);

    findBar->hide();
}

void ViewerWidget::positionFindBar() {
    if (!findBar || !findBar->isVisible()) return;
    findBar->adjustSize();
    findBar->move(viewport()->width() - findBar->width(), 0);
}

void ViewerWidget::showFindBar() {
    ensureFindBar();
    findBar->show();
    positionFindBar();
    findBar->raise();
    findEdit->setFocus();
    findEdit->selectAll();
    if (!searchQuery.isEmpty()) {
        findEdit->setText(searchQuery);
        onFindTextChanged(searchQuery);
    }
}

void ViewerWidget::hideFindBar() {
    if (!findBar) return;
    findBar->hide();
    searchMatches.clear();
    currentMatchIndex = -1;
    viewport()->update();
    viewport()->setFocus();
}

void ViewerWidget::updateFindStatus() {
    if (!findStatusLabel) return;
    if (searchQuery.isEmpty()) {
        findStatusLabel->setText("");
    } else if (searchMatches.empty()) {
        findStatusLabel->setText("No results");
    } else {
        findStatusLabel->setText(QString("%1/%2").arg(currentMatchIndex + 1).arg(searchMatches.size()));
    }
}

void ViewerWidget::onFindTextChanged(const QString& text) {
    searchQuery = text;
    performSearch(text);
    currentMatchIndex = searchMatches.empty() ? -1 : 0;
    updateFindStatus();
    if (currentMatchIndex >= 0) jumpToMatch(currentMatchIndex);
    else viewport()->update();
}

bool ViewerWidget::eventFilter(QObject* watched, QEvent* event) {
    if (watched == findEdit && event->type() == QEvent::KeyPress) {
        QKeyEvent* ke = static_cast<QKeyEvent*>(event);
        if (ke->key() == Qt::Key_Escape) {
            hideFindBar();
            return true;
        } else if (ke->key() == Qt::Key_Return || ke->key() == Qt::Key_Enter) {
            if (ke->modifiers() & Qt::ShiftModifier) findPrevious();
            else findNext();
            return true;
        }
    }
    return QAbstractScrollArea::eventFilter(watched, event);
}

bool ViewerWidget::loadFile(const QString& filepath) {
    engine = createDocumentEngine(filepath.toStdString());
    if (engine && engine->load(filepath.toStdString())) {
        zoom = 1.0f;
        pageCache.clear();
        textCache.clear();
        selecting = false;
        selectedText.clear();
        selectedRects.clear();
        searchQuery.clear();
        searchMatches.clear();
        currentMatchIndex = -1;

        buildLayout();
        updateScrollbars();
        horizontalScrollBar()->setValue(0);
        verticalScrollBar()->setValue(0);
        viewport()->update();
        return true;
    }
    return false;
}

void ViewerWidget::buildLayout() {
    pageSizePts.clear();
    pageYOffsetPts.clear();
    totalHeightPts = 0;
    maxWidthPts = 0;
    if (!engine) return;

    int n = engine->getPageCount();
    pageSizePts.reserve(n);
    pageYOffsetPts.reserve(n);

    double y = 0;
    for (int i = 0; i < n; ++i) {
        float w = 0, h = 0;
        if (!engine->getPageSize(i, w, h) || w <= 0 || h <= 0) {
            w = 612;  // US Letter, points -- fallback if a page's size can't be queried
            h = 792;
        }
        pageSizePts.push_back(QSizeF(w, h));
        pageYOffsetPts.push_back(y);
        y += h + kPageGapPts;
        maxWidthPts = qMax(maxWidthPts, static_cast<double>(w));
    }
    totalHeightPts = n > 0 ? y - kPageGapPts : 0;
}

void ViewerWidget::updateScrollbars() {
    double dh = totalHeightPts * zoom;
    double dw = maxWidthPts * zoom;
    QSize vp = viewport()->size();

    verticalScrollBar()->setPageStep(vp.height());
    verticalScrollBar()->setRange(0, qMax(0, static_cast<int>(dh - vp.height())));

    horizontalScrollBar()->setPageStep(vp.width());
    horizontalScrollBar()->setRange(0, qMax(0, static_cast<int>(dw - vp.width())));
}

int ViewerWidget::pageAt(int viewportY) const {
    if (pageYOffsetPts.empty()) return 0;
    double pts = (verticalScrollBar()->value() + viewportY) / zoom;
    auto it = std::upper_bound(pageYOffsetPts.begin(), pageYOffsetPts.end(), pts);
    int idx = static_cast<int>(it - pageYOffsetPts.begin()) - 1;
    return qBound(0, idx, static_cast<int>(pageSizePts.size()) - 1);
}

QPoint ViewerWidget::pageOrigin(int page) const {
    if (page < 0 || page >= static_cast<int>(pageSizePts.size())) return QPoint();

    double pageTopScreen = pageYOffsetPts[page] * zoom - verticalScrollBar()->value();
    double pageWidthScreen = pageSizePts[page].width() * zoom;
    double vpW = viewport()->width();
    double docWidthScreen = maxWidthPts * zoom;

    double x;
    if (docWidthScreen <= vpW) {
        // Whole document narrower than the viewport: center each page.
        x = (vpW - pageWidthScreen) / 2.0;
    } else {
        // Document wider than the viewport: center each page within the
        // widest page's column, then apply horizontal scroll.
        x = (docWidthScreen - pageWidthScreen) / 2.0 - horizontalScrollBar()->value();
    }
    return QPoint(qRound(x), qRound(pageTopScreen));
}

const QImage& ViewerWidget::renderedPage(int page) {
    auto it = pageCache.find(page);
    if (it != pageCache.end()) return it.value();

    qreal dpr = devicePixelRatioF();
    if (dpr <= 0) dpr = 1.0;

    QImage img;
    int w, h;
    std::vector<unsigned char> buffer = engine->renderPage(page, zoom * dpr, w, h);
    if (!buffer.empty()) {
        img = QImage(buffer.data(), w, h, w * 4, QImage::Format_RGBA8888).copy();
        img.setDevicePixelRatio(dpr);
    }

    auto inserted = pageCache.insert(page, img);

    // Bound memory: drop cached pages that are no longer near the
    // viewport instead of keeping every rendered page for the document's
    // lifetime.
    int first = pageAt(0);
    int last = pageAt(viewport()->height());
    for (auto k : pageCache.keys()) {
        if (k < first - 2 || k > last + 2) pageCache.remove(k);
    }

    return inserted.value();
}

const std::vector<TextBlock>& ViewerWidget::pageText(int page) {
    auto it = textCache.find(page);
    if (it != textCache.end()) return it.value();
    return textCache.insert(page, engine->getText(page)).value();
}

ViewerWidget::DocPoint ViewerWidget::toDocPoint(const QPoint& viewportPt) const {
    int page = pageAt(viewportPt.y());
    QPoint origin = pageOrigin(page);
    return {page, (viewportPt.x() - origin.x()) / zoom, (viewportPt.y() - origin.y()) / zoom};
}

void ViewerWidget::resizeEvent(QResizeEvent* event) {
    QAbstractScrollArea::resizeEvent(event);
    updateScrollbars();
    positionFindBar();
}

void ViewerWidget::paintEvent(QPaintEvent* event) {
    Q_UNUSED(event);
    if (!engine || pageSizePts.empty()) return;

    QPainter painter(viewport());
    int firstPage = pageAt(0);
    int lastPage = pageAt(viewport()->height());

    for (int p = firstPage; p <= lastPage; ++p) {
        const QImage& img = renderedPage(p);
        if (img.isNull()) continue;
        painter.drawImage(pageOrigin(p), img);
    }

    if (!selectedRects.empty() || selecting) {
        painter.setBrush(QColor(0, 120, 215, 100)); // Semi-transparent blue
        painter.setPen(Qt::NoPen);
        for (const auto& sr : selectedRects) {
            if (sr.page < firstPage || sr.page > lastPage) continue;
            QPoint origin = pageOrigin(sr.page);
            painter.drawRect(sr.rect.x0 * zoom + origin.x(), sr.rect.y0 * zoom + origin.y(),
                              (sr.rect.x1 - sr.rect.x0) * zoom, (sr.rect.y1 - sr.rect.y0) * zoom);
        }
    }

    if (!searchMatches.empty()) {
        painter.setPen(Qt::NoPen);
        for (int p = firstPage; p <= lastPage; ++p) {
            QPoint origin = pageOrigin(p);
            for (size_t mi = 0; mi < searchMatches.size(); ++mi) {
                const SearchMatch& m = searchMatches[mi];
                if (m.page != p) continue;
                painter.setBrush(static_cast<int>(mi) == currentMatchIndex
                                      ? QColor(255, 140, 0, 170)
                                      : QColor(255, 255, 0, 110));
                for (const auto& r : m.rects) {
                    painter.drawRect(r.x0 * zoom + origin.x(), r.y0 * zoom + origin.y(),
                                      (r.x1 - r.x0) * zoom, (r.y1 - r.y0) * zoom);
                }
            }
        }
    }
}

void ViewerWidget::keyPressEvent(QKeyEvent* event) {
    if (event->modifiers() & Qt::ControlModifier) {
        if (event->key() == Qt::Key_C) {
            copySelection();
            return;
        } else if (event->key() == Qt::Key_P) {
            printCurrentDocument();
            return;
        } else if (event->key() == Qt::Key_F) {
            showFindBar();
            return;
        } else if (event->key() == Qt::Key_Q) {
            // DC's own hotkey manager (Ctrl+Q closes Quick View) never
            // sees key events this widget handles locally -- it's a real
            // embedded QWidget, but across a native-window boundary, so
            // key events consumed here don't reach DC's top-level window.
            // Repost it there explicitly, matching the pattern used by
            // kpartview/logview for the same problem.
            QWidget* target = QApplication::activeWindow();
            if (!target) target = window();
            if (target) {
                QCoreApplication::postEvent(target, new QKeyEvent(QEvent::KeyPress, Qt::Key_Q, Qt::ControlModifier));
                QCoreApplication::postEvent(target, new QKeyEvent(QEvent::KeyRelease, Qt::Key_Q, Qt::ControlModifier));
            }
            return;
        }
    }

    if (event->key() == Qt::Key_F3) {
        if (event->modifiers() & Qt::ShiftModifier) findPrevious();
        else findNext();
        return;
    }

    if (event->key() == Qt::Key_Plus || event->key() == Qt::Key_Equal) {
        zoom *= 1.2f;
        pageCache.clear();
        updateScrollbars();
        viewport()->update();
    } else if (event->key() == Qt::Key_Minus) {
        zoom /= 1.2f;
        pageCache.clear();
        updateScrollbars();
        viewport()->update();
    } else {
        QAbstractScrollArea::keyPressEvent(event);
    }
}

void ViewerWidget::mousePressEvent(QMouseEvent* event) {
    if (event->button() == Qt::LeftButton && engine) {
        selecting = true;
        selectionStart = event->pos();
        selectionEnd = event->pos();
        updateSelection();
    }
}

void ViewerWidget::mouseMoveEvent(QMouseEvent* event) {
    if (selecting) {
        selectionEnd = event->pos();
        updateSelection();
        viewport()->update();
    }
}

void ViewerWidget::mouseReleaseEvent(QMouseEvent* event) {
    if (event->button() == Qt::LeftButton && selecting) {
        selecting = false;
        if (selectedRects.empty()) {
            selectedText.clear();
        }
        viewport()->update();
    }
}

void ViewerWidget::wheelEvent(QWheelEvent* event) {
    if (event->modifiers() & Qt::ControlModifier) {
        // Keep the same document position centered in the viewport across
        // the zoom change, rather than jumping to whatever the raw
        // (unscaled) scrollbar value now points at.
        double centerPts = (verticalScrollBar()->value() + viewport()->height() / 2.0) / zoom;
        zoom *= (event->angleDelta().y() > 0) ? 1.2f : (1.0f / 1.2f);
        pageCache.clear();
        updateScrollbars();
        verticalScrollBar()->setValue(qMax(0, static_cast<int>(centerPts * zoom - viewport()->height() / 2.0)));
        viewport()->update();
        return;
    }

    // QAbstractScrollArea's default wheel handling scrolls by
    // QScrollBar::singleStep() (1, unset) * QApplication::wheelScrollLines()
    // (3, default) = ~3px per notch, which reads as barely moving. Scroll
    // by a fixed pixel amount per notch directly instead.
    constexpr double kPixelsPerNotch = 45.0; // ~15x the ~3px/notch default
    int pixels = qRound(-event->angleDelta().y() / 120.0 * kPixelsPerNotch);
    verticalScrollBar()->setValue(verticalScrollBar()->value() + pixels);
    event->accept();
}

int ViewerWidget::rowAt(const std::vector<TextBlock>& tb, double y) {
    if (tb.empty()) return -1;
    int bestRow = tb.front().row;
    double bestDist = std::numeric_limits<double>::max();
    for (const auto& t : tb) {
        double dist = (y >= t.bbox.y0 && y <= t.bbox.y1)
            ? 0.0
            : qMin(qAbs(y - t.bbox.y0), qAbs(y - t.bbox.y1));
        if (dist < bestDist) {
            bestDist = dist;
            bestRow = t.row;
            if (dist == 0.0) break;
        }
    }
    return bestRow;
}

void ViewerWidget::updateSelection() {
    if (!engine) return;

    DocPoint a = toDocPoint(selectionStart);
    DocPoint b = toDocPoint(selectionEnd);
    // Order by reading position (page, then y, then x) so drag direction
    // doesn't matter.
    bool aIsFirst = (a.page < b.page) || (a.page == b.page && a.y < b.y) ||
                     (a.page == b.page && a.y == b.y && a.x <= b.x);
    const DocPoint& lo = aIsFirst ? a : b;
    const DocPoint& hi = aIsFirst ? b : a;

    selectedRects.clear();
    selectedText.clear();

    for (int p = lo.page; p <= hi.page; ++p) {
        const std::vector<TextBlock>& tb = pageText(p);
        // Row-aware ("flow") selection: from the clicked row/column
        // onward, through every full row in between, up to the
        // released row/column -- not a bounding-box rectangle, which is
        // wrong for indented/justified/multi-line text. Falls back to a
        // plain bbox rectangle if this backend doesn't report line
        // grouping (row == -1, e.g. DjVu).
        bool haveRows = !tb.empty() && tb.front().row >= 0;
        int startRow = (haveRows && p == lo.page) ? rowAt(tb, lo.y) : -1;
        int endRow = (haveRows && p == hi.page) ? rowAt(tb, hi.y) : -1;

        QString pageStr;
        for (const auto& t : tb) {
            bool include;
            if (!haveRows) {
                double x1 = qMin(lo.x, hi.x), x2 = qMax(lo.x, hi.x);
                double y1 = qMin(lo.y, hi.y), y2 = qMax(lo.y, hi.y);
                include = t.bbox.x1 > x1 && t.bbox.x0 < x2 && t.bbox.y1 > y1 && t.bbox.y0 < y2;
            } else {
                double cx = (t.bbox.x0 + t.bbox.x1) / 2.0;
                bool afterStart = (p != lo.page) || t.row > startRow || (t.row == startRow && cx >= lo.x);
                bool beforeEnd = (p != hi.page) || t.row < endRow || (t.row == endRow && cx <= hi.x);
                include = afterStart && beforeEnd;
            }
            if (include) {
                selectedRects.push_back({p, t.bbox});
                pageStr += QString::fromStdString(t.text);
            }
        }
        if (!pageStr.isEmpty()) {
            if (!selectedText.isEmpty()) selectedText += "\n";
            selectedText += pageStr;
        }
    }
}

void ViewerWidget::contextMenuEvent(QContextMenuEvent* event) {
    QMenu menu(this);
    QAction* copyAction = menu.addAction(selectedText.isEmpty() ? "Copy Page Text" : "Copy Selection");
    connect(copyAction, &QAction::triggered, this, [this, event]() {
        if (selectedText.isEmpty()) {
            int page = pageAt(event->pos().y());
            QString text;
            if (engine) for (const auto& t : pageText(page)) text += QString::fromStdString(t.text);
            QGuiApplication::clipboard()->setText(text);
        } else {
            copySelection();
        }
    });
    menu.addSeparator();
    QAction* findAction = menu.addAction("Find in Document...");
    connect(findAction, &QAction::triggered, this, &ViewerWidget::showFindBar);
    QAction* findNextAction = menu.addAction("Find Next");
    findNextAction->setEnabled(!searchMatches.empty());
    connect(findNextAction, &QAction::triggered, this, &ViewerWidget::findNext);
    QAction* findPrevAction = menu.addAction("Find Previous");
    findPrevAction->setEnabled(!searchMatches.empty());
    connect(findPrevAction, &QAction::triggered, this, &ViewerWidget::findPrevious);
    menu.addSeparator();
    QAction* printAction = menu.addAction("Print...");
    connect(printAction, &QAction::triggered, this, &ViewerWidget::printCurrentDocument);
    menu.exec(event->globalPos());
}

void ViewerWidget::copySelection() {
    if (!selectedText.isEmpty()) {
        QGuiApplication::clipboard()->setText(selectedText);
    }
}

void ViewerWidget::printCurrentDocument() {
    if (!engine) return;
    int page = pageAt(viewport()->height() / 2); // whichever page is centered in view
    QPrinter printer;
    QPrintDialog dialog(&printer, this);
    if (dialog.exec() == QDialog::Accepted) {
        QPainter painter(&printer);
        int prnW, prnH;
        // High DPI render for print (e.g. 4.0 zoom)
        std::vector<unsigned char> prnBuf = engine->renderPage(page, 4.0f, prnW, prnH);
        QImage prnImg(prnBuf.data(), prnW, prnH, prnW * 4, QImage::Format_RGBA8888);
        painter.drawImage(0, 0, prnImg);
    }
}

void ViewerWidget::findNext() {
    if (searchMatches.empty()) { showFindBar(); return; }
    currentMatchIndex = (currentMatchIndex + 1) % static_cast<int>(searchMatches.size());
    updateFindStatus();
    jumpToMatch(currentMatchIndex);
}

void ViewerWidget::findPrevious() {
    if (searchMatches.empty()) { showFindBar(); return; }
    currentMatchIndex = (currentMatchIndex - 1 + static_cast<int>(searchMatches.size())) % static_cast<int>(searchMatches.size());
    updateFindStatus();
    jumpToMatch(currentMatchIndex);
}

void ViewerWidget::performSearch(const QString& query) {
    searchMatches.clear();
    if (!engine || query.isEmpty()) return;

    QString q = query.toLower();
    int pageCount = engine->getPageCount();
    for (int p = 0; p < pageCount; ++p) {
        // getText() returns one TextBlock per character, so the
        // concatenation of their .text strings lines up 1:1 with this
        // page's searchable string (aside from rare multi-QChar
        // codepoints, an accepted simplification here).
        const std::vector<TextBlock>& tb = pageText(p);
        QString text;
        text.reserve(static_cast<int>(tb.size()));
        for (const auto& t : tb) text += QString::fromStdString(t.text);
        QString textLower = text.toLower();

        int pos = 0;
        while ((pos = textLower.indexOf(q, pos)) != -1) {
            if (pos + q.length() <= static_cast<int>(tb.size())) {
                SearchMatch m;
                m.page = p;
                for (int k = 0; k < q.length(); ++k) m.rects.push_back(tb[pos + k].bbox);
                searchMatches.push_back(m);
            }
            pos += q.length();
        }
    }
}

void ViewerWidget::jumpToMatch(int index) {
    if (index < 0 || index >= static_cast<int>(searchMatches.size())) return;
    const SearchMatch& m = searchMatches[index];
    if (m.rects.empty()) return;

    double matchTopPts = pageYOffsetPts[m.page] + m.rects.front().y0;
    double targetY = matchTopPts * zoom - viewport()->height() / 3.0;
    verticalScrollBar()->setValue(qMax(0, static_cast<int>(targetY)));
    viewport()->update();
}
