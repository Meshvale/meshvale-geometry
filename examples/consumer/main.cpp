// SPDX-License-Identifier: Apache-2.0
#include <meshvale/geometry/mesh.h>
#include <meshvale/geometry/topology.h>

int main() {
    meshvale::geometry::Mesh mesh;
    mesh.positions = {{0,0,0},{1,0,0},{0,1,0}};
    mesh.face_offsets = {0,3};
    mesh.corner_vertices = {0,1,2};
    meshvale::geometry::Attribute channel;
    channel.domain = meshvale::geometry::AttributeDomain::corner;
    channel.name = "lightmap";
    channel.semantic = "texcoord";
    channel.set_index = 1;
    channel.components = 2;
    channel.values = std::vector<float>{0,0,1,0,0,1};
    mesh.attributes.push_back(channel);
    if (!meshvale::geometry::inspect_storage(mesh).empty()) return 1;
    const auto inspected = meshvale::geometry::inspect_topology(mesh);
    if (!inspected.topology || !inspected.diagnostics.empty() || inspected.topology->corners().size() != 3 ||
        inspected.topology->boundaries().size() != 1 || inspected.topology->boundaries()[0].kind !=
        meshvale::geometry::BoundaryTopology::cycle) return 2;
    return 0;
}
