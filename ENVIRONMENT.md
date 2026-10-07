# Local development environment

This file owns the boundary between portable configuration and machine settings.

Copy [environment.example.json](environment.example.json) to `.local/environment.json` and fill only the fields needed for your task. Keep local paths, credentials, source assets, and raw logs in ignored storage. The template describes configuration inputs; no automatic loader or native build integration is implemented yet.

`workspace_root` is the optional local checkout/workspace location. `vcpkg_root` is an optional existing vcpkg installation. Compiler, generator, triplet, and corpus fields remain unset until used. Relative build paths resolve from this repository. The initial native target requires CMake 3.24+ and a C++20 compiler, with no native third-party dependencies. See [native build/test/install instructions](docs/attributes.md#build-and-installed-consumer). Local tool paths and installed prefixes remain in ignored settings and logs.

The current checks need Git and Python 3.10 or newer, with no third-party Python packages:

```sh
python scripts/check-portability.py
python scripts/check-docs.py
```

Public files use repository-relative links, public URLs, tool names, and symbolic environment values. Run these checks before committing or publishing. The portability check scans working files and staged content for machine paths and private local files; it does not scan all secrets or determine whether a document should be public. Review public content and package contents separately.
