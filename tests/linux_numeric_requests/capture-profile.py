# SPDX-License-Identifier: Apache-2.0
"""Capture the actual compiled owners' preprocessor route, without changing it."""
import argparse
import hashlib
import json
from pathlib import Path
import shlex
import subprocess

OWNERS = ("scalar_buffer", "vector_buffer")
DROP_SINGLE = {"-c", "-MD", "-MMD", "-MP"}
DROP_PAIR = {"-o", "-MF", "-MT", "-MQ"}

def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path, required=True)
    parser.add_argument("--compile-commands", type=Path, required=True)
    parser.add_argument("--revision", required=True)
    parser.add_argument("--profile", choices=("native", "asan-ubsan"), required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    assert len(args.revision) == 40 and all(c in "0123456789abcdef" for c in args.revision)
    source = args.source.resolve()
    commands = json.loads(args.compile_commands.read_text(encoding="utf-8"))
    args.output.mkdir(parents=True, exist_ok=False)
    report = {"revision": args.revision, "profile": args.profile,
              "compile_commands_sha256": digest(args.compile_commands), "owners": {}}
    for owner in OWNERS:
        actual_source = source / "src" / (owner + ".cpp")
        matches = []
        for entry in commands:
            file = Path(entry["file"])
            if not file.is_absolute():
                file = Path(entry["directory"]) / file
            if file.resolve() == actual_source:
                matches.append(entry)
        assert len(matches) == 1, "Need one exact owning translation-unit command"
        entry = matches[0]
        original = entry.get("arguments") or shlex.split(entry["command"])
        assert "clang" in Path(original[0]).name
        sanitizer_flags = [arg for arg in original if arg.startswith("-fsanitize=")]
        if args.profile == "asan-ubsan":
            features = set().union(*(set(f.split("=", 1)[1].split(",")) for f in sanitizer_flags))
            assert features == {"address", "undefined"}, "Owner instrumentation differs from observer profile"
        else:
            assert not sanitizer_flags, "Native owner unexpectedly instrumented"
        preprocessor = []
        index = 0
        while index < len(original):
            argument = original[index]
            if argument in DROP_PAIR:
                index += 2
                continue
            if argument not in DROP_SINGLE:
                preprocessor.append(argument)
            index += 1
        preprocessor.extend(["-dM", "-E"])
        result = subprocess.run(preprocessor, cwd=entry["directory"], capture_output=True, text=True)
        (args.output / (owner + ".stdout")).write_text(result.stdout, encoding="utf-8")
        (args.output / (owner + ".stderr")).write_text(result.stderr, encoding="utf-8")
        macros = {}
        for line in result.stdout.splitlines():
            if line.startswith("#define "):
                pieces = line.split(maxsplit=2)
                if len(pieces) == 3:
                    macros[pieces[1]] = pieces[2]
        report["owners"][owner] = {"source_sha256": digest(actual_source),
            "original_arguments": original, "preprocessor_arguments": preprocessor,
            "exit_code": result.returncode, "macros": macros}
        (args.output / "profile.json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
        result.check_returncode()
    report["allocation_source_sha256"] = digest(source / "src" / "materialization_allocation.cpp")
    (args.output / "profile.json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    return 0

if __name__ == "__main__":
    raise SystemExit(main())
