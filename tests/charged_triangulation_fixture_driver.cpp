// SPDX-License-Identifier: Apache-2.0
#include <meshvale/geometry/attributes.h>
#include <meshvale/geometry/charged_triangulation.h>
#include <meshvale/geometry/editable_mesh.h>
#include <meshvale/geometry/execution.h>
#include <meshvale/geometry/materialization.h>
#include <meshvale/geometry/triangulation.h>

#include <cstddef>
#include <cstdint>
#include <exception>
#include <iostream>
#include <span>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <variant>
#include <vector>

#include "charged_triangulation_fixture.h"

namespace {
namespace g = meshvale::geometry;
namespace f = meshvale::geometry::charged_test;
constexpr std::string_view kPositions = "positions",
                           kFaceOffsets = "face_offsets",
                           kCorners = "corner_vertices",
                           kAttributes = "attributes", kDomain = "domain",
                           kName = "name", kSemantic = "semantic", kSet = "set",
                           kComponents = "components", kType = "type",
                           kValues = "values", kOffsets = "offsets",
                           kPresent = "present", kMetadata = "metadata",
                           kSource = "source", kUnchanged = "source_unchanged",
                           kMesh = "mesh", kFaceSources = "face_sources",
                           kCornerSources = "corner_sources",
                           kFaceRanges = "face_output_offsets";
void Key(std::ostream& stream, std::string_view key) {
  stream << '"' << key << "\":";
}
void Hex(std::ostream& stream, std::string_view bytes) {
  stream << '"';
  for (const unsigned char byte : bytes)
    stream << f::kHexDigits[byte >> f::kBitsPerNibble]
           << f::kHexDigits[byte & f::kNibbleMask];
  stream << '"';
}
template <class Range>
void Array(std::ostream& stream, const Range& values) {
  stream << '[';
  for (std::size_t i = 0; i < values.size(); ++i) {
    if (i) stream << ',';
    stream << f::Bits(values[i]);
  }
  stream << ']';
}
std::string Dump(const g::MeshReadView& mesh) {
  std::ostringstream stream;
  stream << '{';
  Key(stream, kPositions);
  stream << '[';
  for (std::size_t row = 0;
       row < mesh.positions.size() / f::kPositionComponents; ++row) {
    if (row) stream << ',';
    Array(stream, mesh.positions.subspan(row * f::kPositionComponents,
                                         f::kPositionComponents));
  }
  stream << "],";
  Key(stream, kFaceOffsets);
  Array(stream, mesh.face_offsets);
  stream << ',';
  Key(stream, kCorners);
  Array(stream, mesh.corner_vertices);
  stream << ',';
  Key(stream, kAttributes);
  stream << '[';
  for (std::size_t ordinal = 0; ordinal < mesh.attributes.size(); ++ordinal) {
    if (ordinal) stream << ',';
    const auto& a = mesh.attributes[ordinal];
    stream << '{';
    Key(stream, kDomain);
    stream << static_cast<int>(a.domain);
    stream << ',';
    Key(stream, kName);
    Hex(stream, a.name);
    stream << ',';
    Key(stream, kSemantic);
    Hex(stream, a.semantic);
    stream << ',';
    Key(stream, kSet);
    if (a.set_index)
      stream << *a.set_index;
    else
      stream << "null";
    stream << ',';
    Key(stream, kComponents);
    stream << a.components;
    stream << ',';
    Key(stream, kType);
    stream << a.values.index();
    stream << ',';
    Key(stream, kValues);
    std::visit([&](const auto& values) { Array(stream, values); }, a.values);
    stream << ',';
    Key(stream, kOffsets);
    if (a.offsets)
      Array(stream, *a.offsets);
    else
      stream << "null";
    stream << ',';
    Key(stream, kPresent);
    if (a.present)
      Array(stream, *a.present);
    else
      stream << "null";
    stream << ',';
    Key(stream, kMetadata);
    stream << '[';
    for (std::size_t i = 0; i < a.metadata.size(); ++i) {
      if (i) stream << ',';
      stream << '[';
      Hex(stream, a.metadata[i].key);
      stream << ',';
      Hex(stream, a.metadata[i].value);
      stream << ']';
    }
    stream << "]}";
  }
  stream << "]}";
  return stream.str();
}
std::string LegacyDump(f::Fixture& fixture) {
  const auto input = fixture.Input(false);
  std::vector<g::AttributeReadView> attributes;
  for (const auto& a : input.attributes) {
    g::ScalarReadView values = std::visit(
        [](const auto& value) -> g::ScalarReadView {
          using V = std::remove_cvref_t<decltype(value)>;
          if constexpr (std::is_same_v<V, std::monostate> ||
                        std::is_same_v<V, std::span<const std::int8_t>> ||
                        std::is_same_v<V, std::span<const std::int16_t>>)
            throw std::runtime_error(
                "fixture uses only seven canonical alternatives");
          else
            return value;
        },
        a.values);
    attributes.push_back({a.domain, a.name, a.semantic, a.set_index,
                          a.components, values, a.offsets, a.present,
                          a.metadata});
  }
  const auto topology = std::get<g::PolygonTopologyInput>(input.topology);
  return Dump({std::get<std::span<const double>>(input.positions),
               topology.face_offsets, topology.corner_vertices, attributes});
}
}  // namespace
int main(int argc, char** argv) {
  try {
    f::Require(argc == 3, "driver requires closed route/worker arguments");
    const std::string_view route(argv[1]), workers(argv[2]);
    f::Require(route == "legacy" || route == "retained", "driver route");
    f::Require(workers == "1" || workers == "4", "driver workers");
    f::Fixture fixture;
    const auto legacy_before = LegacyDump(fixture);
    g::ExecutionContext context(
        {workers == "1" ? 1 : f::kWorkers, 0, f::kLedgerBytes});
    const auto source = fixture.Capture(context, route == "retained");
    const auto before = Dump(source.View());
    g::TriangulationOptions options;
    options.minimum_parallel_faces = 0;
    const auto result =
        route == "legacy"
            ? g::TriangulateCharged(fixture.legacy, options, f::Limits(),
                                    f::Work(), context)
            : g::TriangulateCharged(source, options, f::Limits(), f::Work());
    f::Require(result.status == g::ChargedTriangulationStatus::kAccepted &&
                   result.triangulation,
               "actual driver acceptance");
    f::Check(source.View(), *result.triangulation);
    std::cout << '{';
    Key(std::cout, kSource);
    std::cout << before << ',';
    Key(std::cout, kUnchanged);
    std::cout << ((before == Dump(source.View()) &&
                   legacy_before == LegacyDump(fixture))
                      ? "true"
                      : "false")
              << ',';
    Key(std::cout, kMesh);
    std::cout << Dump(result.triangulation->Mesh().View()) << ',';
    Key(std::cout, kFaceSources);
    Array(std::cout, result.triangulation->FaceSources());
    std::cout << ',';
    Key(std::cout, kCornerSources);
    Array(std::cout, result.triangulation->CornerSources());
    std::cout << ',';
    Key(std::cout, kFaceRanges);
    Array(std::cout, result.triangulation->FaceOutputOffsets());
    std::cout << "}\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
