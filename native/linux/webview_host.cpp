#include "../native_webview.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <unordered_map>

#include <gdk/gdkx.h>
#include <gdk/gdkkeysyms.h>
#include <gdk-pixbuf/gdk-pixbuf.h>
#include <gtk/gtk.h>
#include <gtk/gtkx.h>
#include <jsc/jsc.h>
#include <libsoup/soup.h>
#include <webkit2/webkit2.h>

namespace {

constexpr const char *kScriptMessageHandler = "nativeWebView";
constexpr double kZoomStep = 1.1;
constexpr double kMinimumZoom = 0.25;
constexpr double kMaximumZoom = 5.0;

struct Session {
    std::string key;
    WebKitWebsiteDataManager *data_manager = nullptr;
    WebKitWebContext *context = nullptr;
    WebKitCookieManager *cookie_manager = nullptr;
    int references = 1;
};

struct Host {
    GtkWidget *plug = nullptr;
    WebKitWebView *webview = nullptr;
    Session *session = nullptr;
    WebKitUserContentManager *content_manager = nullptr;
    GCancellable *cancellable = nullptr;
    nwv_event_callback callback = nullptr;
    void *callback_user_data = nullptr;
    nwv_policy_callback policy_callback = nullptr;
    void *policy_user_data = nullptr;
    nwv_capture_callback capture_callback = nullptr;
    void *capture_user_data = nullptr;
    uintptr_t native_view = 0;
    int references = 1;
    bool context_menu_enabled = true;
    bool navigation_failed = false;
    bool script_handler_registered = false;
    bool destroyed = false;
};

struct SnapshotRequest {
    Host *host;
    int request_id;
    int x;
    int y;
    int width;
    int height;
    bool jpeg;
};

const char *as_utf8(const void *value) {
    return static_cast<const char *>(value);
}

void host_ref(Host *host) {
    ++host->references;
}

void host_unref(Host *host) {
    if (--host->references == 0) {
        delete host;
    }
}

struct HostGuard {
    explicit HostGuard(Host *value) : host(value) {
        host_ref(host);
    }

    ~HostGuard() {
        host_unref(host);
    }

    Host *host;
};

void emit_event(Host *host, int event_type, const char *message = "") {
    if (host && !host->destroyed && host->callback) {
        host->callback(host->callback_user_data, event_type, message ? message : "");
    }
}

int ask_policy(Host *host, int event_type, const char *message) {
    if (!host || host->destroyed || !host->policy_callback) {
        return 0;
    }
    return host->policy_callback(
        host->policy_user_data,
        event_type,
        message ? message : ""
    );
}

void emit_capture_error(Host *host, int request_id, const char *message) {
    if (host && !host->destroyed && host->capture_callback) {
        host->capture_callback(
            host->capture_user_data,
            request_id,
            0,
            nullptr,
            0,
            message ? message : "Native capture failed."
        );
    }
}

void emit_capture_data(Host *host, int request_id, const uint8_t *data, size_t size) {
    if (host && !host->destroyed && host->capture_callback) {
        host->capture_callback(
            host->capture_user_data,
            request_id,
            1,
            data,
            size,
            nullptr
        );
    }
}

void set_zoom_level(Host *host, double level) {
    if (!host || host->destroyed || !host->webview) {
        return;
    }
    webkit_web_view_set_zoom_level(
        host->webview,
        std::clamp(level, kMinimumZoom, kMaximumZoom)
    );
}

gboolean on_scroll_event(GtkWidget *, GdkEventScroll *event, Host *host) {
    HostGuard guard(host);
    if (!(event->state & GDK_CONTROL_MASK)) {
        return FALSE;
    }

    double direction = 0.0;
    if (event->direction == GDK_SCROLL_UP) {
        direction = 1.0;
    } else if (event->direction == GDK_SCROLL_DOWN) {
        direction = -1.0;
    } else if (event->direction == GDK_SCROLL_SMOOTH) {
        double delta_x = 0.0;
        double delta_y = 0.0;
        if (gdk_event_get_scroll_deltas(
                reinterpret_cast<GdkEvent *>(event),
                &delta_x,
                &delta_y
            )) {
            direction = delta_y < 0.0 ? 1.0 : (delta_y > 0.0 ? -1.0 : 0.0);
        }
    }
    if (direction == 0.0) {
        return FALSE;
    }

    const double current = webkit_web_view_get_zoom_level(host->webview);
    set_zoom_level(host, direction > 0.0 ? current * kZoomStep : current / kZoomStep);
    return TRUE;
}

gboolean on_key_press_event(GtkWidget *, GdkEventKey *event, Host *host) {
    HostGuard guard(host);
    if (!(event->state & GDK_CONTROL_MASK)) {
        return FALSE;
    }

    const double current = webkit_web_view_get_zoom_level(host->webview);
    switch (event->keyval) {
        case GDK_KEY_plus:
        case GDK_KEY_equal:
        case GDK_KEY_KP_Add:
            set_zoom_level(host, current * kZoomStep);
            return TRUE;
        case GDK_KEY_minus:
        case GDK_KEY_KP_Subtract:
            set_zoom_level(host, current / kZoomStep);
            return TRUE;
        case GDK_KEY_0:
        case GDK_KEY_KP_0:
            set_zoom_level(host, 1.0);
            return TRUE;
        default:
            return FALSE;
    }
}

bool initialize_gtk() {
    static const bool initialized = [] {
        gdk_set_allowed_backends("x11");
        if (!gtk_init_check(nullptr, nullptr)) {
            return false;
        }
        GdkDisplay *display = gdk_display_get_default();
        return display && GDK_IS_X11_DISPLAY(display);
    }();
    return initialized;
}

void on_load_changed(WebKitWebView *webview, WebKitLoadEvent event, Host *host) {
    HostGuard guard(host);
    const char *uri = webkit_web_view_get_uri(webview);
    if (event == WEBKIT_LOAD_STARTED) {
        host->navigation_failed = false;
        emit_event(host, NWV_EVENT_NAVIGATION_STARTED, uri);
    } else if (event == WEBKIT_LOAD_FINISHED) {
        if (host->navigation_failed) {
            host->navigation_failed = false;
            return;
        }
        emit_event(host, NWV_EVENT_NAVIGATION_FINISHED, uri);
        if (host->destroyed) {
            return;
        }
        emit_event(host, NWV_EVENT_TITLE_CHANGED, webkit_web_view_get_title(webview));
    }
}

gboolean on_load_failed(
    WebKitWebView *,
    WebKitLoadEvent,
    const char *,
    GError *error,
    Host *host
) {
    HostGuard guard(host);
    host->navigation_failed = true;
    emit_event(
        host,
        NWV_EVENT_NAVIGATION_FAILED,
        error ? error->message : "Navigation failed."
    );
    return FALSE;
}

void on_title_changed(GObject *object, GParamSpec *, Host *host) {
    HostGuard guard(host);
    emit_event(
        host,
        NWV_EVENT_TITLE_CHANGED,
        webkit_web_view_get_title(WEBKIT_WEB_VIEW(object))
    );
}

void on_zoom_changed(GObject *object, GParamSpec *, Host *host) {
    HostGuard guard(host);
    if (!host || host->destroyed) {
        return;
    }
    const double factor = webkit_web_view_get_zoom_level(WEBKIT_WEB_VIEW(object));
    const std::string value = std::to_string(factor);
    emit_event(host, NWV_EVENT_ZOOM_FACTOR_CHANGED, value.c_str());
}

gboolean on_decide_policy(
    WebKitWebView *,
    WebKitPolicyDecision *decision,
    WebKitPolicyDecisionType type,
    Host *host
) {
    HostGuard guard(host);
    if (type == WEBKIT_POLICY_DECISION_TYPE_NEW_WINDOW_ACTION) {
        auto *navigation = WEBKIT_NAVIGATION_POLICY_DECISION(decision);
        WebKitNavigationAction *action =
            webkit_navigation_policy_decision_get_navigation_action(navigation);
        WebKitURIRequest *request = webkit_navigation_action_get_request(action);
        const char *uri = request ? webkit_uri_request_get_uri(request) : "";
        emit_event(host, NWV_EVENT_NEW_WINDOW_REQUESTED, uri);
        webkit_policy_decision_ignore(decision);
        return TRUE;
    }

    if (type == WEBKIT_POLICY_DECISION_TYPE_RESPONSE) {
        auto *response_decision = WEBKIT_RESPONSE_POLICY_DECISION(decision);
        if (!webkit_response_policy_decision_is_mime_type_supported(response_decision)) {
            WebKitURIResponse *response =
                webkit_response_policy_decision_get_response(response_decision);
            const char *uri = response ? webkit_uri_response_get_uri(response) : "";
            emit_event(host, NWV_EVENT_DOWNLOAD_REQUESTED, uri);
            if (host->destroyed) {
                webkit_policy_decision_ignore(decision);
                return TRUE;
            }
            if (ask_policy(host, NWV_EVENT_DOWNLOAD_REQUESTED, uri)) {
                webkit_policy_decision_download(decision);
            } else {
                webkit_policy_decision_ignore(decision);
            }
            return TRUE;
        }
    }

    return FALSE;
}

void on_script_message(
    WebKitUserContentManager *,
    WebKitJavascriptResult *result,
    Host *host
) {
    HostGuard guard(host);
    JSCValue *value = webkit_javascript_result_get_js_value(result);
    if (!value) {
        return;
    }
    gchar *message = jsc_value_to_string(value);
    emit_event(host, NWV_EVENT_SCRIPT_MESSAGE, message);
    g_free(message);
}

gboolean on_context_menu(
    WebKitWebView *,
    WebKitContextMenu *,
    GdkEvent *,
    WebKitHitTestResult *,
    Host *host
) {
    return host && !host->context_menu_enabled ? TRUE : FALSE;
}

std::unordered_map<std::string, Session *> &sessions() {
    static std::unordered_map<std::string, Session *> value;
    return value;
}

Session *acquire_session(const nwv_options *options) {
    const char *data_folder = options ? as_utf8(options->user_data_folder) : nullptr;
    std::string key;
    if (data_folder && *data_folder) {
        gchar *canonical = g_canonicalize_filename(data_folder, nullptr);
        key = canonical;
        g_free(canonical);

        const auto existing = sessions().find(key);
        if (existing != sessions().end()) {
            ++existing->second->references;
            return existing->second;
        }
    }

    WebKitWebsiteDataManager *data_manager = nullptr;
    if (key.empty()) {
        data_manager = webkit_website_data_manager_new_ephemeral();
    } else if (g_mkdir_with_parents(key.c_str(), 0700) == 0) {
        gchar *cache_folder = g_build_filename(key.c_str(), "cache", nullptr);
        if (g_mkdir_with_parents(cache_folder, 0700) == 0) {
            data_manager = webkit_website_data_manager_new(
                "base-data-directory",
                key.c_str(),
                "base-cache-directory",
                cache_folder,
                nullptr
            );
        }
        g_free(cache_folder);
    }
    if (!data_manager) {
        return nullptr;
    }

    WebKitWebContext *context = webkit_web_context_new_with_website_data_manager(data_manager);
    if (!context) {
        g_object_unref(data_manager);
        return nullptr;
    }

    auto *session = new Session();
    session->key = key;
    session->data_manager = data_manager;
    session->context = context;
    session->cookie_manager = webkit_website_data_manager_get_cookie_manager(data_manager);
    if (session->cookie_manager && !key.empty()) {
        gchar *cookie_file = g_build_filename(key.c_str(), "cookies.sqlite", nullptr);
        webkit_cookie_manager_set_persistent_storage(
            session->cookie_manager,
            cookie_file,
            WEBKIT_COOKIE_PERSISTENT_STORAGE_SQLITE
        );
        g_free(cookie_file);
        sessions().emplace(key, session);
    } else if (!key.empty()) {
        g_object_unref(context);
        g_object_unref(data_manager);
        delete session;
        return nullptr;
    }
    return session;
}

void release_session(Session *session) {
    if (!session || --session->references > 0) {
        return;
    }
    if (!session->key.empty()) {
        sessions().erase(session->key);
    }
    g_object_unref(session->context);
    g_object_unref(session->data_manager);
    delete session;
}

SoupCookie *create_cookie(const nwv_cookie *cookie) {
    if (!cookie) {
        return nullptr;
    }
    const char *name = as_utf8(cookie->name);
    const char *value = as_utf8(cookie->value);
    const char *domain = as_utf8(cookie->domain);
    const char *path = as_utf8(cookie->path);
    if (!name || !*name || !value || !domain || !*domain) {
        return nullptr;
    }

    SoupCookie *native_cookie = soup_cookie_new(
        name,
        value,
        domain,
        path && *path ? path : "/",
        -1
    );
    if (!native_cookie) {
        return nullptr;
    }

    if (cookie->expires > 0) {
        GDateTime *expires = std::isfinite(cookie->expires)
                && cookie->expires < static_cast<double>(G_MAXINT64)
            ? g_date_time_new_from_unix_utc(static_cast<gint64>(cookie->expires))
            : nullptr;
        if (expires) {
            soup_cookie_set_expires(native_cookie, expires);
            g_date_time_unref(expires);
        }
    }
    soup_cookie_set_secure(native_cookie, cookie->secure != 0);
    soup_cookie_set_http_only(native_cookie, cookie->http_only != 0);

    SoupSameSitePolicy same_site = SOUP_SAME_SITE_POLICY_LAX;
    if (cookie->same_site == 0) {
        same_site = SOUP_SAME_SITE_POLICY_NONE;
    } else if (cookie->same_site == 2) {
        same_site = SOUP_SAME_SITE_POLICY_STRICT;
    }
    soup_cookie_set_same_site_policy(native_cookie, same_site);
    return native_cookie;
}

void finish_snapshot(GObject *source, GAsyncResult *result, gpointer user_data) {
    auto *request = static_cast<SnapshotRequest *>(user_data);
    Host *host = request->host;
    GError *error = nullptr;
    cairo_surface_t *surface = webkit_web_view_get_snapshot_finish(
        WEBKIT_WEB_VIEW(source),
        result,
        &error
    );

    if (!host->destroyed) {
        if (!surface) {
            emit_capture_error(
                host,
                request->request_id,
                error ? error->message : "WebKitGTK snapshot failed."
            );
        } else if (cairo_surface_get_type(surface) != CAIRO_SURFACE_TYPE_IMAGE) {
            emit_capture_error(
                host,
                request->request_id,
                "WebKitGTK returned an unsupported snapshot surface."
            );
        } else {
            const int surface_width = cairo_image_surface_get_width(surface);
            const int surface_height = cairo_image_surface_get_height(surface);
            const int x = std::clamp(request->x, 0, surface_width);
            const int y = std::clamp(request->y, 0, surface_height);
            const int width = request->width > 0
                ? std::min(request->width, surface_width - x)
                : surface_width - x;
            const int height = request->height > 0
                ? std::min(request->height, surface_height - y)
                : surface_height - y;

            if (width <= 0 || height <= 0) {
                emit_capture_error(
                    host,
                    request->request_id,
                    "Capture region is outside the webview bounds."
                );
            } else {
                GdkPixbuf *pixbuf = gdk_pixbuf_get_from_surface(surface, x, y, width, height);
                gchar *buffer = nullptr;
                gsize size = 0;
                GError *encode_error = nullptr;
                const gboolean encoded = pixbuf && (request->jpeg
                    ? gdk_pixbuf_save_to_buffer(
                        pixbuf,
                        &buffer,
                        &size,
                        "jpeg",
                        &encode_error,
                        "quality",
                        "72",
                        nullptr
                    )
                    : gdk_pixbuf_save_to_buffer(
                        pixbuf,
                        &buffer,
                        &size,
                        "png",
                        &encode_error,
                        nullptr
                    ));

                if (encoded && buffer && size > 0) {
                    emit_capture_data(
                        host,
                        request->request_id,
                        reinterpret_cast<const uint8_t *>(buffer),
                        size
                    );
                } else {
                    emit_capture_error(
                        host,
                        request->request_id,
                        encode_error ? encode_error->message : "Failed to encode snapshot."
                    );
                }
                if (encode_error) {
                    g_error_free(encode_error);
                }
                g_free(buffer);
                if (pixbuf) {
                    g_object_unref(pixbuf);
                }
            }
        }
    }

    if (error) {
        g_error_free(error);
    }
    if (surface) {
        cairo_surface_destroy(surface);
    }
    delete request;
    host_unref(host);
}

int start_snapshot(Host *host, SnapshotRequest *request) {
    if (!host || host->destroyed || !host->webview || !host->capture_callback) {
        delete request;
        return 0;
    }
    host_ref(host);
    webkit_web_view_get_snapshot(
        host->webview,
        WEBKIT_SNAPSHOT_REGION_VISIBLE,
        WEBKIT_SNAPSHOT_OPTIONS_NONE,
        host->cancellable,
        finish_snapshot,
        request
    );
    return 1;
}

} // namespace

extern "C" {

NWV_EXPORT void *nwv_create(void *, const nwv_options *options) {
    if (!initialize_gtk()) {
        return nullptr;
    }

    auto *host = new Host();
    host->session = acquire_session(options);
    if (!host->session) {
        delete host;
        return nullptr;
    }
    host->content_manager = webkit_user_content_manager_new();
    host->cancellable = g_cancellable_new();
    if (!host->content_manager || !host->cancellable) {
        nwv_destroy(host);
        return nullptr;
    }

    g_signal_connect(
        host->content_manager,
        "script-message-received::nativeWebView",
        G_CALLBACK(on_script_message),
        host
    );
    if (!webkit_user_content_manager_register_script_message_handler(
            host->content_manager,
            kScriptMessageHandler
        )) {
        nwv_destroy(host);
        return nullptr;
    }
    host->script_handler_registered = true;

    host->webview = WEBKIT_WEB_VIEW(g_object_new(
        WEBKIT_TYPE_WEB_VIEW,
        "web-context",
        host->session->context,
        "user-content-manager",
        host->content_manager,
        nullptr
    ));
    host->plug = gtk_plug_new(0);
    if (!host->webview || !host->plug) {
        nwv_destroy(host);
        return nullptr;
    }

    WebKitSettings *settings = webkit_web_view_get_settings(host->webview);
    webkit_settings_set_media_playback_requires_user_gesture(settings, FALSE);
    webkit_settings_set_enable_developer_extras(settings, FALSE);

    if (options && options->transparent) {
        const GdkRGBA transparent = {0.0, 0.0, 0.0, 0.0};
        webkit_web_view_set_background_color(host->webview, &transparent);
        gtk_widget_set_app_paintable(GTK_WIDGET(host->webview), TRUE);
    }

    g_signal_connect(host->webview, "load-changed", G_CALLBACK(on_load_changed), host);
    g_signal_connect(host->webview, "load-failed", G_CALLBACK(on_load_failed), host);
    g_signal_connect(host->webview, "notify::title", G_CALLBACK(on_title_changed), host);
    g_signal_connect(host->webview, "notify::zoom-level", G_CALLBACK(on_zoom_changed), host);
    g_signal_connect(host->webview, "scroll-event", G_CALLBACK(on_scroll_event), host);
    g_signal_connect(host->webview, "key-press-event", G_CALLBACK(on_key_press_event), host);
    g_signal_connect(host->webview, "decide-policy", G_CALLBACK(on_decide_policy), host);
    g_signal_connect(host->webview, "context-menu", G_CALLBACK(on_context_menu), host);

    gtk_container_add(GTK_CONTAINER(host->plug), GTK_WIDGET(host->webview));
    gtk_widget_add_events(
        GTK_WIDGET(host->webview),
        GDK_SCROLL_MASK | GDK_SMOOTH_SCROLL_MASK | GDK_KEY_PRESS_MASK
    );
    gtk_widget_set_can_focus(GTK_WIDGET(host->webview), TRUE);
    gtk_window_set_default_size(GTK_WINDOW(host->plug), 1, 1);
    gtk_widget_show_all(host->plug);
    host->native_view = static_cast<uintptr_t>(gtk_plug_get_id(GTK_PLUG(host->plug)));
    if (!host->native_view) {
        nwv_destroy(host);
        return nullptr;
    }
    return host;
}

NWV_EXPORT uintptr_t nwv_get_native_view(void *handle) {
    auto *host = static_cast<Host *>(handle);
    return host && !host->destroyed ? host->native_view : 0;
}

NWV_EXPORT void nwv_destroy(void *handle) {
    auto *host = static_cast<Host *>(handle);
    if (!host || host->destroyed) {
        return;
    }
    host->destroyed = true;
    if (host->cancellable) {
        g_cancellable_cancel(host->cancellable);
    }
    if (host->webview) {
        webkit_web_view_stop_loading(host->webview);
        webkit_web_view_try_close(host->webview);
    }
    if (host->content_manager && host->script_handler_registered) {
        webkit_user_content_manager_unregister_script_message_handler(
            host->content_manager,
            kScriptMessageHandler
        );
        host->script_handler_registered = false;
    }
    if (host->content_manager) {
        g_signal_handlers_disconnect_by_data(host->content_manager, host);
    }
    if (host->webview) {
        g_signal_handlers_disconnect_by_data(host->webview, host);
    }
    if (host->plug) {
        gtk_widget_destroy(host->plug);
        host->plug = nullptr;
        host->webview = nullptr;
    } else if (host->webview) {
        gtk_widget_destroy(GTK_WIDGET(host->webview));
        host->webview = nullptr;
    }
    if (host->content_manager) {
        g_object_unref(host->content_manager);
        host->content_manager = nullptr;
    }
    if (host->session) {
        release_session(host->session);
        host->session = nullptr;
    }
    if (host->cancellable) {
        g_object_unref(host->cancellable);
        host->cancellable = nullptr;
    }
    host_unref(host);
}

NWV_EXPORT void nwv_set_event_callback(
    void *handle,
    nwv_event_callback callback,
    void *user_data
) {
    auto *host = static_cast<Host *>(handle);
    if (!host || host->destroyed) {
        return;
    }
    host->callback = callback;
    host->callback_user_data = user_data;
    emit_event(host, NWV_EVENT_READY);
}

NWV_EXPORT void nwv_set_policy_callback(
    void *handle,
    nwv_policy_callback callback,
    void *user_data
) {
    auto *host = static_cast<Host *>(handle);
    if (!host || host->destroyed) {
        return;
    }
    host->policy_callback = callback;
    host->policy_user_data = user_data;
}

NWV_EXPORT void nwv_set_capture_callback(
    void *handle,
    nwv_capture_callback callback,
    void *user_data
) {
    auto *host = static_cast<Host *>(handle);
    if (!host || host->destroyed) {
        return;
    }
    host->capture_callback = callback;
    host->capture_user_data = user_data;
}

NWV_EXPORT void nwv_resize(void *handle, int width, int height) {
    auto *host = static_cast<Host *>(handle);
    if (!host || host->destroyed || !host->plug || width <= 0 || height <= 0) {
        return;
    }
    gtk_window_resize(GTK_WINDOW(host->plug), width, height);
    gtk_widget_set_size_request(GTK_WIDGET(host->webview), width, height);
}

NWV_EXPORT int nwv_navigate(void *handle, const void *url) {
    auto *host = static_cast<Host *>(handle);
    const char *value = as_utf8(url);
    if (!host || host->destroyed || !host->webview || !value || !*value) {
        return 0;
    }
    webkit_web_view_load_uri(host->webview, value);
    return 1;
}

NWV_EXPORT int nwv_set_html(void *handle, const void *html, const void *base_url) {
    auto *host = static_cast<Host *>(handle);
    const char *value = as_utf8(html);
    if (!host || host->destroyed || !host->webview || !value) {
        return 0;
    }
    webkit_web_view_load_html(host->webview, value, as_utf8(base_url));
    return 1;
}

NWV_EXPORT int nwv_reload(void *handle) {
    auto *host = static_cast<Host *>(handle);
    if (!host || host->destroyed || !host->webview) {
        return 0;
    }
    webkit_web_view_reload(host->webview);
    return 1;
}

NWV_EXPORT int nwv_go_back(void *handle) {
    auto *host = static_cast<Host *>(handle);
    if (!host || host->destroyed || !host->webview) {
        return 0;
    }
    webkit_web_view_go_back(host->webview);
    return 1;
}

NWV_EXPORT int nwv_go_forward(void *handle) {
    auto *host = static_cast<Host *>(handle);
    if (!host || host->destroyed || !host->webview) {
        return 0;
    }
    webkit_web_view_go_forward(host->webview);
    return 1;
}

NWV_EXPORT int nwv_eval_js(void *handle, const void *script) {
    auto *host = static_cast<Host *>(handle);
    const char *value = as_utf8(script);
    if (!host || host->destroyed || !host->webview || !value) {
        return 0;
    }
    webkit_web_view_evaluate_javascript(
        host->webview,
        value,
        -1,
        nullptr,
        nullptr,
        host->cancellable,
        nullptr,
        nullptr
    );
    return 1;
}

NWV_EXPORT int nwv_add_document_script(void *handle, const void *script) {
    auto *host = static_cast<Host *>(handle);
    const char *value = as_utf8(script);
    if (!host || host->destroyed || !host->content_manager || !value) {
        return 0;
    }
    WebKitUserScript *user_script = webkit_user_script_new(
        value,
        WEBKIT_USER_CONTENT_INJECT_ALL_FRAMES,
        WEBKIT_USER_SCRIPT_INJECT_AT_DOCUMENT_START,
        nullptr,
        nullptr
    );
    webkit_user_content_manager_add_script(host->content_manager, user_script);
    webkit_user_script_unref(user_script);
    return 1;
}

NWV_EXPORT int nwv_set_default_context_menu_enabled(void *handle, int enabled) {
    auto *host = static_cast<Host *>(handle);
    if (!host || host->destroyed) {
        return 0;
    }
    host->context_menu_enabled = enabled != 0;
    return 1;
}

NWV_EXPORT int nwv_set_devtools_enabled(void *handle, int enabled) {
    auto *host = static_cast<Host *>(handle);
    if (!host || host->destroyed || !host->webview) {
        return 0;
    }
    WebKitSettings *settings = webkit_web_view_get_settings(host->webview);
    webkit_settings_set_enable_developer_extras(settings, enabled != 0);
    return 1;
}

NWV_EXPORT int nwv_capture_png(
    void *handle,
    int request_id,
    int x,
    int y,
    int width,
    int height
) {
    auto *host = static_cast<Host *>(handle);
    return start_snapshot(
        host,
        new SnapshotRequest{host, request_id, x, y, width, height, false}
    );
}

NWV_EXPORT int nwv_capture_jpeg(void *handle, int request_id) {
    auto *host = static_cast<Host *>(handle);
    return start_snapshot(
        host,
        new SnapshotRequest{host, request_id, 0, 0, 0, 0, true}
    );
}

NWV_EXPORT int nwv_start_frame_stream(void *, int, int, int, int) {
    return 0;
}

NWV_EXPORT int nwv_stop_frame_stream(void *) {
    return 1;
}

NWV_EXPORT int nwv_set_cookie(void *handle, const nwv_cookie *cookie) {
    auto *host = static_cast<Host *>(handle);
    if (!host || host->destroyed || !host->session || !host->session->cookie_manager) {
        return 0;
    }
    SoupCookie *native_cookie = create_cookie(cookie);
    if (!native_cookie) {
        return 0;
    }
    webkit_cookie_manager_add_cookie(
        host->session->cookie_manager,
        native_cookie,
        host->cancellable,
        nullptr,
        nullptr
    );
    soup_cookie_free(native_cookie);
    return 1;
}

NWV_EXPORT int nwv_clear_cookies(void *handle) {
    auto *host = static_cast<Host *>(handle);
    if (!host || host->destroyed || !host->session || !host->session->cookie_manager) {
        return 0;
    }
    webkit_cookie_manager_delete_all_cookies(host->session->cookie_manager);
    return 1;
}

NWV_EXPORT int nwv_can_go_back(void *handle) {
    auto *host = static_cast<Host *>(handle);
    return host && !host->destroyed && host->webview
        && webkit_web_view_can_go_back(host->webview)
        ? 1
        : 0;
}

NWV_EXPORT int nwv_can_go_forward(void *handle) {
    auto *host = static_cast<Host *>(handle);
    return host && !host->destroyed && host->webview
        && webkit_web_view_can_go_forward(host->webview)
        ? 1
        : 0;
}

NWV_EXPORT int nwv_set_zoom_factor(void *handle, double factor) {
    auto *host = static_cast<Host *>(handle);
    if (!host || host->destroyed || !host->webview || !std::isfinite(factor)
        || factor < 0.25 || factor > 5.0) {
        return 0;
    }
    webkit_web_view_set_zoom_level(host->webview, factor);
    return 1;
}

NWV_EXPORT double nwv_get_zoom_factor(void *handle) {
    auto *host = static_cast<Host *>(handle);
    if (!host || host->destroyed || !host->webview) {
        return 0.0;
    }
    return webkit_web_view_get_zoom_level(host->webview);
}

} // extern "C"
