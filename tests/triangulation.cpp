// SPDX-License-Identifier: Apache-2.0
#include <meshvale/geometry/triangulation.h>

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <future>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <thread>
#include <type_traits>
#include <vector>

#include "execution_internal.h"

namespace {
using namespace meshvale::geometry;
void Require(bool value, const char* message) {
  if (!value) throw std::runtime_error(message);
}
template <class T>
bool BitsEqual(const ScalarBuffer<T>& a, const ScalarBuffer<T>& b) {
  return a.size() == b.size() &&
         (a.empty() ||
          std::memcmp(a.data(), b.data(), a.size() * sizeof(T)) == 0);
}
bool BitsEqual(const PositionBuffer& a, const PositionBuffer& b) {
  if (a.size() != b.size()) return false;
  for (std::size_t row = 0; row < a.size(); ++row) {
    const auto first = a.Get(row), second = b.Get(row);
    if (std::memcmp(first.data(), second.data(), 3 * sizeof(double)) != 0)
      return false;
  }
  return true;
}
bool SameAttribute(const Attribute& a, const Attribute& b) {
  if (a.domain != b.domain || a.name != b.name || a.semantic != b.semantic ||
      a.set_index != b.set_index || a.components != b.components ||
      a.offsets != b.offsets || a.present != b.present ||
      a.metadata != b.metadata || a.values.index() != b.values.index())
    return false;
  return std::visit(
      [&](const auto& values) {
        return BitsEqual(values,
                         std::get<std::decay_t<decltype(values)>>(b.values));
      },
      a.values);
}
bool SameMesh(const Mesh& a, const Mesh& b) {
  if (!BitsEqual(a.positions, b.positions) ||
      a.face_offsets != b.face_offsets ||
      a.corner_vertices != b.corner_vertices ||
      a.attributes.size() != b.attributes.size())
    return false;
  for (std::size_t i = 0; i < a.attributes.size(); ++i)
    if (!SameAttribute(a.attributes[i], b.attributes[i])) return false;
  return true;
}
template <class T>
Attribute MakeAttribute(AttributeDomain domain, std::size_t rows, bool ragged,
                        std::size_t kind) {
  Attribute a;
  a.domain = domain;
  a.name = "custom_" + std::to_string(kind) + "_" + std::to_string(ragged);
  a.semantic = "CUSTOM";
  a.set_index = static_cast<std::uint32_t>(kind + 1);
  a.components = 2;
  a.metadata = {{"authoring", "original"}, {"pair", "unmodified"}};
  if (ragged) a.offsets = std::vector<index_t>{0};
  a.present = std::vector<std::uint8_t>{};
  std::vector<T> values;
  for (std::size_t row = 0; row < rows; ++row) {
    const auto width = ragged ? row % 4 * 2 : 2;
    for (std::size_t scalar = 0; scalar < width; ++scalar)
      values.push_back(static_cast<T>(row * 7 + scalar));
    if (a.offsets) a.offsets->push_back(values.size());
    a.present->push_back(static_cast<std::uint8_t>(row % 3 != 1));
  }
  if (!values.empty()) {
    if constexpr (std::is_same_v<T, float>)
      values[0] = std::bit_cast<float>(std::uint32_t{0x7fc01234});
    if constexpr (std::is_same_v<T, double>)
      values[0] = std::bit_cast<double>(std::uint64_t{0x7ff8000000001234});
  }
  if (values.size() > 3) {
    if constexpr (std::is_same_v<T, float>)
      values[3] = std::bit_cast<float>(std::uint32_t{0x80000000});
    if constexpr (std::is_same_v<T, double>)
      values[3] = std::bit_cast<double>(std::uint64_t{0x8000000000000000});
    if constexpr (std::is_same_v<T, std::int32_t>) values[3] = -7;
    if constexpr (std::is_same_v<T, std::uint64_t>)
      values[3] = std::uint64_t{1} << 63;
  }
  a.values = std::move(values);
  return a;
}
void AddAttributes(Mesh& mesh) {
  for (auto domain : {AttributeDomain::vertex, AttributeDomain::face,
                      AttributeDomain::corner}) {
    const auto rows = static_cast<std::size_t>(mesh.row_count(domain));
    for (const bool ragged : {false, true}) {
      mesh.attributes.push_back(MakeAttribute<float>(domain, rows, ragged, 0));
      mesh.attributes.push_back(MakeAttribute<double>(domain, rows, ragged, 1));
      mesh.attributes.push_back(
          MakeAttribute<std::int32_t>(domain, rows, ragged, 2));
      mesh.attributes.push_back(
          MakeAttribute<std::uint8_t>(domain, rows, ragged, 3));
      mesh.attributes.push_back(
          MakeAttribute<std::uint16_t>(domain, rows, ragged, 4));
      mesh.attributes.push_back(
          MakeAttribute<std::uint32_t>(domain, rows, ragged, 5));
      mesh.attributes.push_back(
          MakeAttribute<std::uint64_t>(domain, rows, ragged, 6));
    }
  }
  for (std::uint32_t set : {0u, 1u, 3u}) {
    auto uv = MakeAttribute<float>(AttributeDomain::corner,
                                   mesh.corner_vertices.size(), false, set);
    uv.name = "uv_" + std::to_string(set);
    uv.semantic = "TEXCOORD";
    uv.set_index = set;
    uv.present.reset();
    mesh.attributes.push_back(std::move(uv));
  }
}
Mesh Mixed(std::size_t copies = 1) {
  Mesh mesh;
  mesh.positions = {{0, 0, 0}, {4, 0, 0}, {4, 4, 0}, {0, 4, 0},
                    {2, 1, 0}, {8, 0, 0}, {8, 4, 0}};
  for (std::size_t copy = 0; copy < copies; ++copy)
    for (const auto& face : std::vector<std::vector<index_t>>{
             {0, 1, 4}, {1, 5, 6, 2}, {0, 1, 2, 4, 3}}) {
      mesh.corner_vertices.insert(mesh.corner_vertices.end(), face.begin(),
                                  face.end());
      mesh.face_offsets.push_back(mesh.corner_vertices.size());
    }
  AddAttributes(mesh);
  return mesh;
}
void CheckMapsAndRows(const Mesh& source, const TriangulationResult& result) {
  Require(result.status == TriangulationStatus::kAccepted &&
              result.mesh.has_value(),
          "conversion not accepted");
  const auto& output = *result.mesh;
  Require(inspect_storage(output).empty(), "candidate storage invalid");
  Require(BitsEqual(source.positions, output.positions),
          "position bytes changed");
  Require(result.face_output_offsets.size() == source.face_count() + 1 &&
              result.face_output_offsets.front() == 0 &&
              result.face_output_offsets.back() == output.face_count(),
          "face output ranges incomplete");
  Require(result.face_sources.size() == output.face_count() &&
              result.corner_sources.size() == output.corner_vertices.size(),
          "output map cardinality");
  for (std::size_t face = 0; face < source.face_count(); ++face) {
    Require(result.face_output_offsets[face + 1] -
                    result.face_output_offsets[face] ==
                source.face_offsets[face + 1] - source.face_offsets[face] - 2,
            "face output triangle count");
    for (auto f = result.face_output_offsets[face];
         f < result.face_output_offsets[face + 1]; ++f) {
      Require(result.face_sources[f] == face, "source face map wrong");
      for (std::size_t c = 0; c < 3; ++c) {
        const auto output_corner = f * 3 + c,
                   source_corner = result.corner_sources[output_corner];
        Require(source_corner >= source.face_offsets[face] &&
                    source_corner < source.face_offsets[face + 1],
                "source corner maps outside face");
        Require(output.corner_vertices[output_corner] ==
                    source.corner_vertices[source_corner],
                "source vertex map changed");
      }
    }
  }
  for (std::size_t channel = 0; channel < source.attributes.size(); ++channel) {
    const auto& a = source.attributes[channel];
    const auto& b = output.attributes[channel];
    Require(a.domain == b.domain && a.name == b.name &&
                a.semantic == b.semantic && a.set_index == b.set_index &&
                a.components == b.components && a.metadata == b.metadata &&
                a.values.index() == b.values.index(),
            "channel descriptor changed");
    Require(a.offsets.has_value() == b.offsets.has_value() &&
                a.present.has_value() == b.present.has_value(),
            "channel shape/presence kind changed");
    const auto rows = static_cast<std::size_t>(output.row_count(b.domain));
    std::visit(
        [&](const auto& values) {
          const auto& copied =
              std::get<std::decay_t<decltype(values)>>(b.values);
          for (std::size_t row = 0; row < rows; ++row) {
            const auto source_row = a.domain == AttributeDomain::vertex ? row
                                    : a.domain == AttributeDomain::face
                                        ? result.face_sources[row]
                                        : result.corner_sources[row];
            const auto first = a.offsets ? (*a.offsets)[source_row]
                                         : source_row * a.components;
            const auto last =
                a.offsets ? (*a.offsets)[source_row + 1] : first + a.components;
            const auto out_first =
                b.offsets ? (*b.offsets)[row] : row * b.components;
            const auto out_last =
                b.offsets ? (*b.offsets)[row + 1] : out_first + b.components;
            Require(last - first == out_last - out_first,
                    "ragged row width changed");
            for (auto scalar = index_t{0}; scalar < last - first; ++scalar)
              Require(std::memcmp(&values[first + scalar],
                                  &copied[out_first + scalar],
                                  sizeof(typename std::decay_t<
                                         decltype(values)>::value_type)) == 0,
                      "scalar backing bits changed");
            if (a.present)
              Require((*a.present)[source_row] == (*b.present)[row],
                      "missingness changed");
          }
        },
        a.values);
  }
}
void CheckNoPartial(const TriangulationResult& result) {
  Require(!result.mesh && result.face_sources.empty() &&
              result.corner_sources.empty() &&
              result.face_output_offsets.empty(),
          "failed conversion exposed partial result");
}
void NativeCases() {
  ExecutionContext serial({1, 0});
  auto mesh = Mixed();
  const auto before = mesh;
  auto baseline = Triangulate(mesh, {}, serial);
  CheckMapsAndRows(mesh, baseline);
  Require(SameMesh(mesh, before), "source changed");
  for (const bool reversed : {false, true})
    for (const int plane : {0, 1, 2, 3}) {
      auto rotated = mesh;
      for (std::size_t row = 0; row < rotated.positions.size(); ++row) {
        auto p = rotated.positions.Get(row);
        const auto x = p[0], y = p[1];
        if (plane == 1) p = {0, x, y};
        if (plane == 2) p = {x, 0, y};
        if (plane == 3) p = {2 * x + y, -x + 3 * y, x - 2 * y};
        rotated.positions.Set(row, p);
      }
      if (reversed)
        for (std::size_t f = 0; f < rotated.face_count(); ++f)
          std::reverse(
              rotated.corner_vertices.begin() +
                  static_cast<std::ptrdiff_t>(rotated.face_offsets[f]),
              rotated.corner_vertices.begin() +
                  static_cast<std::ptrdiff_t>(rotated.face_offsets[f + 1]));
      CheckMapsAndRows(rotated, Triangulate(rotated, {}, serial));
    }
  Mesh tiny;
  tiny.positions = {{0, 0, 0},
                    {std::numeric_limits<double>::denorm_min(), 0, 0},
                    {0, std::numeric_limits<double>::denorm_min(), 0}};
  tiny.face_offsets = {0, 3};
  tiny.corner_vertices = {0, 1, 2};
  CheckMapsAndRows(tiny, Triangulate(tiny, {}, serial));
  tiny.positions = {{0, 0, 0}, {1, 1, 0}, {2, std::nextafter(2.0, 3.0), 0}};
  CheckMapsAndRows(tiny, Triangulate(tiny, {}, serial));
  Mesh collinear;
  collinear.positions = {{0, 0, 0}, {1, 0, 0}, {2, 0, 0},
                         {4, 0, 0}, {4, 4, 0}, {0, 4, 0}};
  collinear.face_offsets = {0, 6};
  collinear.corner_vertices = {0, 1, 2, 3, 4, 5};
  AddAttributes(collinear);
  CheckMapsAndRows(collinear, Triangulate(collinear, {}, serial));
  auto bad = mesh;
  auto nonplanar = bad.positions.Get(3);
  nonplanar[2] = std::numeric_limits<double>::denorm_min();
  bad.positions.Set(3, nonplanar);
  const auto rejected = Triangulate(bad, {}, serial);
  Require(rejected.status == TriangulationStatus::kBlocked &&
              rejected.diagnostics[0].code == "conversion.nonplanar_face" &&
              rejected.diagnostics[0].element == 2,
          "last face diagnostic provenance");
  CheckNoPartial(rejected);
  bad = mesh;
  bad.attributes[0].present->at(0) = 2;
  const auto invalid = Triangulate(bad, {}, serial);
  Require(invalid.diagnostics[0].code == "attribute.presence_value",
          "malformed storage diagnostic");
  CheckNoPartial(invalid);
  TriangulationOptions lower;
  lower.max_corners_per_face = 3;
  const auto limited = Triangulate(mesh, lower, serial);
  Require(limited.diagnostics[0].code == "conversion.face_corner_limit",
          "lowered face limit ignored");
  CheckNoPartial(limited);
  lower.max_corners_per_face = 4097;
  Require(Triangulate(mesh, lower, serial).diagnostics[0].code ==
              "conversion.unsupported_corner_limit",
          "unsupported ceiling accepted");
  ExecutionContext exact({1, 0, baseline.peak_tracked_payload_bytes});
  CheckMapsAndRows(mesh, Triangulate(mesh, {}, exact));
  ExecutionContext insufficient(
      {1, 0, baseline.peak_tracked_payload_bytes - 1});
  const auto capped = Triangulate(mesh, {}, insufficient);
  Require(capped.diagnostics[0].code == "conversion.payload_budget",
          "payload boundary ignored");
  CheckNoPartial(capped);
  Require(!serial.ActiveWorkers() && !serial.ActiveTrackedPayload() &&
              !insufficient.ActiveTrackedPayload(),
          "payload reservation leaked");
  Require(exact.PeakTrackedPayload() == baseline.peak_tracked_payload_bytes,
          "reservation metric does not match declared payload");
  Mesh empty;
  const auto empty_result = Triangulate(empty, {}, serial);
  Require(empty_result.status == TriangulationStatus::kAccepted &&
              empty_result.face_output_offsets == std::vector<index_t>{0},
          "empty conversion result");
  std::stop_source stopped;
  stopped.request_stop();
  const auto canceled = Triangulate(mesh, {}, serial, stopped.get_token());
  Require(canceled.status == TriangulationStatus::kCanceled,
          "pre-cancel ignored");
  CheckNoPartial(canceled);
}
void ExecutionCases() {
  const auto source = Mixed(80);
  ExecutionContext serial({1, 0}), parallel({4, 0});
  const auto copy = parallel;
  const auto expected = Triangulate(source, {}, serial);
  const auto actual = Triangulate(source, {}, parallel);
  Require(actual.workers_used == 4 && actual.serial_reason.empty(),
          "four workers not used");
  Require(SameMesh(*expected.mesh, *actual.mesh) &&
              expected.face_sources == actual.face_sources &&
              expected.corner_sources == actual.corner_sources &&
              expected.face_output_offsets == actual.face_output_offsets,
          "serial/parallel mismatch");
  Require(copy.PeakWorkers() == 4 &&
              copy.PeakTrackedPayload() == parallel.PeakTrackedPayload(),
          "copied context does not share state");
  const auto shared = execution_detail::Access::Get(parallel);
  {
    execution_detail::WorkerReservation nested(shared, 2);
    const auto reduced = Triangulate(source, {}, copy);
    Require(reduced.workers_used == 2 && parallel.ActiveWorkers() == 2,
            "nested workers exceed shared cap");
    Mesh bounds_source;
    for (std::size_t i = 0; i < 256; ++i)
      bounds_source.positions.Append({static_cast<double>(i), 0, 0});
    auto imported = EditableMesh::ImportMesh(bounds_source);
    const auto bounds = ComputeBounds(imported.mesh.Snapshot(), copy);
    Require(bounds.workers_used == 2 && parallel.ActiveWorkers() == 2,
            "bounds failed to share worker state");
  }
  {
    execution_detail::PayloadReservation other(shared);
    Require(other.Add(parallel.TrackedPayloadBudget()),
            "test payload reservation failed");
    const auto denied = Triangulate(source, {}, copy);
    Require(denied.diagnostics[0].code == "conversion.payload_budget",
            "shared payload cap ignored");
    CheckNoPartial(denied);
  }
  Mesh busy;
  for (std::size_t i = 0; i < 256; ++i)
    busy.positions.Append(
        {static_cast<double>(i), static_cast<double>(i * i), 0});
  for (std::size_t f = 0; f < 128; ++f) {
    for (std::size_t i = 0; i < busy.positions.size(); ++i)
      busy.corner_vertices.push_back(i);
    busy.face_offsets.push_back(busy.corner_vertices.size());
  }
  std::stop_source stop;
  auto running = std::async(std::launch::async, [&] {
    return Triangulate(busy, {}, parallel, stop.get_token());
  });
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(30);
  while (parallel.ActiveWorkers() == 0 &&
         running.wait_for(std::chrono::seconds(0)) !=
             std::future_status::ready &&
         std::chrono::steady_clock::now() < deadline)
    std::this_thread::yield();
  Require(parallel.ActiveWorkers() > 0,
          "conversion finished before concurrent cancellation admission");
  stop.request_stop();
  const auto canceled = running.get();
  Require(canceled.status == TriangulationStatus::kCanceled &&
              canceled.workers_used == 4,
          "concurrent cancellation did not join created workers");
  CheckNoPartial(canceled);
  Require(parallel.ActiveWorkers() == 0 && parallel.ActiveTrackedPayload() == 0,
          "concurrent cancellation reservation leaked");
  Require(parallel.PeakWorkers() <= parallel.WorkerBudget() &&
              parallel.PeakTrackedPayload() <= parallel.TrackedPayloadBudget(),
          "shared accounting exceeded cap");
}
}  // namespace
int main() {
  try {
    NativeCases();
    ExecutionCases();
    std::cout << "Native triangulation, byte correspondence, payload and "
                 "execution checks passed\n";
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
