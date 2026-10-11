// SPDX-License-Identifier: Apache-2.0
#ifndef MESHVALE_GEOMETRY_VECTOR_BUFFER_H_
#define MESHVALE_GEOMETRY_VECTOR_BUFFER_H_

#include <array>
#include <cstddef>
#include <initializer_list>
#include <memory>
#include <span>
#include <type_traits>

namespace meshvale::geometry {

// Fixed vector widths for UVs, xyz vectors and homogeneous/tangent values.
inline constexpr int kVector2Components = 2;
inline constexpr int kVector3Components = 3;
inline constexpr int kVector4Components = 4;

// Owned fixed-width rows with independent copies and bit-preserving native byte
// transfer. float/double vectors of width 2, 3 and 4 are compiled
// instantiations; link meshvale::geometry. Get returns a copied row; no Eigen
// types or borrowed rows escape. Separate immutable buffers support concurrent
// reads; mutation of the same buffer needs external synchronization.
// Allocation/length failures leave storage unchanged. Default/moved-from
// buffers are empty and reusable.
template <class T, int Components>
class VectorBuffer {
  static_assert(std::is_same_v<T, float> || std::is_same_v<T, double>,
                "VectorBuffer supports float and double scalars");
  static_assert(Components >= kVector2Components &&
                    Components <= kVector4Components,
                "VectorBuffer supports widths 2, 3 and 4");

 public:
  using Row = std::array<T, Components>;

  VectorBuffer();
  VectorBuffer(std::initializer_list<Row> rows);
  VectorBuffer(const VectorBuffer& other);
  VectorBuffer& operator=(const VectorBuffer& other);
  VectorBuffer(VectorBuffer&& other) noexcept;
  VectorBuffer& operator=(VectorBuffer&& other) noexcept;
  ~VectorBuffer();

  [[nodiscard]] std::size_t size() const noexcept;
  [[nodiscard]] bool empty() const noexcept;
  void reserve(std::size_t rows);
  void clear() noexcept;
  // Indices outside the logical row count throw std::out_of_range.
  [[nodiscard]] Row Get(std::size_t row) const;
  void Set(std::size_t row, const Row& value);
  void Append(const Row& value);

  // Native-format bytes, not portable serialization. Source contains complete
  // rows; destination has exactly size()*sizeof(Row) bytes. Misaligned input
  // and empty null spans are accepted. Bad extents throw std::invalid_argument.
  // AssignBytes copies before replacing storage; CopyBytesTo permits overlap.
  // Authored NaN payloads/signed zero are retained without normalization.
  void AssignBytes(std::span<const std::byte> source);
  void CopyBytesTo(std::span<std::byte> destination) const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

extern template class VectorBuffer<float, kVector2Components>;
extern template class VectorBuffer<float, kVector3Components>;
extern template class VectorBuffer<float, kVector4Components>;
extern template class VectorBuffer<double, kVector2Components>;
extern template class VectorBuffer<double, kVector3Components>;
extern template class VectorBuffer<double, kVector4Components>;

}  // namespace meshvale::geometry

#endif  // MESHVALE_GEOMETRY_VECTOR_BUFFER_H_
