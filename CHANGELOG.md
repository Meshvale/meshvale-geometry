# Changelog

## Unreleased

### Added

- Raw variable-length polygon storage with non-mutating structural diagnostics.
- Named typed vertex, face and corner attributes, including multiple UV sets, missing values and variable-length skin influence data.
- Owned polygon incidence snapshots, non-manifold edge/vertex-fan diagnostics, existing winding checks, face/boundary components and explicit inspection coverage.
- An installable C++20 CMake target and a separate installed-consumer example.
- Optional Python immutable mesh snapshots with owned read-only buffer exchange, extensible attribute records, native storage/topology inspection and source/wheel packaging.

These additions are development interfaces. Format adapters, geometry repair and stable compatibility are forthcoming.
