from __future__ import annotations

import ctypes
import json
import sys
import tempfile
from ctypes import wintypes

from PySide6 import QtCore, QtWidgets

from sideview import NativeWebView


class Rect(ctypes.Structure):
    _fields_ = (
        ("left", wintypes.LONG),
        ("top", wintypes.LONG),
        ("right", wintypes.LONG),
        ("bottom", wintypes.LONG),
    )


def _client_size(user32: ctypes.WinDLL, hwnd: int) -> tuple[int, int]:
    rect = Rect()
    if not user32.GetClientRect(hwnd, ctypes.byref(rect)):
        raise ctypes.WinError(ctypes.get_last_error())
    return rect.right - rect.left, rect.bottom - rect.top


def _process_events_until(
    app: QtWidgets.QApplication,
    predicate,
    *,
    timeout_ms: int,
) -> bool:
    deadline = QtCore.QDeadlineTimer(timeout_ms)
    while not deadline.hasExpired():
        app.processEvents(QtCore.QEventLoop.ProcessEventsFlag.AllEvents, 20)
        if predicate():
            return True
        QtCore.QThread.msleep(10)
    return predicate()


def main() -> int:
    if sys.platform != "win32":
        raise RuntimeError("Native bounds validation only supports Windows.")

    user32 = ctypes.WinDLL("user32", use_last_error=True)
    user32.GetClientRect.argtypes = [wintypes.HWND, ctypes.POINTER(Rect)]
    user32.GetClientRect.restype = wintypes.BOOL
    user32.FindWindowExW.argtypes = (
        wintypes.HWND,
        wintypes.HWND,
        wintypes.LPCWSTR,
        wintypes.LPCWSTR,
    )
    user32.FindWindowExW.restype = wintypes.HWND

    app = QtWidgets.QApplication([])
    data_root = tempfile.TemporaryDirectory(
        prefix="sideview-native-bounds-",
        ignore_cleanup_errors=True,
    )
    window = QtWidgets.QMainWindow()
    view = NativeWebView(session_data_root=data_root.name)
    window.setCentralWidget(view)
    window.resize(800, 600)

    ready = False

    def mark_ready() -> None:
        nonlocal ready
        ready = True

    view.ready.connect(mark_ready)
    window.show()

    try:
        if not _process_events_until(app, lambda: ready, timeout_ms=15_000):
            raise RuntimeError("Native WebView did not become ready.")

        parent_hwnd = int(view.winId())
        child_hwnd = int(user32.FindWindowExW(parent_hwnd, 0, "Static", None))
        if not child_hwnd:
            raise RuntimeError("SideView native child HWND was not created.")

        window.resize(960, 640)
        if not _process_events_until(
            app,
            lambda: _client_size(user32, child_hwnd) == _client_size(user32, parent_hwnd),
            timeout_ms=5_000,
        ):
            parent_size = _client_size(user32, parent_hwnd)
            child_size = _client_size(user32, child_hwnd)
            raise RuntimeError(
                "Native child bounds do not match the Qt host: "
                f"parent={parent_size}, child={child_size}."
            )

        result = {
            "device_pixel_ratio": view.devicePixelRatioF(),
            "logical_size": [view.width(), view.height()],
            "native_size": list(_client_size(user32, parent_hwnd)),
        }
        print(json.dumps(result, sort_keys=True))
        return 0
    finally:
        view.dispose()
        window.close()
        app.processEvents()
        data_root.cleanup()


if __name__ == "__main__":
    raise SystemExit(main())
