# Python mesh snapshots and buffer records

| Field | Value |
|---|---|
| ID | GEO-PYTHON-001 |
| Version | 0.2.0 |
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

The buffer lifetime requirements follow [Python's buffer protocol](https://docs.python.org/3/c-api/buffer.html). Build configuration follows [nanobind packaging](https://nanobind.readthedocs.io/en/latest/packaging.html) and [scikit-build-core version metadata](https://scikit-build-core.readthedocs.io/en/stable/configuration/dynamic.html). Focused acceptance tests cover every scalar kind, multiple UV sets, ragged joint/weight rows, explicit missingness, strict incompatible buffers, malformed mesh inspection, source/export lifetime independence and installed package contents. Independently installed development combinations are also exercised by the product-owned [Interchange runner](https://github.com/Meshvale/meshvale-interchange/blob/d8459f802a425687108298b3167182a62dbfd8ce/scripts/test-installed.py) and [Repair runner](https://github.com/Meshvale/meshvale-repair/blob/f01bb4ae2ab7b9802a1e9f39445140c930087d1a/scripts/test-installed.py), including import-order checks. These pinned combinations do not establish compatibility with arbitrary versions or a supported release matrix.

The [independent record consumer](../examples/record-consumer/CMakeLists.txt) compiles against installed native headers, in a separate nanobind domain, without importing or registering Geometry's Python `Mesh` type. Its [installed check](../examples/record-consumer/check.py) exercises round-trip buffers, native inspection, source destruction and both import orders in separate processes. It is a development consumer of the `0.0.0` native snapshot package, not a released application or proof of the complete processing workflow. The [fresh-environment runner](../scripts/test-installed.py) installs both candidate wheels outside the checkout; [archive checks](../scripts/check-package.py) inspect Geometry package contents. CI evaluates CPython 3.10/3.14 on hosted Windows/Linux/macOS; this is a test matrix, not a broad release support promise.

### Compiled record adapter for another extension

`python/record.h` declares record operations and retains a small `write_buffer`
template forwarding to checked compiled buffer construction. Readers, buffer
ownership and detailed conversion live in `record.cpp`. Calls require the GIL;
all imported/exported payload remains independently owned as described above.

The native installation supplies an implementation source and the
`meshvale_geometry_add_python_record` CMake factory. Package discovery includes
the factory without finding Python or nanobind. A consumer opts in after finding
ordinary GIL-enabled Python and nanobind, then creates an `NB_STATIC` extension
and a record target with the same explicit domain:

```cmake
find_package(MeshvaleGeometry CONFIG REQUIRED)
find_package(Python 3.10 REQUIRED COMPONENTS Interpreter Development.Module)
# Locate nanobind using the consumer's Python environment as in the example.
find_package(nanobind 3.1 CONFIG REQUIRED)
nanobind_add_module(my_extension NB_STATIC NB_DOMAIN my_extension bindings.cpp)
meshvale_geometry_add_python_record(my_records DOMAIN my_extension)
target_link_libraries(my_extension PRIVATE my_records)
```

The factory creates a static PIC library from the installed source, links the
compiled Geometry target and that build's `nanobind-static`, and applies C++20,
warnings as errors and matching `NB_DOMAIN`. Each extension builds its own
adapter. This is an implementation-source factory, not a precompiled Python ABI
export. Use the same interpreter, compiler, runtime mode and domain for both
targets. Ordinary `NB_STATIC` builds are the supported arrangement; shared,
split, stable-ABI and free-threaded variants require separate evidence. Legacy
`.hpp` includes have been removed; use `python/record.h` and link the record
implementation target rather than relying on header-only definitions.

## Python polygon conversion

`triangulate(mesh, *, options=None, execution=None, cancellation=None)` accepts
the canonical immutable `Mesh` snapshot. It invokes the [native exact profile](triangulation.md)
without rewriting the source: single simple exactly planar binary64 loops,
convex/concave faces in arbitrary 3D planes and both windings, up to 4096 corners
per face. Unsupported geometry and malformed mesh storage return native blocked
diagnostics; there is no fan fallback or implicit repair.

`TriangulationOptions(max_corners_per_face=4096, minimum_parallel_faces=64)` is a
frozen value. Its fields require built-in integers; booleans, floats, strings and
integer subclasses raise `TypeError`, negatives raise `ValueError`, and values
outside the native uint64/size_t range raise `OverflowError`. The corner limit
must be 3 through 4096 or raises `ValueError`. These configuration errors are
distinct from blocked mesh outcomes. Limits below a source face's corner count
produce `conversion.face_corner_limit`; zero parallel threshold is permitted.

`ExecutionContext` takes keyword-only `worker_budget=0`,
`minimum_parallel_vertices=65536`, and `tracked_payload_budget_bytes=268435456`,
with the same strict nonnegative size_t validation. A zero worker budget selects
hardware concurrency with at least one worker. Passing the same context, or a
`copy.copy` of it, shares native worker/payload admission across concurrent calls.
None creates a fresh context for that call. Readonly properties expose
`worker_budget`, `active_workers`, `peak_workers`, `tracked_payload_budget_bytes`,
`active_tracked_payload_bytes` and `peak_tracked_payload_bytes`. Peaks are lifetime
reservation peaks of that shared context. External calling Python threads are
outside the library-owned worker count.

`Cancellation()` owns a native stop source. `request_stop()` returns true on the
first request and false after a previous request; `stop_requested` is readonly.
A stopped source remains stopped and may be reused to pre-cancel later calls.
Use a new source for another uncanceled operation. A synchronous conversion
releases the GIL only while executing native code, so another Python thread can
inspect active context metrics and request stop. No Python callbacks run on
native workers. Cancellation joins workers and returns no partial success.
Native allocation/thread-creation exceptions retain their exception behavior.

The frozen `TriangulationResult` contains `status` (`accepted`, `blocked` or
`canceled`), `candidate` (canonical `Mesh` or None), a tuple of frozen
`Diagnostic(code, subject, element)` values, `face_sources`, `corner_sources`,
`face_output_offsets`, `workers_used`, `serial_reason`, and
`peak_tracked_payload_bytes`. Correspondence maps are readonly native-format
uint64 memoryviews backed by independently owned immutable Python bytes; they
retain exact indices without conversion through float. Their directions/ranges
match the native contract. Blocked/canceled outcomes have no candidate and all
three maps empty. Candidate, diagnostics and retained views survive destruction
of the source, context, cancellation and result object. Returning a candidate
does not reconstruct authored polygon topology.

The native tracked budget covers declared reservations during computation,
including its snapshot/output/maps/scratch. Python input/result wrappers,
diagnostic marshalling and the copies into Python-owned map buffers occur
outside those reservations. Retaining returned candidates/views is also outside
active accounting. The per-result peak and context peaks are reserved payload,
not measured allocation or process RSS; allocator spare capacity, overhead and
thread stacks remain excluded by the native contract. The GIL is held during
record import/export and result marshalling.

See the [installed concave/UV example](../examples/python/triangulate_mesh.py) and
[installed acceptance tests](../tests/python/test_triangulation.py), which use an
independent Fraction coverage/correspondence oracle and exercise worker sharing,
payload contention, real Python-thread cancellation, strict configuration and
result lifetimes. Conversion does not establish destination float32 loss bounds
or format publication.

## Portable development candidates

The manually dispatched [candidate workflow](../.github/workflows/portable-candidates.yml)
selects ordinary GIL-enabled CPython 3.13 only: Linux x86_64 with a
`manylinux_2_28_x86_64` (glibc 2.28) baseline, and Windows x64. This is a
packaging pilot, not a released support matrix. macOS, other architectures and
Python ABIs require separate final-artifact evidence.

The workflow pins cibuildwheel 4.2.0, its dependency set, and an immutable
manylinux image. It builds from full source history, inspects the source archive,
then independently rebuilds that archive outside Git. Linux uses auditwheel;
Windows uses delvewheel with the Microsoft C++ runtime kept external. Windows
consumers need ordinary x64 CPython 3.13 and the official
[x64 Visual C++ v14 Redistributable](https://learn.microsoft.com/en-us/cpp/windows/latest-supported-vc-redist/),
at least as recent as the compiler recorded in the build log. CPython supplies
its own Python/VCRuntime DLLs; Windows supplies UCRT and system DLLs. The
`windows-2022` hosted image is mutable, so its image identity is recorded for each
run. A clean Windows 11 consumer check remains necessary before declaring that
runtime baseline supported.

Both repaired builds run the snapshot and conversion tests, the independent installed
record consumer in both import orders, report validation and `pip check` in
isolated test environments outside source. Successful jobs retain wheel/source
archives, their required license notices, dependency inspection and a SHA256
manifest as **development CI artifacts** for 14 days. Download an artifact from
the workflow run, check `manifest.json`, and use its `wheels` directory in a fresh
CPython 3.13 environment on the matching platform:

```sh
python -m pip install --no-index --find-links wheels meshvale-geometry
python -I -c "from meshvale_geometry import Mesh; print(Mesh().vertex_count)"
```

The standalone snapshot import needs no additional Python dependency. The
optional report validator still requires `jsonschema==4.26.0`. A successful job
proves the exact candidate tested there; it does not establish compatibility
with arbitrary downstream development versions. No tag, release or package-index
upload is performed. Existing `linux_x86_64` or `macosx_26_0_arm64` host wheels
must be rebuilt for a lower baseline rather than renamed.
