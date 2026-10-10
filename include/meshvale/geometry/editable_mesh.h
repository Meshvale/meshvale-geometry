// SPDX-License-Identifier: Apache-2.0
#ifndef MESHVALE_GEOMETRY_EDITABLE_MESH_H_
#define MESHVALE_GEOMETRY_EDITABLE_MESH_H_

#include <meshvale/geometry/mesh.h>

#include <array>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <utility>
#include <vector>

namespace meshvale::geometry {
namespace execution_detail {
struct Access;
}
namespace editing_detail {
struct Owner;
struct Session;
struct Snapshot;
struct Access;
struct Execution;
}  // namespace editing_detail

enum class ElementKind { kVertex, kEdge, kFace, kCorner, kProperty };
class ElementIdentity {
 public:
  ElementIdentity() = default;
  [[nodiscard]] std::uint64_t OwnerIdentity() const noexcept { return owner_; }
  [[nodiscard]] ElementKind Kind() const noexcept { return kind_; }
  auto operator<=>(const ElementIdentity&) const = default;

 private:
  friend struct editing_detail::Access;
  std::uint64_t owner_ = 0;
  std::uint64_t slot_ = 0;
  std::uint64_t generation_ = 0;
  ElementKind kind_ = ElementKind::kVertex;
};
enum class EditorErrorCode {
  kInvalidObject,
  kForeignOwner,
  kOwnerDestroyed,
  kSessionClosed,
  kWrongThread,
  kReferencedElement,
  kInvalidTopology,
  kInvalidProperty,
  kRevisionConflict,
  kCanceled,
  kIdentityExhausted,
  kUnsupportedProjection
};
class EditorError : public std::runtime_error {
 public:
  EditorError(EditorErrorCode code, std::string message);
  [[nodiscard]] EditorErrorCode Code() const noexcept;

 private:
  EditorErrorCode code_;
};

class Edge;
class Face;
class Corner;
class Property;
class Vertex {
 public:
  Vertex() = default;
  [[nodiscard]] ElementIdentity Identity() const noexcept;
  [[nodiscard]] bool IsValid() const;
  [[nodiscard]] std::array<double, 3> Position() const;
  [[nodiscard]] std::vector<Edge> Edges() const;
  [[nodiscard]] std::vector<Corner> Corners() const;

 private:
  friend struct editing_detail::Access;
  std::weak_ptr<editing_detail::Owner> owner_;
  ElementIdentity identity_;
};
class Edge {
 public:
  Edge() = default;
  [[nodiscard]] ElementIdentity Identity() const noexcept;
  [[nodiscard]] bool IsValid() const;
  [[nodiscard]] std::array<Vertex, 2> Endpoints() const;
  [[nodiscard]] std::vector<Corner> Corners() const;

 private:
  friend struct editing_detail::Access;
  std::weak_ptr<editing_detail::Owner> owner_;
  ElementIdentity identity_;
};
class Face {
 public:
  Face() = default;
  [[nodiscard]] ElementIdentity Identity() const noexcept;
  [[nodiscard]] bool IsValid() const;
  [[nodiscard]] std::vector<Corner> Corners() const;

 private:
  friend struct editing_detail::Access;
  std::weak_ptr<editing_detail::Owner> owner_;
  ElementIdentity identity_;
};
class Corner {
 public:
  Corner() = default;
  [[nodiscard]] ElementIdentity Identity() const noexcept;
  [[nodiscard]] bool IsValid() const;
  [[nodiscard]] Vertex GetVertex() const;
  [[nodiscard]] Edge GetEdge() const;
  [[nodiscard]] Face GetFace() const;

 private:
  friend struct editing_detail::Access;
  std::weak_ptr<editing_detail::Owner> owner_;
  ElementIdentity identity_;
};

enum class PropertyDomain { kVertex, kEdge, kFace, kCorner };
enum class PropertyScalarType {
  kFloat32,
  kFloat64,
  kInt32,
  kUint8,
  kUint16,
  kUint32,
  kUint64
};
enum class NewRowPolicy { kMissing, kDefault, kRequireExplicit };
struct PropertyRow {
  AttributeValues values = std::vector<float>{};
  bool present = true;
};
struct PropertyDescriptor {
  PropertyDomain domain = PropertyDomain::kVertex;
  std::string name;
  PropertyScalarType scalar_type = PropertyScalarType::kFloat32;
  std::uint32_t components = 1;
  bool ragged = false;
  std::string semantic;
  std::optional<std::uint32_t> set_index;
  std::map<std::string, std::string> metadata;
  NewRowPolicy new_row_policy = NewRowPolicy::kMissing;
  std::optional<PropertyRow> default_row;
};
class Property {
 public:
  Property() = default;
  [[nodiscard]] ElementIdentity Identity() const noexcept;
  [[nodiscard]] bool IsValid() const;
  [[nodiscard]] PropertyDescriptor Descriptor() const;
  [[nodiscard]] PropertyRow Row(ElementIdentity element) const;

 private:
  friend struct editing_detail::Access;
  std::weak_ptr<editing_detail::Owner> owner_;
  ElementIdentity identity_;
};
struct PropertyAssignment {
  Property property;
  PropertyRow row;
};
struct FaceCorner {
  Vertex vertex;
  Edge edge;
  std::vector<PropertyAssignment> properties;
};
enum class ErasePolicy { kRejectReferenced, kCascade };
enum class UnusedEdgePolicy { kKeep, kPrune };

struct DenseProperty {
  Property property;
  PropertyDescriptor descriptor;
  AttributeValues values;
  std::optional<std::vector<index_t>> offsets;
  std::vector<std::uint8_t> present;
};
struct DenseEditorMesh {
  std::uint64_t owner = 0;
  std::uint64_t revision = 0;
  bool is_candidate = false;
  std::vector<Vertex> vertices;
  std::vector<Edge> edges;
  std::vector<Face> faces;
  std::vector<Corner> corners;
  std::map<ElementIdentity, index_t> vertex_rows, edge_rows, face_rows,
      corner_rows;
  std::vector<std::array<double, 3>> positions;
  std::vector<std::array<index_t, 2>> edge_vertices;
  std::vector<index_t> face_offsets{0};
  std::vector<index_t> corner_vertices, corner_edges, corner_faces;
  std::vector<DenseProperty> properties;
};

// All returned collections/rows are owned values. A snapshot remains usable
// after edits, owner moves and destruction. Its objects resolve through the
// snapshot's methods; ordinary object methods query only current accepted
// state.
class EditorSnapshot {
 public:
  // A default-constructed snapshot has no state; queries throw kInvalidObject.
  EditorSnapshot() = default;
  [[nodiscard]] std::uint64_t OwnerIdentity() const;
  [[nodiscard]] std::uint64_t Revision() const;
  [[nodiscard]] bool IsCandidate() const;
  [[nodiscard]] std::vector<Vertex> Vertices() const;
  [[nodiscard]] std::vector<Edge> Edges() const;
  [[nodiscard]] std::vector<Face> Faces() const;
  [[nodiscard]] std::vector<Corner> Corners() const;
  [[nodiscard]] std::vector<Property> Properties() const;
  [[nodiscard]] std::array<double, 3> Position(Vertex vertex) const;
  [[nodiscard]] std::array<Vertex, 2> Endpoints(Edge edge) const;
  [[nodiscard]] std::vector<Corner> Corners(Face face) const;
  [[nodiscard]] std::vector<Corner> Corners(Edge edge) const;
  [[nodiscard]] PropertyDescriptor Descriptor(Property property) const;
  [[nodiscard]] PropertyRow Row(Property property,
                                ElementIdentity element) const;
  [[nodiscard]] DenseEditorMesh Materialize() const;
  [[nodiscard]] Mesh ExportMesh() const;

 private:
  friend struct editing_detail::Access;
  std::shared_ptr<const editing_detail::Snapshot> snapshot_;
};
struct CommitResult {
  std::uint64_t revision = 0;
  std::vector<ElementIdentity> created;
  std::vector<ElementIdentity> erased;
};

// Sessions are move-only and confined to the thread that begins them. Different
// sessions and accepted reads may run concurrently. No session keeps the owner
// alive as a mesh: operations fail explicitly after the aggregate is destroyed.
// Expected failures throw EditorError; allocation failures propagate. Every
// failed operation/commit preserves accepted state and the previous candidate.
class EditSession {
 public:
  EditSession(EditSession&&) noexcept;
  EditSession& operator=(EditSession&&) noexcept;
  ~EditSession();
  EditSession(const EditSession&) = delete;
  EditSession& operator=(const EditSession&) = delete;
  [[nodiscard]] Vertex CreateVertex(
      std::array<double, 3> position,
      std::span<const PropertyAssignment> properties = {});
  [[nodiscard]] Edge CreateEdge(
      Vertex first, Vertex second,
      std::span<const PropertyAssignment> properties = {});
  [[nodiscard]] Face CreateFace(
      std::span<const FaceCorner> corners,
      std::span<const PropertyAssignment> properties = {});
  void SetPosition(Vertex vertex, std::array<double, 3> position);
  void EraseVertex(Vertex vertex,
                   ErasePolicy policy = ErasePolicy::kRejectReferenced);
  void EraseEdge(Edge edge,
                 ErasePolicy policy = ErasePolicy::kRejectReferenced);
  void EraseFace(Face face, UnusedEdgePolicy policy = UnusedEdgePolicy::kKeep);
  [[nodiscard]] Property CreateProperty(PropertyDescriptor descriptor);
  void RemoveProperty(Property property);
  void SetPropertyRow(Property property, ElementIdentity element,
                      PropertyRow row);
  [[nodiscard]] EditorSnapshot Snapshot() const;
  [[nodiscard]] CommitResult Commit(std::stop_token stop = {});
  void Discard();

 private:
  friend struct editing_detail::Access;
  explicit EditSession(std::unique_ptr<editing_detail::Session> session);
  std::unique_ptr<editing_detail::Session> session_;
};

struct ForkResult;
struct ImportResult;
class EditableMesh {
 public:
  EditableMesh();
  EditableMesh(EditableMesh&&) noexcept;
  EditableMesh& operator=(EditableMesh&&) noexcept;
  ~EditableMesh();
  EditableMesh(const EditableMesh&) = delete;
  EditableMesh& operator=(const EditableMesh&) = delete;
  [[nodiscard]] EditSession BeginEdit();
  [[nodiscard]] EditorSnapshot Snapshot() const;
  [[nodiscard]] ForkResult Fork() const;
  [[nodiscard]] static ImportResult ImportMesh(const Mesh& mesh);

 private:
  std::shared_ptr<editing_detail::Owner> owner_;
};
struct ElementCorrespondence {
  std::map<ElementIdentity, ElementIdentity> original_to_fork;
  std::map<ElementIdentity, ElementIdentity> fork_to_original;
};
struct ForkResult {
  EditableMesh mesh;
  ElementCorrespondence correspondence;
};
// Raw records have no authored edge identities. Import derives one undirected
// edge per unordered endpoint pair (including self pairs), never manifold
// twins.
struct ImportResult {
  EditableMesh mesh;
  std::string edge_policy = "derived-undirected-endpoint-pair";
  DenseEditorMesh correspondence;
};

struct ExecutionOptions {
  std::size_t worker_budget = 0;  // 0 selects hardware concurrency, at least 1.
  std::size_t minimum_parallel_vertices = 65536;
  // Cap on declared payload reservations of active computations, not total RSS.
  std::size_t tracked_payload_budget_bytes = 256 * 1024 * 1024;
};
// Copies share one cap on library-owned worker threads across overlapping or
// nested calls. External caller threads are not created/accounted by this API.
class ExecutionContext {
 public:
  explicit ExecutionContext(ExecutionOptions options = {});
  [[nodiscard]] std::size_t WorkerBudget() const;
  [[nodiscard]] std::size_t ActiveWorkers() const;
  [[nodiscard]] std::size_t PeakWorkers() const;
  [[nodiscard]] std::size_t TrackedPayloadBudget() const;
  [[nodiscard]] std::size_t ActiveTrackedPayload() const;
  [[nodiscard]] std::size_t PeakTrackedPayload() const;

 private:
  friend struct editing_detail::Access;
  friend struct execution_detail::Access;
  std::shared_ptr<editing_detail::Execution> execution_;
};
struct Bounds {
  std::array<double, 3> minimum, maximum;
};
struct BoundsResult {
  std::optional<Bounds> bounds;
  std::size_t workers_used = 0;  // 0 means the caller performed serial work.
  std::string serial_reason;
};
// Exact componentwise min/max with stable traversal/reduction, no fast-math.
// Cancellation throws kCanceled. Snapshots are immutable and may be shared.
[[nodiscard]] BoundsResult ComputeBounds(const EditorSnapshot& snapshot,
                                         const ExecutionContext& execution,
                                         std::stop_token stop = {});

}  // namespace meshvale::geometry
#endif  // MESHVALE_GEOMETRY_EDITABLE_MESH_H_
