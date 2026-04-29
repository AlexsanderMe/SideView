from __future__ import annotations

import sys
from pathlib import Path
from urllib.parse import quote_plus, urlparse

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "src"))

from PySide6 import QtCore, QtGui, QtWidgets

from native_webview_widget import NativeWebView


HOME_URL = "https://www.google.com"
DOWNLOAD_HOST_WHITELIST: set[str] = set()


def normalize_location(text: str) -> str:
    value = text.strip()
    if not value:
        return HOME_URL

    parsed = urlparse(value)
    if parsed.scheme in {"http", "https"} and parsed.netloc:
        return value

    if " " not in value and "." in value:
        return f"https://{value}"

    return f"https://www.google.com/search?q={quote_plus(value)}"


def is_download_allowed(url: str) -> bool:
    parsed = urlparse(url)
    hostname = (parsed.hostname or "").lower()
    return hostname in DOWNLOAD_HOST_WHITELIST


class BrowserTab(QtWidgets.QWidget):
    titleChanged = QtCore.Signal(str)
    urlChanged = QtCore.Signal(str)
    navigationStateChanged = QtCore.Signal()
    newTabRequested = QtCore.Signal(str)
    downloadBlocked = QtCore.Signal(str)
    downloadAllowed = QtCore.Signal(str)

    def __init__(self, url: str = HOME_URL, parent: QtWidgets.QWidget | None = None) -> None:
        super().__init__(parent)

        self.current_url = url
        self.webview = NativeWebView(url=url)
        self.webview.set_download_policy(self._download_policy)
        self.webview.set_devtools_enabled(False)
        self.webview.install_context_menu_bridge()
        self.webview.titleChanged.connect(self.titleChanged)
        self.webview.navigationStarted.connect(self._navigation_started)
        self.webview.navigationFinished.connect(lambda url: self._navigation_finished(url))
        self.webview.navigationFailed.connect(lambda _: self.navigationStateChanged.emit())
        self.webview.newWindowRequested.connect(self._handle_new_window)
        self.webview.downloadRequested.connect(self._handle_download_requested)
        self.webview.contextMenuRequested.connect(self._show_context_menu)

        layout = QtWidgets.QVBoxLayout(self)
        layout.setContentsMargins(0, 0, 0, 0)
        layout.addWidget(self.webview)

    def navigate(self, text: str) -> None:
        self.webview.navigate(normalize_location(text))

    def dispose(self) -> None:
        self.webview.dispose()

    def _navigation_started(self, url: str) -> None:
        if url:
            self.current_url = url
            self.urlChanged.emit(url)
        self.navigationStateChanged.emit()

    def _navigation_finished(self, url: str) -> None:
        if url:
            self.current_url = url
            self.urlChanged.emit(url)
        self.navigationStateChanged.emit()

    def _handle_new_window(self, url: str) -> None:
        self.newTabRequested.emit(url or HOME_URL)

    def _handle_download_requested(self, url: str) -> None:
        if is_download_allowed(url):
            self.downloadAllowed.emit(url)
        else:
            self.downloadBlocked.emit(url)

    def _download_policy(self, url: str) -> bool:
        return is_download_allowed(url)

    def _show_context_menu(self, payload: dict) -> None:
        menu = QtWidgets.QMenu(self)
        href = str(payload.get("href") or "")
        src = str(payload.get("src") or "")

        if href:
            open_link = menu.addAction("Open link in new tab")
            open_link.triggered.connect(lambda: self.newTabRequested.emit(href))

        if src:
            copy_media = menu.addAction("Copy media URL")
            copy_media.triggered.connect(lambda: QtGui.QGuiApplication.clipboard().setText(src))

        copy_page = menu.addAction("Copy page URL")
        copy_page.triggered.connect(lambda: QtGui.QGuiApplication.clipboard().setText(self.webview_url_hint()))

        menu.addSeparator()
        reload_action = menu.addAction("Reload")
        reload_action.triggered.connect(self.webview.reload)

        x = int(payload.get("x") or 0)
        y = int(payload.get("y") or 0)
        menu.exec(self.webview.mapToGlobal(QtCore.QPoint(x, y)))

    def webview_url_hint(self) -> str:
        return self.current_url


class BrowserWindow(QtWidgets.QMainWindow):
    def __init__(self) -> None:
        super().__init__()
        self.setWindowTitle("Native WebView Browser")
        self.resize(1280, 820)

        self.back_button = QtWidgets.QToolButton()
        self.back_button.setText("<")
        self.back_button.setToolTip("Back")
        self.back_button.clicked.connect(lambda: self.current_tab().webview.go_back())

        self.forward_button = QtWidgets.QToolButton()
        self.forward_button.setText(">")
        self.forward_button.setToolTip("Forward")
        self.forward_button.clicked.connect(lambda: self.current_tab().webview.go_forward())

        self.reload_button = QtWidgets.QToolButton()
        self.reload_button.setText("R")
        self.reload_button.setToolTip("Reload")
        self.reload_button.clicked.connect(lambda: self.current_tab().webview.reload())

        self.new_tab_button = QtWidgets.QToolButton()
        self.new_tab_button.setText("+")
        self.new_tab_button.setToolTip("New tab")
        self.new_tab_button.clicked.connect(lambda: self.add_tab(HOME_URL))

        self.close_tab_button = QtWidgets.QToolButton()
        self.close_tab_button.setText("x")
        self.close_tab_button.setToolTip("Close tab")
        self.close_tab_button.clicked.connect(lambda: self.close_tab(self.tabs.currentIndex()))

        self.url_edit = QtWidgets.QLineEdit()
        self.url_edit.returnPressed.connect(self._navigate_from_bar)

        toolbar = QtWidgets.QToolBar()
        toolbar.setMovable(False)
        toolbar.addWidget(self.back_button)
        toolbar.addWidget(self.forward_button)
        toolbar.addWidget(self.reload_button)
        toolbar.addWidget(self.new_tab_button)
        toolbar.addWidget(self.close_tab_button)
        toolbar.addWidget(self.url_edit)
        self.addToolBar(toolbar)

        self.tabs = QtWidgets.QTabWidget()
        self.tabs.setDocumentMode(True)
        self.tabs.setTabsClosable(True)
        self.tabs.currentChanged.connect(self._sync_toolbar)
        self.tabs.tabCloseRequested.connect(self.close_tab)
        self.setCentralWidget(self.tabs)

        self.add_tab(HOME_URL)

    def current_tab(self) -> BrowserTab:
        tab = self.tabs.currentWidget()
        if not isinstance(tab, BrowserTab):
            raise RuntimeError("No active browser tab.")
        return tab

    def add_tab(self, url: str = HOME_URL) -> BrowserTab:
        tab = BrowserTab(url)
        index = self.tabs.addTab(tab, "New tab")
        self.tabs.setCurrentIndex(index)

        tab.titleChanged.connect(lambda title, current_tab=tab: self._set_tab_title(current_tab, title))
        tab.urlChanged.connect(lambda current_url, current_tab=tab: self._set_current_url(current_tab, current_url))
        tab.navigationStateChanged.connect(self._sync_toolbar)
        tab.newTabRequested.connect(self.add_tab)
        tab.downloadBlocked.connect(self._download_blocked)
        tab.downloadAllowed.connect(self._download_allowed)
        return tab

    def close_tab(self, index: int) -> None:
        if self.tabs.count() == 1:
            self.add_tab(HOME_URL)

        widget = self.tabs.widget(index)
        self.tabs.removeTab(index)

        if isinstance(widget, BrowserTab):
            widget.dispose()
            widget.deleteLater()

        self._sync_toolbar()

    def _navigate_from_bar(self) -> None:
        self.current_tab().navigate(self.url_edit.text())

    def _set_tab_title(self, tab: BrowserTab, title: str) -> None:
        index = self.tabs.indexOf(tab)
        if index >= 0:
            self.tabs.setTabText(index, title[:32] if title else "New tab")
        if tab is self.tabs.currentWidget():
            self.setWindowTitle(title or "Native WebView Browser")

    def _set_current_url(self, tab: BrowserTab, url: str) -> None:
        if tab is self.tabs.currentWidget():
            self.url_edit.setText(url)

    def _sync_toolbar(self) -> None:
        if self.tabs.count() == 0:
            return

        tab = self.current_tab()
        self.back_button.setEnabled(tab.webview.can_go_back())
        self.forward_button.setEnabled(tab.webview.can_go_forward())
        self.close_tab_button.setEnabled(self.tabs.count() > 1)

    def _download_blocked(self, url: str) -> None:
        self.statusBar().showMessage(f"Download blocked by policy: {url}", 8000)

    def _download_allowed(self, url: str) -> None:
        self.statusBar().showMessage(f"Download allowed by policy: {url}", 8000)


def main() -> int:
    app = QtWidgets.QApplication(sys.argv)
    window = BrowserWindow()
    window.show()
    return app.exec()


if __name__ == "__main__":
    raise SystemExit(main())
