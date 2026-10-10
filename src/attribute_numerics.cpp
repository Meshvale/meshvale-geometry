// SPDX-License-Identifier: Apache-2.0
#include "meshvale/geometry/attribute_numerics.h"

#include <meshvale/geometry/execution.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <exception>
#include <limits>
#include <new>
#include <stdexcept>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

#include "eigen_types.h"

namespace meshvale::geometry {
namespace {
struct Failure {
  const char* code;
  std::optional<index_t> row = std::nullopt;
};
void CheckStop(std::stop_token stop) {
  if (stop.stop_requested()) throw Failure{"numerical.cancelled"};
}
std::size_t AddBytes(std::size_t a, std::size_t b) {
  if (b > std::numeric_limits<std::size_t>::max() - a)
    throw Failure{"numerical.host_extent"};
  return a + b;
}
std::size_t MultiplyBytes(std::size_t a, std::size_t b) {
  if (a && b > std::numeric_limits<std::size_t>::max() / a)
    throw Failure{"numerical.host_extent"};
  return a * b;
}
void CheckExtent(index_t value) {
  if (value > static_cast<index_t>(
                  std::numeric_limits<eigen_types::Index>::max()) ||
      value > std::numeric_limits<std::size_t>::max())
    throw Failure{"numerical.host_extent"};
}
void CheckShape(const Attribute& a, index_t rows, std::stop_token stop) {
  CheckStop(stop);
  CheckExtent(rows);
  CheckExtent(a.value_count());
  if (a.domain != AttributeDomain::vertex &&
      a.domain != AttributeDomain::face && a.domain != AttributeDomain::corner)
    throw Failure{"attribute.invalid_domain"};
  if (a.name.empty()) throw Failure{"attribute.empty_name"};
  if (!a.components) throw Failure{"attribute.zero_components"};
  const auto count = a.value_count();
  if (a.offsets) {
    const auto& offsets = *a.offsets;
    CheckExtent(offsets.size());
    if (offsets.empty()) throw Failure{"attribute.empty_offsets"};
    if (offsets.size() - 1 != rows) throw Failure{"attribute.row_count"};
    if (offsets.front()) throw Failure{"attribute.offset_start"};
    if (offsets.back() != count) throw Failure{"attribute.offset_end"};
    for (std::size_t i = 0; i < offsets.size(); ++i) {
      CheckStop(stop);
      if (offsets[i] > count) throw Failure{"attribute.offset_range", i};
      if (i && offsets[i] < offsets[i - 1])
        throw Failure{"attribute.offset_order", i};
      if (offsets[i] % a.components)
        throw Failure{"attribute.component_alignment", i};
    }
  } else if (count % a.components || count / a.components != rows) {
    throw Failure{"attribute.row_count"};
  }
  if (a.present) {
    CheckExtent(a.present->size());
    if (a.present->size() != rows) throw Failure{"attribute.presence_count"};
    for (std::size_t i = 0; i < a.present->size(); ++i) {
      CheckStop(stop);
      if ((*a.present)[i] > 1) throw Failure{"attribute.presence_value", i};
    }
  }
}

// One requested block holds its ledger owner, alignment and typed payload.
// All stored types are at most max_align_t aligned. No allocator cookie or
// adapter header is omitted from the admitted operator-new request.
struct alignas(std::max_align_t) Block {
  PayloadLease lease;
  explicit Block(PayloadLease charge) noexcept : lease(std::move(charge)) {}
  void* Data() noexcept {
    return reinterpret_cast<std::byte*>(this) + sizeof(Block);
  }
  const void* Data() const noexcept {
    return reinterpret_cast<const std::byte*>(this) + sizeof(Block);
  }
  static Block* Allocate(const ExecutionContext& execution, std::size_t bytes,
                         std::stop_token stop, std::size_t& peak) {
    const auto requested = AddBytes(sizeof(Block), bytes);
    auto admission = TryReservePayload(execution, requested, stop);
    if (admission.status != ReservationStatus::kAccepted)
      throw Failure{admission.status == ReservationStatus::kCanceled
                        ? "numerical.cancelled"
                        : "numerical.payload_budget"};
    peak = AddBytes(peak, requested);
    auto* memory = ::operator new(requested);
    return ::new (memory) Block(std::move(admission.lease));
  }
  static Block* FromData(void* data) noexcept {
    return reinterpret_cast<Block*>(static_cast<std::byte*>(data) -
                                    sizeof(Block));
  }
  static void Free(Block* block) noexcept {
    // Retain the ledger owner until the entire requested allocation is freed.
    auto charge = std::move(block->lease);
    block->~Block();
    ::operator delete(block);
    charge.Reset();
  }
};
struct BlockDeleter {
  void operator()(Block* block) const noexcept { Block::Free(block); }
};
using BlockOwner = std::unique_ptr<Block, BlockDeleter>;

template <class T>
class OwnedArray {
 public:
  OwnedArray() noexcept = default;
  OwnedArray(const ExecutionContext& execution, std::size_t count,
             std::stop_token stop, std::size_t& peak) {
    static_assert(alignof(T) <= alignof(Block));
    if (!count) return;
    block_.reset(Block::Allocate(execution, MultiplyBytes(count, sizeof(T)),
                                 stop, peak));
    std::uninitialized_value_construct_n(Data(), count);
    count_ = count;
  }
  OwnedArray(OwnedArray&& other) noexcept
      : block_(std::move(other.block_)),
        count_(std::exchange(other.count_, 0)) {}
  OwnedArray& operator=(OwnedArray&& other) noexcept {
    if (this != &other) {
      Clear();
      block_ = std::move(other.block_);
      count_ = std::exchange(other.count_, 0);
    }
    return *this;
  }
  ~OwnedArray() noexcept { Clear(); }
  OwnedArray(const OwnedArray&) = delete;
  OwnedArray& operator=(const OwnedArray&) = delete;
  T* Data() noexcept {
    return block_ ? static_cast<T*>(block_->Data()) : nullptr;
  }
  const T* Data() const noexcept {
    return block_ ? static_cast<const T*>(block_->Data()) : nullptr;
  }
  std::size_t size() const noexcept { return count_; }
  T& operator[](std::size_t row) noexcept { return Data()[row]; }
  const T& operator[](std::size_t row) const noexcept { return Data()[row]; }

 private:
  void Clear() noexcept {
    if (count_) std::destroy_n(Data(), count_);
    count_ = 0;
    block_.reset();
  }
  BlockOwner block_;
  std::size_t count_ = 0;
};
using OwnedValues =
    std::variant<OwnedArray<float>, OwnedArray<double>,
                 OwnedArray<std::int32_t>, OwnedArray<std::uint8_t>,
                 OwnedArray<std::uint16_t>, OwnedArray<std::uint32_t>,
                 OwnedArray<std::uint64_t>>;
struct Capture {
  OwnedValues values;
  OwnedArray<index_t> offsets;
  OwnedArray<std::uint8_t> present;
  bool ragged = false;
  bool sparse = false;
  std::uint32_t components = 1;
};
template <class T>
OwnedArray<T> CopyBits(const std::vector<T>& source,
                       const ExecutionContext& execution, std::stop_token stop,
                       std::size_t& peak) {
  OwnedArray<T> result(execution, source.size(), stop, peak);
  for (std::size_t first = 0; first < source.size();) {
    CheckStop(stop);
    const auto count = std::min<std::size_t>(4096, source.size() - first);
    std::memcpy(result.Data() + first, source.data() + first,
                count * sizeof(T));
    first += count;
  }
  return result;
}
Capture CaptureBits(const Attribute& source, const ExecutionContext& execution,
                    std::stop_token stop, std::size_t& peak) {
  Capture result;
  result.components = source.components;
  result.values = std::visit(
      [&](const auto& values) -> OwnedValues {
        return CopyBits(values, execution, stop, peak);
      },
      source.values);
  if (source.offsets) {
    result.ragged = true;
    result.offsets = CopyBits(*source.offsets, execution, stop, peak);
  }
  if (source.present) {
    result.sparse = true;
    result.present = CopyBits(*source.present, execution, stop, peak);
  }
  return result;
}
struct RowOutcome {
  const char* failure = nullptr;
  std::exception_ptr exception;
};
template <class T>
struct CheckedAdd {
  std::stop_token stop;
  T operator()(T a, T b) const {
    CheckStop(stop);
    if constexpr (std::is_integral_v<T>) {
      if constexpr (std::is_signed_v<T>) {
        if ((b > 0 && a > std::numeric_limits<T>::max() - b) ||
            (b < 0 && a < std::numeric_limits<T>::min() - b))
          throw Failure{"numerical.integer_overflow"};
      } else if (a > std::numeric_limits<T>::max() - b) {
        throw Failure{"numerical.integer_overflow"};
      }
    }
    const T value = static_cast<T>(a + b);
    if constexpr (std::is_floating_point_v<T>)
      if (!std::isfinite(value)) throw Failure{"numerical.nonfinite_result"};
    return value;
  }
};
template <class T>
struct CheckedScalar {
  std::stop_token stop;
  bool squared;
  T operator()(T value) const {
    CheckStop(stop);
    if constexpr (std::is_floating_point_v<T>) {
      if (!std::isfinite(value)) throw Failure{"numerical.nonfinite_value"};
      if (!squared) return value;
      const T result = value * value;
      if (!std::isfinite(result)) throw Failure{"numerical.nonfinite_result"};
      return result;
    } else {
      if (!squared) return value;
      using Unsigned = std::make_unsigned_t<T>;
      Unsigned magnitude;
      if constexpr (std::is_signed_v<T>) {
        magnitude = value < 0 ? static_cast<Unsigned>(-(value + 1)) + 1
                              : static_cast<Unsigned>(value);
      } else {
        magnitude = value;
      }
      const auto maximum = static_cast<Unsigned>(std::numeric_limits<T>::max());
      if (magnitude && magnitude > maximum / magnitude)
        throw Failure{"numerical.integer_overflow"};
      return static_cast<T>(magnitude * magnitude);
    }
  }
};
}  // namespace

struct AttributeReductionResult::Impl {
  OwnedValues values;
  OwnedArray<std::uint8_t> present;
};
void AttributeReductionResult::ImplDeleter::operator()(
    Impl* impl) const noexcept {
  auto* block = Block::FromData(impl);
  impl->~Impl();
  Block::Free(block);
}
AttributeReductionResult::AttributeReductionResult() noexcept = default;
AttributeReductionResult::AttributeReductionResult(
    AttributeReductionResult&& other) noexcept
    : impl_(std::move(other.impl_)),
      status_(std::exchange(other.status_, AttributeReductionStatus::kBlocked)),
      diagnostic_(std::exchange(other.diagnostic_, std::nullopt)),
      workers_used_(std::exchange(other.workers_used_, 0)),
      serial_reason_(std::exchange(other.serial_reason_, {})),
      peak_bytes_(std::exchange(other.peak_bytes_, 0)) {}
AttributeReductionResult& AttributeReductionResult::operator=(
    AttributeReductionResult&& other) noexcept {
  if (this != &other) {
    impl_ = std::move(other.impl_);
    status_ = std::exchange(other.status_, AttributeReductionStatus::kBlocked);
    diagnostic_ = std::exchange(other.diagnostic_, std::nullopt);
    workers_used_ = std::exchange(other.workers_used_, 0);
    serial_reason_ = std::exchange(other.serial_reason_, {});
    peak_bytes_ = std::exchange(other.peak_bytes_, 0);
  }
  return *this;
}
AttributeReductionResult::~AttributeReductionResult() noexcept = default;
AttributeReductionStatus AttributeReductionResult::Status() const noexcept {
  return status_;
}
AttributeScalar AttributeReductionResult::Get(index_t row) const {
  if (!impl_ || row >= impl_->present.size())
    throw std::out_of_range("attribute reduction row");
  return std::visit(
      [&](const auto& values) -> AttributeScalar {
        return values[static_cast<std::size_t>(row)];
      },
      impl_->values);
}
AttributeValues AttributeReductionResult::CopyValues() const {
  if (!impl_)
    throw std::logic_error("attribute reduction has no accepted values");
  return std::visit(
      [](const auto& values) -> AttributeValues {
        using T =
            std::remove_cv_t<std::remove_pointer_t<decltype(values.Data())>>;
        std::vector<T> result(values.size());
        if (!result.empty())
          std::memcpy(result.data(), values.Data(), result.size() * sizeof(T));
        return result;
      },
      impl_->values);
}
std::span<const std::uint8_t> AttributeReductionResult::Presence()
    const noexcept {
  return impl_ ? std::span<const std::uint8_t>(impl_->present.Data(),
                                               impl_->present.size())
               : std::span<const std::uint8_t>{};
}
std::span<const AttributeReductionDiagnostic>
AttributeReductionResult::Diagnostics() const noexcept {
  return diagnostic_
             ? std::span<const AttributeReductionDiagnostic>(&*diagnostic_, 1)
             : std::span<const AttributeReductionDiagnostic>{};
}
std::size_t AttributeReductionResult::WorkersUsed() const noexcept {
  return workers_used_;
}
std::string_view AttributeReductionResult::SerialReason() const noexcept {
  return serial_reason_;
}
std::size_t AttributeReductionResult::PeakTrackedPayloadBytes() const noexcept {
  return peak_bytes_;
}

AttributeReductionResult AttributeReductionResult::Reduce(
    const Attribute& source, index_t expected_rows,
    const AttributeReductionOptions& options, const ExecutionContext& execution,
    std::stop_token stop, bool squared) {
  const auto budget = execution.WorkerBudget();
  AttributeReductionResult result;
  try {
    CheckShape(source, expected_rows, stop);
    const auto rows = static_cast<std::size_t>(expected_rows);
    BlockOwner impl_block(
        Block::Allocate(execution, sizeof(Impl), stop, result.peak_bytes_));
    auto* impl = ::new (impl_block->Data()) Impl;
    result.impl_.reset(impl);
    impl_block.release();
    const auto captured =
        CaptureBits(source, execution, stop, result.peak_bytes_);
    OwnedArray<RowOutcome> outcomes(execution, rows, stop, result.peak_bytes_);
    result.impl_->present =
        OwnedArray<std::uint8_t>(execution, rows, stop, result.peak_bytes_);
    for (std::size_t row = 0; row < rows; ++row) {
      CheckStop(stop);
      result.impl_->present[row] = captured.sparse ? captured.present[row] : 1;
    }
    result.impl_->values = std::visit(
        [&](const auto& values) -> OwnedValues {
          using T =
              std::remove_cv_t<std::remove_pointer_t<decltype(values.Data())>>;
          return OwnedArray<T>(execution, rows, stop, result.peak_bytes_);
        },
        captured.values);
    const auto desired = rows >= options.minimum_parallel_rows && rows >= 2
                             ? std::min(rows, budget)
                             : 0;
    auto workers = TryReserveWorkers(execution, desired);
    if (!workers.Count())
      result.serial_reason_ = rows == 0     ? "empty attribute"
                              : rows < 2    ? "fewer than two rows"
                              : budget <= 1 ? "worker budget is one"
                              : rows < options.minimum_parallel_rows
                                  ? "below parallel row threshold"
                                  : "shared worker budget unavailable";
    std::visit(
        [&](const auto& values) {
          using T =
              std::remove_cv_t<std::remove_pointer_t<decltype(values.Data())>>;
          auto& output = std::get<OwnedArray<T>>(result.impl_->values);
          auto compute = [&](std::size_t first, std::size_t last) {
            for (std::size_t row = first; row < last; ++row) {
              if (stop.stop_requested()) break;
              if (!result.impl_->present[row]) continue;
              try {
                const auto begin =
                    captured.ragged
                        ? captured.offsets[row]
                        : row * static_cast<index_t>(captured.components);
                const auto end = captured.ragged ? captured.offsets[row + 1]
                                                 : begin + captured.components;
                if (begin == end) continue;
                const auto* data =
                    values.Data() + static_cast<std::size_t>(begin);
                if (reinterpret_cast<std::uintptr_t>(data) % alignof(T))
                  throw Failure{"numerical.alignment"};
                const eigen_types::ConstVectorMap<T> mapped(
                    data, static_cast<eigen_types::Index>(end - begin));
                output[row] = mapped.unaryExpr(CheckedScalar<T>{stop, squared})
                                  .redux(CheckedAdd<T>{stop});
              } catch (const Failure& failure) {
                outcomes[row].failure = failure.code;
              } catch (...) {
                outcomes[row].exception = std::current_exception();
              }
            }
          };
          if (!workers.Count())
            compute(0, rows);
          else {
            OwnedArray<std::jthread> threads(execution, workers.Count(), stop,
                                             result.peak_bytes_);
            for (std::size_t i = 0; i < workers.Count(); ++i) {
              const auto first = rows / workers.Count() * i +
                                 std::min(i, rows % workers.Count());
              const auto last = rows / workers.Count() * (i + 1) +
                                std::min(i + 1, rows % workers.Count());
              threads[i] =
                  std::jthread([&, first, last] { compute(first, last); });
            }
            for (std::size_t i = 0; i < threads.size(); ++i) threads[i].join();
            result.workers_used_ = threads.size();
          }
        },
        captured.values);
    for (std::size_t row = 0; row < rows; ++row)
      if (outcomes[row].exception)
        std::rethrow_exception(outcomes[row].exception);
    CheckStop(stop);
    for (std::size_t row = 0; row < rows; ++row)
      if (outcomes[row].failure) throw Failure{outcomes[row].failure, row};
    result.status_ = AttributeReductionStatus::kAccepted;
    CheckStop(stop);
  } catch (const Failure& failure) {
    result.impl_.reset();
    result.status_ = stop.stop_requested() ? AttributeReductionStatus::kCanceled
                                           : AttributeReductionStatus::kBlocked;
    result.diagnostic_ = {result.status_ == AttributeReductionStatus::kCanceled
                              ? "numerical.cancelled"
                              : failure.code,
                          result.status_ == AttributeReductionStatus::kCanceled
                              ? std::nullopt
                              : failure.row};
  }
  return result;
}
AttributeReductionResult ComputeAttributeRowSums(
    const Attribute& source, index_t expected_rows,
    const AttributeReductionOptions& options, const ExecutionContext& execution,
    std::stop_token stop) {
  return AttributeReductionResult::Reduce(source, expected_rows, options,
                                          execution, stop, false);
}
AttributeReductionResult ComputeAttributeRowSquaredNorms(
    const Attribute& source, index_t expected_rows,
    const AttributeReductionOptions& options, const ExecutionContext& execution,
    std::stop_token stop) {
  return AttributeReductionResult::Reduce(source, expected_rows, options,
                                          execution, stop, true);
}
}  // namespace meshvale::geometry
