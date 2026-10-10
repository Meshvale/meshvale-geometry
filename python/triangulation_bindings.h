// SPDX-License-Identifier: Apache-2.0
#ifndef MESHVALE_GEOMETRY_PYTHON_TRIANGULATION_BINDINGS_H_
#define MESHVALE_GEOMETRY_PYTHON_TRIANGULATION_BINDINGS_H_

#include <nanobind/nanobind.h>

namespace meshvale::geometry::bindings {
void RegisterTriangulationBindings(nanobind::module_& module);
}  // namespace meshvale::geometry::bindings
#endif  // MESHVALE_GEOMETRY_PYTHON_TRIANGULATION_BINDINGS_H_
