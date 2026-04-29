#include "../native_webview.h"

#import <Cocoa/Cocoa.h>
#import <WebKit/WebKit.h>

@interface NWVNavigationDelegate : NSObject <WKNavigationDelegate>
@property(nonatomic, assign) void *host;
@end

@interface NWVUIDelegate : NSObject <WKUIDelegate>
@property(nonatomic, assign) void *host;
@end

namespace {

struct Host {
    NSView *parent = nil;
    NSView *container = nil;
    WKWebView *webview = nil;
    NWVNavigationDelegate *navigationDelegate = nil;
    NWVUIDelegate *uiDelegate = nil;
    nwv_event_callback callback = nullptr;
    void *callback_user_data = nullptr;
    nwv_policy_callback policy_callback = nullptr;
    void *policy_user_data = nullptr;
    bool destroyed = false;
};

NSString *as_string(const void *value) {
    if (!value) {
        return nil;
    }
    return [NSString stringWithUTF8String:static_cast<const char *>(value)];
}

bool ask_policy(Host *host, int event_type, NSString *message = @"") {
    if (!host || host->destroyed || !host->policy_callback) {
        return false;
    }

    return host->policy_callback(host->policy_user_data, event_type, [message UTF8String]) != 0;
}

void emit_event(Host *host, int event_type, NSString *message = @"") {
    if (!host || host->destroyed || !host->callback) {
        return;
    }

    host->callback(host->callback_user_data, event_type, [message UTF8String]);
}

void resize_host(Host *host, int width, int height) {
    if (!host || host->destroyed || !host->container || !host->webview) {
        return;
    }

    NSRect frame = NSMakeRect(0, 0, width, height);
    [host->container setFrame:frame];
    [host->webview setFrame:frame];
}

void run_on_main_sync(dispatch_block_t block) {
    if ([NSThread isMainThread]) {
        block();
    } else {
        dispatch_sync(dispatch_get_main_queue(), block);
    }
}

} // namespace

@implementation NWVNavigationDelegate

- (void)webView:(WKWebView *)webView didStartProvisionalNavigation:(WKNavigation *)navigation {
    Host *nativeHost = static_cast<Host *>(self.host);
    NSString *url = webView.URL.absoluteString ?: @"";
    emit_event(nativeHost, NWV_EVENT_NAVIGATION_STARTED, url);
}

- (void)webView:(WKWebView *)webView didFinishNavigation:(WKNavigation *)navigation {
    Host *nativeHost = static_cast<Host *>(self.host);
    emit_event(nativeHost, NWV_EVENT_NAVIGATION_FINISHED, webView.URL.absoluteString ?: @"");
    emit_event(nativeHost, NWV_EVENT_TITLE_CHANGED, webView.title ?: @"");
}

- (void)webView:(WKWebView *)webView didFailNavigation:(WKNavigation *)navigation withError:(NSError *)error {
    Host *nativeHost = static_cast<Host *>(self.host);
    emit_event(nativeHost, NWV_EVENT_NAVIGATION_FAILED, error.localizedDescription ?: @"Navigation failed");
}

- (void)webView:(WKWebView *)webView didFailProvisionalNavigation:(WKNavigation *)navigation withError:(NSError *)error {
    Host *nativeHost = static_cast<Host *>(self.host);
    emit_event(nativeHost, NWV_EVENT_NAVIGATION_FAILED, error.localizedDescription ?: @"Navigation failed");
}

- (void)webView:(WKWebView *)webView
    decidePolicyForNavigationResponse:(WKNavigationResponse *)navigationResponse
                      decisionHandler:(void (^)(WKNavigationResponsePolicy))decisionHandler {
    Host *nativeHost = static_cast<Host *>(self.host);
    NSString *url = navigationResponse.response.URL.absoluteString ?: @"";

    if (!navigationResponse.canShowMIMEType) {
        emit_event(nativeHost, NWV_EVENT_DOWNLOAD_REQUESTED, url);
        decisionHandler(ask_policy(nativeHost, NWV_EVENT_DOWNLOAD_REQUESTED, url)
            ? WKNavigationResponsePolicyAllow
            : WKNavigationResponsePolicyCancel);
        return;
    }

    decisionHandler(WKNavigationResponsePolicyAllow);
}

@end

@implementation NWVUIDelegate

- (WKWebView *)webView:(WKWebView *)webView
    createWebViewWithConfiguration:(WKWebViewConfiguration *)configuration
               forNavigationAction:(WKNavigationAction *)navigationAction
                    windowFeatures:(WKWindowFeatures *)windowFeatures {
    Host *nativeHost = static_cast<Host *>(self.host);
    NSString *url = navigationAction.request.URL.absoluteString ?: @"";
    emit_event(nativeHost, NWV_EVENT_NEW_WINDOW_REQUESTED, url);
    return nil;
}

@end

extern "C" {

NWV_EXPORT void *nwv_create(void *parent_view, const nwv_options *options) {
    __block Host *host = new Host();

    run_on_main_sync(^{
        host->parent = (__bridge NSView *)parent_view;
        if (!host->parent) {
            delete host;
            host = nullptr;
            return;
        }

        NSRect frame = [host->parent bounds];
        host->container = [[NSView alloc] initWithFrame:frame];
        host->container.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;

        WKWebViewConfiguration *configuration = [[WKWebViewConfiguration alloc] init];
        configuration.allowsAirPlayForMediaPlayback = YES;

        if (@available(macOS 10.12, *)) {
            configuration.mediaTypesRequiringUserActionForPlayback = WKAudiovisualMediaTypeNone;
        }

        host->webview = [[WKWebView alloc] initWithFrame:frame configuration:configuration];
        host->webview.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;

        if (options && options->transparent) {
            host->webview.wantsLayer = YES;
            host->webview.layer.opaque = NO;
            host->webview.layer.backgroundColor = NSColor.clearColor.CGColor;
            if (@available(macOS 11.0, *)) {
                host->webview.underPageBackgroundColor = NSColor.clearColor;
            }
        }

        host->navigationDelegate = [[NWVNavigationDelegate alloc] init];
        host->navigationDelegate.host = host;
        host->webview.navigationDelegate = host->navigationDelegate;
        host->uiDelegate = [[NWVUIDelegate alloc] init];
        host->uiDelegate.host = host;
        host->webview.UIDelegate = host->uiDelegate;

        [host->container addSubview:host->webview];
        [host->parent addSubview:host->container];
    });

    return host;
}

NWV_EXPORT void nwv_destroy(void *handle) {
    auto *host = static_cast<Host *>(handle);
    if (!host) {
        return;
    }

    run_on_main_sync(^{
        host->destroyed = true;
        host->webview.navigationDelegate = nil;
        host->webview.UIDelegate = nil;
        [host->webview removeFromSuperview];
        [host->container removeFromSuperview];
        host->webview = nil;
        host->container = nil;
        host->navigationDelegate = nil;
        host->uiDelegate = nil;
    });

    delete host;
}

NWV_EXPORT void nwv_set_event_callback(void *handle, nwv_event_callback callback, void *user_data) {
    auto *host = static_cast<Host *>(handle);
    if (!host) {
        return;
    }

    host->callback = callback;
    host->callback_user_data = user_data;
    emit_event(host, NWV_EVENT_READY);
}

NWV_EXPORT void nwv_set_policy_callback(void *handle, nwv_policy_callback callback, void *user_data) {
    auto *host = static_cast<Host *>(handle);
    if (!host) {
        return;
    }

    host->policy_callback = callback;
    host->policy_user_data = user_data;
}

NWV_EXPORT void nwv_resize(void *handle, int width, int height) {
    auto *host = static_cast<Host *>(handle);
    run_on_main_sync(^{
        resize_host(host, width, height);
    });
}

NWV_EXPORT int nwv_navigate(void *handle, const void *url) {
    auto *host = static_cast<Host *>(handle);
    NSString *urlString = as_string(url);
    if (!host || host->destroyed || !urlString) {
        return 0;
    }

    run_on_main_sync(^{
        NSURL *nsurl = [NSURL URLWithString:urlString];
        if (nsurl) {
            [host->webview loadRequest:[NSURLRequest requestWithURL:nsurl]];
        }
    });

    return 1;
}

NWV_EXPORT int nwv_set_html(void *handle, const void *html, const void *base_url) {
    auto *host = static_cast<Host *>(handle);
    NSString *htmlString = as_string(html);
    NSString *baseString = as_string(base_url);
    if (!host || host->destroyed || !htmlString) {
        return 0;
    }

    run_on_main_sync(^{
        NSURL *baseURL = baseString ? [NSURL URLWithString:baseString] : nil;
        [host->webview loadHTMLString:htmlString baseURL:baseURL];
    });

    return 1;
}

NWV_EXPORT int nwv_reload(void *handle) {
    auto *host = static_cast<Host *>(handle);
    if (!host || host->destroyed) {
        return 0;
    }
    run_on_main_sync(^{
        [host->webview reload];
    });
    return 1;
}

NWV_EXPORT int nwv_go_back(void *handle) {
    auto *host = static_cast<Host *>(handle);
    if (!host || host->destroyed) {
        return 0;
    }
    run_on_main_sync(^{
        [host->webview goBack];
    });
    return 1;
}

NWV_EXPORT int nwv_go_forward(void *handle) {
    auto *host = static_cast<Host *>(handle);
    if (!host || host->destroyed) {
        return 0;
    }
    run_on_main_sync(^{
        [host->webview goForward];
    });
    return 1;
}

NWV_EXPORT int nwv_eval_js(void *handle, const void *script) {
    auto *host = static_cast<Host *>(handle);
    NSString *scriptString = as_string(script);
    if (!host || host->destroyed || !scriptString) {
        return 0;
    }
    run_on_main_sync(^{
        [host->webview evaluateJavaScript:scriptString completionHandler:nil];
    });
    return 1;
}

NWV_EXPORT int nwv_can_go_back(void *handle) {
    auto *host = static_cast<Host *>(handle);
    return host && !host->destroyed && host->webview.canGoBack ? 1 : 0;
}

NWV_EXPORT int nwv_can_go_forward(void *handle) {
    auto *host = static_cast<Host *>(handle);
    return host && !host->destroyed && host->webview.canGoForward ? 1 : 0;
}

} // extern "C"
