# meshvale-geometry

A C++20 library for polygon mesh geometry and attributes.

**Status:** Development C++20 library with raw polygon storage, extensible attribute channels, scoped topology inspection and an installed CMake target. Python bindings, further geometry operations, format adapters, stable interfaces and release packages are forthcoming.

See [the storage contract and build instructions](docs/attributes.md), [topology inspection contract](docs/topology.md), [installed consumer](examples/consumer/CMakeLists.txt), and [changelog](CHANGELOG.md).

Topology inspection retains every edge occurrence, detects disconnected vertex fans, reports existing winding conflicts and classifies boundary graphs. It returns an owned snapshot and explicit check coverage, including blocked malformed topology and unsupported geometric/solid checks. Non-manifold input is retained; inspection does not normalize or repair it.

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
