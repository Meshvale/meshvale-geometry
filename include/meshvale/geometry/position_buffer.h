// SPDX-License-Identifier: Apache-2.0
#ifndef MESHVALE_GEOMETRY_POSITION_BUFFER_H_
#define MESHVALE_GEOMETRY_POSITION_BUFFER_H_

#include <array>
#include <cstddef>
#include <initializer_list>
#include <memory>
#include <span>

namespace meshvale::geometry {

// Owned xyz rows with independent value copies. Raw nonfinite values and scalar
// bits are retained. Get returns an owned row; no matrix or borrowed row
// escapes. Separate immutable buffers may be read concurrently. Mutation of the
// same buffer requires external synchronization. Allocation/length errors leave
// the buffer unchanged; invalid row indices throw std::out_of_range.
class PositionBuffer {
 public:
  using Row = std::array<double, 3>;

  PositionBuffer();
  PositionBuffer(std::initializer_list<Row> rows);
  PositionBuffer(const PositionBuffer& other);
  PositionBuffer& operator=(const PositionBuffer& other);
  PositionBuffer(PositionBuffer&& other) noexcept;
  PositionBuffer& operator=(PositionBuffer&& other) noexcept;
  ~PositionBuffer();

  [[nodiscard]] std::size_t size() const noexcept;
  [[nodiscard]] bool empty() const noexcept;
  void reserve(std::size_t rows);
  void clear() noexcept;
  [[nodiscard]] Row Get(std::size_t row) const;
  void Set(std::size_t row, const Row& value);
  void Append(const Row& value);

  // Native-format byte transfer, not portable serialization. Source bytes must
  // contain complete xyz binary64 triples. Destination must have exactly
  // size()*3*sizeof(double) bytes. Misaligned byte buffers are accepted;
  // transfers copy once and retain scalar bits. Bad byte shapes throw
  // std::invalid_argument. AssignBytes reads its entire source before replacing
  // storage; CopyBytesTo permits overlapping byte ranges. Empty transfers
  // accept a null data pointer.
  void AssignBytes(std::span<const std::byte> source);
  void CopyBytesTo(std::span<std::byte> destination) const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace meshvale::geometry

#endif  // MESHVALE_GEOMETRY_POSITION_BUFFER_H_
