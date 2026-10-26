# Changelog

- Canonical attributes now own private Eigen vectors for all seven scalar
  encodings. Replace vector alternatives with `ScalarBuffer<T>` and rebuild
  native/per-extension consumers; raw dense/ragged shapes, bits, presence and
  Python mesh/1 records remain unchanged. See [numerical ownership](docs/numerics.md#canonical-attribute-ownership).

## Unreleased

- Add a development source candidate for charged immutable native triangulation,
  complete typed channels/maps and actual per-operation request accounting.
  Native/package/installed/CI qualification remains pending; legacy behavior and
  Python/destination policies remain unchanged.

### Documentation

- Document the implemented [source-version profile](docs/versioning.md) and its
  compiler-free metadata checks. Release/runtime qualification remains separate.

### Fixed

- Resolve the CMake project and installed CMake package version from one
  checked source identity, including tagged archives and source packages.
  Cross-check a matching Python build's full version against that identity.

### Changed

- Generalize fixed-width numerical storage to `VectorBuffer<T, N>` for float/double
  rows of width 2, 3 and 4. `PositionBuffer` is now its double xyz alias; existing
  row calls and include paths work, but native type identity/symbols require a
  coordinated rebuild. Common private Eigen aliases depend only on Eigen.

- Raw Mesh positions use an Eigen 3.4.1 row-major owning `PositionBuffer` with
  explicit Get/Set/Append row values and strong copy/growth rollback. This changes
  C++ source/ABI shapes; polygon channels, canonical mesh/1 records and Python
  owned snapshot semantics retain their contracts.

- Remove legacy `.hpp` forwarding paths; canonical `.h` headers declare storage
  and topology operations implemented in compiled static CMake targets.
- Provide an opt-in installed Python record implementation-source factory for
  each extension's nanobind domain, retaining owned buffer exchange.
- Project C++ is formatted with the Google configuration and a pinned formatter check in CI; public naming compatibility is retained separately.

### Added

- `MaterializedMesh::ShareExecutionContext()` shares the retained allocation and
  worker budget with downstream operations, without allocating or retaining mesh
  payloads or an earlier cancellation token. Rebuild the compiled materialization
  target and native consumers to use the new member.

- Immutable full-storage capture, raw inspection and face selection through
  `materialization.h` and `meshvale::materialization`, with complete typed
  attributes, ordered metadata, shared allocation/worker admission and retained
  face/corner maps. Nonempty numerical capture currently admits only the
  documented [Windows/MSVC allocation profile](docs/materialization.md).

- Checked native attribute row sums and squared norms through
  `attribute_numerics.h` and `meshvale::attribute_numerics`, retaining all seven
  scalar encodings, dense/ragged rows and authored/missing distinctions. Owned
  results and immutable captures admit complete allocation blocks under shared
  execution budgets; integer intermediates are checked and workers are joined.

- Move-only native payload and worker leases through `execution.h` and
  `meshvale::editing`, sharing existing context caps with bounds/triangulation.
  Explicit charges survive caller-context destruction; failed growth preserves
  prior charges and cancellation never blocks shrink or cleanup.

- Python `triangulate` for canonical immutable snapshots, frozen outcomes with
  readonly owned uint64 correspondence, shared native execution contexts and
  thread-requested cancellation during GIL-released native computation.

- Native exact represented-planar polygon triangulation through `triangulation.h` and `meshvale::triangulation`, with complete Mesh/face/corner correspondence, bit-preserving channel transfer, shared CPU worker/payload admission and cooperative cancellation. The single-loop profile supports up to 4096 corners per face; approximate planarity is forthcoming.

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
