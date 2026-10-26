# Charged immutable polygon triangulation

Version: 0.1.0. Status: implemented development C++20 interface, qualified within
the declared and tested development configurations. This does not establish a
supported platform floor, stable ABI or released SDK/package.

The C++20 [header](../include/meshvale/geometry/charged_triangulation.h) and compiled
`meshvale::charged_triangulation` target provide two typed `TriangulateCharged`
overloads. A legacy `Mesh` takes an explicit `ExecutionContext` and is captured
into charged storage. A `MaterializedMesh` retains its actual immutable input and
derives its execution ledger with `ShareExecutionContext()`. It never converts
through the legacy metadata map. No generic public storage converter is needed.
Both operations return one immutable `ChargedTriangulation` owner with a complete
`MaterializedMesh`, its actual storage inspection, counts and all correspondence.
The [standalone installed example](../examples/charged-triangulation-consumer/main.cpp)
uses public headers for both routes with typed, missing and ragged channels.

The existing [legacy conversion](triangulation.md) and its numerical profile,
strings, maps, worker behavior and conservative metric are unchanged. Both
interfaces use one exact predicate/ear implementation with distinct allocator
policies. This operation adds no Python binding, encoding, rounding, attribute
loss, asset rewrite, material conversion or file publication policy.

## Complete result and typed correspondence

Only `kAccepted` exposes `result.triangulation`. Other normal outcomes have no
accepted mesh or partial maps. `Mesh()` and `Inspection()` return real strong
owners; the inspection's `Source()` is the same output storage. `Counts()` contains
the original and output vertex, face and corner counts. Vertices, including unused
vertices, retain their source order and bits. Each output face has three corners.

`FaceSources()` maps output triangles to authored source faces. `CornerSources()`
maps output corners to authored source corners, distinct from vertex IDs.
`FaceOutputOffsets()` has source-face-count plus one entries, begins at zero and
partitions output triangles in source order. Its last entry equals output-face
count. Empty input retains canonical face offsets `{0}`, empty face/corner maps
and source-face output ranges `{0}`.

All seven canonical scalar alternatives retain their exact type and bits.
Attributes retain order, domain/name, semantic/set labels, component width,
complete string bytes (including embedded zeros), ordered metadata, dense/ragged
shape, optional-array authoredness, presence and missing backing. Face rows
replicate through the face map; corner rows transfer through the corner map;
vertex rows retain all backing. Ragged offsets are rebuilt from complete copied
scalar ranges, including empty rows. Presence zero does not erase its backing.
No normalization, interpolation, welding or channel interpretation is performed.

The legacy route begins with its unique sorted metadata map order. Retained input
keeps its actual ordered representation. Actual source inspection refuses invalid
raw storage, including duplicate metadata keys, instead of normalizing it. Storage
inspection does not require finite attribute scalars, unit normals, UV ranges or
normalized influences. Position/numerical admission remains the exact legacy
profile: simple, exactly planar ordered loops, no epsilon or generated vertices.

## Configuration, limits and normal outcomes

All fields in `ChargedTriangulationLimits` and `MeshMaterializationWorkPolicy`
are required positive, inclusive and host-representable. The nested source and
output objects use the existing [physical count, descriptor and requested-byte
definitions](materialization.md). Source count ceilings apply to both input
routes. `source.max_new_requested_bytes` caps the legacy source capture; retained
input performs no capture call and has no new capture requests. Source/output
inspections have independent issue/request ceilings. The source is actually
inspected before numeric access, and output is actually inspected before acceptance.

`max_map_entries` bounds the SUM of all three retained map lengths.
`max_core_new_requested_bytes` bounds simultaneous live original requests for
the numeric kernel and per-face plans, including limbs, capacity growth, explicit
worker/failure storage, aligned headers and the initial accounting state.
`max_bridge_new_requested_bytes` separately bounds adaptation/gather/descriptors,
maps and their strong/control owners. They share the same root execution ledger
with actual capture/output/inspection operations and retained source charges.
There is no waiting or new public aggregate cap. All count/prefix/byte arithmetic
is checked before allocation and indexed access.

Pure configuration is checked before stop or allocation in these new overloads.
`max_corners_per_face` retains its existing 3..4096 range, and
`minimum_parallel_faces` retains its existing meaning, including zero. A valid
pre-stopped operation is canceled. Invalid/moved-from contexts retain their owning
exception contract. Expected failure/cancellation returns typed status, stage,
closed code and optional actual face/attribute/storage issue. Actual numerical
refusals are blocked, count/extent ceilings are limit failures, and actual
materialization profile refusal remains `kUnsupportedAllocationProfile`.

Operation-cap and root-ledger admission currently share an underlying budget
outcome. `kCoreRequestBudget` and `kBridgeRequestBudget` identify the actual phase,
and do not claim which of those two causes was uniquely responsible. Delegated
materialization statuses/diagnostics are preserved. Unexpected backend, allocation
or thread exceptions propagate after joins; no normal result is fabricated for
an escaping operation. For normal expected outcomes, an observed stop takes
priority after cleanup. The old legacy API's existing precedence is unchanged.

## Actual phase facts and work

Optional source capture/inspection and output materialization/inspection facts
exist only when the corresponding delegate was actually called and returned.
They retain its actual typed status, diagnostic and execution counters. A requested-
byte snapshot at delegate return is neither a later live count nor a peak.
An inspection's `storage_valid` exists only if an inspection owner actually
returned. Earlier completed facts remain after a later normal failure. Unreached
phases remain absent. `counts` contains all six counts only after complete source
inspection and the full topology-count plan; it is not a partly initialized record.

`adaptation_execution` observes only the actual legacy interleaved-position copy.
It uses the supplied byte-aware private job planner and shared worker/stop state;
retained input has zero adaptation jobs. `transfer_execution` observes actual
map/gather/presence jobs, separate from core and delegate counters. Descriptor-view
construction and dependent prefix scans are serial, bounded and cancellation-
checked. Bulk positions and dense/ragged scalar copying use bounded scalar blocks,
including long rows. The fixed public XYZ row accessor may read its complete
three-component row while a callback copies a partial flat range; this is not an
unbounded serial copy or fixed cancellation-latency promise.

Core parallel eligibility retains the face threshold/shared worker policy.
`core.workers_used` counts actually launched and joined workers on normal return;
its serial reason is a closed classification. Expected outcomes and unexpected
worker exceptions are stored by source face. All workers join before unexpected
exceptions are scanned in face order, then stop is observed, then expected face
outcomes are scanned. Partial launch failure joins already launched workers before
their input/records/leases are destroyed. Requested/reserved slots are not claimed
as actual launches.

`core.new_requested_peak_bytes` is observed at actual full backend requests,
including the initial operation-accounting allocation and overlapping/in-flight
requests. An admission refusal before a backend call does not increase it.
Backend attempts are included in the observation; an escaping backend exception
produces no normal result. It is not a modeled or sampled root-context peak,
cumulative traffic, resident memory or process RSS. Native OS/CRT thread and
standard exception-runtime internals retain their existing opaque boundary.

## Ownership and accounting

Legacy source backing and the supplied execution handle are borrowed for the
entire call, including all joined workers. Keep them alive and unchanged; do not
mutate, assign, move or destroy them concurrently with the call. Separate calls
may read the same immutable legacy source with distinct execution copies, sharing
the existing ledger's worker/payload contention. Retained input copies its strong
owner internally, but its visible source handle must still remain alive and
unassigned throughout the call; no early-release or same-handle mutation race is
promised by that implementation detail.

All actual buffers are admitted before allocation and remain charged for their
real overlapping lifetimes. Core plans/limbs remain charged while maps, gathered
channels, output storage and inspection are constructed. No public legacy result
is lifted after return with a late reservation. Copies/rebounds retain their
actual operation, never a default/global/TLS allocator. No core-backed allocation
or borrowed observer escapes into the strong output or retained facts.

The owner has no public default or move-empty state. Copies allocate nothing and
retain real requested charges until final release. Borrowed references/spans
require a visible alive, unassigned owner. Separately copied `Mesh()` or
`Inspection()` handles keep their actual storage alive after maps are released.
Immutable output storage and views obtained from distinct live copies may be
read concurrently. Copying and const reads of a handle may overlap. Assignment
or destruction of that same visible handle must not overlap any access/copy or
use of its borrowed references without external synchronization. A different
surviving copy can independently keep and read its own views while another copy
is released. The class has no public move-empty operation; this does not permit
using a view after its visible borrowing handle was reassigned or destroyed.
`NewRequestedBytes()` sums the three distinct retained operation owners once:
bridge/maps, output materialization and output inspection. The inspection's
retained source is not counted twice. Original/captured source and released core
or adaptation temporaries are excluded. This live metric is distinct from the
unchanged legacy conservative `peak_tracked_payload_bytes`.

Last destruction frees backing before releasing charges, allocates nothing and
ignores cancellation. Execution handles retain their ledger; a private injected
allocation backend's external state still must outlive every allocated block.
Getters perform no validation or allocation and do not expose backend pointers.

## Build and qualification boundary

Build with the existing [Geometry dependency and profile setup](../ENVIRONMENT.md).
The new static target depends on `meshvale::triangulation` and
`meshvale::materialization`. The legacy target does not acquire a materialization
dependency. Public installed consumers need current matching headers/libraries
and Threads, without private source or Eigen headers.

```text
cmake -S examples/charged-triangulation-consumer -B .local/charged-consumer -DCMAKE_PREFIX_PATH=<installed-prefix> -DMESHVALE_CHARGED_EXPECTED_PROFILE=qualified
cmake --build .local/charged-consumer --config Release
ctest --test-dir .local/charged-consumer -C Release --output-on-failure
```

Use `unsupported` only for a genuinely unsupported producer. That consumer checks
real nonempty capture refusal and actual zero-numerical empty behavior; a test
selector never makes a qualified producer into an unsupported one. New qualified
native suites cover complete channels/maps, real allocation/peak/worker controls
and independent rational geometry through public output. Existing ordinary-new
interception follows the existing allocation-probe option; backend/accounting
controls remain substantive when that interception is disabled.

The applicable PR and main-branch [repository checks](../.github/workflows/repository-checks.yml)
passed for the declared configurations. Qualified producers exercise successful
operations; unsupported producers retain the tested rejection and empty-storage
behavior. A test selector does not enable nonempty capture on an unsupported
producer.

A separate Windows x64/MSVC 19.51 Release/MD development cohort passed 19 native
tests, 12 baseline/current legacy comparisons, a twice-included public header and
a relocated installed consumer. A genuine Git source archive of the
[feature commit](https://github.com/Meshvale/meshvale-geometry/commit/5dd210b19d05bf651b853b8fdac398c7feceeba9),
whose product tree matches the merged implementation, passed its own 19-test
native run. Its coherent SDK contained 13 public headers and six static libraries
in 33 files; a copied public consumer passed after byte-exact SDK relocation.
These results cover the tested development configurations and matching producers.

Stable compatibility, supported release/floor, destination loss/conversion and
complete common-format workflows remain separate gates. This module has no
Python binding and does not encode destination formats.
