// SPDX-License-Identifier: Apache-2.0
#include <meshvale/geometry/execution.h>
#include <meshvale/geometry/position_buffer.h>

#include <iostream>
#include <limits>
#include <new>
#include <stdexcept>
#include <utility>

#include "allocation_failure.h"

namespace {
using namespace meshvale::geometry;
using allocation_test::reject_allocations;
void Require(bool condition, const char* message) {
  if (!condition) throw std::runtime_error(message);
}
void ReplacementCases() {
  reject_allocations = true;
  for (const bool array : {false, true}) {
    try {
      void* pointer = array ? ::operator new[](1) : ::operator new(1);
      reject_allocations = false;
      if (array)
        ::operator delete[](pointer);
      else
        ::operator delete(pointer);
      throw std::runtime_error("throwing allocation replacement inactive");
    } catch (const std::bad_alloc&) {
    }
  }
  reject_allocations = false;
  ::operator delete(::operator new(1));
  ::operator delete(::operator new(1), std::size_t{1});
  ::operator delete[](::operator new[](1));
  ::operator delete[](::operator new[](1), std::size_t{1});
}
// Repeat retained-owner cleanup under fail-new as well as in the independently
// race-instrumented lease suite; each probe owns its allocation runtime.
void LifetimeAndAllocationCases() {
  PayloadLease first, second;
  WorkerLease workers;
  {
    ExecutionContext context({3, 1, 64});
    first = std::move(TryReservePayload(context, 40).lease);
    second = std::move(TryReservePayload(context, 24).lease);
    workers = TryReserveWorkers(context, 3);
  }
  Require(first.TryResize(41) == ReservationStatus::kBudgetExceeded,
          "context destruction lost shared charge");
  second.Reset();
  Require(first.TryResize(64) == ReservationStatus::kAccepted &&
              workers.Count() == 3,
          "lease did not retain owner");
  reject_allocations = true;
  first.Reset();
  workers.Reset();
  second.Reset();
  reject_allocations = false;
}
// The context must be constructed outside the no-allocation window.
void NoAllocationCases() {
  ExecutionContext context({4, 1, 64});
  auto copied = context;
  ExecutionContext separate({4, 1, 64});
  auto relocated_context = std::move(separate);
  reject_allocations = true;
  {
    Require(SharesExecutionBudget(context, copied) &&
                !SharesExecutionBudget(context, relocated_context) &&
                !SharesExecutionBudget(context, separate) &&
                !SharesExecutionBudget(separate, separate) &&
                !SharesExecutionBudget(separate, relocated_context),
            "nonallocating shared ledger identity");
    auto initial = TryReservePayload(context, 32);
    auto payload = std::move(initial.lease);
    auto other = TryReservePayload(context, 16);
    other.lease = std::move(payload);
    Require(other.lease.TryResize(64) == ReservationStatus::kAccepted,
            "no-allocation growth");
    Require(TryReservePayload(context, 1).status ==
                ReservationStatus::kBudgetExceeded,
            "no-allocation rejection");
    auto workers = TryReserveWorkers(context, 2);
    auto more = TryReserveWorkers(context, 2);
    workers = std::move(more);
    WorkerLease moved(std::move(workers));
    moved.Reset();
    other.lease.Reset();
    PayloadLease detached;
    detached.Reset();
    WorkerLease empty;
    empty.Reset();
  }
  reject_allocations = false;
  Require(context.ActiveTrackedPayload() == 0 && context.ActiveWorkers() == 0,
          "allocation-free cleanup leaked");
}
struct Header {
  PayloadLease lease;
};
Header* Allocate(const ExecutionContext& context, std::size_t payload) {
  if (payload > std::numeric_limits<std::size_t>::max() - sizeof(Header))
    throw std::length_error("client block size overflow");
  const auto total = sizeof(Header) + payload;
  auto reservation = TryReservePayload(context, total);
  if (reservation.status != ReservationStatus::kAccepted) return nullptr;
  // Actual allocation failure unwinds the still-owned reservation.
  auto* storage = ::operator new(total);
  return new (storage) Header{std::move(reservation.lease)};
}
void Free(Header* header) {
  auto reservation = std::move(header->lease);
  header->~Header();
  ::operator delete(header);
  // Destroy the local lease after the underlying storage has been freed.
}
void AllocationRollbackCases() {
  const auto total = 2 * sizeof(Header) + 60;
  ExecutionContext context({2, 1, total});
  auto* old = Allocate(context, 20);
  Require(old != nullptr, "old block allocation");
  const auto old_bytes = old->lease.Bytes();
  reject_allocations = true;
  try {
    (void)Allocate(context, 40);
    reject_allocations = false;
    throw std::runtime_error("injected new failure did not propagate");
  } catch (const std::bad_alloc&) {
    reject_allocations = false;
  }
  Require(context.ActiveTrackedPayload() == old_bytes,
          "actual allocation failure leaked charge");
  auto* replacement = Allocate(context, 40);
  Require(replacement && context.ActiveTrackedPayload() == total,
          "growth overlap not charged");
  Require(Allocate(context, 1) == nullptr, "full-cap admission allocated");
  Free(old);
  Require(context.ActiveTrackedPayload() == replacement->lease.Bytes(),
          "old block release");
  reject_allocations = true;
  Free(replacement);
  reject_allocations = false;
  Require(context.ActiveTrackedPayload() == 0 &&
              context.PeakTrackedPayload() == total,
          "block cleanup or peak");
}
void PositionRollbackCases() {
  PositionBuffer source{{1, 2, 3}, {4, 5, 6}};
  PositionBuffer destination{{7, 8, 9}};
  for (const int operation : {0, 1, 2}) {
    reject_allocations = true;
    bool failed = false;
    try {
      if (operation == 0) destination = source;
      if (operation == 1) destination.reserve(16);
      if (operation == 2) destination.Append({1, 2, 3});
    } catch (const std::bad_alloc&) {
      failed = true;
    }
    reject_allocations = false;
    Require(failed && destination.size() == 1 && destination.Get(0)[0] == 7 &&
                source.size() == 2 && source.Get(1)[2] == 6,
            "position allocation failure did not preserve source/destination");
  }
  reject_allocations = true;
  PositionBuffer empty;
  empty.clear();
  destination.Set(0, {7, 8, 9});
  destination = std::move(destination);
  auto moved = std::move(source);
  reject_allocations = false;
  Require(source.empty() && moved.size() == 2 && empty.empty(),
          "position empty/move paths allocated or lost rows");
}
}  // namespace
int main() {
  try {
    ReplacementCases();
    LifetimeAndAllocationCases();
    NoAllocationCases();
    AllocationRollbackCases();
    PositionRollbackCases();
    std::cout << "Execution allocation failure cases passed\n";
  } catch (const std::exception& error) {
    reject_allocations = false;
    std::cerr << error.what() << '\n';
    return 1;
  }
}
