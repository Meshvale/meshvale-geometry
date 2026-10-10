// SPDX-License-Identifier: Apache-2.0
#ifndef MESHVALE_GEOMETRY_TRIANGULATION_H_
#define MESHVALE_GEOMETRY_TRIANGULATION_H_

#include <meshvale/geometry/editable_mesh.h>
#include <meshvale/geometry/mesh.h>

#include <cstddef>
#include <optional>
#include <stop_token>
#include <string>
#include <vector>

namespace meshvale::geometry {

enum class TriangulationStatus { kAccepted, kBlocked, kCanceled };

struct TriangulationOptions {
  // This exact single-loop profile supports at most 4096 corners per face.
  // Valid limits are 3 through 4096; limits outside this range are unsupported.
  index_t max_corners_per_face = 4096;
  std::size_t minimum_parallel_faces = 64;
};

struct TriangulationResult {
  TriangulationStatus status = TriangulationStatus::kBlocked;
  std::optional<Mesh> mesh;
  std::vector<Diagnostic> diagnostics;
  // Output face/corner -> authored source face/corner storage index.
  std::vector<index_t> face_sources;
  std::vector<index_t> corner_sources;
  // Source face i owns output faces [offsets[i], offsets[i+1]).
  std::vector<index_t> face_output_offsets;
  std::size_t workers_used = 0;
  std::string serial_reason;
  // This call's conservative declared payload reservation, not allocated RSS.
  std::size_t peak_tracked_payload_bytes = 0;
};

// Converts ordered simple exactly planar binary64 loops using existing
// vertices. Source must not be modified by the caller during this call. Worker
// computation uses an owned immutable capture. Complete acceptance preserves
// position and channel scalar bits and returns correspondence;
// failures/cancellation expose no partial Mesh/maps. Cancellation has priority
// after worker joins. Copies of execution share worker and active
// declared-payload caps. See the triangulation contract for payload exclusions
// and the exact numerical profile. Allocation/thread-creation failures
// propagate as standard exceptions. Invalid moved-from execution contexts
// retain EditorError behavior. Thread safe for separate calls sharing immutable
// source/context, with external source lifetime synchronization. No
// normalization, interpolation or polygon reconstruction.
[[nodiscard]] TriangulationResult Triangulate(
    const Mesh& source, const TriangulationOptions& options,
    const ExecutionContext& execution, std::stop_token stop = {});

}  // namespace meshvale::geometry
#endif  // MESHVALE_GEOMETRY_TRIANGULATION_H_
