#pragma once

#include <stdint.h>
#include <stddef.h>

#ifdef _WIN32
#define NWV_EXPORT __declspec(dllexport)
#else
#define NWV_EXPORT __attribute__((visibility("default")))
#endif

#ifdef __cplusplus
extern "C" {
#endif

typedef enum nwv_event_type {
    NWV_EVENT_READY = 1,
    NWV_EVENT_NAVIGATION_STARTED = 2,
    NWV_EVENT_NAVIGATION_FINISHED = 3,
    NWV_EVENT_NAVIGATION_FAILED = 4,
    NWV_EVENT_TITLE_CHANGED = 5,
    NWV_EVENT_DOWNLOAD_REQUESTED = 6,
    NWV_EVENT_NEW_WINDOW_REQUESTED = 7,
    NWV_EVENT_SCRIPT_MESSAGE = 8
} nwv_event_type;

typedef void (*nwv_event_callback)(void *user_data, int event_type, const void *message);
typedef int (*nwv_policy_callback)(void *user_data, int event_type, const void *message);
typedef void (*nwv_capture_callback)(
    void *user_data,
    int request_id,
    int success,
    const uint8_t *data,
    size_t size,
    const void *error_message
);

typedef struct nwv_options {
    const void *user_data_folder;
    const void *runtime_path;
    const void *session_id;
    int transparent;
} nwv_options;

typedef struct nwv_cookie {
    const void *name;
    const void *value;
    const void *domain;
    const void *path;
    double expires;
    int secure;
    int http_only;
    int same_site;
} nwv_cookie;

NWV_EXPORT void *nwv_create(void *parent_view, const nwv_options *options);
NWV_EXPORT void nwv_destroy(void *handle);
NWV_EXPORT void nwv_set_event_callback(void *handle, nwv_event_callback callback, void *user_data);
NWV_EXPORT void nwv_set_policy_callback(void *handle, nwv_policy_callback callback, void *user_data);
NWV_EXPORT void nwv_set_capture_callback(void *handle, nwv_capture_callback callback, void *user_data);
NWV_EXPORT void nwv_resize(void *handle, int width, int height);
NWV_EXPORT int nwv_navigate(void *handle, const void *url);
NWV_EXPORT int nwv_set_html(void *handle, const void *html, const void *base_url);
NWV_EXPORT int nwv_reload(void *handle);
NWV_EXPORT int nwv_go_back(void *handle);
NWV_EXPORT int nwv_go_forward(void *handle);
NWV_EXPORT int nwv_eval_js(void *handle, const void *script);
NWV_EXPORT int nwv_add_document_script(void *handle, const void *script);
NWV_EXPORT int nwv_set_default_context_menu_enabled(void *handle, int enabled);
NWV_EXPORT int nwv_set_devtools_enabled(void *handle, int enabled);
NWV_EXPORT int nwv_capture_png(void *handle, int request_id, int x, int y, int width, int height);
NWV_EXPORT int nwv_set_cookie(void *handle, const nwv_cookie *cookie);
NWV_EXPORT int nwv_clear_cookies(void *handle);
NWV_EXPORT int nwv_can_go_back(void *handle);
NWV_EXPORT int nwv_can_go_forward(void *handle);

#ifdef __cplusplus
}
#endif
