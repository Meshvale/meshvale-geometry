# SPDX-License-Identifier: Apache-2.0
"""Inspect wheel/source archives against this product's publication file list."""
import argparse
from pathlib import Path, PurePosixPath
import tarfile
import zipfile


def inspect(path):
    if path.suffix == ".whl":
        with zipfile.ZipFile(path) as archive:
            members = archive.infolist()
            names = [member.filename for member in members if not member.is_dir()]
        for member in members:
            name = member.filename
            parts = PurePosixPath(name).parts
            assert not PurePosixPath(name).is_absolute() and ".." not in parts, name
            assert parts[0] in {"meshvale_geometry", "meshvale_reports"} or parts[0].endswith(".dist-info"), name
            if member.is_dir():
                # Repair tools emit structural ZIP directories, not package payload.
                assert len(parts) == 1 or parts[0].endswith(".dist-info"), name
                continue
            if parts[0] == "meshvale_reports":
                assert len(parts) == 2 and parts[1] in {"__init__.py", "report-v1.schema.json"}, name
            if parts[0] == "meshvale_geometry":
                assert len(parts) == 2 and (parts[1] in {"__init__.py","_version.py"} or
                       (parts[1].startswith("_geometry.") and parts[1].endswith((".pyd",".so")))), name
        assert any(name.endswith("/METADATA") for name in names)
        assert any(name.endswith("/_version.py") for name in names)
        assert "meshvale_reports/report-v1.schema.json" in names
        for license in ["LICENSE","NOTICE","nanobind.txt","robin-map.txt"]:
            assert any(name.endswith("/"+license) and ".dist-info/licenses/" in name for name in names), license
    else:
        with tarfile.open(path, "r:gz") as archive:
            members = archive.getmembers()
        names = []
        allowed = {".clang-format","CMakeLists.txt","pyproject.toml","README.md","AGENTS.md","ENVIRONMENT.md",
                   "environment.example.json","LICENSE","NOTICE","CHANGELOG.md","PKG-INFO"}
        directories = {"cmake","include","src","python","docs","examples","tests","licenses"}
        for member in members:
            assert member.isfile(), member.name
            parts = PurePosixPath(member.name).parts
            assert not PurePosixPath(member.name).is_absolute() and ".." not in parts and len(parts) >= 2, member.name
            name = "/".join(parts[1:]); names.append(name)
            assert name in allowed or parts[1] in directories or name in {
                "scripts/check-package.py","scripts/test-installed.py","scripts/check-docs.py","scripts/check-portability.py","scripts/check-cpp-format.py"}, name
            assert not any(part in {".local",".scratch","__pycache__","references","build",".github"} for part in parts), name
            assert not name.endswith((".pyc",".pyd",".so",".obj",".log")), name
        assert "python/meshvale_geometry/_version.py" in names
        assert ".clang-format" in names
        assert "scripts/check-cpp-format.py" in names
        assert "python/bindings.cpp" in names
        for header in ["attributes", "mesh", "topology", "python/record"]:
            for extension in ["h", "hpp"]:
                assert "include/meshvale/geometry/" + header + "." + extension in names
        assert "tests/python/test_mesh.py" in names
        assert "include/meshvale/geometry/editable_mesh.h" in names
        assert "src/editable_mesh.cpp" in names
        assert "tests/editing.cpp" in names
        assert "examples/editing-consumer/main.cpp" in names
        assert "python/meshvale_reports/report-v1.schema.json" in names
        assert "tests/reports/test_reports.py" in names
    print(f"Package content check passed: {path.name}; {len(names)} files")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("archives",nargs="+",type=Path)
    for path in parser.parse_args().archives:
        inspect(path)
