# Numerical position storage

| Field | Value |
|---|---|
| ID | MESHVALE-NUMERICS-001 |
| Version | 0.1.0 |
| Status | Native development interface; no stable API/ABI or release |
| Owner | Geometry |

[`PositionBuffer`](../include/meshvale/geometry/position_buffer.h) owns raw xyz
rows through a private Eigen 3.4.1 row-major dynamic matrix implementation.
Declarations live in `.h`; ownership and storage details live in compiled `.cpp`.
Public headers expose no Eigen types or borrowed matrix expressions. The
[raw mesh contract](attributes.md) owns polygon offsets, corner indices and all
seven independent attribute scalar types.

## PositionBuffer interface

`size()` counts logical xyz rows; `empty()` tests that count. Default and
moved-from buffers are empty and reusable. The initializer-list constructor takes
owned xyz values, so `mesh.positions = {{0,0,0}, {1,0,0}}` constructs a replacement
value. Copies own independent logical rows. Copy assignment, reserve and growing
Append have a strong guarantee: allocation failure leaves source and destination
unchanged. Move transfers ownership without allocation; self-copy/self-move retain
the value. No mutable reference, vector iterator or Eigen Map escapes the buffer.

`Get(row)` returns an owned `std::array<double,3>`; `Set(row,value)` replaces an
existing row; `Append(value)` adds one complete row. Get/Set throw
`std::out_of_range` outside the logical row count. Read, edit and Set explicitly:

```cpp
auto position = mesh.positions.Get(vertex_row);
position[2] = 1.0;
mesh.positions.Set(vertex_row, position);
mesh.positions.Append({2.0, 3.0, 4.0});
```

`reserve(rows)` changes capacity without adding rows; `clear()` removes logical
rows and retains capacity. Append uses bounded geometric growth. Capacity beyond
logical rows is never inspected/exported. Row count, scalar/byte products and
signed host Eigen::Index limits are checked before allocation; rejected sizes
throw `std::length_error`. Allocation failures propagate `std::bad_alloc`.

`AssignBytes(source)` replaces positions from complete native-format binary64 xyz
triples, accepting misaligned byte input and copying once before publication.
`CopyBytesTo(destination)` requires exactly `size()*3*sizeof(double)` bytes and
copies logical rows once. Bad byte shapes throw `std::invalid_argument`.
AssignBytes reads the entire source before replacing old storage; CopyBytesTo
permits overlapping ranges. Empty transfers accept null pointers. These are
process-local native byte transfers, not portable serialization. They preserve
NaN payloads and signed-zero bits without normalizing or diagnosing coordinates.
Nonfinite positions remain representable for storage inspection.

The raw C++ source/ABI shape changes from
`std::vector<std::array<double,3>>`: replace push_back with Append and indexing/
range iteration with size/Get and explicit Set. Dense editor projections and
pooled rows retain their existing owned types; import/export copies those rows.
The [Python mesh/1 record](python.md) retains flat native-endian float64 xyz,
strict admission, independently owned snapshots and immutable exported buffers.
Every extension still compiles its installed record adapter in its own domain.

Separate immutable buffers support concurrent reads. Mutation, moving or
destruction of the same C++ buffer alongside access requires external
synchronization. No borrowed matrix/span ownership is promised.

## Numerical and execution limits

Eigen position ownership does not replace operation-specific guarantees.
[Exact triangulation](triangulation.md) retains original exact predicates,
represented binary64 coordinates and face/corner correspondence. Positions store
bytes; storage does not perform approximate orientation/planarity or transforms.

Attributes retain all seven typed one-dimensional buffers, domains, offsets,
presence and metadata. Later numerical views must check shape/host indices,
consult authored rows, retain their owner and evaluate owning expressions before
borrowed operands expire. Corner UV/normal rows remain independent of positions;
missingness/normalization metadata does not authorize filling or conversion.

The selected Core subset defines `EIGEN_MPL2_ONLY`, `EIGEN_DONT_PARALLELIZE` and
`EIGEN_FAST_MATH=0`, with strict compiler floating-point flags. Eigen starts no
additional worker pool. Product operations retain worker admission, thresholds,
cancellation and joins under the [execution contract](execution.md). Raw buffers
are outside context accounting. Triangulation retains logical captured/output
row charges and its documented capacity/bookkeeping exclusions; Eigen allocations
are not automatically counted by leases. New numerical temporaries need their
own operation's admission policy. This makes no speed or general robustness claim.

## Dependency and installation

Source builds require CMake 3.24+, C++20 and Eigen **3.4.1**. CMake first resolves
`find_package(Eigen3 3.4.1 EXACT CONFIG)`. Incompatible versions, including Eigen 5
from a package named eigen3, are not selected. Without the exact package, the
default build downloads the official
[`eigen-3.4.1.tar.gz`](https://gitlab.com/libeigen/eigen/-/archive/3.4.1/eigen-3.4.1.tar.gz)
and checks SHA256
`b93c667d1b69265cdb4d9f30ec21f8facbbe8b307cf34c0b9942834c6d4fdbe2`.
The unmodified dependency remains in the build directory. Git-free source
archives use the same acquisition. For offline builds, install Eigen separately:

```sh
cmake -S . -B .local/build -DCMAKE_BUILD_TYPE=Release \
  -DEigen3_DIR="$EIGEN3_CONFIG_DIR" -DMESHVALE_GEOMETRY_FETCH_EIGEN=OFF
cmake --build .local/build --config Release
ctest --test-dir .local/build -C Release --output-on-failure
cmake --install .local/build --config Release --prefix .local/install
```

Eigen is a private header-only implementation dependency. Geometry exports its
compiled static position implementation without an Eigen target/include path.
Installed native/record consumers require the Geometry install and its existing
thread dependency, without Eigen headers or source/private configuration.
Native installs and Python packages retain MPL-2.0, Apache and included Core
notices named in [NOTICE](../NOTICE). No Eigen sources are modified; covered
original source is available from the upstream
[3.4.1 tree](https://gitlab.com/libeigen/eigen/-/tree/3.4.1).

## Verification surface

Storage cases cover signed-zero/NaN bytes, misaligned input, logical capacity,
independent copy/move, owned row values, extents and overflow. Replacement-new
cases inject position implementation allocation failures and check copy/growth
rollback; they do not intercept Eigen's internal malloc/aligned allocation.
Existing polygon/editor/exact conversion suites retain correspondence and
snapshots, with an independent Fraction oracle. Installed, Git-free archive and
Python gates are required for each candidate; configured CI is not executed
sanitizer/package proof.
