# native-webview-widget

Native WebView widget for PySide6 applications that need system browser media support without using `QWebEngineView`.

The library exposes a normal `QWidget` subclass in Python and delegates rendering to:

- Windows: Microsoft Edge WebView2, hosted inside the widget's native `HWND`.
- macOS: `WKWebView`, hosted inside the widget's native `NSView`.

This is intentionally not a full browser. It is a small embedded view for in-app navigation, OAuth/help pages, web dashboards, and video playback using codecs available through the operating system browser stack.

It also gives the host application control over browser-adjacent behavior that should not be left to the embedded engine by default:

- Downloads are intercepted and blocked unless your Python whitelist callback allows them.
- Links that request a new window are intercepted and surfaced as a signal, so the app can open an internal tab instead of letting the engine spawn a separate native window.

## Why not fork pywebview?

`pywebview` is a window abstraction. Its backends are designed to own top-level windows and integrate with their own GUI loops. This library uses the opposite contract: PySide6 owns the application, and the native webview is a child of a Qt widget. That keeps focus, resizing, lifecycle, and application shutdown under Qt's control.

## Current status

This repository contains the production-oriented skeleton and native backend implementation shape:

- Stable Python `NativeWebView` API.
- Native library loader with clear platform errors.
- Windows C++ backend using WebView2.
- macOS Objective-C++ backend using WKWebView.
- CMake build files for native libraries.
- A tabbed PySide6 browser example with back, forward, reload, new tab, close tab, URL/search bar, download policy hooks, and new-window routing.

The native libraries must be compiled for each target platform and placed beside the Python package or pointed to with `NATIVE_WEBVIEW_WIDGET_LIB`.

## Python usage

```python
from PySide6.QtWidgets import QApplication, QMainWindow
from native_webview_widget import NativeWebView

app = QApplication([])

window = QMainWindow()
view = NativeWebView()
window.setCentralWidget(view)
window.resize(1200, 800)
window.show()

view.navigate("https://www.youtube.com")

app.exec()
```

## Downloads and new windows

Downloads are denied by default. Register a callback with `set_download_policy()` to allow only the URLs your app trusts. The callback should return `True` to let the native webview continue the download, or `False` to cancel it so your app can handle the URL itself.

```python
from urllib.parse import urlparse

ALLOWED_DOWNLOAD_HOSTS = {"example.com", "cdn.example.com"}

def allow_download(url: str) -> bool:
    return (urlparse(url).hostname or "").lower() in ALLOWED_DOWNLOAD_HOSTS

view.set_download_policy(allow_download)
view.downloadRequested.connect(lambda url: print("Download requested:", url))
```

Links that try to open a new tab or popup are not opened as separate native windows. Instead, the widget emits `newWindowRequested(url)`.

```python
view.newWindowRequested.connect(lambda url: open_internal_tab(url))
```

## Sessions and cookies

Use `session_id` to isolate browser state per application profile. Views created with the same `session_id` share cookies/cache; views created with different IDs get separate sessions.

```python
profile_id = "default"  # for example, your active app profile id

view = NativeWebView(
    session_id=f"solin_session_{profile_id}",
    session_data_root="path/to/user/data/webview",
)
```

On Windows, the session maps to a WebView2 `userDataFolder`. On macOS, the session maps to a persistent `WKWebsiteDataStore` identifier when the platform supports it. You can still pass `user_data_folder` directly if you need full control over the Windows WebView2 storage path.

Cookies can be set before or after the native view is ready. If the view is not ready yet, the cookie operation is queued and applied before the first pending navigation.

```python
view.set_cookie(
    name="session",
    value="abc123",
    domain=".example.com",
    path="/",
    secure=True,
    http_only=True,
    same_site="lax",
)
```

To clear the current session cookies:

```python
view.clear_cookies()
view.reload()
```

## Building the native backend

You can build locally, or use the manual GitHub Actions workflow in `.github/workflows/build-native.yml`.

### GitHub Actions

The `Build native libraries` workflow is manual by design. In GitHub, open `Actions`, choose `Build native libraries`, then click `Run workflow`.

It builds:

- `native-webview-widget-windows-x64.zip` on `windows-2025`.
- `native-webview-widget-macos-universal.tar.gz` on `macos-15-intel`.

The Windows job downloads the pinned `Microsoft.Web.WebView2` NuGet package version selected in the workflow inputs. The macOS job builds a universal `x86_64;arm64` dylib and ad-hoc signs it. Both artifacts include `SHA256SUMS.txt`.

### Windows

Install the WebView2 Runtime and the WebView2 SDK headers/libraries. The CMake project expects `WEBVIEW2_SDK_DIR` to point at the SDK root that contains `build/native/include/WebView2.h` and the matching loader library.

```powershell
.\scripts\build_windows.ps1 -WebView2SdkDir C:\path\to\Microsoft.Web.WebView2
```

The script uses `vswhere` to locate Visual Studio Build Tools, configures MSVC through `vcvars64.bat`, builds the DLL, and copies it next to the Python package. If you build manually, copy `native_webview_widget.dll` next to `src/native_webview_widget/` or set:

```powershell
$env:NATIVE_WEBVIEW_WIDGET_LIB="C:\path\to\native_webview_widget.dll"
```

### macOS

```bash
cmake -S native -B build/native -DCMAKE_BUILD_TYPE=Release
cmake --build build/native
```

Copy `libnative_webview_widget.dylib` next to `src/native_webview_widget/` or set:

```bash
export NATIVE_WEBVIEW_WIDGET_LIB=/path/to/libnative_webview_widget.dylib
```

## Design constraints

- The native webview is a real native child view. It draws outside Qt's paint engine, so it should not be overlapped by translucent Qt widgets.
- Keep one webview per visible widget unless the product truly needs more; native browser views are heavier than normal widgets.
- Navigation policy, custom context menus, downloads, permission prompts, and devtools should be added deliberately as product requirements, not by default.
- Linux support is not implemented in the initial release. A WebKitGTK-based backend can be added in the future using the same C ABI.

## Example

Run:

```bash
python examples/simple_browser.py
```

The example opens Google in a new tab by default. The address bar goes directly to valid URLs and searches Google when the input looks like plain text. Its download whitelist is intentionally empty:

```python
DOWNLOAD_HOST_WHITELIST: set[str] = set()
```

Replace `is_download_allowed(url)` in `examples/simple_browser.py` with your application policy when you decide how downloads should be handled.
