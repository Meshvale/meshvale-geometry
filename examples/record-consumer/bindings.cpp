// SPDX-License-Identifier: Apache-2.0
#include <meshvale/geometry/python/record.h>
#include <meshvale/geometry/topology.h>

// This independent extension consumes installed headers and standard Python
// records. It never imports or registers Geometry's Mesh Python type.
NB_MODULE(meshvale_record_consumer, module) {
    namespace geo = meshvale::geometry;
    namespace nb = nanobind;
    module.def("inspect_record", [](nb::handle source) {
        const auto mesh = geo::python::from_record(source);
        const auto inspection = geo::inspect_topology(mesh);
        nb::dict result;
        result["record"] = geo::python::to_record(mesh);
        result["face_count"] = mesh.face_count();
        result["corner_count"] = mesh.corner_vertices.size();
        result["edge_count"] = inspection.topology ? nb::cast(inspection.topology->edges().size()) : nb::none();
        result["storage_issues"] = geo::inspect_storage(mesh).size();
        return result;
    });
}
