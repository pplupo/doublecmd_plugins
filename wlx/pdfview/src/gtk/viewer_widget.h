#ifndef VIEWER_WIDGET_H
#define VIEWER_WIDGET_H

#include <gtk/gtk.h>
#include <string>
#include <vector>
#include <map>
#include <memory>
#include "document_engine.h"

class GtkViewerWidget {
public:
    GtkViewerWidget(GtkWidget* parent);
    ~GtkViewerWidget();

    bool loadFile(const std::string& filepath);
    GtkWidget* getWidget() const { return overlay; }
    void grabFocus();
    void clearFocus();

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
    void updateSize();
    int pageAt(double localY) const;      // page index under drawing_area-local y
    double pageOriginX(int page) const;   // left edge of a page in drawing_area-local coords
    GdkPixbuf* renderedPage(int page);    // lazily renders/caches, evicts far-away pages
    const std::vector<TextBlock>& pageText(int page); // lazily fetches/caches per-page text
    DocPoint toDocPoint(double localX, double localY) const;
    static int rowAt(const std::vector<TextBlock>& tb, double y);
    void evictFarPages(int firstVisible, int lastVisible);
    void freePageCache();
    void updateSelection();
    void copySelection();
    void printCurrentDocument();
    void showContextMenu(GdkEventButton* event);
    void ensureFindBar();
    void showFindBar();
    void hideFindBar();
    void updateFindStatus();
    void onFindTextChanged();
    void findNext();
    void findPrevious();
    void performSearch(const std::string& query);
    void jumpToMatch(int index);

    static gboolean on_find_entry_key_press(GtkWidget* widget, GdkEventKey* event, gpointer data);

    static gboolean on_draw(GtkWidget* widget, cairo_t* cr, gpointer data);
    static gboolean on_key_press(GtkWidget* widget, GdkEventKey* event, gpointer data);
    static gboolean on_button_press(GtkWidget* widget, GdkEventButton* event, gpointer data);
    static gboolean on_motion_notify(GtkWidget* widget, GdkEventMotion* event, gpointer data);
    static gboolean on_button_release(GtkWidget* widget, GdkEventButton* event, gpointer data);
    static gboolean on_scroll(GtkWidget* widget, GdkEventScroll* event, gpointer data);
    static void on_print_draw_page(GtkPrintOperation* op, GtkPrintContext* context, gint page_nr, gpointer data);

    GtkWidget* parent_container;
    GtkWidget* overlay;
    GtkWidget* scrolled_window;
    GtkWidget* drawing_area;
    GtkWidget* findBar = nullptr;
    GtkWidget* findEntry = nullptr;
    GtkWidget* findStatusLabel = nullptr;
    std::unique_ptr<DocumentEngine> engine;

    float zoom = 1.0f;

    // Document layout, in native page-units (points, i.e. at zoom = 1.0).
    // drawing_area's own size (and thus its local coordinate space) is
    // set to totalHeightPts * zoom / maxWidthPts * zoom, so page
    // placement needs no separate scroll-offset math -- GtkViewport
    // already translates/clips the draw callback's cairo context.
    std::vector<std::pair<double, double>> pageSizePts; // {width, height}
    std::vector<double> pageYOffsetPts;
    double totalHeightPts = 0;
    double maxWidthPts = 0;
    static constexpr double kPageGapPts = 12.0;

    std::map<int, GdkPixbuf*> pageCache;
    // gdk_pixbuf_get_width/height(cached pixbuf) is in device pixels
    // (rendered at zoom * scale_factor for HiDPI sharpness); this is the
    // integer factor to divide back down to logical/widget pixels.
    int pixbufScale = 1;
    std::map<int, std::vector<TextBlock>> textCache; // page-native, zoom-independent -- persists until a new document loads

    bool selecting = false;
    double startX = 0, startY = 0;
    double endX = 0, endY = 0;

    std::string selectedText;
    std::vector<SelRect> selectedRects; // may span multiple pages

    std::string searchQuery;
    std::vector<SearchMatch> searchMatches;
    int currentMatchIndex = -1;
};

#endif // VIEWER_WIDGET_H
