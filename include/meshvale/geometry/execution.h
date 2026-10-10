// SPDX-License-Identifier: Apache-2.0
#ifndef MESHVALE_GEOMETRY_EXECUTION_H_
#define MESHVALE_GEOMETRY_EXECUTION_H_

#include <meshvale/geometry/editable_mesh.h>

#include <cstddef>
#include <memory>
#include <stop_token>

namespace meshvale::geometry {

enum class ReservationStatus { kAccepted, kBudgetExceeded, kCanceled };

// True only for two valid contexts sharing the same worker/payload ledger.
// Either moved-from context yields false. No allocation, lock or raw identity
// exposure; moving the same context concurrently still needs synchronization.
[[nodiscard]] bool SharesExecutionBudget(
    const ExecutionContext& first, const ExecutionContext& second) noexcept;

// Owns a charge in the existing ledger, not an allocation. Separate leases and
// context copies may be used concurrently; the same lease needs external
// synchronization. Successful zero-byte leases retain their ledger. Default,
// moved-from and reset leases are detached.
class PayloadLease {
 public:
  PayloadLease() noexcept;
  PayloadLease(PayloadLease&&) noexcept;
  PayloadLease& operator=(PayloadLease&&) noexcept;
  ~PayloadLease() noexcept;
  PayloadLease(const PayloadLease&) = delete;
  PayloadLease& operator=(const PayloadLease&) = delete;
  [[nodiscard]] std::size_t Bytes() const noexcept;
  // Absolute charge; failed growth preserves its old charge. Shrink/equality
  // ignore cancellation. Detached zero is a no-op; positive growth throws
  // kInvalidObject. Release bytes only after their storage is freed.
  [[nodiscard]] ReservationStatus TryResize(std::size_t total_bytes,
                                            std::stop_token stop = {});
  void Reset() noexcept;

 private:
  friend struct execution_detail::Access;
  explicit PayloadLease(std::shared_ptr<editing_detail::Execution> execution);
  std::shared_ptr<editing_detail::Execution> execution_;
  std::size_t bytes_ = 0;
};
struct PayloadLeaseResult {
  ReservationStatus status;
  PayloadLease lease;
};
// Initial admission, including zero bytes, observes cancellation. Moved-from
// contexts throw kInvalidObject; expected rejection returns a detached lease.
// Normal admission/resize/move/release allocate nothing. Mutex failures may
// propagate; noexcept cleanup requires a valid ledger mutex.
[[nodiscard]] PayloadLeaseResult TryReservePayload(
    const ExecutionContext& context, std::size_t bytes,
    std::stop_token stop = {});

// Slots only: no thread launch/join. Hold the lease until every worker joins,
// including cancellation and partial-launch failures. The strong ledger owner
// outlives the caller context. The same lease needs external synchronization.
class WorkerLease {
 public:
  WorkerLease() noexcept;
  WorkerLease(WorkerLease&&) noexcept;
  WorkerLease& operator=(WorkerLease&&) noexcept;
  ~WorkerLease() noexcept;
  WorkerLease(const WorkerLease&) = delete;
  WorkerLease& operator=(const WorkerLease&) = delete;
  [[nodiscard]] std::size_t Count() const noexcept;
  void Reset() noexcept;

 private:
  friend struct execution_detail::Access;
  WorkerLease(std::shared_ptr<editing_detail::Execution> execution,
              std::size_t count);
  std::shared_ptr<editing_detail::Execution> execution_;
  std::size_t count_ = 0;
};
// Nonwaiting min(desired, available); fewer than two slots returns zero for
// caller fallback. No thread/pool construction or normal admission allocation.
[[nodiscard]] WorkerLease TryReserveWorkers(const ExecutionContext& context,
                                            std::size_t desired);

}  // namespace meshvale::geometry
#endif  // MESHVALE_GEOMETRY_EXECUTION_H_
