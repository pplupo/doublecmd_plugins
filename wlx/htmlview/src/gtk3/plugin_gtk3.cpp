// htmlview -- GTK3/WebKitGTK backend, the sibling of src/qt6/plugin_qt6.cpp
// for GTK builds of Double Commander. Same design, same ini, same core
// probe; only the engine and the widget toolkit differ.
//
// The request gate is the one piece with no direct WebKitGTK equivalent of
// QWebEngineUrlRequestInterceptor. WebKit's pre-request hook in the UI
// process is "resource-load-started", which hands over the WebKitURIRequest
// before it is dispatched and allows it to be rewritten. Verified live that
// this is genuinely pre-dispatch: with a real HTTP server listening on
// localhost and a page referencing it, rewriting the URI here left the
// server log empty -- zero requests. That is the property the whole
// lockdown rests on, so it was measured rather than assumed.

#include <gtk/gtk.h>
#include <webkit2/webkit2.h>

#include <cstdio>
#include <cstring>
#include <exception>
#include <fstream>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <utility>

#include "wlxplugin.h"

#include "../core/html_probe.h"

#define PLUGNAME "htmlview"

namespace {

// ---------------------------------------------------------------------
// Settings
// ---------------------------------------------------------------------

std::string trim(const std::string &s) {
	size_t b = s.find_first_not_of(" \t\r\n");
	if (b == std::string::npos)
		return "";
	size_t e = s.find_last_not_of(" \t\r\n");
	return s.substr(b, e - b + 1);
}

// The Qt6 edition persists through QSettings' IniFormat, which quotes and
// backslash-escapes any value containing a double quote -- exactly what
// detect_string is full of. Reading and writing that same form here keeps
// one htmlview.ini valid for both editions instead of two dialects of the
// same file.
std::string iniUnescape(const std::string &raw) {
	if (raw.size() < 2 || raw.front() != '"' || raw.back() != '"')
		return raw;
	std::string inner = raw.substr(1, raw.size() - 2);
	std::string out;
	for (size_t i = 0; i < inner.size(); i++) {
		if (inner[i] == '\\' && i + 1 < inner.size())
			i++;
		out += inner[i];
	}
	return out;
}

std::string iniEscape(const std::string &value) {
	if (value.find('"') == std::string::npos &&
	    value.find(',') == std::string::npos && value == trim(value))
		return value;
	std::string out = "\"";
	for (char c : value) {
		if (c == '"' || c == '\\')
			out += '\\';
		out += c;
	}
	return out + "\"";
}

struct Settings {
	std::string theme = "system";  // "system", "dark", "light"
	bool forceDark = false;
	bool allowScripts = false;
	bool allowRemote = false;
	double zoomFactor = 1.0;
	unsigned long long maxFileSize = 32ULL * 1024 * 1024;
	std::string fallbackEncoding = "windows-1252";
	std::string detectString =
	    "(EXT=\"HTML\" | EXT=\"HTM\" | EXT=\"XHTML\" | EXT=\"XHT\" | "
	    "EXT=\"MHT\" | EXT=\"MHTML\")";

	void loadOrInitDefaults(const std::string &iniPath) {
		std::map<std::string, std::string> values;
		std::ifstream file(iniPath);
		if (file) {
			std::string line, section;
			while (std::getline(file, line)) {
				line = trim(line);
				if (line.empty() || line[0] == ';' || line[0] == '#')
					continue;
				if (line.front() == '[' && line.back() == ']') {
					section = line.substr(1, line.size() - 2);
					continue;
				}
				size_t eq = line.find('=');
				if (eq == std::string::npos)
					continue;
				values[section + "/" + trim(line.substr(0, eq))] =
				    trim(line.substr(eq + 1));
			}
		}
		auto get = [&](const char *key) -> const std::string * {
			auto it = values.find(std::string(PLUGNAME "/") + key);
			return it == values.end() ? nullptr : &it->second;
		};
		auto getBool = [&](const char *key, bool def) {
			const std::string *v = get(key);
			return v ? (*v == "true" || *v == "1") : def;
		};
		theme = get("theme") ? *get("theme") : theme;
		forceDark = getBool("force_dark", forceDark);
		allowScripts = getBool("allow_scripts", allowScripts);
		allowRemote = getBool("allow_remote_content", allowRemote);
		if (const std::string *v = get("zoom_factor"))
			zoomFactor = atof(v->c_str());
		if (const std::string *v = get("max_file_size"))
			maxFileSize = strtoull(v->c_str(), nullptr, 10);
		if (const std::string *v = get("fallback_encoding"))
			fallbackEncoding = iniUnescape(*v);
		if (const std::string *v = get("detect_string"))
			detectString = iniUnescape(*v);

		if (zoomFactor < 0.25 || zoomFactor > 5.0)
			zoomFactor = 1.0;
		if (maxFileSize == 0)
			maxFileSize = 32ULL * 1024 * 1024;

		save(iniPath);
	}

	void save(const std::string &iniPath) const {
		std::ofstream out(iniPath);
		if (!out)
			return;
		out << "[" PLUGNAME "]\n";
		out << "allow_remote_content=" << (allowRemote ? "true" : "false") << "\n";
		out << "allow_scripts=" << (allowScripts ? "true" : "false") << "\n";
		out << "detect_string=" << iniEscape(detectString) << "\n";
		out << "fallback_encoding=" << iniEscape(fallbackEncoding) << "\n";
		out << "force_dark=" << (forceDark ? "true" : "false") << "\n";
		out << "max_file_size=" << maxFileSize << "\n";
		out << "theme=" << theme << "\n";
		out << "zoom_factor=" << zoomFactor << "\n";
	}
};

Settings g_settings;
std::string g_configPath;

bool isSystemDark() {
	GtkSettings *settings = gtk_settings_get_default();
	if (!settings)
		return false;
	gboolean preferDark = FALSE;
	g_object_get(settings, "gtk-application-prefer-dark-theme", &preferDark,
	             nullptr);
	return preferDark;
}

bool resolveDarkMode() {
	if (g_settings.theme == "dark")
		return true;
	if (g_settings.theme == "light")
		return false;
	return isSystemDark();
}

// ---------------------------------------------------------------------
// Per-view state
// ---------------------------------------------------------------------

struct ViewState {
	GtkWidget *root = nullptr;      // the widget DC gets back
	GtkWidget *webView = nullptr;
	GtkWidget *findBar = nullptr;
	GtkWidget *findEntry = nullptr;
	GtkWidget *noticeBar = nullptr;
	GtkWidget *noticeLabel = nullptr;

	std::string filePath;
	std::string documentDir;  // absolute, no trailing slash
	std::string source;       // decoded UTF-8, empty on the direct-load path

	int blockedCount = 0;
	bool allowRemoteForView = false;

	// WebKit crashes outright when asked to load new content while a
	// previous load is still settling -- the same defect markdownview_gtk3
	// documents from a live symbolized backtrace. Never load reentrant.
	bool loadInFlight = false;

	// DC's ListSearchText is synchronous while WebKitFindController is not.
	// A bounded nested main loop bridges the two; this flag keeps a second
	// search from nesting inside the first.
	bool searchInFlight = false;
	bool searchFound = false;
	GMainLoop *searchLoop = nullptr;

	// Cleared in destroyState() before delete, so deferred idle callbacks
	// can tell that the panel closed underneath them.
	std::shared_ptr<bool> alive = std::make_shared<bool>(true);
};

// Duplicate "destroy" signals for one panel are a thing here (see
// markdownview_gtk3's note on a second teardown arriving with an already
// freed pointer). Tracking live instances independently of the pointer DC
// hands back turns that into a no-op instead of a use-after-free.
std::set<ViewState *> g_liveStates;

bool isLive(ViewState *state) {
	return state && g_liveStates.count(state) > 0;
}

// ---------------------------------------------------------------------
// The gate
// ---------------------------------------------------------------------

bool isInsideDocumentDir(const ViewState *state, const std::string &path) {
	if (state->documentDir.empty())
		return false;
	if (path == state->documentDir)
		return true;
	return path.compare(0, state->documentDir.size() + 1,
	                    state->documentDir + "/") == 0;
}

void onResourceLoadStarted(WebKitWebView *, WebKitWebResource *,
                           WebKitURIRequest *request, gpointer userData) {
	auto *state = static_cast<ViewState *>(userData);
	if (!isLive(state))
		return;

	const char *uri = webkit_uri_request_get_uri(request);
	if (!uri)
		return;

	// Inline content the document carries itself, and WebKit's own
	// bookkeeping URLs -- neither touches the disk or the network.
	if (g_str_has_prefix(uri, "data:") || g_str_has_prefix(uri, "about:") ||
	    g_str_has_prefix(uri, "blob:"))
		return;

	if (g_str_has_prefix(uri, "file://")) {
		gchar *localPath = g_filename_from_uri(uri, nullptr, nullptr);
		const bool allowed =
		    localPath && isInsideDocumentDir(state, std::string(localPath));
		g_free(localPath);
		if (allowed)
			return;
	} else if (state->allowRemoteForView) {
		return;
	}

	// There is no "cancel" on this signal. Repointing the request at
	// about:blank is the cancellation: verified against a live HTTP server
	// that nothing is dispatched.
	state->blockedCount++;
	webkit_uri_request_set_uri(request, "about:blank");
}

gboolean onDecidePolicy(WebKitWebView *, WebKitPolicyDecision *decision,
                        WebKitPolicyDecisionType type, gpointer userData) {
	auto *state = static_cast<ViewState *>(userData);
	if (!isLive(state) || type != WEBKIT_POLICY_DECISION_TYPE_NAVIGATION_ACTION)
		return FALSE;

	auto *navigation = WEBKIT_NAVIGATION_POLICY_DECISION(decision);
	WebKitNavigationAction *action =
	    webkit_navigation_policy_decision_get_navigation_action(navigation);
	WebKitURIRequest *request = webkit_navigation_action_get_request(action);
	const char *uri = webkit_uri_request_get_uri(request);
	if (!uri)
		return FALSE;

	if (g_str_has_prefix(uri, "about:") || g_str_has_prefix(uri, "data:"))
		return FALSE;  // let WebKit proceed

	if (g_str_has_prefix(uri, "file://")) {
		gchar *localPath = g_filename_from_uri(uri, nullptr, nullptr);
		const bool inside =
		    localPath && isInsideDocumentDir(state, std::string(localPath));
		g_free(localPath);
		if (inside)
			return FALSE;
	}

	// Same split as the Qt6 backend: a clicked link to somewhere else is
	// not a reason to repoint the preview pane, but it is not a dead end
	// either -- hand it to the desktop's browser.
	if (webkit_navigation_action_is_user_gesture(action)) {
		GtkWidget *toplevel = gtk_widget_get_toplevel(state->webView);
		gtk_show_uri_on_window(GTK_IS_WINDOW(toplevel) ? GTK_WINDOW(toplevel)
		                                               : nullptr,
		                       uri, GDK_CURRENT_TIME, nullptr);
	}
	webkit_policy_decision_ignore(decision);
	return TRUE;
}

// ---------------------------------------------------------------------
// Loading
// ---------------------------------------------------------------------

void applyEngineSettings(ViewState *state) {
	WebKitSettings *settings =
	    webkit_web_view_get_settings(WEBKIT_WEB_VIEW(state->webView));
	webkit_settings_set_enable_javascript(settings, g_settings.allowScripts);
	webkit_settings_set_enable_javascript_markup(settings,
	                                             g_settings.allowScripts);
	webkit_settings_set_enable_webgl(settings, FALSE);
	webkit_settings_set_enable_developer_extras(settings, FALSE);
	webkit_settings_set_enable_page_cache(settings, FALSE);
	webkit_settings_set_enable_html5_database(settings, FALSE);
	webkit_settings_set_enable_html5_local_storage(settings, FALSE);
	webkit_settings_set_media_playback_requires_user_gesture(settings, TRUE);
	webkit_settings_set_javascript_can_access_clipboard(settings, FALSE);
	webkit_settings_set_javascript_can_open_windows_automatically(settings,
	                                                              FALSE);
	// ON for the same reason as the Qt6 backend's
	// LocalContentCanAccessFileUrls: this is a coarse switch applied before
	// anything reaches onResourceLoadStarted, so leaving it off blocks the
	// document's own same-directory resources invisibly. The gate is the
	// single, visible authority on what loads -- and it is stricter, since
	// it confines file reads to the document's directory.
	webkit_settings_set_allow_file_access_from_file_urls(settings, TRUE);
	webkit_settings_set_allow_universal_access_from_file_urls(settings, FALSE);
}

// Prepended rather than spliced into <head>: the parser hoists a leading
// <meta>/<style> into the head itself, which is far more robust than
// finding an insertion point in markup we have not parsed.
std::string buildPrelude() {
	const bool dark = resolveDarkMode();
	std::string css = std::string(":root{color-scheme:") +
	                  (dark ? "dark" : "light") + "}";
	if (dark && g_settings.forceDark) {
		css +=
		    "html{filter:invert(1) hue-rotate(180deg);background:#fff}"
		    "img,video,svg,picture,iframe,embed,object"
		    "{filter:invert(1) hue-rotate(180deg)}";
	}
	return "<!doctype html><meta charset=\"utf-8\"><style>" + css + "</style>";
}

void updateNoticeBar(ViewState *state) {
	if (state->blockedCount <= 0 || state->allowRemoteForView) {
		gtk_widget_hide(state->noticeBar);
		return;
	}
	gchar *text = g_strdup_printf(
	    "%d resource(s) blocked. Scripts are %s.", state->blockedCount,
	    g_settings.allowScripts ? "enabled" : "disabled");
	gtk_label_set_text(GTK_LABEL(state->noticeLabel), text);
	g_free(text);
	gtk_widget_show_all(state->noticeBar);
}

void onLoadChanged(WebKitWebView *, WebKitLoadEvent event, gpointer userData) {
	auto *state = static_cast<ViewState *>(userData);
	if (!isLive(state) || event != WEBKIT_LOAD_FINISHED)
		return;
	state->loadInFlight = false;
	updateNoticeBar(state);
}

// Returns false when the file is not ours -- the caller turns that into a
// null handle so DC falls through to the next lister.
bool loadDocument(ViewState *state, const std::string &path) {
	const HtmlProbe::Result probe = HtmlProbe::probeFile(
	    path, g_settings.maxFileSize, g_settings.fallbackEncoding);
	if (probe.verdict != HtmlProbe::Verdict::Renderable) {
		fprintf(stderr, "[" PLUGNAME "_gtk3] declining \"%s\": %s\n",
		        path.c_str(), probe.detail.c_str());
		return false;
	}

	gchar *dirName = g_path_get_dirname(path.c_str());
	state->filePath = path;
	state->documentDir = dirName ? dirName : "";
	g_free(dirName);
	state->source.clear();
	state->blockedCount = 0;
	state->allowRemoteForView = g_settings.allowRemote;

	applyEngineSettings(state);

	const std::string baseUri = "file://" + path;
	bool loaded = false;

	if (probe.delivery == HtmlProbe::Delivery::InlineDecoded) {
		std::ifstream file(path, std::ios::binary);
		if (file) {
			std::string raw((std::istreambuf_iterator<char>(file)),
			                std::istreambuf_iterator<char>());
			std::string utf8;
			if (HtmlProbe::decodeToUtf8(raw, probe.encoding, utf8)) {
				state->source = utf8;
				const std::string html = buildPrelude() + utf8;
				state->loadInFlight = true;
				webkit_web_view_load_html(WEBKIT_WEB_VIEW(state->webView),
				                          html.c_str(), baseUri.c_str());
				loaded = true;
			}
			// Falling through means iconv did not know the declared
			// charset; handing the bytes to WebKit unmodified beats
			// rendering a failed transcode.
		}
	}

	if (!loaded) {
		state->loadInFlight = true;
		webkit_web_view_load_uri(WEBKIT_WEB_VIEW(state->webView),
		                         baseUri.c_str());
	}

	webkit_web_view_set_zoom_level(WEBKIT_WEB_VIEW(state->webView),
	                               g_settings.zoomFactor);
	return true;
}

// Every reload path goes through here. Two hazards, both learned the hard
// way in this repo's other WebKit plugin: calling load_html() while a load
// is in flight crashes WebKit, and doing the work synchronously from
// inside a signal handler (a context-menu item's "activate", WebKit's own
// "context-menu") crashes DC through reentrancy. So: defer to idle, and
// re-arm rather than load reentrant.
gboolean onReloadIdle(gpointer userData) {
	auto *context =
	    static_cast<std::pair<ViewState *, std::weak_ptr<bool>> *>(userData);
	ViewState *state = context->first;
	const bool stillAlive = context->second.lock() != nullptr;
	delete context;

	if (!stillAlive || !isLive(state))
		return G_SOURCE_REMOVE;

	if (state->loadInFlight) {
		auto *retry = new std::pair<ViewState *, std::weak_ptr<bool>>(
		    state, state->alive);
		g_timeout_add(100, onReloadIdle, retry);
		return G_SOURCE_REMOVE;
	}

	loadDocument(state, state->filePath);
	return G_SOURCE_REMOVE;
}

void requestReload(ViewState *state) {
	if (!isLive(state) || state->filePath.empty())
		return;
	auto *context =
	    new std::pair<ViewState *, std::weak_ptr<bool>>(state, state->alive);
	g_idle_add(onReloadIdle, context);
}

// ---------------------------------------------------------------------
// Find bar
// ---------------------------------------------------------------------

void doFind(ViewState *state, bool backward) {
	const gchar *text = gtk_entry_get_text(GTK_ENTRY(state->findEntry));
	if (!text || !*text)
		return;
	WebKitFindController *finder =
	    webkit_web_view_get_find_controller(WEBKIT_WEB_VIEW(state->webView));
	guint32 options =
	    WEBKIT_FIND_OPTIONS_CASE_INSENSITIVE | WEBKIT_FIND_OPTIONS_WRAP_AROUND;
	if (backward)
		options |= WEBKIT_FIND_OPTIONS_BACKWARDS;
	webkit_find_controller_search(finder, text, options, G_MAXUINT);
}

void showFindBar(ViewState *state) {
	gtk_widget_show_all(state->findBar);
	gtk_widget_grab_focus(state->findEntry);
	gtk_editable_select_region(GTK_EDITABLE(state->findEntry), 0, -1);
}

void hideFindBar(ViewState *state) {
	gtk_widget_hide(state->findBar);
	webkit_find_controller_search_finish(
	    webkit_web_view_get_find_controller(WEBKIT_WEB_VIEW(state->webView)));
	gtk_widget_grab_focus(state->webView);
}

gboolean onFindKeyPress(GtkWidget *, GdkEventKey *event, gpointer userData) {
	auto *state = static_cast<ViewState *>(userData);
	if (event->keyval == GDK_KEY_Escape) {
		hideFindBar(state);
		return TRUE;
	}
	if (event->keyval == GDK_KEY_Return || event->keyval == GDK_KEY_KP_Enter) {
		doFind(state, (event->state & GDK_SHIFT_MASK) != 0);
		return TRUE;
	}
	return FALSE;
}

gboolean onWebViewKeyPress(GtkWidget *, GdkEventKey *event, gpointer userData) {
	auto *state = static_cast<ViewState *>(userData);
	if ((event->state & GDK_CONTROL_MASK) &&
	    (event->keyval == GDK_KEY_f || event->keyval == GDK_KEY_F)) {
		showFindBar(state);
		return TRUE;
	}
	return FALSE;
}

GtkWidget *buildFindBar(ViewState *state) {
	GtkWidget *bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
	gtk_widget_set_halign(bar, GTK_ALIGN_END);
	gtk_widget_set_valign(bar, GTK_ALIGN_START);
	gtk_widget_set_margin_top(bar, 8);
	gtk_widget_set_margin_end(bar, 8);
	gtk_style_context_add_class(gtk_widget_get_style_context(bar), "background");

	state->findEntry = gtk_search_entry_new();
	gtk_entry_set_placeholder_text(GTK_ENTRY(state->findEntry), "Find...");
	gtk_widget_set_size_request(state->findEntry, 200, -1);
	g_signal_connect(state->findEntry, "search-changed",
	                 G_CALLBACK(+[](GtkSearchEntry *, gpointer data) {
		                 doFind(static_cast<ViewState *>(data), false);
	                 }),
	                 state);
	g_signal_connect(state->findEntry, "key-press-event",
	                 G_CALLBACK(onFindKeyPress), state);

	GtkWidget *previous = gtk_button_new_with_label("Prev");
	GtkWidget *next = gtk_button_new_with_label("Next");
	GtkWidget *close = gtk_button_new_with_label("\xe2\x9c\x95");
	g_signal_connect(previous, "clicked",
	                 G_CALLBACK(+[](GtkButton *, gpointer data) {
		                 doFind(static_cast<ViewState *>(data), true);
	                 }),
	                 state);
	g_signal_connect(next, "clicked", G_CALLBACK(+[](GtkButton *, gpointer data) {
		                 doFind(static_cast<ViewState *>(data), false);
	                 }),
	                 state);
	g_signal_connect(close, "clicked",
	                 G_CALLBACK(+[](GtkButton *, gpointer data) {
		                 hideFindBar(static_cast<ViewState *>(data));
	                 }),
	                 state);

	gtk_box_pack_start(GTK_BOX(bar), state->findEntry, FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(bar), previous, FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(bar), next, FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(bar), close, FALSE, FALSE, 0);
	return bar;
}

GtkWidget *buildNoticeBar(ViewState *state) {
	GtkWidget *bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
	gtk_widget_set_margin_start(bar, 6);
	gtk_widget_set_margin_end(bar, 6);
	gtk_style_context_add_class(gtk_widget_get_style_context(bar), "background");

	state->noticeLabel = gtk_label_new("");
	gtk_label_set_xalign(GTK_LABEL(state->noticeLabel), 0.0);
	GtkWidget *allow = gtk_button_new_with_label("Allow for this session");
	g_signal_connect(allow, "clicked",
	                 G_CALLBACK(+[](GtkButton *, gpointer data) {
		                 auto *state = static_cast<ViewState *>(data);
		                 g_settings.allowRemote = true;
		                 g_settings.save(g_configPath);
		                 requestReload(state);
	                 }),
	                 state);

	gtk_box_pack_start(GTK_BOX(bar), state->noticeLabel, TRUE, TRUE, 0);
	gtk_box_pack_start(GTK_BOX(bar), allow, FALSE, FALSE, 0);
	return bar;
}

// ---------------------------------------------------------------------
// Zoom, source view, context menu
// ---------------------------------------------------------------------

gboolean onScroll(GtkWidget *widget, GdkEventScroll *event, gpointer) {
	if (!(event->state & GDK_CONTROL_MASK))
		return FALSE;
	WebKitWebView *view = WEBKIT_WEB_VIEW(widget);
	double zoom = webkit_web_view_get_zoom_level(view);
	if (event->direction == GDK_SCROLL_UP) {
		zoom += 0.1;
	} else if (event->direction == GDK_SCROLL_DOWN) {
		zoom -= 0.1;
	} else if (event->direction == GDK_SCROLL_SMOOTH) {
		// Most mice under libinput report SMOOTH with a delta rather than
		// the discrete UP/DOWN directions -- markdownview_gtk3 hit exactly
		// this and had Ctrl+wheel silently do nothing.
		gdouble dx, dy;
		if (!gdk_event_get_scroll_deltas((GdkEvent *)event, &dx, &dy) ||
		    dy == 0.0)
			return FALSE;
		zoom -= dy * 0.1;
	} else {
		return FALSE;
	}
	zoom = CLAMP(zoom, 0.25, 5.0);
	webkit_web_view_set_zoom_level(view, zoom);
	g_settings.zoomFactor = zoom;
	g_settings.save(g_configPath);
	return TRUE;
}

std::string htmlEscape(const std::string &text) {
	std::string out;
	out.reserve(text.size() + text.size() / 8);
	for (char c : text) {
		switch (c) {
		case '&': out += "&amp;"; break;
		case '<': out += "&lt;"; break;
		case '>': out += "&gt;"; break;
		default: out += c;
		}
	}
	return out;
}

gboolean onShowSourceIdle(gpointer userData) {
	auto *context =
	    static_cast<std::pair<ViewState *, std::weak_ptr<bool>> *>(userData);
	ViewState *state = context->first;
	const bool stillAlive = context->second.lock() != nullptr;
	delete context;
	if (!stillAlive || !isLive(state) || state->loadInFlight)
		return G_SOURCE_REMOVE;

	const bool dark = resolveDarkMode();
	const std::string style = dark ? "background:#1e1e1e;color:#d4d4d4"
	                               : "background:#ffffff;color:#000000";
	// Rendered by the same locked-down view -- source view must not become
	// a second, laxer path into the engine.
	const std::string html =
	    "<!doctype html><meta charset=\"utf-8\"><body style=\"" + style +
	    ";margin:0\"><pre style=\"white-space:pre-wrap;word-break:break-word;"
	    "padding:8px;font-family:monospace\">" +
	    htmlEscape(state->source) + "</pre></body>";
	state->loadInFlight = true;
	webkit_web_view_load_html(WEBKIT_WEB_VIEW(state->webView), html.c_str(),
	                          nullptr);
	return G_SOURCE_REMOVE;
}

// Every menu item defers its real work to an idle callback for the
// reentrancy reason documented on onReloadIdle: this runs inside WebKit's
// own "context-menu" signal emission.
void appendMenuItem(GtkWidget *menu, const char *label, ViewState *state,
                    void (*action)(ViewState *)) {
	GtkWidget *item = gtk_menu_item_new_with_label(label);
	struct Payload {
		ViewState *state;
		void (*action)(ViewState *);
	};
	auto *payload = new Payload{state, action};
	g_signal_connect_data(
	    item, "activate", G_CALLBACK(+[](GtkMenuItem *, gpointer data) {
		    auto *payload = static_cast<Payload *>(data);
		    if (isLive(payload->state))
			    payload->action(payload->state);
	    }),
	    payload,
	    +[](gpointer data, GClosure *) { delete static_cast<Payload *>(data); },
	    G_CONNECT_DEFAULT);
	gtk_menu_shell_append(GTK_MENU_SHELL(menu), item);
}

void appendCheckItem(GtkWidget *menu, const char *label, bool checked,
                     ViewState *state, void (*action)(ViewState *)) {
	GtkWidget *item = gtk_check_menu_item_new_with_label(label);
	gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(item), checked);
	struct Payload {
		ViewState *state;
		void (*action)(ViewState *);
	};
	auto *payload = new Payload{state, action};
	g_signal_connect_data(
	    item, "toggled", G_CALLBACK(+[](GtkCheckMenuItem *, gpointer data) {
		    auto *payload = static_cast<Payload *>(data);
		    if (isLive(payload->state))
			    payload->action(payload->state);
	    }),
	    payload,
	    +[](gpointer data, GClosure *) { delete static_cast<Payload *>(data); },
	    G_CONNECT_DEFAULT);
	gtk_menu_shell_append(GTK_MENU_SHELL(menu), item);
}

gboolean onContextMenu(WebKitWebView *, WebKitContextMenu *,
                       GdkEvent *event, WebKitHitTestResult *, gpointer data) {
	auto *state = static_cast<ViewState *>(data);
	if (!isLive(state))
		return FALSE;

	GtkWidget *menu = gtk_menu_new();

	appendMenuItem(menu, "Copy", state, [](ViewState *s) {
		webkit_web_view_execute_editing_command(WEBKIT_WEB_VIEW(s->webView),
		                                        WEBKIT_EDITING_COMMAND_COPY);
	});
	appendMenuItem(menu, "Select All", state, [](ViewState *s) {
		webkit_web_view_execute_editing_command(
		    WEBKIT_WEB_VIEW(s->webView), WEBKIT_EDITING_COMMAND_SELECT_ALL);
	});
	appendMenuItem(menu, "Find...", state, showFindBar);

	gtk_menu_shell_append(GTK_MENU_SHELL(menu), gtk_separator_menu_item_new());

	GtkWidget *themeSub = gtk_menu_new();
	appendCheckItem(themeSub, "System", g_settings.theme == "system", state,
	                [](ViewState *s) {
		                g_settings.theme = "system";
		                g_settings.save(g_configPath);
		                requestReload(s);
	                });
	appendCheckItem(themeSub, "Dark", g_settings.theme == "dark", state,
	                [](ViewState *s) {
		                g_settings.theme = "dark";
		                g_settings.save(g_configPath);
		                requestReload(s);
	                });
	appendCheckItem(themeSub, "Light", g_settings.theme == "light", state,
	                [](ViewState *s) {
		                g_settings.theme = "light";
		                g_settings.save(g_configPath);
		                requestReload(s);
	                });
	GtkWidget *themeItem = gtk_menu_item_new_with_label("Theme");
	gtk_menu_item_set_submenu(GTK_MENU_ITEM(themeItem), themeSub);
	gtk_menu_shell_append(GTK_MENU_SHELL(menu), themeItem);

	appendCheckItem(menu, "Force Dark on Light Pages", g_settings.forceDark,
	                state, [](ViewState *s) {
		                g_settings.forceDark = !g_settings.forceDark;
		                g_settings.save(g_configPath);
		                requestReload(s);
	                });

	gtk_menu_shell_append(GTK_MENU_SHELL(menu), gtk_separator_menu_item_new());

	appendCheckItem(menu, "Allow JavaScript", g_settings.allowScripts, state,
	                [](ViewState *s) {
		                g_settings.allowScripts = !g_settings.allowScripts;
		                g_settings.save(g_configPath);
		                requestReload(s);
	                });
	appendCheckItem(menu, "Allow Remote Content", g_settings.allowRemote, state,
	                [](ViewState *s) {
		                g_settings.allowRemote = !g_settings.allowRemote;
		                g_settings.save(g_configPath);
		                requestReload(s);
	                });

	gtk_menu_shell_append(GTK_MENU_SHELL(menu), gtk_separator_menu_item_new());

	if (!state->source.empty()) {
		appendMenuItem(menu, "View Source", state, [](ViewState *s) {
			auto *context =
			    new std::pair<ViewState *, std::weak_ptr<bool>>(s, s->alive);
			g_idle_add(onShowSourceIdle, context);
		});
	}
	appendMenuItem(menu, "Open in Browser", state, [](ViewState *s) {
		const std::string uri = "file://" + s->filePath;
		GtkWidget *toplevel = gtk_widget_get_toplevel(s->webView);
		gtk_show_uri_on_window(
		    GTK_IS_WINDOW(toplevel) ? GTK_WINDOW(toplevel) : nullptr,
		    uri.c_str(), GDK_CURRENT_TIME, nullptr);
	});
	appendMenuItem(menu, "Reload", state, requestReload);

	gtk_widget_show_all(menu);
	// The real event, not NULL: popping up at the pointer without it
	// misbehaves when called from inside WebKit's "context-menu" emission.
	gtk_menu_popup_at_pointer(GTK_MENU(menu), event);
	return TRUE;  // suppress WebKit's own menu
}

// ---------------------------------------------------------------------
// Synchronous search for DC
// ---------------------------------------------------------------------

void onFoundText(WebKitFindController *, guint, gpointer userData) {
	auto *state = static_cast<ViewState *>(userData);
	if (!isLive(state))
		return;
	state->searchFound = true;
	if (state->searchLoop && g_main_loop_is_running(state->searchLoop))
		g_main_loop_quit(state->searchLoop);
}

void onFailedToFindText(WebKitFindController *, gpointer userData) {
	auto *state = static_cast<ViewState *>(userData);
	if (!isLive(state))
		return;
	state->searchFound = false;
	if (state->searchLoop && g_main_loop_is_running(state->searchLoop))
		g_main_loop_quit(state->searchLoop);
}

gboolean onSearchTimeout(gpointer userData) {
	auto *state = static_cast<ViewState *>(userData);
	if (isLive(state) && state->searchLoop &&
	    g_main_loop_is_running(state->searchLoop))
		g_main_loop_quit(state->searchLoop);
	return G_SOURCE_REMOVE;
}

void destroyState(GtkWidget *, gpointer userData) {
	auto *state = static_cast<ViewState *>(userData);
	if (!isLive(state))
		return;  // stale or duplicate "destroy" for an already torn-down panel
	g_liveStates.erase(state);
	*state->alive = false;  // before delete -- deferred idles read this
	if (state->searchLoop && g_main_loop_is_running(state->searchLoop))
		g_main_loop_quit(state->searchLoop);
	delete state;
}

}  // namespace

// ---------------------------------------------------------------------
// Lister entry points
// ---------------------------------------------------------------------

extern "C" {

HWND DCPCALL ListLoad(HWND ParentWin, char *FileToLoad, int ShowFlags)
try {
	(void)ShowFlags;
	if (!ParentWin || !FileToLoad)
		return nullptr;

	auto *state = new ViewState();
	g_liveStates.insert(state);

	// Ephemeral context: no cookie jar, no cache, no local storage
	// outliving the preview -- the GTK counterpart of the Qt6 backend's
	// off-the-record profile.
	WebKitWebContext *context = webkit_web_context_new_ephemeral();
	state->webView = webkit_web_view_new_with_context(context);
	g_object_unref(context);  // the view holds its own reference
	gtk_widget_set_name(state->webView, "htmlview_webview");

	g_signal_connect(state->webView, "resource-load-started",
	                 G_CALLBACK(onResourceLoadStarted), state);
	g_signal_connect(state->webView, "decide-policy",
	                 G_CALLBACK(onDecidePolicy), state);
	g_signal_connect(state->webView, "load-changed", G_CALLBACK(onLoadChanged),
	                 state);
	g_signal_connect(state->webView, "context-menu", G_CALLBACK(onContextMenu),
	                 state);
	g_signal_connect(state->webView, "key-press-event",
	                 G_CALLBACK(onWebViewKeyPress), state);
	gtk_widget_add_events(state->webView, GDK_SCROLL_MASK);
	g_signal_connect(state->webView, "scroll-event", G_CALLBACK(onScroll),
	                 state);

	WebKitFindController *finder =
	    webkit_web_view_get_find_controller(WEBKIT_WEB_VIEW(state->webView));
	g_signal_connect(finder, "found-text", G_CALLBACK(onFoundText), state);
	g_signal_connect(finder, "failed-to-find-text",
	                 G_CALLBACK(onFailedToFindText), state);

	if (!loadDocument(state, FileToLoad)) {
		// The whole point: a null handle is what makes DC try the next
		// lister, or its own viewer, instead of showing an empty pane.
		g_liveStates.erase(state);
		gtk_widget_destroy(state->webView);
		delete state;
		return nullptr;
	}

	// GTK_CONTAINER add() does not register the child the way GtkLayout
	// expects: DC's ResizeWindow (uwlxmodule.pas) calls gtk_layout_move()
	// on this widget, which asserts its parent is exactly this GtkLayout --
	// only gtk_layout_put() sets that up.
	GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
	gtk_layout_put(GTK_LAYOUT(GTK_WIDGET(ParentWin)), box, 0, 0);
	state->root = box;

	state->noticeBar = buildNoticeBar(state);
	gtk_box_pack_start(GTK_BOX(box), state->noticeBar, FALSE, FALSE, 0);

	// GtkOverlay floats the find bar over the view without changing the
	// widget DC gets back.
	GtkWidget *overlay = gtk_overlay_new();
	gtk_container_add(GTK_CONTAINER(overlay), state->webView);
	state->findBar = buildFindBar(state);
	gtk_overlay_add_overlay(GTK_OVERLAY(overlay), state->findBar);
	gtk_box_pack_start(GTK_BOX(box), overlay, TRUE, TRUE, 0);

	gtk_widget_show_all(box);
	gtk_widget_hide(state->findBar);
	gtk_widget_hide(state->noticeBar);
	gtk_widget_grab_focus(state->webView);

	g_signal_connect(box, "destroy", G_CALLBACK(destroyState), state);
	g_object_set_data(G_OBJECT(box), "__htmlview_state_ptr", state);

	return (HWND)box;
} catch (const std::exception &e) {
	fprintf(stderr, "[" PLUGNAME "_gtk3] ListLoad exception: %s\n", e.what());
	return nullptr;
} catch (...) {
	fprintf(stderr, "[" PLUGNAME "_gtk3] ListLoad unknown exception\n");
	return nullptr;
}

// Reuses the existing window when the user steps to the next file, rather
// than tearing down and rebuilding a web view per arrow key press.
int DCPCALL ListLoadNext(HWND ParentWin, HWND PluginWin, char *FileToLoad,
                         int ShowFlags)
try {
	(void)ParentWin;
	(void)ShowFlags;
	if (!PluginWin || !FileToLoad)
		return LISTPLUGIN_ERROR;
	auto *state = static_cast<ViewState *>(
	    g_object_get_data(G_OBJECT(PluginWin), "__htmlview_state_ptr"));
	if (!isLive(state))
		return LISTPLUGIN_ERROR;

	// The probe has to pass before anything is torn down or reloaded --
	// declining here must leave the currently displayed document intact.
	const HtmlProbe::Result probe = HtmlProbe::probeFile(
	    FileToLoad, g_settings.maxFileSize, g_settings.fallbackEncoding);
	if (probe.verdict != HtmlProbe::Verdict::Renderable)
		return LISTPLUGIN_ERROR;

	state->filePath = FileToLoad;
	requestReload(state);
	return LISTPLUGIN_OK;
} catch (...) {
	return LISTPLUGIN_ERROR;
}

void DCPCALL ListCloseWindow(HWND ListWin) {
	if (ListWin)
		gtk_widget_destroy(GTK_WIDGET(ListWin));
}

void DCPCALL ListGetDetectString(char *DetectString, int maxlen) {
	if (!DetectString || maxlen <= 0)
		return;
	// snprintf's size argument already accounts for the terminator; passing
	// maxlen - 1 would truncate a character early, and would be SIZE_MAX
	// after conversion if maxlen were ever 0.
	snprintf(DetectString, (size_t)maxlen, "%s",
	         g_settings.detectString.c_str());
}

int DCPCALL ListSearchText(HWND ListWin, char *SearchString,
                           int SearchParameter) {
	if (!ListWin || !SearchString)
		return LISTPLUGIN_ERROR;
	auto *state = static_cast<ViewState *>(
	    g_object_get_data(G_OBJECT(ListWin), "__htmlview_state_ptr"));
	if (!isLive(state))
		return LISTPLUGIN_ERROR;

	// DC wants a hit/miss as the return value; WebKitFindController answers
	// through signals. A bounded nested main loop bridges the two. The
	// in-flight guard matters more here than the timeout: nesting a second
	// loop inside the first is the reentrancy pattern that crashes WebKit
	// plugins in this repo.
	if (state->searchInFlight)
		return state->searchFound ? LISTPLUGIN_OK : LISTPLUGIN_ERROR;

	guint32 options = WEBKIT_FIND_OPTIONS_WRAP_AROUND;
	if (!(SearchParameter & lcs_matchcase))
		options |= WEBKIT_FIND_OPTIONS_CASE_INSENSITIVE;
	if (SearchParameter & lcs_backwards)
		options |= WEBKIT_FIND_OPTIONS_BACKWARDS;
	// lcs_wholewords has no counterpart in WebKit's find options; the
	// search runs as a substring match rather than reporting a false miss.

	state->searchInFlight = true;
	state->searchFound = false;
	state->searchLoop = g_main_loop_new(nullptr, FALSE);

	WebKitFindController *finder =
	    webkit_web_view_get_find_controller(WEBKIT_WEB_VIEW(state->webView));
	webkit_find_controller_search(finder, SearchString, options, G_MAXUINT);

	const guint timeoutId = g_timeout_add(3000, onSearchTimeout, state);
	g_main_loop_run(state->searchLoop);
	g_source_remove(timeoutId);

	g_main_loop_unref(state->searchLoop);
	state->searchLoop = nullptr;
	state->searchInFlight = false;
	return state->searchFound ? LISTPLUGIN_OK : LISTPLUGIN_ERROR;
}

int DCPCALL ListSendCommand(HWND ListWin, int Command, int Parameter) {
	(void)Parameter;
	if (!ListWin)
		return LISTPLUGIN_ERROR;
	auto *state = static_cast<ViewState *>(
	    g_object_get_data(G_OBJECT(ListWin), "__htmlview_state_ptr"));
	if (!isLive(state))
		return LISTPLUGIN_ERROR;

	switch (Command) {
	case lc_copy:
		webkit_web_view_execute_editing_command(WEBKIT_WEB_VIEW(state->webView),
		                                        WEBKIT_EDITING_COMMAND_COPY);
		return LISTPLUGIN_OK;
	case lc_selectall:
		webkit_web_view_execute_editing_command(
		    WEBKIT_WEB_VIEW(state->webView), WEBKIT_EDITING_COMMAND_SELECT_ALL);
		return LISTPLUGIN_OK;
	case lc_focus:
		gtk_widget_grab_focus(state->webView);
		return LISTPLUGIN_OK;
	case lc_newparams:
		// lcp_wraptext, lcp_fittowindow, lcp_ansi and friends are text-
		// viewer concepts: wrapping and codepage are decided by the
		// document's own CSS and by the charset resolved at load time.
		// Accepted rather than refused so DC does not treat the plugin as
		// broken.
		return LISTPLUGIN_OK;
	default:
		// Includes lc_setpercent: scrolling to a position needs script
		// execution, which is what this plugin refuses to do by default.
		return LISTPLUGIN_ERROR;
	}
}

int DCPCALL ListPrint(HWND ListWin, char *FileToPrint, char *DefPrinter,
                      int PrintFlags, RECT *Margins) {
	(void)FileToPrint;
	(void)DefPrinter;
	(void)PrintFlags;
	(void)Margins;
	if (!ListWin)
		return LISTPLUGIN_ERROR;
	auto *state = static_cast<ViewState *>(
	    g_object_get_data(G_OBJECT(ListWin), "__htmlview_state_ptr"));
	if (!isLive(state))
		return LISTPLUGIN_ERROR;

	WebKitPrintOperation *operation =
	    webkit_print_operation_new(WEBKIT_WEB_VIEW(state->webView));
	GtkWidget *toplevel = gtk_widget_get_toplevel(state->webView);
	WebKitPrintOperationResponse response = webkit_print_operation_run_dialog(
	    operation, GTK_IS_WINDOW(toplevel) ? GTK_WINDOW(toplevel) : nullptr);
	g_object_unref(operation);
	return response == WEBKIT_PRINT_OPERATION_RESPONSE_CANCEL ? LISTPLUGIN_ERROR
	                                                          : LISTPLUGIN_OK;
}

void DCPCALL ListSetDefaultParams(ListDefaultParamStruct *dps) {
	if (!dps)
		return;
	// DefaultIniName is the SDK's sanctioned way to find the plugin's
	// configuration directory. No dladdr, no fixed-size path buffers, and
	// no setlocale(LC_ALL, "") -- this is a shared library inside someone
	// else's process.
	const std::string iniName(dps->DefaultIniName);
	const size_t slash = iniName.find_last_of('/');
	const std::string dir =
	    slash == std::string::npos ? "." : iniName.substr(0, slash);
	g_configPath = dir + "/" PLUGNAME ".ini";
	g_settings.loadOrInitDefaults(g_configPath);
}

}  // extern "C"
