#!/usr/bin/env python3

"""读取并校验产品版本；发布脚本不得自行生成或拼接软件版本号。"""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path


SEMVER_PATTERN = re.compile(
    r"^(0|[1-9][0-9]*)\."
    r"(0|[1-9][0-9]*)\."
    r"(0|[1-9][0-9]*)"
    r"(?:-(?:0|[1-9][0-9]*|[0-9A-Za-z-]*[A-Za-z-][0-9A-Za-z-]*)"
    r"(?:\.(?:0|[1-9][0-9]*|[0-9A-Za-z-]*[A-Za-z-][0-9A-Za-z-]*))*)?"
    r"(?:\+[0-9A-Za-z-]+(?:\.[0-9A-Za-z-]+)*)?$"
)


class VersionError(ValueError):
    """产品版本文件或显式覆盖值不符合发布契约。"""


def validate_version(value: str, source: str) -> str:
    """只接受纯 SemVer，文件和 API 不保存显示前缀或研发阶段标识。"""
    if not SEMVER_PATTERN.fullmatch(value):
        raise VersionError(
            f"{source} 必须是纯 SemVer（例如 0.9.0 或 0.9.1-rc.1），"
            "不能带显示前缀或研发阶段标识"
        )
    return value


def read_repository_version(repository_root: Path) -> str:
    """严格读取仓库根 VERSION，拒绝 BOM、空白和多行内容。"""
    version_file = repository_root / "VERSION"
    try:
        payload = version_file.read_bytes()
    except OSError as error:
        raise VersionError(f"无法读取产品版本文件 {version_file}: {error}") from error
    try:
        text = payload.decode("utf-8")
    except UnicodeDecodeError as error:
        raise VersionError(f"产品版本文件不是 UTF-8：{version_file}") from error

    if text.endswith("\n"):
        text = text[:-1]
    if "\n" in text or "\r" in text or text != text.strip():
        raise VersionError(f"产品版本文件必须只包含一行纯版本号：{version_file}")
    return validate_version(text, str(version_file))


def resolve_version(repository_root: Path, override: str = "") -> str:
    """显式覆盖优先；正常无覆盖发布读取仓库根 VERSION。"""
    if override:
        return validate_version(override, "显式 PACKAGE_VERSION")
    return read_repository_version(repository_root)


def main() -> int:
    parser = argparse.ArgumentParser(description="解析 edge-controller 产品版本")
    parser.add_argument("--repository-root", required=True, type=Path)
    parser.add_argument("--override", default="")
    arguments = parser.parse_args()
    try:
        version = resolve_version(arguments.repository_root.resolve(), arguments.override)
    except VersionError as error:
        print(f"product-version: ERROR: {error}", file=sys.stderr)
        return 2
    print(version)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
