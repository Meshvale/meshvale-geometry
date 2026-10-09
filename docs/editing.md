# Pooled mesh editing

| Field | Value |
|---|---|
| ID | MESHVALE-EDIT-001 |
| Version | 0.2.0 |
| Status | Native development interface; no stable API/ABI or release |
| Owner | Geometry |
| Related | [Raw storage](attributes.md), [incidence inspection](topology.md) |

This contract owns the native pooled editor in
[`editable_mesh.h`](../include/meshvale/geometry/editable_mesh.h), linked through
`meshvale::editing`. The current `Mesh` record remains a dense
interchange/algorithm input with its existing mutation limits. Python continues
to expose the separate immutable raw-record interface; it does not expose the
editor yet.

## Objects and ownership

**EDIT-001.** `EditableMesh` is the owning aggregate. `Vertex`, `Edge`, `Face` and
`Corner` are typed, read-only element objects referring to identities in that
aggregate. Private pools own their records; callers never own or delete pooled
records directly. Object access resolves identity through the owner rather than
retaining an address into a movable buffer. No shared polymorphic base class is
required merely because these objects are mesh elements.

**EDIT-002.** Identity checks include element kind, owner, slot and generation.
Live objects survive unrelated insertions, deletions, pool relocation and dense
snapshot reordering. Deletion invalidates that identity; slot reuse never makes
an old object valid again. Generation exhaustion retires the slot or rejects
allocation. Destroyed-owner and foreign-owner access fail explicitly.

Moving an owner preserves its identity. Copying an editing aggregate is an
explicit fork with a fresh owner and correspondence; objects from the original
are not accepted by the fork. Runtime handles are not serialized identifiers.
Raw pointers and borrowed mutable property rows are not durable element objects.

## Topology and properties

**EDIT-003.** Faces own ordered corner cycles. Each corner references its vertex,
face and the edge toward the next corner. Edges have explicit identities and
endpoint references, with every incident corner retained. Wire edges, parallel
edges with equal endpoints, non-manifold edges, repeated corners and disconnected
vertex fans are distinct representable cases. Topology integrity checks are
separate from manifoldness, planarity and geometric validity.

An insertion supplies explicit edges or requests an explicitly documented edge
construction policy. Implicit endpoint-pair grouping cannot be used as evidence
of authored edge identity. Converting current raw `Mesh` records must report the
derived-edge policy, since those records contain no authored edge table.

**EDIT-004.** A private property store associates rows with element identity.
Channel descriptors retain domain/name, scalar type, components, semantic/set
metadata, missingness and dense/ragged shape. Multiple UV sets and arbitrary
joint/weight row lengths remain independent channels. Deleting or reordering an
element cannot silently transfer its values to another element. New rows require
an explicit supplied/default/missing policy; storage does not infer interpolation,
weight normalization or unknown-channel semantics.

Edge-domain properties belong to the explicit edge model. They cannot be added
to the existing raw record by treating a derived adjacency row as persistent identity.
Typed property objects also check owner/domain/generation: removing and recreating
a channel with the same name never revives a stale property reference.

## Editing and observation

**EDIT-005.** `EditSession` owns a unit of work against one accepted mesh revision.
All mutation crosses this interface, including connectivity, positions and
properties. Element objects expose queries; session operations perform insertion,
deletion and replacement. A commit accepts one consistent candidate and returns
changes/correspondence. Invalid operations, failed verification, cancellation and
discarded sessions leave the accepted state unchanged.

Pending objects belong to their creating session until commit; another session
or the accepted mesh cannot resolve them prematurely. Discarded-session objects
must never revive through later allocation. Identity issuance therefore cannot
be rolled back with candidate storage. Accepted objects remain valid in the
accepted mesh while their deletion is only pending. Deleting a referenced vertex/edge rejects
by default; any cascading removal is explicit and records affected elements.
Deleting a face removes its corners and updates incidence together. Keeping or
pruning now-unused edges is an explicit policy.

**EDIT-006.** Owned read snapshots capture a revision and remain valid after later
edits. Query results have stated owned/borrowed lifetimes. A session whose base
revision has been replaced cannot commit without an explicit rebase/revalidation
policy. Snapshot iteration order and dense row positions are not identity.
Undo/history is a separate future contract; rollback does not imply resurrection
of deleted objects.

The call flow is `mesh.BeginEdit()`, session-owned creation/property updates,
`session.Commit()`, then `mesh.Snapshot()`. A face construction names each
corner's vertex and outgoing edge. Exact declarations live in the public header.

`session.Snapshot()` owns a candidate observation: `IsCandidate()` is true and
`Revision()` identifies the base accepted revision, not a committed result.
Captured candidate data remains queryable after discard, but its pending objects
never become valid in the accepted mesh. Accepted snapshots report
`IsCandidate()` false. Dense materialization retains the same provenance.

**EDIT-007.** Substantial computation should first evaluate parallel decomposition.
Independent analysis and candidate computation may use immutable owned snapshots
with a declared execution budget. CPU threads, processes and optional GPU work
require actual backend/thread-safety, transfer-cost and numerical evidence.
Mutation commits are coordinated by the owner; parallel commits require proven
conflict detection and topology/property integrity. Thread safety is stated per
interface, not inferred from pooled allocation. Nested execution must respect
the same budget rather than multiply worker counts.

## Dense snapshots and algorithms

**EDIT-008.** Dense snapshots are derived values for algorithms, bindings and
serialization. Materialization returns owner/revision provenance and maps in both
directions for vertices, edges, faces and corners. Surviving runtime identities
remain unchanged when dense ordering changes. Export includes an explicit edge
table where needed; projecting to the current raw `Mesh` must reject or explicitly
report unsupported wires, parallel-edge distinctions or edge properties.

Checked manifold half-edge and triangle views remain optional algorithm views.
Their preconditions, correspondence and revision are explicit. They do not become
a second mutable topology authority. Applying an algorithm result requires its
base revision and its topology/property transfer policy to pass commit validation.

## Implementation and acceptance

The public façade, handle indirection and transactional unit of work hide pool,
incidence and property implementations. Geometric transfer strategies are added
where operations require different policies; arbitrary class hierarchies and
per-element heap ownership are not prerequisites. Pool chunks, free lists and
copy-on-write versus edit journals are implementation choices requiring measured
locality/allocation evidence. Copying the whole mesh per frequent edit is not a
production performance claim.

Acceptance requires real installed-interface tests for stale/foreign/discarded
objects, slot reuse and owner moves/forks; conflicting sessions and failure
rollback; non-manifold/wire/parallel-edge topology; dense and ragged attributes
across multiple UV/influence sets; snapshot lifetime and explicit export maps.
Parallel tests need deterministic results, cancellation and worker-budget evidence.
Native acceptance tests exercise these contracts with active checks in Release
builds, and a separate installed consumer links only the exported editing target.
Platform and sanitizer coverage is recorded by repository checks; a passing
functional concurrency test does not establish race-detector coverage.

## Using the native editor

The owner is move-only. Its typed objects are copyable references to checked
identity, without setters or exposed pool positions. A default object is invalid;
`IsValid()` returns false. Queries on stale or destroyed-owner objects throw
`EditorError`. A default snapshot throws `kInvalidObject` on access. Snapshot
queries resolve their captured state, even when an object is now stale in the
accepted owner.

```cpp
#include <meshvale/geometry/editable_mesh.h>

meshvale::geometry::EditableMesh mesh;
auto edit = mesh.BeginEdit();
auto vertex = edit.CreateVertex({1.0, 2.0, 3.0});
// vertex.IsValid() is false until this session commits.
auto changes = edit.Commit();
auto snapshot = mesh.Snapshot();
auto position = snapshot.Position(vertex);
```

`CreateFace` requires at least three ordered corners and edges connecting each
corner vertex to its successor. It accepts repeated vertices and concave or
non-planar loops; it verifies connectivity rather than geometric validity.
`ImportMesh` requires raw storage inspection to pass and reports
`derived-undirected-endpoint-pair`. Its correspondence preserves source dense
vertex/face/corner order and includes the derived edge table. Face winding and
corner channel values are retained.

Properties cover vertex, edge, face and corner domains. Names are unique within
a domain. `PropertyDescriptor` declares scalar type, component width, ragged
shape and the policy for new elements. Missing rows retain typed backing values;
neither UV interpolation nor skin-weight normalization is implicit.
`kRequireExplicit` rejects creation without a supplied row and cannot initialize
an already populated domain without rows. A property descriptor/default is an
editing policy; raw `Mesh` export retains channel values and metadata but has no
field for this future-row policy or runtime identity.

Expected failures carry an `EditorErrorCode`; allocation and worker-creation
failures propagate as standard exceptions. Failed mutators retain the previous
candidate as well as accepted state. A canceled/conflicting commit leaves the
session available for inspection/discard. Successful commit closes it. No implicit
rebase, topology interpolation, undo or history-resurrection policy is provided.

## Execution and thread safety

An `EditorSnapshot` is immutable and shareable between threads. Different
sessions may compute candidates concurrently; each session is confined to the
thread that began it. The owner serializes publication, and exactly one
same-base candidate can commit before the others conflict. Current object
queries and accepted snapshot capture coordinate with commits. Moving or
destroying the aggregate object itself requires external synchronization with
calls on that same C++ object; this does not prevent retained snapshots from
outliving it.

`ComputeBounds(snapshot, execution, stop)` partitions immutable vertex pages
across CPU threads and reduces exact componentwise minima/maxima in traversal
order, without fast-math. Coordinates are finite by the editor's storage
preconditions. `ExecutionContext` copies share a budget for library-created
worker threads across overlapping calls. External caller threads are outside
that cap. `ActiveWorkers()`/`PeakWorkers()` count reserved worker slots;
`BoundsResult::workers_used` reports threads successfully created and joined
for that call. Reservations are released on cancellation and exceptions.

A zero configured budget selects hardware concurrency, with a minimum of one.
The default parallel threshold is 65,536 live vertices. Worker count is also
limited by a grain of half the threshold, with a minimum grain of one. Empty
input, small input,
a budget of one, unavailable shared capacity or fewer than two pool pages use
the caller thread and supply a `serial_reason`. Threshold and budget are explicit
options, not a guarantee that a parallel run is faster. GPU and process backends
are not implemented. Backend changes require their own transfer, safety and
numerical proof under EDIT-007.

## Storage cost and installation

The implementation uses private 64-record copy-on-write pages. A local edit
copies the affected pages and their record payloads; retained snapshots share
unchanged pages. Beginning a session, staging a mutator and committing still
copy page-directory/free-list metadata and change logs. This is not constant-time
editing, and many single-element mutators can accumulate substantial bookkeeping
cost. Bulk raw import builds unpublished storage directly and produces full
dense correspondence, with its traversal and allocation cost. Adding a property
initializes its whole domain; full dense materialization/export traverses and
copies the mesh. Large-scale batch editing and undo remain future work.

Use the [native build/install commands](attributes.md#build-and-installed-consumer).
The raw target `meshvale::geometry` remains header-only; `meshvale::editing` is a
compiled static C++20 target with the platform thread dependency. The separate
[`editing-consumer`](../examples/editing-consumer/CMakeLists.txt) uses
`find_package(MeshvaleGeometry CONFIG REQUIRED)` without source include paths.
Native installed binaries need a matching compiler/runtime configuration; the
unreleased package version supplies no stable ABI guarantee.
