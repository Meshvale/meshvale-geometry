// SPDX-License-Identifier: Apache-2.0
#include <meshvale/geometry/editable_mesh.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <iostream>
#include <limits>
#include <new>
#include <optional>
#include <stdexcept>
#include <utility>
#include <vector>

#include "allocation_failure.h"

namespace {
using namespace meshvale::geometry;
using allocation_test::allocations_before_failure;
using allocation_test::reject_allocations;
constexpr std::size_t kFixtureVertices = 192;
constexpr std::size_t kRepeatedUpdates = 128;
constexpr std::size_t kFailureSweepLimit = 4096;
constexpr std::size_t kCountAllowance = std::numeric_limits<std::size_t>::max();
void Require(bool condition, const char* message) {
  if (!condition) throw std::runtime_error(message);
}
struct Fixture {
  EditableMesh mesh;
  std::vector<Vertex> vertices;
  Property property;
  Fixture() {
    auto session = mesh.BeginEdit();
    auto batch = session.BeginBatch();
    PropertyDescriptor descriptor;
    descriptor.name = "weights";
    descriptor.ragged = true;
    descriptor.scalar_type = PropertyScalarType::kUint64;
    property = batch.CreateProperty(std::move(descriptor));
    vertices.reserve(kFixtureVertices);
    for (std::size_t i = 0; i < kFixtureVertices; ++i)
      vertices.push_back(batch.CreateVertex({static_cast<double>(i), 0, 0}));
    batch.SetPropertyRow(
        property, vertices.front().Identity(),
        {std::vector<std::uint64_t>{9007199254740993ULL}, true});
    batch.Apply();
    (void)session.Commit();
  }
};
void NoAllocationTraversal() {
  Fixture fixture;
  auto session = fixture.mesh.BeginEdit();
  auto batch = session.BeginBatch();
  const auto a = fixture.vertices[0], b = fixture.vertices[1],
             c = fixture.vertices[2];
  const auto ab = batch.CreateEdge(a, b), bc = batch.CreateEdge(b, c),
             ca = batch.CreateEdge(c, a);
  const std::array<FaceCorner, 3> cycle{
      FaceCorner{a, ab, {}}, FaceCorner{b, bc, {}}, FaceCorner{c, ca, {}}};
  const auto face = batch.CreateFace(cycle);
  batch.EraseVertex(fixture.vertices.back());
  batch.Apply();
  (void)session.Commit();
  const auto snapshot = fixture.mesh.Snapshot();
  std::size_t vertices = 0, edges = 0, faces = 0, corners = 0, properties = 0;
  reject_allocations = true;
  try {
    auto range = snapshot.VertexElements();
    auto copy = range;
    auto first = copy.begin();
    auto independent = first++;
    Require((*independent).Identity() == a.Identity(),
            "iterator lost first vertex");
    for (auto vertex : range) {
      (void)snapshot.Position(vertex);
      (void)snapshot.ViewRow(fixture.property, vertex.Identity()).Values();
      for (auto edge : snapshot.EdgeElements(vertex))
        (void)snapshot.Endpoints(edge);
      for (auto corner : snapshot.CornerElements(vertex))
        (void)snapshot.GetVertex(corner);
      ++vertices;
    }
    for (auto edge : snapshot.EdgeElements()) {
      for (auto corner : snapshot.CornerElements(edge))
        (void)snapshot.GetEdge(corner);
      ++edges;
    }
    for (auto item : snapshot.FaceElements()) {
      for (auto corner : snapshot.CornerElements(item))
        (void)snapshot.GetFace(corner);
      ++faces;
    }
    for (auto corner : snapshot.CornerElements()) {
      (void)snapshot.GetVertex(corner);
      ++corners;
    }
    for (auto property : snapshot.PropertyElements()) {
      Require(snapshot.ViewRow(property, a.Identity()).IsPresent(),
              "present row changed");
      ++properties;
    }
    Require(
        snapshot.GetFace(*snapshot.CornerElements(face).begin()).Identity() ==
            face.Identity(),
        "face traversal changed owner");
  } catch (...) {
    reject_allocations = false;
    throw;
  }
  reject_allocations = false;
  Require(vertices == kFixtureVertices - 1 && edges == 3 && faces == 1 &&
              corners == 3 && properties == 1,
          "allocation-free traversal changed cardinality");
}
void AmortizedMetadataCopies() {
  Fixture fixture;
  auto separate = fixture.mesh.BeginEdit();
  allocations_before_failure = kCountAllowance;
  for (std::size_t i = 0; i < kRepeatedUpdates; ++i)
    separate.SetPosition(fixture.vertices.front(),
                         {static_cast<double>(i), 1, 2});
  const auto separate_count = kCountAllowance - *allocations_before_failure;
  allocations_before_failure.reset();
  auto parent = fixture.mesh.BeginEdit();
  allocations_before_failure = kCountAllowance;
  auto batch = parent.BeginBatch();
  for (std::size_t i = 0; i < kRepeatedUpdates; ++i)
    batch.SetPosition(fixture.vertices.front(), {static_cast<double>(i), 1, 2});
  batch.Apply();
  const auto batch_count = kCountAllowance - *allocations_before_failure;
  allocations_before_failure.reset();
  Require(batch_count < separate_count &&
              parent.Snapshot().Position(fixture.vertices.front()) ==
                  separate.Snapshot().Position(fixture.vertices.front()),
          "batch did not amortize ordinary metadata/page allocations");
  std::cout << "Ordinary allocation comparison: individual=" << separate_count
            << " batch=" << batch_count << " updates=" << kRepeatedUpdates
            << '\n';
}
void FailureRollbackSweep() {
  std::size_t refused = 0;
  bool completed = false;
  for (std::size_t allowance = 0; allowance < kFailureSweepLimit; ++allowance) {
    Fixture fixture;
    auto parent = fixture.mesh.BeginEdit();
    const auto a = fixture.vertices.front();
    parent.SetPosition(a, {7, 8, 9});
    const auto previous = parent.Snapshot();
    std::optional<EditBatch> batch;
    Vertex abandoned;
    PropertyDescriptor descriptor;
    descriptor.name = "face_values";
    descriptor.domain = PropertyDomain::kFace;
    descriptor.scalar_type = PropertyScalarType::kUint64;
    PropertyRow row{std::vector<std::uint64_t>{42}, true};
    allocations_before_failure = allowance;
    try {
      batch.emplace(parent.BeginBatch());
      abandoned = batch->CreateVertex({10, 11, 12});
      const auto edge = batch->CreateEdge(a, abandoned);
      const auto self = batch->CreateEdge(a, a);
      const std::array<FaceCorner, 3> cycle{FaceCorner{a, edge, {}},
                                            FaceCorner{abandoned, edge, {}},
                                            FaceCorner{a, self, {}}};
      const auto face = batch->CreateFace(cycle);
      const auto property = batch->CreateProperty(std::move(descriptor));
      batch->SetPropertyRow(property, face.Identity(), std::move(row));
      batch->SetPosition(a, {20, 21, 22});
      const auto captured = batch->Snapshot();
      batch->EraseFace(face, UnusedEdgePolicy::kPrune);
      batch->EraseVertex(abandoned);
      batch->RemoveProperty(property);
      batch->Apply();
      allocations_before_failure.reset();
      Require(captured.Position(a) == std::array<double, 3>{20, 21, 22} &&
                  parent.Snapshot().Position(a) ==
                      std::array<double, 3>{20, 21, 22} &&
                  previous.Position(a) == std::array<double, 3>{7, 8, 9},
              "successful fault-sweep batch lost snapshot isolation");
      completed = true;
    } catch (const std::bad_alloc&) {
      allocations_before_failure.reset();
      ++refused;
      Require(
          parent.Snapshot().Position(a) == std::array<double, 3>{7, 8, 9} &&
              fixture.mesh.Snapshot().Position(a) ==
                  std::array<double, 3>{0, 0, 0} &&
              !abandoned.IsValid(),
          "allocation failure changed accepted state or previous candidate");
      if (batch) {
        bool closed = false;
        try {
          batch->Apply();
        } catch (const EditorError& error) {
          closed = error.Code() == EditorErrorCode::kSessionClosed;
        }
        Require(closed,
                "allocation failure left partially changed batch applyable");
      }
      // The failed batch releases the parent's gate; the previous candidate is
      // still usable and can publish without abandoned objects/log entries.
      (void)parent.Commit();
      Require(fixture.mesh.Snapshot().Position(a) ==
                      std::array<double, 3>{7, 8, 9} &&
                  fixture.mesh.Snapshot().Vertices().size() == kFixtureVertices,
              "failed batch logs entered subsequent parent commit");
    }
    if (completed) break;
  }
  Require(completed && refused != 0,
          "allocation sweep did not reach both failure and success");
  std::cout << "Atomic batch ordinary allocation fault boundaries=" << refused
            << '\n';
}
}  // namespace
int main() {
  try {
    NoAllocationTraversal();
    AmortizedMetadataCopies();
    FailureRollbackSweep();
    std::cout << "Editor traversal/batch allocation scenarios passed\n";
    return 0;
  } catch (const std::exception& error) {
    allocations_before_failure.reset();
    reject_allocations = false;
    std::cerr << error.what() << '\n';
    return 1;
  }
}
