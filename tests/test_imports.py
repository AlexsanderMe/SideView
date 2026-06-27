def test_public_imports():
    import native_webview_widget

    assert native_webview_widget.NativeWebView is not None
    assert native_webview_widget.NativeWebViewError is not None
    assert hasattr(native_webview_widget.NativeWebView, "capture_frame")
    assert hasattr(native_webview_widget.NativeWebView, "capture_region")
    assert hasattr(native_webview_widget.NativeWebView, "set_zoom_factor")
    assert hasattr(native_webview_widget.NativeWebView, "zoom_factor")


def test_zoom_factor_validation():
    import pytest

    from native_webview_widget import NativeWebView

    assert NativeWebView._validated_zoom_factor(1.25) == 1.25
    with pytest.raises(ValueError):
        NativeWebView._validated_zoom_factor(0.1)
    with pytest.raises(ValueError):
        NativeWebView._validated_zoom_factor(float("nan"))
