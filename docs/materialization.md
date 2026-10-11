# Charged immutable mesh materialization

| Field | Value |
|---|---|
| ID | MESHVALE-MATERIALIZATION-001 |
| Version | 0.2.0 |
| Status | Development candidate with native runtime and installed target; Windows allocation profile qualified |
| Owner | Geometry |

This document specifies the
[`materialization.h`](../include/meshvale/geometry/materialization.h) interface.
The compiled `meshvale::materialization` target depends on Geometry and its shared
execution interface. Installed consumers need no Eigen headers. Base numerical
buffers do not depend on materialization or execution.

The currently admitted nonempty numerical allocation profile is 64-bit Windows
with MSVC, pinned Eigen 3.4.1, 16-byte default alignment, and Eigen's
`EIGEN_MALLOC_ALREADY_ALIGNED` flag equal to one. Native and matching MSVC ASan
requests have been observed on this original allocation route. Other compiled
profiles, including Linux, remain unqualified and reject nonempty numerical
capture before an unplanned request. An all-empty numerical capture does not
need a numerical allocation route; its structural storage remains charged.

`MaterializeMesh` captures complete typed mesh storage into an immutable
`MaterializedMesh`. `InspectMaterializedMesh` inspects that storage without
repairing it. `SelectMeshFaces` creates an immutable selection from valid storage,
with face and corner correspondence. These operations share the
[execution ledger](execution.md) and use a fresh cancellation token on each call.
They do not export an ordinary mutable `Mesh` or perform geometric duplicate detection,
attribute normalization, geometric validation, or repair.

## Input and canonical storage

Ingress spans and string views must refer to live, readable native objects and
remain unchanged until the synchronous call returns. Empty spans are valid typed
inputs. A default `monostate` position, topology, or attribute scalar selector is
invalid transport. Invalid pointers, concurrent mutation and inaccessible source
memory are outside the interface contract.

Positions are flat, interleaved XYZ values. Their physical length must be a
multiple of three. Double input preserves every binary64 bit. Finite float input
widens exactly to binary64, including signed zero and subnormals. Float infinity
preserves its sign. A float NaN maps its sign and 23-bit fraction into binary64:
the fraction shifts left by 29 bits, preserving the quiet/signaling bit and
payload; the exponent becomes the binary64 all-ones exponent. Widening uses bit
operations, without floating arithmetic, implicit NaN quieting, dependence on
rounding mode or flushing subnormals. Nonfinite positions remain raw defects for
inspection.

Polygon input preserves the complete supplied `uint64_t` face-offset and corner
arrays, even when offsets are empty, unordered or out of range. Triangle input
requires a physical corner count divisible by three; it widens each `uint32_t`
corner index exactly and derives offsets delimiting three corners per face.
Degenerate triangles and unused vertices remain unchanged. Canonical positions
own the existing double three-component `VectorBuffer`; every canonical
attribute owns an existing Eigen-backed `ScalarBuffer` as specified by
[numerics](numerics.md).

| Scalar input | Canonical attribute scalar |
|---|---|
| float, double | Same type and exact bits |
| int32, uint8, uint16, uint32, uint64 | Same type and exact bits |
| int8, int16 | Exact sign extension into int32 |

The seven `ScalarReadView` alternatives follow this canonical order. There is no
normalization flag interpretation, clamping, influence limit or lossy integer
conversion. Derived numerical values and source encoding provenance belong to
the caller's processing layer.

Attributes preserve source order, domain, component count, name, semantic,
optional set index, scalar values, optional ragged scalar offsets, optional
presence bytes, and ordered metadata entries. Strings preserve their complete
byte sequences, including embedded zeros. An absent optional row array differs
from an authored empty array. Ragged offsets delimit scalar rows; presence zero
means missing and one means authored. Metadata order and duplicate keys are
retained. Duplicate names, empty names, zero components, foreign `AttributeDomain`
values, malformed row arrays and invalid presence bytes are physically
representable raw defects, rather than invalid configuration or transport.
Capture does not infer a domain or silently correct a descriptor.
The ordered metadata entry representation is specific to this interface; the
legacy mutable `Attribute::metadata` map has unique sorted keys and cannot
represent duplicate-key raw input. Materialization does not round-trip through
that map.

## Required limits and work policy

Every limits and work-policy field must be explicitly positive. A zero value is
`kInvalidConfiguration/kRequiredFieldZero`; it does not select an unlimited mode
or a hidden default. Positive limits still allow empty meshes and arrays.
Configuration values that cannot be represented by the compiled host extents are
`kInvalidConfiguration/kConfigurationExtent`.

All count limits are inclusive. Vertices count complete physical XYZ rows.
Polygon face count is zero for empty offsets, otherwise offset count minus one;
corner and face-offset limits separately bound their complete physical arrays.
Triangle input counts its derived face offsets as well as its widened corners.
Attribute scalar, offset, presence and metadata limits sum physical entries over
all attributes. Descriptor bytes sum all name, semantic, metadata-key and
metadata-value bytes without adding terminators. Attribute count includes every
descriptor, even one with defective raw content. Selection applies these count
limits to its complete candidate, including the unchanged shared vertex part.
Inspection uses its separate positive issue-count and new-request limits.

`max_new_requested_bytes` bounds actual newly requested blocks for that operation.
It includes temporary requests and requests freed before return. The live new
request total must never exceed the cap; cumulative allocation traffic is not
the metric. Existing retained source charges are excluded from this new-operation
cap and remain charged in the common execution ledger. A shared part is charged
once. Output inspection and selection owners retain their operation accounting;
there is no post-hoc reservation or logical-payload substitute.

`parallel_min_bytes` is the copied/inspected scalar and structural byte threshold
for attempting worker admission; descriptors have their actual inspected byte
lengths. `partition_bytes` is the target byte extent of a leaf partition; checked
whole-scalar boundaries may exceed it by less than one scalar. `max_jobs` caps
leaf jobs per phase, so a cap may require larger partitions.
`cancellation_block_scalars` bounds scalar iterations between stop checks; byte
and descriptor loops use the corresponding bounded byte/entry iterations.
Descriptor planning and publication preserve source order. Their dependency on
checked extents and stable row layouts justifies serial execution. Substantial
numeric, structural, row and inspection loops use byte-aware partitions,
including splitting a single long row.

Worker admission uses the retained shared `WorkerLease`, with caller execution
when work is small or fewer than two slots are available. There is no competing
pool. The native backend uses the Windows CRT thread launch interface on Windows
and pthreads on Linux. Explicit Meshvale callable arguments, records, handles,
failure slots and scratch blocks are charged. Opaque OS thread and CRT runtime
internals are outside the requested-block ledger. This boundary does not exempt
explicit Meshvale storage or make the ledger a measure of process memory. Execution statistics report total leaf jobs planned, workers actually
launched and joined, and jobs actually executed on the caller across all phases.
They are observations rather than requested worker counts. A returned result has
equal launched and joined counts. Successful serial and parallel calls produce
identical bytes, correspondence and ordered issues. No throughput guarantee is
implied.

## Inspection and operational admission

Inspection retains its source owner and returns complete ordered, fixed-code
`StorageIssue` records. It checks the raw storage rules from
[attributes](attributes.md): finite positions; topology offset start/end, range
and order; face lengths of at least three; corner vertex range; unique attribute
`(domain, name)` pairs; recognized domains; nonempty attribute names; nonzero
components; dense scalar shape; ragged row count, start/end, range, order and
component alignment; presence row count and values. It additionally reports
duplicate metadata keys within an attribute. It does not require finite attribute
scalars, unit normals, a UV range or normalized weights.

Issues are ordered by these stages: positions in vertex order; topology's empty,
start and end checks, then offset range/order/short-face checks in offset order;
corner references in corner order; attributes in source order. Within an
attribute the order is duplicate name, domain, empty name, zero components,
dense shape or ragged empty/count/start/end and per-offset range/order/alignment,
presence count then per-entry values, and duplicate metadata keys in entry order.
An invalid domain omits checks requiring a domain's expected row count, but
independent descriptor, scalar-offset and presence-value checks still run.
An issue's `attribute` identifies its source attribute ordinal; `element` is the
relevant vertex, offset, face, corner, presence or metadata ordinal. Aggregate
checks have no element. Duplicate-name and duplicate-key issues identify each
later occurrence, without removing it. Floating positions report one issue per
vertex containing any nonfinite coordinate.

`IsStorageValid()` means that the issue list is empty. An inspection reaching
`max_issues` exactly succeeds; a further issue yields
`kLimitExceeded/kIssueLimit` with no partial inspection owner. Inspection does
not truncate a list to report a clean result.

Selection first requires valid source storage under these same rules. Its fixed
failure diagnostic carries the first source issue; it does not allocate a full
inspection report just to reject the source. `kept_faces` must contain unique,
strictly increasing in-range source face ordinals. The empty selection is valid.
All vertices and vertex-domain channels remain unchanged in a shared immutable
vertex part; vertex correspondence is identity. Selected polygon loops and
face/corner-domain dense or ragged rows, offsets and missingness are copied in
source order. All descriptors and metadata remain complete. Empty selection has
the canonical single face offset zero and empty face/corner rows.

`MaterializedFaceSelection::SourceFaces()` maps each output face to its source
face. `SourceCorners()` maps each output corner to its source corner; lengths
equal the candidate face and corner counts. Source counts remain available even
after the input owner is destroyed. The selection owner retains its candidate
and maps. Copying out the candidate retains the candidate storage, but borrowing
the correspondence spans requires the selection owner. Selection does not retain
obsolete source face/corner storage merely to keep maps alive. Many-to-one
duplicate/merge correspondence is a downstream repair concern.

## Ownership, requests and failures

Visible owners are copyable strong immutable handles without a public default
or move-empty state. Copying shares storage and charges; it does not allocate.
Borrowed views require their corresponding visible owner to remain alive and
not be reassigned. Independent owner copies support concurrent reads and calls.
Input arrays, the original `ExecutionContext` and an earlier stop source may be
destroyed after a successful call. Retained storage owns the context needed for
later inspection and selection. Cancellation of an earlier call cannot cancel a
new operation's fresh token.

Every structural, descriptor, string, metadata, diagnostic, correspondence,
scratch, owner/control and rebound request is admitted before construction,
including the operation's initial accounting state and its allocation header.
Canonical numeric requests are planned inside their compiled Eigen owners from
the pinned original allocation route, including PIMPL, header and alignment
padding. Production preserves those original malloc/new routes without global
allocator replacement, thread-local hooks or process-wide flags. An unqualified
compiled allocation profile returns
`kUnsupportedAllocationProfile/kAllocationProfile` before making an unplanned
request. Unpublished numeric destinations permit disjoint fill without hidden
resize, zero-fill or expression temporaries. Actual old/new overlap remains
charged; common-ledger rejection or the new-operation cap returns
`kBudgetExceeded/kRequestBudget`.

Nonempty canonical numerical storage currently admits MSVC x64 with pinned
Eigen 3.4.1, default alignment 16 and malloc-already-aligned exactly 1. Linux
admission is restricted to the observed x86_64 LP64 Debug profile: Clang 18.1.3,
glibc 2.39, libstdc++ release 14/header date 20240908, the same Eigen route and
default SSE2 vectorization. The compiled numerical target must identify Debug;
missing configuration markers, NDEBUG, optimization, fast math and extended
SIMD are unqualified. The observed Linux sanitizer profile combines ASan and
UBSan; TSan, memory/HW address sanitizers and either ASan or UBSan alone remain
unqualified. This admission identifies original numerical request routes; full
factory, worker and sanitizer acceptance remains a separate qualification gate.

Other Linux compilers/configurations, macOS, alternate C++ runtime headers and
other allocation tuples return the unsupported-profile diagnostic for nonempty
numerical storage. Typed empty numerical input and structural storage remain
subject to the same validation and charged ownership contract. Ordinary generic
buffers retain their existing Eigen allocations and do not depend on this
materialization admission policy. Installed consumers use their library's
compiled profile; their own compiler or Eigen settings cannot change it.

`NewRequestedBytes()` observes the operation's current live requested blocks,
including retained parts and accounting owners. It is not an object-local
footprint, logical byte count or RSS. Candidate and selection handles may share
an operation: releasing a map owner can reduce the candidate's observed value.
Retained source requests belong to their original operation. Temporary request
charges disappear after their storage is freed. Final destruction frees storage
before releasing its lease, allocates nothing and ignores cancellation.

Expected failures return fixed diagnostics and no output owner. Invalid physical
position or triangle row extents are invalid transport; checked storage extent
overflow or a count cap is `kLimitExceeded`. A valid raw capture may have many
storage defects and still succeed. Configuration validation precedes transport
validation, then checked physical extents/count limits. Selection admits source
storage before interpreting its row offsets or validating source-dependent
candidate counts. That admission may require charged inspection scratch before
selection checks and candidate allocation; it does not construct a partial
candidate. Cancellation is observed before allocation,
within bounded long loops and after final joins before publication. An earlier
configuration or source-validation failure remains that failure. Deterministic
worker failure slots choose the first source-order failure, independently of
scheduling; actual runtime exceptions take precedence over cooperative worker
cancellation. If cancellation arrives after a first-only leaf has discovered a
source defect, all workers join before the necessary earlier source prefix is
finished without allocating. This diagnostic finalization preserves the first
source-order issue and returns invalid source storage. It does not resume
capture or selection, or publish a successful output after cancellation.

Moved-from execution contexts retain the existing execution interface's
`EditorError(kInvalidObject)` behavior when the context is used. Actual runtime
allocation, thread-launch, synchronization and other exceptions propagate after
rollback and all launched workers have joined. There is no partially published
owner or fallback uncharged allocation. Cleanup does not invoke user callbacks
or allocate while holding the ledger lock; joins and allocation routes are also
outside that lock.

This interface establishes complete charged storage and selection. File formats,
source encoding provenance, charged repair candidates/reports, skinning,
animation, edited re-embedding and Python bindings require their own interfaces.
