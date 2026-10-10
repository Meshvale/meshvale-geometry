// SPDX-License-Identifier: Apache-2.0
#ifndef MESHVALE_GEOMETRY_SRC_EIGEN_TYPES_H_
#define MESHVALE_GEOMETRY_SRC_EIGEN_TYPES_H_

#include <meshvale/geometry/position_buffer.h>

#include <Eigen/Core>
#include <tuple>

namespace meshvale::geometry::eigen_types {

using Index = Eigen::Index;
inline constexpr int kPositionComponents =
    static_cast<int>(std::tuple_size_v<PositionBuffer::Row>);
using PositionMatrix =
    Eigen::Matrix<double, Eigen::Dynamic, kPositionComponents, Eigen::RowMajor>;
template <class Scalar>
using Vector = Eigen::Matrix<Scalar, Eigen::Dynamic, 1>;
template <class Scalar>
using ConstVectorMap = Eigen::Map<const Vector<Scalar>, Eigen::Unaligned>;

static_assert(EIGEN_WORLD_VERSION == 3 && EIGEN_MAJOR_VERSION == 4 &&
                  EIGEN_MINOR_VERSION == 1,
              "Numerical storage requires the pinned Eigen 3.4.1 headers");

}  // namespace meshvale::geometry::eigen_types
#endif  // MESHVALE_GEOMETRY_SRC_EIGEN_TYPES_H_
