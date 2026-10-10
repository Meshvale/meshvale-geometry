# Working in this repository

Read [README.md](README.md) for this product's scope and current status. Follow the implementing interface/specification when one exists; keep behavior in its owning source or specification and link documentation to that owner.

Before changing C++ code, configuring a native build, or reviewing C++, read [MESHVALE-CPP-001 v0.2.0](https://github.com/Meshvale/.github/blob/542b03450a3e4f376ddbbaea39f1b17badd0a6f6/docs/cpp-development.md). That public contract owns shared C++20 development practice and Google style adaptations; this repository's `.clang-format` projects its formatting policy.

For tool discovery or build configuration, read [ENVIRONMENT.md](ENVIRONMENT.md). Actual environment values belong in ignored `.local/` configuration. Public documentation and build instructions must be usable from this repository with its documented public dependencies.

Keep changes focused on this product. Describe validation and limitations; add tests when behavior changes. Keep product instructions and necessary technical contracts public, and retain local notes, raw logs, and unrelated planning in private storage. Use the selected Apache-2.0 license for original contributions and retain third-party notices.

Before a commit or export, run the two checks documented in ENVIRONMENT.md and inspect the staged file list. Add implementation/build requirements when they exist rather than creating empty scaffolds or claiming unimplemented features.
