# Changelog

## Unreleased

### Changed

- Canonical C++ headers use `.h`; existing `.hpp` includes remain installed forwarding headers with the same definitions and API names.
- Project C++ is formatted with the Google configuration and a pinned formatter check in CI; public naming compatibility is retained separately.

### Added

- Native exact represented-planar polygon triangulation through `triangulation.h` and `meshvale::triangulation`, with complete Mesh/face/corner correspondence, bit-preserving channel transfer, shared CPU worker/payload admission and cooperative cancellation. The single-loop profile supports up to 4096 corners per face; approximate planarity and Python conversion bindings are forthcoming.

- A manually dispatched portable packaging pilot for ordinary CPython 3.13 on Linux x86_64 (glibc 2.28) and Windows x64, with repaired-wheel tests and independently rebuilt source archives retained as development CI artifacts.

- Native pooled editing through `editable_mesh.h` and the compiled `meshvale::editing` target: typed checked element/property objects, transactional creation/deletion, explicit non-manifold/wire/parallel-edge incidence, owned revisioned snapshots, forks and dense correspondence.
- Deterministic CPU bounds computation with a shared worker budget, serial reasons and cancellation; editing properties cover all seven scalar types, multiple UV sets, ragged influences and explicit missing/default/required-row policies.
- Raw variable-length polygon storage with non-mutating structural diagnostics.
- Named typed vertex, face and corner attributes, including multiple UV sets, missing values and variable-length skin influence data.
- Owned polygon incidence snapshots, non-manifold edge/vertex-fan diagnostics, existing winding checks, face/boundary components and explicit inspection coverage.
- An installable C++20 CMake target and a separate installed-consumer example.
- Optional Python immutable mesh snapshots with owned read-only buffer exchange, extensible attribute records, native storage/topology inspection and source/wheel packaging.
- Versioned declarative asset/batch report envelopes, snapshot-scoped correspondence including one-to-many polygon conversion, optional Python reference validation, required-coverage evaluation and exit precedence. Actual workflow report producers remain forthcoming.

These additions are development interfaces. Format adapters, geometry repair and stable compatibility are forthcoming.
