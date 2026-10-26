// SPDX-License-Identifier: Apache-2.0
#ifndef MESHVALE_GEOMETRY_SRC_TRIANGULATION_NUMERIC_H_
#define MESHVALE_GEOMETRY_SRC_TRIANGULATION_NUMERIC_H_

#include <meshvale/geometry/attributes.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <numeric>
#include <stdexcept>
#include <stop_token>
#include <type_traits>
#include <utility>
#include <vector>

namespace meshvale::geometry::triangulation_detail {
inline constexpr char kConversionCanceled[] = "conversion.cancelled";
inline constexpr char kConversionFaceCornerLimit[] =
    "conversion.face_corner_limit";
inline constexpr char kConversionRepeatedPosition[] =
    "conversion.repeated_position";
inline constexpr char kConversionDegenerateFace[] =
    "conversion.degenerate_face";
inline constexpr char kConversionNonplanarFace[] = "conversion.nonplanar_face";
inline constexpr char kConversionBacktrackingBoundary[] =
    "conversion.backtracking_boundary";
inline constexpr char kConversionSelfIntersection[] =
    "conversion.self_intersection";
inline constexpr char kConversionNoGeometricEar[] =
    "conversion.no_geometric_ear";
inline constexpr char kConversionDegenerateResult[] =
    "conversion.degenerate_result";
// One predicate/ear implementation. Storage policy is explicit at every nested
// numeric value. Moved-from internal temporaries are destroyed or assigned a
// bound value before reuse; no charged default Integer is constructible.
static_assert(sizeof(double) == sizeof(std::uint64_t) &&
                  std::numeric_limits<double>::is_iec559 &&
                  std::numeric_limits<double>::digits == 53 &&
                  std::numeric_limits<double>::max_exponent == 1024 &&
                  std::numeric_limits<double>::min_exponent == -1021,
              "Exact triangulation requires IEEE binary64 positions");
// Exact finite-binary64 grid arithmetic; original sign/magnitude base 2^32.
template <class A>
class Integer {
 public:
  static constexpr bool kLegacyStorage =
      std::is_same_v<A, std::allocator<std::uint32_t>>;
  Integer()
    requires std::is_same_v<A, std::allocator<std::uint32_t>>
  = default;
  Integer(const Integer&) = default;
  Integer(Integer&&) = default;
  Integer& operator=(const Integer&)
    requires(kLegacyStorage)
  = default;
  Integer& operator=(Integer&&)
    requires(kLegacyStorage)
  = default;
  Integer& operator=(const Integer& other)
    requires(!kLegacyStorage)
  {
    ValidatePair(*this, other);
    limbs_ = other.limbs_;
    sign_ = other.sign_;
    return *this;
  }
  Integer& operator=(Integer&& other)
    requires(!kLegacyStorage)
  {
    ValidatePair(*this, other);
    limbs_ = std::move(other.limbs_);
    sign_ = other.sign_;
    return *this;
  }
  explicit Integer(const A& allocator) : limbs_(allocator) {}
  explicit Integer(std::uint64_t value)
    requires std::is_same_v<A, std::allocator<std::uint32_t>>
      : Integer(value, A{}) {}
  Integer(std::uint64_t value, const A& allocator) : limbs_(allocator) {
    if (value != 0) {
      sign_ = 1;
      limbs_.push_back(static_cast<std::uint32_t>(value));
      if (value >> 32)
        limbs_.push_back(static_cast<std::uint32_t>(value >> 32));
    }
  }
  int Sign() const { return sign_; }
  A GetAllocator() const { return limbs_.get_allocator(); }
  static int CompareMagnitude(const Integer& a, const Integer& b) {
    ValidatePair(a, b);
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
    ValidatePair(a, b);
    if (!a.sign_) return b;
    if (!b.sign_) return a;
    Integer result(a.GetAllocator());
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
    ValidatePair(a, b);
    Integer result(a.GetAllocator());
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
    if (!sign_) return Integer(GetAllocator());
    Integer result(GetAllocator());
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
  static void ValidatePair(const Integer& a, const Integer& b) {
    if constexpr (!kLegacyStorage) {
      if (!(a.GetAllocator() == b.GetAllocator()))
        throw std::invalid_argument("Exact integer allocation owners differ");
    }
  }
  std::uint64_t Limb(std::size_t i) const {
    return i < limbs_.size() ? limbs_[i] : 0;
  }
  void Normalize() {
    while (!limbs_.empty() && !limbs_.back()) limbs_.pop_back();
    if (limbs_.empty()) sign_ = 0;
  }
  int sign_ = 0;
  std::vector<std::uint32_t, A> limbs_;
};

struct ProductError {
  explicit ProductError(const char* message) : code(message) {}
  const char* code;
};
struct Context {
  std::stop_token stop;
  void Check() const {
    if (stop.stop_requested()) throw ProductError(kConversionCanceled);
  }
};
template <class A>
using Point = std::array<Integer<A>, 3>;
template <class A, class T>
using Rebind = typename std::allocator_traits<A>::template rebind_alloc<T>;
template <class A>
Point<A> ZeroPoint(const A& a) {
  return {Integer<A>(a), Integer<A>(a), Integer<A>(a)};
}
template <class A>
Point<A> Subtract(const Point<A>& a, const Point<A>& b) {
  return {a[0] - b[0], a[1] - b[1], a[2] - b[2]};
}
template <class A>
Point<A> Cross(const Point<A>& a, const Point<A>& b) {
  return {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2],
          a[0] * b[1] - a[1] * b[0]};
}
template <class A>
Integer<A> Dot(const Point<A>& a, const Point<A>& b) {
  return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}
inline int Exponent(double value) {
  const auto bits = std::bit_cast<std::uint64_t>(value);
  const auto exponent = static_cast<int>((bits >> 52) & 2047);
  return exponent == 0 ? -1074 : exponent - 1023 - 52;
}
template <class A>
Integer<A> Exact(double value, int exponent, const A& allocator) {
  const auto bits = std::bit_cast<std::uint64_t>(value);
  const auto encoded_exponent = (bits >> 52) & 2047;
  auto mantissa = bits & ((std::uint64_t{1} << 52) - 1);
  if (encoded_exponent) mantissa |= std::uint64_t{1} << 52;
  Integer<A> result(mantissa, allocator);
  result = result.Shift(static_cast<unsigned>(Exponent(value) - exponent));
  return bits >> 63 ? -result : result;
}
template <class A>
using Point2 = std::array<Integer<A>, 2>;
template <class A>
Point2<A> ZeroPoint2(const A& a) {
  return {Integer<A>(a), Integer<A>(a)};
}
template <class A>
Integer<A> Orient(const Point2<A>& a, const Point2<A>& b, const Point2<A>& c,
                  Context& context) {
  context.Check();
  return (b[0] - a[0]) * (c[1] - a[1]) - (b[1] - a[1]) * (c[0] - a[0]);
}
template <class A>
bool Between(const Integer<A>& a, const Integer<A>& b, const Integer<A>& x) {
  return (x - a).Sign() * (x - b).Sign() <= 0;
}
template <class A>
bool OnSegment(const Point2<A>& a, const Point2<A>& b, const Point2<A>& p,
               Context& context) {
  return !Orient(a, b, p, context).Sign() && Between(a[0], b[0], p[0]) &&
         Between(a[1], b[1], p[1]);
}
template <class A>
bool Intersects(const Point2<A>& a, const Point2<A>& b, const Point2<A>& c,
                const Point2<A>& d, Context& context) {
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

template <class Source, class A = std::allocator<std::uint32_t>>
std::vector<std::array<index_t, 3>, Rebind<A, std::array<index_t, 3>>>
TriangulateFace(const Source& mesh, index_t start, index_t end,
                Context& context, const A& allocator = A{}) {
  context.Check();
  const index_t count = end - start;
  int exponent = 971;
  for (index_t i = start; i < end; ++i) {
    context.Check();
    for (const double value : mesh.positions.Get(mesh.corner_vertices[i]))
      if (value != 0) exponent = std::min(exponent, Exponent(value));
  }
  std::vector<Point<A>, Rebind<A, Point<A>>> points{
      Rebind<A, Point<A>>(allocator)};
  for (index_t i = start; i < end; ++i) {
    context.Check();
    auto point = ZeroPoint(allocator);
    for (std::size_t axis = 0; axis < 3; ++axis)
      point[axis] = Exact(mesh.positions.Get(mesh.corner_vertices[i])[axis],
                          exponent, allocator);
    points.push_back(std::move(point));
  }
  for (index_t i = 0; i < count; ++i)
    for (index_t j = i + 1; j < count; ++j) {
      context.Check();
      const auto difference = Subtract(points[i], points[j]);
      if (!difference[0].Sign() && !difference[1].Sign() &&
          !difference[2].Sign())
        throw ProductError(kConversionRepeatedPosition);
    }
  auto normal = ZeroPoint(allocator);
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
  if (!found) throw ProductError(kConversionDegenerateFace);
  for (const auto& point : points) {
    context.Check();
    if (Dot(normal, Subtract(point, points[0])).Sign())
      throw ProductError(kConversionNonplanarFace);
  }
  std::size_t drop = 0;
  for (std::size_t axis = 1; axis < 3; ++axis)
    if (Integer<A>::CompareMagnitude(normal[axis], normal[drop]) > 0)
      drop = axis;
  std::vector<Point2<A>, Rebind<A, Point2<A>>> projected{
      Rebind<A, Point2<A>>(allocator)};
  for (const auto& point : points) {
    context.Check();
    auto p = ZeroPoint2(allocator);
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
      throw ProductError(kConversionBacktrackingBoundary);
  }
  for (index_t i = 0; i < count; ++i) {
    const auto next = (i + 1) % count;
    for (index_t j = i + 1; j < count; ++j) {
      if (j == next || (j + 1) % count == i) continue;
      if (Intersects(projected[i], projected[next], projected[j],
                     projected[(j + 1) % count], context))
        throw ProductError(kConversionSelfIntersection);
    }
  }
  Integer<A> area(allocator);
  for (index_t i = 0; i < count; ++i) {
    context.Check();
    area = area + projected[i][0] * projected[(i + 1) % count][1] -
           projected[i][1] * projected[(i + 1) % count][0];
  }
  const int winding = area.Sign();
  if (!winding) throw ProductError(kConversionDegenerateFace);
  std::vector<index_t, Rebind<A, index_t>> remaining(
      static_cast<std::size_t>(count), Rebind<A, index_t>(allocator));
  std::iota(remaining.begin(), remaining.end(), index_t{0});
  std::vector<std::array<index_t, 3>, Rebind<A, std::array<index_t, 3>>>
      triangles{Rebind<A, std::array<index_t, 3>>(allocator)};
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
    if (!clipped) throw ProductError(kConversionNoGeometricEar);
  }
  if (Orient(projected[remaining[0]], projected[remaining[1]],
             projected[remaining[2]], context)
          .Sign() != winding)
    throw ProductError(kConversionDegenerateResult);
  triangles.push_back(
      {start + remaining[0], start + remaining[1], start + remaining[2]});
  return triangles;
}

}  // namespace meshvale::geometry::triangulation_detail
#endif  // MESHVALE_GEOMETRY_SRC_TRIANGULATION_NUMERIC_H_
