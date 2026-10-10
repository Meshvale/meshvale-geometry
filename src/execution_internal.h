// SPDX-License-Identifier: Apache-2.0
#ifndef MESHVALE_GEOMETRY_SRC_EXECUTION_INTERNAL_H_
#define MESHVALE_GEOMETRY_SRC_EXECUTION_INTERNAL_H_

#include <meshvale/geometry/editable_mesh.h>

#include <algorithm>
#include <cstddef>
#include <memory>
#include <mutex>
#include <utility>

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
  static std::shared_ptr<editing_detail::Execution> Get(
      const ExecutionContext& context) {
    if (!context.execution_)
      throw EditorError(EditorErrorCode::kInvalidObject,
                        "Execution context is moved from");
    return context.execution_;
  }
};

class WorkerReservation {
 public:
  WorkerReservation(std::shared_ptr<editing_detail::Execution> execution,
                    std::size_t desired)
      : execution_(std::move(execution)) {
    std::lock_guard lock(execution_->mutex);
    count_ = std::min(desired,
                      execution_->options.worker_budget - execution_->active);
    if (count_ < 2) count_ = 0;
    execution_->active += count_;
    execution_->peak = std::max(execution_->peak, execution_->active);
  }
  WorkerReservation(const WorkerReservation&) = delete;
  WorkerReservation& operator=(const WorkerReservation&) = delete;
  ~WorkerReservation() {
    std::lock_guard lock(execution_->mutex);
    execution_->active -= count_;
  }
  std::size_t Count() const { return count_; }

 private:
  std::shared_ptr<editing_detail::Execution> execution_;
  std::size_t count_ = 0;
};

class PayloadReservation {
 public:
  explicit PayloadReservation(
      std::shared_ptr<editing_detail::Execution> execution)
      : execution_(std::move(execution)) {}
  PayloadReservation(const PayloadReservation&) = delete;
  PayloadReservation& operator=(const PayloadReservation&) = delete;
  ~PayloadReservation() {
    std::lock_guard lock(execution_->mutex);
    execution_->active_payload -= count_;
  }
  bool Add(std::size_t bytes) {
    std::lock_guard lock(execution_->mutex);
    if (bytes > execution_->options.tracked_payload_budget_bytes -
                    execution_->active_payload)
      return false;
    count_ += bytes;
    execution_->active_payload += bytes;
    execution_->peak_payload =
        std::max(execution_->peak_payload, execution_->active_payload);
    return true;
  }
  std::size_t Count() const { return count_; }

 private:
  std::shared_ptr<editing_detail::Execution> execution_;
  std::size_t count_ = 0;
};
}  // namespace meshvale::geometry::execution_detail
#endif  // MESHVALE_GEOMETRY_SRC_EXECUTION_INTERNAL_H_
