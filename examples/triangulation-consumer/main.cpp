// SPDX-License-Identifier: Apache-2.0
#include <meshvale/geometry/triangulation.h>

#include <iostream>
#include <stdexcept>
#include <vector>

int main() {
  using namespace meshvale::geometry;
  Mesh source;
  source.positions = {{0, 0, 0}, {4, 0, 0}, {4, 4, 0}, {2, 1, 0}, {0, 4, 0}};
  source.face_offsets = {0, 5};
  source.corner_vertices = {0, 1, 2, 3, 4};
  Attribute uv;
  uv.domain = AttributeDomain::corner;
  uv.name = "uv1";
  uv.semantic = "TEXCOORD";
  uv.set_index = 1;
  uv.components = 2;
  uv.values = std::vector<float>{0, 0, 1, 0, 1, 1, .5f, .25f, 0, 1};
  source.attributes.push_back(uv);
  ExecutionContext execution({2, 0});
  const auto result = Triangulate(source, {}, execution);
  if (!result.mesh || result.status != TriangulationStatus::kAccepted ||
      result.mesh->face_count() != 3 ||
      result.face_output_offsets != std::vector<index_t>{0, 3})
    throw std::runtime_error("Concave triangulation was not accepted");
  const auto& source_uv = std::get<std::vector<float>>(uv.values);
  const auto& output_uv =
      std::get<std::vector<float>>(result.mesh->attributes[0].values);
  for (std::size_t i = 0; i < result.corner_sources.size(); ++i) {
    if (result.mesh->corner_vertices[i] !=
        source.corner_vertices[result.corner_sources[i]])
      throw std::runtime_error("Corner correspondence changed");
    for (std::size_t component = 0; component < 2; ++component)
      if (output_uv[2 * i + component] !=
          source_uv[2 * result.corner_sources[i] + component])
        throw std::runtime_error("Authored UV correspondence changed");
  }
  if (source.face_count() != 1 || execution.ActiveWorkers() ||
      execution.ActiveTrackedPayload())
    throw std::runtime_error("Source or execution reservation was retained");
  std::cout
      << "Accepted three triangles with authored corner UV correspondence\n";
}
