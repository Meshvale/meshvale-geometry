# Local development environment

This file owns the boundary between portable configuration and machine settings.

Copy [environment.example.json](environment.example.json) to `.local/environment.json` and fill only the fields needed for your task. Keep local paths, credentials, source assets, and raw logs in ignored storage. The template describes configuration inputs; no automatic loader or native build integration is implemented yet.

`workspace_root` is the optional local checkout/workspace location. `vcpkg_root` is an optional existing vcpkg installation. Compiler, generator, triplet, and corpus fields remain unset until used. Relative build paths resolve from this repository. The native targets require CMake 3.24+, a C++20 compiler and the pinned Eigen build dependency below. See [native build/test/install instructions](docs/attributes.md#build-and-installed-consumer). Local tool paths and installed prefixes remain in ignored settings and logs.

Native builds use Eigen 3.4.1 as a private header-only implementation dependency.
CMake uses an exact installed CONFIG package or downloads the hash-pinned official
archive; offline builds can set `Eigen3_DIR` and disable
`MESHVALE_GEOMETRY_FETCH_EIGEN`. Installed native consumers require the compiled
Geometry library and its existing thread dependency, without Eigen headers.
See [numerical dependency setup](docs/numerics.md#dependency-and-installation).

The current checks need Git and Python 3.10 or newer, with no third-party Python packages:

Optional binding builds use ordinary GIL-enabled CPython and the pinned build dependencies in [pyproject.toml](pyproject.toml). See [Python build/installation and ownership requirements](docs/python.md). These dependencies are not needed for native-only builds or the repository checks below. Keep virtual environments and candidate archives under ignored `.local/`.

```sh
python scripts/check-portability.py
python scripts/check-docs.py
```

For C++ formatting, use an isolated tool environment with `clang-format==23.1.3`, then run `python scripts/check-cpp-format.py`. The check covers Git-visible project `.h`, `.hpp` and `.cpp` files and uses the repository's Google/C++20 configuration. Pass `--formatter` when the executable is outside the current PATH; machine-specific locations stay in ignored configuration. Formatting is checked independently of semantic/native tests and remaining public naming migration.

Public files use repository-relative links, public URLs, tool names, and symbolic environment values. Run these checks before committing or publishing. The portability check scans working files and staged content for machine paths and private local files; it does not scan all secrets or determine whether a document should be public. Review public content and package contents separately.
