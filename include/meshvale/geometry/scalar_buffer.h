// SPDX-License-Identifier: Apache-2.0
#ifndef MESHVALE_GEOMETRY_SCALAR_BUFFER_H_
#define MESHVALE_GEOMETRY_SCALAR_BUFFER_H_

#include <concepts>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <memory>
#include <span>
#include <vector>

namespace meshvale::geometry {

template <class T>
concept SupportedAttributeScalar =
    std::same_as<T, float> || std::same_as<T, double> ||
    std::same_as<T, std::int32_t> || std::same_as<T, std::uint8_t> ||
    std::same_as<T, std::uint16_t> || std::same_as<T, std::uint32_t> ||
    std::same_as<T, std::uint64_t>;

// Contiguous scalar objects owned by a private Eigen vector. Copies retain raw
// bits and own independent logical values; moves leave an empty reusable
// source. The borrowed pointers, references and spans remain valid only while
// this owner lives and its storage is not replaced. Reserve/growing
// resize/append, insert, assignment and moves can invalidate them. Separate
// immutable owners permit concurrent reads; other access to the same owner
// needs external synchronization. Allocation/length failures leave the value
// unchanged. Index access is checked. Raw storage performs no normalization,
// row validation or numeric conversion.
template <SupportedAttributeScalar T>
class ScalarBuffer {
 public:
  using value_type = T;
  using difference_type = std::ptrdiff_t;
  using iterator = T*;
  using const_iterator = const T*;

  ScalarBuffer() noexcept;
  explicit ScalarBuffer(std::size_t count);
  ScalarBuffer(std::initializer_list<T> values);
  // Intentional copying ingress; the source vector never becomes backing
  // storage.
  ScalarBuffer(const std::vector<T>& values);
  // Nonempty ranges must be ordered pointers into the same live scalar array.
  ScalarBuffer(const T* first, const T* last);
  ScalarBuffer(const ScalarBuffer& other);
  ScalarBuffer& operator=(const ScalarBuffer& other);
  ScalarBuffer(ScalarBuffer&& other) noexcept;
  ScalarBuffer& operator=(ScalarBuffer&& other) noexcept;
  ~ScalarBuffer();

  [[nodiscard]] std::size_t size() const noexcept;
  [[nodiscard]] bool empty() const noexcept;
  [[nodiscard]] std::size_t capacity() const noexcept;
  void reserve(std::size_t count);
  void resize(std::size_t count);
  void clear() noexcept;
  [[nodiscard]] T* data() noexcept;
  [[nodiscard]] const T* data() const noexcept;
  [[nodiscard]] iterator begin() noexcept;
  [[nodiscard]] const_iterator begin() const noexcept;
  [[nodiscard]] iterator end() noexcept;
  [[nodiscard]] const_iterator end() const noexcept;
  [[nodiscard]] std::span<T> Values() noexcept;
  [[nodiscard]] std::span<const T> Values() const noexcept;
  [[nodiscard]] T& operator[](std::size_t index);
  [[nodiscard]] const T& operator[](std::size_t index) const;
  [[nodiscard]] T& front();
  [[nodiscard]] const T& front() const;
  [[nodiscard]] T& back();
  [[nodiscard]] const T& back() const;
  void push_back(const T& value);
  // Position must be this buffer's begin/end or an intervening iterator. Source
  // ranges obey the constructor precondition and may overlap this owner's data.
  // Insert uses geometric growth. With spare capacity, overlapping input is
  // staged before mutation; a nonoverlapping range needs no allocation.
  iterator insert(const_iterator position, const T* first, const T* last);

  // Native-format byte transfer, not serialization. Assign requires complete T
  // scalars and reads all source bytes before replacement; misalignment is
  // valid. Copy requires exactly size()*sizeof(T) bytes and permits overlap.
  // Empty transfers accept null pointers. Bad byte extents throw
  // invalid_argument; host/size ceilings throw length_error; invalid indices
  // throw out_of_range.
  void AssignBytes(std::span<const std::byte> source);
  void CopyBytesTo(std::span<std::byte> destination) const;
  [[nodiscard]] bool operator==(const ScalarBuffer& other) const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

extern template class ScalarBuffer<float>;
extern template class ScalarBuffer<double>;
extern template class ScalarBuffer<std::int32_t>;
extern template class ScalarBuffer<std::uint8_t>;
extern template class ScalarBuffer<std::uint16_t>;
extern template class ScalarBuffer<std::uint32_t>;
extern template class ScalarBuffer<std::uint64_t>;

}  // namespace meshvale::geometry
#endif  // MESHVALE_GEOMETRY_SCALAR_BUFFER_H_
