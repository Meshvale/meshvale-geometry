# SPDX-License-Identifier: Apache-2.0
"""Black-box native source identity checks; no compiler or Python backend is run.

All Git mutations and tags belong to original temporary fixture repositories.
Pass --output to retain commands, streams, sources and the case receipt.
"""
import argparse
import hashlib
import io
import json
import os
from pathlib import Path
import shutil
import subprocess
import tarfile
import tempfile


ROOT = Path(__file__).resolve().parents[1]
PRODUCT = "meshvale-geometry"


class Probe:
    def __init__(self, output, cmake):
        self.output = output
        self.cmake = cmake
        self.commands = []
        self.cases = []
        self.environment = os.environ.copy()
        for key in ("GIT_DIR", "GIT_WORK_TREE", "GIT_INDEX_FILE"):
            self.environment.pop(key, None)
        self.environment["GIT_CONFIG_GLOBAL"] = os.devnull
        self.environment["GIT_CONFIG_NOSYSTEM"] = "1"

    def run(self, command, *, success=True):
        result = subprocess.run(command, capture_output=True, env=self.environment,
                                timeout=60)
        stem = f"command-{len(self.commands):03d}"
        for name, data in (("stdout", result.stdout), ("stderr", result.stderr)):
            (self.output / f"{stem}.{name}").write_bytes(data)
        self.commands.append({"command": [str(value) for value in command],
                              "returncode": result.returncode, "streams": stem})
        if success and result.returncode:
            raise AssertionError(f"{stem} failed: {result.stderr.decode('utf-8', errors='replace')}")
        return result

    def git(self, source, *arguments):
        return self.run(["git", "-C", str(source), *arguments])

    def source(self, name):
        source = self.output / "sources" / name
        source.mkdir(parents=True)
        (source / "cmake").mkdir()
        shutil.copyfile(ROOT / "cmake/SourceVersion.cmake", source / "cmake/SourceVersion.cmake")
        for name in (".git_archival.txt", ".gitattributes"):
            shutil.copyfile(ROOT / name, source / name)
        (source / ".gitignore").write_text("ignored\n", encoding="utf-8")
        (source / "CMakeLists.txt").write_text(
            'cmake_minimum_required(VERSION 3.24)\n'
            'include(cmake/SourceVersion.cmake)\n'
            'meshvale_geometry_resolve_source_version(resolved provenance "${CMAKE_CURRENT_SOURCE_DIR}")\n'
            'project(VersionFixture VERSION "${resolved}" LANGUAGES NONE)\n'
            'file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/identity.txt" "${resolved}|${provenance}|${PROJECT_VERSION}")\n',
            encoding="utf-8")
        return source

    def repository(self, name):
        source = self.source(name)
        self.git(source, "init", "--initial-branch=main")
        self.git(source, "config", "user.name", "Version fixture")
        self.git(source, "config", "user.email", "fixture@example.invalid")
        self.git(source, "config", "core.autocrlf", "false")
        self.commit(source)
        return source

    def commit(self, source):
        self.git(source, "add", ".")
        self.git(source, "commit", "-m", "Original version fixture")

    def case(self, name, source, version="0.0.0", provenance=None, *, failure=None, values=()):
        build = self.output / "builds" / name
        result = self.run([self.cmake, "-S", str(source), "-B", str(build), *values], success=False)
        observation = {"name": name, "source": str(source), "returncode": result.returncode}
        self.cases.append(observation)
        if failure:
            assert result.returncode != 0, f"{name}: expected refusal"
            assert failure in result.stderr.decode("utf-8", errors="replace"), name
        else:
            assert result.returncode == 0, f"{name}: {result.stderr.decode('utf-8', errors='replace')}"
            actual = (build / "identity.txt").read_text(encoding="utf-8")
            observation["identity"] = actual
            fields = actual.split("|")
            assert fields[0] == fields[2] == version, (name, actual)
            if provenance:
                assert fields[1] == provenance, (name, actual)

    def archive(self, source, name, revision="HEAD"):
        data = self.git(source, "archive", "--format=tar", revision).stdout
        (self.output / f"{name}.tar").write_bytes(data)
        destination = self.output / "sources" / name
        destination.mkdir()
        with tarfile.open(fileobj=io.BytesIO(data)) as archive:
            for member in archive.getmembers():
                assert member.isfile() or member.isdir()
                path = destination / member.name
                assert path.resolve().is_relative_to(destination.resolve())
                if member.isfile():
                    path.parent.mkdir(parents=True, exist_ok=True)
                    path.write_bytes(archive.extractfile(member).read())
        return destination

    def metadata(self, source, text):
        (source / "PKG-INFO").write_bytes(text.encode("utf-8"))

    def punctuation_tags(self):
        for punctuation, tag in (("comma", "v1.2.3,extra"), ("semicolon", "v1.2.3;extra"),
                                 ("dollar", "v1.2.3$extra"), ("format-text", "v1.2.3$Format")):
            source = self.repository(f"{punctuation}-tag")
            self.git(source, "tag", tag)
            self.case(f"{punctuation}-git", source, provenance="git-development")
            archive = self.archive(source, f"{punctuation}-archive")
            self.case(f"{punctuation}-archive", archive, provenance="archive-development")
            # Ignoring an unrelated complete token must not hide a real stable tag.
            self.git(source, "tag", "v4.5.6")
            self.case(f"{punctuation}-plus-stable-git", source, "4.5.6", "git-release")
            stable = self.archive(source, f"{punctuation}-plus-stable-archive")
            self.case(f"{punctuation}-plus-stable-archive", stable, "4.5.6", "archive-release")

    def describe_shaped_tags(self):
        for name, version in (("unrelated-only", "0.0.0"), ("stable-coexistence", "4.5.6")):
            source = self.repository(f"describe-shaped-{name}")
            self.git(source, "tag", "-a", "v8-4-gdeadbeef", "-m", "Complete unrelated exact tag")
            if version != "0.0.0":
                self.git(source, "tag", "v4.5.6")
            self.case(f"describe-shaped-{name}-git", source, version)
            archive = self.archive(source, f"describe-shaped-{name}-archive")
            marker = (archive / ".git_archival.txt").read_text(encoding="utf-8")
            assert "describe-name: v8-4-gdeadbeef\n" in marker
            self.case(f"describe-shaped-{name}-archive", archive, version)

    def exercise(self):
        self.punctuation_tags()
        self.describe_shaped_tags()
        raw = self.source("raw space 雪")
        self.case("raw-neutral", raw, provenance="unversioned-development")
        marker = (raw / ".git_archival.txt").read_text(encoding="utf-8")
        (raw / ".git_archival.txt").unlink()
        self.case("raw-no-metadata", raw, provenance="unversioned-development")
        self.case("raw-backend-zero", raw, values=("-DSKBUILD_PROJECT_NAME=Meshvale_Geometry", "-DSKBUILD_PROJECT_VERSION_FULL=0.0.0"))
        self.case("raw-backend-stable", raw, failure="lacks matching source provenance", values=("-DSKBUILD_PROJECT_NAME=meshvale-geometry", "-DSKBUILD_PROJECT_VERSION_FULL=1.2.3"))
        self.case("raw-missing-full", raw, failure="requires FULL", values=("-DSKBUILD_PROJECT_NAME=meshvale-geometry", "-DSKBUILD_PROJECT_VERSION=1.2.3"))
        self.case("raw-unrelated-name", raw, values=("-DSKBUILD_PROJECT_NAME=other", "-DSKBUILD_PROJECT_VERSION_FULL=garbage"))
        self.case("raw-absent-name", raw, values=("-DSKBUILD_PROJECT_VERSION_FULL=1.2.3",))
        for index, version in enumerate(("1.2.3.dev4+gabc.d20260101", "1.2.3+local-tag", "01.02.03.DEV004+A_b")):
            self.case(f"raw-dev-{index}", raw, values=("-DSKBUILD_PROJECT_NAME=meshvale.geometry", f"-DSKBUILD_PROJECT_VERSION_FULL={version}", "-DSKBUILD_PROJECT_VERSION=99.8.7"))
        for index, version in enumerate(("1.2.3rc1", "1.2.3.post1", "1!1.2.3", "1.2.3.4", "garbage", "1.2.3+", "")):
            self.case(f"raw-unsupported-{index}", raw, failure="unsupported or malformed", values=("-DSKBUILD_PROJECT_NAME=meshvale-geometry", f"-DSKBUILD_PROJECT_VERSION_FULL={version}"))
        for index, version in enumerate(("01.002.3", "1.2.4.dev5+gabc", "1.2.3+local", "0.0.0")):
            self.metadata(raw, f"Metadata-Version: 2.4\r\nName: Meshvale__Geometry\r\nVersion: {version}\r\nOther: x\r\n folded\r\n\r\nName: wrong\r\nVersion: garbage\r\n")
            self.case(f"pkg-valid-{index}", raw, version="1.2.3" if index == 0 else "0.0.0")
        for index, text in enumerate((
                "Name: other\nVersion: 1.2.3\n\n", "Name: meshvale-geometry\nVersion: 1.2.3\nVersion: 1.2.3\n\n",
                "Name: meshvale-geometry\nVersion: 1.2.3\n folded\n\n", "Name: meshvale-geometry\n\nVersion: 1.2.3\n",
                "Name: meshvale-geometry\nVersion: \n\n", "Name: meshvale-geometry\nName: meshvale-geometry\nVersion: 1.2.3\n\n")):
            self.metadata(raw, text)
            self.case(f"pkg-refused-{index}", raw, failure="PKG-INFO")
        (raw / "PKG-INFO").unlink()
        (raw / ".git_archival.txt").write_text(marker, encoding="utf-8")
        self.metadata(raw, "Name: meshvale-geometry\nVersion: 4.5.6\n\n")
        self.case("template-plus-stable-sdist", raw, version="4.5.6")
        (raw / "PKG-INFO").unlink()
        for index, bad in enumerate((marker.replace("%H", "%h"), marker.replace("meshvale-geometry", "other"), marker + "node: duplicate\n", marker.replace("$Format:%H$", "a" * 40))):
            (raw / ".git_archival.txt").write_text(bad, encoding="utf-8")
            self.case(f"marker-refused-{index}", raw, failure="MeshvaleGeometry:")
        (raw / ".git_archival.txt").write_text(marker, encoding="utf-8")
        self.metadata(raw, "Name: meshvale-geometry\nVersion: 7.8.9\n\n")
        (raw / ".git").write_text("unusable fixture marker\n", encoding="utf-8")
        self.case("unusable-root-marker", raw, provenance="git-unverified-development")
        self.case("missing-git-root-marker", raw, provenance="git-unverified-development", values=("-DCMAKE_DISABLE_FIND_PACKAGE_Git=ON",))
        (raw / ".git").unlink()
        (raw / "PKG-INFO").unlink()

        repo = self.repository("own-repository")
        self.case("own-no-tags", repo, provenance="git-development")
        self.git(repo, "tag", "v01.2.3")
        self.git(repo, "tag", "v1.2.3")
        self.git(repo, "tag", "-a", "release42", "-m", "Unrelated numeric exact tag")
        self.case("canonical-exact-aliases", repo, "1.2.3", "git-release")
        self.case("release-full", repo, "1.2.3", values=("-DSKBUILD_PROJECT_NAME=meshvale-geometry", "-DSKBUILD_PROJECT_VERSION_FULL=01.2.3", "-DSKBUILD_PROJECT_VERSION=99.9.9"))
        self.case("release-full-conflict", repo, failure="FULL release conflict", values=("-DSKBUILD_PROJECT_NAME=meshvale-geometry", "-DSKBUILD_PROJECT_VERSION_FULL=1.2.4.dev1"))
        (repo / "ignored").write_text("ignored\n", encoding="utf-8")
        self.case("ignored-data-clean", repo, "1.2.3")
        (repo / "untracked").write_text("untracked\n", encoding="utf-8")
        self.case("untracked-development", repo, provenance="git-development")
        self.case("untracked-stable-full-conflict", repo, failure="lacks matching source provenance", values=("-DSKBUILD_PROJECT_NAME=meshvale-geometry", "-DSKBUILD_PROJECT_VERSION_FULL=1.2.3"))
        (repo / "untracked").unlink()
        archive = self.archive(repo, "tagged-archive")
        self.case("real-tagged-archive", archive, "1.2.3", "archive-release")
        self.metadata(archive, "Name: meshvale-geometry\nVersion: 1.2.3\n\n")
        self.case("archive-sdist-agree", archive, "1.2.3")
        self.metadata(archive, "Name: meshvale-geometry\nVersion: 0.0.0\n\n")
        self.case("archive-sdist-conflict", archive, failure="release conflict")
        (archive / "PKG-INFO").unlink()
        actual_marker = (archive / ".git_archival.txt").read_text(encoding="utf-8")
        node = next(line.removeprefix("node: ") for line in actual_marker.splitlines() if line.startswith("node: "))
        for name, description, failure in (
                ("unrelated-exact", "v9.8.7rc1", None),
                ("nonexact-stable-refs", f"v9.8.7rc1-1-g{node[:12]}", "distance/ref conflict")):
            changed_marker = "\n".join(
                f"describe-name: {description}" if line.startswith("describe-name: ") else line
                for line in actual_marker.splitlines()) + "\n"
            (archive / ".git_archival.txt").write_text(changed_marker, encoding="utf-8")
            self.case(f"archive-{name}-describe", archive, "1.2.3", failure=failure)
        (archive / ".git_archival.txt").write_text(actual_marker.replace("describe-name: v1.2.3", "describe-name: v9.8.7").replace("describe-name: v01.2.3", "describe-name: v9.8.7"), encoding="utf-8")
        self.case("archive-describe-conflict", archive, failure="describe/ref conflict")
        (archive / ".git_archival.txt").write_text(actual_marker, encoding="utf-8")
        tree = self.git(repo, "rev-parse", "HEAD^{tree}").stdout.decode().strip()
        unexpanded = self.archive(repo, "raw-tree-archive", tree)
        self.case("real-tree-neutral", unexpanded, provenance="unversioned-development")
        self.git(repo, "tag", "v2.0.0")
        self.case("ambiguous-exact-tags", repo, failure="ambiguous exact stable")
        self.git(repo, "tag", "-d", "v2.0.0")
        (repo / "tracked").write_text("development\n", encoding="utf-8")
        self.commit(repo)
        self.case("recognized-ancestor-development", repo)
        development = self.archive(repo, "development-archive")
        self.case("real-development-archive", development, provenance="archive-development")
        self.metadata(development, "Name: meshvale-geometry\nVersion: 1.2.4.dev1+gabc\n\n")
        self.case("development-sdist", development)
        self.metadata(development, "Name: meshvale-geometry\nVersion: 1.2.3\n\n")
        self.case("development-stable-conflict", development, failure="development archive/stable")
        self.metadata(repo, "Name: other\nVersion: garbage\n\n")
        self.commit(repo)
        self.git(repo, "tag", "v3.4.5")
        self.case("live-git-ignores-stale-pkg", repo, "3.4.5")
        child = repo / "nested"
        shutil.copytree(unexpanded, child)
        self.metadata(child, "Name: meshvale-geometry\nVersion: 5.6.7\n\n")
        self.case("ancestor-git-ignored", child, "5.6.7")
        worktree = self.output / "sources" / "proper-worktree"
        self.git(repo, "worktree", "add", "--detach", str(worktree), "HEAD")
        self.case("proper-worktree", worktree, "3.4.5")
        plain = self.repository("no-tag-repository")
        no_tag = self.archive(plain, "no-tag-archive")
        self.case("real-node-only-native-development", no_tag, provenance="archive-development")
        self.case("missing-git-neutral", unexpanded, provenance="unversioned-development", values=("-DCMAKE_DISABLE_FIND_PACKAGE_Git=ON",))

        sibling = self.output / "sibling-driver"
        sibling.mkdir()
        (sibling / "CMakeLists.txt").write_text(
            'cmake_minimum_required(VERSION 3.24)\nproject(Outer VERSION 8.8.8 LANGUAGES NONE)\n'
            f'add_subdirectory("{unexpanded.as_posix()}" child)\n'
            'if(NOT PROJECT_VERSION STREQUAL "8.8.8" OR DEFINED resolved OR DEFINED provenance)\n'
            ' message(FATAL_ERROR "child leaked source state")\nendif()\n', encoding="utf-8")
        self.run([self.cmake, "-S", str(sibling), "-B", str(self.output / "sibling-build"), "-DSKBUILD_PROJECT_NAME=outer", "-DSKBUILD_PROJECT_VERSION_FULL=garbage"])
        self.cases.append({"name": "add-subdirectory-isolation", "returncode": 0})


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cmake", default="cmake")
    parser.add_argument("--output", type=Path)
    arguments = parser.parse_args()
    temporary = None
    if arguments.output:
        output = arguments.output.resolve()
        output.mkdir(parents=True, exist_ok=False)
    else:
        temporary = tempfile.TemporaryDirectory(prefix="meshvale-source-version-")
        output = Path(temporary.name)
    probe = Probe(output, arguments.cmake)
    error = None
    try:
        probe.exercise()
    except Exception as failure:
        error = repr(failure)
        raise
    finally:
        inputs = {name: hashlib.sha256((ROOT / name).read_bytes()).hexdigest()
                  for name in ("cmake/SourceVersion.cmake", ".git_archival.txt", ".gitattributes", "scripts/check-source-version.py")}
        (output / "receipt.json").write_text(json.dumps({"passed": error is None, "error": error,
            "cases": probe.cases, "commands": probe.commands, "source_hashes": inputs,
            "scope": "Native metadata configuration only; no compiled release or Python backend acceptance"}, indent=2) + "\n", encoding="utf-8")
        if temporary:
            temporary.cleanup()
    print(f"Source version checks passed: {len(probe.cases)} native metadata cases; no backend/build claim")


if __name__ == "__main__":
    main()
