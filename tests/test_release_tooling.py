from __future__ import annotations

from pathlib import Path

import pytest

from scripts.validate_pull_request_title import load_allowed_types, validate_title

ROOT = Path(__file__).resolve().parents[1]


@pytest.mark.parametrize(
    "title",
    [
        "feat: add navigation API",
        "fix(widget): dispose native handle",
        "feat!: rename the public package",
        "build(deps): update scikit-build-core",
    ],
)
def test_conventional_pull_request_titles_are_accepted(title):
    validate_title(title, load_allowed_types(ROOT / "release-please-config.json"))


@pytest.mark.parametrize(
    "title",
    [
        "Add navigation API",
        "feature: add navigation API",
        "feat:add navigation API",
        "feat(): add navigation API",
        "feat: ",
    ],
)
def test_invalid_pull_request_titles_are_rejected(title):
    with pytest.raises(ValueError):
        validate_title(title, load_allowed_types(ROOT / "release-please-config.json"))
