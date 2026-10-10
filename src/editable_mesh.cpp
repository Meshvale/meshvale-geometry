// SPDX-License-Identifier: Apache-2.0
#include "meshvale/geometry/editable_mesh.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <limits>
#include <mutex>
#include <set>
#include <thread>
#include <type_traits>

#include "execution_internal.h"

namespace meshvale::geometry {
namespace editing_detail {
constexpr std::size_t kPageSize = 64;
constexpr std::size_t kMinimumFaceCornerCount = 3;
struct Id {
  std::uint64_t slot = 0;
  std::uint64_t generation = 0;
  auto operator<=>(const Id&) const = default;
};
[[noreturn]] void Fail(EditorErrorCode code, const char* message) {
  throw EditorError(code, message);
}

template <class T>
struct Pool {
  struct Slot {
    std::uint64_t generation = 0;
    std::optional<T> value;
  };
  using Page = std::array<Slot, kPageSize>;
  std::vector<std::shared_ptr<Page>> pages;
  std::vector<std::uint64_t> free_slots;
  std::uint64_t size = 0;

  bool Contains(Id id) const {
    return id.slot < size &&
           (*pages[static_cast<std::size_t>(id.slot / kPageSize)])[id.slot %
                                                                   kPageSize]
                   .generation == id.generation &&
           (*pages[static_cast<std::size_t>(id.slot / kPageSize)])[id.slot %
                                                                   kPageSize]
               .value.has_value();
  }
  const T& Get(Id id) const {
    if (!Contains(id))
      Fail(EditorErrorCode::kInvalidObject,
           "Element is stale, invalid or pending in another state");
    return *(*pages[static_cast<std::size_t>(id.slot / kPageSize)])[id.slot %
                                                                    kPageSize]
                .value;
  }
  Slot& MutableSlot(std::uint64_t slot) {
    const auto page = static_cast<std::size_t>(slot / kPageSize);
    if (pages[page].use_count() != 1)
      pages[page] = std::make_shared<Page>(*pages[page]);
    return (*pages[page])[slot % kPageSize];
  }
  T& Mutate(Id id) {
    Get(id);
    return *MutableSlot(id.slot).value;
  }
  Id Add(T value, std::uint64_t generation) {
    std::uint64_t slot;
    if (free_slots.empty()) {
      if (size == std::numeric_limits<std::uint64_t>::max())
        Fail(EditorErrorCode::kIdentityExhausted, "Pool capacity exhausted");
      slot = size;
      if (slot % kPageSize == 0) pages.push_back(std::make_shared<Page>());
      ++size;
    } else {
      slot = free_slots.back();
      free_slots.pop_back();
    }
    auto& record = MutableSlot(slot);
    record.generation = generation;
    record.value = std::move(value);
    return {slot, generation};
  }
  void Erase(Id id) {
    Get(id);
    free_slots.push_back(id.slot);
    MutableSlot(id.slot).value.reset();
  }
  // Property row slots follow the element slots; generation prevents accidental
  // transfer on reuse. Retained snapshots keep previous pages/row values alive.
  void SetAt(Id id, T value) {
    while (pages.size() <= id.slot / kPageSize)
      pages.push_back(std::make_shared<Page>());
    size = std::max(size, id.slot + 1);
    auto& record = MutableSlot(id.slot);
    record.generation = id.generation;
    record.value = std::move(value);
  }
  void ClearAt(Id id) {
    if (Contains(id)) MutableSlot(id.slot).value.reset();
  }
  template <class F>
  void Each(F&& callback) const {
    for (std::uint64_t slot = 0; slot < size; ++slot) {
      const auto& record =
          (*pages[static_cast<std::size_t>(slot / kPageSize)])[slot %
                                                               kPageSize];
      if (record.value) callback(Id{slot, record.generation}, *record.value);
    }
  }
};
struct VertexRecord {
  std::array<double, 3> position;
  std::vector<Id> edges, corners;
};
struct EdgeRecord {
  std::array<Id, 2> endpoints;
  std::vector<Id> corners;
};
struct FaceRecord {
  std::vector<Id> corners;
};
struct CornerRecord {
  Id vertex, edge, face;
};
struct PropertyRecord {
  PropertyDescriptor descriptor;
  Pool<PropertyRow> rows;
};
struct State {
  std::uint64_t revision = 0;
  Pool<VertexRecord> vertices;
  Pool<EdgeRecord> edges;
  Pool<FaceRecord> faces;
  Pool<CornerRecord> corners;
  Pool<std::shared_ptr<PropertyRecord>> properties;
};
std::atomic<std::uint64_t> next_owner{1};
std::uint64_t Issue(std::atomic<std::uint64_t>& sequence) {
  auto value = sequence.load(std::memory_order_relaxed);
  for (;;) {
    if (value == std::numeric_limits<std::uint64_t>::max())
      Fail(EditorErrorCode::kIdentityExhausted, "Identity sequence exhausted");
    if (sequence.compare_exchange_weak(value, value + 1,
                                       std::memory_order_relaxed))
      return value;
  }
}
struct Owner {
  std::uint64_t identity = Issue(next_owner);
  std::atomic<std::uint64_t> next_generation{1};
  mutable std::mutex mutex;
  bool alive = true;
  std::shared_ptr<const State> accepted = std::make_shared<State>();
};
struct Snapshot {
  std::uint64_t owner = 0;
  std::weak_ptr<Owner> control;
  std::shared_ptr<const State> state;
  bool candidate = false;
};
struct Session {
  std::shared_ptr<Owner> owner;
  std::shared_ptr<State> candidate;
  std::uint64_t base_revision = 0;
  std::thread::id thread = std::this_thread::get_id();
  bool closed = false;
  bool batch_active = false;
  bool batch_draft = false;
  std::vector<ElementIdentity> created, erased;
};
struct Access {
  static ElementIdentity MakeIdentity(std::uint64_t owner, Id id,
                                      ElementKind kind) {
    ElementIdentity result;
    result.owner_ = owner;
    result.slot_ = id.slot;
    result.generation_ = id.generation;
    result.kind_ = kind;
    return result;
  }
  static Id GetId(ElementIdentity identity) {
    return {identity.slot_, identity.generation_};
  }
  template <class T>
  static T Make(const std::weak_ptr<Owner>& owner, std::uint64_t identity,
                Id id, ElementKind kind) {
    T result;
    result.owner_ = owner;
    result.identity_ = MakeIdentity(identity, id, kind);
    return result;
  }
  static EditorSnapshot MakeSnapshot(const std::shared_ptr<Owner>& owner,
                                     std::shared_ptr<const State> state,
                                     bool candidate = false) {
    EditorSnapshot result;
    result.snapshot_ = std::make_shared<Snapshot>(
        Snapshot{owner->identity, owner, std::move(state), candidate});
    return result;
  }
  static const Snapshot& GetSnapshot(const EditorSnapshot& snapshot) {
    if (!snapshot.snapshot_)
      Fail(EditorErrorCode::kInvalidObject, "Snapshot is empty");
    return *snapshot.snapshot_;
  }
  template <class Element>
  static SnapshotRange<Element> MakeRange(
      std::shared_ptr<const Snapshot> snapshot,
      std::optional<ElementIdentity> incidence = std::nullopt) {
    return SnapshotRange<Element>(std::move(snapshot), incidence);
  }
  static PropertyRowView MakeRowView(std::shared_ptr<const Snapshot> snapshot,
                                     const PropertyRow& row) {
    PropertyRowView result;
    result.snapshot_ = std::move(snapshot);
    result.row_ = &row;
    return result;
  }
  static EditSession MakeSession(std::unique_ptr<Session> session) {
    return EditSession(std::move(session));
  }
  static const std::shared_ptr<Execution>& GetExecution(
      const ExecutionContext& context) {
    if (!context.execution_) {
      Fail(EditorErrorCode::kInvalidObject, "Execution context is moved from");
    }
    return context.execution_;
  }
};
Id Local(ElementIdentity identity) { return Access::GetId(identity); }
ElementKind Kind(PropertyDomain domain) {
  switch (domain) {
    case PropertyDomain::kVertex:
      return ElementKind::kVertex;
    case PropertyDomain::kEdge:
      return ElementKind::kEdge;
    case PropertyDomain::kFace:
      return ElementKind::kFace;
    case PropertyDomain::kCorner:
      return ElementKind::kCorner;
  }
  Fail(EditorErrorCode::kInvalidProperty, "Invalid property domain");
}
bool Contains(const State& state, ElementIdentity identity) {
  switch (identity.Kind()) {
    case ElementKind::kVertex:
      return state.vertices.Contains(Local(identity));
    case ElementKind::kEdge:
      return state.edges.Contains(Local(identity));
    case ElementKind::kFace:
      return state.faces.Contains(Local(identity));
    case ElementKind::kCorner:
      return state.corners.Contains(Local(identity));
    case ElementKind::kProperty:
      return state.properties.Contains(Local(identity));
  }
  return false;
}
void Check(const State& state, std::uint64_t owner, ElementIdentity identity,
           ElementKind expected) {
  if (identity.OwnerIdentity() != owner)
    Fail(EditorErrorCode::kForeignOwner, "Object belongs to another owner");
  if (identity.Kind() != expected || !Contains(state, identity))
    Fail(EditorErrorCode::kInvalidObject,
         "Object is stale, invalid or pending in another state");
}
void CheckRowElement(const State& state, std::uint64_t owner,
                     ElementIdentity element, PropertyDomain domain) {
  Check(state, owner, element, element.Kind());
  if (element.Kind() != Kind(domain)) {
    Fail(EditorErrorCode::kInvalidProperty,
         "Property belongs to another element domain");
  }
}
Snapshot Current(const std::weak_ptr<Owner>& control,
                 ElementIdentity identity) {
  if (identity.OwnerIdentity() == 0) {
    Fail(EditorErrorCode::kInvalidObject, "Object is default initialized");
  }
  auto owner = control.lock();
  if (!owner) Fail(EditorErrorCode::kOwnerDestroyed, "Owner was destroyed");
  std::lock_guard lock(owner->mutex);
  if (!owner->alive)
    Fail(EditorErrorCode::kOwnerDestroyed, "Owner was destroyed");
  Check(*owner->accepted, owner->identity, identity, identity.Kind());
  return {owner->identity, owner, owner->accepted};
}
bool Valid(const std::weak_ptr<Owner>& control, ElementIdentity identity) {
  auto owner = control.lock();
  if (!owner) return false;
  std::lock_guard lock(owner->mutex);
  return owner->alive && identity.OwnerIdentity() == owner->identity &&
         Contains(*owner->accepted, identity);
}
void CheckSession(const Session* session) {
  if (!session || session->closed)
    Fail(EditorErrorCode::kSessionClosed, "Session is closed or moved from");
  if (session->thread != std::this_thread::get_id())
    Fail(EditorErrorCode::kWrongThread,
         "Session is confined to its creating thread");
  std::lock_guard lock(session->owner->mutex);
  if (!session->owner->alive)
    Fail(EditorErrorCode::kOwnerDestroyed, "Owner was destroyed");
}
void CheckMutableSession(const Session* session) {
  CheckSession(session);
  if (session->batch_active)
    Fail(EditorErrorCode::kBatchActive,
         "Session has an active batch; apply or discard it first");
}
void Finite(std::array<double, 3> position) {
  for (double value : position)
    if (!std::isfinite(value))
      Fail(EditorErrorCode::kInvalidTopology, "Position must be finite");
}
AttributeValues EmptyValues(PropertyScalarType type, std::size_t count = 0) {
  switch (type) {
    case PropertyScalarType::kFloat32:
      return std::vector<float>(count);
    case PropertyScalarType::kFloat64:
      return std::vector<double>(count);
    case PropertyScalarType::kInt32:
      return std::vector<std::int32_t>(count);
    case PropertyScalarType::kUint8:
      return std::vector<std::uint8_t>(count);
    case PropertyScalarType::kUint16:
      return std::vector<std::uint16_t>(count);
    case PropertyScalarType::kUint32:
      return std::vector<std::uint32_t>(count);
    case PropertyScalarType::kUint64:
      return std::vector<std::uint64_t>(count);
  }
  Fail(EditorErrorCode::kInvalidProperty, "Invalid scalar type");
}
void ValidateRow(const PropertyDescriptor& descriptor, const PropertyRow& row) {
  const auto type = static_cast<std::size_t>(descriptor.scalar_type);
  if (type > 6 || row.values.index() != type)
    Fail(EditorErrorCode::kInvalidProperty, "Property scalar type differs");
  const auto count =
      std::visit([](const auto& values) { return values.size(); }, row.values);
  if (descriptor.components == 0 || count % descriptor.components != 0 ||
      (!descriptor.ragged && count != descriptor.components))
    Fail(EditorErrorCode::kInvalidProperty, "Property row shape differs");
}
PropertyRow InitialRow(const PropertyDescriptor& descriptor) {
  switch (descriptor.new_row_policy) {
    case NewRowPolicy::kMissing:
      return {EmptyValues(descriptor.scalar_type,
                          descriptor.ragged ? 0 : descriptor.components),
              false};
    case NewRowPolicy::kDefault:
      if (!descriptor.default_row)
        Fail(EditorErrorCode::kInvalidProperty,
             "Default policy requires a default row");
      ValidateRow(descriptor, *descriptor.default_row);
      return *descriptor.default_row;
    case NewRowPolicy::kRequireExplicit:
      Fail(EditorErrorCode::kInvalidProperty,
           "Property row must be explicitly supplied");
  }
  Fail(EditorErrorCode::kInvalidProperty, "Invalid new-row policy");
}
template <class F>
void EachDomain(const State& state, PropertyDomain domain, F&& callback) {
  switch (domain) {
    case PropertyDomain::kVertex:
      state.vertices.Each([&](Id id, const auto&) { callback(id); });
      break;
    case PropertyDomain::kEdge:
      state.edges.Each([&](Id id, const auto&) { callback(id); });
      break;
    case PropertyDomain::kFace:
      state.faces.Each([&](Id id, const auto&) { callback(id); });
      break;
    case PropertyDomain::kCorner:
      state.corners.Each([&](Id id, const auto&) { callback(id); });
      break;
    default:
      Fail(EditorErrorCode::kInvalidProperty, "Invalid property domain");
  }
}
PropertyRecord& MutableProperty(State& state, Id id) {
  auto& record = state.properties.Mutate(id);
  if (record.use_count() != 1)
    record = std::make_shared<PropertyRecord>(*record);
  return *record;
}
void InitializeRows(State& state, std::uint64_t owner, ElementIdentity element,
                    std::span<const PropertyAssignment> assignments) {
  std::map<Id, const PropertyRow*> supplied;
  for (const auto& assignment : assignments) {
    Check(state, owner, assignment.property.Identity(), ElementKind::kProperty);
    const auto id = Local(assignment.property.Identity());
    const auto& descriptor = state.properties.Get(id)->descriptor;
    if (Kind(descriptor.domain) != element.Kind())
      Fail(EditorErrorCode::kInvalidProperty,
           "Property belongs to another element domain");
    ValidateRow(descriptor, assignment.row);
    if (!supplied.emplace(id, &assignment.row).second)
      Fail(EditorErrorCode::kInvalidProperty, "Property row supplied twice");
  }
  std::vector<Id> channels;
  state.properties.Each([&](Id id, const auto& record) {
    if (Kind(record->descriptor.domain) == element.Kind())
      channels.push_back(id);
  });
  for (Id id : channels) {
    auto& record = MutableProperty(state, id);
    const auto found = supplied.find(id);
    record.rows.SetAt(Local(element), found == supplied.end()
                                          ? InitialRow(record.descriptor)
                                          : *found->second);
  }
}
void ClearRows(State& state, ElementIdentity element) {
  std::vector<Id> channels;
  state.properties.Each([&](Id id, const auto& record) {
    if (Kind(record->descriptor.domain) == element.Kind())
      channels.push_back(id);
  });
  for (Id id : channels)
    MutableProperty(state, id).rows.ClearAt(Local(element));
}
template <class T>
std::vector<T> Objects(const Snapshot& snapshot, const std::vector<Id>& ids,
                       ElementKind kind) {
  std::vector<T> result;
  result.reserve(ids.size());
  for (Id id : ids)
    result.push_back(
        Access::Make<T>(snapshot.control, snapshot.owner, id, kind));
  return result;
}
template <class T, class P>
std::vector<T> Objects(const Snapshot& snapshot, const P& pool,
                       ElementKind kind) {
  std::vector<T> result;
  pool.Each([&](Id id, const auto&) {
    result.push_back(
        Access::Make<T>(snapshot.control, snapshot.owner, id, kind));
  });
  return result;
}
void Remove(std::vector<Id>& ids, Id id) { std::erase(ids, id); }
ElementIdentity Identity(std::uint64_t owner, Id id, ElementKind kind) {
  return Access::MakeIdentity(owner, id, kind);
}
void EraseFaceIn(State& state, std::uint64_t owner, Id face,
                 UnusedEdgePolicy policy, std::vector<ElementIdentity>& erased);
void EraseEdgeIn(State& state, std::uint64_t owner, Id edge, ErasePolicy policy,
                 std::vector<ElementIdentity>& erased) {
  const auto record = state.edges.Get(edge);
  if (!record.corners.empty() && policy == ErasePolicy::kRejectReferenced)
    Fail(EditorErrorCode::kReferencedElement, "Edge is used by faces");
  if (policy != ErasePolicy::kRejectReferenced &&
      policy != ErasePolicy::kCascade)
    Fail(EditorErrorCode::kInvalidTopology, "Invalid erase policy");
  std::set<Id> faces;
  for (Id corner : record.corners) faces.insert(state.corners.Get(corner).face);
  for (Id face : faces)
    EraseFaceIn(state, owner, face, UnusedEdgePolicy::kKeep, erased);
  for (Id vertex : record.endpoints)
    Remove(state.vertices.Mutate(vertex).edges, edge);
  const auto identity = Identity(owner, edge, ElementKind::kEdge);
  ClearRows(state, identity);
  state.edges.Erase(edge);
  erased.push_back(identity);
}
void EraseFaceIn(State& state, std::uint64_t owner, Id face,
                 UnusedEdgePolicy policy,
                 std::vector<ElementIdentity>& erased) {
  if (policy != UnusedEdgePolicy::kKeep && policy != UnusedEdgePolicy::kPrune)
    Fail(EditorErrorCode::kInvalidTopology, "Invalid unused-edge policy");
  const auto corners = state.faces.Get(face).corners;
  std::set<Id> edges;
  for (Id corner : corners) {
    const auto record = state.corners.Get(corner);
    Remove(state.vertices.Mutate(record.vertex).corners, corner);
    Remove(state.edges.Mutate(record.edge).corners, corner);
    edges.insert(record.edge);
    const auto identity = Identity(owner, corner, ElementKind::kCorner);
    ClearRows(state, identity);
    state.corners.Erase(corner);
    erased.push_back(identity);
  }
  const auto identity = Identity(owner, face, ElementKind::kFace);
  ClearRows(state, identity);
  state.faces.Erase(face);
  erased.push_back(identity);
  if (policy == UnusedEdgePolicy::kPrune)
    for (Id edge : edges)
      if (state.edges.Get(edge).corners.empty())
        EraseEdgeIn(state, owner, edge, ErasePolicy::kRejectReferenced, erased);
}

// Directory copies share payload pages. Only touched pages and incident records
// detach; accepted state and existing snapshots are never mutated.
template <class F>
auto Mutate(Session& session, F&& operation) {
  CheckSession(&session);
  if (session.batch_draft) {
    // One metadata copy when the batch starts; detach again only if a caller
    // captured an immutable draft snapshot between operations. Any failure
    // closes the entire batch through its public facade.
    if (session.candidate.use_count() != 1)
      session.candidate = std::make_shared<State>(*session.candidate);
    return operation(*session.candidate, session.created, session.erased);
  }
  auto candidate = std::make_shared<State>(*session.candidate);
  auto created = session.created;
  auto erased = session.erased;
  if constexpr (std::is_void_v<std::invoke_result_t<
                    F, State&, std::vector<ElementIdentity>&,
                    std::vector<ElementIdentity>&>>) {
    operation(*candidate, created, erased);
    session.candidate.swap(candidate);
    session.created.swap(created);
    session.erased.swap(erased);
  } else {
    auto result = operation(*candidate, created, erased);
    session.candidate.swap(candidate);
    session.created.swap(created);
    session.erased.swap(erased);
    return result;
  }
}
}  // namespace editing_detail

using editing_detail::Access;
using editing_detail::Check;
using editing_detail::Fail;
using editing_detail::Id;
using editing_detail::Local;

EditorError::EditorError(EditorErrorCode code, std::string message)
    : std::runtime_error(std::move(message)), code_(code) {}
EditorErrorCode EditorError::Code() const noexcept { return code_; }

ElementIdentity Vertex::Identity() const noexcept { return identity_; }
ElementIdentity Edge::Identity() const noexcept { return identity_; }
ElementIdentity Face::Identity() const noexcept { return identity_; }
ElementIdentity Corner::Identity() const noexcept { return identity_; }
ElementIdentity Property::Identity() const noexcept { return identity_; }
bool Vertex::IsValid() const {
  return editing_detail::Valid(owner_, identity_);
}
bool Edge::IsValid() const { return editing_detail::Valid(owner_, identity_); }
bool Face::IsValid() const { return editing_detail::Valid(owner_, identity_); }
bool Corner::IsValid() const {
  return editing_detail::Valid(owner_, identity_);
}
bool Property::IsValid() const {
  return editing_detail::Valid(owner_, identity_);
}
std::array<double, 3> Vertex::Position() const {
  const auto snapshot = editing_detail::Current(owner_, identity_);
  return snapshot.state->vertices.Get(Local(identity_)).position;
}
std::vector<Edge> Vertex::Edges() const {
  const auto snapshot = editing_detail::Current(owner_, identity_);
  return editing_detail::Objects<Edge>(
      snapshot, snapshot.state->vertices.Get(Local(identity_)).edges,
      ElementKind::kEdge);
}
std::vector<Corner> Vertex::Corners() const {
  const auto snapshot = editing_detail::Current(owner_, identity_);
  return editing_detail::Objects<Corner>(
      snapshot, snapshot.state->vertices.Get(Local(identity_)).corners,
      ElementKind::kCorner);
}
std::array<Vertex, 2> Edge::Endpoints() const {
  const auto snapshot = editing_detail::Current(owner_, identity_);
  const auto& endpoints = snapshot.state->edges.Get(Local(identity_)).endpoints;
  return {Access::Make<Vertex>(snapshot.control, snapshot.owner, endpoints[0],
                               ElementKind::kVertex),
          Access::Make<Vertex>(snapshot.control, snapshot.owner, endpoints[1],
                               ElementKind::kVertex)};
}
std::vector<Corner> Edge::Corners() const {
  const auto snapshot = editing_detail::Current(owner_, identity_);
  return editing_detail::Objects<Corner>(
      snapshot, snapshot.state->edges.Get(Local(identity_)).corners,
      ElementKind::kCorner);
}
std::vector<Corner> Face::Corners() const {
  const auto snapshot = editing_detail::Current(owner_, identity_);
  return editing_detail::Objects<Corner>(
      snapshot, snapshot.state->faces.Get(Local(identity_)).corners,
      ElementKind::kCorner);
}
Vertex Corner::GetVertex() const {
  const auto snapshot = editing_detail::Current(owner_, identity_);
  return Access::Make<Vertex>(
      snapshot.control, snapshot.owner,
      snapshot.state->corners.Get(Local(identity_)).vertex,
      ElementKind::kVertex);
}
Edge Corner::GetEdge() const {
  const auto snapshot = editing_detail::Current(owner_, identity_);
  return Access::Make<Edge>(snapshot.control, snapshot.owner,
                            snapshot.state->corners.Get(Local(identity_)).edge,
                            ElementKind::kEdge);
}
Face Corner::GetFace() const {
  const auto snapshot = editing_detail::Current(owner_, identity_);
  return Access::Make<Face>(snapshot.control, snapshot.owner,
                            snapshot.state->corners.Get(Local(identity_)).face,
                            ElementKind::kFace);
}
PropertyDescriptor Property::Descriptor() const {
  const auto snapshot = editing_detail::Current(owner_, identity_);
  return snapshot.state->properties.Get(Local(identity_))->descriptor;
}
PropertyRow Property::Row(ElementIdentity element) const {
  const auto snapshot = editing_detail::Current(owner_, identity_);
  const auto& property = *snapshot.state->properties.Get(Local(identity_));
  editing_detail::CheckRowElement(*snapshot.state, snapshot.owner, element,
                                  property.descriptor.domain);
  return property.rows.Get(Local(element));
}

std::uint64_t EditorSnapshot::OwnerIdentity() const {
  return Access::GetSnapshot(*this).owner;
}
std::uint64_t EditorSnapshot::Revision() const {
  return Access::GetSnapshot(*this).state->revision;
}
bool EditorSnapshot::IsCandidate() const {
  return Access::GetSnapshot(*this).candidate;
}
std::vector<Vertex> EditorSnapshot::Vertices() const {
  const auto& snapshot = Access::GetSnapshot(*this);
  return editing_detail::Objects<Vertex>(snapshot, snapshot.state->vertices,
                                         ElementKind::kVertex);
}
std::vector<Edge> EditorSnapshot::Edges() const {
  const auto& snapshot = Access::GetSnapshot(*this);
  return editing_detail::Objects<Edge>(snapshot, snapshot.state->edges,
                                       ElementKind::kEdge);
}
std::vector<Face> EditorSnapshot::Faces() const {
  const auto& snapshot = Access::GetSnapshot(*this);
  return editing_detail::Objects<Face>(snapshot, snapshot.state->faces,
                                       ElementKind::kFace);
}
std::vector<Corner> EditorSnapshot::Corners() const {
  const auto& snapshot = Access::GetSnapshot(*this);
  return editing_detail::Objects<Corner>(snapshot, snapshot.state->corners,
                                         ElementKind::kCorner);
}
std::vector<Property> EditorSnapshot::Properties() const {
  const auto& snapshot = Access::GetSnapshot(*this);
  return editing_detail::Objects<Property>(snapshot, snapshot.state->properties,
                                           ElementKind::kProperty);
}
namespace {
template <class Element>
constexpr ElementKind RangeKind() {
  if constexpr (std::is_same_v<Element, Vertex>) return ElementKind::kVertex;
  if constexpr (std::is_same_v<Element, Edge>) return ElementKind::kEdge;
  if constexpr (std::is_same_v<Element, Face>) return ElementKind::kFace;
  if constexpr (std::is_same_v<Element, Corner>) return ElementKind::kCorner;
  if constexpr (std::is_same_v<Element, Property>)
    return ElementKind::kProperty;
}
template <class Element>
const auto& RangePool(const editing_detail::State& state) {
  if constexpr (std::is_same_v<Element, Vertex>) return state.vertices;
  if constexpr (std::is_same_v<Element, Edge>) return state.edges;
  if constexpr (std::is_same_v<Element, Face>) return state.faces;
  if constexpr (std::is_same_v<Element, Corner>) return state.corners;
  if constexpr (std::is_same_v<Element, Property>) return state.properties;
}
template <class Element>
const std::vector<Id>& RangeIncidence(const editing_detail::Snapshot& snapshot,
                                      ElementIdentity source) {
  if constexpr (std::is_same_v<Element, Edge>) {
    return snapshot.state->vertices.Get(Local(source)).edges;
  } else {
    if constexpr (std::is_same_v<Element, Corner>) {
      switch (source.Kind()) {
        case ElementKind::kVertex:
          return snapshot.state->vertices.Get(Local(source)).corners;
        case ElementKind::kEdge:
          return snapshot.state->edges.Get(Local(source)).corners;
        case ElementKind::kFace:
          return snapshot.state->faces.Get(Local(source)).corners;
        default:
          break;
      }
    }
    Fail(EditorErrorCode::kInvalidObject, "Invalid snapshot incidence source");
  }
}
template <class Element>
std::uint64_t RangeLimit(const editing_detail::Snapshot& snapshot,
                         std::optional<ElementIdentity> incidence) {
  if (incidence) return RangeIncidence<Element>(snapshot, *incidence).size();
  return RangePool<Element>(*snapshot.state).size;
}
}  // namespace

template <class Element>
SnapshotRange<Element>::SnapshotRange(
    std::shared_ptr<const editing_detail::Snapshot> snapshot,
    std::optional<ElementIdentity> incidence)
    : snapshot_(std::move(snapshot)), incidence_(incidence) {}
template <class Element>
SnapshotRange<Element>::Iterator::Iterator(
    std::shared_ptr<const editing_detail::Snapshot> snapshot,
    std::optional<ElementIdentity> incidence, std::uint64_t position)
    : snapshot_(std::move(snapshot)),
      incidence_(incidence),
      position_(position) {
  SkipVacant();
}
template <class Element>
void SnapshotRange<Element>::Iterator::SkipVacant() {
  if (!snapshot_ || incidence_) return;
  const auto& pool = RangePool<Element>(*snapshot_->state);
  while (position_ < pool.size) {
    if ((*pool.pages[static_cast<std::size_t>(
            position_ / editing_detail::kPageSize)])[position_ %
                                                     editing_detail::kPageSize]
            .value)
      break;
    ++position_;
  }
}
template <class Element>
Element SnapshotRange<Element>::Iterator::operator*() const {
  if (!snapshot_ || position_ >= RangeLimit<Element>(*snapshot_, incidence_))
    Fail(EditorErrorCode::kInvalidObject, "Snapshot iterator is at end");
  Id id;
  if (incidence_) {
    id = RangeIncidence<Element>(
        *snapshot_, *incidence_)[static_cast<std::size_t>(position_)];
  } else {
    const auto& pool = RangePool<Element>(*snapshot_->state);
    const auto& slot = (*pool.pages[static_cast<std::size_t>(
        position_ / editing_detail::kPageSize)])[position_ %
                                                 editing_detail::kPageSize];
    id = {position_, slot.generation};
  }
  return Access::Make<Element>(snapshot_->control, snapshot_->owner, id,
                               RangeKind<Element>());
}
template <class Element>
typename SnapshotRange<Element>::Iterator&
SnapshotRange<Element>::Iterator::operator++() {
  if (!snapshot_ || position_ >= RangeLimit<Element>(*snapshot_, incidence_))
    Fail(EditorErrorCode::kInvalidObject, "Snapshot iterator is at end");
  ++position_;
  SkipVacant();
  return *this;
}
template <class Element>
typename SnapshotRange<Element>::Iterator
SnapshotRange<Element>::Iterator::operator++(int) {
  auto previous = *this;
  ++*this;
  return previous;
}
template <class Element>
bool SnapshotRange<Element>::Iterator::operator==(
    const Iterator& other) const noexcept {
  return snapshot_ == other.snapshot_ && incidence_ == other.incidence_ &&
         position_ == other.position_;
}
template <class Element>
typename SnapshotRange<Element>::Iterator SnapshotRange<Element>::begin()
    const {
  return Iterator(snapshot_, incidence_, 0);
}
template <class Element>
typename SnapshotRange<Element>::Iterator SnapshotRange<Element>::end() const {
  return Iterator(snapshot_, incidence_,
                  snapshot_ ? RangeLimit<Element>(*snapshot_, incidence_) : 0);
}
template class SnapshotRange<Vertex>;
template class SnapshotRange<Edge>;
template class SnapshotRange<Face>;
template class SnapshotRange<Corner>;
template class SnapshotRange<Property>;

const AttributeValues& PropertyRowView::Values() const {
  if (!snapshot_ || !row_)
    Fail(EditorErrorCode::kInvalidObject, "Property row view is empty");
  return row_->values;
}
bool PropertyRowView::IsPresent() const {
  if (!snapshot_ || !row_)
    Fail(EditorErrorCode::kInvalidObject, "Property row view is empty");
  return row_->present;
}
SnapshotRange<Vertex> EditorSnapshot::VertexElements() const {
  (void)Access::GetSnapshot(*this);
  return Access::MakeRange<Vertex>(snapshot_);
}
SnapshotRange<Edge> EditorSnapshot::EdgeElements() const {
  (void)Access::GetSnapshot(*this);
  return Access::MakeRange<Edge>(snapshot_);
}
SnapshotRange<Face> EditorSnapshot::FaceElements() const {
  (void)Access::GetSnapshot(*this);
  return Access::MakeRange<Face>(snapshot_);
}
SnapshotRange<Corner> EditorSnapshot::CornerElements() const {
  (void)Access::GetSnapshot(*this);
  return Access::MakeRange<Corner>(snapshot_);
}
SnapshotRange<Property> EditorSnapshot::PropertyElements() const {
  (void)Access::GetSnapshot(*this);
  return Access::MakeRange<Property>(snapshot_);
}
SnapshotRange<Edge> EditorSnapshot::EdgeElements(Vertex vertex) const {
  const auto& snapshot = Access::GetSnapshot(*this);
  Check(*snapshot.state, snapshot.owner, vertex.Identity(),
        ElementKind::kVertex);
  return Access::MakeRange<Edge>(snapshot_, vertex.Identity());
}
SnapshotRange<Corner> EditorSnapshot::CornerElements(Vertex vertex) const {
  const auto& snapshot = Access::GetSnapshot(*this);
  Check(*snapshot.state, snapshot.owner, vertex.Identity(),
        ElementKind::kVertex);
  return Access::MakeRange<Corner>(snapshot_, vertex.Identity());
}
SnapshotRange<Corner> EditorSnapshot::CornerElements(Edge edge) const {
  const auto& snapshot = Access::GetSnapshot(*this);
  Check(*snapshot.state, snapshot.owner, edge.Identity(), ElementKind::kEdge);
  return Access::MakeRange<Corner>(snapshot_, edge.Identity());
}
SnapshotRange<Corner> EditorSnapshot::CornerElements(Face face) const {
  const auto& snapshot = Access::GetSnapshot(*this);
  Check(*snapshot.state, snapshot.owner, face.Identity(), ElementKind::kFace);
  return Access::MakeRange<Corner>(snapshot_, face.Identity());
}
Vertex EditorSnapshot::GetVertex(Corner corner) const {
  const auto& snapshot = Access::GetSnapshot(*this);
  Check(*snapshot.state, snapshot.owner, corner.Identity(),
        ElementKind::kCorner);
  return Access::Make<Vertex>(
      snapshot.control, snapshot.owner,
      snapshot.state->corners.Get(Local(corner.Identity())).vertex,
      ElementKind::kVertex);
}
Edge EditorSnapshot::GetEdge(Corner corner) const {
  const auto& snapshot = Access::GetSnapshot(*this);
  Check(*snapshot.state, snapshot.owner, corner.Identity(),
        ElementKind::kCorner);
  return Access::Make<Edge>(
      snapshot.control, snapshot.owner,
      snapshot.state->corners.Get(Local(corner.Identity())).edge,
      ElementKind::kEdge);
}
Face EditorSnapshot::GetFace(Corner corner) const {
  const auto& snapshot = Access::GetSnapshot(*this);
  Check(*snapshot.state, snapshot.owner, corner.Identity(),
        ElementKind::kCorner);
  return Access::Make<Face>(
      snapshot.control, snapshot.owner,
      snapshot.state->corners.Get(Local(corner.Identity())).face,
      ElementKind::kFace);
}
PropertyRowView EditorSnapshot::ViewRow(Property property,
                                        ElementIdentity element) const {
  const auto& snapshot = Access::GetSnapshot(*this);
  Check(*snapshot.state, snapshot.owner, property.Identity(),
        ElementKind::kProperty);
  const auto& record =
      *snapshot.state->properties.Get(Local(property.Identity()));
  editing_detail::CheckRowElement(*snapshot.state, snapshot.owner, element,
                                  record.descriptor.domain);
  return Access::MakeRowView(snapshot_, record.rows.Get(Local(element)));
}

std::array<double, 3> EditorSnapshot::Position(Vertex vertex) const {
  const auto& snapshot = Access::GetSnapshot(*this);
  Check(*snapshot.state, snapshot.owner, vertex.Identity(),
        ElementKind::kVertex);
  return snapshot.state->vertices.Get(Local(vertex.Identity())).position;
}
std::array<Vertex, 2> EditorSnapshot::Endpoints(Edge edge) const {
  const auto& snapshot = Access::GetSnapshot(*this);
  Check(*snapshot.state, snapshot.owner, edge.Identity(), ElementKind::kEdge);
  const auto& endpoints =
      snapshot.state->edges.Get(Local(edge.Identity())).endpoints;
  return {Access::Make<Vertex>(snapshot.control, snapshot.owner, endpoints[0],
                               ElementKind::kVertex),
          Access::Make<Vertex>(snapshot.control, snapshot.owner, endpoints[1],
                               ElementKind::kVertex)};
}
std::vector<Corner> EditorSnapshot::Corners(Face face) const {
  const auto& snapshot = Access::GetSnapshot(*this);
  Check(*snapshot.state, snapshot.owner, face.Identity(), ElementKind::kFace);
  return editing_detail::Objects<Corner>(
      snapshot, snapshot.state->faces.Get(Local(face.Identity())).corners,
      ElementKind::kCorner);
}
std::vector<Corner> EditorSnapshot::Corners(Edge edge) const {
  const auto& snapshot = Access::GetSnapshot(*this);
  Check(*snapshot.state, snapshot.owner, edge.Identity(), ElementKind::kEdge);
  return editing_detail::Objects<Corner>(
      snapshot, snapshot.state->edges.Get(Local(edge.Identity())).corners,
      ElementKind::kCorner);
}
PropertyDescriptor EditorSnapshot::Descriptor(Property property) const {
  const auto& snapshot = Access::GetSnapshot(*this);
  Check(*snapshot.state, snapshot.owner, property.Identity(),
        ElementKind::kProperty);
  return snapshot.state->properties.Get(Local(property.Identity()))->descriptor;
}
PropertyRow EditorSnapshot::Row(Property property,
                                ElementIdentity element) const {
  const auto& snapshot = Access::GetSnapshot(*this);
  Check(*snapshot.state, snapshot.owner, property.Identity(),
        ElementKind::kProperty);
  const auto& record =
      *snapshot.state->properties.Get(Local(property.Identity()));
  editing_detail::CheckRowElement(*snapshot.state, snapshot.owner, element,
                                  record.descriptor.domain);
  return record.rows.Get(Local(element));
}

DenseEditorMesh EditorSnapshot::Materialize() const {
  const auto& snapshot = Access::GetSnapshot(*this);
  DenseEditorMesh result;
  result.owner = snapshot.owner;
  result.revision = snapshot.state->revision;
  result.is_candidate = snapshot.candidate;
  result.vertices = Vertices();
  result.edges = Edges();
  result.faces = Faces();
  for (const auto& vertex : result.vertices) {
    result.vertex_rows.emplace(vertex.Identity(), result.positions.size());
    result.positions.push_back(Position(vertex));
  }
  for (const auto& edge : result.edges) {
    result.edge_rows.emplace(edge.Identity(), result.edge_vertices.size());
    const auto endpoints = Endpoints(edge);
    result.edge_vertices.push_back(
        {result.vertex_rows.at(endpoints[0].Identity()),
         result.vertex_rows.at(endpoints[1].Identity())});
  }
  for (const auto& face : result.faces) {
    const auto face_row = result.face_rows.size();
    result.face_rows.emplace(face.Identity(), face_row);
    for (const auto& corner : Corners(face)) {
      const auto& record =
          snapshot.state->corners.Get(Local(corner.Identity()));
      result.corner_rows.emplace(corner.Identity(), result.corners.size());
      result.corners.push_back(corner);
      result.corner_vertices.push_back(
          result.vertex_rows.at(editing_detail::Identity(
              snapshot.owner, record.vertex, ElementKind::kVertex)));
      result.corner_edges.push_back(
          result.edge_rows.at(editing_detail::Identity(
              snapshot.owner, record.edge, ElementKind::kEdge)));
      result.corner_faces.push_back(face_row);
    }
    result.face_offsets.push_back(result.corners.size());
  }
  for (const auto& property : Properties()) {
    const auto& record =
        *snapshot.state->properties.Get(Local(property.Identity()));
    DenseProperty dense{
        property,
        record.descriptor,
        editing_detail::EmptyValues(record.descriptor.scalar_type),
        std::nullopt,
        {}};
    if (record.descriptor.ragged) dense.offsets = std::vector<index_t>{0};
    auto append = [&](const auto& element) {
      const auto& row = record.rows.Get(Local(element.Identity()));
      std::visit(
          [&](auto& values) {
            using Values = std::decay_t<decltype(values)>;
            const auto& source = std::get<Values>(row.values);
            values.insert(values.end(), source.begin(), source.end());
            if (dense.offsets) dense.offsets->push_back(values.size());
          },
          dense.values);
      dense.present.push_back(row.present ? 1 : 0);
    };
    switch (record.descriptor.domain) {
      case PropertyDomain::kVertex:
        for (const auto& element : result.vertices) append(element);
        break;
      case PropertyDomain::kEdge:
        for (const auto& element : result.edges) append(element);
        break;
      case PropertyDomain::kFace:
        for (const auto& element : result.faces) append(element);
        break;
      case PropertyDomain::kCorner:
        for (const auto& element : result.corners) append(element);
        break;
    }
    result.properties.push_back(std::move(dense));
  }
  return result;
}
Mesh EditorSnapshot::ExportMesh() const {
  const auto& snapshot = Access::GetSnapshot(*this);
  std::set<std::array<Id, 2>> pairs;
  snapshot.state->edges.Each([&](Id, const auto& edge) {
    auto endpoints = edge.endpoints;
    if (endpoints[1] < endpoints[0]) std::swap(endpoints[0], endpoints[1]);
    if (edge.corners.empty() || !pairs.insert(endpoints).second)
      Fail(EditorErrorCode::kUnsupportedProjection,
           "Raw Mesh cannot retain wires or distinct parallel edges");
  });
  snapshot.state->properties.Each([&](Id, const auto& property) {
    if (property->descriptor.domain == PropertyDomain::kEdge)
      Fail(EditorErrorCode::kUnsupportedProjection,
           "Raw Mesh cannot retain edge-domain properties");
  });
  auto dense = Materialize();
  Mesh result;
  result.positions.reserve(dense.positions.size());
  for (const auto& position : dense.positions)
    result.positions.Append(position);
  result.face_offsets = std::move(dense.face_offsets);
  result.corner_vertices = std::move(dense.corner_vertices);
  for (auto& property : dense.properties) {
    Attribute attribute;
    switch (property.descriptor.domain) {
      case PropertyDomain::kVertex:
        attribute.domain = AttributeDomain::vertex;
        break;
      case PropertyDomain::kFace:
        attribute.domain = AttributeDomain::face;
        break;
      case PropertyDomain::kCorner:
        attribute.domain = AttributeDomain::corner;
        break;
      case PropertyDomain::kEdge:
        Fail(EditorErrorCode::kUnsupportedProjection,
             "Raw Mesh has no edge domain");
    }
    attribute.name = std::move(property.descriptor.name);
    attribute.semantic = std::move(property.descriptor.semantic);
    attribute.set_index = property.descriptor.set_index;
    attribute.components = property.descriptor.components;
    attribute.values = std::move(property.values);
    attribute.offsets = std::move(property.offsets);
    attribute.present = std::move(property.present);
    attribute.metadata = std::move(property.descriptor.metadata);
    result.attributes.push_back(std::move(attribute));
  }
  return result;
}

EditSession::EditSession(std::unique_ptr<editing_detail::Session> session)
    : session_(std::move(session)) {}
EditSession::EditSession(EditSession&&) noexcept = default;
EditSession& EditSession::operator=(EditSession&&) noexcept = default;
EditSession::~EditSession() = default;
Vertex EditSession::CreateVertex(
    std::array<double, 3> position,
    std::span<const PropertyAssignment> properties) {
  editing_detail::CheckMutableSession(session_.get());
  editing_detail::Finite(position);
  const auto generation =
      editing_detail::Issue(session_->owner->next_generation);
  return editing_detail::Mutate(
      *session_, [&](auto& state, auto& created, auto&) {
        const auto id = state.vertices.Add({position, {}, {}}, generation);
        const auto identity = editing_detail::Identity(
            session_->owner->identity, id, ElementKind::kVertex);
        editing_detail::InitializeRows(state, session_->owner->identity,
                                       identity, properties);
        created.push_back(identity);
        return Access::Make<Vertex>(session_->owner, session_->owner->identity,
                                    id, ElementKind::kVertex);
      });
}
Edge EditSession::CreateEdge(Vertex first, Vertex second,
                             std::span<const PropertyAssignment> properties) {
  editing_detail::CheckMutableSession(session_.get());
  const auto generation =
      editing_detail::Issue(session_->owner->next_generation);
  return editing_detail::Mutate(*session_, [&](auto& state, auto& created,
                                               auto&) {
    Check(state, session_->owner->identity, first.Identity(),
          ElementKind::kVertex);
    Check(state, session_->owner->identity, second.Identity(),
          ElementKind::kVertex);
    const auto id = state.edges.Add(
        {{Local(first.Identity()), Local(second.Identity())}, {}}, generation);
    state.vertices.Mutate(Local(first.Identity())).edges.push_back(id);
    if (first.Identity() != second.Identity())
      state.vertices.Mutate(Local(second.Identity())).edges.push_back(id);
    const auto identity = editing_detail::Identity(session_->owner->identity,
                                                   id, ElementKind::kEdge);
    editing_detail::InitializeRows(state, session_->owner->identity, identity,
                                   properties);
    created.push_back(identity);
    return Access::Make<Edge>(session_->owner, session_->owner->identity, id,
                              ElementKind::kEdge);
  });
}
Face EditSession::CreateFace(std::span<const FaceCorner> corners,
                             std::span<const PropertyAssignment> properties) {
  editing_detail::CheckMutableSession(session_.get());
  if (corners.size() < editing_detail::kMinimumFaceCornerCount)
    Fail(EditorErrorCode::kInvalidTopology,
         "Face requires at least three corners");
  return editing_detail::Mutate(*session_, [&](auto& state, auto& created,
                                               auto&) {
    const auto owner = session_->owner->identity;
    for (std::size_t i = 0; i < corners.size(); ++i) {
      Check(state, owner, corners[i].vertex.Identity(), ElementKind::kVertex);
      Check(state, owner, corners[i].edge.Identity(), ElementKind::kEdge);
      Check(state, owner, corners[(i + 1) % corners.size()].vertex.Identity(),
            ElementKind::kVertex);
      const auto first = Local(corners[i].vertex.Identity()),
                 second =
                     Local(corners[(i + 1) % corners.size()].vertex.Identity());
      const auto& endpoints =
          state.edges.Get(Local(corners[i].edge.Identity())).endpoints;
      if (!((endpoints[0] == first && endpoints[1] == second) ||
            (endpoints[1] == first && endpoints[0] == second)))
        Fail(EditorErrorCode::kInvalidTopology,
             "Outgoing edge does not connect consecutive corner vertices");
    }
    const auto face = state.faces.Add(
        {}, editing_detail::Issue(session_->owner->next_generation));
    const auto identity =
        editing_detail::Identity(owner, face, ElementKind::kFace);
    editing_detail::InitializeRows(state, owner, identity, properties);
    created.push_back(identity);
    for (const auto& corner : corners) {
      const auto vertex = Local(corner.vertex.Identity()),
                 edge = Local(corner.edge.Identity());
      const auto id = state.corners.Add(
          {vertex, edge, face},
          editing_detail::Issue(session_->owner->next_generation));
      const auto corner_identity =
          editing_detail::Identity(owner, id, ElementKind::kCorner);
      editing_detail::InitializeRows(state, owner, corner_identity,
                                     corner.properties);
      state.vertices.Mutate(vertex).corners.push_back(id);
      state.edges.Mutate(edge).corners.push_back(id);
      state.faces.Mutate(face).corners.push_back(id);
      created.push_back(corner_identity);
    }
    return Access::Make<Face>(session_->owner, owner, face, ElementKind::kFace);
  });
}
void EditSession::SetPosition(Vertex vertex, std::array<double, 3> position) {
  editing_detail::CheckMutableSession(session_.get());
  editing_detail::Finite(position);
  editing_detail::Mutate(*session_, [&](auto& state, auto&, auto&) {
    Check(state, session_->owner->identity, vertex.Identity(),
          ElementKind::kVertex);
    state.vertices.Mutate(Local(vertex.Identity())).position = position;
  });
}
void EditSession::EraseFace(Face face, UnusedEdgePolicy policy) {
  editing_detail::CheckMutableSession(session_.get());
  editing_detail::Mutate(*session_, [&](auto& state, auto&, auto& erased) {
    Check(state, session_->owner->identity, face.Identity(),
          ElementKind::kFace);
    editing_detail::EraseFaceIn(state, session_->owner->identity,
                                Local(face.Identity()), policy, erased);
  });
}
void EditSession::EraseEdge(Edge edge, ErasePolicy policy) {
  editing_detail::CheckMutableSession(session_.get());
  editing_detail::Mutate(*session_, [&](auto& state, auto&, auto& erased) {
    Check(state, session_->owner->identity, edge.Identity(),
          ElementKind::kEdge);
    editing_detail::EraseEdgeIn(state, session_->owner->identity,
                                Local(edge.Identity()), policy, erased);
  });
}
void EditSession::EraseVertex(Vertex vertex, ErasePolicy policy) {
  editing_detail::CheckMutableSession(session_.get());
  editing_detail::Mutate(*session_, [&](auto& state, auto&, auto& erased) {
    Check(state, session_->owner->identity, vertex.Identity(),
          ElementKind::kVertex);
    const auto record = state.vertices.Get(Local(vertex.Identity()));
    if ((!record.edges.empty() || !record.corners.empty()) &&
        policy == ErasePolicy::kRejectReferenced)
      Fail(EditorErrorCode::kReferencedElement,
           "Vertex is referenced by edges or faces");
    if (policy != ErasePolicy::kRejectReferenced &&
        policy != ErasePolicy::kCascade)
      Fail(EditorErrorCode::kInvalidTopology, "Invalid erase policy");
    std::set<Id> faces;
    for (Id corner : record.corners)
      faces.insert(state.corners.Get(corner).face);
    for (Id face : faces)
      editing_detail::EraseFaceIn(state, session_->owner->identity, face,
                                  UnusedEdgePolicy::kKeep, erased);
    for (Id edge : record.edges)
      editing_detail::EraseEdgeIn(state, session_->owner->identity, edge,
                                  ErasePolicy::kRejectReferenced, erased);
    editing_detail::ClearRows(state, vertex.Identity());
    state.vertices.Erase(Local(vertex.Identity()));
    erased.push_back(vertex.Identity());
  });
}
Property EditSession::CreateProperty(PropertyDescriptor descriptor) {
  editing_detail::CheckMutableSession(session_.get());
  if (descriptor.name.empty() || descriptor.components == 0)
    Fail(EditorErrorCode::kInvalidProperty,
         "Property needs a name and nonzero component count");
  editing_detail::Kind(descriptor.domain);
  editing_detail::EmptyValues(descriptor.scalar_type);
  if (descriptor.default_row)
    editing_detail::ValidateRow(descriptor, *descriptor.default_row);
  if (descriptor.new_row_policy != NewRowPolicy::kMissing &&
      descriptor.new_row_policy != NewRowPolicy::kDefault &&
      descriptor.new_row_policy != NewRowPolicy::kRequireExplicit)
    Fail(EditorErrorCode::kInvalidProperty, "Invalid new-row policy");
  if (descriptor.new_row_policy == NewRowPolicy::kDefault &&
      !descriptor.default_row)
    Fail(EditorErrorCode::kInvalidProperty, "Default policy needs a row");
  const auto generation =
      editing_detail::Issue(session_->owner->next_generation);
  return editing_detail::Mutate(*session_, [&](auto& state, auto& created,
                                               auto&) {
    state.properties.Each([&](Id, const auto& record) {
      if (record->descriptor.domain == descriptor.domain &&
          record->descriptor.name == descriptor.name)
        Fail(EditorErrorCode::kInvalidProperty,
             "Property name already exists in domain");
    });
    auto record = std::make_shared<editing_detail::PropertyRecord>();
    record->descriptor = descriptor;
    editing_detail::EachDomain(state, descriptor.domain, [&](Id id) {
      record->rows.SetAt(id, editing_detail::InitialRow(descriptor));
    });
    const auto id = state.properties.Add(std::move(record), generation);
    created.push_back(editing_detail::Identity(session_->owner->identity, id,
                                               ElementKind::kProperty));
    return Access::Make<Property>(session_->owner, session_->owner->identity,
                                  id, ElementKind::kProperty);
  });
}
void EditSession::RemoveProperty(Property property) {
  editing_detail::CheckMutableSession(session_.get());
  editing_detail::Mutate(*session_, [&](auto& state, auto&, auto& erased) {
    Check(state, session_->owner->identity, property.Identity(),
          ElementKind::kProperty);
    state.properties.Erase(Local(property.Identity()));
    erased.push_back(property.Identity());
  });
}
void EditSession::SetPropertyRow(Property property, ElementIdentity element,
                                 PropertyRow row) {
  editing_detail::CheckMutableSession(session_.get());
  editing_detail::Mutate(*session_, [&](auto& state, auto&, auto&) {
    Check(state, session_->owner->identity, property.Identity(),
          ElementKind::kProperty);
    const auto& descriptor =
        state.properties.Get(Local(property.Identity()))->descriptor;
    editing_detail::CheckRowElement(state, session_->owner->identity, element,
                                    descriptor.domain);
    editing_detail::ValidateRow(descriptor, row);
    editing_detail::MutableProperty(state, Local(property.Identity()))
        .rows.SetAt(Local(element), std::move(row));
  });
}
EditorSnapshot EditSession::Snapshot() const {
  editing_detail::CheckSession(session_.get());
  return Access::MakeSnapshot(session_->owner, session_->candidate, true);
}
EditBatch EditSession::BeginBatch() {
  editing_detail::CheckMutableSession(session_.get());
  auto draft = std::make_unique<editing_detail::Session>();
  draft->owner = session_->owner;
  draft->candidate =
      std::make_shared<editing_detail::State>(*session_->candidate);
  draft->base_revision = session_->base_revision;
  draft->created = session_->created;
  draft->erased = session_->erased;
  draft->batch_draft = true;
  // Construction can allocate; activate the parent only after it succeeds.
  EditBatch result(session_, std::move(draft));
  session_->batch_active = true;
  return result;
}
CommitResult EditSession::Commit(std::stop_token stop) {
  editing_detail::CheckMutableSession(session_.get());
  if (stop.stop_requested())
    Fail(EditorErrorCode::kCanceled, "Commit canceled");
  if (session_->base_revision == std::numeric_limits<std::uint64_t>::max())
    Fail(EditorErrorCode::kIdentityExhausted, "Revision exhausted");
  // All result allocations precede publication. Mutators preserve structural
  // invariants in isolated drafts, so commit needs no full-mesh validation
  // scan.
  CommitResult result;
  result.revision = session_->base_revision + 1;
  for (const auto& id : session_->created)
    if (editing_detail::Contains(*session_->candidate, id))
      result.created.push_back(id);
  for (const auto& id : session_->erased)
    if (std::find(session_->created.begin(), session_->created.end(), id) ==
        session_->created.end())
      result.erased.push_back(id);
  auto published =
      std::make_shared<editing_detail::State>(*session_->candidate);
  published->revision = result.revision;
  std::lock_guard lock(session_->owner->mutex);
  if (!session_->owner->alive)
    Fail(EditorErrorCode::kOwnerDestroyed, "Owner was destroyed");
  if (session_->owner->accepted->revision != session_->base_revision)
    Fail(EditorErrorCode::kRevisionConflict,
         "Accepted revision changed; begin a new session");
  if (stop.stop_requested())
    Fail(EditorErrorCode::kCanceled, "Commit canceled");
  // A candidate may already have an immutable session snapshot. Do not mutate
  // its revision field; shallow-clone that state before the publication point.
  session_->owner->accepted = std::move(published);
  session_->closed = true;
  return result;
}
void EditSession::Discard() {
  editing_detail::CheckSession(session_.get());
  session_->closed = true;
  session_->candidate.reset();
}

EditBatch::EditBatch(std::weak_ptr<editing_detail::Session> parent,
                     std::unique_ptr<editing_detail::Session> draft)
    : parent_(std::move(parent)), draft_(std::move(draft)) {}
EditBatch::EditBatch(EditBatch&&) noexcept = default;
EditBatch& EditBatch::operator=(EditBatch&& other) noexcept {
  if (this != &other) {
    Close();
    parent_ = std::move(other.parent_);
    draft_ = std::move(other.draft_);
  }
  return *this;
}
EditBatch::~EditBatch() { Close(); }
void EditBatch::Check() const {
  editing_detail::CheckSession(draft_.session_.get());
  const auto parent = parent_.lock();
  editing_detail::CheckSession(parent.get());
  if (!parent->batch_active)
    Fail(EditorErrorCode::kSessionClosed, "Batch is no longer active");
}
void EditBatch::Close() const noexcept {
  if (!draft_.session_ || draft_.session_->closed) return;
  draft_.session_->closed = true;
  draft_.session_->candidate.reset();
  if (const auto parent = parent_.lock()) parent->batch_active = false;
}
Vertex EditBatch::CreateVertex(std::array<double, 3> position,
                               std::span<const PropertyAssignment> properties) {
  try {
    Check();
    return draft_.CreateVertex(position, properties);
  } catch (...) {
    Close();
    throw;
  }
}
Edge EditBatch::CreateEdge(Vertex first, Vertex second,
                           std::span<const PropertyAssignment> properties) {
  try {
    Check();
    return draft_.CreateEdge(first, second, properties);
  } catch (...) {
    Close();
    throw;
  }
}
Face EditBatch::CreateFace(std::span<const FaceCorner> corners,
                           std::span<const PropertyAssignment> properties) {
  try {
    Check();
    return draft_.CreateFace(corners, properties);
  } catch (...) {
    Close();
    throw;
  }
}
void EditBatch::SetPosition(Vertex vertex, std::array<double, 3> position) {
  try {
    Check();
    draft_.SetPosition(vertex, position);
  } catch (...) {
    Close();
    throw;
  }
}
void EditBatch::EraseVertex(Vertex vertex, ErasePolicy policy) {
  try {
    Check();
    draft_.EraseVertex(vertex, policy);
  } catch (...) {
    Close();
    throw;
  }
}
void EditBatch::EraseEdge(Edge edge, ErasePolicy policy) {
  try {
    Check();
    draft_.EraseEdge(edge, policy);
  } catch (...) {
    Close();
    throw;
  }
}
void EditBatch::EraseFace(Face face, UnusedEdgePolicy policy) {
  try {
    Check();
    draft_.EraseFace(face, policy);
  } catch (...) {
    Close();
    throw;
  }
}
Property EditBatch::CreateProperty(const PropertyDescriptor& descriptor) {
  try {
    Check();
    return draft_.CreateProperty(descriptor);
  } catch (...) {
    Close();
    throw;
  }
}
void EditBatch::RemoveProperty(Property property) {
  try {
    Check();
    draft_.RemoveProperty(property);
  } catch (...) {
    Close();
    throw;
  }
}
void EditBatch::SetPropertyRow(Property property, ElementIdentity element,
                               PropertyRow row) {
  try {
    Check();
    draft_.SetPropertyRow(property, element, std::move(row));
  } catch (...) {
    Close();
    throw;
  }
}
EditorSnapshot EditBatch::Snapshot() const {
  try {
    Check();
    return draft_.Snapshot();
  } catch (...) {
    Close();
    throw;
  }
}
void EditBatch::Apply(std::stop_token stop) {
  try {
    Check();
    if (stop.stop_requested())
      Fail(EditorErrorCode::kCanceled, "Batch apply canceled");
    const auto parent = parent_.lock();
    // No allocation after the cancellation check. Parent mutation is blocked
    // while this draft is active; all three noexcept swaps accept one unit.
    parent->candidate.swap(draft_.session_->candidate);
    parent->created.swap(draft_.session_->created);
    parent->erased.swap(draft_.session_->erased);
    Close();
  } catch (...) {
    Close();
    throw;
  }
}
void EditBatch::Discard() {
  try {
    Check();
    Close();
  } catch (...) {
    Close();
    throw;
  }
}

EditableMesh::EditableMesh()
    : owner_(std::make_shared<editing_detail::Owner>()) {}
EditableMesh::EditableMesh(EditableMesh&&) noexcept = default;
EditableMesh& EditableMesh::operator=(EditableMesh&& other) noexcept {
  if (this != &other) {
    if (owner_) {
      std::lock_guard lock(owner_->mutex);
      owner_->alive = false;
    }
    owner_ = std::move(other.owner_);
  }
  return *this;
}
EditableMesh::~EditableMesh() {
  if (owner_) {
    std::lock_guard lock(owner_->mutex);
    owner_->alive = false;
  }
}
EditSession EditableMesh::BeginEdit() {
  if (!owner_) Fail(EditorErrorCode::kOwnerDestroyed, "Owner is moved from");
  auto session = std::make_unique<editing_detail::Session>();
  session->owner = owner_;
  {
    std::lock_guard lock(owner_->mutex);
    session->base_revision = owner_->accepted->revision;
    session->candidate =
        std::make_shared<editing_detail::State>(*owner_->accepted);
  }
  return Access::MakeSession(std::move(session));
}
EditorSnapshot EditableMesh::Snapshot() const {
  if (!owner_) Fail(EditorErrorCode::kOwnerDestroyed, "Owner is moved from");
  std::lock_guard lock(owner_->mutex);
  return Access::MakeSnapshot(owner_, owner_->accepted);
}
ForkResult EditableMesh::Fork() const {
  if (!owner_) Fail(EditorErrorCode::kOwnerDestroyed, "Owner is moved from");
  ForkResult result;
  std::shared_ptr<const editing_detail::State> state;
  {
    std::lock_guard lock(owner_->mutex);
    state = owner_->accepted;
    result.mesh.owner_->next_generation.store(
        owner_->next_generation.load(std::memory_order_relaxed),
        std::memory_order_relaxed);
  }
  result.mesh.owner_->accepted = state;
  auto add = [&](Id id, ElementKind kind) {
    const auto original = editing_detail::Identity(owner_->identity, id, kind),
               fork = editing_detail::Identity(result.mesh.owner_->identity, id,
                                               kind);
    result.correspondence.original_to_fork.emplace(original, fork);
    result.correspondence.fork_to_original.emplace(fork, original);
  };
  state->vertices.Each(
      [&](Id id, const auto&) { add(id, ElementKind::kVertex); });
  state->edges.Each([&](Id id, const auto&) { add(id, ElementKind::kEdge); });
  state->faces.Each([&](Id id, const auto&) { add(id, ElementKind::kFace); });
  state->corners.Each(
      [&](Id id, const auto&) { add(id, ElementKind::kCorner); });
  state->properties.Each(
      [&](Id id, const auto&) { add(id, ElementKind::kProperty); });
  return result;
}
ImportResult EditableMesh::ImportMesh(const Mesh& mesh) {
  if (!inspect_storage(mesh).empty())
    Fail(EditorErrorCode::kInvalidTopology, "Raw Mesh storage is invalid");
  ImportResult result;
  auto state = std::make_shared<editing_detail::State>();
  auto issue = [&] {
    return editing_detail::Issue(result.mesh.owner_->next_generation);
  };
  std::vector<Id> vertices, faces, corners;
  vertices.reserve(mesh.positions.size());
  faces.reserve(static_cast<std::size_t>(mesh.face_count()));
  corners.reserve(mesh.corner_vertices.size());
  for (std::size_t row = 0; row < mesh.positions.size(); ++row)
    vertices.push_back(
        state->vertices.Add({mesh.positions.Get(row), {}, {}}, issue()));
  std::map<std::array<Id, 2>, Id> edges;
  for (index_t f = 0; f < mesh.face_count(); ++f) {
    const auto face = state->faces.Add({}, issue());
    faces.push_back(face);
    for (index_t c = mesh.face_offsets[static_cast<std::size_t>(f)];
         c < mesh.face_offsets[static_cast<std::size_t>(f + 1)]; ++c) {
      const auto next =
          c + 1 == mesh.face_offsets[static_cast<std::size_t>(f + 1)]
              ? mesh.face_offsets[static_cast<std::size_t>(f)]
              : c + 1;
      const auto vertex = vertices[static_cast<std::size_t>(
          mesh.corner_vertices[static_cast<std::size_t>(c)])];
      const auto second = vertices[static_cast<std::size_t>(
          mesh.corner_vertices[static_cast<std::size_t>(next)])];
      std::array<Id, 2> pair{vertex, second};
      if (pair[1] < pair[0]) std::swap(pair[0], pair[1]);
      auto found = edges.find(pair);
      Id edge;
      if (found == edges.end()) {
        edge = state->edges.Add({pair, {}}, issue());
        edges.emplace(pair, edge);
        state->vertices.Mutate(pair[0]).edges.push_back(edge);
        if (pair[0] != pair[1])
          state->vertices.Mutate(pair[1]).edges.push_back(edge);
      } else
        edge = found->second;
      const auto corner = state->corners.Add({vertex, edge, face}, issue());
      corners.push_back(corner);
      state->vertices.Mutate(vertex).corners.push_back(corner);
      state->edges.Mutate(edge).corners.push_back(corner);
      state->faces.Mutate(face).corners.push_back(corner);
    }
  }
  for (const auto& attribute : mesh.attributes) {
    auto record = std::make_shared<editing_detail::PropertyRecord>();
    auto& descriptor = record->descriptor;
    switch (attribute.domain) {
      case AttributeDomain::vertex:
        descriptor.domain = PropertyDomain::kVertex;
        break;
      case AttributeDomain::face:
        descriptor.domain = PropertyDomain::kFace;
        break;
      case AttributeDomain::corner:
        descriptor.domain = PropertyDomain::kCorner;
        break;
    }
    descriptor.name = attribute.name;
    descriptor.semantic = attribute.semantic;
    descriptor.set_index = attribute.set_index;
    descriptor.components = attribute.components;
    descriptor.ragged = attribute.offsets.has_value();
    descriptor.metadata = attribute.metadata;
    descriptor.scalar_type =
        static_cast<PropertyScalarType>(attribute.values.index());
    const auto& elements =
        descriptor.domain == PropertyDomain::kVertex ? vertices
        : descriptor.domain == PropertyDomain::kFace ? faces
                                                     : corners;
    for (std::size_t row = 0; row < elements.size(); ++row) {
      const auto begin =
          attribute.offsets
              ? (*attribute.offsets)[row]
              : row * static_cast<std::size_t>(attribute.components);
      const auto end = attribute.offsets ? (*attribute.offsets)[row + 1]
                                         : begin + attribute.components;
      PropertyRow value;
      value.values = std::visit(
          [&](const auto& data) -> AttributeValues {
            using Values = std::decay_t<decltype(data)>;
            return Values(
                data.begin() +
                    static_cast<typename Values::difference_type>(begin),
                data.begin() +
                    static_cast<typename Values::difference_type>(end));
          },
          attribute.values);
      value.present = !attribute.present || (*attribute.present)[row] == 1;
      record->rows.SetAt(elements[row], std::move(value));
    }
    state->properties.Add(std::move(record), issue());
  }
  result.mesh.owner_->accepted = state;
  result.correspondence = result.mesh.Snapshot().Materialize();
  return result;
}

ExecutionContext::ExecutionContext(ExecutionOptions options)
    : execution_(std::make_shared<editing_detail::Execution>()) {
  if (options.worker_budget == 0)
    options.worker_budget =
        std::max<std::size_t>(1, std::thread::hardware_concurrency());
  execution_->options = options;
}
std::size_t ExecutionContext::WorkerBudget() const {
  return Access::GetExecution(*this)->options.worker_budget;
}
std::size_t ExecutionContext::ActiveWorkers() const {
  const auto& execution = Access::GetExecution(*this);
  std::lock_guard lock(execution->mutex);
  return execution->active;
}
std::size_t ExecutionContext::PeakWorkers() const {
  const auto& execution = Access::GetExecution(*this);
  std::lock_guard lock(execution->mutex);
  return execution->peak;
}
std::size_t ExecutionContext::TrackedPayloadBudget() const {
  return Access::GetExecution(*this)->options.tracked_payload_budget_bytes;
}
std::size_t ExecutionContext::ActiveTrackedPayload() const {
  const auto& execution = Access::GetExecution(*this);
  std::lock_guard lock(execution->mutex);
  return execution->active_payload;
}
std::size_t ExecutionContext::PeakTrackedPayload() const {
  const auto& execution = Access::GetExecution(*this);
  std::lock_guard lock(execution->mutex);
  return execution->peak_payload;
}
BoundsResult ComputeBounds(const EditorSnapshot& snapshot,
                           const ExecutionContext& context,
                           std::stop_token stop) {
  const auto& source = Access::GetSnapshot(snapshot);
  const auto& state = *source.state;
  auto execution = Access::GetExecution(context);
  if (stop.stop_requested())
    Fail(EditorErrorCode::kCanceled, "Bounds canceled");
  const auto count = state.vertices.size - state.vertices.free_slots.size();
  BoundsResult result;
  if (count == 0) {
    result.serial_reason = "empty mesh";
    return result;
  }
  std::size_t desired = 0;
  if (count >= execution->options.minimum_parallel_vertices &&
      execution->options.worker_budget > 1) {
    const auto grain = std::max<std::size_t>(
        1, execution->options.minimum_parallel_vertices / 2);
    desired = std::min(static_cast<std::size_t>(count) / grain,
                       state.vertices.pages.size());
  }
  execution_detail::WorkerReservation reservation(execution, desired);
  const auto workers = reservation.Count();
  if (workers == 0) {
    result.serial_reason =
        execution->options.worker_budget <= 1 ? "worker budget is one"
        : count < execution->options.minimum_parallel_vertices
            ? "below parallel vertex threshold"
            : "shared worker budget unavailable or fewer than two pages";
  }
  std::atomic<bool> canceled{false};
  auto compute = [&](std::size_t first_page, std::size_t last_page) {
    std::optional<Bounds> bounds;
    for (std::size_t page = first_page; page < last_page; ++page) {
      if (stop.stop_requested()) {
        canceled.store(true, std::memory_order_relaxed);
        break;
      }
      for (const auto& slot : *state.vertices.pages[page])
        if (slot.value) {
          const auto& position = slot.value->position;
          if (!bounds)
            bounds = Bounds{position, position};
          else
            for (std::size_t axis = 0; axis < 3; ++axis) {
              bounds->minimum[axis] =
                  std::min(bounds->minimum[axis], position[axis]);
              bounds->maximum[axis] =
                  std::max(bounds->maximum[axis], position[axis]);
            }
        }
    }
    return bounds;
  };
  if (workers == 0)
    result.bounds = compute(0, state.vertices.pages.size());
  else {
    std::vector<std::optional<Bounds>> partial(workers);
    std::vector<std::jthread> threads;
    threads.reserve(workers);
    for (std::size_t i = 0; i < workers; ++i) {
      const auto first = state.vertices.pages.size() / workers * i +
                         std::min(i, state.vertices.pages.size() % workers);
      const auto last = state.vertices.pages.size() / workers * (i + 1) +
                        std::min(i + 1, state.vertices.pages.size() % workers);
      threads.emplace_back(
          [&, i, first, last] { partial[i] = compute(first, last); });
    }
    for (auto& thread : threads) thread.join();
    for (const auto& bounds : partial)
      if (bounds) {
        if (!result.bounds)
          result.bounds = bounds;
        else
          for (std::size_t axis = 0; axis < 3; ++axis) {
            result.bounds->minimum[axis] =
                std::min(result.bounds->minimum[axis], bounds->minimum[axis]);
            result.bounds->maximum[axis] =
                std::max(result.bounds->maximum[axis], bounds->maximum[axis]);
          }
      }
    result.workers_used = workers;
  }
  if (canceled.load(std::memory_order_relaxed) || stop.stop_requested())
    Fail(EditorErrorCode::kCanceled, "Bounds canceled");
  return result;
}
}  // namespace meshvale::geometry
