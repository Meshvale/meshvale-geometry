# Pooled mesh editing

| Field | Value |
|---|---|
| ID | MESHVALE-EDIT-001 |
| Version | 0.1.0 |
| Status | Draft interface contract; not implemented |
| Owner | Geometry |
| Related | [Raw storage](attributes.md), [incidence inspection](topology.md) |

This contract describes the selected direction for runtime editing. The current
`Mesh` record remains a dense interchange/algorithm input with its existing
mutation limits. There is no `EditableMesh` interface in the installed package yet.

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

The intended call flow is `mesh.BeginEdit()`, session-owned creation/property
updates, `session.Commit()`, then `mesh.Snapshot()`. A face construction names
each corner's vertex and outgoing edge. This is interface direction rather than
compilable API; exact declarations will live in the future public header.

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
No current library test establishes these future editing guarantees.
