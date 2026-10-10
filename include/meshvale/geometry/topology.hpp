// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "mesh.hpp"
#include <algorithm>
#include <numeric>

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
enum class VertexTopology { isolated, interior, boundary, nonmanifold, degenerate };
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
    [[nodiscard]] const auto& corners() const noexcept { return corners_; }
    [[nodiscard]] const auto& edges() const noexcept { return edges_; }
    [[nodiscard]] const auto& vertices() const noexcept { return vertices_; }
    [[nodiscard]] const auto& face_components() const noexcept { return face_components_; }
    [[nodiscard]] const auto& boundaries() const noexcept { return boundaries_; }
    bool operator==(const TopologySnapshot&) const = default;
};
struct TopologyInspection {
    std::optional<TopologySnapshot> topology;
    std::vector<TopologyCheck> checks;
    std::vector<Diagnostic> diagnostics;
};

namespace topology_detail {
class Sets {
    std::vector<index_t> parent_;
    std::vector<std::uint8_t> rank_;
public:
    explicit Sets(index_t count) : parent_(count), rank_(count) {
        std::iota(parent_.begin(), parent_.end(), index_t{0});
    }
    index_t root(index_t value) {
        while (value != parent_[value]) {
            parent_[value] = parent_[parent_[value]];
            value = parent_[value];
        }
        return value;
    }
    void join(index_t a, index_t b) {
        a = root(a); b = root(b);
        if (a == b) return;
        if (rank_[a] < rank_[b]) std::swap(a, b);
        parent_[b] = a;
        if (rank_[a] == rank_[b]) ++rank_[a];
    }
};
} // namespace topology_detail

[[nodiscard]] inline TopologyInspection inspect_topology(const Mesh& mesh) {
    TopologyInspection result;
    result.diagnostics = inspect_storage(mesh);
    const bool unsafe = std::any_of(result.diagnostics.begin(), result.diagnostics.end(), [](const auto& d) {
        return d.code.starts_with("mesh.") && d.code != "mesh.nonfinite_position";
    });
    result.checks.push_back({"storage", TopologyCheckStatus::performed, ""});
    for (const auto& name : {"edge_incidence", "vertex_fans", "edge_orientation", "boundary_components", "face_components"})
        result.checks.push_back({name, unsafe ? TopologyCheckStatus::blocked : TopologyCheckStatus::performed,
                                 unsafe ? "unsafe topology storage" : ""});
    for (const auto& name : {"geometric_degeneracy", "face_planarity", "self_intersection", "outward_orientation", "solid_containment"})
        result.checks.push_back({name, TopologyCheckStatus::unsupported, "not evaluated by combinatorial inspection"});
    if (unsafe) return result;
    TopologySnapshot view;
    view.corners_.resize(mesh.corner_vertices.size());
    view.vertices_.resize(mesh.positions.size());
    std::map<std::array<index_t, 2>, std::vector<index_t>> groups;
    std::vector<std::uint8_t> degenerate(mesh.positions.size()), branching(mesh.positions.size());
    auto issue = [&](const char* code, const char* domain, index_t element) {
        result.diagnostics.push_back({code, domain, element});
    };
    for (index_t face = 0; face < mesh.face_count(); ++face) {
        const auto begin = mesh.face_offsets[face], end = mesh.face_offsets[face + 1];
        std::set<index_t> seen;
        bool repeated = false;
        for (index_t corner = begin; corner < end; ++corner) {
            const auto next = corner + 1 == end ? begin : corner + 1;
            const auto previous = corner == begin ? end - 1 : corner - 1;
            const auto a = mesh.corner_vertices[corner], b = mesh.corner_vertices[next];
            view.corners_[corner] = {a, face, previous, next, 0};
            groups[{std::min(a,b), std::max(a,b)}].push_back(corner);
            if (!seen.insert(a).second) { repeated = true; degenerate[a] = 1; }
        }
        if (repeated) issue("topology.face_repeated_vertex", "faces", face);
    }
    topology_detail::Sets fan_sets(view.corners_.size()), face_sets(mesh.face_count()), boundary_sets(view.vertices_.size());
    std::vector<index_t> boundary_degree(view.vertices_.size());
    for (auto& [vertices, corners] : groups) {
        const auto edge = static_cast<index_t>(view.edges_.size());
        const auto a = vertices[0], b = vertices[1];
        bool repeated_face = false;
        std::set<index_t> faces;
        std::array<std::optional<index_t>, 2> first;
        for (auto c : corners) {
            auto& occurrence = view.corners_[c];
            occurrence.edge = edge;
            if (!faces.insert(occurrence.face).second) repeated_face = true;
            face_sets.join(view.corners_[corners.front()].face, occurrence.face);
            for (std::size_t endpoint = 0; endpoint < 2; ++endpoint) {
                const auto at = occurrence.vertex == vertices[endpoint] ? c : occurrence.next;
                if (first[endpoint]) fan_sets.join(*first[endpoint], at); else first[endpoint] = at;
            }
            // A self-loop has two distinct corner occurrences at the same vertex.
            if (a == b) fan_sets.join(c, occurrence.next);
        }
        if (a == b) {
            issue("topology.edge_self_loop", "edges", edge);
            degenerate[a] = 1;
        }
        if (repeated_face) {
            issue("topology.edge_repeated_face", "edges", edge);
            degenerate[a] = degenerate[b] = 1;
        }
        if (corners.size() > 2) {
            issue("topology.edge_nonmanifold", "edges", edge);
            branching[a] = branching[b] = 1;
        }
        if (a == b || repeated_face || corners.size() > 2)
            issue("topology.edge_orientation_ambiguous", "edges", edge);
        else if (corners.size() == 2 && view.corners_[corners[0]].vertex == view.corners_[corners[1]].vertex)
            issue("topology.edge_orientation_conflict", "edges", edge);
        if (corners.size() == 1) {
            ++boundary_degree[a]; ++boundary_degree[b];
            boundary_sets.join(a,b);
        }
        view.edges_.push_back({vertices, std::move(corners)});
    }
    std::vector<std::map<index_t, index_t>> fan_indices(view.vertices_.size());
    for (index_t c = 0; c < view.corners_.size(); ++c) {
        const auto vertex = view.corners_[c].vertex, root = fan_sets.root(c);
        auto& fans = view.vertices_[vertex].fans;
        const auto [found, inserted] = fan_indices[vertex].emplace(root, fans.size());
        if (inserted) fans.emplace_back();
        fans[found->second].push_back(c);
    }
    for (index_t vertex = 0; vertex < view.vertices_.size(); ++vertex) {
        auto& item = view.vertices_[vertex];
        if (item.fans.empty()) {
            issue("topology.vertex_isolated", "vertices", vertex);
        } else if (degenerate[vertex]) {
            item.kind = VertexTopology::degenerate;
            issue("topology.vertex_degenerate", "vertices", vertex);
        } else if (branching[vertex] || item.fans.size() != 1 ||
                   (boundary_degree[vertex] != 0 && boundary_degree[vertex] != 2)) {
            item.kind = VertexTopology::nonmanifold;
            issue("topology.vertex_nonmanifold", "vertices", vertex);
        } else item.kind = boundary_degree[vertex] ? VertexTopology::boundary : VertexTopology::interior;
    }
    std::map<index_t, index_t> components;
    for (index_t face = 0; face < mesh.face_count(); ++face) {
        const auto [found, inserted] = components.emplace(face_sets.root(face), view.face_components_.size());
        if (inserted) view.face_components_.emplace_back();
        view.face_components_[found->second].push_back(face);
    }
    components.clear();
    for (index_t edge = 0; edge < view.edges_.size(); ++edge) {
        const auto& item = view.edges_[edge];
        if (item.corners.size() != 1) continue;
        const auto [found, inserted] = components.emplace(boundary_sets.root(item.vertices[0]), view.boundaries_.size());
        if (inserted) view.boundaries_.emplace_back();
        auto& component = view.boundaries_[found->second];
        component.edges.push_back(edge);
        if (item.vertices[0] == item.vertices[1]) component.kind = BoundaryTopology::degenerate;
    }
    for (index_t vertex = 0; vertex < view.vertices_.size(); ++vertex) {
        if (!boundary_degree[vertex]) continue;
        view.boundaries_[components.at(boundary_sets.root(vertex))].vertices.push_back(vertex);
    }
    for (index_t id = 0; id < view.boundaries_.size(); ++id) {
        auto& component = view.boundaries_[id];
        if (component.kind == BoundaryTopology::degenerate) {
            issue("topology.boundary_degenerate", "boundaries", id); continue;
        }
        index_t ends = 0;
        bool branch = false;
        for (auto vertex : component.vertices) {
            ends += boundary_degree[vertex] == 1;
            branch |= boundary_degree[vertex] > 2;
        }
        if (branch || (ends != 0 && ends != 2)) {
            component.kind = BoundaryTopology::branched;
            issue("topology.boundary_branched", "boundaries", id);
        } else if (ends == 2) {
            component.kind = BoundaryTopology::chain;
            issue("topology.boundary_chain", "boundaries", id);
        }
    }
    result.topology = std::move(view);
    return result;
}
} // namespace meshvale::geometry
