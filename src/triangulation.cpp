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

namespace meshvale::geometry {
namespace {
static_assert(sizeof(double) == sizeof(std::uint64_t) &&
                  std::numeric_limits<double>::is_iec559 &&
                  std::numeric_limits<double>::digits == 53 &&
                  std::numeric_limits<double>::max_exponent == 1024 &&
                  std::numeric_limits<double>::min_exponent == -1021,
              "Exact triangulation requires IEEE binary64 positions");
// Exact finite-binary64 grid arithmetic; original sign/magnitude base 2^32.
class Integer {
 public:
  Integer() = default;
  explicit Integer(std::uint64_t value) {
    if (value != 0) {
      sign_ = 1;
      limbs_.push_back(static_cast<std::uint32_t>(value));
      if (value >> 32)
        limbs_.push_back(static_cast<std::uint32_t>(value >> 32));
    }
  }
  int Sign() const { return sign_; }
  static int CompareMagnitude(const Integer& a, const Integer& b) {
    if (a.limbs_.size() != b.limbs_.size())
      return a.limbs_.size() > b.limbs_.size() ? 1 : -1;
    for (std::size_t i = a.limbs_.size(); i > 0; --i)
      if (a.limbs_[i - 1] != b.limbs_[i - 1])
        return a.limbs_[i - 1] > b.limbs_[i - 1] ? 1 : -1;
    return 0;
  }
  friend Integer operator-(Integer value) {
    value.sign_ = -value.sign_;
    return value;
  }
  friend Integer operator+(const Integer& a, const Integer& b) {
    if (!a.sign_) return b;
    if (!b.sign_) return a;
    Integer result;
    if (a.sign_ == b.sign_) {
      result.sign_ = a.sign_;
      const auto count = std::max(a.limbs_.size(), b.limbs_.size());
      std::uint64_t carry = 0;
      for (std::size_t i = 0; i < count; ++i) {
        const std::uint64_t sum = carry + a.Limb(i) + b.Limb(i);
        result.limbs_.push_back(static_cast<std::uint32_t>(sum));
        carry = sum >> 32;
      }
      if (carry) result.limbs_.push_back(static_cast<std::uint32_t>(carry));
    } else {
      const int order = CompareMagnitude(a, b);
      if (!order) return result;
      const auto& large = order > 0 ? a : b;
      const auto& small = order > 0 ? b : a;
      result.sign_ = large.sign_;
      std::uint64_t borrow = 0;
      for (std::size_t i = 0; i < large.limbs_.size(); ++i) {
        const std::uint64_t subtrahend = small.Limb(i) + borrow;
        const std::uint64_t minuend = large.Limb(i);
        result.limbs_.push_back(
            static_cast<std::uint32_t>(minuend - subtrahend));
        borrow = minuend < subtrahend ? 1 : 0;
      }
      result.Normalize();
    }
    return result;
  }
  friend Integer operator-(const Integer& a, const Integer& b) {
    return a + -b;
  }
  friend Integer operator*(const Integer& a, const Integer& b) {
    Integer result;
    if (!a.sign_ || !b.sign_) return result;
    result.sign_ = a.sign_ * b.sign_;
    result.limbs_.resize(a.limbs_.size() + b.limbs_.size(), 0);
    for (std::size_t i = 0; i < a.limbs_.size(); ++i) {
      std::uint64_t carry = 0;
      for (std::size_t j = 0; j < b.limbs_.size(); ++j) {
        // Max product + existing limb + carry is at most 2^64-1.
        const std::uint64_t value =
            static_cast<std::uint64_t>(a.limbs_[i]) * b.limbs_[j] +
            result.limbs_[i + j] + carry;
        result.limbs_[i + j] = static_cast<std::uint32_t>(value);
        carry = value >> 32;
      }
      result.limbs_[i + b.limbs_.size()] = static_cast<std::uint32_t>(carry);
    }
    result.Normalize();
    return result;
  }
  Integer Shift(unsigned bits) const {
    if (!sign_) return {};
    Integer result;
    result.sign_ = sign_;
    result.limbs_.resize(bits / 32, 0);
    std::uint64_t carry = 0;
    for (const auto value : limbs_) {
      const std::uint64_t shifted =
          (static_cast<std::uint64_t>(value) << (bits % 32)) | carry;
      result.limbs_.push_back(static_cast<std::uint32_t>(shifted));
      carry = shifted >> 32;
    }
    if (carry) result.limbs_.push_back(static_cast<std::uint32_t>(carry));
    return result;
  }

 private:
  std::uint64_t Limb(std::size_t i) const {
    return i < limbs_.size() ? limbs_[i] : 0;
  }
  void Normalize() {
    while (!limbs_.empty() && !limbs_.back()) limbs_.pop_back();
    if (limbs_.empty()) sign_ = 0;
  }
  int sign_ = 0;
  std::vector<std::uint32_t> limbs_;
};

struct ProductError {
  explicit ProductError(const char* message) : code(message) {}
  const char* code;
};
struct Context {
  std::stop_token stop;
  void Check() const {
    if (stop.stop_requested()) throw ProductError("conversion.cancelled");
  }
};
using Point = std::array<Integer, 3>;
Point Subtract(const Point& a, const Point& b) {
  return {a[0] - b[0], a[1] - b[1], a[2] - b[2]};
}
Point Cross(const Point& a, const Point& b) {
  return {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2],
          a[0] * b[1] - a[1] * b[0]};
}
Integer Dot(const Point& a, const Point& b) {
  return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}
int Exponent(double value) {
  const auto bits = std::bit_cast<std::uint64_t>(value);
  const auto exponent = static_cast<int>((bits >> 52) & 2047);
  return exponent == 0 ? -1074 : exponent - 1023 - 52;
}
Integer Exact(double value, int exponent) {
  const auto bits = std::bit_cast<std::uint64_t>(value);
  const auto encoded_exponent = (bits >> 52) & 2047;
  auto mantissa = bits & ((std::uint64_t{1} << 52) - 1);
  if (encoded_exponent) mantissa |= std::uint64_t{1} << 52;
  Integer result(mantissa);
  result = result.Shift(static_cast<unsigned>(Exponent(value) - exponent));
  return bits >> 63 ? -result : result;
}
using Point2 = std::array<Integer, 2>;
Integer Orient(const Point2& a, const Point2& b, const Point2& c,
               Context& context) {
  context.Check();
  return (b[0] - a[0]) * (c[1] - a[1]) - (b[1] - a[1]) * (c[0] - a[0]);
}
bool Between(const Integer& a, const Integer& b, const Integer& x) {
  return (x - a).Sign() * (x - b).Sign() <= 0;
}
bool OnSegment(const Point2& a, const Point2& b, const Point2& p,
               Context& context) {
  return !Orient(a, b, p, context).Sign() && Between(a[0], b[0], p[0]) &&
         Between(a[1], b[1], p[1]);
}
bool Intersects(const Point2& a, const Point2& b, const Point2& c,
                const Point2& d, Context& context) {
  const int x = Orient(a, b, c, context).Sign();
  const int y = Orient(a, b, d, context).Sign();
  const int z = Orient(c, d, a, context).Sign();
  const int w = Orient(c, d, b, context).Sign();
  if (x * y < 0 && z * w < 0) return true;
  return (!x && OnSegment(a, b, c, context)) ||
         (!y && OnSegment(a, b, d, context)) ||
         (!z && OnSegment(c, d, a, context)) ||
         (!w && OnSegment(c, d, b, context));
}

std::vector<std::array<index_t, 3>> TriangulateFace(const Mesh& mesh,
                                                    index_t start, index_t end,
                                                    Context& context) {
  context.Check();
  const index_t count = end - start;
  int exponent = 971;
  for (index_t i = start; i < end; ++i) {
    context.Check();
    for (const double value : mesh.positions.Get(mesh.corner_vertices[i]))
      if (value != 0) exponent = std::min(exponent, Exponent(value));
  }
  std::vector<Point> points;
  for (index_t i = start; i < end; ++i) {
    context.Check();
    Point point;
    for (std::size_t axis = 0; axis < 3; ++axis)
      point[axis] =
          Exact(mesh.positions.Get(mesh.corner_vertices[i])[axis], exponent);
    points.push_back(std::move(point));
  }
  for (index_t i = 0; i < count; ++i)
    for (index_t j = i + 1; j < count; ++j) {
      context.Check();
      const auto difference = Subtract(points[i], points[j]);
      if (!difference[0].Sign() && !difference[1].Sign() &&
          !difference[2].Sign())
        throw ProductError("conversion.repeated_position");
    }
  Point normal;
  bool found = false;
  for (index_t i = 2; i < count; ++i) {
    context.Check();
    normal =
        Cross(Subtract(points[1], points[0]), Subtract(points[i], points[0]));
    if (normal[0].Sign() || normal[1].Sign() || normal[2].Sign()) {
      found = true;
      break;
    }
  }
  if (!found) throw ProductError("conversion.degenerate_face");
  for (const auto& point : points) {
    context.Check();
    if (Dot(normal, Subtract(point, points[0])).Sign())
      throw ProductError("conversion.nonplanar_face");
  }
  std::size_t drop = 0;
  for (std::size_t axis = 1; axis < 3; ++axis)
    if (Integer::CompareMagnitude(normal[axis], normal[drop]) > 0) drop = axis;
  std::vector<Point2> projected;
  for (const auto& point : points) {
    context.Check();
    Point2 p;
    std::size_t output = 0;
    for (std::size_t axis = 0; axis < 3; ++axis)
      if (axis != drop) p[output++] = point[axis];
    projected.push_back(std::move(p));
  }
  for (index_t i = 0; i < count; ++i) {
    const auto previous = (i + count - 1) % count;
    const auto next = (i + 1) % count;
    if (!Orient(projected[previous], projected[i], projected[next], context)
             .Sign() &&
        !OnSegment(projected[previous], projected[next], projected[i], context))
      throw ProductError("conversion.backtracking_boundary");
  }
  for (index_t i = 0; i < count; ++i) {
    const auto next = (i + 1) % count;
    for (index_t j = i + 1; j < count; ++j) {
      if (j == next || (j + 1) % count == i) continue;
      if (Intersects(projected[i], projected[next], projected[j],
                     projected[(j + 1) % count], context))
        throw ProductError("conversion.self_intersection");
    }
  }
  Integer area;
  for (index_t i = 0; i < count; ++i) {
    context.Check();
    area = area + projected[i][0] * projected[(i + 1) % count][1] -
           projected[i][1] * projected[(i + 1) % count][0];
  }
  const int winding = area.Sign();
  if (!winding) throw ProductError("conversion.degenerate_face");
  std::vector<index_t> remaining(static_cast<std::size_t>(count));
  std::iota(remaining.begin(), remaining.end(), index_t{0});
  std::vector<std::array<index_t, 3>> triangles;
  triangles.reserve(static_cast<std::size_t>(count - 2));
  while (remaining.size() > 3) {
    bool clipped = false;
    for (std::size_t i = 0; i < remaining.size(); ++i) {
      const auto a = remaining[(i + remaining.size() - 1) % remaining.size()];
      const auto b = remaining[i];
      const auto c = remaining[(i + 1) % remaining.size()];
      if (Orient(projected[a], projected[b], projected[c], context).Sign() !=
          winding)
        continue;
      bool blocked = false;
      for (const auto p : remaining) {
        if (p == a || p == b || p == c) continue;
        if (winding * Orient(projected[a], projected[b], projected[p], context)
                          .Sign() >=
                0 &&
            winding * Orient(projected[b], projected[c], projected[p], context)
                          .Sign() >=
                0 &&
            winding * Orient(projected[c], projected[a], projected[p], context)
                          .Sign() >=
                0) {
          blocked = true;
          break;
        }
      }
      if (blocked) continue;
      // Explicit diagonal intersections protect the interior boundary.
      for (std::size_t j = 0; j < remaining.size(); ++j) {
        const auto x = remaining[j], y = remaining[(j + 1) % remaining.size()];
        if (x == a || x == c || y == a || y == c) continue;
        if (Intersects(projected[a], projected[c], projected[x], projected[y],
                       context)) {
          blocked = true;
          break;
        }
      }
      if (blocked) continue;
      triangles.push_back({start + a, start + b, start + c});
      remaining.erase(remaining.begin() + static_cast<std::ptrdiff_t>(i));
      clipped = true;
      break;
    }
    if (!clipped) throw ProductError("conversion.no_geometric_ear");
  }
  if (Orient(projected[remaining[0]], projected[remaining[1]],
             projected[remaining[2]], context)
          .Sign() != winding)
    throw ProductError("conversion.degenerate_result");
  triangles.push_back(
      {start + remaining[0], start + remaining[1], start + remaining[2]});
  return triangles;
}

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
      result.diagnostics = {
          {"conversion.cancelled", "conversion", std::nullopt}};
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
        fail("conversion.face_corner_limit", "face", face);
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
