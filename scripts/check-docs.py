# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 CarlYaki and Meshvale contributors.

"""Verify local Markdown links and anchors within one independent checkout."""
import json
import re
import subprocess
from pathlib import Path
from urllib.parse import unquote


def source_files(root):
    result = subprocess.run(
        ["git", "-C", str(root), "ls-files", "--cached", "--others",
         "--exclude-standard", "-z"], capture_output=True, check=True
    )
    return sorted(set(result.stdout.decode("utf-8").split("\0")) - {""})


def anchors(text):
    found = set(re.findall(r'<a\s+id=["\x27]([^"\x27]+)', text))
    duplicates = {}
    for heading in re.findall(r"^#{1,6}\s+(.+)$", text, re.M):
        slug = re.sub(r"[^\w\s-]", "", heading).strip().lower()
        slug = re.sub(r"\s", "-", slug)
        number = duplicates.get(slug, 0)
        duplicates[slug] = number + 1
        found.add(slug if number == 0 else f"{slug}-{number}")
    return found


def main():
    root = Path(__file__).resolve().parents[1]
    errors = []
    count = 0
    names = source_files(root)
    for name in names:
        path = root / name
        if path.suffix != ".md" or not path.is_file():
            continue
        text = path.read_text(encoding="utf-8-sig")
        for target in re.findall(r"!?\[[^\]]*\]\(([^)\n]+)\)", text):
            target = target.strip()
            if target.startswith("<"):
                target = target[1:target.index(">")]
            else:
                target = target.split(' "', 1)[0]
            if re.match(r"[A-Za-z][A-Za-z0-9+.-]*:", target):
                continue
            local, _, fragment = target.partition("#")
            linked = (path.parent / unquote(local)).resolve() if local else path
            count += 1
            if not linked.is_relative_to(root):
                errors.append(f"{name}: link leaves this checkout: {target}")
            elif not linked.is_file():
                errors.append(f"{name}: missing file: {target}")
            elif fragment and linked.suffix == ".md":
                if unquote(fragment) not in anchors(linked.read_text(encoding="utf-8-sig")):
                    errors.append(f"{name}: missing anchor: {target}")
    template = root / "environment.example.json"
    if template.exists():
        try:
            config = json.loads(template.read_text(encoding="utf-8-sig"))
            for field in ("workspace_root", "vcpkg_root", "asset_corpus_root"):
                if field in config and config[field] is not None:
                    errors.append(f"environment.example.json: {field} must be unset")
        except (ValueError, UnicodeError) as error:
            errors.append(f"environment.example.json: invalid JSON: {error}")
    if errors:
        print("\n".join(errors))
        return 1
    print(f"Documentation check passed: {count} local links/anchors; environment template valid.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
