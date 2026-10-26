// SPDX-License-Identifier: Apache-2.0
#ifndef MESHVALE_GEOMETRY_SRC_TRIANGULATION_INTERNAL_H_
#define MESHVALE_GEOMETRY_SRC_TRIANGULATION_INTERNAL_H_

#include <meshvale/geometry/attributes.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <memory>
#include <span>
#include <vector>

#include "triangulation_numeric.h"

namespace meshvale::geometry::triangulation_detail {
inline constexpr std::size_t kPositionComponents = 3;
inline constexpr std::size_t kTriangleCorners = 3;
inline constexpr index_t kMinimumFaceCorners = 3;
inline constexpr index_t kMaximumFaceCorners = 4096;
using Triangle = std::array<index_t, kTriangleCorners>;
// Pin the legacy instantiation's representation independently of charged
// policy.
struct LegacyIntegerLayout {
  int sign = 0;
  std::vector<std::uint32_t> limbs;
};
struct LegacyFaceOutcomeLayout {
  std::vector<Triangle> triangles;
  const char* failure = nullptr;
  std::exception_ptr exception;
};
static_assert(sizeof(Integer<std::allocator<std::uint32_t>>) ==
              sizeof(LegacyIntegerLayout));
static_assert(alignof(Integer<std::allocator<std::uint32_t>>) ==
              alignof(LegacyIntegerLayout));

struct PositionView {
  std::span<const double> scalars;
  [[nodiscard]] std::array<double, kPositionComponents> Get(index_t row) const {
    const auto first = static_cast<std::size_t>(row) * kPositionComponents;
    return {scalars[first], scalars[first + 1], scalars[first + 2]};
  }
};
struct MeshView {
  PositionView positions;
  std::span<const index_t> corner_vertices;
};
}  // namespace meshvale::geometry::triangulation_detail
#endif  // MESHVALE_GEOMETRY_SRC_TRIANGULATION_INTERNAL_H_
