# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 CarlYaki and Meshvale contributors.

"""Check publishable files for machine paths and private environment records."""

import argparse
import re
import subprocess
from pathlib import Path


PRIVATE_DIRS = {".local", ".scratch", "references", "build", "__pycache__"}
LOCAL_FILES = {"CMakeUserPresets.json", ".env"}
PATH_PATTERNS = (
    re.compile(r"\b[A-Za-z]:[\\/]"),
    re.compile(r"\\\\[\w.-]+\\[\w.$-]+"),
    re.compile(r"file[:][/][/]", re.IGNORECASE),
    re.compile(r"(?<![\w/:])/(?:home|Users|mnt|Volumes)/[^\s)\]\"'<>]+"),
)


def private_file(path):
    return (
        any(part in PRIVATE_DIRS for part in path.parts)
        or path.name in LOCAL_FILES
        or (path.name.startswith(".env.") and path.name != ".env.example")
        or path.suffix == ".pyc"
    )


def git_run(root, *args):
    return subprocess.run(
        ["git", "-C", str(root), *args], capture_output=True, check=False
    )


def candidates(root):
    try:
        probe = git_run(root, "rev-parse", "--show-toplevel")
    except FileNotFoundError:
        probe = None
    if probe and probe.returncode == 0:
        top = Path(probe.stdout.decode().strip()).resolve()
        if top != root:
            raise RuntimeError("Run against the repository root, not a nested directory.")
        result = git_run(root, "ls-files", "--cached", "--others", "--exclude-standard", "-z")
        if result.returncode:
            raise RuntimeError("Git file enumeration failed.")
        names = sorted(set(result.stdout.decode("utf-8").split("\0")) - {""})
        staged = git_run(root, "ls-files", "--cached", "-z")
        if staged.returncode:
            raise RuntimeError("Git index enumeration failed.")
        index_names = set(staged.stdout.decode("utf-8").split("\0")) - {""}
        return [Path(name) for name in names], index_names
    return sorted(
        p.relative_to(root) for p in root.rglob("*")
        if p.is_file() and ".git" not in p.relative_to(root).parts
        and not private_file(p.relative_to(root))
    ), set()


def inspect(data, label):
    if b"\0" in data:
        return [f"{label}: binary file requires publication review"]
    try:
        content = data.decode("utf-8-sig")
    except UnicodeDecodeError:
        return [f"{label}: non-UTF-8 file requires publication review"]
    return [
        f"{label}:{number}: machine-specific path"
        for number, line in enumerate(content.splitlines(), 1)
        if any(pattern.search(line) for pattern in PATH_PATTERNS)
    ]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, default=Path(__file__).resolve().parents[1])
    root = parser.parse_args().root.resolve()
    errors = []
    paths, index_names = candidates(root)
    for relative in paths:
        label = relative.as_posix()
        if private_file(relative):
            errors.append(f"{label}: private local file is in the publication set")
            continue
        file = root / relative
        if file.is_symlink():
            errors.append(f"{label}: symbolic link requires publication review")
            continue
        if file.exists():
            errors.extend(inspect(file.read_bytes(), label))
        if label in index_names:
            staged = git_run(root, "show", f":{label}")
            if staged.returncode:
                errors.append(f"{label}: could not inspect index content")
            elif not file.exists() or staged.stdout != file.read_bytes():
                errors.extend(inspect(staged.stdout, label + " (index)"))
    if errors:
        print("\n".join(errors))
        return 1
    print(f"Portability check passed: {len(paths)} publishable files inspected.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
