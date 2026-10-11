// SPDX-License-Identifier: Apache-2.0
#include <meshvale/geometry/editable_mesh.h>
#include <meshvale/geometry/execution.h>
#include <meshvale/geometry/materialization.h>
#include <meshvale/geometry/scalar_buffer.h>

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <exception>
#include <iostream>
#include <limits>
#include <span>
#include <stdexcept>
#include <stop_token>
#include <string_view>
#include <variant>

#include "materialization_allocation.h"
#include "materialization_test_internal.h"
#include "materialization_workers.h"
#include "numeric_buffer_internal.h"

namespace {
using namespace meshvale::geometry;
namespace allocation = materialization_detail;
constexpr std::size_t kRequestBudget = 1024 * 1024;
constexpr std::size_t kWorkerBudget = 2;
constexpr std::size_t kMinimumParallelVertices = 1;
constexpr std::size_t kScalarCount = 7;
constexpr std::size_t kPositionRows = 3;
ExecutionOptions TestExecutionOptions() {
  return {.worker_budget = kWorkerBudget,
          .minimum_parallel_vertices = kMinimumParallelVertices,
          .tracked_payload_budget_bytes = kRequestBudget};
}
void Require(bool condition, const char* message) {
  if (!condition) throw std::runtime_error(message);
}
template <SupportedAttributeScalar T>
void ScalarCase() {
  ExecutionContext execution(TestExecutionOptions());
  {
    auto operation = allocation::AllocationContext(execution, {})
                         .WithRequestLimit(kRequestBudget);
    const auto seed = operation.OperationActiveBytes();
    const auto zero = numeric_detail::Access::ScalarPlan<T>(0);
    Require(zero.implementation_bytes == 0 && zero.numerical_bytes == 0,
            "zero scalar plan requested a block");
    const auto plan = numeric_detail::Access::ScalarPlan<T>(kScalarCount);
    Require(plan.qualified, "unqualified scalar test profile");
    const auto original_bytes =
        plan.implementation_bytes + plan.numerical_bytes;
    {
      auto lease = operation.ReserveOriginalRequest(original_bytes);
      auto values = numeric_detail::Access::AllocateScalars<T>(kScalarCount);
      Require(
          values.size() == kScalarCount && values.capacity() == kScalarCount,
          "uninitialized buffer extent");
      Require(execution.ActiveTrackedPayload() == seed + original_bytes,
              "original request not charged before fill");
      for (std::size_t i = 0; i < kScalarCount; ++i)
        values.data()[i] = static_cast<T>(i);
      Require(
          values.data()[kScalarCount - 1] == static_cast<T>(kScalarCount - 1),
          "private scalar destination not writable");
    }
    Require(execution.ActiveTrackedPayload() == seed,
            "original requests retained after numerical destruction");
  }
  Require(execution.ActiveTrackedPayload() == 0, "operation seed leaked");
}
void PositionCase() {
  ExecutionContext execution(TestExecutionOptions());
  {
    auto operation = allocation::AllocationContext(execution, {})
                         .WithRequestLimit(kRequestBudget);
    const auto plan = numeric_detail::Access::PositionPlan(kPositionRows);
    Require(plan.qualified, "unqualified position test profile");
    auto lease = operation.ReserveOriginalRequest(plan.implementation_bytes +
                                                  plan.numerical_bytes);
    auto positions = numeric_detail::Access::AllocatePositions(kPositionRows);
    auto scalars = numeric_detail::Access::PositionScalars(positions);
    constexpr std::uint64_t kNegativeZeroBits = UINT64_C(0x8000000000000000);
    constexpr std::uint64_t kSignalingNanBits = UINT64_C(0x7ff0000000000001);
    const auto negative_zero = std::bit_cast<double>(kNegativeZeroBits);
    const auto signaling_nan = std::bit_cast<double>(kSignalingNanBits);
    std::memcpy(&scalars[0], &negative_zero, sizeof(double));
    std::memcpy(&scalars[1], &signaling_nan, sizeof(double));
    for (std::size_t i = 2; i < scalars.size(); ++i) scalars[i] = 0;
    const auto view = numeric_detail::Access::PositionScalars(
        static_cast<const decltype(positions)&>(positions));
    Require(std::bit_cast<std::uint64_t>(view[0]) == kNegativeZeroBits &&
                std::bit_cast<std::uint64_t>(view[1]) == kSignalingNanBits,
            "raw position bits changed");
  }
  Require(execution.ActiveTrackedPayload() == 0, "position requests leaked");
}
void AdmissionCase() {
  ExecutionContext execution(TestExecutionOptions());
  const auto plan = numeric_detail::Access::ScalarPlan<double>(kScalarCount);
  const auto bytes = plan.implementation_bytes + plan.numerical_bytes;
  std::size_t seed = 0;
  {
    auto operation = allocation::AllocationContext(execution, {})
                         .WithRequestLimit(kRequestBudget);
    seed = operation.OperationActiveBytes();
  }
  {
    auto operation = allocation::AllocationContext(execution, {})
                         .WithRequestLimit(seed + bytes);
    auto lease = operation.ReserveOriginalRequest(bytes);
    Require(operation.OperationActiveBytes() == seed + bytes,
            "exact original request cap");
    bool rejected = false;
    try {
      (void)operation.ReserveOriginalRequest(1);
    } catch (const allocation::AdmissionFailure& failure) {
      rejected = failure.Code() == allocation::AdmissionCode::kBudgetExceeded;
    }
    Require(rejected && operation.OperationActiveBytes() == seed + bytes,
            "original request plus one mutated cap");
  }
  {
    std::stop_source stop;
    auto operation = allocation::AllocationContext(execution, stop.get_token())
                         .WithRequestLimit(kRequestBudget);
    const auto before = operation.OperationActiveBytes();
    stop.request_stop();
    bool canceled = false;
    try {
      (void)operation.ReserveOriginalRequest(bytes);
    } catch (const allocation::AdmissionFailure& failure) {
      canceled = failure.Code() == allocation::AdmissionCode::kCanceled;
    }
    Require(canceled && operation.OperationActiveBytes() == before,
            "canceled original request did not roll back");
  }
  Require(execution.ActiveTrackedPayload() == 0, "failed admission leaked");
}
constexpr std::uint64_t kFixtureCountLimit = 64;
constexpr std::uint64_t kFixtureDescriptorLimit = 4096;
constexpr std::uint64_t kFixtureParallelBytes = 64;
constexpr std::uint64_t kFixturePartitionBytes = 16;
constexpr std::uint64_t kFixtureCancelScalars = 8;
constexpr std::uint64_t kFixtureMaximumJobs = 16;
constexpr std::string_view kEmptyChannelName = "authored-empty";
constexpr std::string_view kDuplicateMetadataKey = "duplicate";
MeshMaterializationLimits CaptureLimits() {
  return {kFixtureCountLimit,      kFixtureCountLimit, kFixtureCountLimit,
          kFixtureCountLimit,      kFixtureCountLimit, kFixtureCountLimit,
          kFixtureCountLimit,      kFixtureCountLimit, kFixtureCountLimit,
          kFixtureDescriptorLimit, kRequestBudget};
}
MeshMaterializationWorkPolicy CaptureWork() {
  return {kFixtureParallelBytes, kFixturePartitionBytes, kFixtureCancelScalars,
          kFixtureMaximumJobs};
}
void ValidationCase() {
  ExecutionContext execution(TestExecutionOptions());
  std::stop_source stopped;
  stopped.request_stop();
  const auto config =
      MaterializeMesh({}, {}, CaptureWork(), execution, stopped.get_token());
  Require(config.status == MaterializationStatus::kInvalidConfiguration,
          "stop changed an earlier configuration defect");
  const auto transport = MaterializeMesh({}, CaptureLimits(), CaptureWork(),
                                         execution, stopped.get_token());
  Require(transport.status == MaterializationStatus::kInvalidTransport,
          "stop changed an earlier physical selector defect");
  constexpr std::size_t kIncompletePositionScalars = 2;
  const std::array<double, kIncompletePositionScalars> incomplete{};
  const MeshMaterializationInput incomplete_input{
      std::span<const double>(incomplete),
      TriangleTopologyInput{std::span<const std::uint32_t>()},
      {}};
  const auto extent =
      MaterializeMesh(incomplete_input, CaptureLimits(), CaptureWork(),
                      execution, stopped.get_token());
  Require(extent.status == MaterializationStatus::kInvalidTransport &&
              extent.diagnostic.code ==
                  MaterializationDiagnosticCode::kPositionExtent,
          "incomplete XYZ transport was silently changed");
  Require(execution.ActiveTrackedPayload() == 0,
          "validation allocated or retained a block");
}
void BackendExceptionCase() {
  ExecutionContext execution(TestExecutionOptions());
  allocation::TestHooks hooks;
  hooks.allocation.allocate = [](std::size_t, std::size_t, void*) -> void* {
    throw std::length_error("injected original runtime exception");
  };
  const MeshMaterializationInput input{
      std::span<const double>(),
      TriangleTopologyInput{std::span<const std::uint32_t>()},
      {}};
  bool propagated = false;
  try {
    (void)allocation::TestMaterializeMesh(input, CaptureLimits(), CaptureWork(),
                                          execution, {}, hooks);
  } catch (const std::length_error&) {
    propagated = true;
  }
  Require(propagated && execution.ActiveTrackedPayload() == 0,
          "runtime length error was converted to an extent diagnostic");
}
void AuthoredEmptyCase() {
  ExecutionContext execution(TestExecutionOptions());
  constexpr std::size_t kMetadataEntries = 2;
  const std::array<MetadataEntryView, kMetadataEntries> metadata{
      MetadataEntryView{kDuplicateMetadataKey, "first"},
      MetadataEntryView{kDuplicateMetadataKey, "second"}};
  AttributeMaterializationInput channel;
  channel.domain = AttributeDomain::face;
  channel.name = kEmptyChannelName;
  channel.components = 1;
  channel.values = std::span<const std::int8_t>();
  channel.offsets = std::span<const index_t>();
  channel.present = std::span<const std::uint8_t>();
  channel.metadata = metadata;
  const MeshMaterializationInput input{
      std::span<const float>(),
      TriangleTopologyInput{std::span<const std::uint32_t>()},
      std::span<const AttributeMaterializationInput>(&channel, 1)};
  {
    const auto captured =
        MaterializeMesh(input, CaptureLimits(), CaptureWork(), execution);
    Require(captured.mesh.has_value(), "typed empty raw input was rejected");
    const auto view = captured.mesh->View();
    Require(view.face_offsets.size() == 1 && view.face_offsets.front() == 0,
            "empty triangle topology did not derive its sole offset");
    const auto& retained = view.attributes.front();
    Require(std::holds_alternative<std::span<const std::int32_t>>(
                retained.values) &&
                retained.offsets.has_value() && retained.offsets->empty() &&
                retained.present.has_value() && retained.present->empty(),
            "authored empty arrays or signed canonical selector were lost");
    const auto inspected = InspectMaterializedMesh(
        *captured.mesh, {kFixtureCountLimit, kRequestBudget}, CaptureWork());
    Require(inspected.inspection.has_value(), "raw empty inspection failed");
    const auto issues = inspected.inspection->Issues();
    constexpr std::size_t kExpectedIssues = 2;
    Require(
        issues.size() == kExpectedIssues &&
            issues.front().code == StorageIssueCode::kAttributeOffsetsEmpty &&
            issues.back().code == StorageIssueCode::kMetadataDuplicateKey &&
            issues.back().element == 1,
        "raw empty or ordered duplicate metadata inspection changed");
    const auto selected =
        SelectMeshFaces(*captured.mesh, {}, CaptureLimits(), CaptureWork());
    Require(selected.status == MaterializationStatus::kInvalidSourceStorage &&
                selected.diagnostic.storage_issue.has_value() &&
                selected.diagnostic.storage_issue->code ==
                    StorageIssueCode::kAttributeOffsetsEmpty,
            "selection implicitly repaired raw empty rows");
  }
  Require(
      execution.ActiveTrackedPayload() == 0 && execution.ActiveWorkers() == 0,
      "raw owners left charges or worker slots behind");
}
struct StopAfterWorkers {
  struct Wrapped {
    allocation::NativeWorkerRecord entry;
    allocation::NativeWorkerRecord* original = nullptr;
    std::stop_source* stop = nullptr;
  };
  std::array<Wrapped, kWorkerBudget> records;
  allocation::NativeWorkerBackend original =
      allocation::DefaultNativeWorkerBackend();
  std::stop_source stop;
  std::size_t next = 0;
  static void Entry(allocation::NativeWorkerRecord& record) noexcept {
    auto& wrapped = *static_cast<Wrapped*>(record.state);
    wrapped.original->entry(*wrapped.original);
    if (wrapped.original->ordinal == 0) wrapped.stop->request_stop();
  }
  static std::uintptr_t Launch(allocation::NativeWorkerRecord* record,
                               void* argument) {
    auto& state = *static_cast<StopAfterWorkers*>(argument);
    auto& wrapped = state.records.at(state.next++);
    wrapped.original = record;
    wrapped.stop = &state.stop;
    wrapped.entry = {Entry, &wrapped, record->ordinal};
    return state.original.launch(&wrapped.entry, state.original.state);
  }
  static void Join(std::uintptr_t handle, void* argument) noexcept {
    auto& state = *static_cast<StopAfterWorkers*>(argument);
    state.original.join(handle, state.original.state);
  }
};
void SourceDefectPrecedenceCase() {
  ExecutionContext execution(TestExecutionOptions());
  constexpr std::size_t kTrianglePositionScalars = 9;
  constexpr std::size_t kTriangleCornerCount = 3;
  constexpr std::uint64_t kSignalingNanBits = UINT64_C(0x7ff0000000000001);
  constexpr std::uint64_t kPostEntryParallelThreshold = 1;
  std::array<double, kTrianglePositionScalars> positions{};
  const std::array<std::uint32_t, kTriangleCornerCount> corners{0, 1, 2};
  std::memcpy(positions.data(), &kSignalingNanBits, sizeof(kSignalingNanBits));
  const MeshMaterializationInput input{
      std::span<const double>(positions), TriangleTopologyInput{corners}, {}};
  auto captured =
      MaterializeMesh(input, CaptureLimits(), CaptureWork(), execution);
  Require(captured.mesh.has_value(), "defective source capture failed");
  auto work = CaptureWork();
  work.parallel_min_bytes = kPostEntryParallelThreshold;
  StopAfterWorkers stopping;
  allocation::TestHooks hooks;
  hooks.workers = {StopAfterWorkers::Launch, StopAfterWorkers::Join, &stopping};
  const auto selected =
      allocation::TestSelectMeshFaces(*captured.mesh, {}, CaptureLimits(), work,
                                      stopping.stop.get_token(), hooks);
  Require(
      selected.status == MaterializationStatus::kInvalidSourceStorage &&
          selected.diagnostic.storage_issue.has_value() &&
          selected.diagnostic.storage_issue->code ==
              StorageIssueCode::kPositionNonfinite &&
          selected.diagnostic.storage_issue->element == 0 &&
          selected.execution.launched_workers != 0 &&
          selected.execution.launched_workers ==
              selected.execution.joined_workers,
      "post-entry cancellation concealed an already observed source defect");
  positions.fill(0);
  auto valid =
      MaterializeMesh(input, CaptureLimits(), CaptureWork(), execution);
  Require(valid.mesh.has_value(), "valid control source capture failed");
  StopAfterWorkers control;
  hooks.workers = {StopAfterWorkers::Launch, StopAfterWorkers::Join, &control};
  const auto canceled = allocation::TestSelectMeshFaces(
      *valid.mesh, {}, CaptureLimits(), work, control.stop.get_token(), hooks);
  Require(canceled.status == MaterializationStatus::kCanceled &&
              !canceled.selection.has_value() &&
              canceled.execution.launched_workers ==
                  canceled.execution.joined_workers,
          "valid source cancellation acquired a source defect diagnostic");
  Require(execution.ActiveWorkers() == 0,
          "source-defect precedence left a worker slot retained");
}
}  // namespace
int main() {
  try {
    ScalarCase<float>();
    ScalarCase<double>();
    ScalarCase<std::int32_t>();
    ScalarCase<std::uint8_t>();
    ScalarCase<std::uint16_t>();
    ScalarCase<std::uint32_t>();
    ScalarCase<std::uint64_t>();
    PositionCase();
    AdmissionCase();
    ValidationCase();
    BackendExceptionCase();
    AuthoredEmptyCase();
    SourceDefectPrecedenceCase();
    std::cout << "Materialization request and typed capture checks passed\n";
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
