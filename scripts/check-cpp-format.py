# SPDX-License-Identifier: Apache-2.0
"""Check repository-owned C++ against the pinned Google formatter projection."""
import argparse
from pathlib import Path
import subprocess


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--formatter', default='clang-format')
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    version = subprocess.check_output([args.formatter, '--version'], text=True).strip()
    if version != 'clang-format version 23.1.3':
        parser.error('use clang-format 23.1.3 for reproducible formatting')
    tracked = subprocess.check_output(
        ['git', 'ls-files', '--cached', '--others', '--exclude-standard', '-z'], cwd=root
    ).decode('utf-8').split('\0')
    names = sorted({name for name in tracked if name and
                    Path(name).suffix in {'.h', '.hpp', '.cpp'} and (root / name).is_file()})
    if names:
        subprocess.run([args.formatter, '--dry-run', '--Werror', *names], cwd=root, check=True)
    print(f'C++ format check passed: {len(names)} project files; {version}')
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
