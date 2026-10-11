// SPDX-License-Identifier: Apache-2.0
#ifndef MESHVALE_GEOMETRY_SRC_NUMERIC_BUFFER_INTERNAL_H_
#define MESHVALE_GEOMETRY_SRC_NUMERIC_BUFFER_INTERNAL_H_

#include <meshvale/geometry/scalar_buffer.h>
#include <meshvale/geometry/vector_buffer.h>

#include <cstddef>
#include <span>

namespace meshvale::geometry::numeric_detail {
struct RequestPlan {
  std::size_t implementation_bytes = 0;
  std::size_t numerical_bytes = 0;
  bool qualified = false;
};
// Implemented with the owning compiled Impl and pinned Eigen profile. No
// execution/allocation module dependency enters the base buffer target.
struct Access {
  template <SupportedAttributeScalar T>
  static RequestPlan ScalarPlan(std::size_t count);
  template <SupportedAttributeScalar T>
  static ScalarBuffer<T> AllocateScalars(std::size_t count);
  static RequestPlan PositionPlan(std::size_t rows);
  static VectorBuffer<double, kVector3Components> AllocatePositions(
      std::size_t rows);
  static std::span<double> PositionScalars(
      VectorBuffer<double, kVector3Components>& buffer) noexcept;
  static std::span<const double> PositionScalars(
      const VectorBuffer<double, kVector3Components>& buffer) noexcept;
};
}  // namespace meshvale::geometry::numeric_detail
#endif  // MESHVALE_GEOMETRY_SRC_NUMERIC_BUFFER_INTERNAL_H_
