# Python mesh snapshots and buffer records

| Field | Value |
|---|---|
| ID | GEO-PYTHON-001 |
| Version | 0.1.0 |
| Status | Development interface; no stable release |
| Owner | Python snapshot ownership, buffer exchange and package consumption |

## Ownership and inspection

`meshvale_geometry.Mesh()` constructs an empty immutable snapshot. `Mesh.from_record(record)` copies a complete record into native storage. `mesh.to_record()` copies native storage into a new dictionary whose numeric buffers are read-only memoryviews backed by Python-owned immutable bytes. Each import and export copies numeric payload once; this is not zero-copy exchange. Metadata and containers also have independent ownership. No buffer borrows a vector, and no mutable native mesh is exposed. Source mutation, buffer release, mesh destruction and later exports cannot alter an existing snapshot or exported buffer. Replacing dictionary entries changes that record alone. Concurrent mutation of an input buffer during import is outside this contract.

`vertex_count`, `face_count` and `corner_count` describe stored rows. `inspect_storage()` returns diagnostic dictionaries with `code`, `subject` and nullable `element`. `inspect_topology()` returns `checks`, `diagnostics` and nullable `topology`. Each check has `name`, `status` (`performed`, `blocked`, `unsupported`) and `reason`. A present topology dictionary contains copied `corners`, `edges`, `vertices`, `face_components` and `boundaries` matching the [native topology contract](topology.md). These returned dictionaries/lists may be edited without changing the mesh. They are inspection data, not the shared workflow report schema or an editing interface.

Import validates the record representation, not mesh validity: bad face offsets, out-of-range vertex references, nonfinite positions and misaligned attribute rows remain available for [storage inspection](attributes.md). A representation error raises `TypeError`, `ValueError` or `OverflowError`, with no partially published mesh. A released memoryview raises `ValueError`. Native allocation failure raises `MemoryError`. Topology coverage and defects retain their native meaning; no global clean/valid flag is added.

## GEO-PYTHON-002: Record version and shape

The record is an exact dictionary with these keys; missing, extra or non-string keys are rejected. `schema` is exactly `"meshvale.mesh/1"`. This is a process-local buffer protocol, not a portable file serialization or C++ ABI. Extensions exchange these Python containers/buffers, not native pointers, handles or duplicated registered `Mesh` types.

| Key | Representation |
|---|---|
| `schema` | Version string above |
| `positions` | Flat one-dimensional native-endian float64 buffer; xyz triples, including an empty buffer |
| `face_offsets` | Flat native-endian uint64 buffer |
| `corner_vertices` | Flat native-endian uint64 buffer |
| `attributes` | List of exact attribute dictionaries, in source order |

Every numeric input must support Python's buffer protocol, be one-dimensional and C-contiguous, and have a matching scalar format and byte width. No numeric casting, endian conversion, flattening, missing-value inference or index truncation is performed. A multidimensional or strided array must be explicitly reshaped/copied by its caller. Read-only and writable source buffers are accepted, but neither is retained. Indirect buffers and explicitly foreign-endian formats are rejected. Native PEP 3118 scalar formats may have `@` or `=` prefixes; uint64 may use `Q` or an eight-byte `L`, and int32/uint32 may use four-byte `i`/`I` or `l`/`L`. Empty buffers obey the same dtype/shape rules. Export uses native scalar formats `f`, `d`, `i`, `B`, `H`, `I`, `Q` with the corresponding widths.

Every attribute has exactly these fields: `domain` (`vertex`, `face`, `corner`), `name`, `semantic`, nullable uint32 `set_index`, uint32 `components`, `scalar_type`, `values`, nullable uint64 `offsets`, nullable uint8 `present`, and string-to-string dictionary `metadata`. Boolean integers and integers outside uint32 range are rejected for `components`/`set_index`; a zero component count remains a storage diagnostic. `scalar_type` is one of `float32`, `float64`, `int32`, `uint8`, `uint16`, `uint32`, `uint64`, and must match the `values` buffer. Dense/ragged rows, offsets measured in scalars, presence masks and attribute identity retain the [storage contract](attributes.md). No UV-set or joint-influence count is imposed. Edge channels and scene/skin binding are outside this record version.

## Build, install and try

The distribution is `meshvale-geometry`; the import is `meshvale_geometry`. Building requires C++20, CMake 3.24+, ordinary GIL-enabled CPython 3.10+ and the pinned build dependencies in [pyproject.toml](../pyproject.toml). Native-only CMake builds remain independent of Python/nanobind. Builds use nanobind with a private static runtime and an explicit record seam; no cross-extension C++ type exchange is promised. Wheels are specific to the building Python ABI/platform; free-threaded builds, stable-ABI wheels and a broad release matrix are not supported by this experiment. No runtime NumPy dependency is required.

```sh
python -m pip wheel . --no-deps --wheel-dir .local/wheels
python -m pip install --no-index --find-links .local/wheels meshvale-geometry
python -m unittest discover -s tests/python -v
python examples/python/inspect_mesh.py
```

Use a fresh virtual environment for installed-consumer tests. An installed wheel contains the Python package/extension and required license notices, not native CMake headers/configuration, tests, private files or build logs. Source distributions contain an explicit product build/test/documentation file list and must rebuild independently of Git or this checkout. Exact `vX.Y.Z` tags own released package versions; untagged builds carry a development version from setuptools-scm. The generated version module retains the source version when building from a source distribution.

```python
from array import array
from meshvale_geometry import Mesh

mesh = Mesh.from_record({
    "schema": "meshvale.mesh/1",
    "positions": array("d", [0, 0, 0, 1, 0, 0, 0, 1, 0]),
    "face_offsets": array("Q", [0, 3]),
    "corner_vertices": array("Q", [0, 1, 2]),
    "attributes": [],
})
assert mesh.face_count == 1
assert mesh.to_record()["positions"].readonly
```

The buffer lifetime requirements follow [Python's buffer protocol](https://docs.python.org/3/c-api/buffer.html). Build configuration follows [nanobind packaging](https://nanobind.readthedocs.io/en/latest/packaging.html) and [scikit-build-core version metadata](https://scikit-build-core.readthedocs.io/en/stable/configuration/dynamic.html). Focused acceptance tests cover every scalar kind, multiple UV sets, ragged joint/weight rows, explicit missingness, strict incompatible buffers, malformed mesh inspection, source/export lifetime independence and installed package contents. Compatibility with real independently installed Interchange/Repair Python packages remains to be proved separately.

The [independent record consumer](../examples/record-consumer/CMakeLists.txt) compiles against installed native headers, in a separate nanobind domain, without importing or registering Geometry's Python `Mesh` type. Its [installed check](../examples/record-consumer/check.py) exercises round-trip buffers, native inspection, source destruction and both import orders in separate processes. It is a development consumer of the `0.0.0` native snapshot package, not a released application or proof of the complete processing workflow. The [fresh-environment runner](../scripts/test-installed.py) installs both candidate wheels outside the checkout; [archive checks](../scripts/check-package.py) inspect Geometry package contents. CI evaluates CPython 3.10/3.14 on hosted Windows/Linux/macOS; this is a test matrix, not a broad release support promise.
