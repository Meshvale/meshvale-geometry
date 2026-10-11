# SPDX-License-Identifier: Apache-2.0
"""Capture the actual compiled owners' preprocessor route, without changing it."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import shlex
import subprocess

OWNERS = ("scalar_buffer", "vector_buffer")
DROP_SINGLE = {"-c", "-MD", "-MMD", "-MP"}
DROP_PAIR = {"-o", "-MF", "-MT", "-MQ"}
SUPPRESSION_PREFIXES = ("-fno-sanitize", "-fsanitize-ignorelist",
                        "-fsanitize-blacklist", "-fsanitize-system-ignorelist")
PROFILES = ("native", "asan-ubsan", "tsan")
MACRO_DEFINITION_PREFIX = "#define "


def validate_instrumentation(arguments, profile):
    assert not any(arg.startswith(SUPPRESSION_PREFIXES) for arg in arguments), \
        "Owner sanitizer disabling or ignorelist inputs are forbidden"
    assert not any(arg.startswith("@") or arg == "--config" or arg.startswith("--config=")
                   for arg in arguments), "Opaque response/config inputs cannot prove owner instrumentation"
    assert "-Xclang" not in arguments, "Opaque frontend inputs cannot prove owner instrumentation"
    assert not any(arg.startswith("--optimize") for arg in arguments), \
        "Unclassified optimization aliases cannot prove owner instrumentation"
    sanitizer_flags = [arg for arg in arguments if arg.startswith("-fsanitize=")]
    if profile in ("asan-ubsan", "tsan"):
        features = set().union(*(set(f.split("=", 1)[1].split(",")) for f in sanitizer_flags))
        expected = {"thread"} if profile == "tsan" else {"address", "undefined"}
        assert features == expected, "Owner instrumentation differs from observer profile"
        if profile == "tsan":
            optimizations = [arg for arg in arguments if arg.startswith("-O")]
            debug = [arg for arg in arguments if arg.startswith("-g")]
            assert all(arg == "-g" or re.fullmatch(r"-g[0-3]", arg)
                       for arg in debug), "Unclassified debug options cannot prove TSan debug information"
            frames = [arg for arg in arguments if arg in ("-fomit-frame-pointer", "-fno-omit-frame-pointer")]
            assert optimizations and optimizations[-1] == "-O1", "TSan owner must use effective -O1"
            assert debug and debug[-1] != "-g0", "TSan owner must retain debug information"
            assert frames and frames[-1] == "-fno-omit-frame-pointer", "TSan owner must retain frame pointers"
    else:
        assert not sanitizer_flags, "Native owner unexpectedly instrumented"

def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()

def parse_macro_definitions(stdout):
    """Membership means defined; each value is its possibly empty replacement."""
    macros = {}
    for line in stdout.splitlines():
        if line.startswith(MACRO_DEFINITION_PREFIX):
            pieces = line.split(maxsplit=2)
            if len(pieces) < 2:
                raise ValueError("Preprocessor definition is missing its name")
            macros[pieces[1]] = pieces[2] if len(pieces) == 3 else ""
    return macros

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path, required=True)
    parser.add_argument("--compile-commands", type=Path, required=True)
    parser.add_argument("--revision", required=True)
    parser.add_argument("--profile", choices=PROFILES, required=True)
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
        validate_instrumentation(original, args.profile)
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
        macros = parse_macro_definitions(result.stdout)
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
