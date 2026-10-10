// SPDX-License-Identifier: Apache-2.0
#include <meshvale/geometry/execution.h>

#include <exception>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>

namespace {
using namespace meshvale::geometry;
void Require(bool condition) {
  if (!condition)
    throw std::runtime_error("installed execution contract failed");
}
void RetainedLedger() {
  ExecutionContext original({3, 1, 64});
  auto copied = original;
  ExecutionContext different({3, 1, 64});
  Require(SharesExecutionBudget(original, copied));
  Require(!SharesExecutionBudget(original, different));
  auto moved = std::move(original);
  Require(SharesExecutionBudget(copied, moved));
  Require(!SharesExecutionBudget(original, moved));
  Require(!SharesExecutionBudget(original, original));
  PayloadLease first, second;
  WorkerLease retained_workers;
  {
    ExecutionContext caller({3, 1, 64});
    first = std::move(TryReservePayload(caller, 40).lease);
    second = std::move(TryReservePayload(caller, 24).lease);
    retained_workers = TryReserveWorkers(caller, 3);
  }
  // The original caller context is gone; the leases still share its ledger.
  Require(first.TryResize(41) == ReservationStatus::kBudgetExceeded);
  second.Reset();
  Require(first.TryResize(64) == ReservationStatus::kAccepted);
  Require(retained_workers.Count() == 3);
  first.Reset();
  retained_workers.Reset();
}
void JoinedWorkers() {
  ExecutionContext context({3, 1});
  Mesh source;
  for (std::size_t i = 0; i < 256; ++i)
    source.positions.Append({static_cast<double>(i), 0, 0});
  auto imported = EditableMesh::ImportMesh(source);
  const auto snapshot = imported.mesh.Snapshot();
  auto slots = TryReserveWorkers(context, 3);
  Require(slots.Count() == 3);
  std::vector<BoundsResult> results(slots.Count());
  std::vector<std::exception_ptr> errors(slots.Count());
  // This owner unwinds before slots even if a later launch throws.
  std::vector<std::jthread> workers;
  workers.reserve(slots.Count());
  for (std::size_t i = 0; i < slots.Count(); ++i) {
    workers.emplace_back([&, i, copy = context] {
      try {
        results[i] = ComputeBounds(snapshot, copy);
      } catch (...) {
        errors[i] = std::current_exception();
      }
    });
  }
  for (auto& worker : workers) worker.join();
  Require(context.ActiveWorkers() == 3);
  for (std::size_t i = 0; i < results.size(); ++i) {
    if (errors[i]) std::rethrow_exception(errors[i]);
    Require(results[i].workers_used == 0 && results[i].bounds.has_value());
    Require(results[i].bounds->maximum[0] == 255);
  }
  slots.Reset();
  Require(context.ActiveWorkers() == 0);
}
}  // namespace
int main() {
  try {
    RetainedLedger();
    JoinedWorkers();
    std::cout
        << "Installed shared execution leases and joined workers passed\n";
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
