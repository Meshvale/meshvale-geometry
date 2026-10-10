# Numerical position and attribute storage and operations

| Field | Value |
|---|---|
| ID | MESHVALE-NUMERICS-001 |
| Version | 0.3.0 |
| Status | Native development interface; no stable API/ABI or release |
| Owner | Geometry |

[`PositionBuffer`](../include/meshvale/geometry/position_buffer.h) owns raw xyz
rows through a private Eigen 3.4.1 row-major dynamic matrix implementation.
Declarations live in `.h`; ownership and storage details live in compiled `.cpp`.
Shared Eigen aliases live in the private [src/eigen_types.h](../src/eigen_types.h):
row-major position
storage, typed dynamic vectors, unaligned const vector Maps and the signed host
index. Public headers expose no Eigen types or borrowed matrix expressions. The
[raw mesh contract](attributes.md) owns polygon offsets, corner indices and all
seven independent attribute scalar types. Canonical attribute scalars now also
own Eigen vectors through [`ScalarBuffer<T>`](../include/meshvale/geometry/scalar_buffer.h).
The typed scalar vector is the backing allocation, rather than a Map over a
`std::vector`. Offsets and presence remain independent row metadata.

## Canonical attribute ownership

`AttributeValues` is a variant of `ScalarBuffer<float>`, `ScalarBuffer<double>`,
`ScalarBuffer<int32_t>`, `ScalarBuffer<uint8_t>`, `ScalarBuffer<uint16_t>`,
`ScalarBuffer<uint32_t>` and `ScalarBuffer<uint64_t>`, in that order. Each owns a
private Eigen dynamic vector through a nullable value-type implementation.
Default and moved-from buffers are empty, reusable and allocate no storage.
Copies own independent logical scalars and omit unused source capacity. There
are no Eigen types in installed declarations; link `meshvale::geometry` for the
seven explicit compiled instantiations.

Flat scalar ownership deliberately retains malformed shapes: component width,
dense/ragged offsets, authored/missing rows and semantic metadata remain on the
`Attribute`. It does not force ragged weights into four columns or merge corner
UV/normal rows with positions. All seven encodings, signed zeros, floating NaN
payloads and integer joint identifiers above 2^53 survive byte-copy construction,
copy, growth and export. No normalization, truncation, type conversion or shape
repair occurs in storage. Offsets and presence masks keep their current vector
representation; reduction result/capture allocations keep their separate admitted
implementation below. This completes canonical attribute scalar ownership, not
every metadata/result allocation or a skinning evaluator.

The contiguous scalar interface provides size/capacity/reserve/resize/clear,
checked indexing/front/back, pointer iterators, `Values()` spans, scalar append
and range insert. References and pointers address real owned `T` objects.
Borrowing requires a live owner with no storage replacement, mutation races,
move or destruction; separate immutable owners support concurrent reads.
Reserve and growing resize/append, insert and assignment can invalidate views.
Copy assignment and allocating operations preserve the old value on allocation
or length failure. Count construction and newly exposed resize slots initialize
to `T{}`. Clear retains capacity; empty byte assignment releases it. Insert
constructs a replacement before publication and accepts self-overlapping ranges.
Pointer-range input must be ordered within one live scalar array; insert positions
must belong to the destination's logical range, including its end.

`AssignBytes` accepts complete native-format scalars, including misaligned input,
and reads all bytes before replacement. `CopyBytesTo` requires the exact logical
byte count and uses an overlap-safe copy. Bad byte extents throw
`std::invalid_argument`, invalid indices/insert positions throw
`std::out_of_range`, and scalar/byte/alignment/signed-host ceilings throw
`std::length_error`; allocation failures propagate `std::bad_alloc`. These are
process-local transfers, not portable serialization. Buffer equality preserves
the former scalar comparison semantics: signed zeros compare equal and NaN
compares unequal, even though byte copying preserves their different bits.

This is an intentional C++ source/ABI change. Vector assignment remains copying
ingress, but replace `std::get<std::vector<T>>(attribute.values)` with
`std::get<ScalarBuffer<T>>(attribute.values)`. Rebuild Geometry and all native or
per-extension record consumers together; old binaries cannot consume the new
layout. Generic variant visitors can use the contiguous scalar interface. The
Python `meshvale.mesh/1` schema, scalar names/formats, raw bytes, independent
immutable exports and extension-domain ownership remain unchanged.

Raw storage construction/copy does not receive an execution context or work
policy. Its single byte transfer remains serial to retain exact ownership and
publication semantics without creating unmanaged workers. This is not a
parallel throughput claim; substantial numerical processing uses the admitted
operation scheduler below. Parallel raw capture/preparation requires its own
context-aware operation contract and qualified threshold.

## PositionBuffer interface

`size()` counts logical xyz rows; `empty()` tests that count. Default and
moved-from buffers are empty and reusable. The initializer-list constructor takes
owned xyz values, so `mesh.positions = {{0,0,0}, {1,0,0}}` constructs a replacement
value. Copies own independent logical rows. Copy assignment, reserve and growing
Append have a strong guarantee: allocation failure leaves source and destination
unchanged. Move transfers ownership without allocation; self-copy/self-move retain
the value. No mutable reference, vector iterator or Eigen Map escapes the buffer.

`Get(row)` returns an owned `std::array<double,3>`; `Set(row,value)` replaces an
existing row; `Append(value)` adds one complete row. This array is a copied xyz
value at the interface; the backing allocation is the owned Eigen matrix.
Get/Set throw
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

Attributes retain all seven Eigen-owned typed one-dimensional buffers, domains, offsets,
presence and metadata. Later numerical views must check shape/host indices,
consult authored rows, retain their owner and evaluate owning expressions before
borrowed operands expire. Corner UV/normal rows remain independent of positions;
missingness/normalization metadata does not authorize filling or conversion.

The selected Core subset defines `EIGEN_MPL2_ONLY`, `EIGEN_DONT_PARALLELIZE` and
`EIGEN_FAST_MATH=0`, with strict compiler floating-point flags. Eigen starts no
additional worker pool. This macro does not disable product parallelism.
Attribute reductions partition admitted large inputs into independent row ranges
computed by product-owned threads, then join all workers before returning.
Product operations retain worker admission, thresholds,
cancellation and joins under the [execution contract](execution.md). Raw buffers
are outside context accounting. Triangulation retains logical captured/output
row charges and its documented capacity/bookkeeping exclusions; Eigen allocations
are not automatically counted by leases. New numerical temporaries need their
own operation's admission policy. This makes no speed or general robustness claim.

## Attribute row reductions

`ComputeAttributeRowSums` and `ComputeAttributeRowSquaredNorms` in
[`attribute_numerics.h`](../include/meshvale/geometry/attribute_numerics.h)
accept one canonical `Attribute`, its expected domain row count, options, an
`ExecutionContext` and an optional stop token. Link the compiled
`meshvale::attribute_numerics` target. These operations reduce every scalar in
one dense or ragged row to one scalar in the **same storage type**. They support
float32, float64, int32, uint8, uint16, uint32 and uint64. Normal/tangent, UV and
color rows can be measured by squared norm; fixed or arbitrary-length weight
rows can be measured by sum. No semantic label changes the arithmetic: encoded
integer weights remain encoded integers, normalization metadata is descriptive,
and joint identifiers above 2^53 remain integers. These reductions do not evaluate
skinning, transforms, or normalized/interpolated channels.

Shape admission checks the existing attribute contract, domain, expected row
count, offsets, component alignment, presence values, and signed host
`Eigen::Index`/byte extents before mapping. A malformed shape returns `kBlocked`
with its `attribute.*` diagnostic; a host extent failure uses
`numerical.host_extent`. The source remains representable and unchanged, even
when rejected. Corner rows are independent of position rows and other channels;
separate UV sets remain separate calls. Caller-supplied row count does not prove
skin pairing, valid joint references or any asset-level semantics.

Accepted results own a typed vector with one result per source row and an explicit
presence mask. A missing row has mask zero, its backing scalars are never evaluated,
and its result slot is an unauthored zero placeholder. An authored empty ragged row
has mask one and a zero reduction. Result values never fill missing source rows.
`Get(row)` returns one scalar value in a seven-type variant. Out-of-range or
blocked access throws `std::out_of_range`. `CopyValues()` returns an independent
canonical `AttributeValues` copy, throws `std::logic_error` when blocked, and
propagates export allocation failure. Export copies are caller allocations outside
the context ledger.
`Presence()` and fixed code/row `Diagnostics()` borrow from the result. Borrowed references/spans
last until result move/destruction and require external synchronization with either.
Results are movable and noncopyable, retain their payload charge until their storage
is destroyed, and survive source or context destruction. Default/moved-from results
are blocked and contain no values. No Eigen type, Map, expression or mutable output
reference is public.

Each signed/unsigned integer addition and squaring intermediate is checked in the
source type. An unrepresentable intermediate returns `numerical.integer_overflow`,
even if a differently ordered final mathematical sum would fit. Floating inputs
must be finite on authored rows; NaN/infinity returns `numerical.nonfinite_value`.
Floating addition/squaring uses source float32/float64 arithmetic and the compiler's
strict floating profile. A nonfinite intermediate returns
`numerical.nonfinite_result`; finite rounding, subnormals, underflow and signed-zero
arithmetic remain ordinary host floating behavior. There is no exact-sum or
cross-toolchain bitwise-result guarantee. Numeric-domain rejection is distinct
from shape rejection and neither changes canonical raw NaN/signed-zero payloads.
The first failing source row is reported deterministically after workers join.

The implementation admits a private byte-preserving capture, row outcomes and
owned output under the shared execution payload ledger before allocating them.
Each privately owned block admits its complete requested allocation before
allocation: its payload lease header, maximum-native alignment, typed elements and
result implementation. Captured scalars/offsets/presence, output scalars/presence,
row outcomes and worker-object arrays coexist under that ledger. No private vector
capacity or adapter header is excluded. Caller raw storage/export copies and upstream
allocator internals, thread stacks/runtime and exception-runtime allocations are
excluded. Expected diagnostics and serial reasons use immutable literal codes;
no diagnostic string allocation is needed.
Canonical attribute channels and admitted capture/output storage remain typed
scalar buffers; this stage does not migrate all attributes to owning Eigen
matrices or expose reusable matrix views. Dense and ragged rows use the same
checked typed vector Map, preserving their offsets and authored presence.
Eigen uses lexical unaligned typed Maps over scalar arrays and evaluated scalar
reductions; no dynamic Eigen matrix temporary is allocated. Capture/output leases
are released only after their owned storage. `PeakTrackedPayloadBytes()` reports
this call's declared peak, not resident memory. Expected budget refusal returns
`numerical.payload_budget`; allocation and thread-launch failures propagate their
standard exceptions without changing source or leaking leases.

Rows at or above `minimum_parallel_rows` (default 1024) are eligible for joined
workers admitted by the existing context; fewer than two available slots run on
the caller. `WorkersUsed()` and `SerialReason()` report the actual path. Eigen
internal parallelism stays disabled. Workers own disjoint result slots and read
only the private immutable capture. Cancellation observes admission, copying,
scalar work and joined completion, returns `kCanceled` / `numerical.cancelled`,
and exposes no partial values. It takes priority over expected row failures after
joins; unexpected exceptions still propagate. Separate calls can share an immutable
source and context. The caller must keep source alive and prevent its mutation
throughout capture; the result can outlive both. No performance claim follows.

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
