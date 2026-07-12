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
    assert "NWV_EVENT_ZOOM_FACTOR_REQUESTED" in source
    assert "webkit_web_view_set_zoom_level(host->webview" in source


def test_native_zoom_request_is_observable_and_applied(monkeypatch):
    import native_webview_widget.widget as widget_module
    from PySide6 import QtWidgets

    applied: list[float] = []

    class FakeBackend:
        EVENT_READY = 1
        EVENT_NAVIGATION_STARTED = 2
        EVENT_NAVIGATION_FINISHED = 3
        EVENT_NAVIGATION_FAILED = 4
        EVENT_TITLE_CHANGED = 5
        EVENT_DOWNLOAD_REQUESTED = 6
        EVENT_NEW_WINDOW_REQUESTED = 7
        EVENT_SCRIPT_MESSAGE = 8
        EVENT_ZOOM_FACTOR_CHANGED = 9
        EVENT_ZOOM_FACTOR_REQUESTED = 10
        uses_foreign_window = False
        zoom_supported = True

        def set_zoom_factor(self, _handle, factor):
            applied.append(factor)
            return True

        def get_zoom_factor(self, _handle):
            return applied[-1]

        def stop_frame_stream(self, _handle):
            return True

        def destroy(self, _handle):
            return None

    monkeypatch.setenv("QT_QPA_PLATFORM", "offscreen")
    monkeypatch.setattr(widget_module, "NativeBackend", FakeBackend)
    app = QtWidgets.QApplication.instance() or QtWidgets.QApplication([])
    view = widget_module.NativeWebView()
    view._handle = 7
    view._created = True
    view._native_ready = True
    requested: list[tuple[float, float]] = []
    view.zoomFactorRequested.connect(
        lambda factor: requested.append((factor, view.zoom_factor()))
    )

    view._handle_native_event(FakeBackend.EVENT_ZOOM_FACTOR_REQUESTED, "1.25")

    assert requested == [(1.25, 1.25)]
    assert applied == [1.25]
    assert view.zoom_factor() == 1.25
    view.dispose()
    app.processEvents()


def test_native_surface_visibility_does_not_dispose_backend(monkeypatch):
    import native_webview_widget.widget as widget_module
    from PySide6 import QtWidgets

    class FakeBackend:
        uses_foreign_window = False

    monkeypatch.setenv("QT_QPA_PLATFORM", "offscreen")
    monkeypatch.setattr(widget_module, "NativeBackend", FakeBackend)
    app = QtWidgets.QApplication.instance() or QtWidgets.QApplication([])
    view = widget_module.NativeWebView()
    container = QtWidgets.QWidget(view)
    container.show()
    view._native_container = container

    view.set_native_surface_visible(False)
    assert container.isHidden()
    assert not view._disposed

    view.set_native_surface_visible(True)
    assert not container.isHidden()
    assert not view._disposed
    view.dispose()
    app.processEvents()
