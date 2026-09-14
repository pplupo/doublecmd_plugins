#ifndef VIEWER_WIDGET_H
#define VIEWER_WIDGET_H

#include <QAbstractScrollArea>
#include <QImage>
#include <QPaintEvent>
#include <QHash>
#include <memory>
#include <vector>
#include "document_engine.h"

class QLineEdit;
class QLabel;
class QToolButton;
class QWidget;

class ViewerWidget : public QAbstractScrollArea {
    Q_OBJECT
public:
    explicit ViewerWidget(QWidget* parent = nullptr);
    ~ViewerWidget() override;

    bool loadFile(const QString& filepath);

protected:
    void paintEvent(QPaintEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;
    void contextMenuEvent(QContextMenuEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    struct SearchMatch {
        int page;
        std::vector<Rect> rects; // one bbox per matched char, in page-native points
    };
    struct DocPoint {
        int page;
        double x, y; // page-native points
    };
    struct SelRect {
        int page;
        Rect rect;
    };

    void buildLayout();
    void updateScrollbars();
    int pageAt(int viewportY) const;      // page index under a viewport-relative y
    QPoint pageOrigin(int page) const;    // top-left of a page in viewport coords
    const QImage& renderedPage(int page); // lazily renders/caches, evicts far-away pages
    const std::vector<TextBlock>& pageText(int page); // lazily fetches/caches per-page text
    DocPoint toDocPoint(const QPoint& viewportPt) const;
    static int rowAt(const std::vector<TextBlock>& tb, double y);
    void updateSelection();
    void copySelection();
    void printCurrentDocument();
    void ensureFindBar();
    void showFindBar();
    void hideFindBar();
    void positionFindBar();
    void onFindTextChanged(const QString& text);
    void findNext();
    void findPrevious();
    void performSearch(const QString& query);
    void jumpToMatch(int index);
    void updateFindStatus();

    std::unique_ptr<DocumentEngine> engine;
    float zoom = 1.0f;

    // Document layout, in native page-units (points, i.e. at zoom = 1.0).
    // Screen position of anything is (this) * zoom - scrollbar value.
    std::vector<QSizeF> pageSizePts;
    std::vector<double> pageYOffsetPts;
    double totalHeightPts = 0;
    double maxWidthPts = 0;
    static constexpr double kPageGapPts = 12.0;

    QHash<int, QImage> pageCache;
    QHash<int, std::vector<TextBlock>> textCache; // page-native, zoom-independent -- persists until a new document loads

    bool selecting = false;
    QPoint selectionStart;
    QPoint selectionEnd;

    QString selectedText;
    std::vector<SelRect> selectedRects; // may span multiple pages

    QString searchQuery;
    std::vector<SearchMatch> searchMatches;
    int currentMatchIndex = -1;

    QWidget* findBar = nullptr;
    QLineEdit* findEdit = nullptr;
    QLabel* findStatusLabel = nullptr;
    QToolButton* findPrevButton = nullptr;
    QToolButton* findNextButton = nullptr;
};

#endif // VIEWER_WIDGET_H
