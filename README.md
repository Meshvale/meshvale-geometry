# meshvale-geometry

A C++20 library for polygon mesh geometry and attributes.

Canonical `.h` headers declare the native interface; compiled static CMake targets
provide storage, topology, editing and conversion implementations. Include paths
ending in `.hpp` have been removed. Python record consumers explicitly compile
the installed adapter source for their own extension domain; native builds do
not require Python.

**Status:** Development C++20 library with raw polygon storage, extensible attributes, scoped topology inspection, a native pooled editor, exact native polygon triangulation, installed CMake targets, optional Python snapshots and a declarative workflow report contract. Triangulation bindings, further operations, format adapters, stable interfaces and released packages are forthcoming.

See [the storage contract and native build instructions](docs/attributes.md), [topology inspection contract](docs/topology.md), [Python buffer records and installation](docs/python.md), [report envelopes and reference validation](docs/reports.md), [installed native consumer](examples/consumer/CMakeLists.txt), and [changelog](CHANGELOG.md).

Topology inspection retains every edge occurrence, detects disconnected vertex fans, reports existing winding conflicts and classifies boundary graphs. It returns an owned snapshot and explicit check coverage, including blocked malformed topology and unsupported geometric/solid checks. Non-manifold input is retained; inspection does not normalize or repair it.

The [native pooled editor](docs/editing.md) provides checked vertex, edge, face
and corner objects, transactional edits, revisioned snapshots and flexible
properties. It retains wire/parallel edges and non-manifold incidence. CPU bounds
computation uses immutable snapshots and a shared worker budget. See the
[installed editing example](examples/editing-consumer/CMakeLists.txt) for the
separate `meshvale::editing` target. Editing bindings and stable compatibility
are forthcoming.

[Shared execution leases](docs/execution.md) let native clients retain explicit
payload charges and worker slots under that same context budget. The
[installed lease example](examples/execution-consumer/CMakeLists.txt) shows
context lifetime and joined workers with nested caller fallback.

The [native triangulation contract](docs/triangulation.md) describes explicit
conversion of simple exactly planar polygon faces, authored face/corner maps,
channel backing and shared execution. Approximate-planarity profiles and Python
conversion bindings are forthcoming.

## Planned capabilities

- Triangles, quads, n-gons, and mixed polygon meshes.
- Mesh and attribute interfaces, including multiple UV maps, skin influence channels and face-corner values.
- Topology queries, invariant checks, and shared geometry operations.
- Declared input requirements and reported topology changes.

Supported formats, operation guarantees, and platform compatibility will be documented and tested with each implementation and release.

## Development

Read [ENVIRONMENT.md](ENVIRONMENT.md) for portable configuration and repository checks, and [the storage documentation](docs/attributes.md#build-and-installed-consumer) for native build/test/install instructions. [AGENTS.md](AGENTS.md) provides focused instructions for work in this repository.

## Contributing

Use this repository's issues for reproducible problems and feature requests. Follow the public [contribution guide](https://github.com/Meshvale/.github/blob/main/CONTRIBUTING.md) and include how your change was validated. Share only assets you have permission to redistribute.

## License

Original material is licensed under [Apache-2.0](LICENSE). See [NOTICE](NOTICE) for attribution; third-party material retains its own terms.
