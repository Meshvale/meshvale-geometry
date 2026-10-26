// SPDX-License-Identifier: Apache-2.0
#ifndef MESHVALE_GEOMETRY_CHARGED_TRIANGULATION_H_
#define MESHVALE_GEOMETRY_CHARGED_TRIANGULATION_H_

#include <meshvale/geometry/attributes.h>
#include <meshvale/geometry/execution.h>
#include <meshvale/geometry/materialization.h>
#include <meshvale/geometry/mesh.h>
#include <meshvale/geometry/triangulation.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <stop_token>

namespace meshvale::geometry {
namespace charged_triangulation_detail {
struct Access;
struct State;
}  // namespace charged_triangulation_detail

enum class ChargedTriangulationStatus {
  kAccepted = 0,
  kInvalidConfiguration = 1,
  kLimitExceeded = 2,
  kBlocked = 3,
  kUnsupportedAllocationProfile = 4,
  kBudgetExceeded = 5,
  kCanceled = 6,
  kDelegateFailure = 7,
};
enum class ChargedTriangulationStage {
  kConfiguration = 0,
  kSourceCounts = 1,
  kSourceCapture = 2,
  kSourceInspection = 3,
  kTriangulation = 4,
  kCorrespondence = 5,
  kAttributeTransfer = 6,
  kOutputMaterialization = 7,
  kOutputInspection = 8,
  kComplete = 9,
};
enum class ChargedTriangulationCode {
  kNone = 0,
  kRequiredFieldZero = 1,
  kConfigurationExtent = 2,
  kUnsupportedCornerLimit = 3,
  kSourceCountLimit = 4,
  kOutputCountLimit = 5,
  kMapEntryLimit = 6,
  kCoreRequestBudget = 7,
  kBridgeRequestBudget = 8,
  kSourceStorage = 9,
  kFaceCornerLimit = 10,
  kRepeatedPosition = 11,
  kDegenerateFace = 12,
  kNonplanarFace = 13,
  kBacktrackingBoundary = 14,
  kSelfIntersection = 15,
  kNoGeometricEar = 16,
  kDegenerateResult = 17,
  kSizeOverflow = 18,
  kRequestBudget = 19,
  kStopRequested = 20,
  kMaterialization = 21,
  kInspection = 22,
  kOutputStorage = 23,
  kAllocationProfile = 24,
};
enum class ChargedTriangulationSerialReason {
  kNone = 0,
  kEmptyMesh = 1,
  kWorkerBudgetOne = 2,
  kBelowFaceThreshold = 3,
  kSharedWorkersUnavailableOrFewerThanTwoFaces = 4,
};

struct ChargedTriangulationLimits {
  // Inclusive physical source counts/descriptors in both overloads.
  // max_new_requested_bytes caps the actual legacy-source capture only;
  // an already retained source has no capture call or fresh capture requests.
  MeshMaterializationLimits source;
  MeshInspectionLimits source_inspection;
  MeshMaterializationLimits output;
  MeshInspectionLimits output_inspection;
  // Sum of all three retained map lengths, not a per-map allowance.
  std::uint64_t max_map_entries = 0;
  // Actual simultaneous-live core original requests, including accounting
  // state, aligned headers, integer limbs, plans and explicit worker storage.
  std::uint64_t max_core_new_requested_bytes = 0;
  // Actual live bridge requests, including adaptation/gather/worker scratch,
  // maps and their owner/control blocks. Delegates have their own limits.
  std::uint64_t max_bridge_new_requested_bytes = 0;
};

struct ChargedTriangulationCounts {
  index_t source_vertices = 0;
  index_t source_faces = 0;
  index_t source_corners = 0;
  index_t output_vertices = 0;
  index_t output_faces = 0;
  index_t output_corners = 0;
};
struct ChargedTriangulationDiagnostic {
  ChargedTriangulationStage stage = ChargedTriangulationStage::kConfiguration;
  ChargedTriangulationCode code = ChargedTriangulationCode::kNone;
  std::optional<index_t> face;
  std::optional<std::uint64_t> attribute;
  // Only an actual completed inspection can supply this issue.
  std::optional<StorageIssue> storage_issue;
};
struct ChargedTriangulationMaterializationFacts {
  MaterializationStatus status = MaterializationStatus::kInvalidConfiguration;
  MaterializationDiagnostic diagnostic;
  MaterializationExecutionStats execution;
  // Snapshot at delegate return; not a retained/live or peak-byte promise.
  std::uint64_t new_requested_bytes_at_return = 0;
};
struct ChargedTriangulationInspectionFacts {
  MaterializationStatus status = MaterializationStatus::kInvalidConfiguration;
  MaterializationDiagnostic diagnostic;
  MaterializationExecutionStats execution;
  // Absent if no inspection owner was actually returned.
  std::optional<bool> storage_valid;
  std::uint64_t new_requested_bytes_at_return = 0;
};
struct ChargedTriangulationCoreFacts {
  TriangulationStatus status = TriangulationStatus::kBlocked;
  std::size_t workers_used = 0;
  ChargedTriangulationSerialReason serial_reason =
      ChargedTriangulationSerialReason::kNone;
  // Observed full-byte backend-request peak, including accounting seed
  // and overlapping/in-flight requests; not a sampled/model/global/RSS peak.
  std::uint64_t new_requested_peak_bytes = 0;
};

// Strong, immutable, copy-sharing owner. No public default constructor and no
// move-empty state. Borrowed references/spans require this visible handle alive
// and unassigned; copies retain storage until its final actual owner releases.
// Immutable storage may be read concurrently through distinct live copies.
// Copying/const reads of a handle may overlap; assignment or destruction of
// that same visible handle must not overlap any access/copy or use of its
// borrowed references without external synchronization. Another surviving copy
// permits independent reads of its own references while a different copy is
// released.
class ChargedTriangulation {
 public:
  ChargedTriangulation(const ChargedTriangulation&) noexcept;
  ChargedTriangulation& operator=(const ChargedTriangulation&) noexcept;
  ~ChargedTriangulation() noexcept;
  [[nodiscard]] const MaterializedMesh& Mesh() const noexcept;
  [[nodiscard]] const MeshInspection& Inspection() const noexcept;
  [[nodiscard]] const ChargedTriangulationCounts& Counts() const noexcept;
  [[nodiscard]] std::span<const index_t> FaceSources() const noexcept;
  [[nodiscard]] std::span<const index_t> CornerSources() const noexcept;
  [[nodiscard]] std::span<const index_t> FaceOutputOffsets() const noexcept;
  [[nodiscard]] ExecutionContext ShareExecutionContext() const noexcept;
  // Sum once over the three distinct retained allocation-operation owners:
  // bridge/maps, output materialization and output inspection. Existing input
  // and released capture/temporary owners are excluded. Copying adds no charge.
  [[nodiscard]] std::uint64_t NewRequestedBytes() const noexcept;

 private:
  friend struct charged_triangulation_detail::Access;
  explicit ChargedTriangulation(
      std::shared_ptr<const charged_triangulation_detail::State>
          state) noexcept;
  std::shared_ptr<const charged_triangulation_detail::State> state_;
};
struct ChargedTriangulationResult {
  ChargedTriangulationStatus status =
      ChargedTriangulationStatus::kInvalidConfiguration;
  ChargedTriangulationDiagnostic diagnostic;
  std::optional<ChargedTriangulation> triangulation;
  // Counts exist only after their whole physical-count scan/plan completes.
  std::optional<ChargedTriangulationCounts> counts;
  // Absent means not called, including source_capture for retained input.
  std::optional<ChargedTriangulationMaterializationFacts> source_capture;
  std::optional<ChargedTriangulationInspectionFacts> source_inspection;
  std::optional<ChargedTriangulationCoreFacts> core;
  std::optional<ChargedTriangulationMaterializationFacts>
      output_materialization;
  std::optional<ChargedTriangulationInspectionFacts> output_inspection;
  // Actual legacy position-interleaving jobs only; retained route is zero.
  // Constant-time descriptor view construction stays serial and is not a job.
  MaterializationExecutionStats adaptation_execution;
  // Actual bridge gather/map jobs only; delegate and core scopes stay separate.
  MaterializationExecutionStats transfer_execution;
};

// Legacy route: captures complete canonical storage on the supplied ledger.
// Source backing and the execution handle are borrowed for the entire call,
// including joined workers: keep both alive and do not mutate/assign/move them.
// Concurrent calls may read the same immutable source with distinct context
// copies; shared ledger contention follows ExecutionContext's existing
// contract.
[[nodiscard]] ChargedTriangulationResult TriangulateCharged(
    const Mesh& source, const TriangulationOptions& options,
    const ChargedTriangulationLimits& limits,
    const MeshMaterializationWorkPolicy& work,
    const ExecutionContext& execution, std::stop_token stop = {});
// Retained route: keeps a strong source handle and derives its actual ledger;
// no foreign context parameter or MaterializedMesh -> legacy Mesh conversion.
// The visible source handle must remain alive and unassigned throughout the
// call; concurrent const reads/distinct copies are allowed, while mutation of
// that same handle needs external synchronization. Its backing stays immutable.
[[nodiscard]] ChargedTriangulationResult TriangulateCharged(
    const MaterializedMesh& source, const TriangulationOptions& options,
    const ChargedTriangulationLimits& limits,
    const MeshMaterializationWorkPolicy& work, std::stop_token stop = {});

}  // namespace meshvale::geometry
#endif  // MESHVALE_GEOMETRY_CHARGED_TRIANGULATION_H_
