/*
 * mpv_wayland WLX plugin for Double Commander — GTK3 UI layer.
 *
 * Uses GtkGLArea for rendering, mirroring mdk's GTK3 port (same pattern:
 * MpvEngine owns all libmpv state, this file only wires it to GTK
 * widgets and the WLX plugin ABI).
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <gtk/gtk.h>
#include <epoxy/gl.h>
#include <epoxy/egl.h>
#include <cstdio>
#include <cstring>
#include <memory>

#include "wlxplugin.h"
#include "MpvEngine.h"

namespace {

struct MpvGtkPlayer {
    std::unique_ptr<MpvEngine> engine;
    GtkWidget *root = nullptr;
    GtkWidget *glArea = nullptr;
    bool active = false;
    std::shared_ptr<bool> alive = std::make_shared<bool>(true);
};

void *eglGetProcAddressWrapper(void *ctx, const char *name)
{
    (void)ctx;
    return reinterpret_cast<void *>(epoxy_eglGetProcAddress(name));
}

gboolean onGlRealize(GtkGLArea *area, gpointer userData)
{
    auto *p = static_cast<MpvGtkPlayer *>(userData);
    gtk_gl_area_make_current(area);
    if (gtk_gl_area_get_error(area) != nullptr)
        return FALSE;

    if (!p->engine->isValid())
        return FALSE;

    if (!p->engine->initRenderContext(eglGetProcAddressWrapper, nullptr)) {
        std::fprintf(stderr, "[mpv_wayland_gtk3] initRenderContext failed\n");
        return FALSE;
    }

    std::weak_ptr<bool> aliveWeak = p->alive;
    GtkWidget *glAreaRaw = p->glArea;
    p->engine->setUpdateCallback([glAreaRaw, aliveWeak]() {
        auto *ctx = new std::pair<GtkWidget *, std::weak_ptr<bool>>(glAreaRaw, aliveWeak);
        g_idle_add(+[](gpointer data) -> gboolean {
            auto *ctx = static_cast<std::pair<GtkWidget *, std::weak_ptr<bool>> *>(data);
            if (auto locked = ctx->second.lock())
                gtk_widget_queue_draw(ctx->first);
            delete ctx;
            return G_SOURCE_REMOVE;
        }, ctx);
    });

    return FALSE;
}

gboolean onGlRender(GtkGLArea *, GdkGLContext *, gpointer userData)
{
    auto *p = static_cast<MpvGtkPlayer *>(userData);
    int w = gtk_widget_get_allocated_width(p->glArea);
    int h = gtk_widget_get_allocated_height(p->glArea);
    gint scaleFactor = gtk_widget_get_scale_factor(p->glArea);

    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);

    // GtkGLArea binds its own FBO (not necessarily 0) before "render"
    // fires — query it instead of assuming the default framebuffer.
    GLint fbo = 0;
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &fbo);

    p->engine->render(fbo, w * scaleFactor, h * scaleFactor);
    return TRUE;
}

gboolean onMotion(GtkWidget *widget, GdkEventMotion *event, gpointer userData)
{
    auto *p = static_cast<MpvGtkPlayer *>(userData);
    gint scaleFactor = gtk_widget_get_scale_factor(widget);
    char cmd[64];
    std::snprintf(cmd, sizeof(cmd), "mouse %d %d",
                  (int)(event->x * scaleFactor), (int)(event->y * scaleFactor));
    p->engine->commandString(cmd);
    return TRUE;
}

gboolean onButtonPress(GtkWidget *widget, GdkEventButton *event, gpointer userData)
{
    auto *p = static_cast<MpvGtkPlayer *>(userData);
    p->active = true;
    gtk_widget_grab_focus(widget);
    if (event->button == GDK_BUTTON_PRIMARY) p->engine->commandString("keydown MOUSE_BTN0");
    else if (event->button == GDK_BUTTON_SECONDARY) p->engine->commandString("keydown MOUSE_BTN2");
    return TRUE;
}

gboolean onButtonRelease(GtkWidget *, GdkEventButton *event, gpointer userData)
{
    auto *p = static_cast<MpvGtkPlayer *>(userData);
    if (event->button == GDK_BUTTON_PRIMARY) p->engine->commandString("keyup MOUSE_BTN0");
    else if (event->button == GDK_BUTTON_SECONDARY) p->engine->commandString("keyup MOUSE_BTN2");
    return TRUE;
}

gboolean onScroll(GtkWidget *, GdkEventScroll *event, gpointer userData)
{
    auto *p = static_cast<MpvGtkPlayer *>(userData);
    if (event->direction == GDK_SCROLL_UP) p->engine->commandString("keypress WHEEL_UP");
    else if (event->direction == GDK_SCROLL_DOWN) p->engine->commandString("keypress WHEEL_DOWN");
    return TRUE;
}

gboolean onLeave(GtkWidget *, GdkEventCrossing *, gpointer userData)
{
    auto *p = static_cast<MpvGtkPlayer *>(userData);
    p->engine->commandString("mouse -100 -100");
    return TRUE;
}

const char *mapGdkKeyToMpvKey(guint keyval)
{
    switch (keyval) {
        case GDK_KEY_space: return "SPACE";
        case GDK_KEY_Left: return "LEFT";
        case GDK_KEY_Right: return "RIGHT";
        case GDK_KEY_Up: return "UP";
        case GDK_KEY_Down: return "DOWN";
        case GDK_KEY_Return:
        case GDK_KEY_KP_Enter: return "ENTER";
        case GDK_KEY_Escape: return "ESC";
        case GDK_KEY_BackSpace: return "BS";
        case GDK_KEY_Page_Up: return "PGUP";
        case GDK_KEY_Page_Down: return "PGDWN";
        case GDK_KEY_Home: return "HOME";
        case GDK_KEY_End: return "END";
        case GDK_KEY_Tab: return "TAB";
    }
    return nullptr;
}

gboolean onKeyPress(GtkWidget *, GdkEventKey *event, gpointer userData)
{
    auto *p = static_cast<MpvGtkPlayer *>(userData);
    if (!p->active) return FALSE;

    if (event->keyval == GDK_KEY_Escape) {
        p->active = false;
        return TRUE;
    }
    if (event->keyval == GDK_KEY_q && (event->state & GDK_CONTROL_MASK)) {
        p->active = false;
        return FALSE; // let it propagate so DC's own Ctrl+Q handling can fire
    }

    if (const char *mpvKey = mapGdkKeyToMpvKey(event->keyval)) {
        char cmd[64];
        std::snprintf(cmd, sizeof(cmd), "keypress %s", mpvKey);
        p->engine->commandString(cmd);
        return TRUE;
    }
    // Printable character fallback
    guint32 unicode = gdk_keyval_to_unicode(event->keyval);
    if (unicode >= 0x20 && unicode < 0x110000) {
        char utf8[8] = {0};
        g_unichar_to_utf8(unicode, utf8);
        std::string cmd = std::string("keypress ") + utf8;
        p->engine->commandString(cmd);
        return TRUE;
    }
    return FALSE;
}

void destroyPlayer(gpointer data)
{
    auto *p = static_cast<MpvGtkPlayer *>(data);
    *p->alive = false;
    delete p;
}

} // namespace

extern "C" {

HWND DCPCALL ListLoad(HWND ParentWin, char *FileToLoad, int ShowFlags)
{
    fprintf(stderr, "[mpv_wayland_gtk3] ListLoad: file=%s\n", FileToLoad);

    GtkWidget *parent = GTK_WIDGET(ParentWin);
    auto *p = new MpvGtkPlayer();
    p->engine = std::make_unique<MpvEngine>();

    p->root = gtk_event_box_new();
    gtk_container_add(GTK_CONTAINER(parent), p->root);

    p->glArea = gtk_gl_area_new();
    gtk_container_add(GTK_CONTAINER(p->root), p->glArea);
    gtk_widget_set_can_focus(p->glArea, TRUE);
    gtk_widget_add_events(p->glArea,
        GDK_POINTER_MOTION_MASK | GDK_BUTTON_PRESS_MASK | GDK_BUTTON_RELEASE_MASK |
        GDK_SCROLL_MASK | GDK_KEY_PRESS_MASK | GDK_LEAVE_NOTIFY_MASK);

    g_signal_connect(p->glArea, "realize", G_CALLBACK(onGlRealize), p);
    g_signal_connect(p->glArea, "render", G_CALLBACK(onGlRender), p);
    g_signal_connect(p->glArea, "motion-notify-event", G_CALLBACK(onMotion), p);
    g_signal_connect(p->glArea, "button-press-event", G_CALLBACK(onButtonPress), p);
    g_signal_connect(p->glArea, "button-release-event", G_CALLBACK(onButtonRelease), p);
    g_signal_connect(p->glArea, "scroll-event", G_CALLBACK(onScroll), p);
    g_signal_connect(p->glArea, "leave-notify-event", G_CALLBACK(onLeave), p);
    g_signal_connect(p->glArea, "key-press-event", G_CALLBACK(onKeyPress), p);

    g_object_set_data_full(G_OBJECT(p->root), "mpv-player", p, destroyPlayer);

    gtk_widget_show_all(p->root);

    if (p->engine->isValid())
        p->engine->loadFile(std::string(FileToLoad));

    fprintf(stderr, "[mpv_wayland_gtk3] ListLoad: widget=%p\n", (void *)p->root);
    return reinterpret_cast<HWND>(p->root);
}

void DCPCALL ListCloseWindow(HWND ListWin)
{
    fprintf(stderr, "[mpv_wayland_gtk3] ListCloseWindow\n");
    GtkWidget *root = GTK_WIDGET(ListWin);
    if (root) {
        auto *p = static_cast<MpvGtkPlayer *>(g_object_get_data(G_OBJECT(root), "mpv-player"));
        if (p && p->engine) p->engine->closeFile();
        gtk_widget_destroy(root);
    }
}

int DCPCALL ListSearchDialog(HWND, int)
{
    return LISTPLUGIN_OK;
}

int DCPCALL ListSendCommand(HWND, int, int)
{
    return LISTPLUGIN_ERROR;
}

void DCPCALL ListSetDefaultParams(ListDefaultParamStruct *)
{
}

void DCPCALL ListGetDetectString(char *DetectString, int maxlen)
{
    snprintf(DetectString, maxlen,
        "EXT=\"MKV\" | EXT=\"MP4\" | EXT=\"AVI\" | EXT=\"MOV\" | EXT=\"MP3\" | "
        "EXT=\"FLAC\" | EXT=\"WAV\" | EXT=\"OGG\" | EXT=\"WEBM\"");
}

} // extern "C"
