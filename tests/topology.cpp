// SPDX-License-Identifier: Apache-2.0
#include <meshvale/geometry/topology.h>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <type_traits>

using namespace meshvale::geometry;
static_assert(std::is_const_v<std::remove_reference_t<decltype(std::declval<TopologySnapshot&>().corners())>>);
static_assert(std::is_const_v<std::remove_reference_t<decltype(std::declval<TopologySnapshot&>().edges())>>);
static_assert(std::is_const_v<std::remove_reference_t<decltype(std::declval<TopologySnapshot&>().vertices())>>);
static_assert(std::is_const_v<std::remove_reference_t<decltype(std::declval<TopologySnapshot&>().face_components())>>);
static_assert(std::is_const_v<std::remove_reference_t<decltype(std::declval<TopologySnapshot&>().boundaries())>>);
void require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}
bool has(const TopologyInspection& result, const std::string& code, std::optional<index_t> id = std::nullopt) {
    return std::any_of(result.diagnostics.begin(), result.diagnostics.end(),
        [&](const auto& item) { return item.code == code && (!id || item.element == id); });
}
TopologyCheckStatus coverage(const TopologyInspection& result, const std::string& name) {
    const auto found = std::find_if(result.checks.begin(), result.checks.end(),
                                   [&](const auto& item) { return item.name == name; });
    require(found != result.checks.end(), "check coverage missing");
    return found->status;
}
Mesh mesh(index_t vertices, const std::vector<std::vector<index_t>>& faces) {
    Mesh result; result.positions.resize(vertices);
    for (index_t v = 0; v < vertices; ++v) result.positions[v] = {static_cast<double>(v), 0, 0};
    for (const auto& face : faces) {
        result.corner_vertices.insert(result.corner_vertices.end(), face.begin(), face.end());
        result.face_offsets.push_back(result.corner_vertices.size());
    }
    return result;
}
index_t edge_id(const TopologySnapshot& view, index_t a, index_t b) {
    const std::array<index_t, 2> pair{std::min(a,b), std::max(a,b)};
    for (index_t e = 0; e < view.edges().size(); ++e) if (view.edges()[e].vertices == pair) return e;
    throw std::runtime_error("expected edge missing");
}
void mixed_polygons_and_seams() {
    auto input = mesh(10, {{0,1,2},{1,3,4,2},{5,6,7,8,9}});
    input.positions = {{0,0,0},{1,0,0},{0,1,0},{2,0,0},{2,1,0},
                       {4,0,0},{6,0,0},{6,2,0},{5,1,0},{4,2,0}};
    const auto before = inspect_topology(input);
    require(before.topology && before.diagnostics.empty(), "clean mixed polygons rejected");
    const auto& view = *before.topology;
    require(view.corners().size() == 12 && view.edges().size() == 11 && view.vertices().size() == 10,
            "mixed polygon incidence count wrong");
    require(view.face_components() == std::vector<std::vector<index_t>>{{0,1},{2}}, "face components wrong");
    require(view.boundaries().size() == 2 && view.boundaries()[0].kind == BoundaryTopology::cycle &&
            view.boundaries()[1].kind == BoundaryTopology::cycle, "open sheets lost boundary cycles");
    require(view.edges()[edge_id(view,1,2)].corners == std::vector<index_t>{1,6}, "shared edge correspondence wrong");
    for (index_t c = 0; c < view.corners().size(); ++c) {
        const auto& item = view.corners()[c];
        require(view.corners()[item.next].previous == c && view.corners()[item.previous].next == c,
                "polygon navigation inconsistent");
        require(item.vertex == input.corner_vertices[c], "corner identity changed");
    }
    for (const auto& v : view.vertices()) require(v.kind == VertexTopology::boundary, "boundary vertex misclassified");
    Attribute uv; uv.domain = AttributeDomain::corner; uv.name = "uv0"; uv.semantic = "texcoord";
    uv.components = 2; uv.set_index = 0; uv.values = std::vector<double>(24, 0.0);
    auto uv1 = uv; uv1.name = "uv1"; uv1.set_index = 1;
    std::get<std::vector<double>>(uv.values)[6] = 0.25;
    std::get<std::vector<double>>(uv1.values)[12] = 0.75;
    input.attributes = {uv,uv1};
    require(inspect_topology(input).topology == before.topology, "UV seam/sets changed connectivity");
    require(input.face_offsets == std::vector<index_t>{0,3,7,12} && input.corner_vertices.size() == 12 &&
            std::get<std::vector<double>>(input.attributes[0].values)[6] == 0.25, "inspection changed source");
    require(coverage(before,"self_intersection") == TopologyCheckStatus::unsupported &&
            coverage(before,"face_planarity") == TopologyCheckStatus::unsupported, "geometric coverage overstated");
}
void closed_mesh_and_winding() {
    auto tetra = mesh(4, {{0,2,1},{0,1,3},{1,2,3},{2,0,3}});
    tetra.positions = {{0,0,0},{1,0,0},{0,1,0},{0,0,1}};
    auto result = inspect_topology(tetra);
    require(result.topology && result.diagnostics.empty(), "consistent tetrahedron diagnosed as defective");
    const auto& view = *result.topology;
    require(view.edges().size() == 6 && view.boundaries().empty() && view.face_components().size() == 1, "closed incidence wrong");
    for (const auto& v : view.vertices())
        require(v.kind == VertexTopology::interior && v.fans.size() == 1 && v.fans.front().size() == 3,
                "interior vertex fan wrong");
    for (const auto& e : view.edges()) require(e.corners.size() == 2, "interior edge lost occurrence");
    auto conflict = inspect_topology(mesh(4, {{0,1,2},{0,1,3}}));
    const auto shared = edge_id(*conflict.topology,0,1);
    require(has(conflict,"topology.edge_orientation_conflict",shared), "same directed winding not diagnosed");
    require(conflict.topology->vertices()[0].kind == VertexTopology::boundary,
            "winding conflict confused with undirected vertex manifoldness");
    require(coverage(result,"outward_orientation") == TopologyCheckStatus::unsupported &&
            coverage(result,"solid_containment") == TopologyCheckStatus::unsupported, "solid coverage overstated");
}
void higher_incidence_and_disconnected_fans() {
    const auto result = inspect_topology(mesh(5, {{0,1,2},{1,0,3},{0,1,4}}));
    require(result.topology.has_value(), "non-manifold edge blocked raw inspection");
    const auto& view = *result.topology;
    const auto edge = edge_id(view,0,1);
    require(view.edges()[edge].corners == std::vector<index_t>{0,3,6}, "third edge occurrence lost");
    require(has(result,"topology.edge_nonmanifold",edge) && has(result,"topology.edge_orientation_ambiguous",edge),
            "higher incidence/orientation not reported");
    require(view.vertices()[0].fans.size() == 1 && view.vertices()[0].fans.front() == std::vector<index_t>{0,4,6} &&
            view.vertices()[0].kind == VertexTopology::nonmanifold, "connected branching fan misclassified");
    require(view.face_components() == std::vector<std::vector<index_t>>{{0,1,2}}, "higher incidence disconnected faces");
    require(view.boundaries().size() == 1 && view.boundaries()[0].kind == BoundaryTopology::branched,
            "branched boundary invented a loop");
    const auto bowtie = inspect_topology(mesh(5, {{0,1,2},{0,3,4}}));
    require(bowtie.topology && bowtie.topology->vertices()[0].fans == std::vector<std::vector<index_t>>{{0},{3}} &&
            bowtie.topology->vertices()[0].kind == VertexTopology::nonmanifold,
            "disconnected vertex fans missed");
    require(has(bowtie,"topology.vertex_nonmanifold",0) && !has(bowtie,"topology.edge_nonmanifold"),
            "edge manifoldness used as vertex manifoldness");
    require(bowtie.topology->face_components() == std::vector<std::vector<index_t>>{{0},{1}}, "vertex contact joined face components");
    require(bowtie.topology->boundaries().front().kind == BoundaryTopology::branched, "touching boundary cycles not reported");
}
void repeated_occurrences_and_duplicates() {
    const auto repeated = inspect_topology(mesh(5, {{0,1,2,0,3,4}}));
    require(repeated.topology && repeated.topology->corners().size() == 6 &&
            has(repeated,"topology.face_repeated_vertex",0) && has(repeated,"topology.vertex_degenerate",0),
            "repeated face vertex normalized away");
    require(repeated.topology->vertices()[0].fans == std::vector<std::vector<index_t>>{{0},{3}},
            "repeated vertex occurrences merged within face");
    const auto reused = inspect_topology(mesh(3, {{0,1,0,2}}));
    const auto edge = edge_id(*reused.topology,0,1);
    require(has(reused,"topology.edge_repeated_face",edge) && reused.topology->edges()[edge].corners.size() == 2 &&
            has(reused,"topology.edge_orientation_ambiguous",edge), "same-face edge use mistaken for manifold adjacency");
    const auto self = inspect_topology(mesh(2, {{0,0,1}}));
    require(self.topology && has(self,"topology.edge_self_loop",edge_id(*self.topology,0,0)) &&
            self.topology->boundaries().front().kind == BoundaryTopology::degenerate, "self-loop boundary reported as cycle");
    const auto same = inspect_topology(mesh(3, {{0,1,2},{1,2,0}}));
    require(same.topology && same.topology->corners().size() == 6 && same.topology->boundaries().empty() &&
            has(same,"topology.edge_orientation_conflict"), "duplicate faces lost or winding hidden");
    const auto reversed = inspect_topology(mesh(3, {{0,1,2},{2,1,0}}));
    require(reversed.topology && reversed.diagnostics.empty() && reversed.topology->corners().size() == 6,
            "reversed duplicate confused with geometric intersection check");
    require(coverage(reversed,"self_intersection") == TopologyCheckStatus::unsupported, "overlapping faces claimed geometrically valid");
}
void boundary_chains_and_cycle_with_nonmanifold_vertex() {
    // Additional faces pair four outer edges; only the path 0--4--1 remains boundary.
    auto chain = inspect_topology(mesh(5, {{0,1,2},{1,0,3},{0,1,4},{0,2,3},{1,3,2}}));
    require(chain.topology && chain.topology->boundaries().size() == 1 &&
            chain.topology->boundaries()[0].kind == BoundaryTopology::chain &&
            chain.topology->boundaries()[0].vertices == std::vector<index_t>{0,1,4} &&
            chain.topology->boundaries()[0].edges.size() == 2 && has(chain,"topology.boundary_chain",0),
            "boundary chain mistaken for a closed loop");
    auto input = mesh(7, {{0,1,2},{0,1,3},{0,1,4},{0,1,5},{1,0,6}});
    // All five sheets contribute two boundary edges: this is a branched graph.
    auto branched = inspect_topology(input);
    require(branched.topology->boundaries().front().kind == BoundaryTopology::branched, "high-valence boundary misclassified");
    // A closed tetrahedron and a triangle share only vertex 0: the boundary graph remains a cycle,
    // while the surface vertex has two fans and is not manifold.
    auto cycle = inspect_topology(mesh(6, {{0,2,1},{0,1,3},{1,2,3},{2,0,3},{0,4,5}}));
    require(cycle.topology->boundaries().size() == 1 && cycle.topology->boundaries()[0].kind == BoundaryTopology::cycle &&
            cycle.topology->vertices()[0].kind == VertexTopology::nonmanifold, "boundary graph confused with surface manifoldness");
}
void malformed_storage_and_coverage() {
    const auto good = mesh(3, {{0,1,2}});
    std::vector<Mesh> malformed;
    auto bad = good; bad.face_offsets.clear(); malformed.push_back(bad);
    bad = good; bad.face_offsets.front() = 1; malformed.push_back(bad);
    bad = good; bad.face_offsets.back() = 99; malformed.push_back(bad);
    bad = good; bad.face_offsets = {0,3,2}; malformed.push_back(bad);
    bad = good; bad.face_offsets = {0,2,3}; malformed.push_back(bad);
    bad = good; bad.corner_vertices[1] = 99; malformed.push_back(bad);
    for (const auto& input : malformed) {
        const auto result = inspect_topology(input);
        require(!result.topology && !result.diagnostics.empty(), "unsafe topology constructed");
        require(coverage(result,"storage") == TopologyCheckStatus::performed &&
                coverage(result,"vertex_fans") == TopologyCheckStatus::blocked &&
                coverage(result,"self_intersection") == TopologyCheckStatus::unsupported, "blocked/unsupported coverage conflated");
    }
    bad = good; bad.positions[0][0] = std::numeric_limits<double>::infinity();
    Attribute invalid; invalid.name = "uv"; invalid.domain = AttributeDomain::corner;
    invalid.components = 2; invalid.values = std::vector<float>{0}; bad.attributes.push_back(invalid);
    const auto result = inspect_topology(bad);
    require(result.topology && has(result,"mesh.nonfinite_position") && has(result,"attribute.row_count") &&
            coverage(result,"edge_incidence") == TopologyCheckStatus::performed,
            "safe combinatorial inspection blocked by geometric/attribute defects");
}
void ownership_determinism_and_coincident_positions() {
    const auto empty = inspect_topology(Mesh{});
    require(empty.topology && empty.topology->corners().empty() && empty.topology->vertices().empty() &&
            empty.diagnostics.empty(), "empty mesh inspection failed");
    auto isolated = inspect_topology(mesh(1, {}));
    require(isolated.topology && isolated.topology->vertices()[0].kind == VertexTopology::isolated &&
            has(isolated,"topology.vertex_isolated",0), "isolated vertex assigned surface manifoldness");
    TopologySnapshot snapshot;
    {
        auto input = mesh(6, {{0,1,2},{3,4,5}});
        std::fill(input.positions.begin(), input.positions.end(), std::array<double,3>{0,0,0});
        auto a = inspect_topology(input), b = inspect_topology(input);
        require(a.topology == b.topology && a.topology->face_components().size() == 2,
                "inspection nondeterministic or coincident vertices welded");
        snapshot = *a.topology;
        input.corner_vertices.clear(); input.positions.clear();
        require(a.topology == snapshot, "snapshot borrowed source storage");
    }
    require(snapshot.corners()[0].next == 1 && snapshot.edges().size() == 6 && snapshot.vertices()[3].fans[0][0] == 3,
            "snapshot invalid after source destruction");
    auto copy = snapshot; copy = TopologySnapshot{};
    require(snapshot.edges()[0].corners.size() == 1, "snapshot copies aliased");
}
void many_incident_faces() {
    constexpr index_t count = 2048;
    std::vector<std::vector<index_t>> faces;
    for (index_t i = 0; i < count; ++i) faces.push_back({0,1,i+2});
    const auto result = inspect_topology(mesh(count+2, faces));
    require(result.topology && result.topology->edges()[edge_id(*result.topology,0,1)].corners.size() == count &&
            result.topology->vertices()[0].fans.size() == 1 && result.topology->vertices()[0].fans[0].size() == count &&
            result.topology->face_components()[0].size() == count, "high-incidence traversal dropped faces");
}
int main() {
    try {
        mixed_polygons_and_seams(); closed_mesh_and_winding(); higher_incidence_and_disconnected_fans();
        repeated_occurrences_and_duplicates(); boundary_chains_and_cycle_with_nonmanifold_vertex();
        malformed_storage_and_coverage(); ownership_determinism_and_coincident_positions(); many_incident_faces();
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
    std::cout << "General polygon topology checks passed\n";
}
