// SPDX-License-Identifier: Apache-2.0
#include "meshvale/geometry/triangulation.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <limits>
#include <numeric>
#include <optional>
#include <set>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include "execution_internal.h"
#include "triangulation_internal.h"
#include "triangulation_numeric.h"

namespace meshvale::geometry {
namespace {
using triangulation_detail::Context;
using triangulation_detail::ProductError;
using triangulation_detail::TriangulateFace;
using Integer = triangulation_detail::Integer<std::allocator<std::uint32_t>>;

std::size_t Add(std::size_t a, std::size_t b) {
  if (b > std::numeric_limits<std::size_t>::max() - a)
    throw ProductError("conversion.size_overflow");
  return a + b;
}
std::size_t Multiply(std::size_t a, std::size_t b) {
  if (a && b > std::numeric_limits<std::size_t>::max() / a)
    throw ProductError("conversion.size_overflow");
  return a * b;
}
std::size_t Strings(const Attribute& attribute, Context& context) {
  auto bytes = Add(attribute.name.size(), attribute.semantic.size());
  for (const auto& [key, value] : attribute.metadata) {
    context.Check();
    bytes = Add(bytes, Add(sizeof(std::pair<const std::string, std::string>),
                           Add(key.size(), value.size())));
  }
  return bytes;
}
std::size_t ScalarSize(const Attribute& attribute) {
  return std::visit(
      [](const auto& values) {
        return sizeof(typename std::decay_t<decltype(values)>::value_type);
      },
      attribute.values);
}
std::size_t AttributePayload(const Attribute& attribute, Context& context) {
  auto bytes =
      Add(sizeof(Attribute),
          Add(Strings(attribute, context),
              Multiply(static_cast<std::size_t>(attribute.value_count()),
                       ScalarSize(attribute))));
  if (attribute.offsets)
    bytes = Add(bytes, Multiply(attribute.offsets->size(), sizeof(index_t)));
  if (attribute.present) bytes = Add(bytes, attribute.present->size());
  return bytes;
}
std::size_t SourcePayload(const Mesh& source, Context& context) {
  auto bytes =
      Add(sizeof(Mesh), Multiply(source.positions.size(), 3 * sizeof(double)));
  bytes = Add(bytes, Multiply(Add(source.face_offsets.size(),
                                  source.corner_vertices.size()),
                              sizeof(index_t)));
  for (const auto& attribute : source.attributes) {
    context.Check();
    bytes = Add(bytes, AttributePayload(attribute, context));
  }
  return bytes;
}
template <class T>
std::vector<T> Copy(const std::vector<T>& values, Context& context) {
  std::vector<T> copied;
  copied.reserve(values.size());
  for (std::size_t i = 0; i < values.size(); ++i) {
    if (i % 256 == 0) context.Check();
    copied.push_back(values[i]);
  }
  return copied;
}
PositionBuffer Copy(const PositionBuffer& values, Context& context) {
  PositionBuffer copied;
  copied.reserve(values.size());
  for (std::size_t i = 0; i < values.size(); ++i) {
    if (i % 256 == 0) context.Check();
    copied.Append(values.Get(i));
  }
  return copied;
}
template <class T>
ScalarBuffer<T> Copy(const ScalarBuffer<T>& values, Context& context) {
  // Preserve the existing scalar-copy cancellation interval.
  constexpr std::size_t kCopyCheckpointScalars = 256;
  ScalarBuffer<T> copied;
  copied.reserve(values.size());
  for (std::size_t i = 0; i < values.size(); ++i) {
    if (i % kCopyCheckpointScalars == 0) context.Check();
    copied.push_back(values[i]);
  }
  return copied;
}
Attribute Descriptor(const Attribute& source, Context& context) {
  context.Check();
  Attribute output;
  output.domain = source.domain;
  output.name = source.name;
  output.semantic = source.semantic;
  output.set_index = source.set_index;
  output.components = source.components;
  for (const auto& [key, value] : source.metadata) {
    context.Check();
    output.metadata.emplace(key, value);
  }
  return output;
}
Attribute CopyAttribute(const Attribute& source, Context& context) {
  Attribute output = Descriptor(source, context);
  output.values = std::visit(
      [&](const auto& values) -> AttributeValues {
        return Copy(values, context);
      },
      source.values);
  if (source.offsets) output.offsets = Copy(*source.offsets, context);
  if (source.present) output.present = Copy(*source.present, context);
  return output;
}
Mesh Capture(const Mesh& source, Context& context) {
  Mesh captured;
  captured.positions = Copy(source.positions, context);
  captured.face_offsets = Copy(source.face_offsets, context);
  captured.corner_vertices = Copy(source.corner_vertices, context);
  captured.attributes.reserve(source.attributes.size());
  for (const auto& attribute : source.attributes)
    captured.attributes.push_back(CopyAttribute(attribute, context));
  return captured;
}

// Return the first storage diagnostic without constructing a potentially huge
// diagnostic list from hostile malformed presence/offset arrays.
std::optional<Diagnostic> StorageIssue(const Mesh& mesh, Context& context) {
  const auto issue = [](const char* code, std::string subject,
                        std::optional<index_t> row = std::nullopt) {
    return std::optional<Diagnostic>(Diagnostic{code, std::move(subject), row});
  };
  for (std::size_t i = 0; i < mesh.positions.size(); ++i) {
    context.Check();
    for (const auto coordinate : mesh.positions.Get(i))
      if (!std::isfinite(coordinate))
        return issue("mesh.nonfinite_position", "positions", i);
  }
  if (mesh.face_offsets.empty())
    return issue("mesh.empty_offsets", "face_offsets");
  if (mesh.face_offsets.front() != 0)
    return issue("mesh.offset_start", "face_offsets");
  if (mesh.face_offsets.back() != mesh.corner_vertices.size())
    return issue("mesh.offset_end", "face_offsets");
  for (std::size_t i = 0; i < mesh.face_offsets.size(); ++i) {
    context.Check();
    if (mesh.face_offsets[i] > mesh.corner_vertices.size())
      return issue("mesh.offset_range", "face_offsets", i);
    if (i && mesh.face_offsets[i] < mesh.face_offsets[i - 1])
      return issue("mesh.offset_order", "face_offsets", i);
    if (i && mesh.face_offsets[i] - mesh.face_offsets[i - 1] < 3)
      return issue("mesh.short_face", "face_offsets", i - 1);
  }
  for (std::size_t i = 0; i < mesh.corner_vertices.size(); ++i) {
    context.Check();
    if (mesh.corner_vertices[i] >= mesh.positions.size())
      return issue("mesh.vertex_range", "corner_vertices", i);
  }
  std::set<std::pair<AttributeDomain, std::string_view>> names;
  for (const auto& a : mesh.attributes) {
    context.Check();
    if (a.domain != AttributeDomain::vertex &&
        a.domain != AttributeDomain::face &&
        a.domain != AttributeDomain::corner)
      return issue("attribute.invalid_domain", a.name);
    if (!names.emplace(a.domain, a.name).second)
      return issue("attribute.duplicate_name", a.name);
    if (a.name.empty()) return issue("attribute.empty_name", a.name);
    if (!a.components) return issue("attribute.zero_components", a.name);
    const auto rows = mesh.row_count(a.domain), values = a.value_count();
    if (a.offsets) {
      const auto& offsets = *a.offsets;
      if (offsets.empty()) return issue("attribute.empty_offsets", a.name);
      if (offsets.size() - 1 != rows)
        return issue("attribute.row_count", a.name);
      if (offsets.front() != 0) return issue("attribute.offset_start", a.name);
      if (offsets.back() != values)
        return issue("attribute.offset_end", a.name);
      for (std::size_t i = 0; i < offsets.size(); ++i) {
        context.Check();
        if (offsets[i] > values)
          return issue("attribute.offset_range", a.name, i);
        if (i && offsets[i] < offsets[i - 1])
          return issue("attribute.offset_order", a.name, i);
        if (offsets[i] % a.components)
          return issue("attribute.component_alignment", a.name, i);
      }
    } else if (values % a.components || values / a.components != rows)
      return issue("attribute.row_count", a.name);
    if (a.present) {
      if (a.present->size() != rows)
        return issue("attribute.presence_count", a.name);
      for (std::size_t i = 0; i < a.present->size(); ++i) {
        context.Check();
        if ((*a.present)[i] > 1)
          return issue("attribute.presence_value", a.name, i);
      }
    }
  }
  return std::nullopt;
}
std::pair<index_t, index_t> Row(const Attribute& a, index_t row) {
  if (a.offsets) return {(*a.offsets)[row], (*a.offsets)[row + 1]};
  return {row * a.components, (row + 1) * a.components};
}
Attribute Transfer(const Attribute& source, const std::vector<index_t>& rows,
                   Context& context) {
  Attribute output = Descriptor(source, context);
  std::size_t count = 0;
  for (const auto row : rows) {
    context.Check();
    const auto [a, b] = Row(source, row);
    count = Add(count, static_cast<std::size_t>(b - a));
  }
  if (source.offsets) {
    output.offsets = std::vector<index_t>{0};
    output.offsets->reserve(Add(rows.size(), 1));
  }
  if (source.present) {
    output.present = std::vector<std::uint8_t>{};
    output.present->reserve(rows.size());
  }
  output.values = std::visit(
      [&](const auto& values) -> AttributeValues {
        using Values = std::decay_t<decltype(values)>;
        Values copied;
        copied.reserve(count);
        for (const auto row : rows) {
          context.Check();
          const auto [a, b] = Row(source, row);
          for (index_t i = a; i < b; ++i) {
            if ((i - a) % 256 == 0) context.Check();
            copied.push_back(values[static_cast<std::size_t>(i)]);
          }
          if (output.offsets) output.offsets->push_back(copied.size());
          if (output.present) output.present->push_back((*source.present)[row]);
        }
        return copied;
      },
      source.values);
  return output;
}
struct FaceOutcome {
  std::vector<std::array<index_t, 3>> triangles;
  const char* failure = nullptr;
  std::exception_ptr exception;
};
static_assert(sizeof(FaceOutcome) ==
              sizeof(triangulation_detail::LegacyFaceOutcomeLayout));
static_assert(alignof(FaceOutcome) ==
              alignof(triangulation_detail::LegacyFaceOutcomeLayout));
std::size_t OutputPayload(const Mesh& source,
                          const std::vector<FaceOutcome>& faces,
                          std::size_t triangles, Context& context) {
  const auto corners = Multiply(triangles, 3);
  // Output positions, topology, both output maps and source-face output ranges.
  auto bytes =
      Add(sizeof(Mesh), Multiply(source.positions.size(), 3 * sizeof(double)));
  bytes =
      Add(bytes, Multiply(Add(Add(Multiply(corners, 2), Multiply(triangles, 2)),
                              Add(2, faces.size())),
                          sizeof(index_t)));
  for (const auto& a : source.attributes) {
    context.Check();
    if (a.domain == AttributeDomain::vertex) {
      bytes = Add(bytes, AttributePayload(a, context));
      continue;
    }
    std::size_t scalars = 0;
    for (std::size_t face = 0; face < faces.size(); ++face) {
      context.Check();
      if (a.domain == AttributeDomain::face) {
        const auto [first, last] = Row(a, face);
        scalars = Add(scalars, Multiply(static_cast<std::size_t>(last - first),
                                        faces[face].triangles.size()));
      } else
        for (const auto& triangle : faces[face].triangles)
          for (const auto corner : triangle) {
            context.Check();
            const auto [first, last] = Row(a, corner);
            scalars = Add(scalars, static_cast<std::size_t>(last - first));
          }
    }
    bytes = Add(
        bytes, Add(sizeof(Attribute),
                   Add(Strings(a, context), Multiply(scalars, ScalarSize(a)))));
    const auto rows = a.domain == AttributeDomain::face ? triangles : corners;
    if (a.offsets) bytes = Add(bytes, Multiply(Add(rows, 1), sizeof(index_t)));
    if (a.present) bytes = Add(bytes, rows);
  }
  return bytes;
}
}  // namespace

TriangulationResult Triangulate(const Mesh& source,
                                const TriangulationOptions& options,
                                const ExecutionContext& execution,
                                std::stop_token stop) {
  auto shared = execution_detail::Access::Get(execution);
  execution_detail::PayloadReservation payload(shared);
  TriangulationResult result;
  Context context{stop};
  auto fail = [&](const char* code, std::string subject = "conversion",
                  std::optional<index_t> face = std::nullopt) {
    result.mesh.reset();
    result.face_sources.clear();
    result.corner_sources.clear();
    result.face_output_offsets.clear();
    result.diagnostics = {{code, std::move(subject), face}};
    result.status = stop.stop_requested() ? TriangulationStatus::kCanceled
                                          : TriangulationStatus::kBlocked;
    if (result.status == TriangulationStatus::kCanceled)
      result.diagnostics = {{triangulation_detail::kConversionCanceled,
                             "conversion", std::nullopt}};
  };
  try {
    context.Check();
    if (options.max_corners_per_face < 3 || options.max_corners_per_face > 4096)
      throw ProductError("conversion.unsupported_corner_limit");
    const auto capture_bytes = SourcePayload(source, context);
    if (!payload.Add(Add(
            Add(capture_bytes, 65536),
            Multiply(source.attributes.size(),
                     sizeof(std::pair<AttributeDomain, std::string_view>)))))
      throw ProductError("conversion.payload_budget");
    result.peak_tracked_payload_bytes = payload.Count();
    const Mesh captured = Capture(source, context);
    if (const auto issue = StorageIssue(captured, context)) {
      fail(issue->code.c_str(), issue->subject, issue->element);
      return result;
    }
    std::size_t triangles = 0, maximum_corners = 0;
    for (std::size_t face = 0; face < captured.face_count(); ++face) {
      context.Check();
      const auto count =
          captured.face_offsets[face + 1] - captured.face_offsets[face];
      if (count > options.max_corners_per_face) {
        fail(triangulation_detail::kConversionFaceCornerLimit, "face", face);
        return result;
      }
      maximum_corners =
          std::max(maximum_corners, static_cast<std::size_t>(count));
      triangles = Add(triangles, static_cast<std::size_t>(count - 2));
    }
    const auto face_count = static_cast<std::size_t>(captured.face_count());
    const auto desired =
        face_count >= options.minimum_parallel_faces
            ? std::min(face_count, shared->options.worker_budget)
            : 0;
    execution_detail::WorkerReservation workers(shared, desired);
    if (!workers.Count())
      result.serial_reason =
          face_count == 0                      ? "empty mesh"
          : shared->options.worker_budget <= 1 ? "worker budget is one"
          : face_count < options.minimum_parallel_faces
              ? "below parallel face threshold"
              : "shared worker budget unavailable or fewer than two faces";
    // Binary64 grid coordinates need <=66 32-bit limbs. Reserve 68 per
    // coordinate and 64 KiB for live predicate temporaries, plus ear indices.
    const auto scratch =
        face_count
            ? Add(Multiply(maximum_corners,
                           5 * (68 * sizeof(std::uint32_t) + sizeof(Integer)) +
                               sizeof(index_t)),
                  65536)
            : 0;
    const auto lanes = std::max<std::size_t>(1, workers.Count());
    if (!payload.Add(Add(Add(Multiply(triangles, 3 * sizeof(index_t)),
                             Multiply(face_count, sizeof(FaceOutcome))),
                         Multiply(scratch, lanes))))
      throw ProductError("conversion.payload_budget");
    result.peak_tracked_payload_bytes = payload.Count();
    std::vector<FaceOutcome> faces(face_count);
    auto compute = [&](std::size_t first, std::size_t last) {
      for (std::size_t face = first; face < last; ++face) {
        if (stop.stop_requested()) break;
        try {
          faces[face].triangles =
              TriangulateFace(captured, captured.face_offsets[face],
                              captured.face_offsets[face + 1], context);
        } catch (const ProductError& error) {
          faces[face].failure = error.code;
        } catch (...) {
          faces[face].exception = std::current_exception();
        }
      }
    };
    if (!workers.Count())
      compute(0, face_count);
    else {
      std::vector<std::jthread> threads;
      threads.reserve(workers.Count());
      for (std::size_t i = 0; i < workers.Count(); ++i) {
        const auto first = face_count / workers.Count() * i +
                           std::min(i, face_count % workers.Count());
        const auto last = face_count / workers.Count() * (i + 1) +
                          std::min(i + 1, face_count % workers.Count());
        threads.emplace_back([&, first, last] { compute(first, last); });
      }
      for (auto& thread : threads) thread.join();
      result.workers_used = threads.size();
    }
    // Unexpected worker exceptions retain their standard exception contract;
    // cancellation takes priority only over expected face outcomes.
    for (const auto& face : faces)
      if (face.exception) std::rethrow_exception(face.exception);
    context.Check();
    for (std::size_t face = 0; face < faces.size(); ++face) {
      if (faces[face].failure) {
        fail(faces[face].failure, "face", face);
        return result;
      }
    }
    if (!payload.Add(OutputPayload(captured, faces, triangles, context)))
      throw ProductError("conversion.payload_budget");
    result.peak_tracked_payload_bytes = payload.Count();
    Mesh candidate;
    candidate.positions = Copy(captured.positions, context);
    candidate.face_offsets.reserve(Add(triangles, 1));
    candidate.corner_vertices.reserve(Multiply(triangles, 3));
    result.face_sources.reserve(triangles);
    result.corner_sources.reserve(Multiply(triangles, 3));
    result.face_output_offsets.reserve(Add(face_count, 1));
    result.face_output_offsets.push_back(0);
    for (std::size_t face = 0; face < faces.size(); ++face) {
      for (const auto& triangle : faces[face].triangles) {
        context.Check();
        result.face_sources.push_back(face);
        for (const auto corner : triangle) {
          result.corner_sources.push_back(corner);
          candidate.corner_vertices.push_back(captured.corner_vertices[corner]);
        }
        candidate.face_offsets.push_back(candidate.corner_vertices.size());
      }
      result.face_output_offsets.push_back(result.face_sources.size());
    }
    candidate.attributes.reserve(captured.attributes.size());
    for (const auto& attribute : captured.attributes) {
      context.Check();
      candidate.attributes.push_back(
          attribute.domain == AttributeDomain::vertex
              ? CopyAttribute(attribute, context)
              : Transfer(attribute,
                         attribute.domain == AttributeDomain::face
                             ? result.face_sources
                             : result.corner_sources,
                         context));
    }
    if (const auto issue = StorageIssue(candidate, context))
      throw ProductError("conversion.invalid_candidate");
    context.Check();
    result.mesh.emplace(std::move(candidate));
    result.status = TriangulationStatus::kAccepted;
    context.Check();
  } catch (const ProductError& error) {
    fail(error.code);
  }
  return result;
}
}  // namespace meshvale::geometry
