// SPDX-License-Identifier: Apache-2.0
#ifndef MESHVALE_GEOMETRY_SRC_MATERIALIZATION_TEST_INTERNAL_H_
#define MESHVALE_GEOMETRY_SRC_MATERIALIZATION_TEST_INTERNAL_H_

#include <meshvale/geometry/attributes.h>
#include <meshvale/geometry/execution.h>
#include <meshvale/geometry/materialization.h>

#include <span>
#include <stop_token>

#include "materialization_allocation.h"
#include "materialization_workers.h"

namespace meshvale::geometry::materialization_detail {
// Per-call injection only. Backend state outlives every returned allocation;
// callbacks obey original allocation/free and successful native launch/join.
struct TestHooks {
  AllocationBackend allocation = DefaultAllocationBackend();
  NativeWorkerBackend workers = DefaultNativeWorkerBackend();
  MaterializationExecutionStats* execution_observer = nullptr;
};
[[nodiscard]] MeshMaterializationResult TestMaterializeMesh(
    const MeshMaterializationInput& input,
    const MeshMaterializationLimits& limits,
    const MeshMaterializationWorkPolicy& work, const ExecutionContext& context,
    std::stop_token stop, const TestHooks& hooks);
[[nodiscard]] MeshInspectionResult TestInspectMaterializedMesh(
    const MaterializedMesh& source, const MeshInspectionLimits& limits,
    const MeshMaterializationWorkPolicy& work, std::stop_token stop,
    const TestHooks& hooks);
[[nodiscard]] MeshFaceSelectionResult TestSelectMeshFaces(
    const MaterializedMesh& source, std::span<const index_t> kept_faces,
    const MeshMaterializationLimits& limits,
    const MeshMaterializationWorkPolicy& work, std::stop_token stop,
    const TestHooks& hooks);
}  // namespace meshvale::geometry::materialization_detail
#endif  // MESHVALE_GEOMETRY_SRC_MATERIALIZATION_TEST_INTERNAL_H_
