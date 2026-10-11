# SPDX-License-Identifier: Apache-2.0
"""Verify explicit CMake numerical/probe profiles and exact native inventory."""
import argparse
import json
from pathlib import Path
import subprocess
import re

MATERIALIZATION_PROFILES = ("qualified", "unsupported")
COMMON_TESTS = frozenset({"attribute_storage", "polygon_topology", "pooled_editing",
    "shared_execution_leases", "exact_polygon_triangulation", "triangulation_fraction_oracle",
    "checked_attribute_numerics", "eigen_scalar_storage", "editor_traversal_batch"})
QUALIFIED_TESTS = frozenset({"charged_materialization", "materialization_semantics",
    "materialization_accounting_workers", "materialization_fault_workers"})
UNSUPPORTED_TESTS = frozenset({"unsupported_materialization_profile"})
QUALIFIED_INSTALLED_TESTS = frozenset({"installed_materialization_consumer"})
UNSUPPORTED_INSTALLED_TESTS = frozenset({"installed_unsupported_materialization_profile"})
ALLOCATION_TESTS = frozenset({"execution_allocation_failure",
    "attribute_numerics_allocation_failure", "editor_batch_allocation_failure"})
PROFILE_CACHE_KEY = "MESHVALE_GEOMETRY_MATERIALIZATION_TEST_PROFILE"
PROBE_CACHE_KEY = "MESHVALE_GEOMETRY_ALLOCATION_PROBE"
TRUE_CACHE_VALUES = frozenset({"ON", "TRUE", "YES", "1"})
FALSE_CACHE_VALUES = frozenset({"OFF", "FALSE", "NO", "0"})


def cache_entries(path):
    result = {}
    for line in path.read_text(encoding="utf-8").splitlines():
        if not line or line.startswith(("#", "//")):
            continue
        name_and_type, separator, value = line.partition("=")
        name, typed, cache_type = name_and_type.partition(":")
        if separator and typed:
            if name in result:
                raise ValueError("duplicate CMake cache entry: " + name)
            result[name] = (cache_type, value)
    return result


def cache_value(cache, name, expected_type):
    if name not in cache or cache[name][0] != expected_type:
        raise ValueError("missing or incorrectly typed CMake cache entry: " + name)
    return cache[name][1]


def cache_bool(cache, name):
    value = cache_value(cache, name, "BOOL").upper()
    if value in TRUE_CACHE_VALUES:
        return True
    if value in FALSE_CACHE_VALUES:
        return False
    raise ValueError("unrecognized CMake boolean: " + name)


def validate_cache(cache, profile, probe, configuration):
    if cache_value(cache, PROFILE_CACHE_KEY, "STRING") != profile:
        raise ValueError("materialization CLI profile differs from configured CMake profile")
    if not cache_bool(cache, "BUILD_TESTING"):
        raise ValueError("native inventory requires BUILD_TESTING=ON")
    if cache_bool(cache, PROBE_CACHE_KEY) != (probe == "enabled"):
        raise ValueError("allocation-probe CLI profile differs from configured CMake profile")
    validate_configuration(cache, configuration)


def validate_configuration(cache, configuration):
    if "CMAKE_CONFIGURATION_TYPES" in cache:
        configurations = cache_value(cache, "CMAKE_CONFIGURATION_TYPES", "STRING").split(";")
        if configuration not in configurations:
            raise ValueError("requested configuration is absent from multi-config cache")
    elif cache_value(cache, "CMAKE_BUILD_TYPE", "STRING") != configuration:
        raise ValueError("requested configuration differs from single-config CMake build type")


def validate_compiler(build_dir, expected_id, expected_version):
    records = list((build_dir / "CMakeFiles").glob("*/CMakeCXXCompiler.cmake"))
    if len(records) != 1:
        raise ValueError("need one fresh CMake compiled-compiler identity record")
    text = records[0].read_text(encoding="utf-8")
    actual_id = re.search(r'set\(CMAKE_CXX_COMPILER_ID "([^"\n]+)"\)', text)
    actual_version = re.search(r'set\(CMAKE_CXX_COMPILER_VERSION "([^"\n]+)"\)', text)
    if actual_id is None or actual_id.group(1) != expected_id:
        raise ValueError("configured C++ compiler identity differs from declared CI cohort")
    if expected_version and (actual_version is None or actual_version.group(1) != expected_version):
        raise ValueError("configured C++ compiler version differs from declared admitted cohort")


def expected_inventory(profile, probe):
    expected = set(COMMON_TESTS)
    expected.update(QUALIFIED_TESTS if profile == "qualified" else UNSUPPORTED_TESTS)
    if probe == "enabled":
        expected.update(ALLOCATION_TESTS)
    return expected


def validate_inventory(build_dir, configuration, expected):
    tests = json.loads(subprocess.check_output(["ctest", "--test-dir", str(build_dir),
        "-C", configuration, "--show-only=json-v1"], text=True))["tests"]
    names = [test["name"] for test in tests]
    if len(names) != len(expected) or set(names) != expected:
        raise ValueError(f"expected {sorted(expected)}, found {names}")
    return names


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", type=Path, required=True)
    parser.add_argument("--installed-build-dir", type=Path)
    parser.add_argument("--configuration", default="Release")
    parser.add_argument("--compiler-id", choices=("MSVC", "Clang", "GNU", "AppleClang"), required=True)
    parser.add_argument("--compiler-version")
    parser.add_argument("--materialization-profile", choices=MATERIALIZATION_PROFILES, required=True)
    parser.add_argument("--allocation-probe", choices=("enabled", "disabled"), default="enabled")
    args = parser.parse_args()
    try:
        cache = cache_entries(args.build_dir / "CMakeCache.txt")
        validate_cache(cache, args.materialization_profile, args.allocation_probe, args.configuration)
        validate_compiler(args.build_dir, args.compiler_id, args.compiler_version)
        expected = expected_inventory(args.materialization_profile, args.allocation_probe)
        names = validate_inventory(args.build_dir, args.configuration, expected)
        installed_names = None
        if args.installed_build_dir:
            validate_compiler(args.installed_build_dir, args.compiler_id, args.compiler_version)
            installed_cache = cache_entries(args.installed_build_dir / "CMakeCache.txt")
            validate_configuration(installed_cache, args.configuration)
            installed_expected = (QUALIFIED_INSTALLED_TESTS
                                  if args.materialization_profile == "qualified"
                                  else UNSUPPORTED_INSTALLED_TESTS)
            installed_names = validate_inventory(args.installed_build_dir, args.configuration,
                                                 installed_expected)
    except (OSError, ValueError, KeyError, subprocess.CalledProcessError) as error:
        parser.error(str(error))
    print(f"Native test inventory passed: {len(names)} suites; materialization "
          f"{args.materialization_profile}; allocation probe {args.allocation_probe}; "
          f"configuration {args.configuration}; {names}")
    if installed_names is not None:
        print(f"Installed materialization inventory passed: {installed_names}")


if __name__ == "__main__":
    main()
