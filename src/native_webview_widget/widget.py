from __future__ import annotations

from typing import Callable
from pathlib import Path
from uuid import NAMESPACE_URL, uuid5

from PySide6 import QtCore, QtGui, QtWidgets

from ._backend import NativeBackend, NativeCookie, NativeOptions, NativeWebViewError


DownloadPolicy = Callable[[str], bool]


class _EventBridge(QtCore.QObject):
    received = QtCore.Signal(int, str)


class NativeWebView(QtWidgets.QWidget):
    ready = QtCore.Signal()
    navigationStarted = QtCore.Signal(str)
    navigationFinished = QtCore.Signal(str)
    navigationFailed = QtCore.Signal(str)
    titleChanged = QtCore.Signal(str)
    downloadRequested = QtCore.Signal(str)
    newWindowRequested = QtCore.Signal(str)

    def __init__(
        self,
        parent: QtWidgets.QWidget | None = None,
        *,
        url: str | None = None,
        html: str | None = None,
        session_id: str | None = None,
        session_data_root: str | Path | None = None,
        user_data_folder: str | None = None,
        runtime_path: str | None = None,
        transparent: bool = False,
    ) -> None:
        super().__init__(parent)
        self.setAttribute(QtCore.Qt.WidgetAttribute.WA_NativeWindow, True)
        self.setAttribute(QtCore.Qt.WidgetAttribute.WA_DontCreateNativeAncestors, False)
        self.setFocusPolicy(QtCore.Qt.FocusPolicy.StrongFocus)

        self._backend = NativeBackend()
        self._handle = 0
        self._created = False
        self._native_ready = False
        self._pending_url = url
        self._pending_html = html
        self._pending_base_url: str | None = None
        self._pending_cookies: list[NativeCookie] = []
        self._pending_clear_cookies = False
        self._download_policy: DownloadPolicy | None = None
        resolved_session_id = session_id or "default"
        self._options = NativeOptions(
            user_data_folder=user_data_folder
            or self._session_data_folder(resolved_session_id, session_data_root),
            runtime_path=runtime_path,
            session_id=str(uuid5(NAMESPACE_URL, f"native-webview-widget:{resolved_session_id}")),
            transparent=transparent,
        )
        self._bridge = _EventBridge(self)
        self._bridge.received.connect(self._handle_native_event, QtCore.Qt.ConnectionType.QueuedConnection)

    def navigate(self, url: str) -> None:
        if not self._created or not self._native_ready:
            self._pending_url = url
            self._pending_html = None
            return
        if not self._backend.navigate(self._handle, url):
            raise NativeWebViewError(f"Failed to navigate to {url!r}.")

    def set_html(self, html: str, base_url: str | None = None) -> None:
        if not self._created or not self._native_ready:
            self._pending_html = html
            self._pending_url = None
            self._pending_base_url = base_url
            return
        if not self._backend.set_html(self._handle, html, base_url):
            raise NativeWebViewError("Failed to set HTML content.")

    def reload(self) -> None:
        self._require_created()
        self._backend.reload(self._handle)

    def go_back(self) -> None:
        self._require_created()
        self._backend.go_back(self._handle)

    def go_forward(self) -> None:
        self._require_created()
        self._backend.go_forward(self._handle)

    def eval_js(self, script: str) -> None:
        self._require_created()
        if not self._backend.eval_js(self._handle, script):
            raise NativeWebViewError("Failed to evaluate JavaScript.")

    def set_download_policy(self, callback: DownloadPolicy | None) -> None:
        """Set a synchronous whitelist callback for native downloads.

        Return True to allow the browser engine to continue the download.
        Return False to cancel it so the application can handle the URL itself.
        """
        self._download_policy = callback
        if self._created:
            self._backend.set_policy_callback(self._handle, self._handle_policy_request)

    def set_cookie(
        self,
        *,
        name: str,
        value: str,
        domain: str,
        path: str = "/",
        expires: float = 0,
        secure: bool = False,
        http_only: bool = False,
        same_site: str = "lax",
    ) -> None:
        cookie = NativeCookie(
            name=name,
            value=value,
            domain=domain,
            path=path,
            expires=expires,
            secure=secure,
            http_only=http_only,
            same_site=same_site,
        )
        if not self._created or not self._native_ready:
            self._pending_cookies.append(cookie)
            return
        if not self._backend.set_cookie(self._handle, cookie):
            raise NativeWebViewError(f"Failed to set cookie {name!r} for {domain!r}.")

    def clear_cookies(self) -> None:
        self._pending_cookies.clear()
        if not self._created or not self._native_ready:
            self._pending_clear_cookies = True
            return
        self._require_created()
        if not self._backend.clear_cookies(self._handle):
            raise NativeWebViewError("Failed to clear cookies.")

    def can_go_back(self) -> bool:
        return self._created and self._backend.can_go_back(self._handle)

    def can_go_forward(self) -> bool:
        return self._created and self._backend.can_go_forward(self._handle)

    def showEvent(self, event: QtGui.QShowEvent) -> None:  # type: ignore[name-defined]
        super().showEvent(event)
        self._ensure_created()

    def resizeEvent(self, event: QtGui.QResizeEvent) -> None:  # type: ignore[name-defined]
        super().resizeEvent(event)
        if self._created:
            size = event.size()
            self._backend.resize(self._handle, size.width(), size.height())

    def closeEvent(self, event: QtGui.QCloseEvent) -> None:  # type: ignore[name-defined]
        self.dispose()
        super().closeEvent(event)

    def dispose(self) -> None:
        if self._handle:
            self._backend.destroy(self._handle)
        self._handle = 0
        self._created = False
        self._native_ready = False

    def _ensure_created(self) -> None:
        if self._created:
            return

        parent_handle = int(self.winId())
        self._handle = self._backend.create(parent_handle, self._options, self._emit_native_event)
        self._created = True
        self._backend.set_policy_callback(self._handle, self._handle_policy_request)
        self._backend.resize(self._handle, self.width(), self.height())

    def _require_created(self) -> None:
        if not self._created:
            self._ensure_created()

    def _emit_native_event(self, event_type: int, message: str) -> None:
        self._bridge.received.emit(event_type, message)

    def _handle_native_event(self, event_type: int, message: str) -> None:
        if event_type == NativeBackend.EVENT_READY:
            self._native_ready = True
            self.ready.emit()
            self._flush_pending_clear_cookies()
            self._flush_pending_cookies()
            self._flush_pending_load()
        elif event_type == NativeBackend.EVENT_NAVIGATION_STARTED:
            self.navigationStarted.emit(message)
        elif event_type == NativeBackend.EVENT_NAVIGATION_FINISHED:
            self.navigationFinished.emit(message)
        elif event_type == NativeBackend.EVENT_NAVIGATION_FAILED:
            self.navigationFailed.emit(message)
        elif event_type == NativeBackend.EVENT_TITLE_CHANGED:
            self.titleChanged.emit(message)
        elif event_type == NativeBackend.EVENT_DOWNLOAD_REQUESTED:
            self.downloadRequested.emit(message)
        elif event_type == NativeBackend.EVENT_NEW_WINDOW_REQUESTED:
            self.newWindowRequested.emit(message)

    def _handle_policy_request(self, event_type: int, message: str) -> bool:
        if event_type == NativeBackend.EVENT_DOWNLOAD_REQUESTED:
            return bool(self._download_policy and self._download_policy(message))
        return False

    def _flush_pending_load(self) -> None:
        if self._pending_html is not None:
            html = self._pending_html
            base_url = self._pending_base_url
            self._pending_html = None
            self._pending_base_url = None
            if not self._backend.set_html(self._handle, html, base_url):
                self.navigationFailed.emit("Failed to set pending HTML content.")
        elif self._pending_url is not None:
            url = self._pending_url
            self._pending_url = None
            if not self._backend.navigate(self._handle, url):
                self.navigationFailed.emit(f"Failed to navigate to {url!r}.")

    def _flush_pending_cookies(self) -> None:
        pending = self._pending_cookies
        self._pending_cookies = []
        for cookie in pending:
            if not self._backend.set_cookie(self._handle, cookie):
                self.navigationFailed.emit(f"Failed to set pending cookie {cookie.name!r}.")

    def _flush_pending_clear_cookies(self) -> None:
        if not self._pending_clear_cookies:
            return
        self._pending_clear_cookies = False
        if not self._backend.clear_cookies(self._handle):
            self.navigationFailed.emit("Failed to clear pending cookies.")

    @staticmethod
    def _session_data_folder(session_id: str, session_data_root: str | Path | None) -> str:
        root = Path(session_data_root) if session_data_root else Path.home() / ".native-webview-widget"
        safe_session = "".join(ch if ch.isalnum() or ch in ("-", "_", ".") else "_" for ch in session_id)
        return str(root / "sessions" / safe_session)
