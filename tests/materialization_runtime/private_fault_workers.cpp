// SPDX-License-Identifier: Apache-2.0
// Private-seam proof consumer; no global or thread-local production hooks.
#include <meshvale/geometry/editable_mesh.h>
#include <meshvale/geometry/execution.h>
#include <meshvale/geometry/materialization.h>
#include <meshvale/geometry/vector_buffer.h>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <exception>
#include <iostream>
#include <mutex>
#include <new>
#include <optional>
#include <span>
#include <stdexcept>
#include <stop_token>
#include <string_view>
#include <system_error>
#include <variant>
#include <vector>

#include "materialization_allocation.h"
#include "materialization_test_internal.h"
#include "materialization_workers.h"

namespace {
namespace geo = meshvale::geometry;
namespace detail = geo::materialization_detail;
constexpr std::size_t kBytesPerKiB = 1024;
constexpr std::size_t kBudgetBytes = 64 * kBytesPerKiB * kBytesPerKiB;
constexpr std::size_t kWorkerBudget = 2;
constexpr std::size_t kFailSecondLaunch = 2;
constexpr std::size_t kVertexCount = 4096;
constexpr std::size_t kFaceCount = 4096;
constexpr std::size_t kTriangleCorners = 3;
constexpr std::size_t kLongRowScalars = 65536;
constexpr std::size_t kMaximumRecordedBlocks = 4096;
constexpr std::size_t kPartitionBytes = 1024;
constexpr std::size_t kMaximumJobs = 64;
constexpr std::size_t kCancellationBlockScalars = 64;
constexpr std::size_t kSelectionStride = 2;
constexpr std::size_t kAttributeCount = 1;
constexpr std::size_t kMetadataCount = 1;
constexpr std::size_t kNoFault = 0;
constexpr std::string_view kMetadataKey = "proof-kind";
constexpr std::string_view kMetadataValue = "delegating-private-seam";
constexpr std::string_view kAttributeName = "long-row";
constexpr std::string_view kAttributeSemantic = "proof.scalar";

void Require(bool condition, const char* message) {
  if (!condition) throw std::runtime_error(message);
}
geo::ExecutionContext Context() {
  geo::ExecutionOptions options;
  options.worker_budget = kWorkerBudget;
  options.tracked_payload_budget_bytes = kBudgetBytes;
  return geo::ExecutionContext(options);
}
geo::MeshMaterializationWorkPolicy Work() {
  geo::MeshMaterializationWorkPolicy work;
  work.parallel_min_bytes = 1;
  work.partition_bytes = kPartitionBytes;
  work.cancellation_block_scalars = kCancellationBlockScalars;
  work.max_jobs = kMaximumJobs;
  return work;
}
struct Fixture {
  std::vector<double> positions;
  std::vector<geo::index_t> offsets;
  std::vector<geo::index_t> corners;
  std::vector<float> values;
  std::vector<geo::index_t> rows;
  std::vector<geo::index_t> kept;
  std::array<geo::MetadataEntryView, kMetadataCount> metadata;
  std::array<geo::AttributeMaterializationInput, kAttributeCount> attributes;
  Fixture()
      : positions(kVertexCount * geo::kVector3Components, 0.0),
        offsets(kFaceCount + 1),
        corners(kFaceCount * kTriangleCorners),
        values(kLongRowScalars, 1.0f),
        rows(kVertexCount + 1, kLongRowScalars) {
    rows.front() = 0;
    for (std::size_t vertex = 0; vertex < kVertexCount; ++vertex)
      positions[vertex * geo::kVector3Components] = static_cast<double>(vertex);
    for (std::size_t face = 0; face < kFaceCount; ++face) {
      offsets[face] = face * kTriangleCorners;
      for (std::size_t local = 0; local < kTriangleCorners; ++local)
        corners[face * kTriangleCorners + local] = local;
      if (face % kSelectionStride == 0) kept.push_back(face);
    }
    offsets.back() = corners.size();
    metadata.front() = {kMetadataKey, kMetadataValue};
    auto& attribute = attributes.front();
    attribute.domain = geo::AttributeDomain::vertex;
    attribute.name = kAttributeName;
    attribute.semantic = kAttributeSemantic;
    attribute.components = 1;
    attribute.values = std::span<const float>(values);
    attribute.offsets = std::span<const geo::index_t>(rows);
    attribute.metadata = metadata;
  }
  geo::MeshMaterializationInput Input() const {
    geo::MeshMaterializationInput input;
    input.positions = std::span<const double>(positions);
    input.topology = geo::PolygonTopologyInput{offsets, corners};
    input.attributes = attributes;
    return input;
  }
  geo::MeshMaterializationLimits Limits() const {
    geo::MeshMaterializationLimits limits;
    limits.max_vertices = kVertexCount;
    limits.max_faces = kFaceCount;
    limits.max_corners = corners.size();
    limits.max_face_offsets = offsets.size();
    limits.max_attributes = attributes.size();
    limits.max_attribute_scalars = values.size();
    limits.max_attribute_offsets = rows.size();
    limits.max_presence_bytes = 1;
    limits.max_metadata_entries = metadata.size();
    limits.max_descriptor_bytes = kBytesPerKiB;
    limits.max_new_requested_bytes = kBudgetBytes;
    return limits;
  }
};

// Only explicit structural requests use this seam. Canonical Eigen/PIMPL
// allocations still execute through their original, unmodified backends.
struct AllocationProbe {
  struct Block {
    void* pointer = nullptr;
    std::size_t bytes = 0, alignment = 0;
  };
  detail::AllocationBackend original = detail::DefaultAllocationBackend();
  const geo::ExecutionContext& context;
  std::mutex mutex;
  std::array<Block, kMaximumRecordedBlocks> blocks{};
  std::size_t attempts = 0, allocations = 0, frees = 0, live = 0;
  std::size_t fail_at = kNoFault;
  bool violated = false;
  std::stop_source* stop_after_allocate = nullptr;
  explicit AllocationProbe(const geo::ExecutionContext& ledger)
      : context(ledger) {}
  static void* Allocate(std::size_t bytes, std::size_t alignment, void* state) {
    auto& probe = *static_cast<AllocationProbe*>(state);
    std::lock_guard lock(probe.mutex);
    ++probe.attempts;
    if (probe.context.ActiveTrackedPayload() < bytes) probe.violated = true;
    if (probe.fail_at != kNoFault && probe.attempts == probe.fail_at)
      throw std::bad_alloc();
    auto free_slot = probe.blocks.end();
    for (auto it = probe.blocks.begin(); it != probe.blocks.end(); ++it)
      if (!it->pointer) {
        free_slot = it;
        break;
      }
    Require(free_slot != probe.blocks.end(),
            "Structural observer capacity exhausted");
    void* block =
        probe.original.allocate(bytes, alignment, probe.original.state);
    Require(block != nullptr, "Default allocation backend returned null");
    *free_slot = {block, bytes, alignment};
    ++probe.allocations;
    ++probe.live;
    if (probe.stop_after_allocate) probe.stop_after_allocate->request_stop();
    return block;
  }
  static void Deallocate(void* block, std::size_t bytes, std::size_t alignment,
                         void* state) noexcept {
    auto& probe = *static_cast<AllocationProbe*>(state);
    std::lock_guard lock(probe.mutex);
    auto found = probe.blocks.end();
    for (auto it = probe.blocks.begin(); it != probe.blocks.end(); ++it)
      if (it->pointer == block) {
        found = it;
        break;
      }
    if (found == probe.blocks.end() || found->bytes != bytes ||
        found->alignment != alignment ||
        probe.context.ActiveTrackedPayload() < bytes)
      probe.violated = true;
    probe.original.deallocate(block, bytes, alignment, probe.original.state);
    if (found != probe.blocks.end()) {
      *found = {};
      --probe.live;
    }
    ++probe.frees;
  }
  detail::AllocationBackend Backend() { return {Allocate, Deallocate, this}; }
  void Clean() {
    Require(!violated && live == 0 && allocations == frees,
            "Structural request ownership/admission/free contract violated");
  }
};
struct LaunchFailure final : std::system_error {
  LaunchFailure()
      : std::system_error(
            std::make_error_code(std::errc::resource_unavailable_try_again)) {}
};
struct WorkerProbe {
  struct WrappedRecord {
    detail::NativeWorkerRecord native;
    detail::NativeWorkerRecord* delegate = nullptr;
    WorkerProbe* probe = nullptr;
  };
  detail::NativeWorkerBackend original = detail::DefaultNativeWorkerBackend();
  std::array<WrappedRecord, kWorkerBudget> records{};
  std::size_t attempts = 0, launched = 0, joined = 0;
  std::size_t fail_at = kNoFault;
  std::atomic<std::size_t> entered = 0;
  std::stop_source* cancel_on_entry = nullptr;
  bool cancel_before_failed_launch = false;
  static void Entry(detail::NativeWorkerRecord& native) noexcept {
    auto& record = *static_cast<WrappedRecord*>(native.state);
    record.probe->entered.fetch_add(1, std::memory_order_relaxed);
    if (record.probe->cancel_on_entry)
      record.probe->cancel_on_entry->request_stop();
    record.delegate->entry(*record.delegate);
  }
  static std::uintptr_t Launch(detail::NativeWorkerRecord* source,
                               void* state) {
    auto& probe = *static_cast<WorkerProbe*>(state);
    ++probe.attempts;
    if (probe.fail_at != kNoFault && probe.attempts == probe.fail_at) {
      if (probe.cancel_before_failed_launch && probe.cancel_on_entry)
        probe.cancel_on_entry->request_stop();
      throw LaunchFailure();
    }
    Require(probe.launched - probe.joined < kWorkerBudget,
            "Native backend exceeded shared worker capacity");
    auto& record = probe.records[probe.launched % kWorkerBudget];
    record.delegate = source;
    record.probe = &probe;
    record.native = {Entry, &record, source->ordinal};
    const auto handle =
        probe.original.launch(&record.native, probe.original.state);
    ++probe.launched;
    return handle;
  }
  static void Join(std::uintptr_t handle, void* state) noexcept {
    auto& probe = *static_cast<WorkerProbe*>(state);
    probe.original.join(handle, probe.original.state);
    ++probe.joined;
  }
  detail::NativeWorkerBackend Backend() { return {Launch, Join, this}; }
  void Match(const geo::MaterializationExecutionStats& observer) const {
    Require(
        launched == joined && observer.launched_workers == launched &&
            observer.joined_workers == joined && entered.load() == launched,
        "Actual native launches/entries/joins and surviving observer differ");
  }
};

enum class Operation { kCapture, kInspect, kSelect };
struct Outcome {
  geo::MaterializationStatus status;
  bool owner;
};
Outcome Invoke(const Fixture& fixture, Operation operation,
               const std::optional<geo::MaterializedMesh>& source,
               const geo::ExecutionContext& context, std::stop_token stop,
               const detail::TestHooks& hooks) {
  if (operation == Operation::kCapture) {
    auto result = detail::TestMaterializeMesh(fixture.Input(), fixture.Limits(),
                                              Work(), context, stop, hooks);
    return {result.status, result.mesh.has_value()};
  }
  Require(source.has_value(), "Subsequent operation requires a source");
  if (operation == Operation::kInspect) {
    geo::MeshInspectionLimits limits;
    limits.max_issues = kVertexCount;
    limits.max_new_requested_bytes = kBudgetBytes;
    auto result = detail::TestInspectMaterializedMesh(*source, limits, Work(),
                                                      stop, hooks);
    return {result.status, result.inspection.has_value()};
  }
  auto result = detail::TestSelectMeshFaces(
      *source, fixture.kept, fixture.Limits(), Work(), stop, hooks);
  return {result.status, result.selection.has_value()};
}
struct Trial {
  const Fixture& fixture;
  geo::ExecutionContext context = Context();
  std::optional<geo::MaterializedMesh> source;
  std::uint64_t baseline = 0;
  Trial(const Fixture& input_fixture, Operation operation)
      : fixture(input_fixture) {
    if (operation != Operation::kCapture) {
      auto result = geo::MaterializeMesh(fixture.Input(), fixture.Limits(),
                                         Work(), context);
      Require(
          result.status == geo::MaterializationStatus::kSuccess && result.mesh,
          "Source capture must succeed on the qualified profile");
      source.emplace(*result.mesh);
      result.mesh.reset();
      baseline = context.ActiveTrackedPayload();
    }
  }
  void Restored() const {
    Require(context.ActiveWorkers() == 0 &&
                context.ActiveTrackedPayload() == baseline,
            "Exceptional/canceled operation did not restore shared "
            "slots/retained charges");
    if (source) {
      const auto view = source->View();
      Require(
          view.positions.size() == fixture.positions.size() &&
              std::memcmp(view.positions.data(), fixture.positions.data(),
                          view.positions.size_bytes()) == 0 &&
              view.face_offsets.size() == fixture.offsets.size() &&
              std::memcmp(view.face_offsets.data(), fixture.offsets.data(),
                          view.face_offsets.size_bytes()) == 0 &&
              view.corner_vertices.size() == fixture.corners.size() &&
              std::memcmp(view.corner_vertices.data(), fixture.corners.data(),
                          view.corner_vertices.size_bytes()) == 0,
          "Failure changed retained source topology/positions");
      Require(view.attributes.size() == kAttributeCount,
              "Failure changed source attributes");
      const auto values =
          std::get<std::span<const float>>(view.attributes.front().values);
      Require(values.size() == fixture.values.size() &&
                  std::memcmp(values.data(), fixture.values.data(),
                              values.size_bytes()) == 0,
              "Failure changed retained canonical attribute scalars");
    }
  }
};
std::size_t ObserveStructuralRequests(const Fixture& fixture,
                                      Operation operation) {
  Trial trial(fixture, operation);
  AllocationProbe allocation(trial.context);
  WorkerProbe workers;
  geo::MaterializationExecutionStats observer;
  detail::TestHooks hooks{allocation.Backend(), workers.Backend(), &observer};
  const auto outcome =
      Invoke(fixture, operation, trial.source, trial.context, {}, hooks);
  Require(
      outcome.status == geo::MaterializationStatus::kSuccess && outcome.owner,
      "Delegating baseline operation failed");
  workers.Match(observer);
  allocation.Clean();
  trial.Restored();
  Require(allocation.attempts > 0,
          "No explicit structural request was observed");
  return allocation.attempts;
}
void CheckEveryStructuralFault(const Fixture& fixture, Operation operation) {
  const auto total = ObserveStructuralRequests(fixture, operation);
  for (std::size_t ordinal = 1; ordinal <= total; ++ordinal) {
    Trial trial(fixture, operation);
    AllocationProbe allocation(trial.context);
    allocation.fail_at = ordinal;
    WorkerProbe workers;
    geo::MaterializationExecutionStats observer;
    detail::TestHooks hooks{allocation.Backend(), workers.Backend(), &observer};
    bool threw = false;
    try {
      (void)Invoke(fixture, operation, trial.source, trial.context, {}, hooks);
    } catch (const std::bad_alloc&) {
      threw = true;
    }
    Require(
        threw && allocation.attempts == ordinal,
        "Explicit structural allocation failure must propagate after cleanup");
    workers.Match(observer);
    allocation.Clean();
    trial.Restored();
  }
  std::cout << "operation=" << static_cast<int>(operation)
            << " explicit_request_fault_ordinals=" << total << '\n';
}
void CheckPartialLaunch(const Fixture& fixture, Operation operation,
                        bool simultaneous_stop) {
  Trial trial(fixture, operation);
  AllocationProbe allocation(trial.context);
  std::stop_source stop;
  WorkerProbe workers;
  workers.fail_at = kFailSecondLaunch;
  if (simultaneous_stop) {
    workers.cancel_on_entry = &stop;
    workers.cancel_before_failed_launch = true;
  }
  geo::MaterializationExecutionStats observer;
  detail::TestHooks hooks{allocation.Backend(), workers.Backend(), &observer};
  bool threw = false;
  try {
    (void)Invoke(fixture, operation, trial.source, trial.context,
                 stop.get_token(), hooks);
  } catch (const LaunchFailure&) {
    threw = true;
  }
  Require(
      threw && workers.attempts == kFailSecondLaunch && workers.launched == 1,
      "Second actual launch failure was not preserved over worker "
      "cancellation");
  workers.Match(observer);
  allocation.Clean();
  trial.Restored();
}
void CheckWorkerEntryCancellation(const Fixture& fixture, Operation operation) {
  Trial trial(fixture, operation);
  AllocationProbe allocation(trial.context);
  std::stop_source stop;
  WorkerProbe workers;
  workers.cancel_on_entry = &stop;
  geo::MaterializationExecutionStats observer;
  detail::TestHooks hooks{allocation.Backend(), workers.Backend(), &observer};
  const auto outcome = Invoke(fixture, operation, trial.source, trial.context,
                              stop.get_token(), hooks);
  Require(outcome.status == geo::MaterializationStatus::kCanceled &&
              !outcome.owner && workers.launched > 0,
          "Stop requested by an actual native worker must cancel without "
          "partial owner");
  workers.Match(observer);
  allocation.Clean();
  trial.Restored();
}
void CheckPostAllocationCancellation(const Fixture& fixture,
                                     Operation operation) {
  Trial trial(fixture, operation);
  AllocationProbe allocation(trial.context);
  std::stop_source stop;
  allocation.stop_after_allocate = &stop;
  WorkerProbe workers;
  geo::MaterializationExecutionStats observer;
  detail::TestHooks hooks{allocation.Backend(), workers.Backend(), &observer};
  const auto outcome = Invoke(fixture, operation, trial.source, trial.context,
                              stop.get_token(), hooks);
  Require(outcome.status == geo::MaterializationStatus::kCanceled &&
              !outcome.owner && allocation.allocations > 0,
          "Cancellation after actual structural allocation must release "
          "seed/control storage");
  workers.Match(observer);
  allocation.Clean();
  trial.Restored();
}
}  // namespace
int main() {
  try {
    Fixture fixture;
    constexpr std::array operations{Operation::kCapture, Operation::kInspect,
                                    Operation::kSelect};
    for (auto operation : operations) {
      CheckEveryStructuralFault(fixture, operation);
      CheckPartialLaunch(fixture, operation, false);
      CheckPartialLaunch(fixture, operation, true);
      CheckWorkerEntryCancellation(fixture, operation);
      CheckPostAllocationCancellation(fixture, operation);
    }
    std::cout << "Delegating private-seam accounting/cleanup consumer passed\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
