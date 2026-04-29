#include "../native_webview.h"

#include <WebView2.h>
#include <wrl.h>
#include <Shlwapi.h>
#include <memory>
#include <string>

using Microsoft::WRL::Callback;
using Microsoft::WRL::ComPtr;

namespace {

struct Host {
    HWND parent = nullptr;
    HWND hwnd = nullptr;
    ComPtr<ICoreWebView2Controller> controller;
    ComPtr<ICoreWebView2> webview;
    ComPtr<ICoreWebView2_4> webview4;
    nwv_event_callback callback = nullptr;
    void *callback_user_data = nullptr;
    nwv_policy_callback policy_callback = nullptr;
    void *policy_user_data = nullptr;
    EventRegistrationToken navigation_starting_token {};
    EventRegistrationToken navigation_completed_token {};
    EventRegistrationToken title_changed_token {};
    EventRegistrationToken download_starting_token {};
    EventRegistrationToken new_window_requested_token {};
    bool destroyed = false;
};

struct HostHandle {
    std::shared_ptr<Host> host;
};

const wchar_t *as_wide(const void *value) {
    return static_cast<const wchar_t *>(value);
}

void emit_event(Host *host, int event_type, const wchar_t *message = L"") {
    if (host && !host->destroyed && host->callback) {
        host->callback(host->callback_user_data, event_type, message);
    }
}

bool ask_policy(Host *host, int event_type, const wchar_t *message = L"") {
    if (!host || host->destroyed || !host->policy_callback) {
        return false;
    }
    return host->policy_callback(host->policy_user_data, event_type, message) != 0;
}

RECT client_rect(HWND hwnd) {
    RECT rect {};
    GetClientRect(hwnd, &rect);
    return rect;
}

void update_bounds(Host *host) {
    if (!host || host->destroyed || !host->hwnd) {
        return;
    }

    RECT rect = client_rect(host->parent);
    SetWindowPos(
        host->hwnd,
        nullptr,
        0,
        0,
        rect.right - rect.left,
        rect.bottom - rect.top,
        SWP_NOZORDER | SWP_NOACTIVATE
    );

    if (host->controller) {
        RECT host_rect = client_rect(host->hwnd);
        host->controller->put_Bounds(host_rect);
    }
}

void attach_events(const std::shared_ptr<Host> &host) {
    host->webview->add_NavigationStarting(
        Callback<ICoreWebView2NavigationStartingEventHandler>(
            [host](ICoreWebView2 *, ICoreWebView2NavigationStartingEventArgs *args) -> HRESULT {
                LPWSTR uri = nullptr;
                if (SUCCEEDED(args->get_Uri(&uri)) && uri) {
                    emit_event(host.get(), NWV_EVENT_NAVIGATION_STARTED, uri);
                    CoTaskMemFree(uri);
                } else {
                    emit_event(host.get(), NWV_EVENT_NAVIGATION_STARTED);
                }
                return S_OK;
            }
        ).Get(),
        &host->navigation_starting_token
    );

    host->webview->add_NavigationCompleted(
        Callback<ICoreWebView2NavigationCompletedEventHandler>(
            [host](ICoreWebView2 *, ICoreWebView2NavigationCompletedEventArgs *args) -> HRESULT {
                BOOL success = FALSE;
                args->get_IsSuccess(&success);
                emit_event(
                    host.get(),
                    success ? NWV_EVENT_NAVIGATION_FINISHED : NWV_EVENT_NAVIGATION_FAILED
                );
                return S_OK;
            }
        ).Get(),
        &host->navigation_completed_token
    );

    host->webview->add_DocumentTitleChanged(
        Callback<ICoreWebView2DocumentTitleChangedEventHandler>(
            [host](ICoreWebView2 *sender, IUnknown *) -> HRESULT {
                LPWSTR title = nullptr;
                if (SUCCEEDED(sender->get_DocumentTitle(&title)) && title) {
                    emit_event(host.get(), NWV_EVENT_TITLE_CHANGED, title);
                    CoTaskMemFree(title);
                }
                return S_OK;
            }
        ).Get(),
        &host->title_changed_token
    );

    if (SUCCEEDED(host->webview.As(&host->webview4)) && host->webview4) {
        host->webview4->add_DownloadStarting(
            Callback<ICoreWebView2DownloadStartingEventHandler>(
                [host](ICoreWebView2 *, ICoreWebView2DownloadStartingEventArgs *args) -> HRESULT {
                    ComPtr<ICoreWebView2DownloadOperation> operation;
                    std::wstring uri;

                    if (SUCCEEDED(args->get_DownloadOperation(&operation)) && operation) {
                        LPWSTR raw_uri = nullptr;
                        if (SUCCEEDED(operation->get_Uri(&raw_uri)) && raw_uri) {
                            uri = raw_uri;
                            CoTaskMemFree(raw_uri);
                        }
                    }

                    emit_event(host.get(), NWV_EVENT_DOWNLOAD_REQUESTED, uri.c_str());

                    const bool allowed = ask_policy(
                        host.get(),
                        NWV_EVENT_DOWNLOAD_REQUESTED,
                        uri.c_str()
                    );
                    args->put_Cancel(allowed ? FALSE : TRUE);
                    args->put_Handled(TRUE);
                    return S_OK;
                }
            ).Get(),
            &host->download_starting_token
        );
    }

    host->webview->add_NewWindowRequested(
        Callback<ICoreWebView2NewWindowRequestedEventHandler>(
            [host](ICoreWebView2 *, ICoreWebView2NewWindowRequestedEventArgs *args) -> HRESULT {
                LPWSTR uri = nullptr;
                if (SUCCEEDED(args->get_Uri(&uri)) && uri) {
                    emit_event(host.get(), NWV_EVENT_NEW_WINDOW_REQUESTED, uri);
                    CoTaskMemFree(uri);
                } else {
                    emit_event(host.get(), NWV_EVENT_NEW_WINDOW_REQUESTED);
                }

                args->put_Handled(TRUE);
                return S_OK;
            }
        ).Get(),
        &host->new_window_requested_token
    );
}

} // namespace

extern "C" {

NWV_EXPORT void *nwv_create(void *parent_view, const nwv_options *options) {
    auto host = std::make_shared<Host>();
    host->parent = static_cast<HWND>(parent_view);

    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    host->hwnd = CreateWindowExW(
        0,
        L"STATIC",
        L"",
        WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS | WS_CLIPCHILDREN,
        0,
        0,
        1,
        1,
        host->parent,
        nullptr,
        GetModuleHandleW(nullptr),
        nullptr
    );

    if (!host->hwnd) {
        return nullptr;
    }

    const wchar_t *runtime_path = options ? as_wide(options->runtime_path) : nullptr;
    const wchar_t *user_data_folder = options ? as_wide(options->user_data_folder) : nullptr;

    HRESULT hr = CreateCoreWebView2EnvironmentWithOptions(
        runtime_path,
        user_data_folder,
        nullptr,
        Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>(
            [host](HRESULT result, ICoreWebView2Environment *environment) -> HRESULT {
                if (host->destroyed) {
                    return S_OK;
                }
                if (FAILED(result) || !environment) {
                    emit_event(host.get(), NWV_EVENT_NAVIGATION_FAILED, L"Failed to create WebView2 environment");
                    return result;
                }

                environment->CreateCoreWebView2Controller(
                    host->hwnd,
                    Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
                        [host](HRESULT controller_result, ICoreWebView2Controller *controller) -> HRESULT {
                            if (host->destroyed) {
                                return S_OK;
                            }
                            if (FAILED(controller_result) || !controller) {
                                emit_event(host.get(), NWV_EVENT_NAVIGATION_FAILED, L"Failed to create WebView2 controller");
                                return controller_result;
                            }

                            host->controller = controller;
                            host->controller->get_CoreWebView2(&host->webview);
                            update_bounds(host.get());

                            if (host->webview) {
                                attach_events(host);
                            }

                            emit_event(host.get(), NWV_EVENT_READY);
                            return S_OK;
                        }
                    ).Get()
                );
                return S_OK;
            }
        ).Get()
    );

    if (FAILED(hr)) {
        DestroyWindow(host->hwnd);
        return nullptr;
    }

    update_bounds(host.get());
    return new HostHandle { host };
}

NWV_EXPORT void nwv_destroy(void *handle) {
    auto *host_handle = static_cast<HostHandle *>(handle);
    if (!host_handle) {
        return;
    }
    auto host = host_handle->host;

    host->destroyed = true;
    if (host->webview) {
        host->webview->remove_NavigationStarting(host->navigation_starting_token);
        host->webview->remove_NavigationCompleted(host->navigation_completed_token);
        host->webview->remove_DocumentTitleChanged(host->title_changed_token);
        if (host->webview4) {
            host->webview4->remove_DownloadStarting(host->download_starting_token);
        }
        host->webview->remove_NewWindowRequested(host->new_window_requested_token);
    }

    if (host->controller) {
        host->controller->Close();
        host->controller.Reset();
    }

    host->webview.Reset();
    host->webview4.Reset();

    if (host->hwnd) {
        DestroyWindow(host->hwnd);
    }

    delete host_handle;
}

NWV_EXPORT void nwv_set_event_callback(void *handle, nwv_event_callback callback, void *user_data) {
    auto *host_handle = static_cast<HostHandle *>(handle);
    if (!host_handle) {
        return;
    }
    auto host = host_handle->host;

    host->callback = callback;
    host->callback_user_data = user_data;
}

NWV_EXPORT void nwv_set_policy_callback(void *handle, nwv_policy_callback callback, void *user_data) {
    auto *host_handle = static_cast<HostHandle *>(handle);
    if (!host_handle) {
        return;
    }
    auto host = host_handle->host;

    host->policy_callback = callback;
    host->policy_user_data = user_data;
}

NWV_EXPORT void nwv_resize(void *handle, int width, int height) {
    auto *host_handle = static_cast<HostHandle *>(handle);
    if (!host_handle) {
        return;
    }
    auto host = host_handle->host;
    if (host->destroyed || !host->hwnd) {
        return;
    }

    SetWindowPos(host->hwnd, nullptr, 0, 0, width, height, SWP_NOZORDER | SWP_NOACTIVATE);
    if (host->controller) {
        RECT rect { 0, 0, width, height };
        host->controller->put_Bounds(rect);
    }
}

NWV_EXPORT int nwv_navigate(void *handle, const void *url) {
    auto *host_handle = static_cast<HostHandle *>(handle);
    if (!host_handle || host_handle->host->destroyed || !host_handle->host->webview || !url) {
        return 0;
    }
    return SUCCEEDED(host_handle->host->webview->Navigate(as_wide(url))) ? 1 : 0;
}

NWV_EXPORT int nwv_set_html(void *handle, const void *html, const void *) {
    auto *host_handle = static_cast<HostHandle *>(handle);
    if (!host_handle || host_handle->host->destroyed || !host_handle->host->webview || !html) {
        return 0;
    }
    return SUCCEEDED(host_handle->host->webview->NavigateToString(as_wide(html))) ? 1 : 0;
}

NWV_EXPORT int nwv_reload(void *handle) {
    auto *host_handle = static_cast<HostHandle *>(handle);
    return host_handle && !host_handle->host->destroyed && host_handle->host->webview
        && SUCCEEDED(host_handle->host->webview->Reload()) ? 1 : 0;
}

NWV_EXPORT int nwv_go_back(void *handle) {
    auto *host_handle = static_cast<HostHandle *>(handle);
    return host_handle && !host_handle->host->destroyed && host_handle->host->webview
        && SUCCEEDED(host_handle->host->webview->GoBack()) ? 1 : 0;
}

NWV_EXPORT int nwv_go_forward(void *handle) {
    auto *host_handle = static_cast<HostHandle *>(handle);
    return host_handle && !host_handle->host->destroyed && host_handle->host->webview
        && SUCCEEDED(host_handle->host->webview->GoForward()) ? 1 : 0;
}

NWV_EXPORT int nwv_eval_js(void *handle, const void *script) {
    auto *host_handle = static_cast<HostHandle *>(handle);
    if (!host_handle || host_handle->host->destroyed || !host_handle->host->webview || !script) {
        return 0;
    }
    return SUCCEEDED(host_handle->host->webview->ExecuteScript(as_wide(script), nullptr)) ? 1 : 0;
}

NWV_EXPORT int nwv_can_go_back(void *handle) {
    auto *host_handle = static_cast<HostHandle *>(handle);
    BOOL can_go_back = FALSE;
    if (host_handle && !host_handle->host->destroyed && host_handle->host->webview) {
        host_handle->host->webview->get_CanGoBack(&can_go_back);
    }
    return can_go_back ? 1 : 0;
}

NWV_EXPORT int nwv_can_go_forward(void *handle) {
    auto *host_handle = static_cast<HostHandle *>(handle);
    BOOL can_go_forward = FALSE;
    if (host_handle && !host_handle->host->destroyed && host_handle->host->webview) {
        host_handle->host->webview->get_CanGoForward(&can_go_forward);
    }
    return can_go_forward ? 1 : 0;
}

} // extern "C"
