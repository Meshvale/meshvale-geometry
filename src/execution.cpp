// SPDX-License-Identifier: Apache-2.0
#include <meshvale/geometry/execution.h>

#include <algorithm>
#include <limits>
#include <mutex>
#include <utility>

#include "execution_internal.h"

namespace meshvale::geometry {
namespace {
ReservationStatus Resize(editing_detail::Execution& execution,
                         std::size_t& charge, std::size_t total,
                         std::stop_token stop, bool initial = false) {
  const bool admission = initial || total > charge;
  if (admission && stop.stop_requested()) return ReservationStatus::kCanceled;
  std::lock_guard lock(execution.mutex);
  if (admission && stop.stop_requested()) return ReservationStatus::kCanceled;
  if (total > charge) {
    const auto growth = total - charge;
    if (growth > execution.options.tracked_payload_budget_bytes -
                     execution.active_payload)
      return ReservationStatus::kBudgetExceeded;
    execution.active_payload += growth;
    execution.peak_payload =
        std::max(execution.peak_payload, execution.active_payload);
  } else {
    execution.active_payload -= charge - total;
  }
  charge = total;
  return ReservationStatus::kAccepted;
}
}  // namespace

bool SharesExecutionBudget(const ExecutionContext& first,
                           const ExecutionContext& second) noexcept {
  return execution_detail::Access::Shares(first, second);
}

PayloadLease::PayloadLease() noexcept = default;
PayloadLease::PayloadLease(std::shared_ptr<editing_detail::Execution> execution)
    : execution_(std::move(execution)) {}
PayloadLease::PayloadLease(PayloadLease&& other) noexcept
    : execution_(std::move(other.execution_)),
      bytes_(std::exchange(other.bytes_, 0)) {}
PayloadLease& PayloadLease::operator=(PayloadLease&& other) noexcept {
  if (this != &other) {
    Reset();
    execution_ = std::move(other.execution_);
    bytes_ = std::exchange(other.bytes_, 0);
  }
  return *this;
}
PayloadLease::~PayloadLease() noexcept { Reset(); }
std::size_t PayloadLease::Bytes() const noexcept { return bytes_; }
ReservationStatus PayloadLease::TryResize(std::size_t total_bytes,
                                          std::stop_token stop) {
  if (!execution_) {
    if (total_bytes == 0) return ReservationStatus::kAccepted;
    throw EditorError(EditorErrorCode::kInvalidObject,
                      "Payload lease is detached");
  }
  return Resize(*execution_, bytes_, total_bytes, stop);
}
void PayloadLease::Reset() noexcept {
  if (!execution_) return;
  {
    std::lock_guard lock(execution_->mutex);
    execution_->active_payload -= bytes_;
    bytes_ = 0;
  }
  execution_.reset();
}
PayloadLeaseResult TryReservePayload(const ExecutionContext& context,
                                     std::size_t bytes, std::stop_token stop) {
  return execution_detail::Access::ReservePayload(
      execution_detail::Access::Get(context), bytes, stop);
}
WorkerLease::WorkerLease() noexcept = default;
WorkerLease::WorkerLease(std::shared_ptr<editing_detail::Execution> execution,
                         std::size_t count)
    : execution_(std::move(execution)), count_(count) {}
WorkerLease::WorkerLease(WorkerLease&& other) noexcept
    : execution_(std::move(other.execution_)),
      count_(std::exchange(other.count_, 0)) {}
WorkerLease& WorkerLease::operator=(WorkerLease&& other) noexcept {
  if (this != &other) {
    Reset();
    execution_ = std::move(other.execution_);
    count_ = std::exchange(other.count_, 0);
  }
  return *this;
}
WorkerLease::~WorkerLease() noexcept { Reset(); }
std::size_t WorkerLease::Count() const noexcept { return count_; }
void WorkerLease::Reset() noexcept {
  if (!execution_) return;
  {
    std::lock_guard lock(execution_->mutex);
    execution_->active -= count_;
    count_ = 0;
  }
  execution_.reset();
}
WorkerLease TryReserveWorkers(const ExecutionContext& context,
                              std::size_t desired) {
  return execution_detail::Access::ReserveWorkers(
      execution_detail::Access::Get(context), desired);
}

namespace execution_detail {
bool Access::Shares(const ExecutionContext& first,
                    const ExecutionContext& second) noexcept {
  return first.execution_ && second.execution_ &&
         first.execution_ == second.execution_;
}
std::shared_ptr<editing_detail::Execution> Access::Get(
    const ExecutionContext& context) {
  if (!context.execution_)
    throw EditorError(EditorErrorCode::kInvalidObject,
                      "Execution context is moved from");
  return context.execution_;
}
PayloadLease Access::BindPayload(
    std::shared_ptr<editing_detail::Execution> execution) {
  return PayloadLease(std::move(execution));
}
PayloadLeaseResult Access::ReservePayload(
    std::shared_ptr<editing_detail::Execution> execution, std::size_t bytes,
    std::stop_token stop) {
  std::size_t charge = 0;
  const auto status = Resize(*execution, charge, bytes, stop, true);
  if (status != ReservationStatus::kAccepted) return {status, {}};
  PayloadLease lease(std::move(execution));
  lease.bytes_ = charge;
  return {status, std::move(lease)};
}
WorkerLease Access::ReserveWorkers(
    std::shared_ptr<editing_detail::Execution> execution, std::size_t desired) {
  std::size_t count;
  {
    std::lock_guard lock(execution->mutex);
    count =
        std::min(desired, execution->options.worker_budget - execution->active);
    if (count < 2) count = 0;
    execution->active += count;
    execution->peak = std::max(execution->peak, execution->active);
  }
  return WorkerLease(std::move(execution), count);
}
WorkerReservation::WorkerReservation(
    std::shared_ptr<editing_detail::Execution> execution, std::size_t desired)
    : lease_(Access::ReserveWorkers(std::move(execution), desired)) {}
WorkerReservation::~WorkerReservation() = default;
std::size_t WorkerReservation::Count() const { return lease_.Count(); }
PayloadReservation::PayloadReservation(
    std::shared_ptr<editing_detail::Execution> execution)
    : lease_(Access::BindPayload(std::move(execution))) {}
PayloadReservation::~PayloadReservation() = default;
bool PayloadReservation::Add(std::size_t bytes) {
  if (bytes > std::numeric_limits<std::size_t>::max() - lease_.Bytes())
    return false;
  return lease_.TryResize(lease_.Bytes() + bytes) ==
         ReservationStatus::kAccepted;
}
std::size_t PayloadReservation::Count() const { return lease_.Bytes(); }
}  // namespace execution_detail
}  // namespace meshvale::geometry
