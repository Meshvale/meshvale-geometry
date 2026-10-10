// SPDX-License-Identifier: Apache-2.0
#include <meshvale/geometry/triangulation.h>

#include <bit>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace {
using namespace meshvale::geometry;
template <class T>
std::uint64_t Bits(T value) {
  if constexpr (std::is_same_v<T, float>)
    return std::bit_cast<std::uint32_t>(value);
  else if constexpr (std::is_same_v<T, double>)
    return std::bit_cast<std::uint64_t>(value);
  else if constexpr (std::is_same_v<T, std::int32_t>)
    return std::bit_cast<std::uint32_t>(value);
  else
    return value;
}
template <class T>
T FromBits(std::uint64_t value) {
  if constexpr (std::is_same_v<T, float>)
    return std::bit_cast<float>(static_cast<std::uint32_t>(value));
  else if constexpr (std::is_same_v<T, double>)
    return std::bit_cast<double>(value);
  else if constexpr (std::is_same_v<T, std::int32_t>)
    return std::bit_cast<std::int32_t>(static_cast<std::uint32_t>(value));
  else
    return static_cast<T>(value);
}
template <class T>
std::vector<T> ReadValues(std::istream& stream) {
  std::size_t count;
  stream >> count;
  if (count > 1000000) throw std::runtime_error("fixture input value bound");
  std::vector<T> values;
  for (std::size_t i = 0; i < count; ++i) {
    std::uint64_t value;
    stream >> value;
    values.push_back(FromBits<T>(value));
  }
  return values;
}
Mesh Read(std::istream& stream) {
  Mesh mesh;
  std::size_t positions, faces;
  stream >> positions >> faces;
  if (positions > 100000 || faces > 10000)
    throw std::runtime_error("fixture input size bound");
  for (std::size_t i = 0; i < positions; ++i) {
    std::array<double, 3> point;
    for (auto& coordinate : point) {
      std::uint64_t value;
      stream >> value;
      coordinate = std::bit_cast<double>(value);
    }
    mesh.positions.Append(point);
  }
  for (std::size_t i = 0; i < faces; ++i) {
    const auto corners = ReadValues<index_t>(stream);
    mesh.corner_vertices.insert(mesh.corner_vertices.end(), corners.begin(),
                                corners.end());
    mesh.face_offsets.push_back(mesh.corner_vertices.size());
  }
  std::size_t attributes;
  stream >> attributes;
  if (attributes > 1000) throw std::runtime_error("fixture attribute bound");
  for (std::size_t i = 0; i < attributes; ++i) {
    Attribute attribute;
    int domain, type;
    std::int64_t set;
    stream >> domain >> std::quoted(attribute.name) >>
        std::quoted(attribute.semantic) >> set >> attribute.components >> type;
    attribute.domain = static_cast<AttributeDomain>(domain);
    if (set >= 0) attribute.set_index = static_cast<std::uint32_t>(set);
    switch (type) {
      case 0:
        attribute.values = ReadValues<float>(stream);
        break;
      case 1:
        attribute.values = ReadValues<double>(stream);
        break;
      case 2:
        attribute.values = ReadValues<std::int32_t>(stream);
        break;
      case 3:
        attribute.values = ReadValues<std::uint8_t>(stream);
        break;
      case 4:
        attribute.values = ReadValues<std::uint16_t>(stream);
        break;
      case 5:
        attribute.values = ReadValues<std::uint32_t>(stream);
        break;
      case 6:
        attribute.values = ReadValues<std::uint64_t>(stream);
        break;
      default:
        throw std::runtime_error("unknown scalar");
    }
    int present;
    stream >> present;
    if (present) attribute.offsets = ReadValues<index_t>(stream);
    stream >> present;
    if (present) attribute.present = ReadValues<std::uint8_t>(stream);
    std::size_t metadata;
    stream >> metadata;
    for (std::size_t j = 0; j < metadata; ++j) {
      std::string key, value;
      stream >> std::quoted(key) >> std::quoted(value);
      attribute.metadata.emplace(key, value);
    }
    mesh.attributes.push_back(std::move(attribute));
  }
  if (!stream) throw std::runtime_error("truncated fixture input");
  return mesh;
}
std::string Quoted(const std::string& value) {
  std::ostringstream output;
  output << '"';
  for (const unsigned char c : value) {
    if (c == '"' || c == '\\')
      output << '\\' << c;
    else if (c < 32)
      output << "\\u" << std::hex << std::setw(4) << std::setfill('0')
             << static_cast<unsigned>(c) << std::dec;
    else
      output << c;
  }
  output << '"';
  return output.str();
}
template <class T>
void WriteArray(std::ostream& stream, const std::vector<T>& values) {
  stream << '[';
  for (std::size_t i = 0; i < values.size(); ++i) {
    if (i) stream << ',';
    stream << Bits(values[i]);
  }
  stream << ']';
}
std::string Dump(const Mesh& mesh) {
  std::ostringstream stream;
  stream << "{\"positions\":[";
  for (std::size_t i = 0; i < mesh.positions.size(); ++i) {
    if (i) stream << ',';
    stream << '[';
    for (std::size_t a = 0; a < 3; ++a) {
      if (a) stream << ',';
      stream << Bits(mesh.positions.Get(i)[a]);
    }
    stream << ']';
  }
  stream << "],\"face_offsets\":";
  WriteArray(stream, mesh.face_offsets);
  stream << ",\"corner_vertices\":";
  WriteArray(stream, mesh.corner_vertices);
  stream << ",\"attributes\":[";
  for (std::size_t i = 0; i < mesh.attributes.size(); ++i) {
    if (i) stream << ',';
    const auto& a = mesh.attributes[i];
    stream << "{\"domain\":" << static_cast<int>(a.domain)
           << ",\"name\":" << Quoted(a.name)
           << ",\"semantic\":" << Quoted(a.semantic) << ",\"set\":";
    if (a.set_index)
      stream << *a.set_index;
    else
      stream << "null";
    stream << ",\"components\":" << a.components
           << ",\"type\":" << a.values.index() << ",\"values\":";
    std::visit([&](const auto& values) { WriteArray(stream, values); },
               a.values);
    stream << ",\"offsets\":";
    if (a.offsets)
      WriteArray(stream, *a.offsets);
    else
      stream << "null";
    stream << ",\"present\":";
    if (a.present)
      WriteArray(stream, *a.present);
    else
      stream << "null";
    stream << ",\"metadata\":{";
    std::size_t entry = 0;
    for (const auto& [key, value] : a.metadata) {
      if (entry++) stream << ',';
      stream << Quoted(key) << ':' << Quoted(value);
    }
    stream << "}}";
  }
  stream << "]}";
  return stream.str();
}
}  // namespace
int main(int argc, char** argv) {
  try {
    const auto source = Read(std::cin);
    const auto before = Dump(source);
    const auto budget =
        argc > 1 ? static_cast<std::size_t>(std::stoull(argv[1])) : 1;
    ExecutionContext execution({budget, 0});
    TriangulationOptions options;
    options.minimum_parallel_faces = 0;
    std::int64_t cancel;
    std::cin >> cancel;
    std::stop_source stop;
    if (cancel == 0) stop.request_stop();
    const auto result =
        Triangulate(source, options, execution, stop.get_token());
    std::cout << "{\"accepted\":" << (result.mesh ? "true" : "false")
              << ",\"source\":" << before << ",\"source_unchanged\":"
              << (before == Dump(source) ? "true" : "false")
              << ",\"mesh\":" << (result.mesh ? Dump(*result.mesh) : "null")
              << ",\"face_sources\":";
    WriteArray(std::cout, result.face_sources);
    std::cout << ",\"corner_sources\":";
    WriteArray(std::cout, result.corner_sources);
    std::cout << ",\"face_output_offsets\":";
    WriteArray(std::cout, result.face_output_offsets);
    std::cout << ",\"workers_used\":" << result.workers_used;
    std::cout << ",\"peak_tracked_payload_bytes\":"
              << result.peak_tracked_payload_bytes;
    if (!result.diagnostics.empty()) {
      std::cout << ",\"code\":" << Quoted(result.diagnostics[0].code);
      std::cout << ",\"element\":";
      if (result.diagnostics[0].element)
        std::cout << *result.diagnostics[0].element;
      else
        std::cout << "null";
    }
    std::cout << "}\n";
    if (execution.ActiveWorkers() || execution.ActiveTrackedPayload())
      throw std::runtime_error("reservation leak");
    return result.mesh ? 0 : 2;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 3;
  }
}
