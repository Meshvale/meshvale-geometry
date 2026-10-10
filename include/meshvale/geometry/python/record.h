// SPDX-License-Identifier: Apache-2.0
#ifndef MESHVALE_GEOMETRY_PYTHON_RECORD_H_
#define MESHVALE_GEOMETRY_PYTHON_RECORD_H_

// Optional adapter: link a per-extension Python record implementation target.
// Calls require the GIL and ordinary GIL-enabled Python/nanobind.
// Exchanges owned Python records, never registered native Mesh objects.
#include <meshvale/geometry/mesh.h>
#include <nanobind/nanobind.h>

#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <string>
#include <vector>

namespace meshvale::geometry::python {
namespace nb = nanobind;
inline constexpr auto record_schema = "meshvale.mesh/1";

nb::dict exact_dict(nb::handle value,
                    std::initializer_list<const char*> fields);

std::string text(nb::handle value);

std::uint32_t uint32(nb::handle value);

// Copies checked byte storage into an immutable Python-owned buffer.
// data must address count * scalar_size readable bytes when count is nonzero.
nb::object WriteBufferData(const void* data, std::size_t count,
                           std::size_t scalar_size, const char* format);

template <typename T>
inline nb::object write_buffer(const std::vector<T>& values,
                               const char* format) {
  return WriteBufferData(values.empty() ? nullptr : values.data(),
                         values.size(), sizeof(T), format);
}

Attribute read_attribute(nb::handle source);

Mesh from_record(nb::handle source);

nb::dict to_record(const Mesh& mesh);
}  // namespace meshvale::geometry::python

#endif  // MESHVALE_GEOMETRY_PYTHON_RECORD_H_
