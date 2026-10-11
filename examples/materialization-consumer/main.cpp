// SPDX-License-Identifier: Apache-2.0
#include <meshvale/geometry/materialization.h>

#include <array>
#include <cstddef>
#include <exception>
#include <iostream>
#include <span>
#include <stdexcept>
#include <string_view>
#include <variant>

namespace {
using namespace meshvale::geometry;

constexpr std::size_t kPayloadBudgetBytes = 1024 * 1024;
constexpr std::size_t kWorkerBudget = 2;
constexpr std::uint32_t kXyzComponents = 3;
constexpr std::uint32_t kUvComponents = 2;
constexpr std::uint32_t kUvSet = 0;
constexpr std::string_view kNormalName = "normal";
constexpr std::string_view kNormalSemantic = "NORMAL";
constexpr std::string_view kUvName = "uv";
constexpr std::string_view kUvSemantic = "TEXCOORD";
constexpr std::string_view kMetadataKey = "source";
constexpr std::string_view kMetadataValue = "installed-example";
constexpr MeshMaterializationLimits kSampleLimits{
    .max_vertices = 4,
    .max_faces = 2,
    .max_corners = 6,
    .max_face_offsets = 3,
    .max_attributes = 2,
    .max_attribute_scalars = 24,
    .max_attribute_offsets = 1,
    .max_presence_bytes = 1,
    .max_metadata_entries = 1,
    .max_descriptor_bytes = 128,
    .max_new_requested_bytes = kPayloadBudgetBytes};
constexpr MeshInspectionLimits kInspectionLimits{
    .max_issues = 16, .max_new_requested_bytes = kPayloadBudgetBytes};
constexpr MeshMaterializationWorkPolicy kWork{.parallel_min_bytes = 64,
                                              .partition_bytes = 64,
                                              .cancellation_block_scalars = 8,
                                              .max_jobs = 4};

void Require(bool condition) {
  if (!condition)
    throw std::runtime_error("installed materialization contract failed");
}

MaterializedMesh Capture(const ExecutionContext& context) {
  // Input storage only lives for this call; the result owns canonical buffers.
  const std::array<float, 12> positions{0, 0, 0, 1, 0, 0, 1, 1, 0, 0, 1, 0};
  const std::array<std::uint32_t, 6> corners{0, 1, 2, 0, 2, 3};
  const std::array<float, 12> normals{0, 0, 1, 0, 0, 1, 0, 0, 1, 0, 0, 1};
  const std::array<float, 12> uv{0, 0, 1, 0, 1, 1, 0, 0, 1, 1, 0, 1};
  const std::array<MetadataEntryView, 1> metadata{
      MetadataEntryView{kMetadataKey, kMetadataValue}};
  std::array<AttributeMaterializationInput, 2> attributes;
  attributes[0].domain = AttributeDomain::vertex;
  attributes[0].name = kNormalName;
  attributes[0].semantic = kNormalSemantic;
  attributes[0].components = kXyzComponents;
  attributes[0].values = std::span<const float>(normals);
  attributes[1].domain = AttributeDomain::corner;
  attributes[1].name = kUvName;
  attributes[1].semantic = kUvSemantic;
  attributes[1].set_index = kUvSet;
  attributes[1].components = kUvComponents;
  attributes[1].values = std::span<const float>(uv);
  attributes[1].metadata = metadata;
  MeshMaterializationInput input;
  input.positions = std::span<const float>(positions);
  input.topology = TriangleTopologyInput{corners};
  input.attributes = attributes;
  auto result = MaterializeMesh(input, kSampleLimits, kWork, context);
  Require(result.status == MaterializationStatus::kSuccess &&
          result.mesh.has_value());
  Require(result.execution.launched_workers == result.execution.joined_workers);
  return *result.mesh;
}

void InspectAndSelect() {
  ExecutionOptions options;
  options.worker_budget = kWorkerBudget;
  options.tracked_payload_budget_bytes = kPayloadBudgetBytes;
  ExecutionContext context(options);
  {
    const auto source = Capture(context);
    const auto inspected =
        InspectMaterializedMesh(source, kInspectionLimits, kWork);
    Require(inspected.status == MaterializationStatus::kSuccess &&
            inspected.inspection.has_value());
    Require(inspected.inspection->IsStorageValid());
    const std::array<index_t, 1> kept_faces{1};
    const auto selected =
        SelectMeshFaces(source, kept_faces, kSampleLimits, kWork);
    Require(selected.status == MaterializationStatus::kSuccess &&
            selected.selection.has_value());
    Require(selected.execution.launched_workers ==
            selected.execution.joined_workers);
    const auto original = source.View();
    const auto candidate = selected.selection->Candidate().View();
    Require(candidate.positions.data() == original.positions.data());
    Require(candidate.positions.size() == original.positions.size());
    const auto original_normals =
        std::get<std::span<const float>>(original.attributes[0].values);
    const auto selected_normals =
        std::get<std::span<const float>>(candidate.attributes[0].values);
    Require(selected_normals.data() == original_normals.data());
    Require(selected.selection->SourceFaces().size() == kept_faces.size());
    Require(selected.selection->SourceFaces()[0] == kept_faces[0]);
    const std::array<index_t, 3> expected_corners{3, 4, 5};
    Require(selected.selection->SourceCorners().size() ==
            expected_corners.size());
    for (std::size_t i = 0; i < expected_corners.size(); ++i)
      Require(selected.selection->SourceCorners()[i] == expected_corners[i]);
    const auto selected_uv =
        std::get<std::span<const float>>(candidate.attributes[1].values);
    const std::array<float, 6> expected_uv{0, 0, 1, 1, 0, 1};
    Require(selected_uv.size() == expected_uv.size());
    for (std::size_t i = 0; i < expected_uv.size(); ++i)
      Require(selected_uv[i] == expected_uv[i]);
    Require(candidate.attributes[1].metadata.size() == 1);
    Require(candidate.attributes[1].metadata[0].value == kMetadataValue);
    Require(source.NewRequestedBytes() > 0);
    Require(selected.selection->NewRequestedBytes() > 0);
  }
  Require(context.ActiveTrackedPayload() == 0);
  Require(context.ActiveWorkers() == 0);
}
}  // namespace

int main() {
  try {
    InspectAndSelect();
    std::cout
        << "Installed materialization, inspection and face selection passed\n";
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
