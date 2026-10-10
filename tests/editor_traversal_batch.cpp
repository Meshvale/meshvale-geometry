// SPDX-License-Identifier: Apache-2.0
#include <meshvale/geometry/editable_mesh.h>
#include <meshvale/geometry/execution.h>

#include <array>
#include <cmath>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <future>
#include <iostream>
#include <limits>
#include <optional>
#include <ranges>
#include <stdexcept>
#include <stop_token>
#include <utility>
#include <vector>

namespace {
using namespace meshvale::geometry;
static_assert(std::ranges::forward_range<SnapshotRange<Vertex>>);
static_assert(std::ranges::forward_range<SnapshotRange<Edge>>);
static_assert(std::ranges::forward_range<SnapshotRange<Face>>);
static_assert(std::ranges::forward_range<SnapshotRange<Corner>>);
static_assert(std::ranges::forward_range<SnapshotRange<Property>>);
void Require(bool condition, const char* message) {
  if (!condition) throw std::runtime_error(message);
}
template <class Callable>
void RequireError(Callable&& call, EditorErrorCode code, const char* message) {
  try {
    std::forward<Callable>(call)();
  } catch (const EditorError& error) {
    Require(error.Code() == code, message);
    return;
  }
  throw std::runtime_error(message);
}
template <class Element>
std::vector<ElementIdentity> Identities(const SnapshotRange<Element>& range) {
  std::vector<ElementIdentity> result;
  for (auto element : range) result.push_back(element.Identity());
  return result;
}
void RangesAndRows() {
  EditorSnapshot captured;
  SnapshotRange<Vertex> pinned;
  SnapshotRange<Vertex>::Iterator iterator;
  PropertyRowView row_view;
  Vertex a, removed;
  Face face;
  {
    EditableMesh mesh;
    auto session = mesh.BeginEdit();
    auto batch = session.BeginBatch();
    a = batch.CreateVertex({0, 0, 0});
    removed = batch.CreateVertex({9, 9, 9});
    const auto b = batch.CreateVertex({1, 0, 0});
    const auto c = batch.CreateVertex({0, 1, 0});
    const auto ab = batch.CreateEdge(a, b);
    const auto bc = batch.CreateEdge(b, c);
    const auto ca = batch.CreateEdge(c, a);
    const auto parallel = batch.CreateEdge(a, b);
    const auto wire = batch.CreateEdge(a, c);
    const std::array<FaceCorner, 3> cycle{
        FaceCorner{a, ab, {}}, FaceCorner{b, bc, {}}, FaceCorner{c, ca, {}}};
    face = batch.CreateFace(cycle);
    (void)batch.CreateFace(cycle);
    (void)batch.CreateFace(cycle);
    PropertyDescriptor descriptor;
    descriptor.name = "weights";
    descriptor.ragged = true;
    descriptor.scalar_type = PropertyScalarType::kUint64;
    const auto property = batch.CreateProperty(descriptor);
    batch.SetPropertyRow(
        property, a.Identity(),
        {std::vector<std::uint64_t>{9007199254740993ULL, 42}, true});
    batch.EraseVertex(removed);
    batch.Apply();
    (void)session.Commit();
    captured = mesh.Snapshot();
    pinned = captured.VertexElements();
    iterator = pinned.begin();
    row_view = captured.ViewRow(property, a.Identity());
    Require(std::ranges::distance(pinned) == 3 &&
                std::ranges::distance(captured.EdgeElements()) == 5 &&
                std::ranges::distance(captured.FaceElements()) == 3 &&
                std::ranges::distance(captured.CornerElements()) == 9 &&
                std::ranges::distance(captured.PropertyElements()) == 1,
            "pool ranges included holes or omitted live identities");
    Require(
        Identities(captured.EdgeElements(a)) ==
            std::vector<ElementIdentity>{ab.Identity(), ca.Identity(),
                                         parallel.Identity(), wire.Identity()},
        "vertex incidence range changed authored edge order");
    Require(std::ranges::distance(captured.CornerElements(ab)) == 3 &&
                std::ranges::distance(captured.CornerElements(parallel)) == 0 &&
                std::ranges::distance(captured.CornerElements(wire)) == 0 &&
                std::ranges::distance(captured.CornerElements(a)) == 3,
            "wire/parallel/non-manifold incidence was lost");
    Require(
        Identities(captured.CornerElements(face)) ==
            [&] {
              std::vector<ElementIdentity> ids;
              for (auto corner : captured.Corners(face))
                ids.push_back(corner.Identity());
              return ids;
            }(),
        "face range disagrees with its authored corner cycle");
    for (auto corner : captured.CornerElements(face)) {
      Require(captured.GetFace(corner).Identity() == face.Identity(),
              "snapshot corner face query changed identity");
      Require(captured.GetVertex(corner).Identity() ==
                      corner.GetVertex().Identity() &&
                  captured.GetEdge(corner).Identity() ==
                      corner.GetEdge().Identity(),
              "snapshot corner navigation disagrees with accepted state");
    }
    auto copied = iterator;
    Require((*copied).Identity() == (*iterator).Identity() &&
                (*(copied++)).Identity() == a.Identity() && copied != iterator,
            "forward iterator copy/postincrement is not independent");
    Require(!captured.ViewRow(property, b.Identity()).IsPresent(),
            "missing row became present in read-only view");
    auto change = mesh.BeginEdit();
    change.SetPosition(a, {7, 8, 9});
    change.SetPropertyRow(property, a.Identity(),
                          {std::vector<std::uint64_t>{5}, true});
    (void)change.Commit();
    Require(a.Position() == std::array<double, 3>{7, 8, 9} &&
                captured.Position(*iterator) == std::array<double, 3>{0, 0, 0},
            "range handle current-state semantics or captured values changed");
    captured = {};
    pinned = {};
  }
  Require(
      (*iterator).Identity() == a.Identity() && !a.IsValid() &&
          row_view.Values() == AttributeValues(std::vector<std::uint64_t>{
                                   9007199254740993ULL, 42}) &&
          row_view.IsPresent(),
      "iterator/row view failed to retain captured data beyond owner lifetime");
  ++iterator;
  Require((*iterator).Identity() != removed.Identity(),
          "retained iterator revived a dead slot");
  Require(std::ranges::empty(SnapshotRange<Vertex>{}),
          "default range is not empty");
  RequireError([&] { (void)EditorSnapshot{}.VertexElements(); },
               EditorErrorCode::kInvalidObject,
               "default snapshot exposed a range");
  RequireError([&] { (void)PropertyRowView{}.Values(); },
               EditorErrorCode::kInvalidObject,
               "default row view dereferenced null");
  auto moved_view = std::move(row_view);
  Require(moved_view.IsPresent(), "moved row view lost its snapshot");
  RequireError([&] { (void)row_view.Values(); },
               EditorErrorCode::kInvalidObject,
               "moved-from row view used an unowned backing pointer");
}
void AtomicBatches() {
  EditableMesh mesh;
  auto session = mesh.BeginEdit();
  const auto a = session.CreateVertex({1, 2, 3});
  const auto prior = session.Snapshot();
  auto batch = session.BeginBatch();
  const auto b = batch.CreateVertex({4, 5, 6});
  const auto edge = batch.CreateEdge(a, b);
  batch.SetPosition(a, {8, 9, 10});
  const auto captured = batch.Snapshot();
  batch.SetPosition(a, {11, 12, 13});
  Require(
      captured.Position(a) == std::array<double, 3>{8, 9, 10} &&
          prior.Position(a) == std::array<double, 3>{1, 2, 3} &&
          session.Snapshot().Position(a) == std::array<double, 3>{1, 2, 3} &&
          std::ranges::empty(mesh.Snapshot().VertexElements()),
      "batch changes escaped or mutated a retained observation");
  RequireError([&] { session.SetPosition(a, {0, 0, 0}); },
               EditorErrorCode::kBatchActive,
               "parent mutation proceeded during batch");
  RequireError([&] { (void)session.BeginBatch(); },
               EditorErrorCode::kBatchActive, "second active batch proceeded");
  RequireError([&] { (void)session.Commit(); }, EditorErrorCode::kBatchActive,
               "parent commit proceeded during batch");
  auto moved_session = std::move(session);
  batch.Apply();
  Require(moved_session.Snapshot().Position(a) ==
                  std::array<double, 3>{11, 12, 13} &&
              moved_session.Snapshot().Endpoints(edge)[1].Identity() ==
                  b.Identity(),
          "batch apply did not accept into moved parent session");
  RequireError([&] { batch.Apply(); }, EditorErrorCode::kSessionClosed,
               "batch applied twice");
  const auto changes = moved_session.Commit();
  Require(changes.created.size() == 3 && a.IsValid() && b.IsValid(),
          "batch change log or publication lost mixed creations");
  auto failing = mesh.BeginEdit();
  failing.SetPosition(a, {20, 21, 22});
  auto rejected = failing.BeginBatch();
  const auto abandoned = rejected.CreateVertex({30, 31, 32});
  rejected.SetPosition(a, {40, 41, 42});
  const auto retained = rejected.Snapshot();
  RequireError([&] { rejected.EraseVertex(a); },
               EditorErrorCode::kReferencedElement,
               "invalid batch did not reject referenced vertex");
  RequireError([&] { rejected.Apply(); }, EditorErrorCode::kSessionClosed,
               "failed batch remained applyable");
  Require(
      failing.Snapshot().Position(a) == std::array<double, 3>{20, 21, 22} &&
          retained.Position(abandoned) == std::array<double, 3>{30, 31, 32} &&
          !abandoned.IsValid(),
      "batch failure lost prior candidate or snapshot");
  auto replacement = failing.BeginBatch();
  const auto fresh = replacement.CreateVertex({50, 51, 52});
  Require(fresh.Identity() != abandoned.Identity(),
          "discarded batch identity revived");
  replacement.Apply();
  (void)failing.Commit();
  Require(!abandoned.IsValid(), "failed batch creation entered accepted mesh");
  auto canceled_parent = mesh.BeginEdit();
  canceled_parent.SetPosition(a, {60, 61, 62});
  auto canceled = canceled_parent.BeginBatch();
  canceled.SetPosition(a, {70, 71, 72});
  std::stop_source stop;
  stop.request_stop();
  RequireError([&] { canceled.Apply(stop.get_token()); },
               EditorErrorCode::kCanceled, "canceled batch applied");
  Require(canceled_parent.Snapshot().Position(a) ==
              std::array<double, 3>{60, 61, 62},
          "canceled apply lost prior candidate");
  (void)canceled_parent.Commit();
  RequireError([&] { canceled.Apply(); }, EditorErrorCode::kSessionClosed,
               "canceled batch was retryable without a new batch");
}
void ParentLifetimeAndThreads() {
  EditableMesh mesh;
  std::optional<EditBatch> orphan;
  {
    auto session = mesh.BeginEdit();
    orphan.emplace(session.BeginBatch());
    (void)orphan->CreateVertex({1, 2, 3});
  }
  RequireError([&] { orphan->Apply(); }, EditorErrorCode::kSessionClosed,
               "batch accepted through destroyed session");
  auto session = mesh.BeginEdit();
  auto discarded = session.BeginBatch();
  session.Discard();
  RequireError([&] { (void)discarded.CreateVertex({0, 0, 0}); },
               EditorErrorCode::kSessionClosed,
               "batch survived explicit parent discard");
  auto threaded = mesh.BeginEdit();
  auto batch = threaded.BeginBatch();
  const auto code = std::async(std::launch::async, [&] {
                      try {
                        batch.SetPosition(Vertex{}, {1, 2, 3});
                      } catch (const EditorError& error) {
                        return error.Code();
                      }
                      return EditorErrorCode::kInvalidTopology;
                    }).get();
  Require(code == EditorErrorCode::kWrongThread,
          "batch ignored creating thread");
  (void)threaded.CreateVertex({3, 4, 5});
  (void)threaded.Commit();
  std::optional<EditSession> dead_owner_parent;
  std::optional<EditBatch> dead_owner_batch;
  {
    EditableMesh temporary;
    dead_owner_parent.emplace(temporary.BeginEdit());
    dead_owner_batch.emplace(dead_owner_parent->BeginBatch());
  }
  RequireError([&] { dead_owner_batch->Apply(); },
               EditorErrorCode::kOwnerDestroyed,
               "batch applied after aggregate destruction");
  auto first = mesh.BeginEdit();
  auto second = mesh.BeginEdit();
  auto first_batch = first.BeginBatch();
  auto second_batch = second.BeginBatch();
  second_batch = std::move(first_batch);
  (void)second.CreateVertex({1, 2, 3});
  second_batch.Apply();
  (void)first.Commit();
  RequireError([&] { (void)second.Commit(); },
               EditorErrorCode::kRevisionConflict,
               "batch bypassed existing same-base conflict detection");
}
}  // namespace
int main() {
  try {
    RangesAndRows();
    AtomicBatches();
    ParentLifetimeAndThreads();
    std::cout << "Snapshot traversal and atomic batch scenarios passed\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
