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


def test_linux_library_resolution(monkeypatch, tmp_path):
    import native_webview_widget._backend as backend_module

    monkeypatch.delenv("NATIVE_WEBVIEW_WIDGET_LIB", raising=False)
    library = tmp_path / "libnative_webview_widget.so"
    library.touch()
    monkeypatch.setattr(backend_module, "__file__", str(tmp_path / "_backend.py"))

    backend = backend_module.NativeBackend.__new__(backend_module.NativeBackend)
    backend._system = "Linux"

    assert backend._resolve_library() == library


def test_dispose_is_terminal_and_idempotent(monkeypatch):
    import native_webview_widget.widget as widget_module
    import pytest
    from PySide6 import QtWidgets

    calls: list[tuple[str, int]] = []

    class FakeBackend:
        uses_foreign_window = False

        def stop_frame_stream(self, handle):
            calls.append(("stop", handle))

        def destroy(self, handle):
            calls.append(("destroy", handle))

    monkeypatch.setenv("QT_QPA_PLATFORM", "offscreen")
    monkeypatch.setattr(widget_module, "NativeBackend", FakeBackend)
    app = QtWidgets.QApplication.instance() or QtWidgets.QApplication([])
    view = widget_module.NativeWebView()
    view._handle = 7
    view._created = True

    view.dispose()
    view.dispose()

    assert calls == [("stop", 7), ("destroy", 7)]
    with pytest.raises(widget_module.NativeWebViewError, match="disposed"):
        view._ensure_created()
    app.processEvents()


def test_parent_destruction_disposes_native_handle(monkeypatch):
    import native_webview_widget.widget as widget_module
    import shiboken6
    from PySide6 import QtWidgets

    destroyed_handles: list[int] = []

    class FakeBackend:
        uses_foreign_window = False

        def stop_frame_stream(self, _handle):
            pass

        def destroy(self, handle):
            destroyed_handles.append(handle)

    monkeypatch.setenv("QT_QPA_PLATFORM", "offscreen")
    monkeypatch.setattr(widget_module, "NativeBackend", FakeBackend)
    app = QtWidgets.QApplication.instance() or QtWidgets.QApplication([])
    parent = QtWidgets.QWidget()
    view = widget_module.NativeWebView(parent)
    view._handle = 11
    view._created = True

    shiboken6.delete(parent)

    assert destroyed_handles == [11]
    app.processEvents()


def test_destroy_foreign_window_tolerates_qt_owned_container_teardown(monkeypatch):
    import native_webview_widget.widget as widget_module
    import shiboken6
    from PySide6 import QtWidgets

    monkeypatch.setenv("QT_QPA_PLATFORM", "offscreen")
    app = QtWidgets.QApplication.instance() or QtWidgets.QApplication([])
    container = QtWidgets.QWidget()
    shiboken6.delete(container)

    widget_module.NativeWebView._destroy_foreign_window(container, None)
    app.processEvents()


def test_linux_backend_handles_native_zoom_inputs():
    from pathlib import Path

    source = (
        Path(__file__).parents[1] / "native" / "linux" / "webview_host.cpp"
    ).read_text(encoding="utf-8")

    assert '"scroll-event"' in source
    assert '"key-press-event"' in source
    assert "GDK_CONTROL_MASK" in source
    assert "GDK_KEY_KP_Add" in source
    assert "GDK_KEY_KP_Subtract" in source
    assert "GDK_KEY_KP_0" in source
