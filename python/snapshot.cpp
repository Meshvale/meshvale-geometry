// SPDX-License-Identifier: Apache-2.0
#include "snapshot.h"

#include <utility>

namespace meshvale::geometry::bindings {
Snapshot::Snapshot() = default;
Snapshot::Snapshot(Mesh mesh) : mesh_(std::move(mesh)) {}
const Mesh& Snapshot::Data() const { return mesh_; }
}  // namespace meshvale::geometry::bindings
