// SPDX-License-Identifier: Apache-2.0
#ifndef MESHVALE_GEOMETRY_MATERIALIZATION_H_
#define MESHVALE_GEOMETRY_MATERIALIZATION_H_

#include <meshvale/geometry/attributes.h>
#include <meshvale/geometry/execution.h>

#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <stop_token>
#include <string_view>
#include <variant>

namespace meshvale::geometry {
namespace materialization_detail {
struct Access;
struct MeshState;
struct InspectionState;
struct SelectionState;
}  // namespace materialization_detail

// Borrowed native scalars. A monostate ingress is an invalid transport
// selector.
using ScalarReadView =
    std::variant<std::span<const float>, std::span<const double>,
                 std::span<const std::int32_t>, std::span<const std::uint8_t>,
                 std::span<const std::uint16_t>, std::span<const std::uint32_t>,
                 std::span<const std::uint64_t>>;
using ScalarIngress =
    std::variant<std::monostate, std::span<const float>,
                 std::span<const double>, std::span<const std::int32_t>,
                 std::span<const std::uint8_t>, std::span<const std::uint16_t>,
                 std::span<const std::uint32_t>, std::span<const std::uint64_t>,
                 std::span<const std::int8_t>, std::span<const std::int16_t>>;
using PositionIngress = std::variant<std::monostate, std::span<const float>,
                                     std::span<const double>>;

struct MetadataEntryView {
  std::string_view key;
  std::string_view value;
};
struct AttributeReadView {
  AttributeDomain domain = AttributeDomain::vertex;
  std::string_view name;
  std::string_view semantic;
  std::optional<std::uint32_t> set_index;
  std::uint32_t components = 0;
  ScalarReadView values;
  std::optional<std::span<const index_t>> offsets;
  std::optional<std::span<const std::uint8_t>> present;
  std::span<const MetadataEntryView> metadata;
};
struct MeshReadView {
  // Interleaved complete XYZ rows, backed by the canonical double VectorBuffer.
  std::span<const double> positions;
  std::span<const index_t> face_offsets;
  std::span<const index_t> corner_vertices;
  std::span<const AttributeReadView> attributes;
};
struct AttributeMaterializationInput {
  AttributeDomain domain = AttributeDomain::vertex;
  std::string_view name;
  std::string_view semantic;
  std::optional<std::uint32_t> set_index;
  std::uint32_t components = 0;
  ScalarIngress values;
  std::optional<std::span<const index_t>> offsets;
  std::optional<std::span<const std::uint8_t>> present;
  std::span<const MetadataEntryView> metadata;
};
struct PolygonTopologyInput {
  std::span<const index_t> face_offsets;
  std::span<const index_t> corner_vertices;
};
struct TriangleTopologyInput {
  std::span<const std::uint32_t> corner_vertices;
};
using TopologyIngress =
    std::variant<std::monostate, PolygonTopologyInput, TriangleTopologyInput>;
struct MeshMaterializationInput {
  PositionIngress positions;
  TopologyIngress topology;
  std::span<const AttributeMaterializationInput> attributes;
};

// Every field is required and positive; limits are inclusive physical counts.
struct MeshMaterializationLimits {
  std::uint64_t max_vertices = 0;
  std::uint64_t max_faces = 0;
  std::uint64_t max_corners = 0;
  std::uint64_t max_face_offsets = 0;
  std::uint64_t max_attributes = 0;
  std::uint64_t max_attribute_scalars = 0;
  std::uint64_t max_attribute_offsets = 0;
  std::uint64_t max_presence_bytes = 0;
  std::uint64_t max_metadata_entries = 0;
  std::uint64_t max_descriptor_bytes = 0;
  std::uint64_t max_new_requested_bytes = 0;
};
struct MeshInspectionLimits {
  std::uint64_t max_issues = 0;
  std::uint64_t max_new_requested_bytes = 0;
};
struct MeshMaterializationWorkPolicy {
  std::uint64_t parallel_min_bytes = 0;
  std::uint64_t partition_bytes = 0;
  std::uint64_t cancellation_block_scalars = 0;
  std::uint64_t max_jobs = 0;
};
struct MaterializationExecutionStats {
  std::uint64_t planned_jobs = 0;
  std::uint64_t launched_workers = 0;
  std::uint64_t joined_workers = 0;
  std::uint64_t caller_jobs = 0;
};
enum class StorageIssueCode {
  kPositionNonfinite,
  kFaceOffsetsEmpty,
  kFaceOffsetsStart,
  kFaceOffsetsEnd,
  kFaceOffsetRange,
  kFaceOffsetOrder,
  kFaceTooShort,
  kCornerVertexRange,
  kAttributeDuplicateName,
  kAttributeDomain,
  kAttributeNameEmpty,
  kAttributeComponentsZero,
  kAttributeDenseShape,
  kAttributeOffsetsEmpty,
  kAttributeOffsetsCount,
  kAttributeOffsetsStart,
  kAttributeOffsetsEnd,
  kAttributeOffsetRange,
  kAttributeOffsetOrder,
  kAttributeOffsetAlignment,
  kAttributePresenceCount,
  kAttributePresenceValue,
  kMetadataDuplicateKey
};
struct StorageIssue {
  StorageIssueCode code = StorageIssueCode::kPositionNonfinite;
  std::optional<index_t> attribute;
  std::optional<index_t> element;
};
enum class MaterializationStatus {
  kSuccess,
  kInvalidConfiguration,
  kInvalidTransport,
  kLimitExceeded,
  kUnsupportedAllocationProfile,
  kBudgetExceeded,
  kCanceled,
  kInvalidSourceStorage,
  kInvalidSelection
};
enum class MaterializationDiagnosticCode {
  kNone,
  kRequiredFieldZero,
  kConfigurationExtent,
  kTransportSelector,
  kPositionExtent,
  kTriangleExtent,
  kStorageExtent,
  kCountLimit,
  kIssueLimit,
  kAllocationProfile,
  kRequestBudget,
  kStopRequested,
  kSourceStorage,
  kSelectionOrder,
  kSelectionRange
};
struct MaterializationDiagnostic {
  MaterializationDiagnosticCode code = MaterializationDiagnosticCode::kNone;
  std::optional<index_t> attribute;
  std::optional<index_t> element;
  std::optional<StorageIssue> storage_issue;
};

// Copy-only strong owners: copying shares immutable storage and charges.
// Views are borrowed and require their visible owner to remain alive.
class MaterializedMesh {
 public:
  MaterializedMesh(const MaterializedMesh&) noexcept;
  MaterializedMesh& operator=(const MaterializedMesh&) noexcept;
  ~MaterializedMesh() noexcept;
  [[nodiscard]] MeshReadView View() const noexcept;
  [[nodiscard]] std::uint64_t NewRequestedBytes() const noexcept;

 private:
  friend struct materialization_detail::Access;
  explicit MaterializedMesh(
      std::shared_ptr<const materialization_detail::MeshState> state) noexcept;
  std::shared_ptr<const materialization_detail::MeshState> state_;
};
class MeshInspection {
 public:
  MeshInspection(const MeshInspection&) noexcept;
  MeshInspection& operator=(const MeshInspection&) noexcept;
  ~MeshInspection() noexcept;
  [[nodiscard]] const MaterializedMesh& Source() const noexcept;
  [[nodiscard]] std::span<const StorageIssue> Issues() const noexcept;
  [[nodiscard]] bool IsStorageValid() const noexcept;
  [[nodiscard]] std::uint64_t NewRequestedBytes() const noexcept;

 private:
  friend struct materialization_detail::Access;
  explicit MeshInspection(
      std::shared_ptr<const materialization_detail::InspectionState>
          state) noexcept;
  std::shared_ptr<const materialization_detail::InspectionState> state_;
};
class MaterializedFaceSelection {
 public:
  MaterializedFaceSelection(const MaterializedFaceSelection&) noexcept;
  MaterializedFaceSelection& operator=(
      const MaterializedFaceSelection&) noexcept;
  ~MaterializedFaceSelection() noexcept;
  [[nodiscard]] const MaterializedMesh& Candidate() const noexcept;
  // Output row -> source row. Vertex correspondence is identity.
  [[nodiscard]] std::span<const index_t> SourceFaces() const noexcept;
  [[nodiscard]] std::span<const index_t> SourceCorners() const noexcept;
  [[nodiscard]] index_t SourceVertexCount() const noexcept;
  [[nodiscard]] index_t SourceFaceCount() const noexcept;
  [[nodiscard]] index_t SourceCornerCount() const noexcept;
  [[nodiscard]] std::uint64_t NewRequestedBytes() const noexcept;

 private:
  friend struct materialization_detail::Access;
  explicit MaterializedFaceSelection(
      std::shared_ptr<const materialization_detail::SelectionState>
          state) noexcept;
  std::shared_ptr<const materialization_detail::SelectionState> state_;
};
struct MeshMaterializationResult {
  MaterializationStatus status = MaterializationStatus::kInvalidConfiguration;
  std::optional<MaterializedMesh> mesh;
  MaterializationDiagnostic diagnostic;
  MaterializationExecutionStats execution;
};
struct MeshInspectionResult {
  MaterializationStatus status = MaterializationStatus::kInvalidConfiguration;
  std::optional<MeshInspection> inspection;
  MaterializationDiagnostic diagnostic;
  MaterializationExecutionStats execution;
};
struct MeshFaceSelectionResult {
  MaterializationStatus status = MaterializationStatus::kInvalidConfiguration;
  std::optional<MaterializedFaceSelection> selection;
  MaterializationDiagnostic diagnostic;
  MaterializationExecutionStats execution;
};

[[nodiscard]] MeshMaterializationResult MaterializeMesh(
    const MeshMaterializationInput& input,
    const MeshMaterializationLimits& limits,
    const MeshMaterializationWorkPolicy& work, const ExecutionContext& context,
    std::stop_token stop = {});
[[nodiscard]] MeshInspectionResult InspectMaterializedMesh(
    const MaterializedMesh& source, const MeshInspectionLimits& limits,
    const MeshMaterializationWorkPolicy& work, std::stop_token stop = {});
[[nodiscard]] MeshFaceSelectionResult SelectMeshFaces(
    const MaterializedMesh& source, std::span<const index_t> kept_faces,
    const MeshMaterializationLimits& limits,
    const MeshMaterializationWorkPolicy& work, std::stop_token stop = {});

}  // namespace meshvale::geometry
#endif  // MESHVALE_GEOMETRY_MATERIALIZATION_H_
