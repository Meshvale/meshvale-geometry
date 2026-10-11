// SPDX-License-Identifier: Apache-2.0
#ifndef MESHVALE_TEST_LINUX_NUMERIC_REQUESTS_TRACE_H_
#define MESHVALE_TEST_LINUX_NUMERIC_REQUESTS_TRACE_H_
#include <meshvale/geometry/editable_mesh.h>
#include <meshvale/geometry/execution.h>

#include <cstddef>
#include <cstdint>

#include "materialization_allocation.h"
namespace observation {
enum class Route {
  kMalloc,
  kFree,
  kCalloc,
  kRealloc,
  kNew,
  kNewArray,
  kNewAligned,
  kNewArrayAligned,
  kDelete,
  kDeleteArray,
  kDeleteSized,
  kDeleteArraySized,
  kDeleteAligned,
  kDeleteArrayAligned,
  kDeleteSizedAligned,
  kDeleteArraySizedAligned,
  kLedger
};
enum class Action { kBegin, kEnd, kSample };
enum class Phase {
  kCalibration,
  kSeed,
  kSeedDestroy,
  kAdmitted,
  kConstruct,
  kDestroy,
  kOwnerFreed,
  kLeaseFreed,
  kDenied
};
enum class Mode { kControl, kSeed, kSeedDenied, kAdmitted, kDenied };
struct Case {
  const char* shape;
  std::size_t scalar_bytes;
  int width;
  std::size_t count;
  std::size_t implementation_bytes;
  std::size_t numerical_bytes;
  bool qualified;
  Mode mode;
  std::uintptr_t numeric_data = 0;
};
void Begin(const meshvale::geometry::ExecutionContext* context = nullptr,
           const meshvale::geometry::materialization_detail::AllocationContext*
               operation = nullptr) noexcept;
void SetPhase(Phase phase) noexcept;
void Sample() noexcept;
void Record(Route kind, Action action, std::size_t first, std::size_t second,
            std::uintptr_t pointer, std::uintptr_t previous = 0) noexcept;
bool End(const Case& observed) noexcept;
}  // namespace observation
#endif  // MESHVALE_TEST_LINUX_NUMERIC_REQUESTS_TRACE_H_
