// SPDX-License-Identifier: Apache-2.0
#include "meshvale/geometry/vector_buffer.h"

#include <Eigen/Core>
#include <algorithm>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <utility>

#include "eigen_types.h"

namespace meshvale::geometry {
namespace {
static_assert(sizeof(float) == 4 && sizeof(double) == 8);
template <class T, int Components>
std::size_t MaximumRows() {
  // Eigen's aligned allocator needs padding/header space in addition to
  // payload.
  constexpr std::size_t kAlignmentOverhead =
      EIGEN_MAX_ALIGN_BYTES + 2 * sizeof(void*);
  constexpr std::size_t kRowBytes = Components * sizeof(T);
  return std::min(
      (std::numeric_limits<std::size_t>::max() - kAlignmentOverhead) /
          kRowBytes,
      static_cast<std::size_t>(std::numeric_limits<eigen_types::Index>::max()) /
          Components);
}
template <class T, int Components>
void CheckRows(std::size_t rows) {
  if (rows > MaximumRows<T, Components>())
    throw std::length_error("Vector buffer exceeds scalar/byte/index range");
}
}  // namespace

template <class T, int Components>
struct VectorBuffer<T, Components>::Impl {
  static_assert(sizeof(Row) == Components * sizeof(T));
  static_assert(std::numeric_limits<T>::is_iec559);
  eigen_types::RowMatrix<T, Components> allocation;
  std::size_t count = 0;
  explicit Impl(std::size_t capacity)
      : allocation(static_cast<eigen_types::Index>(capacity), Components) {}
};

template <class T, int Components>
VectorBuffer<T, Components>::VectorBuffer() = default;
template <class T, int Components>
VectorBuffer<T, Components>::VectorBuffer(std::initializer_list<Row> rows) {
  reserve(rows.size());
  for (const auto& row : rows) Append(row);
}
template <class T, int Components>
VectorBuffer<T, Components>::VectorBuffer(const VectorBuffer& other) {
  if (other.empty()) return;
  reserve(other.size());
  std::memcpy(impl_->allocation.data(), other.impl_->allocation.data(),
              other.size() * sizeof(Row));
  impl_->count = other.size();
}
template <class T, int Components>
VectorBuffer<T, Components>& VectorBuffer<T, Components>::operator=(
    const VectorBuffer& other) {
  if (this != &other) {
    VectorBuffer replacement(other);
    impl_.swap(replacement.impl_);
  }
  return *this;
}
template <class T, int Components>
VectorBuffer<T, Components>::VectorBuffer(VectorBuffer&& other) noexcept =
    default;
template <class T, int Components>
VectorBuffer<T, Components>& VectorBuffer<T, Components>::operator=(
    VectorBuffer&& other) noexcept {
  if (this != &other) impl_ = std::move(other.impl_);
  return *this;
}
template <class T, int Components>
VectorBuffer<T, Components>::~VectorBuffer() = default;
template <class T, int Components>
std::size_t VectorBuffer<T, Components>::size() const noexcept {
  return impl_ ? impl_->count : 0;
}
template <class T, int Components>
bool VectorBuffer<T, Components>::empty() const noexcept {
  return size() == 0;
}
template <class T, int Components>
void VectorBuffer<T, Components>::reserve(std::size_t rows) {
  CheckRows<T, Components>(rows);
  if (rows == 0 ||
      (impl_ && rows <= static_cast<std::size_t>(impl_->allocation.rows())))
    return;
  auto replacement = std::make_unique<Impl>(rows);
  if (!empty())
    std::memcpy(replacement->allocation.data(), impl_->allocation.data(),
                size() * sizeof(Row));
  replacement->count = size();
  impl_.swap(replacement);
}
template <class T, int Components>
void VectorBuffer<T, Components>::clear() noexcept {
  if (impl_) impl_->count = 0;
}
template <class T, int Components>
typename VectorBuffer<T, Components>::Row VectorBuffer<T, Components>::Get(
    std::size_t row) const {
  if (row >= size()) throw std::out_of_range("Vector row is out of range");
  Row result;
  std::memcpy(result.data(), impl_->allocation.data() + row * Components,
              sizeof(Row));
  return result;
}
template <class T, int Components>
void VectorBuffer<T, Components>::Set(std::size_t row, const Row& value) {
  if (row >= size()) throw std::out_of_range("Vector row is out of range");
  std::memcpy(impl_->allocation.data() + row * Components, value.data(),
              sizeof(Row));
}
template <class T, int Components>
void VectorBuffer<T, Components>::Append(const Row& value) {
  if (size() == MaximumRows<T, Components>())
    throw std::length_error("Vector buffer row count overflow");
  const auto needed = size() + 1;
  const auto capacity =
      impl_ ? static_cast<std::size_t>(impl_->allocation.rows()) : 0;
  if (needed > capacity) {
    const auto grown = capacity <= MaximumRows<T, Components>() / 2
                           ? capacity * 2
                           : MaximumRows<T, Components>();
    reserve(std::max(needed, grown));
  }
  std::memcpy(impl_->allocation.data() + impl_->count * Components,
              value.data(), sizeof(Row));
  ++impl_->count;
}
template <class T, int Components>
void VectorBuffer<T, Components>::AssignBytes(
    std::span<const std::byte> source) {
  if (source.size() % sizeof(Row) != 0)
    throw std::invalid_argument("Vector bytes must contain complete rows");
  VectorBuffer replacement;
  const auto rows = source.size() / sizeof(Row);
  replacement.reserve(rows);
  if (rows != 0) {
    std::memcpy(replacement.impl_->allocation.data(), source.data(),
                source.size());
    replacement.impl_->count = rows;
  }
  impl_.swap(replacement.impl_);
}
template <class T, int Components>
void VectorBuffer<T, Components>::CopyBytesTo(
    std::span<std::byte> destination) const {
  if (destination.size() != size() * sizeof(Row))
    throw std::invalid_argument("Vector destination has incorrect byte size");
  if (!empty())
    std::memmove(destination.data(), impl_->allocation.data(),
                 destination.size());
}

template class VectorBuffer<float, kVector2Components>;
template class VectorBuffer<float, kVector3Components>;
template class VectorBuffer<float, kVector4Components>;
template class VectorBuffer<double, kVector2Components>;
template class VectorBuffer<double, kVector3Components>;
template class VectorBuffer<double, kVector4Components>;

}  // namespace meshvale::geometry
