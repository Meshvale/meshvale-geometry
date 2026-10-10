// SPDX-License-Identifier: Apache-2.0
#include <meshvale/geometry/python/record.h>
#include <meshvale/geometry/topology.h>
#include <nanobind/stl/string.h>
#include <nanobind/stl/vector.h>

#include "snapshot.h"
#include "triangulation_bindings.h"

namespace nb = nanobind;
namespace geo = meshvale::geometry;

namespace {
using geo::bindings::Snapshot;
nb::list diagnostics(const std::vector<geo::Diagnostic>& values) {
  nb::list result;
  for (const auto& value : values) {
    nb::dict item;
    item["code"] = nb::cast(value.code);
    item["subject"] = nb::cast(value.subject);
    item["element"] = value.element ? nb::cast(*value.element) : nb::none();
    result.append(item);
  }
  return result;
}
nb::dict topology(const Snapshot& mesh) {
  const auto inspection = geo::inspect_topology(mesh.Data());
  nb::dict result;
  result["diagnostics"] = diagnostics(inspection.diagnostics);
  nb::list checks;
  for (const auto& check : inspection.checks) {
    nb::dict item;
    item["name"] = nb::cast(check.name);
    item["status"] =
        check.status == geo::TopologyCheckStatus::performed ? "performed"
        : check.status == geo::TopologyCheckStatus::blocked ? "blocked"
                                                            : "unsupported";
    item["reason"] = nb::cast(check.reason);
    checks.append(item);
  }
  result["checks"] = checks;
  result["topology"] = nb::none();
  if (!inspection.topology) return result;
  const auto& view = *inspection.topology;
  nb::dict data;
  nb::list corners, edges, vertices, boundaries;
  for (const auto& corner : view.corners()) {
    nb::dict item;
    item["vertex"] = corner.vertex;
    item["face"] = corner.face;
    item["previous"] = corner.previous;
    item["next"] = corner.next;
    item["edge"] = corner.edge;
    corners.append(item);
  }
  for (const auto& edge : view.edges()) {
    nb::dict item;
    nb::list endpoints;
    endpoints.append(edge.vertices[0]);
    endpoints.append(edge.vertices[1]);
    item["vertices"] = endpoints;
    item["corners"] = nb::cast(edge.corners);
    edges.append(item);
  }
  for (const auto& vertex : view.vertices()) {
    nb::dict item;
    constexpr const char* kinds[] = {"isolated", "interior", "boundary",
                                     "nonmanifold", "degenerate"};
    item["kind"] = kinds[static_cast<unsigned>(vertex.kind)];
    item["fans"] = nb::cast(vertex.fans);
    vertices.append(item);
  }
  for (const auto& boundary : view.boundaries()) {
    nb::dict item;
    constexpr const char* kinds[] = {"cycle", "chain", "branched",
                                     "degenerate"};
    item["kind"] = kinds[static_cast<unsigned>(boundary.kind)];
    item["edges"] = nb::cast(boundary.edges);
    item["vertices"] = nb::cast(boundary.vertices);
    boundaries.append(item);
  }
  data["corners"] = corners;
  data["edges"] = edges;
  data["vertices"] = vertices;
  data["boundaries"] = boundaries;
  data["face_components"] = nb::cast(view.face_components());
  result["topology"] = data;
  return result;
}
}  // namespace

NB_MODULE(_geometry, module) {
  nb::class_<Snapshot>(module, "Mesh")
      .def(nb::init<>())
      .def_static(
          "from_record",
          [](nb::handle record) {
            return Snapshot(geo::python::from_record(record));
          },
          nb::arg("record"),
          "Copy a versioned flat-buffer record into an immutable native "
          "snapshot.")
      .def("to_record",
           [](const Snapshot& mesh) {
             return geo::python::to_record(mesh.Data());
           })
      .def_prop_ro(
          "vertex_count",
          [](const Snapshot& mesh) { return mesh.Data().positions.size(); })
      .def_prop_ro(
          "face_count",
          [](const Snapshot& mesh) { return mesh.Data().face_count(); })
      .def_prop_ro("corner_count",
                   [](const Snapshot& mesh) {
                     return mesh.Data().corner_vertices.size();
                   })
      .def("inspect_storage",
           [](const Snapshot& mesh) {
             return diagnostics(geo::inspect_storage(mesh.Data()));
           })
      .def("inspect_topology", &topology);
  geo::bindings::RegisterTriangulationBindings(module);
}
