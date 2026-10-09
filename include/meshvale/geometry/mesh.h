// SPDX-License-Identifier: Apache-2.0
#ifndef MESHVALE_GEOMETRY_MESH_H_
#define MESHVALE_GEOMETRY_MESH_H_

#include <meshvale/geometry/attributes.h>

#include <array>
#include <cmath>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace meshvale::geometry {

struct Mesh {
    std::vector<std::array<double, 3>> positions;
    std::vector<index_t> face_offsets{0};
    std::vector<index_t> corner_vertices;
    std::vector<Attribute> attributes;

    [[nodiscard]] index_t face_count() const {
        return face_offsets.empty() ? 0 : face_offsets.size() - 1;
    }
    [[nodiscard]] index_t row_count(AttributeDomain domain) const {
        switch (domain) {
            case AttributeDomain::vertex: return positions.size();
            case AttributeDomain::face: return face_count();
            case AttributeDomain::corner: return corner_vertices.size();
        }
        return 0;
    }
};

// Diagnostics inspect raw storage; they never normalize or repair the mesh.
[[nodiscard]] inline std::vector<Diagnostic> inspect_storage(const Mesh& mesh) {
    std::vector<Diagnostic> issues;
    auto issue = [&](std::string code, std::string subject,
                     std::optional<index_t> element = std::nullopt) {
        issues.push_back({std::move(code), std::move(subject), element});
    };
    for (index_t v = 0; v < mesh.positions.size(); ++v)
        for (double coordinate : mesh.positions[v])
            if (!std::isfinite(coordinate)) {
                issue("mesh.nonfinite_position", "positions", v);
                break;
            }
    if (mesh.face_offsets.empty()) {
        issue("mesh.empty_offsets", "face_offsets");
    } else {
        if (mesh.face_offsets.front() != 0) issue("mesh.offset_start", "face_offsets");
        if (mesh.face_offsets.back() != mesh.corner_vertices.size())
            issue("mesh.offset_end", "face_offsets");
        for (index_t i = 0; i < mesh.face_offsets.size(); ++i) {
            const auto offset = mesh.face_offsets[i];
            if (offset > mesh.corner_vertices.size()) issue("mesh.offset_range", "face_offsets", i);
            if (i > 0) {
                const auto previous = mesh.face_offsets[i - 1];
                if (offset < previous) issue("mesh.offset_order", "face_offsets", i);
                else if (offset - previous < 3) issue("mesh.short_face", "face_offsets", i - 1);
            }
        }
    }
    for (index_t c = 0; c < mesh.corner_vertices.size(); ++c)
        if (mesh.corner_vertices[c] >= mesh.positions.size())
            issue("mesh.vertex_range", "corner_vertices", c);

    std::set<std::pair<AttributeDomain, std::string>> names;
    for (const auto& attribute : mesh.attributes) {
        if (!names.insert({attribute.domain, attribute.name}).second)
            issue("attribute.duplicate_name", attribute.name);
        switch (attribute.domain) {
            case AttributeDomain::vertex:
            case AttributeDomain::face:
            case AttributeDomain::corner: break;
            default: issue("attribute.invalid_domain", attribute.name); continue;
        }
        auto found = inspect_attribute(attribute, mesh.row_count(attribute.domain));
        issues.insert(issues.end(), found.begin(), found.end());
    }
    return issues;
}

}  // namespace meshvale::geometry

#endif  // MESHVALE_GEOMETRY_MESH_H_
