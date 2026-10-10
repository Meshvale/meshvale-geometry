// SPDX-License-Identifier: Apache-2.0
#ifndef MESHVALE_GEOMETRY_MESH_H_
#define MESHVALE_GEOMETRY_MESH_H_

#include <meshvale/geometry/attributes.h>

#include <array>
#include <vector>

namespace meshvale::geometry {

struct Mesh {
  std::vector<std::array<double, 3>> positions;
  std::vector<index_t> face_offsets{0};
  std::vector<index_t> corner_vertices;
  std::vector<Attribute> attributes;

  [[nodiscard]] index_t face_count() const;
  [[nodiscard]] index_t row_count(AttributeDomain domain) const;
};

// Diagnostics inspect raw storage; they never normalize or repair the mesh.
[[nodiscard]] std::vector<Diagnostic> inspect_storage(const Mesh& mesh);

}  // namespace meshvale::geometry

#endif  // MESHVALE_GEOMETRY_MESH_H_
