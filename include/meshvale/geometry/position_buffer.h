// SPDX-License-Identifier: Apache-2.0
#ifndef MESHVALE_GEOMETRY_POSITION_BUFFER_H_
#define MESHVALE_GEOMETRY_POSITION_BUFFER_H_

#include "meshvale/geometry/vector_buffer.h"

namespace meshvale::geometry {

// Raw mesh positions are double-precision xyz vectors. Storage behavior is
// owned by VectorBuffer; this alias owns only the mesh's position
// representation.
using PositionBuffer = VectorBuffer<double, kVector3Components>;

}  // namespace meshvale::geometry

#endif  // MESHVALE_GEOMETRY_POSITION_BUFFER_H_
