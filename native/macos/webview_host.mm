#include "../native_webview.h"

#import <Cocoa/Cocoa.h>
#import <WebKit/WebKit.h>

@interface NWVNavigationDelegate : NSObject <WKNavigationDelegate>
@property(nonatomic, assign) void *host;
@end

@interface NWVUIDelegate : NSObject <WKUIDelegate>
@property(nonatomic, assign) void *host;
@end

@interface NWVScriptMessageHandler : NSObject <WKScriptMessageHandler>
@property(nonatomic, assign) void *host;
@end

namespace {

struct Host {
    NSView *parent = nil;
    NSView *container = nil;
    WKWebView *webview = nil;
    WKWebsiteDataStore *dataStore = nil;
    WKUserContentController *userContentController = nil;
    NWVNavigationDelegate *navigationDelegate = nil;
    NWVUIDelegate *uiDelegate = nil;
    NWVScriptMessageHandler *scriptMessageHandler = nil;
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

WKWebsiteDataStore *data_store_from_options(const nwv_options *options) {
    NSString *sessionID = options ? as_string(options->session_id) : nil;
    if (sessionID.length == 0) {
        return [WKWebsiteDataStore defaultDataStore];
    }

    NSUUID *uuid = [[NSUUID alloc] initWithUUIDString:sessionID];
    if (!uuid) {
        return [WKWebsiteDataStore defaultDataStore];
    }

    SEL selector = @selector(dataStoreForIdentifier:);
    if ([WKWebsiteDataStore respondsToSelector:selector]) {
        return [WKWebsiteDataStore dataStoreForIdentifier:uuid];
    }

    return [WKWebsiteDataStore defaultDataStore];
}

NSHTTPCookie *cookie_from_native(const nwv_cookie *cookie) {
    if (!cookie) {
        return nil;
    }

    NSString *name = as_string(cookie->name);
    NSString *value = as_string(cookie->value);
    NSString *domain = as_string(cookie->domain);
    NSString *path = as_string(cookie->path) ?: @"/";
    if (name.length == 0 || !value || domain.length == 0) {
        return nil;
    }

    NSMutableDictionary<NSHTTPCookiePropertyKey, id> *properties = [@{
        NSHTTPCookieName: name,
        NSHTTPCookieValue: value,
        NSHTTPCookieDomain: domain,
        NSHTTPCookiePath: path,
    } mutableCopy];

    if (cookie->expires > 0) {
        properties[NSHTTPCookieExpires] = [NSDate dateWithTimeIntervalSince1970:cookie->expires];
    }
    if (cookie->secure) {
        properties[NSHTTPCookieSecure] = @"TRUE";
    }
    if (cookie->http_only) {
        properties[NSHTTPCookieHTTPOnly] = @"TRUE";
    }

    return [NSHTTPCookie cookieWithProperties:properties];
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

@implementation NWVScriptMessageHandler

- (void)userContentController:(WKUserContentController *)userContentController
      didReceiveScriptMessage:(WKScriptMessage *)message {
    Host *nativeHost = static_cast<Host *>(self.host);
    id body = message.body;
    NSString *text = nil;

    if ([body isKindOfClass:[NSString class]]) {
        text = (NSString *)body;
    } else if ([NSJSONSerialization isValidJSONObject:body]) {
        NSData *data = [NSJSONSerialization dataWithJSONObject:body options:0 error:nil];
        if (data) {
            text = [[NSString alloc] initWithData:data encoding:NSUTF8StringEncoding];
        }
    } else if (body) {
        text = [body description];
    }

    emit_event(nativeHost, NWV_EVENT_SCRIPT_MESSAGE, text ?: @"");
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
        host->dataStore = data_store_from_options(options);
        configuration.websiteDataStore = host->dataStore;
        host->userContentController = [[WKUserContentController alloc] init];
        host->scriptMessageHandler = [[NWVScriptMessageHandler alloc] init];
        host->scriptMessageHandler.host = host;
        [host->userContentController addScriptMessageHandler:host->scriptMessageHandler name:@"nativeWebView"];
        configuration.userContentController = host->userContentController;

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
        [host->userContentController removeScriptMessageHandlerForName:@"nativeWebView"];
        [host->webview removeFromSuperview];
        [host->container removeFromSuperview];
        host->webview = nil;
        host->dataStore = nil;
        host->userContentController = nil;
        host->container = nil;
        host->navigationDelegate = nil;
        host->uiDelegate = nil;
        host->scriptMessageHandler = nil;
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

NWV_EXPORT int nwv_add_document_script(void *handle, const void *script) {
    auto *host = static_cast<Host *>(handle);
    NSString *scriptString = as_string(script);
    if (!host || host->destroyed || !host->userContentController || !scriptString) {
        return 0;
    }

    run_on_main_sync(^{
        WKUserScript *userScript = [[WKUserScript alloc]
            initWithSource:scriptString
            injectionTime:WKUserScriptInjectionTimeAtDocumentStart
            forMainFrameOnly:NO
        ];
        [host->userContentController addUserScript:userScript];
    });
    return 1;
}

NWV_EXPORT int nwv_set_default_context_menu_enabled(void *handle, int enabled) {
    auto *host = static_cast<Host *>(handle);
    if (!host || host->destroyed) {
        return 0;
    }

    if (!enabled) {
        return nwv_add_document_script(handle, "document.addEventListener('contextmenu', function(e){ e.preventDefault(); }, true);");
    }
    return 1;
}

NWV_EXPORT int nwv_set_devtools_enabled(void *handle, int enabled) {
    return handle ? 1 : 0;
}

NWV_EXPORT int nwv_set_cookie(void *handle, const nwv_cookie *cookie) {
    auto *host = static_cast<Host *>(handle);
    if (!host || host->destroyed || !host->dataStore || !cookie) {
        return 0;
    }

    __block BOOL accepted = NO;
    run_on_main_sync(^{
        NSHTTPCookie *nativeCookie = cookie_from_native(cookie);
        if (!nativeCookie) {
            accepted = NO;
            return;
        }

        [host->dataStore.httpCookieStore setCookie:nativeCookie completionHandler:^{
        }];
        accepted = YES;
    });
    return accepted ? 1 : 0;
}

NWV_EXPORT int nwv_clear_cookies(void *handle) {
    auto *host = static_cast<Host *>(handle);
    if (!host || host->destroyed || !host->dataStore) {
        return 0;
    }

    run_on_main_sync(^{
        [host->dataStore.httpCookieStore getAllCookies:^(NSArray<NSHTTPCookie *> *cookies) {
            for (NSHTTPCookie *cookie in cookies) {
                [host->dataStore.httpCookieStore deleteCookie:cookie completionHandler:^{
                }];
            }
        }];
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
