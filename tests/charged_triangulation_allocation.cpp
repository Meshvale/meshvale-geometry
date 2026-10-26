// SPDX-License-Identifier: Apache-2.0
#include <meshvale/geometry/charged_triangulation.h>
#include <meshvale/geometry/editable_mesh.h>
#include <meshvale/geometry/execution.h>
#include <meshvale/geometry/materialization.h>
#include <meshvale/geometry/triangulation.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <initializer_list>
#include <iostream>
#include <limits>
#include <new>
#include <optional>
#include <stop_token>
#include <system_error>

#include "allocation_failure.h"
#include "charged_triangulation_fixture.h"
#include "charged_triangulation_internal.h"
#include "materialization_allocation.h"
#include "materialization_storage.h"
#include "materialization_workers.h"
#include "triangulation_numeric.h"

namespace {
namespace g = meshvale::geometry;
namespace f = meshvale::geometry::charged_test;
namespace md = meshvale::geometry::materialization_detail;
namespace td = meshvale::geometry::triangulation_detail;
using Access = meshvale::geometry::charged_triangulation_detail::Access;
using Status = g::ChargedTriangulationStatus;
using Code = g::ChargedTriangulationCode;
constexpr std::size_t kPressureLedgerBytes = 4 * f::kMiB;
constexpr std::size_t kRefusalHeadroomBytes = 1;
struct Audit {
  std::atomic<std::size_t> calls{0}, live{0}, peak{0}, frees{0};
  std::size_t fail_on = 0;
  static void* Allocate(std::size_t bytes, std::size_t alignment, void* state) {
    auto& audit = *static_cast<Audit*>(state);
    const auto call = audit.calls.fetch_add(1) + 1;
    if (call == audit.fail_on) throw std::bad_alloc();
    auto* block =
        md::DefaultAllocationBackend().allocate(bytes, alignment, nullptr);
    const auto observed_live = audit.live.fetch_add(bytes) + bytes;
    auto old_peak = audit.peak.load();
    while (old_peak < observed_live &&
           !audit.peak.compare_exchange_weak(old_peak, observed_live)) {
    }
    return block;
  }
  static void Free(void* block, std::size_t bytes, std::size_t alignment,
                   void* state) noexcept {
    auto& audit = *static_cast<Audit*>(state);
    md::DefaultAllocationBackend().deallocate(block, bytes, alignment, nullptr);
    audit.live.fetch_sub(bytes);
    audit.frees.fetch_add(1);
  }
  md::AllocationBackend Backend() { return {Allocate, Free, this}; }
};
struct OrdinaryTrap {
  OrdinaryTrap() {
#if defined(MESHVALE_CHARGED_ORDINARY_ALLOCATION_PROBE)
    g::allocation_test::reject_allocations = true;
#endif
  }
  ~OrdinaryTrap() {
#if defined(MESHVALE_CHARGED_ORDINARY_ALLOCATION_PROBE)
    g::allocation_test::reject_allocations = false;
#endif
  }
};
void NestedKernelAndRollback() {
  f::Fixture fixture;
  g::ExecutionContext context({1, 0, f::kLedgerBytes});
  Audit audit;
  {
    auto allocation = md::AllocationContext(context, {}, audit.Backend())
                          .WithRequestLimit(f::kRequestLimit);
    const auto baseline = context.ActiveTrackedPayload();
    td::Context token{};
    {
      OrdinaryTrap trap;
      // Real five-corner arithmetic reaches zero/shift/area/point/projected/ear
      // temporaries. Ordinary default limb or nested-vector allocation fails.
      const auto triangles =
          td::TriangulateFace(fixture.legacy, fixture.legacy.face_offsets[2],
                              fixture.legacy.face_offsets[3], token,
                              md::Allocator<std::uint32_t>(allocation));
      f::Require(triangles.size() == 3,
                 "real nested charged kernel under ordinary-allocation trap");
    }
    f::Require(context.ActiveTrackedPayload() == baseline,
               "kernel temporary requests freed");
    audit.fail_on = audit.calls.load() + 3;
    bool escaped = false;
    try {
      OrdinaryTrap trap;
      (void)td::TriangulateFace(fixture.legacy, fixture.legacy.face_offsets[2],
                                fixture.legacy.face_offsets[3], token,
                                md::Allocator<std::uint32_t>(allocation));
    } catch (const std::bad_alloc&) {
      escaped = true;
    }
    f::Require(escaped && context.ActiveTrackedPayload() == baseline,
               "actual backend bad_alloc escapes with no fake result and rolls "
               "back requests");
  }
  f::Require(audit.calls > 1 && audit.peak > 0 && audit.live == 0 &&
                 context.ActiveTrackedPayload() == 0,
             "accounting seed/last backend release");
}
void ObservedCoreBoundary() {
  f::Fixture fixture;
  g::ExecutionContext context({f::kWorkers, 0, f::kLedgerBytes});
  const auto source = fixture.Capture(context);
  const auto baseline = context.ActiveTrackedPayload();
  g::TriangulationOptions options;
  options.minimum_parallel_faces = std::numeric_limits<std::size_t>::max();
  std::uint64_t peak;
  {
    Audit audit;
    auto result =
        Access::Run(source, options, f::Limits(), f::Work(), {},
                    audit.Backend(), md::DefaultNativeWorkerBackend());
    f::Require(result.status == Status::kAccepted && result.core &&
                   result.triangulation,
               "actual serial core observation");
    peak = result.core->new_requested_peak_bytes;
    f::Require(
        peak > 0 && peak <= f::kRequestLimit && audit.peak >= peak,
        "observed full-request core peak includes actual accounting seed");
    result.triangulation.reset();
    f::Require(audit.live == 0, "retained bridge backend lifetime");
  }
  f::Require(context.ActiveTrackedPayload() == baseline,
             "first observation no leaked core/copy");
  for (const auto cap : {peak, peak - 1, std::uint64_t{1}}) {
    Audit audit;
    auto limits = f::Limits();
    limits.max_core_new_requested_bytes = cap;
    auto result =
        Access::Run(source, options, limits, f::Work(), {}, audit.Backend(),
                    md::DefaultNativeWorkerBackend());
    if (cap == peak) {
      f::Require(result.status == Status::kAccepted && result.core &&
                     result.core->new_requested_peak_bytes == peak,
                 "actual observed exact core cap admits");
    } else {
      f::Require(result.status == Status::kBudgetExceeded &&
                     result.diagnostic.code == Code::kCoreRequestBudget &&
                     !result.triangulation && result.core &&
                     result.core->new_requested_peak_bytes <= cap &&
                     !result.output_materialization,
                 "actual one-short/bootstrap refusal, phase-only budget cause");
      if (cap == 1)
        f::Require(result.core->new_requested_peak_bytes == 0,
                   "refused bootstrap was never presented to core backend");
    }
    result.triangulation.reset();
    f::Require(audit.live == 0 && context.ActiveTrackedPayload() == baseline,
               "cap refusal and success fully released");
  }
}
void ActualLedgerPressure() {
  f::Fixture fixture;
  g::ExecutionContext context({f::kWorkers, 0, kPressureLedgerBytes});
  const auto source = fixture.Capture(context);
  const auto baseline = context.ActiveTrackedPayload();
  const auto prefix = md::AllocationRequestBytes(0, alignof(std::byte));
  f::Require(baseline + prefix + kRefusalHeadroomBytes < kPressureLedgerBytes,
             "pressure envelope");
  const auto count =
      kPressureLedgerBytes - baseline - prefix - kRefusalHeadroomBytes;
  {
    md::FlatArray<std::byte> pressure(md::AllocationContext(context, {}),
                                      count);
    pressure.Values().front() = std::byte{1};
    pressure.Values().back() = std::byte{2};
    f::Require(context.ActiveTrackedPayload() ==
                   kPressureLedgerBytes - kRefusalHeadroomBytes,
               "actual allocated backing consumes computed shared headroom");
    const auto result =
        g::TriangulateCharged(source, {}, f::Limits(), f::Work());
    f::Require(result.status == Status::kBudgetExceeded &&
                   !result.triangulation && !result.source_inspection &&
                   !result.core,
               "real same-ledger pressure refusal");
  }
  f::Require(context.ActiveTrackedPayload() == baseline,
             "real pressure owner released");
  g::ExecutionContext foreign({f::kWorkers, 0, kPressureLedgerBytes});
  {
    md::FlatArray<std::byte> pressure(md::AllocationContext(foreign, {}),
                                      count);
    pressure.Values().front() = std::byte{1};
    pressure.Values().back() = std::byte{2};
    const auto result =
        g::TriangulateCharged(source, {}, f::Limits(), f::Work());
    f::Require(
        result.status == Status::kAccepted && result.triangulation &&
            g::SharesExecutionBudget(
                context, result.triangulation->ShareExecutionContext()) &&
            !g::SharesExecutionBudget(
                foreign, result.triangulation->ShareExecutionContext()),
        "released original ledger succeeds despite real foreign occupied "
        "owner");
    f::Check(source.View(), *result.triangulation);
  }
  f::Require(context.ActiveTrackedPayload() == baseline &&
                 foreign.ActiveTrackedPayload() == 0,
             "all pressure/output owners actually released");
}
struct WorkerAudit {
  std::size_t launches = 0, joins = 0, fail_on = 0;
  std::stop_source* cancel = nullptr;
  static std::uintptr_t Launch(md::NativeWorkerRecord* record, void* state) {
    auto& audit = *static_cast<WorkerAudit*>(state);
    if (audit.launches + 1 == audit.fail_on)
      throw std::system_error(
          std::make_error_code(std::errc::resource_unavailable_try_again));
    const auto handle =
        md::DefaultNativeWorkerBackend().launch(record, nullptr);
    ++audit.launches;
    if (audit.cancel) audit.cancel->request_stop();
    return handle;
  }
  static void Join(std::uintptr_t handle, void* state) noexcept {
    auto& audit = *static_cast<WorkerAudit*>(state);
    md::DefaultNativeWorkerBackend().join(handle, nullptr);
    ++audit.joins;
  }
  md::NativeWorkerBackend Backend() { return {Launch, Join, this}; }
};
void JoinAndCancel() {
  f::Fixture fixture;
  g::ExecutionContext context({f::kWorkers, 0, f::kLedgerBytes});
  const auto source = fixture.Capture(context);
  const auto baseline = context.ActiveTrackedPayload();
  g::TriangulationOptions options;
  options.minimum_parallel_faces = 0;
  {
    Audit audit;
    WorkerAudit workers;
    workers.fail_on = 2;
    bool escaped = false;
    try {
      (void)Access::Run(source, options, f::Limits(), f::Work(), {},
                        audit.Backend(), workers.Backend());
    } catch (const std::system_error&) {
      escaped = true;
    }
    f::Require(
        escaped && workers.launches == 1 && workers.joins == 1 &&
            audit.live == 0 && context.ActiveWorkers() == 0 &&
            context.ActiveTrackedPayload() == baseline,
        "real partial launch joins before exception/storage/lease cleanup");
  }
  {
    Audit audit;
    WorkerAudit workers;
    std::stop_source stop;
    workers.cancel = &stop;
    const auto result =
        Access::Run(source, options, f::Limits(), f::Work(), stop.get_token(),
                    audit.Backend(), workers.Backend());
    f::Require(
        result.status == Status::kCanceled && !result.triangulation &&
            result.core &&
            result.core->status == g::TriangulationStatus::kCanceled &&
            workers.launches > 0 && workers.joins == workers.launches &&
            result.core->workers_used == workers.launches &&
            !result.output_materialization && audit.live == 0 &&
            context.ActiveWorkers() == 0 &&
            context.ActiveTrackedPayload() == baseline,
        "actual interior stop preserves joined facts and no partial owner");
  }
}
struct TransferStop {
  static constexpr std::size_t kStopAfterCopiedScalars = 2;
  std::stop_source stop;
  std::atomic<std::size_t> copied{0};
  static void Observe(std::size_t scalars, void* state) noexcept {
    auto& progress = *static_cast<TransferStop*>(state);
    const auto actual = progress.copied.fetch_add(scalars) + scalars;
    if (actual >= kStopAfterCopiedScalars) progress.stop.request_stop();
  }
};
void TransferInteriorStop() {
  f::Fixture fixture;
  g::ExecutionContext context({f::kWorkers, 0, f::kLedgerBytes});
  const auto source = fixture.Capture(context);
  const auto baseline = context.ActiveTrackedPayload();
  g::TriangulationOptions options;
  options.minimum_parallel_faces = 0;
  auto work = f::Work();
  // Explicit test policy: actual scalar gather has multiple leaf jobs and
  // checkpoints between individual writes. Existing production defaults stay.
  work.partition_bytes = 1;
  work.cancellation_block_scalars = 1;
  Audit audit;
  WorkerAudit workers;
  TransferStop progress;
  const g::charged_triangulation_detail::TransferObserver observer{
      TransferStop::Observe, &progress};
  const auto result =
      Access::Run(source, options, f::Limits(), work, progress.stop.get_token(),
                  audit.Backend(), workers.Backend(), &observer);
  f::Require(progress.copied >= TransferStop::kStopAfterCopiedScalars &&
                 progress.stop.stop_requested(),
             "actual gathered scalar writes precede stop observation");
  f::Require(
      result.status == Status::kCanceled && !result.triangulation &&
          result.diagnostic.stage ==
              g::ChargedTriangulationStage::kAttributeTransfer &&
          result.diagnostic.code == Code::kStopRequested &&
          !result.source_capture && result.source_inspection &&
          result.source_inspection->status ==
              g::MaterializationStatus::kSuccess &&
          result.source_inspection->storage_valid == true && result.core &&
          result.core->status == g::TriangulationStatus::kAccepted &&
          result.counts && result.counts->output_faces == f::kOutputFaces &&
          result.counts->output_corners == f::kOutputCorners &&
          !result.output_materialization && !result.output_inspection,
      "transfer stop preserves completed source/core/count facts and no "
      "invented output");
  f::Require(
      result.transfer_execution.planned_jobs > 0 &&
          result.transfer_execution.launched_workers >= 2 &&
          result.transfer_execution.joined_workers ==
              result.transfer_execution.launched_workers &&
          workers.launches == workers.joins &&
          workers.launches == result.core->workers_used +
                                  result.transfer_execution.launched_workers &&
          audit.live == 0 && context.ActiveWorkers() == 0 &&
          context.ActiveTrackedPayload() == baseline,
      "real begun transfer joins and releases backend/storage/worker owners");
  std::cout << "Transfer stop after " << progress.copied.load()
            << " actual scalar writes; transfer workers "
            << result.transfer_execution.launched_workers << " launched / "
            << result.transfer_execution.joined_workers
            << " joined; core workers " << result.core->workers_used
            << "; backend bytes " << audit.live.load() << '\n';
}
}  // namespace
int main() {
  try {
    NestedKernelAndRollback();
    ObservedCoreBoundary();
    ActualLedgerPressure();
    JoinAndCancel();
    TransferInteriorStop();
    std::cout << "charged allocation/peak/real pressure/joins passed\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
