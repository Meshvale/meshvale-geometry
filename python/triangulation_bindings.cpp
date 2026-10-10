// SPDX-License-Identifier: Apache-2.0
#include "triangulation_bindings.h"

#include <meshvale/geometry/editable_mesh.h>
#include <meshvale/geometry/python/record.h>
#include <meshvale/geometry/triangulation.h>
#include <nanobind/stl/string.h>

#include <cstddef>
#include <cstdint>
#include <limits>
#include <stop_token>
#include <string>
#include <utility>

#include "snapshot.h"

namespace meshvale::geometry::bindings {
namespace nb = nanobind;
namespace {

std::uint64_t CheckedUnsigned(nb::handle value, const char* name) {
  if (!PyLong_CheckExact(value.ptr()))
    throw nb::type_error(
        (std::string(name) + " must be a built-in integer").c_str());
  nb::int_ zero(0);
  const int negative = PyObject_RichCompareBool(value.ptr(), zero.ptr(), Py_LT);
  if (negative < 0) throw nb::python_error();
  if (negative)
    throw nb::value_error((std::string(name) + " must be nonnegative").c_str());
  const auto result = PyLong_AsUnsignedLongLong(value.ptr());
  if (PyErr_Occurred()) throw nb::python_error();
  return result;
}

std::size_t CheckedSize(nb::handle value, const char* name) {
  const auto result = CheckedUnsigned(value, name);
  if (result > std::numeric_limits<std::size_t>::max()) {
    PyErr_SetString(PyExc_OverflowError, "integer exceeds native size_t range");
    throw nb::python_error();
  }
  return static_cast<std::size_t>(result);
}

ExecutionContext MakeExecution(nb::handle workers, nb::handle minimum,
                               nb::handle payload) {
  return ExecutionContext(
      {CheckedSize(workers, "worker_budget"),
       CheckedSize(minimum, "minimum_parallel_vertices"),
       CheckedSize(payload, "tracked_payload_budget_bytes")});
}

nb::tuple Convert(nb::object source_owner, nb::handle maximum,
                  nb::handle minimum, const ExecutionContext& execution,
                  nb::object cancellation_owner) {
  // Owners are retained across GIL release; all Python allocation/destruction
  // occurs before release or after reacquisition, including exception
  // unwinding.
  if (!nb::isinstance<Snapshot>(source_owner))
    throw nb::type_error("mesh must be the canonical immutable Mesh");
  const auto& snapshot = nb::cast<const Snapshot&>(source_owner);
  TriangulationOptions options{CheckedUnsigned(maximum, "max_corners_per_face"),
                               CheckedSize(minimum, "minimum_parallel_faces")};
  if (options.max_corners_per_face < 3 || options.max_corners_per_face > 4096)
    throw nb::value_error("max_corners_per_face must be between 3 and 4096");
  const ExecutionContext owned_execution = execution;
  std::stop_token token;
  if (!cancellation_owner.is_none()) {
    if (!nb::isinstance<std::stop_source>(cancellation_owner))
      throw nb::type_error("cancellation must be Cancellation or None");
    token = nb::cast<const std::stop_source&>(cancellation_owner).get_token();
  }
  TriangulationResult result;
  {
    nb::gil_scoped_release release;
    result = Triangulate(snapshot.Data(), options, owned_execution, token);
  }
  const char* status =
      result.status == TriangulationStatus::kAccepted   ? "accepted"
      : result.status == TriangulationStatus::kCanceled ? "canceled"
                                                        : "blocked";
  nb::object candidate = nb::none();
  if (result.mesh) candidate = nb::cast(Snapshot(std::move(*result.mesh)));
  nb::list diagnostics;
  for (const auto& diagnostic : result.diagnostics)
    diagnostics.append(nb::make_tuple(
        diagnostic.code, diagnostic.subject,
        diagnostic.element ? nb::cast(*diagnostic.element) : nb::none()));
  return nb::make_tuple(status, candidate, nb::tuple(diagnostics),
                        python::write_buffer(result.face_sources, "Q"),
                        python::write_buffer(result.corner_sources, "Q"),
                        python::write_buffer(result.face_output_offsets, "Q"),
                        result.workers_used, result.serial_reason,
                        result.peak_tracked_payload_bytes);
}
}  // namespace

void RegisterTriangulationBindings(nb::module_& module) {
  nb::class_<ExecutionContext>(module, "ExecutionContext")
      .def(nb::new_(&MakeExecution), nb::kw_only(),
           nb::arg("worker_budget") = 0,
           nb::arg("minimum_parallel_vertices") = 65536,
           nb::arg("tracked_payload_budget_bytes") = 256 * 1024 * 1024)
      .def_prop_ro("worker_budget", &ExecutionContext::WorkerBudget)
      .def_prop_ro("active_workers", &ExecutionContext::ActiveWorkers)
      .def_prop_ro("peak_workers", &ExecutionContext::PeakWorkers)
      .def_prop_ro("tracked_payload_budget_bytes",
                   &ExecutionContext::TrackedPayloadBudget)
      .def_prop_ro("active_tracked_payload_bytes",
                   &ExecutionContext::ActiveTrackedPayload)
      .def_prop_ro("peak_tracked_payload_bytes",
                   &ExecutionContext::PeakTrackedPayload)
      .def("__copy__", [](const ExecutionContext& context) { return context; });
  nb::class_<std::stop_source>(module, "Cancellation")
      .def(nb::init<>())
      .def("request_stop", &std::stop_source::request_stop)
      .def_prop_ro("stop_requested", &std::stop_source::stop_requested);
  module.def("_triangulate", &Convert, nb::arg("mesh"), nb::kw_only(),
             nb::arg("max_corners_per_face"), nb::arg("minimum_parallel_faces"),
             nb::arg("execution"), nb::arg("cancellation") = nb::none());
}
}  // namespace meshvale::geometry::bindings
