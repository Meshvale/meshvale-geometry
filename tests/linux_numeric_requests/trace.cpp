// SPDX-License-Identifier: Apache-2.0
#include "trace.h"

#include <meshvale/geometry/editable_mesh.h>
#include <meshvale/geometry/execution.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>

#include "materialization_allocation.h"
namespace observation {
const int kProtocolVersion = 1;
const char kProfileRecordFormat[] =
    "PROFILE\t%d\t%s\t%s\tCLANG=%s\tASAN=%d\tTSAN=%d\n";
const char kSourcesRecordFormat[] = "SOURCES\t%s\t%s\t%s\n";
namespace {
constexpr std::size_t kEventCapacity = 256;
constexpr char kCaseRecordFormat[] =
    "CASE\t%s\t%s\t%zu\t%d\t%zu\t%zu\t%zu\t%d\t%zu\t%zu\t%zu\t%zu\t%zu\t%"
    "zu\n";
constexpr char kEventRecordFormat[] =
    "EVENT\t%zu\t%s\t%s\t%s\t%zu\t%zu\t%zu\t%zu\t%zu\t%zu\n";
constexpr std::array kRoutes{"malloc",
                             "free",
                             "calloc",
                             "realloc",
                             "new",
                             "new-array",
                             "new-aligned",
                             "new-array-aligned",
                             "delete",
                             "delete-array",
                             "delete-sized",
                             "delete-array-sized",
                             "delete-aligned",
                             "delete-array-aligned",
                             "delete-sized-aligned",
                             "delete-array-sized-aligned",
                             "ledger"};
constexpr std::array kActions{"begin", "end", "sample"};
constexpr std::array kPhases{"calibration", "seed",        "seed-destroy",
                             "admitted",    "construct",   "destroy",
                             "owner-freed", "lease-freed", "denied"};
constexpr std::array kModes{"control", "seed", "seed-denied", "admitted",
                            "denied"};
struct Event {
  Phase phase;
  Route kind;
  Action action;
  std::size_t first, second;
  std::uintptr_t pointer, previous;
  std::size_t root_bytes, operation_bytes;
};
std::array<Event, kEventCapacity> events;
std::size_t count = 0;
bool active = false;
Phase phase = Phase::kCalibration;
const meshvale::geometry::ExecutionContext* ledger = nullptr;
const meshvale::geometry::materialization_detail::AllocationContext* operation =
    nullptr;
std::size_t root_before = 0, operation_before = 0;
std::size_t RootBytes() noexcept {
  return ledger ? ledger->ActiveTrackedPayload() : 0;
}
std::size_t OperationBytes() noexcept {
  return operation ? operation->OperationActiveBytes() : 0;
}
}  // namespace
void Begin(const meshvale::geometry::ExecutionContext* context,
           const meshvale::geometry::materialization_detail::AllocationContext*
               current) noexcept {
  // This observer is intentionally serial. Reset and dump never overlap an
  // original call; the factory's real worker proofs have a separate owner.
  ledger = context;
  operation = current;
  root_before = RootBytes();
  operation_before = OperationBytes();
  count = 0;
  phase = Phase::kCalibration;
  active = true;
}
void SetPhase(Phase value) noexcept { phase = value; }
void Record(Route kind, Action action, std::size_t first, std::size_t second,
            std::uintptr_t pointer, std::uintptr_t previous) noexcept {
  if (!active) return;
  const auto slot = count++;
  if (slot < events.size())
    events[slot] = {phase,   kind,     action,      first,           second,
                    pointer, previous, RootBytes(), OperationBytes()};
}
void Sample() noexcept { Record(Route::kLedger, Action::kSample, 0, 0, 0); }
bool End(const Case& observed) noexcept {
  active = false;
  std::printf(kCaseRecordFormat, observed.shape,
              kModes[static_cast<std::size_t>(observed.mode)],
              observed.scalar_bytes, observed.width, observed.count,
              observed.implementation_bytes, observed.numerical_bytes,
              observed.qualified, count, root_before, operation_before,
              RootBytes(), OperationBytes(),
              static_cast<std::size_t>(observed.numeric_data));
  const auto visible = count < events.size() ? count : events.size();
  for (std::size_t i = 0; i != visible; ++i) {
    const auto& event = events[i];
    std::printf(kEventRecordFormat, i,
                kPhases[static_cast<std::size_t>(event.phase)],
                kRoutes[static_cast<std::size_t>(event.kind)],
                kActions[static_cast<std::size_t>(event.action)], event.first,
                event.second, static_cast<std::size_t>(event.pointer),
                static_cast<std::size_t>(event.previous), event.root_bytes,
                event.operation_bytes);
  }
  return count <= events.size();
}
}  // namespace observation
