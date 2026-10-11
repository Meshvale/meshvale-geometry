// SPDX-License-Identifier: Apache-2.0
#include "materialization_workers.h"

#include <meshvale/geometry/execution.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <stop_token>
#include <system_error>
#include <vector>

#include "materialization_allocation.h"

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <process.h>
#include <windows.h>

#include <cerrno>
#else
#include <pthread.h>
#endif

namespace meshvale::geometry::materialization_detail {
namespace {
constexpr std::size_t kMinimumParallelWorkers = 2;
#if defined(_WIN32)
unsigned __stdcall NativeEntry(void* argument) noexcept {
  auto& record = *static_cast<NativeWorkerRecord*>(argument);
  record.entry(record);
  return 0;
}
std::uintptr_t LaunchDefault(NativeWorkerRecord* record, void*) {
  auto handle = _beginthreadex(nullptr, 0, NativeEntry, record, 0, nullptr);
  if (!handle) throw std::system_error(errno, std::generic_category());
  return handle;
}
void JoinDefault(std::uintptr_t handle, void*) noexcept {
  const auto native = reinterpret_cast<HANDLE>(handle);
  if (WaitForSingleObject(native, INFINITE) != WAIT_OBJECT_0) std::terminate();
  if (!CloseHandle(native)) std::terminate();
}
#else
void* NativeEntry(void* argument) noexcept {
  auto& record = *static_cast<NativeWorkerRecord*>(argument);
  record.entry(record);
  return nullptr;
}
std::uintptr_t LaunchDefault(NativeWorkerRecord* record, void*) {
  static_assert(sizeof(pthread_t) <= sizeof(std::uintptr_t));
  pthread_t thread;
  const auto error = pthread_create(&thread, nullptr, NativeEntry, record);
  if (error != 0) throw std::system_error(error, std::generic_category());
  return static_cast<std::uintptr_t>(thread);
}
void JoinDefault(std::uintptr_t handle, void*) noexcept {
  if (pthread_join(static_cast<pthread_t>(handle), nullptr) != 0)
    std::terminate();
}
#endif
struct FailureSlot {
  std::exception_ptr exception;
  bool canceled = false;
};
struct Team {
  std::size_t count;
  std::size_t jobs;
  std::size_t workers;
  RangeOperation operation;
  void* state;
  std::stop_token stop;
  FailureSlot* failures;
};
void RunJob(Team& team, std::size_t ordinal) noexcept {
  auto& failure = team.failures[ordinal];
  if (team.stop.stop_requested()) {
    failure.canceled = true;
    return;
  }
  // Quotient/remainder partitioning avoids count * ordinal overflow.
  const auto quotient = team.count / team.jobs;
  const auto remainder = team.count % team.jobs;
  const auto begin = ordinal * quotient + std::min(ordinal, remainder);
  const auto end = begin + quotient + (ordinal < remainder ? 1 : 0);
  try {
    team.operation(team.state, begin, end);
  } catch (const AdmissionFailure& error) {
    if (error.Code() == AdmissionCode::kCanceled)
      failure.canceled = true;
    else
      failure.exception = std::current_exception();
  } catch (...) {
    failure.exception = std::current_exception();
  }
}
void WorkerEntry(NativeWorkerRecord& record) noexcept {
  auto& team = *static_cast<Team*>(record.state);
  for (auto job = record.ordinal; job < team.jobs; job += team.workers)
    RunJob(team, job);
}
void Observe(const MaterializationExecutionStats& stats,
             MaterializationExecutionStats* observer) noexcept {
  if (observer) *observer = stats;
}
}  // namespace
NativeWorkerBackend DefaultNativeWorkerBackend() noexcept {
  return {LaunchDefault, JoinDefault, nullptr};
}
void RunRange(const ExecutionContext& context,
              const AllocationContext& allocation,
              const MeshMaterializationWorkPolicy& work, std::size_t count,
              std::size_t scalar_bytes, RangeOperation operation, void* state,
              std::stop_token stop, MaterializationExecutionStats& stats,
              const NativeWorkerBackend& backend,
              MaterializationExecutionStats* observer) {
  allocation.Checkpoint();
  if (count == 0) return;
  const auto bytes = CheckedProduct(count, scalar_bytes);
  const auto target_scalars =
      std::max<std::size_t>(1, work.partition_bytes / scalar_bytes);
  auto jobs = count / target_scalars + (count % target_scalars != 0 ? 1 : 0);
  jobs = std::min<std::size_t>(jobs, work.max_jobs);
  auto workers =
      bytes >= work.parallel_min_bytes && jobs >= kMinimumParallelWorkers
          ? TryReserveWorkers(context, jobs)
          : WorkerLease();
  if (workers.Count() == 0) {
    ++stats.planned_jobs;
    ++stats.caller_jobs;
    Observe(stats, observer);
    operation(state, 0, count);
    allocation.Checkpoint();
    return;
  }
  std::vector<FailureSlot, Allocator<FailureSlot>> failures{
      Allocator<FailureSlot>(allocation)};
  std::vector<NativeWorkerRecord, Allocator<NativeWorkerRecord>> records{
      Allocator<NativeWorkerRecord>(allocation)};
  std::vector<std::uintptr_t, Allocator<std::uintptr_t>> handles{
      Allocator<std::uintptr_t>(allocation)};
  failures.resize(jobs);
  records.resize(workers.Count());
  handles.reserve(workers.Count());
  Team team{count, jobs, workers.Count(), operation,
            state, stop, failures.data()};
  stats.planned_jobs += jobs;
  try {
    for (std::size_t i = 0; i < workers.Count(); ++i) {
      records[i] = {WorkerEntry, &team, i};
      handles.push_back(backend.launch(&records[i], backend.state));
      ++stats.launched_workers;
      Observe(stats, observer);
    }
  } catch (...) {
    for (const auto handle : handles) {
      backend.join(handle, backend.state);
      ++stats.joined_workers;
      Observe(stats, observer);
    }
    throw;
  }
  for (const auto handle : handles) {
    backend.join(handle, backend.state);
    ++stats.joined_workers;
    Observe(stats, observer);
  }
  // Slot order preserves actual exception priority over cooperative
  // cancellation.
  for (const auto& failure : failures)
    if (failure.exception) std::rethrow_exception(failure.exception);
  for (const auto& failure : failures)
    if (failure.canceled) throw AdmissionFailure(AdmissionCode::kCanceled);
  allocation.Checkpoint();
}
}  // namespace meshvale::geometry::materialization_detail
