// SPDX-License-Identifier: Apache-2.0
#ifndef MESHVALE_GEOMETRY_SRC_MATERIALIZATION_STORAGE_H_
#define MESHVALE_GEOMETRY_SRC_MATERIALIZATION_STORAGE_H_

#include <meshvale/geometry/attributes.h>
#include <meshvale/geometry/editable_mesh.h>
#include <meshvale/geometry/execution.h>
#include <meshvale/geometry/materialization.h>
#include <meshvale/geometry/position_buffer.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include "materialization_allocation.h"

namespace meshvale::geometry::materialization_detail {
// Primitive storage has exact capacity and starts object lifetimes without a
// hidden value-initialization pass. Every published slot is filled first.
template <class T>
  requires(std::is_trivially_copyable_v<T> &&
           std::is_nothrow_default_constructible_v<T>)
class FlatArray {
 public:
  FlatArray() noexcept = default;
  FlatArray(const AllocationContext& allocation, std::size_t count)
      : size_(count) {
    if (count != 0) {
      data_ = static_cast<T*>(
          allocation.Allocate(CheckedProduct(count, sizeof(T)), alignof(T)));
      std::uninitialized_default_construct_n(data_, count);
    }
  }
  FlatArray(FlatArray&& other) noexcept
      : data_(std::exchange(other.data_, nullptr)),
        size_(std::exchange(other.size_, 0)) {}
  FlatArray& operator=(FlatArray&& other) noexcept {
    if (this != &other) {
      AllocationContext::Deallocate(data_);
      data_ = std::exchange(other.data_, nullptr);
      size_ = std::exchange(other.size_, 0);
    }
    return *this;
  }
  ~FlatArray() noexcept { AllocationContext::Deallocate(data_); }
  FlatArray(const FlatArray&) = delete;
  FlatArray& operator=(const FlatArray&) = delete;
  std::span<T> Values() noexcept { return {data_, size_}; }
  std::span<const T> Values() const noexcept { return {data_, size_}; }

 private:
  T* data_ = nullptr;
  std::size_t size_ = 0;
};
template <class T>
using ChargedVector = std::vector<T, Allocator<T>>;
struct AttributeState {
  explicit AttributeState(const AllocationContext& allocation);
  AllocationContext allocation;
  // Reverse destruction order frees the actual Eigen owner before its lease.
  OriginalRequestLease numerical_lease;
  AttributeValues values;
  FlatArray<index_t> offsets;
  FlatArray<std::uint8_t> present;
  FlatArray<char> text;
  ChargedVector<MetadataEntryView> metadata;
  AttributeDomain domain = AttributeDomain::vertex;
  std::uint32_t components = 0;
  std::optional<std::uint32_t> set_index;
  std::string_view name;
  std::string_view semantic;
  bool has_offsets = false;
  bool has_present = false;
  [[nodiscard]] AttributeReadView View() const noexcept;
};
struct VertexState {
  explicit VertexState(const AllocationContext& allocation);
  AllocationContext allocation;
  OriginalRequestLease numerical_lease;
  PositionBuffer positions;
};
struct MeshState {
  MeshState(const AllocationContext& allocation,
            const ExecutionContext& context);
  AllocationContext allocation;
  ExecutionContext context;
  std::shared_ptr<const VertexState> vertices;
  FlatArray<index_t> face_offsets;
  FlatArray<index_t> corner_vertices;
  ChargedVector<std::shared_ptr<const AttributeState>> attributes;
  ChargedVector<AttributeReadView> views;
};
struct InspectionState {
  InspectionState(const AllocationContext& allocation,
                  const MaterializedMesh& source);
  AllocationContext allocation;
  MaterializedMesh source;
  ChargedVector<StorageIssue> issues;
};
struct SelectionState {
  SelectionState(const AllocationContext& allocation,
                 const MaterializedMesh& candidate);
  AllocationContext allocation;
  MaterializedMesh candidate;
  FlatArray<index_t> source_faces;
  FlatArray<index_t> source_corners;
  index_t source_vertex_count = 0;
  index_t source_face_count = 0;
  index_t source_corner_count = 0;
};
struct Access {
  static const MeshState& State(const MaterializedMesh& mesh) noexcept;
  static MaterializedMesh MakeMesh(std::shared_ptr<const MeshState> state);
  static MeshInspection MakeInspection(
      std::shared_ptr<const InspectionState> state);
  static MaterializedFaceSelection MakeSelection(
      std::shared_ptr<const SelectionState> state);
};
}  // namespace meshvale::geometry::materialization_detail
#endif  // MESHVALE_GEOMETRY_SRC_MATERIALIZATION_STORAGE_H_
