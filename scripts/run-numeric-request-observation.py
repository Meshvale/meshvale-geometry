# SPDX-License-Identifier: Apache-2.0
"""Record fresh Linux original numerical requests for one exact source archive."""
import argparse
import hashlib
import io
import json
from pathlib import Path
import platform
import re
import runpy
import shlex
import shutil
import subprocess
import sys
import tarfile
from urllib.parse import urlsplit

PROFILE_NATIVE = "native"
PROFILE_ASAN_UBSAN = "asan-ubsan"
PROFILE_TSAN = "tsan"
PROFILES = (PROFILE_NATIVE, PROFILE_ASAN_UBSAN, PROFILE_TSAN)
EXPECTED_COMPILED_PROFILES = {
    PROFILE_NATIVE: 1,
    PROFILE_ASAN_UBSAN: 2,
    PROFILE_TSAN: 3,
}
EXPECTED_COMPILED_PROFILE_OPTION = "--expected-compiled-profile"
EXPECTED_COMPILED_PROFILE_KEY = "expected_compiled_profile"
SANITIZER_FLAGS = {
    PROFILE_ASAN_UBSAN: ("-fsanitize=address,undefined -fno-omit-frame-pointer",
                        "-fsanitize=address,undefined"),
    PROFILE_TSAN: ("-fsanitize=thread -O1 -g -fno-omit-frame-pointer",
                   "-fsanitize=thread"),
}
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
    expected_compiled_profile = EXPECTED_COMPILED_PROFILES[args.profile]
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
        command = {"name": name, "arguments": [str(value) for value in arguments],
                   "timeout_seconds": timeout, "outcome": "started"}
        commands.append(command)
        write_json("commands.json", commands)
        try:
            with (output / (name + ".stdout")).open("w", encoding="utf-8") as stdout, \
                 (output / (name + ".stderr")).open("w", encoding="utf-8") as stderr:
                result = subprocess.run(command["arguments"], cwd=cwd or output,
                                        stdout=stdout, stderr=stderr, timeout=timeout)
        except subprocess.TimeoutExpired as error:
            command.update(outcome="timeout", error=str(error))
            write_json("commands.json", commands)
            raise
        except OSError as error:
            command.update(outcome="launch-error", error=str(error))
            write_json("commands.json", commands)
            raise
        command.update(outcome="completed", exit_code=result.returncode)
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
               EXPECTED_COMPILED_PROFILE_KEY: expected_compiled_profile,
               "tree": subprocess.check_output(["git", "show", "-s", "--format=%T", revision],
                                              cwd=repository, text=True).strip(),
               "archive_sha256": digest(output / "source.tar"), "files": source_files})
    cmake = tools["cmake"]
    build = output / "producer-build"
    prefix = output / "producer-prefix"
    dependency_workspace = output / "dependency-workspace"
    flags = ["-DCMAKE_BUILD_TYPE=Debug", "-DCMAKE_CXX_COMPILER=" + tools["clang++"],
             "-DCMAKE_EXPORT_COMPILE_COMMANDS=ON", "-DCMAKE_INTERPROCEDURAL_OPTIMIZATION=OFF"]
    if args.profile in SANITIZER_FLAGS:
        compile_flags, link_flags = SANITIZER_FLAGS[args.profile]
        flags += ["-DCMAKE_CXX_FLAGS=" + compile_flags,
                  "-DCMAKE_EXE_LINKER_FLAGS=" + link_flags]
    run("producer-configure", [cmake, "-S", source, "-B", build, "-G", "Ninja",
        *flags, "-DBUILD_TESTING=OFF", "-DCMAKE_DISABLE_FIND_PACKAGE_Eigen3=ON",
        "-DFETCHCONTENT_BASE_DIR=" + str(dependency_workspace)])
    dependency_owner = source / "cmake/MeshvaleEigen.cmake"
    dependency_contract = dependency_owner.read_text(encoding="utf-8")
    dependency_url = re.search(r"URL\s+(https://\S+)", dependency_contract).group(1)
    dependency_hash = re.search(r"URL_HASH\s+SHA256=([0-9a-f]{64})",
                                dependency_contract).group(1)
    dependency_archives = list(dependency_workspace.rglob(Path(urlsplit(dependency_url).path).name))
    assert len(dependency_archives) == 1
    dependency_archive = dependency_archives[0]
    assert digest(dependency_archive) == dependency_hash
    dependency_source = dependency_workspace / "meshvale_eigen-src"
    assert (dependency_source / "Eigen/Core").is_file()
    dependency_files = manifest(dependency_source)
    write_json("dependency-inputs.json", {"owner_sha256": digest(dependency_owner),
        "url": dependency_url, "archive_path": dependency_archive.relative_to(output).as_posix(),
        "archive_sha256": digest(dependency_archive), "files": dependency_files})
    run("producer-build", [cmake, "--build", build, "--parallel", BUILD_WORKERS])
    run("producer-install", [cmake, "--install", build, "--prefix", prefix])
    prefix_files = manifest(prefix)
    write_json("producer-prefix-inputs.json", prefix_files)
    observer_source = source / "tests/linux_numeric_requests"
    validate_instrumentation = runpy.run_path(
        str(observer_source / "capture-profile.py"))["validate_instrumentation"]

    def verify_compiled_inputs(name, compile_commands, source_root):
        records = []
        entries = json.loads(compile_commands.read_text(encoding="utf-8"))
        assert entries, "No project compilation inputs retained"
        for entry in entries:
            directory = Path(entry["directory"])
            actual_source = Path(entry["file"])
            if not actual_source.is_absolute():
                actual_source = directory / actual_source
            assert actual_source.resolve().is_relative_to(source_root.resolve())
            arguments = entry.get("arguments") or shlex.split(entry["command"])
            validate_instrumentation(arguments, args.profile)
            object_file = Path(arguments[arguments.index("-o") + 1])
            if not object_file.is_absolute():
                object_file = directory / object_file
            assert object_file.is_file()
            records.append({"source": actual_source.relative_to(source).as_posix(),
                            "source_sha256": digest(actual_source),
                            "arguments": arguments,
                            "object": object_file.relative_to(output).as_posix(),
                            "object_sha256": digest(object_file)})
        write_json(name, {"profile": args.profile, "records": records,
                         "compile_commands_sha256": digest(compile_commands)})

    verify_compiled_inputs("producer-compiled-inputs.json",
                           build / "compile_commands.json", source)
    owner_profile = output / "owner-profile"
    run("owner-profile", [sys.executable, observer_source / "capture-profile.py",
        "--source", source, "--compile-commands", build / "compile_commands.json",
        "--revision", revision, "--profile", args.profile, "--output", owner_profile])
    for owner in json.loads((owner_profile / "profile.json").read_text(encoding="utf-8"))["owners"].values():
        assert str(dependency_source) in owner["original_arguments"]
    observer_build = output / "observer-build"
    run("observer-configure", [cmake, "-S", observer_source, "-B", observer_build, "-G",
        "Ninja", *flags, "-DCMAKE_PREFIX_PATH=" + str(prefix),
        "-DOBSERVER_PROFILE=" + args.profile,
        "-DOBSERVER_GEOMETRY_SOURCE=" + str(source),
        "-DOBSERVER_GEOMETRY_REVISION=" + revision])
    run("observer-build", [cmake, "--build", observer_build, "--parallel", BUILD_WORKERS])
    verify_compiled_inputs("observer-compiled-inputs.json",
                           observer_build / "compile_commands.json", observer_source)
    executable = observer_build / "observer"
    run("observer-symbols", [tools["readelf"], "--symbols", "--wide", executable])
    run("observer-disassembly", [tools["objdump"], "--disassemble", "--reloc", executable])
    run("observer-runtime-dependencies", [tools["ldd"], executable])
    run("observer-execute", [executable], timeout=OBSERVATION_TIMEOUT_SECONDS)
    run("observer-check", [sys.executable, observer_source / "check-trace.py",
        output / "observer-execute.stdout", owner_profile / "profile.json",
        output / "request-report.json", EXPECTED_COMPILED_PROFILE_OPTION,
        str(expected_compiled_profile)])
    assert manifest(source) == source_files and manifest(prefix) == prefix_files
    assert manifest(dependency_source) == dependency_files and digest(dependency_archive) == dependency_hash
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
        EXPECTED_COMPILED_PROFILE_KEY: expected_compiled_profile,
        "files": files, "scope": "Original numerical request observation prerequisite; public factory/profile admission and real-worker TSan remain separate"})
    print("Original numerical observation completed; production profile admission remains separate")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
