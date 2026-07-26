from __future__ import annotations

import argparse
import tarfile
import zipfile
from email.parser import BytesParser
from pathlib import Path, PurePosixPath

from packaging.utils import parse_sdist_filename, parse_wheel_filename

PACKAGE = "sideview"
PYTHON_FILES = {
    "sideview/__init__.py",
    "sideview/_backend.py",
    "sideview/widget.py",
    "sideview/py.typed",
}
SDIST_FILES = {
    "CHANGELOG.md",
    "CONTRIBUTING.md",
    "LICENSE",
    "README.md",
    "SECURITY.md",
    "native/CMakeLists.txt",
    "native/linux/webview_host.cpp",
    "native/macos/webview_host.mm",
    "native/native_webview.h",
    "native/win32/webview_host.cpp",
    "pyproject.toml",
    "release-please-config.json",
    "scripts/validate_distribution.py",
    "scripts/validate_pull_request_title.py",
    "src/sideview/__init__.py",
    "src/sideview/_backend.py",
    "src/sideview/widget.py",
    "src/sideview/py.typed",
}
NATIVE_FILES = {
    "windows": "sideview/sideview_native.dll",
    "macos": "sideview/libsideview_native.dylib",
}


def _metadata_name_and_version(data: bytes) -> tuple[str, str]:
    metadata = BytesParser().parsebytes(data)
    return str(metadata["Name"]), str(metadata["Version"])


def _without_archive_root(name: str) -> str:
    parts = PurePosixPath(name).parts
    return PurePosixPath(*parts[1:]).as_posix() if len(parts) > 1 else name


def validate_wheel(path: Path) -> None:
    name, version, _build, tags = parse_wheel_filename(path.name)
    if name != PACKAGE:
        raise ValueError(f"{path.name}: expected distribution name {PACKAGE!r}, got {name!r}")
    if not all(tag.interpreter == "py3" and tag.abi == "none" for tag in tags):
        raise ValueError(f"{path.name}: expected a py3-none platform wheel")

    platforms = {tag.platform for tag in tags}
    if all(platform == "win_amd64" for platform in platforms):
        native_file = NATIVE_FILES["windows"]
    elif all(platform.endswith("_universal2") for platform in platforms):
        native_file = NATIVE_FILES["macos"]
    else:
        raise ValueError(f"{path.name}: unsupported wheel platform tags: {sorted(platforms)}")

    with zipfile.ZipFile(path) as archive:
        names = set(archive.namelist())
        missing = (PYTHON_FILES | {native_file}) - names
        if missing:
            raise ValueError(f"{path.name}: missing wheel files: {sorted(missing)}")
        if any("native_webview_widget" in name for name in names):
            raise ValueError(f"{path.name}: contains a legacy package or binary name")

        metadata_paths = [name for name in names if name.endswith(".dist-info/METADATA")]
        if len(metadata_paths) != 1:
            raise ValueError(f"{path.name}: expected exactly one METADATA file")
        metadata_name, metadata_version = _metadata_name_and_version(
            archive.read(metadata_paths[0])
        )

    if metadata_name != PACKAGE or metadata_version != str(version):
        raise ValueError(
            f"{path.name}: metadata mismatch ({metadata_name!r}, {metadata_version!r})"
        )


def validate_sdist(path: Path) -> None:
    name, version = parse_sdist_filename(path.name)
    if name != PACKAGE:
        raise ValueError(f"{path.name}: expected distribution name {PACKAGE!r}, got {name!r}")

    with tarfile.open(path, mode="r:gz") as archive:
        members = {member.name for member in archive.getmembers() if member.isfile()}
        names = {_without_archive_root(member) for member in members}
        missing = SDIST_FILES - names
        if missing:
            raise ValueError(f"{path.name}: missing sdist files: {sorted(missing)}")
        if any("native_webview_widget" in name for name in names):
            raise ValueError(f"{path.name}: contains a legacy package path")
        if any("__pycache__" in PurePosixPath(name).parts for name in names):
            raise ValueError(f"{path.name}: source distribution contains Python cache files")
        if any(name.endswith((".pyc", ".pyo")) for name in names):
            raise ValueError(f"{path.name}: source distribution contains compiled Python files")
        if any(name.endswith((".dll", ".dylib", ".so")) for name in names):
            raise ValueError(f"{path.name}: source distribution contains native binaries")

        metadata_paths = [name for name in members if name.endswith("/PKG-INFO")]
        if len(metadata_paths) != 1:
            raise ValueError(f"{path.name}: expected exactly one PKG-INFO file")
        extracted = archive.extractfile(metadata_paths[0])
        if extracted is None:
            raise ValueError(f"{path.name}: could not read PKG-INFO")
        metadata_name, metadata_version = _metadata_name_and_version(extracted.read())

    if metadata_name != PACKAGE or metadata_version != str(version):
        raise ValueError(
            f"{path.name}: metadata mismatch ({metadata_name!r}, {metadata_version!r})"
        )


def discover_distributions(path: Path) -> list[Path]:
    if path.is_file():
        return [path]
    if not path.is_dir():
        raise ValueError(f"Distribution path does not exist: {path}")
    return sorted((*path.glob("*.whl"), *path.glob("*.tar.gz")))


def main() -> int:
    parser = argparse.ArgumentParser(description="Validate SideView release distributions.")
    parser.add_argument("path", type=Path, help="Distribution file or directory.")
    args = parser.parse_args()

    distributions = discover_distributions(args.path)
    if not distributions:
        raise ValueError(f"No wheel or sdist found under {args.path}")

    for distribution in distributions:
        if distribution.suffix == ".whl":
            validate_wheel(distribution)
        elif distribution.name.endswith(".tar.gz"):
            validate_sdist(distribution)
        else:
            raise ValueError(f"Unsupported distribution: {distribution}")
        print(f"Validated {distribution}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
