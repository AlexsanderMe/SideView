from __future__ import annotations

import argparse
import json
import re
from pathlib import Path

TITLE_PATTERN = re.compile(
    r"^(?P<type>[a-z]+)"
    r"(?:\([a-z0-9][a-z0-9._/-]*\))?"
    r"!?: "
    r"\S(?:.*\S)?$"
)


def load_allowed_types(config_path: Path) -> set[str]:
    config = json.loads(config_path.read_text(encoding="utf-8"))
    return {section["type"] for section in config["changelog-sections"]}


def validate_title(title: str, allowed_types: set[str]) -> None:
    match = TITLE_PATTERN.fullmatch(title)
    if match is None:
        raise ValueError(
            "Pull request title must use Conventional Commits, for example "
            "'feat: add navigation API' or 'fix(widget): dispose native handle'."
        )

    commit_type = match.group("type")
    if commit_type not in allowed_types:
        choices = ", ".join(sorted(allowed_types))
        raise ValueError(f"Unsupported pull request type {commit_type!r}. Use one of: {choices}.")


def main() -> int:
    parser = argparse.ArgumentParser(description="Validate a pull request title.")
    parser.add_argument("title", help="Pull request title to validate.")
    parser.add_argument(
        "--config",
        type=Path,
        default=Path("release-please-config.json"),
        help="Release Please configuration containing the allowed commit types.",
    )
    args = parser.parse_args()

    validate_title(args.title, load_allowed_types(args.config))
    print(f"Validated pull request title: {args.title}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
