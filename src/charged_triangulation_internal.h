// SPDX-License-Identifier: Apache-2.0
#ifndef MESHVALE_GEOMETRY_SRC_CHARGED_TRIANGULATION_INTERNAL_H_
#define MESHVALE_GEOMETRY_SRC_CHARGED_TRIANGULATION_INTERNAL_H_

#include <meshvale/geometry/charged_triangulation.h>
#include <meshvale/geometry/execution.h>
#include <meshvale/geometry/materialization.h>
#include <meshvale/geometry/mesh.h>
#include <meshvale/geometry/triangulation.h>

#include <cstddef>
#include <stop_token>

#include "materialization_allocation.h"
#include "materialization_workers.h"

namespace meshvale::geometry::charged_triangulation_detail {
// Concrete test observation after actual gathered scalar writes. The default
// null observer changes no work. State/callback must remain alive until this
// synchronous call joins; neither is retained by output or phase facts.
struct TransferObserver {
  void (*copied)(std::size_t scalars, void* state) noexcept = nullptr;
  void* state = nullptr;
};
// Test-only dependency seam, never an installed callback interface. Backend
// state must outlive returned bridge blocks, as in materialization allocation.
struct Access {
  [[nodiscard]] static ChargedTriangulationResult Run(
      const Mesh& source, const TriangulationOptions& options,
      const ChargedTriangulationLimits& limits,
      const MeshMaterializationWorkPolicy& work,
      const ExecutionContext& execution, std::stop_token stop,
      const materialization_detail::AllocationBackend& allocation,
      const materialization_detail::NativeWorkerBackend& workers,
      const TransferObserver* transfer = nullptr);
  [[nodiscard]] static ChargedTriangulationResult Run(
      const MaterializedMesh& source, const TriangulationOptions& options,
      const ChargedTriangulationLimits& limits,
      const MeshMaterializationWorkPolicy& work, std::stop_token stop,
      const materialization_detail::AllocationBackend& allocation,
      const materialization_detail::NativeWorkerBackend& workers,
      const TransferObserver* transfer = nullptr);
};
}  // namespace meshvale::geometry::charged_triangulation_detail
#endif  // MESHVALE_GEOMETRY_SRC_CHARGED_TRIANGULATION_INTERNAL_H_
