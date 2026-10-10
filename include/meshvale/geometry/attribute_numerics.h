// SPDX-License-Identifier: Apache-2.0
#ifndef MESHVALE_GEOMETRY_ATTRIBUTE_NUMERICS_H_
#define MESHVALE_GEOMETRY_ATTRIBUTE_NUMERICS_H_

#include <meshvale/geometry/attributes.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <stop_token>
#include <string_view>
#include <variant>

namespace meshvale::geometry {

class ExecutionContext;
enum class AttributeReductionStatus { kAccepted, kBlocked, kCanceled };
using AttributeScalar =
    std::variant<float, double, std::int32_t, std::uint8_t, std::uint16_t,
                 std::uint32_t, std::uint64_t>;
struct AttributeReductionDiagnostic {
  std::string_view code;
  std::optional<index_t> row;
};
struct AttributeReductionOptions {
  std::size_t minimum_parallel_rows = 1024;
};

// Immutable owned results and their payload charges. Presence/Diagnostics
// borrow until this result moves or dies. Default/moved-from results are
// blocked with no values. Moving/destruction alongside reads needs
// synchronization.
class AttributeReductionResult {
 public:
  AttributeReductionResult() noexcept;
  AttributeReductionResult(AttributeReductionResult&&) noexcept;
  AttributeReductionResult& operator=(AttributeReductionResult&&) noexcept;
  ~AttributeReductionResult() noexcept;
  AttributeReductionResult(const AttributeReductionResult&) = delete;
  AttributeReductionResult& operator=(const AttributeReductionResult&) = delete;
  [[nodiscard]] AttributeReductionStatus Status() const noexcept;
  // Get throws std::out_of_range for absent/out-of-range output. CopyValues
  // throws std::logic_error unless accepted; caller-owned exports are
  // uncharged.
  [[nodiscard]] AttributeScalar Get(index_t row) const;
  [[nodiscard]] AttributeValues CopyValues() const;
  [[nodiscard]] std::span<const std::uint8_t> Presence() const noexcept;
  [[nodiscard]] std::span<const AttributeReductionDiagnostic> Diagnostics()
      const noexcept;
  [[nodiscard]] std::size_t WorkersUsed() const noexcept;
  [[nodiscard]] std::string_view SerialReason() const noexcept;
  [[nodiscard]] std::size_t PeakTrackedPayloadBytes() const noexcept;

 private:
  struct Impl;
  struct ImplDeleter {
    void operator()(Impl* impl) const noexcept;
  };
  static AttributeReductionResult Reduce(
      const Attribute& source, index_t expected_rows,
      const AttributeReductionOptions& options,
      const ExecutionContext& execution, std::stop_token stop, bool squared);
  friend AttributeReductionResult ComputeAttributeRowSums(
      const Attribute&, index_t, const AttributeReductionOptions&,
      const ExecutionContext&, std::stop_token);
  friend AttributeReductionResult ComputeAttributeRowSquaredNorms(
      const Attribute&, index_t, const AttributeReductionOptions&,
      const ExecutionContext&, std::stop_token);
  std::unique_ptr<Impl, ImplDeleter> impl_;
  AttributeReductionStatus status_ = AttributeReductionStatus::kBlocked;
  std::optional<AttributeReductionDiagnostic> diagnostic_;
  std::size_t workers_used_ = 0;
  std::string_view serial_reason_;
  std::size_t peak_bytes_ = 0;
};

// Reduce all scalars of each dense/ragged row in its source encoding. Missing
// rows remain unauthored; authored empty rows yield zero. Complete shape/host
// extents precede lexical Eigen access. Integer intermediates are checked;
// authored floating inputs/results must be finite. No normalization or casts.
// Reads capture an immutable owner; caller must prevent source mutation during
// capture. Joined workers and retained output charges share execution's budget.
// Expected shape/arithmetic/budget failures block; cancellation exposes no
// partial values. Allocation/thread failures propagate, with RAII cleanup.
// See docs/numerics.md for arithmetic ordering and declared-payload exclusions.
[[nodiscard]] AttributeReductionResult ComputeAttributeRowSums(
    const Attribute& source, index_t expected_rows,
    const AttributeReductionOptions& options, const ExecutionContext& execution,
    std::stop_token stop = {});
[[nodiscard]] AttributeReductionResult ComputeAttributeRowSquaredNorms(
    const Attribute& source, index_t expected_rows,
    const AttributeReductionOptions& options, const ExecutionContext& execution,
    std::stop_token stop = {});

}  // namespace meshvale::geometry
#endif  // MESHVALE_GEOMETRY_ATTRIBUTE_NUMERICS_H_
