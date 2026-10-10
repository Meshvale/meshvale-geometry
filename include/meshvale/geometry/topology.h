// SPDX-License-Identifier: Apache-2.0
#ifndef MESHVALE_GEOMETRY_TOPOLOGY_H_
#define MESHVALE_GEOMETRY_TOPOLOGY_H_

#include <meshvale/geometry/mesh.h>

#include <array>
#include <optional>
#include <string>
#include <vector>

namespace meshvale::geometry {

enum class TopologyCheckStatus { performed, blocked, unsupported };
struct TopologyCheck {
  std::string name;
  TopologyCheckStatus status;
  std::string reason;
};
struct CornerIncidence {
  index_t vertex, face, previous, next, edge;
  bool operator==(const CornerIncidence&) const = default;
};
struct EdgeIncidence {
  std::array<index_t, 2> vertices;
  std::vector<index_t> corners;
  bool operator==(const EdgeIncidence&) const = default;
};
enum class VertexTopology {
  isolated,
  interior,
  boundary,
  nonmanifold,
  degenerate
};
struct VertexIncidence {
  std::vector<std::vector<index_t>> fans;
  VertexTopology kind{VertexTopology::isolated};
  bool operator==(const VertexIncidence&) const = default;
};
enum class BoundaryTopology { cycle, chain, branched, degenerate };
struct BoundaryComponent {
  std::vector<index_t> edges;
  std::vector<index_t> vertices;
  BoundaryTopology kind{BoundaryTopology::cycle};
  bool operator==(const BoundaryComponent&) const = default;
};
struct TopologyInspection;
class TopologySnapshot {
  friend TopologyInspection inspect_topology(const Mesh&);
  std::vector<CornerIncidence> corners_;
  std::vector<EdgeIncidence> edges_;
  std::vector<VertexIncidence> vertices_;
  std::vector<std::vector<index_t>> face_components_;
  std::vector<BoundaryComponent> boundaries_;

 public:
  [[nodiscard]] const std::vector<CornerIncidence>& corners() const noexcept {
    return corners_;
  }
  [[nodiscard]] const std::vector<EdgeIncidence>& edges() const noexcept {
    return edges_;
  }
  [[nodiscard]] const std::vector<VertexIncidence>& vertices() const noexcept {
    return vertices_;
  }
  [[nodiscard]] const std::vector<std::vector<index_t>>& face_components()
      const noexcept {
    return face_components_;
  }
  [[nodiscard]] const std::vector<BoundaryComponent>& boundaries()
      const noexcept {
    return boundaries_;
  }
  bool operator==(const TopologySnapshot&) const = default;
};
struct TopologyInspection {
  std::optional<TopologySnapshot> topology;
  std::vector<TopologyCheck> checks;
  std::vector<Diagnostic> diagnostics;
};

[[nodiscard]] TopologyInspection inspect_topology(const Mesh& mesh);
}  // namespace meshvale::geometry

#endif  // MESHVALE_GEOMETRY_TOPOLOGY_H_
