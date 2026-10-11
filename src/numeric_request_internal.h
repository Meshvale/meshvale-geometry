// SPDX-License-Identifier: Apache-2.0
#ifndef MESHVALE_GEOMETRY_SRC_NUMERIC_REQUEST_INTERNAL_H_
#define MESHVALE_GEOMETRY_SRC_NUMERIC_REQUEST_INTERNAL_H_

#include <cstddef>

namespace meshvale::geometry::numeric_detail {
// This is a request plan, not an allocator replacement. The original pinned
// Eigen constructor still executes its own allocation and release route.
[[nodiscard]] bool QualifiedRequestProfile() noexcept;
[[nodiscard]] std::size_t NumericalRequestBytes(std::size_t payload);
}  // namespace meshvale::geometry::numeric_detail
#endif  // MESHVALE_GEOMETRY_SRC_NUMERIC_REQUEST_INTERNAL_H_
