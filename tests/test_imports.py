def test_public_imports():
    import native_webview_widget

    assert native_webview_widget.NativeWebView is not None
    assert native_webview_widget.NativeWebViewError is not None
    assert hasattr(native_webview_widget.NativeWebView, "capture_frame")
    assert hasattr(native_webview_widget.NativeWebView, "capture_region")
