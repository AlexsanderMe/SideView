from __future__ import annotations

import re
from pathlib import Path

from PySide6 import QtCore, QtWidgets


class _FakeBackend:
    uses_foreign_window = False

    def __init__(self) -> None:
        self.resizes: list[tuple[int, int, int]] = []

    def resize(self, handle: int, width: int, height: int) -> None:
        self.resizes.append((handle, width, height))

    def stop_frame_stream(self, _handle: int) -> None:
        pass

    def destroy(self, _handle: int) -> None:
        pass


def _created_view(monkeypatch) -> tuple[QtWidgets.QApplication, object, _FakeBackend]:
    import sideview.widget as widget_module

    backend = _FakeBackend()
    monkeypatch.setenv("QT_QPA_PLATFORM", "offscreen")
    monkeypatch.setattr(widget_module, "NativeBackend", lambda: backend)
    app = QtWidgets.QApplication.instance() or QtWidgets.QApplication([])
    view = widget_module.NativeWebView()
    view._handle = 17
    view._created = True
    return app, view, backend


def test_resize_synchronizes_native_bounds(monkeypatch):
    app, view, backend = _created_view(monkeypatch)
    view.show()
    app.processEvents()
    backend.resizes.clear()

    view.resize(640, 360)
    app.processEvents()

    assert backend.resizes[-1] == (17, 640, 360)
    view.dispose()


def test_device_pixel_ratio_change_schedules_native_bounds_sync(monkeypatch):
    import sideview.widget as widget_module

    if widget_module._DEVICE_PIXEL_RATIO_CHANGE is None:
        return

    app, view, backend = _created_view(monkeypatch)
    view.resize(800, 450)
    app.processEvents()
    backend.resizes.clear()

    event = QtCore.QEvent(widget_module._DEVICE_PIXEL_RATIO_CHANGE)
    QtCore.QCoreApplication.sendEvent(view, event)
    app.processEvents()

    assert backend.resizes == [(17, 800, 450)]
    view.dispose()


def test_windows_resize_uses_native_parent_client_rect():
    source = (Path(__file__).parents[1] / "native" / "win32" / "webview_host.cpp").read_text(
        encoding="utf-8"
    )
    resize_body = re.search(
        r"NWV_EXPORT void nwv_resize\([^)]*\) \{(?P<body>.*?)\n\}",
        source,
        re.DOTALL,
    )

    assert resize_body is not None
    assert "sync_bounds_to_parent(host.get());" in resize_body.group("body")
    assert "SetWindowPos" not in resize_body.group("body")
    assert "GetClientRect(host->parent" in source
