// SPDX-License-Identifier: Apache-2.0
#include <meshvale/geometry/mesh.hpp>

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
    return meshvale::geometry::inspect_storage(mesh).empty() ? 0 : 1;
}
