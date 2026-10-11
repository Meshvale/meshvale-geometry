# SPDX-License-Identifier: Apache-2.0
"""Record fresh Linux original numerical requests for one exact source archive."""
import argparse
import hashlib
import io
import json
from pathlib import Path
import platform
import shutil
import subprocess
import sys
import tarfile

PROFILES = ("native", "asan-ubsan")
SANITIZER_FLAGS = "-fsanitize=address,undefined -fno-omit-frame-pointer"
BUILD_WORKERS = "2"
COMMAND_TIMEOUT_SECONDS = 600
OBSERVATION_TIMEOUT_SECONDS = 120


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def manifest(directory):
    return [{"path": path.relative_to(directory).as_posix(),
             "bytes": path.stat().st_size, "sha256": digest(path)}
            for path in sorted(directory.rglob("*")) if path.is_file()]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--profile", choices=PROFILES, required=True)
    parser.add_argument("--revision", required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    repository = Path(__file__).resolve().parents[1]
    assert platform.system() == "Linux" and platform.machine() == "x86_64"
    revision = subprocess.check_output(
        ["git", "rev-parse", "HEAD"], cwd=repository, text=True).strip()
    assert revision == args.revision
    assert not subprocess.check_output(
        ["git", "status", "--porcelain"], cwd=repository, text=True).strip()
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    commands = []

    def write_json(name, value):
        (output / name).write_text(json.dumps(value, indent=2) + "\n", encoding="utf-8")

    def run(name, arguments, cwd=None, timeout=COMMAND_TIMEOUT_SECONDS):
        with (output / (name + ".stdout")).open("w", encoding="utf-8") as stdout, \
             (output / (name + ".stderr")).open("w", encoding="utf-8") as stderr:
            result = subprocess.run([str(value) for value in arguments], cwd=cwd or output,
                                    stdout=stdout, stderr=stderr, timeout=timeout)
        commands.append({"name": name, "arguments": [str(value) for value in arguments],
                         "exit_code": result.returncode})
        write_json("commands.json", commands)
        print(f"{args.profile} {name}: {result.returncode}", flush=True)
        result.check_returncode()

    tools = {}
    for name in ("cmake", "clang++", "ninja", "ld", "ldd", "objdump", "readelf"):
        executable = shutil.which(name)
        assert executable, f"Required observation tool is unavailable: {name}"
        tools[name] = executable
        run("version-" + name.replace("+", "x"), [executable, "--version"])
    write_json("host.json", {"system": platform.platform(), "machine": platform.machine(),
                            "python": sys.version, "tools": tools})
    archive = subprocess.check_output(
        ["git", "archive", "--format=tar", revision], cwd=repository)
    (output / "source.tar").write_bytes(archive)
    source = output / "source"
    source.mkdir()
    with tarfile.open(fileobj=io.BytesIO(archive)) as members:
        for member in members:
            destination = source / member.name
            assert destination.resolve().is_relative_to(source.resolve())
            if member.isdir():
                destination.mkdir(parents=True, exist_ok=True)
            else:
                assert member.isfile()
                destination.parent.mkdir(parents=True, exist_ok=True)
                destination.write_bytes(members.extractfile(member).read())
    source_files = manifest(source)
    write_json("inputs.json", {"revision": revision, "profile": args.profile,
               "tree": subprocess.check_output(["git", "show", "-s", "--format=%T", revision],
                                              cwd=repository, text=True).strip(),
               "archive_sha256": digest(output / "source.tar"), "files": source_files})
    cmake = tools["cmake"]
    build = output / "producer-build"
    prefix = output / "producer-prefix"
    flags = ["-DCMAKE_BUILD_TYPE=Debug", "-DCMAKE_CXX_COMPILER=" + tools["clang++"],
             "-DCMAKE_EXPORT_COMPILE_COMMANDS=ON", "-DCMAKE_INTERPROCEDURAL_OPTIMIZATION=OFF"]
    if args.profile == "asan-ubsan":
        flags += ["-DCMAKE_CXX_FLAGS=" + SANITIZER_FLAGS,
                  "-DCMAKE_EXE_LINKER_FLAGS=-fsanitize=address,undefined"]
    run("producer-configure", [cmake, "-S", source, "-B", build, "-G", "Ninja",
                              *flags, "-DBUILD_TESTING=OFF"])
    run("producer-build", [cmake, "--build", build, "--parallel", BUILD_WORKERS])
    run("producer-install", [cmake, "--install", build, "--prefix", prefix])
    prefix_files = manifest(prefix)
    write_json("producer-prefix-inputs.json", prefix_files)
    observer_source = source / "tests/linux_numeric_requests"
    owner_profile = output / "owner-profile"
    run("owner-profile", [sys.executable, observer_source / "capture-profile.py",
        "--source", source, "--compile-commands", build / "compile_commands.json",
        "--revision", revision, "--profile", args.profile, "--output", owner_profile])
    observer_build = output / "observer-build"
    run("observer-configure", [cmake, "-S", observer_source, "-B", observer_build, "-G",
        "Ninja", *flags, "-DCMAKE_PREFIX_PATH=" + str(prefix),
        "-DOBSERVER_PROFILE=" + args.profile,
        "-DOBSERVER_GEOMETRY_SOURCE=" + str(source),
        "-DOBSERVER_GEOMETRY_REVISION=" + revision])
    run("observer-build", [cmake, "--build", observer_build, "--parallel", BUILD_WORKERS])
    executable = observer_build / "observer"
    run("observer-symbols", [tools["readelf"], "--symbols", "--wide", executable])
    run("observer-disassembly", [tools["objdump"], "--disassemble", "--reloc", executable])
    run("observer-runtime-dependencies", [tools["ldd"], executable])
    run("observer-execute", [executable], timeout=OBSERVATION_TIMEOUT_SECONDS)
    run("observer-check", [sys.executable, observer_source / "check-trace.py",
        output / "observer-execute.stdout", owner_profile / "profile.json",
        output / "request-report.json"])
    assert manifest(source) == source_files and manifest(prefix) == prefix_files
    files = [{"path": path.relative_to(output).as_posix(), "bytes": path.stat().st_size,
              "sha256": digest(path)} for path in sorted(output.rglob("*"))
             if path.is_file() and "source" not in path.relative_to(output).parts
             and "producer-build" not in path.relative_to(output).parts
             and "observer-build" not in path.relative_to(output).parts]
    for path in (build / "compile_commands.json", observer_build / "compile_commands.json",
                 observer_build / "observer.map", executable):
        files.append({"path": path.relative_to(output).as_posix(),
                      "bytes": path.stat().st_size, "sha256": digest(path)})
    write_json("evidence-index.json", {"revision": revision, "profile": args.profile,
        "files": files, "scope": "Original numerical request observation prerequisite; public factory/profile admission and real-worker TSan remain separate"})
    print("Original numerical observation completed; production profile admission remains separate")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
