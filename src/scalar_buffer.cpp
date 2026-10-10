// SPDX-License-Identifier: Apache-2.0
#include "meshvale/geometry/scalar_buffer.h"

#include <Eigen/Core>
#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <utility>

#include "eigen_types.h"

namespace meshvale::geometry {
namespace {
template <class T>
std::size_t MaximumCount() {
  // Retain room for Eigen's aligned-allocation padding/header as well as the
  // signed Index ceiling. Raw buffers are not execution-context allocations.
  constexpr std::size_t kAlignmentOverhead =
      EIGEN_MAX_ALIGN_BYTES + 2 * sizeof(void*);
  return std::min(
      static_cast<std::size_t>(std::numeric_limits<eigen_types::Index>::max()),
      (std::numeric_limits<std::size_t>::max() - kAlignmentOverhead) /
          sizeof(T));
}
template <class T>
void CheckCount(std::size_t count) {
  if (count > MaximumCount<T>())
    throw std::length_error("scalar buffer exceeds host extent");
}
template <class T>
std::size_t RangeCount(const T* first, const T* last) {
  if (first == last) return 0;
  if (!first || !last || last < first)
    throw std::invalid_argument("scalar buffer range is not ordered");
  const auto count = static_cast<std::size_t>(last - first);
  CheckCount<T>(count);
  return count;
}
template <class T>
std::size_t Growth(std::size_t capacity, std::size_t required) {
  CheckCount<T>(required);
  const auto maximum = MaximumCount<T>();
  constexpr std::size_t kCapacityGrowthFactor = 2;
  const auto doubled = capacity > maximum / kCapacityGrowthFactor
                           ? maximum
                           : capacity * kCapacityGrowthFactor;
  return std::max(required, doubled);
}
}  // namespace

template <SupportedAttributeScalar T>
struct ScalarBuffer<T>::Impl {
  explicit Impl(std::size_t capacity)
      : values(static_cast<eigen_types::Index>(capacity)) {}
  eigen_types::Vector<T> values;
  std::size_t size = 0;
};

template <SupportedAttributeScalar T>
ScalarBuffer<T>::ScalarBuffer() noexcept = default;
template <SupportedAttributeScalar T>
ScalarBuffer<T>::ScalarBuffer(std::size_t count) {
  resize(count);
}
template <SupportedAttributeScalar T>
ScalarBuffer<T>::ScalarBuffer(std::initializer_list<T> values) {
  AssignBytes(std::as_bytes(std::span(values.begin(), values.size())));
}
template <SupportedAttributeScalar T>
ScalarBuffer<T>::ScalarBuffer(const std::vector<T>& values) {
  AssignBytes(std::as_bytes(std::span(values)));
}
template <SupportedAttributeScalar T>
ScalarBuffer<T>::ScalarBuffer(const T* first, const T* last) {
  const auto count = RangeCount(first, last);
  AssignBytes(std::as_bytes(std::span(first, count)));
}
template <SupportedAttributeScalar T>
ScalarBuffer<T>::ScalarBuffer(const ScalarBuffer& other) {
  AssignBytes(std::as_bytes(other.Values()));
}
template <SupportedAttributeScalar T>
ScalarBuffer<T>& ScalarBuffer<T>::operator=(const ScalarBuffer& other) {
  if (this != &other) {
    ScalarBuffer replacement(other);
    impl_.swap(replacement.impl_);
  }
  return *this;
}
template <SupportedAttributeScalar T>
ScalarBuffer<T>::ScalarBuffer(ScalarBuffer&& other) noexcept = default;
template <SupportedAttributeScalar T>
ScalarBuffer<T>& ScalarBuffer<T>::operator=(ScalarBuffer&& other) noexcept {
  if (this != &other) impl_ = std::move(other.impl_);
  return *this;
}
template <SupportedAttributeScalar T>
ScalarBuffer<T>::~ScalarBuffer() = default;
template <SupportedAttributeScalar T>
std::size_t ScalarBuffer<T>::size() const noexcept {
  return impl_ ? impl_->size : 0;
}
template <SupportedAttributeScalar T>
bool ScalarBuffer<T>::empty() const noexcept {
  return size() == 0;
}
template <SupportedAttributeScalar T>
std::size_t ScalarBuffer<T>::capacity() const noexcept {
  return impl_ ? static_cast<std::size_t>(impl_->values.size()) : 0;
}
template <SupportedAttributeScalar T>
void ScalarBuffer<T>::reserve(std::size_t count) {
  CheckCount<T>(count);
  if (count <= capacity()) return;
  auto replacement = std::make_unique<Impl>(count);
  replacement->size = size();
  if (!empty())
    std::memcpy(replacement->values.data(), data(), size() * sizeof(T));
  impl_.swap(replacement);
}
template <SupportedAttributeScalar T>
void ScalarBuffer<T>::resize(std::size_t count) {
  const auto old_size = size();
  reserve(count);
  if (count > old_size) std::fill_n(data() + old_size, count - old_size, T{});
  if (impl_) impl_->size = count;
}
template <SupportedAttributeScalar T>
void ScalarBuffer<T>::clear() noexcept {
  if (impl_) impl_->size = 0;
}
template <SupportedAttributeScalar T>
T* ScalarBuffer<T>::data() noexcept {
  return impl_ ? impl_->values.data() : nullptr;
}
template <SupportedAttributeScalar T>
const T* ScalarBuffer<T>::data() const noexcept {
  return impl_ ? impl_->values.data() : nullptr;
}
template <SupportedAttributeScalar T>
typename ScalarBuffer<T>::iterator ScalarBuffer<T>::begin() noexcept {
  return data();
}
template <SupportedAttributeScalar T>
typename ScalarBuffer<T>::const_iterator ScalarBuffer<T>::begin()
    const noexcept {
  return data();
}
template <SupportedAttributeScalar T>
typename ScalarBuffer<T>::iterator ScalarBuffer<T>::end() noexcept {
  return empty() ? data() : data() + size();
}
template <SupportedAttributeScalar T>
typename ScalarBuffer<T>::const_iterator ScalarBuffer<T>::end() const noexcept {
  return empty() ? data() : data() + size();
}
template <SupportedAttributeScalar T>
std::span<T> ScalarBuffer<T>::Values() noexcept {
  return {data(), size()};
}
template <SupportedAttributeScalar T>
std::span<const T> ScalarBuffer<T>::Values() const noexcept {
  return {data(), size()};
}
template <SupportedAttributeScalar T>
T& ScalarBuffer<T>::operator[](std::size_t index) {
  if (index >= size()) throw std::out_of_range("scalar buffer index");
  return data()[index];
}
template <SupportedAttributeScalar T>
const T& ScalarBuffer<T>::operator[](std::size_t index) const {
  if (index >= size()) throw std::out_of_range("scalar buffer index");
  return data()[index];
}
template <SupportedAttributeScalar T>
T& ScalarBuffer<T>::front() {
  return (*this)[0];
}
template <SupportedAttributeScalar T>
const T& ScalarBuffer<T>::front() const {
  return (*this)[0];
}
template <SupportedAttributeScalar T>
T& ScalarBuffer<T>::back() {
  if (empty()) throw std::out_of_range("scalar buffer index");
  return (*this)[size() - 1];
}
template <SupportedAttributeScalar T>
const T& ScalarBuffer<T>::back() const {
  if (empty()) throw std::out_of_range("scalar buffer index");
  return (*this)[size() - 1];
}
template <SupportedAttributeScalar T>
void ScalarBuffer<T>::push_back(const T& value) {
  if (size() == MaximumCount<T>())
    throw std::length_error("scalar buffer exceeds host extent");
  std::array<std::byte, sizeof(T)> copied;
  std::memcpy(copied.data(), &value, sizeof(T));
  const auto old_size = size();
  if (old_size == capacity()) reserve(Growth<T>(capacity(), old_size + 1));
  std::memcpy(data() + old_size, copied.data(), sizeof(T));
  impl_->size = old_size + 1;
}
template <SupportedAttributeScalar T>
typename ScalarBuffer<T>::iterator ScalarBuffer<T>::insert(
    const_iterator position, const T* first, const T* last) {
  const auto address = reinterpret_cast<std::uintptr_t>(position);
  const auto base = reinterpret_cast<std::uintptr_t>(data());
  const auto bytes = size() * sizeof(T);
  if (address < base || address - base > bytes ||
      (address - base) % sizeof(T) != 0)
    throw std::out_of_range("scalar buffer insert position");
  const auto offset = (address - base) / sizeof(T);
  const auto count = RangeCount(first, last);
  if (count > MaximumCount<T>() - size())
    throw std::length_error("scalar buffer exceeds host extent");
  if (count == 0) return empty() ? data() : data() + offset;
  const auto required = size() + count;
  auto replacement = std::make_unique<Impl>(std::max(capacity(), required));
  auto* output = replacement->values.data();
  if (offset != 0) std::memcpy(output, data(), offset * sizeof(T));
  std::memcpy(output + offset, first, count * sizeof(T));
  if (offset != size())
    std::memcpy(output + offset + count, data() + offset,
                (size() - offset) * sizeof(T));
  replacement->size = required;
  impl_.swap(replacement);
  return data() + offset;
}
template <SupportedAttributeScalar T>
void ScalarBuffer<T>::AssignBytes(std::span<const std::byte> source) {
  if (source.size() % sizeof(T) != 0)
    throw std::invalid_argument("scalar buffer byte extent");
  const auto count = source.size() / sizeof(T);
  CheckCount<T>(count);
  std::unique_ptr<Impl> replacement;
  if (count != 0) {
    replacement = std::make_unique<Impl>(count);
    std::memcpy(replacement->values.data(), source.data(), source.size());
    replacement->size = count;
  }
  impl_.swap(replacement);
}
template <SupportedAttributeScalar T>
void ScalarBuffer<T>::CopyBytesTo(std::span<std::byte> destination) const {
  if (destination.size() != size() * sizeof(T))
    throw std::invalid_argument("scalar buffer byte extent");
  if (!empty()) std::memmove(destination.data(), data(), destination.size());
}
template <SupportedAttributeScalar T>
bool ScalarBuffer<T>::operator==(const ScalarBuffer& other) const {
  if (size() != other.size()) return false;
  for (std::size_t i = 0; i < size(); ++i)
    if (data()[i] != other.data()[i]) return false;
  return true;
}

template class ScalarBuffer<float>;
template class ScalarBuffer<double>;
template class ScalarBuffer<std::int32_t>;
template class ScalarBuffer<std::uint8_t>;
template class ScalarBuffer<std::uint16_t>;
template class ScalarBuffer<std::uint32_t>;
template class ScalarBuffer<std::uint64_t>;
}  // namespace meshvale::geometry
