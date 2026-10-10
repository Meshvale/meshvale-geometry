# Exact polygon triangulation

The native interface and [Python `triangulate`](python.md#python-polygon-conversion)
perform the same explicit conversion and return owned source correspondence.

This development C++20 operation explicitly converts polygons to triangles.
The [header](../include/meshvale/geometry/triangulation.h) owns the API and the
compiled static target is `meshvale::triangulation`. Python conversion uses the
same native operation. Stable API/ABI guarantees are forthcoming.

```cpp
#include <meshvale/geometry/triangulation.h>

meshvale::geometry::ExecutionContext execution({4, 65536});
meshvale::geometry::TriangulationOptions options;
auto result = meshvale::geometry::Triangulate(source, options, execution);
if (result.status == meshvale::geometry::TriangulationStatus::kAccepted) {
  // result.mesh owns triangles. result.corner_sources identifies authored rows.
}
```

## Supported numerical profile

Input faces are ordered simple single loops of existing vertices in arbitrary
3D orientation. Convex/concave faces, both windings, mixed face sizes and
collinear authored boundary corners are supported. Represented binary64
coordinates must be exactly planar with nonzero area. Exact integer-grid
arithmetic decides planarity, orientation, intersections and geometric ears.
There is no epsilon, area underflow threshold, index fan or generated vertex.
Projection drops the largest absolute exact plane-normal component.

Coincident points, collinear faces, adjacent backtracking, nonadjacent boundary
crossing/touch/overlap and represented-nonplanarity are diagnosed. Tiny nonzero
geometry and the finite binary64 exponent range do not become degenerate through
floating area overflow/underflow. Positions retain original bits, including
signed zero. Commonly rounded coordinates intended to form a plane can fail
exact coplanarity; `conversion.nonplanar_face` is a profile outcome, with no
implicit tolerance or geometric modification. Multiple rings/holes, nonplanar
triangulation and generated intersections are unsupported.

`max_corners_per_face` defaults to 4096 and can be lowered to at least three.
Exceeding that selected limit diagnoses `conversion.face_corner_limit`; selecting
less than three or more than 4096 diagnoses `conversion.unsupported_corner_limit`.
The size ceiling does not establish throughput at that ceiling. Cross-face
intersections, manifoldness, solids, arbitrary channel semantics and asset/skin
bindings are outside this operation's checks.

## Complete result and correspondence

Only `kAccepted` contains a complete independent Mesh and maps:

- `face_sources[i]` identifies the authored source face for output triangle i.
- `corner_sources[i]` identifies the actual authored source corner for output
  corner i, distinct from its geometric vertex index.
- Consecutive `face_output_offsets` entries delimit the output triangles of each
  source face. There are source-face-count plus one entries, beginning at zero.

Positions and vertex rows retain source order. Face rows replicate and corner
rows transfer through these maps. Every named channel retains scalar type and
bits, dense/ragged shape, presence masks and missing backing, component width,
domain/name, semantic/set labels and metadata. No normalization, interpolation
or deduplication occurs. Triangle topology is explicit conversion; it does not
preserve or reconstruct authored polygon faces. UV seams and arbitrary stored
ragged influences remain ordinary channel rows.

Empty meshes are accepted with `{0}` face-output offsets. `kBlocked` and
`kCanceled` expose no accepted Mesh or partial maps. Geometric diagnostics use
subject `face` and a source-face element index. Malformed data reports its first
storage diagnostic before unsafe geometry access. Declared-payload admission
failure diagnoses `conversion.payload_budget`; checked count/byte overflow
diagnoses `conversion.size_overflow`.

Keep `source` alive and unchanged for the entire call. The implementation
captures owned immutable storage before worker computation and never modifies
the source. Separate calls can share immutable source data and execution copies.

## Shared execution and cancellation

`minimum_parallel_faces` defaults to 64. Eligible independent faces partition
across CPU workers under the same `ExecutionContext` reservation cap as
`ComputeBounds`. Context copies share the cap across overlapping/nested calls.
Low face count, a one-worker budget or unavailable slots produce a reported
serial caller fallback. At least two faces/slots are required; one large face
remains serial. `workers_used` counts successfully created and joined threads.
Reserved slots and external caller threads are not a process-wide thread count.

Per-face work is isolated. Output/maps assemble in source-face order with stable
ear choices and expected diagnostic ordering. All workers join before acceptance.
Stop requests take priority over expected blocked outcomes. Unexpected worker
exceptions are rethrown in source-face order before this expected-outcome
cancellation policy; ordinary allocation/thread-creation exceptions propagate.
No exception escapes a worker. Moved-from contexts retain `EditorError` behavior.

Stop tokens are checked during capture/admission, exact coordinate construction,
repeated-position/plane/boundary/ear work, transfer and final acceptance. This is
cooperative cancellation without fixed latency. Failure/cancellation publishes
no accepted partial mesh; RAII releases reservations.

## Declared payload budget

`ExecutionOptions::tracked_payload_budget_bytes` defaults to 256 MiB. Existing
two-field aggregate option callers remain valid. A zero budget rejects conversion.
Bounds share worker accounting and reserve no triangulation payload.

Before construction, conversion reserves conservative declared bytes for captured
Mesh storage, retained face triangle triples, per-lane numerical/ear scratch,
complete output Mesh/maps and channel backing. Byte/count arithmetic is checked.
Declared Mesh/Attribute objects, metadata pair objects, face-outcome descriptors,
integer descriptors, scalar/index/coordinate storage and string bytes are charged.
Per-lane scratch admits 68 32-bit limbs per copied coordinate plus 64 KiB for
bounded predicate temporaries; the exact binary64 coordinate bound is 66 limbs.
Assembly is charged while earlier work buffers still coexist.

`peak_tracked_payload_bytes` is this call's cumulative conservative reservation,
not allocated bytes or RSS. Context `TrackedPayloadBudget()`,
`ActiveTrackedPayload()` and `PeakTrackedPayload()` expose shared reservations;
the peak is a lifetime peak. Contention can reject admission; there is no payload
waiting or separate independent operation cap.

Accounting excludes allocator bookkeeping, unused capacity, tree-node linkage/
implementation bookkeeping, thread stacks, external input/Python objects,
unrelated runtime memory and process totals. After an accepted result returns,
its retained lifetime is outside active computation accounting. Keeping many
returned results is not bounded by the context. These metrics do not establish
a process memory limit or replace allocator exception handling.

## Build and checks

Follow [native build/install instructions](attributes.md#build-and-installed-consumer).
Native acceptance tests require Python 3.10+ for the independent Fraction oracle;
`BUILD_TESTING=OFF` native builds do not require Python. The
[installed triangulation consumer](../examples/triangulation-consumer/CMakeLists.txt)
uses only exported targets and independently includes the canonical header.

Native tests check row/bit maps, numerical failures, serial/parallel agreement,
shared bounds/worker/payload admission, cancellation/no-partial-result and budget
boundaries. The Fraction oracle verifies containment, signed area, zero triangle
interior overlap, authored boundary winding and every channel/map. Applicable
CI sanitizer jobs run these targets. Platform/package claims require actual
checks for that build; conversion does not establish destination float32 or
format publication guarantees.
