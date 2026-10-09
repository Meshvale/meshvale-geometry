// SPDX-License-Identifier: Apache-2.0
#include <meshvale/geometry/editable_mesh.h>

#include <array>
#include <cstdint>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <utility>
#include <vector>

namespace {

void Require(bool condition, const char* message) {
  if (!condition) throw std::runtime_error(message);
}

}  // namespace

int main() {
  using namespace meshvale::geometry;
  try {
    EditableMesh mesh;
    auto edit = mesh.BeginEdit();
    auto descriptor = PropertyDescriptor{};
    descriptor.name = "joint_indices";
    descriptor.scalar_type = PropertyScalarType::kUint64;
    descriptor.ragged = true;
    descriptor.semantic = "joint_index";
    descriptor.set_index = 1;
    descriptor.metadata = {{"skeleton", "installed-consumer"}};
    const auto property = edit.CreateProperty(descriptor);
    const auto a = edit.CreateVertex({0, 0, 0});
    const auto b = edit.CreateVertex({1, 0, 0});
    const auto c = edit.CreateVertex({0, 1, 0});
    edit.SetPropertyRow(
        property, a.Identity(),
        {std::vector<std::uint64_t>{9007199254740993ULL, 42}, true});
    const auto ab = edit.CreateEdge(a, b);
    const auto bc = edit.CreateEdge(b, c);
    const auto ca = edit.CreateEdge(c, a);
    const std::array<FaceCorner, 3> corners{
        FaceCorner{a, ab, {}}, FaceCorner{b, bc, {}}, FaceCorner{c, ca, {}}};
    const auto face = edit.CreateFace(corners);
    Require(!a.IsValid(), "pending element escaped before commit");
    (void)edit.Commit();
    const auto before = mesh.Snapshot();
    const auto raw = before.ExportMesh();
    Require(raw.face_offsets == std::vector<index_t>{0, 3} &&
                raw.attributes.size() == 1 &&
                raw.attributes.front().values ==
                    AttributeValues(
                        std::vector<std::uint64_t>{9007199254740993ULL, 42}),
            "installed editor lost polygon/property projection");
    auto moved = std::move(mesh);
    auto erase = moved.BeginEdit();
    erase.EraseFace(face, UnusedEdgePolicy::kPrune);
    erase.EraseVertex(a);
    const auto replacement = erase.CreateVertex({5, 6, 7});
    (void)erase.Commit();
    Require(!a.IsValid() && replacement.IsValid() &&
                before.Position(a) == std::array<double, 3>{0, 0, 0} &&
                before.Corners(face).size() == 3 &&
                before.Row(property, a.Identity()).values ==
                    AttributeValues(
                        std::vector<std::uint64_t>{9007199254740993ULL, 42}),
            "installed identities or snapshot lifetime are broken");
    const auto dense = moved.Snapshot().Materialize();
    Require(
        dense.vertex_rows.at(replacement.Identity()) < dense.vertices.size(),
        "installed dense correspondence lacks replacement identity");
    ExecutionContext execution({2, 1});
    const auto bounds = ComputeBounds(moved.Snapshot(), execution);
    Require(bounds.bounds &&
                bounds.bounds->minimum == std::array<double, 3>{0, 0, 0} &&
                bounds.bounds->maximum == std::array<double, 3>{5, 6, 7} &&
                execution.ActiveWorkers() == 0 &&
                (bounds.workers_used != 0 || !bounds.serial_reason.empty()),
            "installed small-snapshot bounds did not execute correctly");
    Mesh parallel_raw;
    for (int i = 0; i != 512; ++i)
      parallel_raw.positions.push_back(
          {static_cast<double>(i), static_cast<double>(-i), 1});
    auto parallel_mesh = EditableMesh::ImportMesh(parallel_raw);
    const auto parallel_bounds =
        ComputeBounds(parallel_mesh.mesh.Snapshot(), execution);
    Require(parallel_bounds.bounds &&
                parallel_bounds.bounds->minimum ==
                    std::array<double, 3>{0, -511, 1} &&
                parallel_bounds.bounds->maximum ==
                    std::array<double, 3>{511, 0, 1} &&
                parallel_bounds.workers_used == 2 &&
                execution.ActiveWorkers() == 0 && execution.PeakWorkers() == 2,
            "installed parallel bounds did not use the bounded worker target");
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
