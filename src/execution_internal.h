// SPDX-License-Identifier: Apache-2.0
#ifndef MESHVALE_GEOMETRY_SRC_EXECUTION_INTERNAL_H_
#define MESHVALE_GEOMETRY_SRC_EXECUTION_INTERNAL_H_

#include <meshvale/geometry/execution.h>

#include <cstddef>
#include <memory>
#include <mutex>

namespace meshvale::geometry::editing_detail {
struct Execution {
  ExecutionOptions options;
  mutable std::mutex mutex;
  std::size_t active = 0, peak = 0;
  std::size_t active_payload = 0, peak_payload = 0;
};
}  // namespace meshvale::geometry::editing_detail

namespace meshvale::geometry::execution_detail {
struct Access {
  static bool Shares(const ExecutionContext& first,
                     const ExecutionContext& second) noexcept;
  static std::shared_ptr<editing_detail::Execution> Get(
      const ExecutionContext& context);
  static PayloadLease BindPayload(
      std::shared_ptr<editing_detail::Execution> execution);
  static PayloadLeaseResult ReservePayload(
      std::shared_ptr<editing_detail::Execution> execution, std::size_t bytes,
      std::stop_token stop);
  static WorkerLease ReserveWorkers(
      std::shared_ptr<editing_detail::Execution> execution,
      std::size_t desired);
};
// Existing algorithms retain their admission interface and exception ordering.
class WorkerReservation {
 public:
  WorkerReservation(std::shared_ptr<editing_detail::Execution> execution,
                    std::size_t desired);
  WorkerReservation(const WorkerReservation&) = delete;
  WorkerReservation& operator=(const WorkerReservation&) = delete;
  ~WorkerReservation();
  std::size_t Count() const;

 private:
  WorkerLease lease_;
};
class PayloadReservation {
 public:
  explicit PayloadReservation(
      std::shared_ptr<editing_detail::Execution> execution);
  PayloadReservation(const PayloadReservation&) = delete;
  PayloadReservation& operator=(const PayloadReservation&) = delete;
  ~PayloadReservation();
  bool Add(std::size_t bytes);
  std::size_t Count() const;

 private:
  PayloadLease lease_;
};
}  // namespace meshvale::geometry::execution_detail
#endif  // MESHVALE_GEOMETRY_SRC_EXECUTION_INTERNAL_H_
