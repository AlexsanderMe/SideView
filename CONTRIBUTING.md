# Contributing to SideView

SideView combines Python, PySide6, and a small native backend. Changes should
remain focused, testable, and consistent across the supported platforms.

## Development workflow

Create a branch from the latest `master`. Use short English branch names such as
`feat/linux-shortcuts`, `fix/widget-lifecycle`, or `docs/installation`.

Do not commit directly to `master`. Open a pull request, wait for all required
checks, and prefer squash merging so the pull request title becomes the
Conventional Commit consumed by Release Please.

## Local validation

Install the Python development tools with the Python installation you use for
the project:

```bash
python -m pip install "PySide6>=6.5" "pytest>=8,<10" "ruff>=0.12,<1" "build>=1.3,<2" "twine>=6,<7"
```

Run the same code-quality checks used by CI:

```bash
python -m ruff check .
python -m ruff format --check .
python -m pytest
```

Building a wheel also requires the native toolchain and platform dependencies
described in the README. Building an sdist does not compile the native backend:

```bash
python -m build --sdist
python -m twine check --strict dist/*
python scripts/validate_distribution.py dist
```

## Pull requests

Keep the pull request title in Conventional Commit format:

- `fix: ...` produces a patch release.
- `feat: ...` produces a minor release.
- `feat!: ...`, `fix!: ...`, or a `BREAKING CHANGE:` footer marks a breaking
  release. While SideView is below `1.0.0`, breaking changes bump the minor
  version; after `1.0.0`, they bump the major version.
- `docs:`, `test:`, `build:`, `ci:`, `refactor:`, and `chore:` describe their
  respective maintenance work. They do not force a version bump by themselves.

CI rejects pull request titles that do not follow this format. When squash
merging is used, the title becomes the commit consumed by Release Please.

Document user-visible behavior and public API changes. Tests should cover the
root cause of a bug instead of only reproducing its symptom.

## Releases

Release Please runs after changes reach `master`. It creates or updates a
Release PR containing the next version and generated changelog. Normal merges
therefore do not publish immediately.

To publish a release:

1. Review the Release PR version and changelog.
2. Merge the Release PR.
3. Wait for all source and wheel builds to pass.
4. Approve the `testpypi` deployment and inspect the TestPyPI project.
5. Approve the `pypi` deployment.

The release workflow uploads the exact same validated artifacts to TestPyPI,
PyPI, and the GitHub Release. Authentication uses Trusted Publishing; no PyPI
API token is stored in the repository.

Do not edit `CHANGELOG.md` or the project version for routine releases. Release
Please owns both. A release-specific correction should be made in the Release PR
before it is merged.
