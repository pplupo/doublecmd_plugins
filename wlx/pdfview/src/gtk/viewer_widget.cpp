#include "viewer_widget.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <limits>

GtkViewerWidget::GtkViewerWidget(GtkWidget* parent) : parent_container(parent) {
    drawing_area = gtk_drawing_area_new();
    gtk_widget_set_can_focus(drawing_area, TRUE);
    gtk_widget_add_events(drawing_area,
                          GDK_BUTTON_PRESS_MASK | GDK_BUTTON_RELEASE_MASK |
                          GDK_POINTER_MOTION_MASK | GDK_SCROLL_MASK |
                          GDK_KEY_PRESS_MASK);

    g_signal_connect(drawing_area, "draw", G_CALLBACK(on_draw), this);
    g_signal_connect(drawing_area, "key-press-event", G_CALLBACK(on_key_press), this);
    g_signal_connect(drawing_area, "button-press-event", G_CALLBACK(on_button_press), this);
    g_signal_connect(drawing_area, "motion-notify-event", G_CALLBACK(on_motion_notify), this);
    g_signal_connect(drawing_area, "button-release-event", G_CALLBACK(on_button_release), this);
    g_signal_connect(drawing_area, "scroll-event", G_CALLBACK(on_scroll), this);

    // A GtkScrolledWindow auto-wraps drawing_area in a GtkViewport, which
    // gives it a real scrollbar and handles the scroll offset/clipping of
    // the "draw" callback's cairo context for us.
    scrolled_window = gtk_scrolled_window_new(nullptr, nullptr);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scrolled_window),
                                    GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
    gtk_container_add(GTK_CONTAINER(scrolled_window), drawing_area);

    // A GtkOverlay lets the find bar float over the top-right corner of
    // the document instead of taking up its own row in the layout.
    overlay = gtk_overlay_new();
    gtk_container_add(GTK_CONTAINER(overlay), scrolled_window);
    gtk_container_add(GTK_CONTAINER(parent), overlay);
    gtk_widget_show_all(overlay);
}

GtkViewerWidget::~GtkViewerWidget() {
    freePageCache();
}

void GtkViewerWidget::freePageCache() {
    for (auto& kv : pageCache) g_object_unref(kv.second);
    pageCache.clear();
}

bool GtkViewerWidget::loadFile(const std::string& filepath) {
    engine = createDocumentEngine(filepath);
    if (engine && engine->load(filepath)) {
        zoom = 1.0f;
        freePageCache();
        textCache.clear();
        selecting = false;
        selectedText.clear();
        selectedRects.clear();
        searchQuery.clear();
        searchMatches.clear();
        currentMatchIndex = -1;

        buildLayout();
        updateSize();

        GtkAdjustment* vadj = gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(scrolled_window));
        GtkAdjustment* hadj = gtk_scrolled_window_get_hadjustment(GTK_SCROLLED_WINDOW(scrolled_window));
        gtk_adjustment_set_value(vadj, 0);
        gtk_adjustment_set_value(hadj, 0);

        gtk_widget_queue_draw(drawing_area);
        return true;
    }
    return false;
}

void GtkViewerWidget::buildLayout() {
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
            w = 612; // US Letter, points -- fallback if a page's size can't be queried
            h = 792;
        }
        pageSizePts.push_back({static_cast<double>(w), static_cast<double>(h)});
        pageYOffsetPts.push_back(y);
        y += h + kPageGapPts;
        maxWidthPts = std::max(maxWidthPts, static_cast<double>(w));
    }
    totalHeightPts = n > 0 ? y - kPageGapPts : 0;
}

void GtkViewerWidget::updateSize() {
    int w = static_cast<int>(std::ceil(maxWidthPts * zoom));
    int h = static_cast<int>(std::ceil(totalHeightPts * zoom));
    gtk_widget_set_size_request(drawing_area, w, h);
}

int GtkViewerWidget::pageAt(double localY) const {
    if (pageYOffsetPts.empty()) return 0;
    double pts = localY / zoom;
    auto it = std::upper_bound(pageYOffsetPts.begin(), pageYOffsetPts.end(), pts);
    int idx = static_cast<int>(it - pageYOffsetPts.begin()) - 1;
    idx = std::max(0, std::min(idx, static_cast<int>(pageSizePts.size()) - 1));
    return idx;
}

double GtkViewerWidget::pageOriginX(int page) const {
    if (page < 0 || page >= static_cast<int>(pageSizePts.size())) return 0;
    double pageWidthScreen = pageSizePts[page].first * zoom;
    double docWidthScreen = maxWidthPts * zoom;
    return (docWidthScreen - pageWidthScreen) / 2.0;
}

void GtkViewerWidget::evictFarPages(int firstVisible, int lastVisible) {
    for (auto it = pageCache.begin(); it != pageCache.end();) {
        if (it->first < firstVisible - 2 || it->first > lastVisible + 2) {
            g_object_unref(it->second);
            it = pageCache.erase(it);
        } else {
            ++it;
        }
    }
}

GdkPixbuf* GtkViewerWidget::renderedPage(int page) {
    auto it = pageCache.find(page);
    if (it != pageCache.end()) return it->second;

    // Render at the screen's actual scale factor, not just at `zoom` --
    // otherwise GTK silently re-stretches this already-correctly-
    // rasterized page to match a HiDPI screen, which is what looked like
    // "aliasing on zoom" despite the source being vector and re-rendered
    // fresh at every zoom level.
    int scale = gtk_widget_get_scale_factor(drawing_area);
    if (scale < 1) scale = 1;
    pixbufScale = scale;

    int w, h;
    std::vector<unsigned char> buffer = engine->renderPage(page, zoom * scale, w, h);
    if (buffer.empty()) return nullptr;

    GdkPixbuf* pixbuf = gdk_pixbuf_new(GDK_COLORSPACE_RGB, TRUE, 8, w, h);
    int stride = gdk_pixbuf_get_rowstride(pixbuf);
    guchar* pixels = gdk_pixbuf_get_pixels(pixbuf);
    for (int y = 0; y < h; ++y) {
        memcpy(pixels + y * stride, buffer.data() + y * w * 4, w * 4);
    }

    pageCache[page] = pixbuf;
    return pixbuf;
}

const std::vector<TextBlock>& GtkViewerWidget::pageText(int page) {
    auto it = textCache.find(page);
    if (it != textCache.end()) return it->second;
    return textCache.emplace(page, engine->getText(page)).first->second;
}

GtkViewerWidget::DocPoint GtkViewerWidget::toDocPoint(double localX, double localY) const {
    int page = pageAt(localY);
    double ox = pageOriginX(page);
    double oy = pageYOffsetPts[page] * zoom;
    return {page, (localX - ox) / zoom, (localY - oy) / zoom};
}

gboolean GtkViewerWidget::on_draw(GtkWidget* widget, cairo_t* cr, gpointer data) {
    GtkViewerWidget* self = static_cast<GtkViewerWidget*>(data);
    if (!self->engine || self->pageSizePts.empty()) return FALSE;

    double cx1, cy1, cx2, cy2;
    cairo_clip_extents(cr, &cx1, &cy1, &cx2, &cy2);
    int firstPage = self->pageAt(cy1);
    int lastPage = self->pageAt(cy2);

    for (int p = firstPage; p <= lastPage; ++p) {
        GdkPixbuf* pix = self->renderedPage(p);
        if (!pix) continue;
        double ox = self->pageOriginX(p);
        double oy = self->pageYOffsetPts[p] * self->zoom;

        cairo_save(cr);
        cairo_translate(cr, ox, oy);
        cairo_scale(cr, 1.0 / self->pixbufScale, 1.0 / self->pixbufScale);
        gdk_cairo_set_source_pixbuf(cr, pix, 0, 0);
        cairo_paint(cr);
        cairo_restore(cr);
    }

    self->evictFarPages(firstPage, lastPage);

    if (self->selecting || !self->selectedRects.empty()) {
        cairo_set_source_rgba(cr, 0.0, 0.47, 0.84, 0.4); // Semi-transparent blue
        for (const auto& sr : self->selectedRects) {
            if (sr.page < firstPage || sr.page > lastPage) continue;
            double ox = self->pageOriginX(sr.page);
            double oy = self->pageYOffsetPts[sr.page] * self->zoom;
            cairo_rectangle(cr, sr.rect.x0 * self->zoom + ox, sr.rect.y0 * self->zoom + oy,
                            (sr.rect.x1 - sr.rect.x0) * self->zoom, (sr.rect.y1 - sr.rect.y0) * self->zoom);
            cairo_fill(cr);
        }
    }

    if (!self->searchMatches.empty()) {
        for (int p = firstPage; p <= lastPage; ++p) {
            double ox = self->pageOriginX(p);
            double oy = self->pageYOffsetPts[p] * self->zoom;
            for (size_t mi = 0; mi < self->searchMatches.size(); ++mi) {
                const SearchMatch& m = self->searchMatches[mi];
                if (m.page != p) continue;
                if (static_cast<int>(mi) == self->currentMatchIndex) {
                    cairo_set_source_rgba(cr, 1.0, 0.55, 0.0, 0.65);
                } else {
                    cairo_set_source_rgba(cr, 1.0, 1.0, 0.0, 0.45);
                }
                for (const auto& r : m.rects) {
                    cairo_rectangle(cr, r.x0 * self->zoom + ox, r.y0 * self->zoom + oy,
                                    (r.x1 - r.x0) * self->zoom, (r.y1 - r.y0) * self->zoom);
                    cairo_fill(cr);
                }
            }
        }
    }

    return TRUE;
}

gboolean GtkViewerWidget::on_key_press(GtkWidget* widget, GdkEventKey* event, gpointer data) {
    GtkViewerWidget* self = static_cast<GtkViewerWidget*>(data);
    // Ctrl+Q (close Quick View) is deliberately not handled here -- it
    // falls through to `return FALSE` below so it propagates to DC's own
    // window and its hotkey manager can act on it, same as mpv_wayland's
    // GTK3 plugin does for the same key.
    if (event->state & GDK_CONTROL_MASK) {
        if (event->keyval == GDK_KEY_c || event->keyval == GDK_KEY_C) {
            self->copySelection();
            return TRUE;
        } else if (event->keyval == GDK_KEY_p || event->keyval == GDK_KEY_P) {
            self->printCurrentDocument();
            return TRUE;
        } else if (event->keyval == GDK_KEY_f || event->keyval == GDK_KEY_F) {
            self->showFindBar();
            return TRUE;
        }
    }

    if (event->keyval == GDK_KEY_F3) {
        if (event->state & GDK_SHIFT_MASK) self->findPrevious();
        else self->findNext();
        return TRUE;
    }

    if (event->keyval == GDK_KEY_plus || event->keyval == GDK_KEY_equal) {
        self->zoom *= 1.2f;
        self->freePageCache();
        self->updateSize();
        gtk_widget_queue_draw(widget);
    } else if (event->keyval == GDK_KEY_minus) {
        self->zoom /= 1.2f;
        self->freePageCache();
        self->updateSize();
        gtk_widget_queue_draw(widget);
    } else {
        return FALSE;
    }

    return TRUE;
}

gboolean GtkViewerWidget::on_button_press(GtkWidget* widget, GdkEventButton* event, gpointer data) {
    GtkViewerWidget* self = static_cast<GtkViewerWidget*>(data);
    if (event->button == 1) {
        gtk_widget_grab_focus(widget);
        self->selecting = true;
        self->startX = event->x;
        self->startY = event->y;
        self->endX = event->x;
        self->endY = event->y;
        self->updateSelection();
    } else if (event->button == 3) {
        gtk_widget_grab_focus(widget);
        self->showContextMenu(event);
    }
    return TRUE;
}

gboolean GtkViewerWidget::on_motion_notify(GtkWidget* widget, GdkEventMotion* event, gpointer data) {
    GtkViewerWidget* self = static_cast<GtkViewerWidget*>(data);
    if (self->selecting) {
        self->endX = event->x;
        self->endY = event->y;
        self->updateSelection();
        gtk_widget_queue_draw(widget);
    }
    return TRUE;
}

gboolean GtkViewerWidget::on_button_release(GtkWidget* widget, GdkEventButton* event, gpointer data) {
    GtkViewerWidget* self = static_cast<GtkViewerWidget*>(data);
    if (event->button == 1 && self->selecting) {
        self->selecting = false;
        if (self->selectedRects.empty()) {
            self->selectedText.clear();
        }
        gtk_widget_queue_draw(widget);
    }
    return TRUE;
}

gboolean GtkViewerWidget::on_scroll(GtkWidget* widget, GdkEventScroll* event, gpointer data) {
    GtkViewerWidget* self = static_cast<GtkViewerWidget*>(data);
    GtkAdjustment* vadj = gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(self->scrolled_window));

    if (event->state & GDK_CONTROL_MASK) {
        // Keep the same document position centered in the viewport across
        // the zoom change, rather than jumping to whatever the raw
        // (unscaled) adjustment value now points at.
        double centerPts = (gtk_adjustment_get_value(vadj) + gtk_adjustment_get_page_size(vadj) / 2.0) / self->zoom;

        if (event->direction == GDK_SCROLL_UP) {
            self->zoom *= 1.2f;
        } else if (event->direction == GDK_SCROLL_DOWN) {
            self->zoom /= 1.2f;
        } else {
            return TRUE;
        }
        self->freePageCache();
        self->updateSize();

        double newValue = centerPts * self->zoom - gtk_adjustment_get_page_size(vadj) / 2.0;
        gtk_adjustment_set_value(vadj, std::max(0.0, newValue));
        gtk_widget_queue_draw(self->drawing_area);
        return TRUE;
    }

    // GtkScrolledWindow's own default step (GtkAdjustment::step-increment)
    // is small and reads as barely moving -- scroll by a fixed pixel
    // amount per notch directly instead.
    constexpr double kPixelsPerNotch = 45.0; // ~15x the sluggish default
    double delta = 0;
    if (event->direction == GDK_SCROLL_DOWN) delta = kPixelsPerNotch;
    else if (event->direction == GDK_SCROLL_UP) delta = -kPixelsPerNotch;
    else return FALSE; // smooth/horizontal: let the default handling deal with it

    double maxValue = gtk_adjustment_get_upper(vadj) - gtk_adjustment_get_page_size(vadj);
    double newValue = std::max(0.0, std::min(gtk_adjustment_get_value(vadj) + delta, maxValue));
    gtk_adjustment_set_value(vadj, newValue);
    return TRUE;
}

int GtkViewerWidget::rowAt(const std::vector<TextBlock>& tb, double y) {
    if (tb.empty()) return -1;
    int bestRow = tb.front().row;
    double bestDist = std::numeric_limits<double>::max();
    for (const auto& t : tb) {
        double dist = (y >= t.bbox.y0 && y <= t.bbox.y1)
            ? 0.0
            : std::min(std::abs(y - t.bbox.y0), std::abs(y - t.bbox.y1));
        if (dist < bestDist) {
            bestDist = dist;
            bestRow = t.row;
            if (dist == 0.0) break;
        }
    }
    return bestRow;
}

void GtkViewerWidget::updateSelection() {
    if (!engine) return;

    DocPoint a = toDocPoint(startX, startY);
    DocPoint b = toDocPoint(endX, endY);
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

        std::string pageStr;
        for (const auto& t : tb) {
            bool include;
            if (!haveRows) {
                double x1 = std::min(lo.x, hi.x), x2 = std::max(lo.x, hi.x);
                double y1 = std::min(lo.y, hi.y), y2 = std::max(lo.y, hi.y);
                include = t.bbox.x1 > x1 && t.bbox.x0 < x2 && t.bbox.y1 > y1 && t.bbox.y0 < y2;
            } else {
                double cx = (t.bbox.x0 + t.bbox.x1) / 2.0;
                bool afterStart = (p != lo.page) || t.row > startRow || (t.row == startRow && cx >= lo.x);
                bool beforeEnd = (p != hi.page) || t.row < endRow || (t.row == endRow && cx <= hi.x);
                include = afterStart && beforeEnd;
            }
            if (include) {
                selectedRects.push_back({p, t.bbox});
                pageStr += t.text;
            }
        }
        if (!pageStr.empty()) {
            if (!selectedText.empty()) selectedText += "\n";
            selectedText += pageStr;
        }
    }
}

void GtkViewerWidget::copySelection() {
    if (!selectedText.empty()) {
        GtkClipboard* clipboard = gtk_clipboard_get(GDK_SELECTION_CLIPBOARD);
        gtk_clipboard_set_text(clipboard, selectedText.c_str(), -1);
    }
}

void GtkViewerWidget::on_print_draw_page(GtkPrintOperation*, GtkPrintContext* context, gint, gpointer data) {
    GtkViewerWidget* self = static_cast<GtkViewerWidget*>(data);
    if (!self->engine) return;

    cairo_t* cr = gtk_print_context_get_cairo_context(context);
    int prnW, prnH;
    // High DPI render for print (e.g. 4.0 zoom). Print the page currently
    // centered in the viewport, since there's no single "current page" in
    // a continuous-scroll view.
    GtkAdjustment* vadj = gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(self->scrolled_window));
    int page = self->pageAt(gtk_adjustment_get_value(vadj) + gtk_adjustment_get_page_size(vadj) / 2.0);
    std::vector<unsigned char> prnBuf = self->engine->renderPage(page, 4.0f, prnW, prnH);
    if (prnBuf.empty()) return;

    // Our render buffer is byte-order RGBA, which matches GdkPixbuf, not
    // cairo's native ARGB32 (0xAARRGGBB) -- go through GdkPixbuf rather
    // than wrapping the raw bytes as a cairo surface directly, or the R/B
    // channels come out swapped.
    GdkPixbuf* pixbuf = gdk_pixbuf_new(GDK_COLORSPACE_RGB, TRUE, 8, prnW, prnH);
    int stride = gdk_pixbuf_get_rowstride(pixbuf);
    guchar* pixels = gdk_pixbuf_get_pixels(pixbuf);
    for (int y = 0; y < prnH; ++y) {
        memcpy(pixels + y * stride, prnBuf.data() + y * prnW * 4, prnW * 4);
    }
    gdk_cairo_set_source_pixbuf(cr, pixbuf, 0, 0);
    cairo_paint(cr);
    g_object_unref(pixbuf);
}

void GtkViewerWidget::printCurrentDocument() {
    if (!engine) return;
    GtkPrintOperation* print = gtk_print_operation_new();
    gtk_print_operation_set_n_pages(print, 1);
    g_signal_connect(print, "draw-page", G_CALLBACK(on_print_draw_page), this);
    gtk_print_operation_run(print, GTK_PRINT_OPERATION_ACTION_PRINT_DIALOG, nullptr, nullptr);
    g_object_unref(print);
}

void GtkViewerWidget::showContextMenu(GdkEventButton* event) {
    GtkWidget* menu = gtk_menu_new();
    double clickY = event->y;

    GtkWidget* copyItem = gtk_menu_item_new_with_label(selectedText.empty() ? "Copy Page Text" : "Copy Selection");
    // "Copy Page Text" needs to know which page was under the cursor;
    // stash the click position on the menu item for the handler to read.
    g_object_set_data(G_OBJECT(copyItem), "click-y", GINT_TO_POINTER(static_cast<int>(clickY)));
    g_signal_connect(copyItem, "activate", G_CALLBACK(+[](GtkMenuItem* item, gpointer data) {
        GtkViewerWidget* self = static_cast<GtkViewerWidget*>(data);
        if (self->selectedText.empty()) {
            int clickY = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(item), "click-y"));
            int page = self->pageAt(clickY);
            std::string text;
            if (self->engine) for (const auto& t : self->pageText(page)) text += t.text;
            GtkClipboard* clipboard = gtk_clipboard_get(GDK_SELECTION_CLIPBOARD);
            gtk_clipboard_set_text(clipboard, text.c_str(), -1);
        } else {
            self->copySelection();
        }
    }), this);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), copyItem);

    gtk_menu_shell_append(GTK_MENU_SHELL(menu), gtk_separator_menu_item_new());

    GtkWidget* findItem = gtk_menu_item_new_with_label("Find in Document...");
    g_signal_connect(findItem, "activate", G_CALLBACK(+[](GtkMenuItem*, gpointer data) {
        static_cast<GtkViewerWidget*>(data)->showFindBar();
    }), this);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), findItem);

    GtkWidget* findNextItem = gtk_menu_item_new_with_label("Find Next");
    gtk_widget_set_sensitive(findNextItem, !searchMatches.empty());
    g_signal_connect(findNextItem, "activate", G_CALLBACK(+[](GtkMenuItem*, gpointer data) {
        static_cast<GtkViewerWidget*>(data)->findNext();
    }), this);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), findNextItem);

    GtkWidget* findPrevItem = gtk_menu_item_new_with_label("Find Previous");
    gtk_widget_set_sensitive(findPrevItem, !searchMatches.empty());
    g_signal_connect(findPrevItem, "activate", G_CALLBACK(+[](GtkMenuItem*, gpointer data) {
        static_cast<GtkViewerWidget*>(data)->findPrevious();
    }), this);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), findPrevItem);

    gtk_menu_shell_append(GTK_MENU_SHELL(menu), gtk_separator_menu_item_new());

    GtkWidget* printItem = gtk_menu_item_new_with_label("Print...");
    g_signal_connect(printItem, "activate", G_CALLBACK(+[](GtkMenuItem*, gpointer data) {
        static_cast<GtkViewerWidget*>(data)->printCurrentDocument();
    }), this);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), printItem);

    gtk_widget_show_all(menu);
    gtk_menu_popup_at_pointer(GTK_MENU(menu), reinterpret_cast<GdkEvent*>(event));
}

void GtkViewerWidget::grabFocus() {
    gtk_widget_grab_focus(drawing_area);
}

void GtkViewerWidget::clearFocus() {
    GtkWidget* toplevel = gtk_widget_get_toplevel(drawing_area);
    if (toplevel && GTK_IS_WINDOW(toplevel)) {
        gtk_window_set_focus(GTK_WINDOW(toplevel), nullptr);
    }
}

void GtkViewerWidget::ensureFindBar() {
    if (findBar) return;

    static const char* css =
        "#pdfview-find-bar { background-color: #2b2b2b; border-bottom: 2px solid #3a8ee6; padding: 6px 8px; }"
        "#pdfview-find-bar entry { background-color: #1e1e1e; color: #eee; }"
        "#pdfview-find-bar label { color: #bbb; }"
        "#pdfview-find-bar button { color: #ddd; }";
    GtkCssProvider* provider = gtk_css_provider_new();
    gtk_css_provider_load_from_data(provider, css, -1, nullptr);

    findBar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_widget_set_name(findBar, "pdfview-find-bar");
    gtk_style_context_add_provider(gtk_widget_get_style_context(findBar),
                                    GTK_STYLE_PROVIDER(provider), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    gtk_widget_set_halign(findBar, GTK_ALIGN_END);
    gtk_widget_set_valign(findBar, GTK_ALIGN_START);

    findEntry = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(findEntry), "Find in document...");
    gtk_entry_set_width_chars(GTK_ENTRY(findEntry), 22);
    g_signal_connect(findEntry, "changed", G_CALLBACK(+[](GtkEditable*, gpointer data) {
        static_cast<GtkViewerWidget*>(data)->onFindTextChanged();
    }), this);
    g_signal_connect(findEntry, "key-press-event", G_CALLBACK(on_find_entry_key_press), this);

    findStatusLabel = gtk_label_new("");

    GtkWidget* prevButton = gtk_button_new_with_label("Prev");
    gtk_button_set_relief(GTK_BUTTON(prevButton), GTK_RELIEF_NONE);
    g_signal_connect(prevButton, "clicked", G_CALLBACK(+[](GtkButton*, gpointer data) {
        static_cast<GtkViewerWidget*>(data)->findPrevious();
    }), this);

    GtkWidget* nextButton = gtk_button_new_with_label("Next");
    gtk_button_set_relief(GTK_BUTTON(nextButton), GTK_RELIEF_NONE);
    g_signal_connect(nextButton, "clicked", G_CALLBACK(+[](GtkButton*, gpointer data) {
        static_cast<GtkViewerWidget*>(data)->findNext();
    }), this);

    GtkWidget* closeButton = gtk_button_new_with_label("×");
    gtk_button_set_relief(GTK_BUTTON(closeButton), GTK_RELIEF_NONE);
    g_signal_connect(closeButton, "clicked", G_CALLBACK(+[](GtkButton*, gpointer data) {
        static_cast<GtkViewerWidget*>(data)->hideFindBar();
    }), this);

    gtk_box_pack_start(GTK_BOX(findBar), findEntry, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(findBar), findStatusLabel, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(findBar), prevButton, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(findBar), nextButton, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(findBar), closeButton, FALSE, FALSE, 0);

    gtk_overlay_add_overlay(GTK_OVERLAY(overlay), findBar);
    g_object_unref(provider);
}

void GtkViewerWidget::showFindBar() {
    ensureFindBar();
    gtk_widget_show_all(findBar);
    gtk_widget_grab_focus(findEntry);
    gtk_editable_select_region(GTK_EDITABLE(findEntry), 0, -1);
    if (!searchQuery.empty()) {
        gtk_entry_set_text(GTK_ENTRY(findEntry), searchQuery.c_str());
        onFindTextChanged();
    }
}

void GtkViewerWidget::hideFindBar() {
    if (!findBar) return;
    gtk_widget_hide(findBar);
    searchMatches.clear();
    currentMatchIndex = -1;
    gtk_widget_queue_draw(drawing_area);
    grabFocus();
}

void GtkViewerWidget::updateFindStatus() {
    if (!findStatusLabel) return;
    if (searchQuery.empty()) {
        gtk_label_set_text(GTK_LABEL(findStatusLabel), "");
    } else if (searchMatches.empty()) {
        gtk_label_set_text(GTK_LABEL(findStatusLabel), "No results");
    } else {
        gtk_label_set_text(GTK_LABEL(findStatusLabel),
            (std::to_string(currentMatchIndex + 1) + "/" + std::to_string(searchMatches.size())).c_str());
    }
}

void GtkViewerWidget::onFindTextChanged() {
    searchQuery = gtk_entry_get_text(GTK_ENTRY(findEntry));
    performSearch(searchQuery);
    currentMatchIndex = searchMatches.empty() ? -1 : 0;
    updateFindStatus();
    if (currentMatchIndex >= 0) jumpToMatch(currentMatchIndex);
    else gtk_widget_queue_draw(drawing_area);
}

gboolean GtkViewerWidget::on_find_entry_key_press(GtkWidget*, GdkEventKey* event, gpointer data) {
    GtkViewerWidget* self = static_cast<GtkViewerWidget*>(data);
    if (event->keyval == GDK_KEY_Escape) {
        self->hideFindBar();
        return TRUE;
    } else if (event->keyval == GDK_KEY_Return || event->keyval == GDK_KEY_KP_Enter) {
        if (event->state & GDK_SHIFT_MASK) self->findPrevious();
        else self->findNext();
        return TRUE;
    }
    return FALSE;
}

void GtkViewerWidget::findNext() {
    if (searchMatches.empty()) { showFindBar(); return; }
    currentMatchIndex = (currentMatchIndex + 1) % static_cast<int>(searchMatches.size());
    updateFindStatus();
    jumpToMatch(currentMatchIndex);
}

void GtkViewerWidget::findPrevious() {
    if (searchMatches.empty()) { showFindBar(); return; }
    currentMatchIndex = (currentMatchIndex - 1 + static_cast<int>(searchMatches.size())) % static_cast<int>(searchMatches.size());
    updateFindStatus();
    jumpToMatch(currentMatchIndex);
}

void GtkViewerWidget::performSearch(const std::string& query) {
    searchMatches.clear();
    if (!engine || query.empty()) return;

    std::string q = query;
    std::transform(q.begin(), q.end(), q.begin(), [](unsigned char c) { return std::tolower(c); });

    int pageCount = engine->getPageCount();
    for (int p = 0; p < pageCount; ++p) {
        // getText() returns one TextBlock per character, so the
        // concatenation of their .text strings lines up 1:1 with this
        // page's searchable string (aside from rare multi-byte UTF-8
        // codepoints, an accepted simplification here).
        const std::vector<TextBlock>& tb = pageText(p);
        std::string text;
        for (const auto& t : tb) text += t.text;
        std::string textLower = text;
        std::transform(textLower.begin(), textLower.end(), textLower.begin(),
                        [](unsigned char c) { return std::tolower(c); });

        size_t pos = 0;
        while ((pos = textLower.find(q, pos)) != std::string::npos) {
            if (pos + q.length() <= tb.size()) {
                SearchMatch m;
                m.page = p;
                for (size_t k = 0; k < q.length(); ++k) m.rects.push_back(tb[pos + k].bbox);
                searchMatches.push_back(m);
            }
            pos += q.length();
        }
    }
}

void GtkViewerWidget::jumpToMatch(int index) {
    if (index < 0 || index >= static_cast<int>(searchMatches.size())) return;
    const SearchMatch& m = searchMatches[index];
    if (m.rects.empty()) return;

    double matchTopPts = pageYOffsetPts[m.page] + m.rects.front().y0;
    GtkAdjustment* vadj = gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(scrolled_window));
    double target = matchTopPts * zoom - gtk_adjustment_get_page_size(vadj) / 3.0;
    gtk_adjustment_set_value(vadj, std::max(0.0, target));
    gtk_widget_queue_draw(drawing_area);
}
