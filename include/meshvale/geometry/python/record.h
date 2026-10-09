// SPDX-License-Identifier: Apache-2.0
#ifndef MESHVALE_GEOMETRY_PYTHON_RECORD_H_
#define MESHVALE_GEOMETRY_PYTHON_RECORD_H_

// Optional adapter: consumers must explicitly provide Python and nanobind.
// Exchanges owned Python records, never registered native Mesh objects.
#include <meshvale/geometry/mesh.h>
#include <nanobind/nanobind.h>
#include <nanobind/stl/string.h>

#include <cstring>
#include <cstdint>
#include <initializer_list>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace meshvale::geometry::python {
namespace nb = nanobind;
inline constexpr auto record_schema = "meshvale.mesh/1";

inline nb::dict exact_dict(nb::handle value, std::initializer_list<const char*> fields) {
    if (!PyDict_Check(value.ptr())) throw nb::type_error("record must be a dictionary");
    auto result = nb::borrow<nb::dict>(value);
    if (result.size() != fields.size()) throw nb::value_error("record has missing or extra keys");
    for (const char* key : fields)
        if (!result.contains(key)) throw nb::value_error("record has missing or extra keys");
    return result;
}

inline std::string text(nb::handle value) {
    if (!PyUnicode_Check(value.ptr())) throw nb::type_error("record text must be a string");
    return nb::cast<std::string>(value);
}

inline std::uint32_t uint32(nb::handle value) {
    if (!PyLong_Check(value.ptr()) || PyBool_Check(value.ptr()))
        throw nb::type_error("record uint32 must be an integer, not a boolean");
    const auto number = PyLong_AsUnsignedLongLong(value.ptr());
    if (PyErr_Occurred()) throw nb::python_error();
    if (number > std::numeric_limits<std::uint32_t>::max())
        throw nb::value_error("record integer exceeds uint32 range");
    return static_cast<std::uint32_t>(number);
}

class Buffer {
    Py_buffer value_{};
public:
    explicit Buffer(nb::handle source) {
        if (PyObject_GetBuffer(source.ptr(), &value_, PyBUF_FORMAT | PyBUF_STRIDES) != 0)
            throw nb::python_error();
    }
    Buffer(const Buffer&) = delete;
    Buffer& operator=(const Buffer&) = delete;
    ~Buffer() { PyBuffer_Release(&value_); }
    const Py_buffer& get() const { return value_; }
};

template<typename T> inline std::size_t buffer_count(const Py_buffer& buffer, std::string_view formats) {
    std::string_view format = buffer.format ? buffer.format : "";
    if (!format.empty() && (format.front() == '@' || format.front() == '=')) format.remove_prefix(1);
    if (buffer.ndim != 1 || !PyBuffer_IsContiguous(&buffer, 'C') || buffer.suboffsets ||
        buffer.itemsize != static_cast<Py_ssize_t>(sizeof(T)) || format.size() != 1 ||
        formats.find(format.front()) == std::string_view::npos || buffer.len < 0 || !buffer.shape ||
        buffer.shape[0] < 0 || buffer.len % static_cast<Py_ssize_t>(sizeof(T)) != 0 ||
        buffer.shape[0] != buffer.len / static_cast<Py_ssize_t>(sizeof(T)))
        throw nb::value_error("buffer must be flat, contiguous and have the required native scalar format/width");
    return static_cast<std::size_t>(buffer.len) / sizeof(T);
}

template<typename T> inline std::vector<T> read_buffer(nb::handle source, std::string_view formats) {
    Buffer owner(source);
    const auto& buffer = owner.get();
    std::vector<T> result(buffer_count<T>(buffer, formats));
    if (!result.empty()) std::memcpy(result.data(), buffer.buf, static_cast<std::size_t>(buffer.len));
    return result;
}

template<typename T> inline nb::object write_buffer(const std::vector<T>& values, const char* format) {
    if (values.size() > static_cast<std::size_t>(PY_SSIZE_T_MAX) / sizeof(T))
        throw nb::value_error("buffer exceeds Python size range");
    // bytes owns a copy before the memoryview is made; empty vectors are supported.
    auto data = nb::bytes(values.empty() ? "" : static_cast<const void*>(values.data()), values.size() * sizeof(T));
    auto view = nb::steal<nb::object>(PyMemoryView_FromObject(data.ptr()));
    if (!view.is_valid()) throw nb::python_error();
    return view.attr("cast")(format);
}

inline Attribute read_attribute(nb::handle source) {
    const auto record = exact_dict(source, {"domain","name","semantic","set_index","components",
        "scalar_type","values","offsets","present","metadata"});
    Attribute result;
    const auto domain = text(record["domain"]);
    if (domain == "vertex") result.domain = AttributeDomain::vertex;
    else if (domain == "face") result.domain = AttributeDomain::face;
    else if (domain == "corner") result.domain = AttributeDomain::corner;
    else throw nb::value_error("attribute domain must be vertex, face or corner");
    result.name = text(record["name"]);
    result.semantic = text(record["semantic"]);
    if (!record["set_index"].is_none()) result.set_index = uint32(record["set_index"]);
    result.components = uint32(record["components"]);
    const auto type = text(record["scalar_type"]);
    const auto values = record["values"];
    if (type == "float32") result.values = read_buffer<float>(values, "f");
    else if (type == "float64") result.values = read_buffer<double>(values, "d");
    else if (type == "int32") result.values = read_buffer<std::int32_t>(values, "il");
    else if (type == "uint8") result.values = read_buffer<std::uint8_t>(values, "B");
    else if (type == "uint16") result.values = read_buffer<std::uint16_t>(values, "H");
    else if (type == "uint32") result.values = read_buffer<std::uint32_t>(values, "IL");
    else if (type == "uint64") result.values = read_buffer<std::uint64_t>(values, "QL");
    else throw nb::value_error("unsupported attribute scalar_type");
    if (!record["offsets"].is_none()) result.offsets = read_buffer<index_t>(record["offsets"], "QL");
    if (!record["present"].is_none()) result.present = read_buffer<std::uint8_t>(record["present"], "B");
    if (!PyDict_Check(record["metadata"].ptr())) throw nb::type_error("metadata must be a dictionary");
    for (auto item : nb::borrow<nb::dict>(record["metadata"]))
        result.metadata.emplace(text(item.first), text(item.second));
    return result;
}

inline Mesh from_record(nb::handle source) {
    const auto record = exact_dict(source, {"schema","positions","face_offsets","corner_vertices","attributes"});
    if (text(record["schema"]) != record_schema) throw nb::value_error("unsupported mesh record schema");
    Mesh result;
    {
        Buffer coordinates(record["positions"]);
        const auto scalar_count = buffer_count<double>(coordinates.get(), "d");
        if (scalar_count % 3 != 0) throw nb::value_error("positions must contain xyz triples");
        result.positions.resize(scalar_count / 3);
        const auto* bytes = static_cast<const char*>(coordinates.get().buf);
        for (std::size_t i = 0; i < result.positions.size(); ++i)
            std::memcpy(result.positions[i].data(), bytes + i*3*sizeof(double), 3*sizeof(double));
    }
    result.face_offsets = read_buffer<index_t>(record["face_offsets"], "QL");
    result.corner_vertices = read_buffer<index_t>(record["corner_vertices"], "QL");
    if (!PyList_Check(record["attributes"].ptr())) throw nb::type_error("attributes must be a list");
    for (auto item : nb::borrow<nb::list>(record["attributes"])) result.attributes.push_back(read_attribute(item));
    return result;
}

inline nb::dict to_record(const Mesh& mesh) {
    nb::dict result;
    result["schema"] = record_schema;
    if (mesh.positions.size() > static_cast<std::size_t>(PY_SSIZE_T_MAX)/(3*sizeof(double)))
        throw nb::value_error("positions exceed Python size range");
    auto coordinates = nb::steal<nb::bytes>(PyBytes_FromStringAndSize(nullptr,
        static_cast<Py_ssize_t>(mesh.positions.size()*3*sizeof(double))));
    if (!coordinates.is_valid()) throw nb::python_error();
    auto* bytes = PyBytes_AS_STRING(coordinates.ptr());
    for (std::size_t i = 0; i < mesh.positions.size(); ++i)
        std::memcpy(bytes + i*3*sizeof(double), mesh.positions[i].data(), 3*sizeof(double));
    auto view = nb::steal<nb::object>(PyMemoryView_FromObject(coordinates.ptr()));
    if (!view.is_valid()) throw nb::python_error();
    result["positions"] = view.attr("cast")("d");
    result["face_offsets"] = write_buffer(mesh.face_offsets, "Q");
    result["corner_vertices"] = write_buffer(mesh.corner_vertices, "Q");
    nb::list attributes;
    for (const auto& attribute : mesh.attributes) {
        nb::dict row;
        switch (attribute.domain) {
            case AttributeDomain::vertex: row["domain"] = "vertex"; break;
            case AttributeDomain::face: row["domain"] = "face"; break;
            case AttributeDomain::corner: row["domain"] = "corner"; break;
            default: throw nb::value_error("cannot export an invalid attribute domain");
        }
        row["name"] = nb::cast(attribute.name);
        row["semantic"] = nb::cast(attribute.semantic);
        row["set_index"] = attribute.set_index ? nb::cast(*attribute.set_index) : nb::none();
        row["components"] = attribute.components;
        constexpr const char* types[] = {"float32","float64","int32","uint8","uint16","uint32","uint64"};
        constexpr const char* formats[] = {"f","d","i","B","H","I","Q"};
        row["scalar_type"] = types[attribute.values.index()];
        row["values"] = std::visit([&](const auto& values) { return write_buffer(values, formats[attribute.values.index()]); }, attribute.values);
        row["offsets"] = attribute.offsets ? write_buffer(*attribute.offsets,"Q") : nb::none();
        row["present"] = attribute.present ? write_buffer(*attribute.present,"B") : nb::none();
        nb::dict metadata;
        for (const auto& [key,value] : attribute.metadata) metadata[nb::cast(key)] = nb::cast(value);
        row["metadata"] = metadata;
        attributes.append(row);
    }
    result["attributes"] = attributes;
    return result;
}
} // namespace meshvale::geometry::python

#endif  // MESHVALE_GEOMETRY_PYTHON_RECORD_H_
