from importlib.metadata import PackageNotFoundError, version

from .widget import NativeWebView, NativeWebViewError

try:
    __version__ = version("sideview")
except PackageNotFoundError:
    __version__ = "0+unknown"

__all__ = ["NativeWebView", "NativeWebViewError", "__version__"]
