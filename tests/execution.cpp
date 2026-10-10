// SPDX-License-Identifier: Apache-2.0
#include <meshvale/geometry/execution.h>
#include <meshvale/geometry/triangulation.h>

#include <atomic>
#include <barrier>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

namespace {
using namespace meshvale::geometry;
static_assert(!std::is_copy_constructible_v<PayloadLease>);
static_assert(!std::is_copy_constructible_v<WorkerLease>);
static_assert(std::is_nothrow_move_constructible_v<PayloadLease>);
static_assert(std::is_nothrow_move_assignable_v<WorkerLease>);
void Require(bool condition, const char* message) {
  if (!condition) throw std::runtime_error(message);
}
template <class F>
void Invalid(F operation) {
  try {
    operation();
  } catch (const EditorError& error) {
    Require(error.Code() == EditorErrorCode::kInvalidObject,
            "wrong object error");
    return;
  }
  throw std::runtime_error("invalid object accepted");
}
void PayloadCases() {
  ExecutionContext context({4, 1, 64});
  auto copied = context;
  PayloadLease detached;
  Require(detached.Bytes() == 0 &&
              detached.TryResize(0) == ReservationStatus::kAccepted,
          "detached zero resize");
  Invalid([&] { (void)detached.TryResize(1); });
  auto zero = TryReservePayload(context, 0);
  Require(zero.status == ReservationStatus::kAccepted, "zero admission");
  Require(zero.lease.TryResize(64) == ReservationStatus::kAccepted,
          "exact cap");
  Require(copied.ActiveTrackedPayload() == 64, "copied context ledger");
  Require(TryReservePayload(context, 1).status ==
              ReservationStatus::kBudgetExceeded,
          "cap plus one admitted");
  Require(zero.lease.TryResize(65) == ReservationStatus::kBudgetExceeded &&
              zero.lease.Bytes() == 64 && context.ActiveTrackedPayload() == 64,
          "failed growth changed charge");
  std::stop_source stop;
  stop.request_stop();
  Require(TryReservePayload(context, 0, stop.get_token()).status ==
              ReservationStatus::kCanceled,
          "initial zero cancellation ignored");
  Require(TryReservePayload(context, 1, stop.get_token()).status ==
              ReservationStatus::kCanceled,
          "cancellation priority over full cap");
  Require(zero.lease.TryResize(65, stop.get_token()) ==
                  ReservationStatus::kCanceled &&
              zero.lease.Bytes() == 64,
          "canceled growth changed charge");
  Require(zero.lease.TryResize(32, stop.get_token()) ==
              ReservationStatus::kAccepted,
          "canceled shrink blocked");
  Require(zero.lease.TryResize(32, stop.get_token()) ==
              ReservationStatus::kAccepted,
          "canceled equality blocked");
  auto second = TryReservePayload(context, 16);
  second.lease = std::move(zero.lease);
  Require(context.ActiveTrackedPayload() == 32 && second.lease.Bytes() == 32 &&
              zero.lease.Bytes() == 0,
          "move assignment did not release old charge");
  Invalid([&] { (void)zero.lease.TryResize(1); });
  second.lease.Reset();
  second.lease.Reset();
  Invalid([&] { (void)second.lease.TryResize(1); });
  Require(
      context.ActiveTrackedPayload() == 0 && context.PeakTrackedPayload() == 64,
      "payload release or peak");
  ExecutionContext empty({1, 1, 0});
  Require(TryReservePayload(empty, 0).status == ReservationStatus::kAccepted &&
              TryReservePayload(empty, 1).status ==
                  ReservationStatus::kBudgetExceeded,
          "zero cap");
  ExecutionContext maximum({2, 1, std::numeric_limits<std::size_t>::max()});
  auto full =
      TryReservePayload(maximum, std::numeric_limits<std::size_t>::max());
  Require(full.status == ReservationStatus::kAccepted &&
              TryReservePayload(maximum, 1).status ==
                  ReservationStatus::kBudgetExceeded,
          "counter overflow at size max");
  Require(full.lease.TryResize(1) == ReservationStatus::kAccepted &&
              full.lease.TryResize(std::numeric_limits<std::size_t>::max()) ==
                  ReservationStatus::kAccepted,
          "max resize arithmetic");
  ExecutionContext moved({2, 1, 8});
  auto destination = std::move(moved);
  Invalid([&] { (void)TryReservePayload(moved, 0, stop.get_token()); });
  Invalid([&] { (void)TryReserveWorkers(moved, 0); });
  Require(destination.TrackedPayloadBudget() == 8,
          "context move changed budget");
}
void LifetimeCases() {
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
  first.Reset();
  workers.Reset();
  second.Reset();
}
Mesh Quads(std::size_t faces) {
  Mesh mesh;
  for (std::size_t face = 0; face < faces; ++face) {
    const auto base = static_cast<index_t>(mesh.positions.size());
    const auto x = static_cast<double>(face * 2);
    mesh.positions.insert(mesh.positions.end(),
                          {{x, 0, 0}, {x + 1, 0, 0}, {x + 1, 1, 0}, {x, 1, 0}});
    for (index_t corner = 0; corner < 4; ++corner)
      mesh.corner_vertices.push_back(base + corner);
    mesh.face_offsets.push_back(
        static_cast<index_t>(mesh.corner_vertices.size()));
  }
  return mesh;
}
void WorkerAndAlgorithmCases() {
  ExecutionContext context({4, 1});
  auto source = Quads(80);
  auto imported = EditableMesh::ImportMesh(source);
  auto snapshot = imported.mesh.Snapshot();
  Require(TryReserveWorkers(context, 0).Count() == 0 &&
              TryReserveWorkers(context, 1).Count() == 0,
          "minimum two slots");
  {
    auto held = TryReserveWorkers(context, 3);
    Require(held.Count() == 3 && TryReserveWorkers(context, 4).Count() == 0,
            "single remaining slot admitted");
    std::atomic<std::size_t> joined{0};
    {
      std::vector<std::jthread> threads;
      for (std::size_t i = 0; i < held.Count(); ++i) {
        threads.emplace_back([&] {
          const auto bounds = ComputeBounds(snapshot, context);
          const auto triangles = Triangulate(source, {}, context);
          Require(bounds.workers_used == 0 && triangles.workers_used == 0 &&
                      triangles.status == TriangulationStatus::kAccepted,
                  "nested operation escaped worker cap");
          joined.fetch_add(1);
        });
      }
    }
    Require(joined == 3 && context.ActiveWorkers() == 3,
            "lease released before real joins");
    auto moved = std::move(held);
    Require(held.Count() == 0 && moved.Count() == 3, "worker move");
  }
  Require(context.ActiveWorkers() == 0, "worker cleanup");
  const auto parallel = Triangulate(source, {}, context);
  Require(parallel.workers_used == 4 &&
              ComputeBounds(snapshot, context).workers_used == 4,
          "existing real parallel worker policy changed");
  auto full = TryReservePayload(context, context.TrackedPayloadBudget());
  const auto denied = Triangulate(source, {}, context);
  Require(denied.status == TriangulationStatus::kBlocked && !denied.mesh &&
              denied.diagnostics[0].code == "conversion.payload_budget",
          "existing triangulation ignored public payload");
  Require(ComputeBounds(snapshot, context).bounds.has_value(),
          "bounds charged payload");
  std::stop_source stop;
  stop.request_stop();
  Require(Triangulate(source, {}, context, stop.get_token()).status ==
              TriangulationStatus::kCanceled,
          "existing cancellation priority changed");
  full.lease.Reset();
  Require(parallel.mesh.has_value() && context.ActiveTrackedPayload() == 0,
          "returned triangulation result became charged");
  std::atomic<std::size_t> active_before_join{0};
  try {
    auto lease = TryReserveWorkers(context, 4);
    std::vector<std::jthread> threads;
    threads.emplace_back([&] { active_before_join = context.ActiveWorkers(); });
    // Deterministic failure before the second launch; unwinding joins the
    // first.
    throw std::runtime_error("injected partial launch");
  } catch (const std::runtime_error&) {
  }
  Require(active_before_join == 4 && context.ActiveWorkers() == 0,
          "partial-launch unwind released before join");
  auto first = TryReserveWorkers(context, 2);
  auto second = TryReserveWorkers(context, 2);
  first = std::move(second);
  Require(
      context.ActiveWorkers() == 2 && first.Count() == 2 && second.Count() == 0,
      "worker move assignment charge");
}
void ConcurrentCases() {
  ExecutionContext context({4, 1, 32});
  std::barrier barrier(8);
  std::atomic<bool> valid{true};
  std::vector<std::jthread> threads;
  for (std::size_t i = 0; i < 8; ++i) {
    threads.emplace_back([&, i, copy = context] {
      auto payload = TryReservePayload(copy, 8);
      auto workers = TryReserveWorkers(copy, 2);
      barrier.arrive_and_wait();
      if (i == 0 &&
          (copy.ActiveTrackedPayload() != 32 || copy.ActiveWorkers() != 4))
        valid = false;
      barrier.arrive_and_wait();
      payload.lease.Reset();
      workers.Reset();
      for (std::size_t iteration = 0; iteration < 500; ++iteration) {
        auto reservation = TryReservePayload(copy, 8);
        auto slots = TryReserveWorkers(copy, 4);
        if (reservation.status == ReservationStatus::kAccepted) {
          const auto status = reservation.lease.TryResize(16);
          if (status == ReservationStatus::kBudgetExceeded &&
              reservation.lease.Bytes() != 8)
            valid = false;
          if (reservation.lease.TryResize(0) != ReservationStatus::kAccepted)
            valid = false;
        }
        if (copy.ActiveTrackedPayload() > 32 || copy.ActiveWorkers() > 4)
          valid = false;
      }
    });
  }
  for (auto& thread : threads) thread.join();
  Require(valid && context.ActiveTrackedPayload() == 0 &&
              context.ActiveWorkers() == 0 &&
              context.PeakTrackedPayload() == 32 && context.PeakWorkers() == 4,
          "concurrent mixed ledger invariant");
}
}  // namespace
int main() {
  try {
    PayloadCases();
    LifetimeCases();
    WorkerAndAlgorithmCases();
    ConcurrentCases();
    std::cout << "Shared execution lease cases passed\n";
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
