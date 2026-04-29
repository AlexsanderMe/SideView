#include "../native_webview.h"

#include <WebView2.h>
#include <wrl.h>
#include <Shlwapi.h>
#include <wincodec.h>
#include <algorithm>
#include <memory>
#include <string>
#include <vector>

using Microsoft::WRL::Callback;
using Microsoft::WRL::ComPtr;

namespace {

struct Host {
    HWND parent = nullptr;
    HWND hwnd = nullptr;
    ComPtr<ICoreWebView2Controller> controller;
    ComPtr<ICoreWebView2> webview;
    ComPtr<ICoreWebView2_2> webview2;
    ComPtr<ICoreWebView2_4> webview4;
    nwv_event_callback callback = nullptr;
    void *callback_user_data = nullptr;
    nwv_policy_callback policy_callback = nullptr;
    void *policy_user_data = nullptr;
    nwv_capture_callback capture_callback = nullptr;
    void *capture_user_data = nullptr;
    EventRegistrationToken navigation_starting_token {};
    EventRegistrationToken navigation_completed_token {};
    EventRegistrationToken title_changed_token {};
    EventRegistrationToken download_starting_token {};
    EventRegistrationToken new_window_requested_token {};
    EventRegistrationToken web_message_received_token {};
    bool destroyed = false;
};

struct HostHandle {
    std::shared_ptr<Host> host;
};

const wchar_t *as_wide(const void *value) {
    return static_cast<const wchar_t *>(value);
}

const wchar_t *as_wide_or_empty(const void *value) {
    const wchar_t *text = as_wide(value);
    return text ? text : L"";
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

void emit_capture(
    Host *host,
    int request_id,
    bool success,
    const std::vector<uint8_t> &data,
    const wchar_t *error_message = L""
) {
    if (!host || host->destroyed || !host->capture_callback) {
        return;
    }

    host->capture_callback(
        host->capture_user_data,
        request_id,
        success ? 1 : 0,
        data.empty() ? nullptr : data.data(),
        data.size(),
        error_message
    );
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

bool stream_to_bytes(IStream *stream, std::vector<uint8_t> &out) {
    if (!stream) {
        return false;
    }

    STATSTG stat {};
    if (FAILED(stream->Stat(&stat, STATFLAG_NONAME))) {
        return false;
    }

    ULARGE_INTEGER size = stat.cbSize;
    if (size.HighPart != 0) {
        return false;
    }

    LARGE_INTEGER start {};
    if (FAILED(stream->Seek(start, STREAM_SEEK_SET, nullptr))) {
        return false;
    }

    out.assign(static_cast<size_t>(size.LowPart), 0);
    if (out.empty()) {
        return true;
    }

    ULONG read = 0;
    return SUCCEEDED(stream->Read(out.data(), static_cast<ULONG>(out.size()), &read))
        && read == out.size();
}

bool encode_crop_png(
    const std::vector<uint8_t> &input,
    int x,
    int y,
    int width,
    int height,
    std::vector<uint8_t> &output,
    std::wstring &error
) {
    if (width <= 0 || height <= 0) {
        output = input;
        return true;
    }

    ComPtr<IStream> input_stream;
    input_stream.Attach(SHCreateMemStream(input.data(), static_cast<UINT>(input.size())));
    if (!input_stream) {
        error = L"Failed to create capture input stream.";
        return false;
    }

    ComPtr<IWICImagingFactory> factory;
    HRESULT hr = CoCreateInstance(
        CLSID_WICImagingFactory,
        nullptr,
        CLSCTX_INPROC_SERVER,
        IID_PPV_ARGS(&factory)
    );
    if (FAILED(hr) || !factory) {
        error = L"Failed to create WIC imaging factory.";
        return false;
    }

    ComPtr<IWICBitmapDecoder> decoder;
    hr = factory->CreateDecoderFromStream(
        input_stream.Get(),
        nullptr,
        WICDecodeMetadataCacheOnLoad,
        &decoder
    );
    if (FAILED(hr) || !decoder) {
        error = L"Failed to decode captured PNG.";
        return false;
    }

    ComPtr<IWICBitmapFrameDecode> frame;
    hr = decoder->GetFrame(0, &frame);
    if (FAILED(hr) || !frame) {
        error = L"Failed to read captured PNG frame.";
        return false;
    }

    UINT image_width = 0;
    UINT image_height = 0;
    frame->GetSize(&image_width, &image_height);
    if (image_width == 0 || image_height == 0) {
        error = L"Captured PNG has an empty size.";
        return false;
    }

    const int left = std::max(0, x);
    const int top = std::max(0, y);
    if (left >= static_cast<int>(image_width) || top >= static_cast<int>(image_height)) {
        error = L"Capture region is outside the webview bounds.";
        return false;
    }

    const int crop_width = std::min(width, static_cast<int>(image_width) - left);
    const int crop_height = std::min(height, static_cast<int>(image_height) - top);
    if (crop_width <= 0 || crop_height <= 0) {
        error = L"Capture region is empty.";
        return false;
    }

    if (left == 0 && top == 0
        && crop_width == static_cast<int>(image_width)
        && crop_height == static_cast<int>(image_height)) {
        output = input;
        return true;
    }

    WICRect rect { left, top, crop_width, crop_height };
    ComPtr<IWICBitmapClipper> clipper;
    hr = factory->CreateBitmapClipper(&clipper);
    if (FAILED(hr) || !clipper || FAILED(clipper->Initialize(frame.Get(), &rect))) {
        error = L"Failed to crop captured PNG.";
        return false;
    }

    ComPtr<IWICFormatConverter> converter;
    hr = factory->CreateFormatConverter(&converter);
    if (FAILED(hr) || !converter) {
        error = L"Failed to create WIC format converter.";
        return false;
    }

    hr = converter->Initialize(
        clipper.Get(),
        GUID_WICPixelFormat32bppBGRA,
        WICBitmapDitherTypeNone,
        nullptr,
        0.0,
        WICBitmapPaletteTypeCustom
    );
    if (FAILED(hr)) {
        error = L"Failed to convert cropped image.";
        return false;
    }

    ComPtr<IStream> output_stream;
    output_stream.Attach(SHCreateMemStream(nullptr, 0));
    if (!output_stream) {
        error = L"Failed to create capture output stream.";
        return false;
    }

    ComPtr<IWICBitmapEncoder> encoder;
    hr = factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder);
    if (FAILED(hr) || !encoder || FAILED(encoder->Initialize(output_stream.Get(), WICBitmapEncoderNoCache))) {
        error = L"Failed to initialize PNG encoder.";
        return false;
    }

    ComPtr<IWICBitmapFrameEncode> encoded_frame;
    hr = encoder->CreateNewFrame(&encoded_frame, nullptr);
    if (FAILED(hr) || !encoded_frame || FAILED(encoded_frame->Initialize(nullptr))) {
        error = L"Failed to create encoded PNG frame.";
        return false;
    }

    WICPixelFormatGUID pixel_format = GUID_WICPixelFormat32bppBGRA;
    hr = encoded_frame->SetSize(static_cast<UINT>(crop_width), static_cast<UINT>(crop_height));
    if (FAILED(hr) || FAILED(encoded_frame->SetPixelFormat(&pixel_format))) {
        error = L"Failed to configure encoded PNG frame.";
        return false;
    }

    hr = encoded_frame->WriteSource(converter.Get(), nullptr);
    if (FAILED(hr) || FAILED(encoded_frame->Commit()) || FAILED(encoder->Commit())) {
        error = L"Failed to encode cropped PNG.";
        return false;
    }

    if (!stream_to_bytes(output_stream.Get(), output)) {
        error = L"Failed to read cropped PNG bytes.";
        return false;
    }
    return true;
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

    host->webview->add_WebMessageReceived(
        Callback<ICoreWebView2WebMessageReceivedEventHandler>(
            [host](ICoreWebView2 *, ICoreWebView2WebMessageReceivedEventArgs *args) -> HRESULT {
                LPWSTR message = nullptr;
                if (SUCCEEDED(args->TryGetWebMessageAsString(&message)) && message) {
                    emit_event(host.get(), NWV_EVENT_SCRIPT_MESSAGE, message);
                    CoTaskMemFree(message);
                    return S_OK;
                }

                if (SUCCEEDED(args->get_WebMessageAsJson(&message)) && message) {
                    emit_event(host.get(), NWV_EVENT_SCRIPT_MESSAGE, message);
                    CoTaskMemFree(message);
                }
                return S_OK;
            }
        ).Get(),
        &host->web_message_received_token
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
                            host->webview.As(&host->webview2);
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
        host->webview->remove_WebMessageReceived(host->web_message_received_token);
    }

    if (host->controller) {
        host->controller->Close();
        host->controller.Reset();
    }

    host->webview.Reset();
    host->webview2.Reset();
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

NWV_EXPORT void nwv_set_capture_callback(void *handle, nwv_capture_callback callback, void *user_data) {
    auto *host_handle = static_cast<HostHandle *>(handle);
    if (!host_handle) {
        return;
    }
    auto host = host_handle->host;

    host->capture_callback = callback;
    host->capture_user_data = user_data;
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

NWV_EXPORT int nwv_add_document_script(void *handle, const void *script) {
    auto *host_handle = static_cast<HostHandle *>(handle);
    if (!host_handle || host_handle->host->destroyed || !host_handle->host->webview || !script) {
        return 0;
    }

    return SUCCEEDED(host_handle->host->webview->AddScriptToExecuteOnDocumentCreated(
        as_wide(script),
        nullptr
    )) ? 1 : 0;
}

NWV_EXPORT int nwv_set_default_context_menu_enabled(void *handle, int enabled) {
    auto *host_handle = static_cast<HostHandle *>(handle);
    if (!host_handle || host_handle->host->destroyed || !host_handle->host->webview) {
        return 0;
    }

    ComPtr<ICoreWebView2Settings> settings;
    if (FAILED(host_handle->host->webview->get_Settings(&settings)) || !settings) {
        return 0;
    }

    return SUCCEEDED(settings->put_AreDefaultContextMenusEnabled(enabled ? TRUE : FALSE)) ? 1 : 0;
}

NWV_EXPORT int nwv_set_devtools_enabled(void *handle, int enabled) {
    auto *host_handle = static_cast<HostHandle *>(handle);
    if (!host_handle || host_handle->host->destroyed || !host_handle->host->webview) {
        return 0;
    }

    ComPtr<ICoreWebView2Settings> settings;
    if (FAILED(host_handle->host->webview->get_Settings(&settings)) || !settings) {
        return 0;
    }

    return SUCCEEDED(settings->put_AreDevToolsEnabled(enabled ? TRUE : FALSE)) ? 1 : 0;
}

NWV_EXPORT int nwv_capture_png(void *handle, int request_id, int x, int y, int width, int height) {
    auto *host_handle = static_cast<HostHandle *>(handle);
    if (!host_handle || host_handle->host->destroyed || !host_handle->host->webview) {
        return 0;
    }

    auto host = host_handle->host;
    ComPtr<IStream> stream;
    stream.Attach(SHCreateMemStream(nullptr, 0));
    if (!stream) {
        emit_capture(host.get(), request_id, false, {}, L"Failed to create capture stream.");
        return 0;
    }

    HRESULT hr = host->webview->CapturePreview(
        COREWEBVIEW2_CAPTURE_PREVIEW_IMAGE_FORMAT_PNG,
        stream.Get(),
        Callback<ICoreWebView2CapturePreviewCompletedHandler>(
            [host, stream, request_id, x, y, width, height](HRESULT error_code) -> HRESULT {
                if (host->destroyed) {
                    return S_OK;
                }
                if (FAILED(error_code)) {
                    emit_capture(host.get(), request_id, false, {}, L"WebView2 CapturePreview failed.");
                    return S_OK;
                }

                std::vector<uint8_t> full_png;
                if (!stream_to_bytes(stream.Get(), full_png) || full_png.empty()) {
                    emit_capture(host.get(), request_id, false, {}, L"Failed to read captured PNG.");
                    return S_OK;
                }

                std::vector<uint8_t> result_png;
                std::wstring crop_error;
                if (!encode_crop_png(full_png, x, y, width, height, result_png, crop_error)) {
                    emit_capture(host.get(), request_id, false, {}, crop_error.c_str());
                    return S_OK;
                }

                emit_capture(host.get(), request_id, true, result_png);
                return S_OK;
            }
        ).Get()
    );

    if (FAILED(hr)) {
        emit_capture(host.get(), request_id, false, {}, L"Failed to start WebView2 CapturePreview.");
        return 0;
    }
    return 1;
}

NWV_EXPORT int nwv_set_cookie(void *handle, const nwv_cookie *cookie) {
    auto *host_handle = static_cast<HostHandle *>(handle);
    if (!host_handle || host_handle->host->destroyed || !host_handle->host->webview2 || !cookie) {
        return 0;
    }

    ComPtr<ICoreWebView2CookieManager> cookie_manager;
    if (FAILED(host_handle->host->webview2->get_CookieManager(&cookie_manager)) || !cookie_manager) {
        return 0;
    }

    ComPtr<ICoreWebView2Cookie> native_cookie;
    HRESULT hr = cookie_manager->CreateCookie(
        as_wide_or_empty(cookie->name),
        as_wide_or_empty(cookie->value),
        as_wide_or_empty(cookie->domain),
        as_wide_or_empty(cookie->path),
        &native_cookie
    );
    if (FAILED(hr) || !native_cookie) {
        return 0;
    }

    if (cookie->expires > 0) {
        native_cookie->put_Expires(cookie->expires);
    }
    native_cookie->put_IsSecure(cookie->secure ? TRUE : FALSE);
    native_cookie->put_IsHttpOnly(cookie->http_only ? TRUE : FALSE);
    native_cookie->put_SameSite(static_cast<COREWEBVIEW2_COOKIE_SAME_SITE_KIND>(cookie->same_site));

    return SUCCEEDED(cookie_manager->AddOrUpdateCookie(native_cookie.Get())) ? 1 : 0;
}

NWV_EXPORT int nwv_clear_cookies(void *handle) {
    auto *host_handle = static_cast<HostHandle *>(handle);
    if (!host_handle || host_handle->host->destroyed || !host_handle->host->webview2) {
        return 0;
    }

    ComPtr<ICoreWebView2CookieManager> cookie_manager;
    if (FAILED(host_handle->host->webview2->get_CookieManager(&cookie_manager)) || !cookie_manager) {
        return 0;
    }

    return SUCCEEDED(cookie_manager->DeleteAllCookies()) ? 1 : 0;
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
