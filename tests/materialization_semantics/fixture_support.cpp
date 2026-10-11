// SPDX-License-Identifier: Apache-2.0
#include "fixture_support.h"

#include <meshvale/geometry/editable_mesh.h>
#include <meshvale/geometry/materialization.h>

#include <cstdint>
#include <stdexcept>
namespace materialization_test {
namespace {
constexpr std::uint64_t kFixtureMaxElements = 4096;
constexpr std::uint64_t kFixtureMaxDescriptorBytes = 65536;
constexpr std::uint64_t kFixtureRequestBudgetBytes = 64 * 1024 * 1024;
constexpr std::uint64_t kParallelThresholdBytes = 64;
constexpr std::uint64_t kPartitionBytes = 16;
constexpr std::uint64_t kCancellationBlockScalars = 8;
constexpr std::uint64_t kMaximumJobs = 64;
}  // namespace
void Require(bool condition, const char* message) {
  if (!condition) throw std::runtime_error(message);
}
MeshMaterializationLimits Limits() {
  MeshMaterializationLimits limits;
  limits.max_vertices = kFixtureMaxElements;
  limits.max_faces = kFixtureMaxElements;
  limits.max_corners = kFixtureMaxElements;
  limits.max_face_offsets = kFixtureMaxElements;
  limits.max_attributes = kFixtureMaxElements;
  limits.max_attribute_scalars = kFixtureMaxElements;
  limits.max_attribute_offsets = kFixtureMaxElements;
  limits.max_presence_bytes = kFixtureMaxElements;
  limits.max_metadata_entries = kFixtureMaxElements;
  limits.max_descriptor_bytes = kFixtureMaxDescriptorBytes;
  limits.max_new_requested_bytes = kFixtureRequestBudgetBytes;
  return limits;
}
MeshMaterializationWorkPolicy Work() {
  return {kParallelThresholdBytes, kPartitionBytes, kCancellationBlockScalars,
          kMaximumJobs};
}
MeshInspectionLimits InspectionLimits(std::uint64_t issue_limit) {
  return {issue_limit, kFixtureRequestBudgetBytes};
}
MaterializedMesh Capture(const MeshMaterializationInput& input,
                         const ExecutionContext& context) {
  auto result = MaterializeMesh(input, Limits(), Work(), context);
  Require(result.status == MaterializationStatus::kSuccess &&
              result.mesh.has_value(),
          "semantic capture did not succeed on a qualified allocation profile");
  return *result.mesh;
}
MeshInspection Inspect(const MaterializedMesh& source,
                       std::uint64_t issue_limit) {
  auto result =
      InspectMaterializedMesh(source, InspectionLimits(issue_limit), Work());
  Require(result.status == MaterializationStatus::kSuccess &&
              result.inspection.has_value(),
          "inspection did not return its complete owner");
  return *result.inspection;
}
}  // namespace materialization_test
