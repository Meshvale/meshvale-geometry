// SPDX-License-Identifier: Apache-2.0
#include "meshvale/geometry/position_buffer.h"

#include <Eigen/Core>
#include <algorithm>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <utility>

namespace meshvale::geometry {
namespace {
constexpr std::size_t kRowBytes = 3 * sizeof(double);
static_assert(sizeof(PositionBuffer::Row) == kRowBytes);
static_assert(sizeof(double) == 8 && std::numeric_limits<double>::is_iec559);
static_assert(EIGEN_WORLD_VERSION == 3 && EIGEN_MAJOR_VERSION == 4 &&
                  EIGEN_MINOR_VERSION == 1,
              "Position storage requires the pinned Eigen 3.4.1 headers");

std::size_t MaximumRows() {
  // Leave room for Eigen's internal aligned-allocation padding/header before
  // converting the checked scalar count to its signed host Index.
  constexpr std::size_t kAlignmentOverhead =
      EIGEN_MAX_ALIGN_BYTES + 2 * sizeof(void*);
  return std::min(
      (std::numeric_limits<std::size_t>::max() - kAlignmentOverhead) /
          kRowBytes,
      static_cast<std::size_t>(std::numeric_limits<Eigen::Index>::max()) / 3);
}
void CheckRows(std::size_t rows) {
  if (rows > MaximumRows())
    throw std::length_error("Position buffer exceeds scalar/byte/index range");
}
}  // namespace

struct PositionBuffer::Impl {
  Eigen::Matrix<double, Eigen::Dynamic, 3, Eigen::RowMajor> allocation;
  std::size_t count = 0;
  explicit Impl(std::size_t capacity)
      : allocation(static_cast<Eigen::Index>(capacity), 3) {}
};

PositionBuffer::PositionBuffer() = default;
PositionBuffer::PositionBuffer(std::initializer_list<Row> rows) {
  reserve(rows.size());
  for (const auto& row : rows) Append(row);
}
PositionBuffer::PositionBuffer(const PositionBuffer& other) {
  if (other.empty()) return;
  reserve(other.size());
  std::memcpy(impl_->allocation.data(), other.impl_->allocation.data(),
              other.size() * kRowBytes);
  impl_->count = other.size();
}
PositionBuffer& PositionBuffer::operator=(const PositionBuffer& other) {
  if (this != &other) {
    PositionBuffer replacement(other);
    impl_.swap(replacement.impl_);
  }
  return *this;
}
PositionBuffer::PositionBuffer(PositionBuffer&& other) noexcept = default;
PositionBuffer& PositionBuffer::operator=(PositionBuffer&& other) noexcept {
  if (this != &other) impl_ = std::move(other.impl_);
  return *this;
}
PositionBuffer::~PositionBuffer() = default;
std::size_t PositionBuffer::size() const noexcept {
  return impl_ ? impl_->count : 0;
}
bool PositionBuffer::empty() const noexcept { return size() == 0; }
void PositionBuffer::reserve(std::size_t rows) {
  CheckRows(rows);
  if (rows == 0 ||
      (impl_ && rows <= static_cast<std::size_t>(impl_->allocation.rows())))
    return;
  auto replacement = std::make_unique<Impl>(rows);
  if (!empty())
    std::memcpy(replacement->allocation.data(), impl_->allocation.data(),
                size() * kRowBytes);
  replacement->count = size();
  impl_.swap(replacement);
}
void PositionBuffer::clear() noexcept {
  if (impl_) impl_->count = 0;
}
PositionBuffer::Row PositionBuffer::Get(std::size_t row) const {
  if (row >= size()) throw std::out_of_range("Position row is out of range");
  Row result;
  std::memcpy(result.data(), impl_->allocation.data() + row * 3, kRowBytes);
  return result;
}
void PositionBuffer::Set(std::size_t row, const Row& value) {
  if (row >= size()) throw std::out_of_range("Position row is out of range");
  std::memcpy(impl_->allocation.data() + row * 3, value.data(), kRowBytes);
}
void PositionBuffer::Append(const Row& value) {
  if (size() == MaximumRows())
    throw std::length_error("Position buffer row count overflow");
  const auto needed = size() + 1;
  const auto capacity =
      impl_ ? static_cast<std::size_t>(impl_->allocation.rows()) : 0;
  if (needed > capacity) {
    const auto grown =
        capacity <= MaximumRows() / 2 ? capacity * 2 : MaximumRows();
    reserve(std::max(needed, grown));
  }
  std::memcpy(impl_->allocation.data() + impl_->count * 3, value.data(),
              kRowBytes);
  ++impl_->count;
}
void PositionBuffer::AssignBytes(std::span<const std::byte> source) {
  if (source.size() % kRowBytes != 0)
    throw std::invalid_argument("Position bytes must contain xyz triples");
  PositionBuffer replacement;
  const auto rows = source.size() / kRowBytes;
  replacement.reserve(rows);
  if (rows != 0) {
    std::memcpy(replacement.impl_->allocation.data(), source.data(),
                source.size());
    replacement.impl_->count = rows;
  }
  impl_.swap(replacement.impl_);
}
void PositionBuffer::CopyBytesTo(std::span<std::byte> destination) const {
  if (destination.size() != size() * kRowBytes)
    throw std::invalid_argument("Position destination has incorrect byte size");
  if (!empty())
    std::memmove(destination.data(), impl_->allocation.data(),
                 destination.size());
}
}  // namespace meshvale::geometry
