// SPDX-License-Identifier: Apache-2.0
#ifndef MESHVALE_GEOMETRY_PYTHON_SNAPSHOT_H_
#define MESHVALE_GEOMETRY_PYTHON_SNAPSHOT_H_

#include <meshvale/geometry/mesh.h>

namespace meshvale::geometry::bindings {
// The one canonical Mesh type registered by this extension.
class Snapshot {
 public:
  Snapshot();
  explicit Snapshot(Mesh mesh);
  [[nodiscard]] const Mesh& Data() const;

 private:
  Mesh mesh_;
};
}  // namespace meshvale::geometry::bindings
#endif  // MESHVALE_GEOMETRY_PYTHON_SNAPSHOT_H_
