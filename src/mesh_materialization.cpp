// SPDX-License-Identifier: Apache-2.0
#include <meshvale/geometry/attributes.h>
#include <meshvale/geometry/execution.h>
#include <meshvale/geometry/materialization.h>
#include <meshvale/geometry/position_buffer.h>
#include <meshvale/geometry/vector_buffer.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <exception>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <stop_token>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>

#include "materialization_allocation.h"
#include "materialization_storage.h"
#include "materialization_test_internal.h"
#include "materialization_workers.h"
#include "numeric_buffer_internal.h"

namespace meshvale::geometry::materialization_detail {
AttributeState::AttributeState(const AllocationContext& context)
    : allocation(context), metadata(Allocator<MetadataEntryView>(context)) {}
AttributeReadView AttributeState::View() const noexcept {
  ScalarReadView scalars = std::visit(
      [](const auto& buffer) -> ScalarReadView { return buffer.Values(); },
      values);
  return {domain,
          name,
          semantic,
          set_index,
          components,
          scalars,
          has_offsets ? std::optional(offsets.Values()) : std::nullopt,
          has_present ? std::optional(present.Values()) : std::nullopt,
          metadata};
}
VertexState::VertexState(const AllocationContext& context)
    : allocation(context) {}
MeshState::MeshState(const AllocationContext& allocation,
                     const ExecutionContext& context)
    : allocation(allocation),
      context(context),
      attributes(Allocator<std::shared_ptr<const AttributeState>>(allocation)),
      views(Allocator<AttributeReadView>(allocation)) {}
InspectionState::InspectionState(const AllocationContext& allocation,
                                 const MaterializedMesh& source)
    : allocation(allocation),
      source(source),
      issues(Allocator<StorageIssue>(allocation)) {}
SelectionState::SelectionState(const AllocationContext& allocation,
                               const MaterializedMesh& candidate)
    : allocation(allocation), candidate(candidate) {}
const MeshState& Access::State(const MaterializedMesh& mesh) noexcept {
  return *mesh.state_;
}
MaterializedMesh Access::MakeMesh(std::shared_ptr<const MeshState> state) {
  return MaterializedMesh(std::move(state));
}
MeshInspection Access::MakeInspection(
    std::shared_ptr<const InspectionState> state) {
  return MeshInspection(std::move(state));
}
MaterializedFaceSelection Access::MakeSelection(
    std::shared_ptr<const SelectionState> state) {
  return MaterializedFaceSelection(std::move(state));
}
namespace {
constexpr std::size_t kCornersPerTriangle = 3;
constexpr index_t kMinimumFaceCorners = 3;
struct ExpectedFailure {
  MaterializationStatus status;
  MaterializationDiagnostic diagnostic;
};
[[noreturn]] void Fail(MaterializationStatus status,
                       MaterializationDiagnosticCode code,
                       std::optional<index_t> attribute = {},
                       std::optional<index_t> element = {}) {
  throw ExpectedFailure{status, {code, attribute, element, {}}};
}
std::size_t CheckedSum(std::size_t a, std::size_t b) {
  if (b > std::numeric_limits<std::size_t>::max() - a)
    Fail(MaterializationStatus::kLimitExceeded,
         MaterializationDiagnosticCode::kStorageExtent);
  return a + b;
}
template <class... Values>
void RequiredConfiguration(Values... values) {
  for (const auto value : std::array{values...}) {
    if (value == 0)
      Fail(MaterializationStatus::kInvalidConfiguration,
           MaterializationDiagnosticCode::kRequiredFieldZero);
    if (value > std::numeric_limits<std::size_t>::max())
      Fail(MaterializationStatus::kInvalidConfiguration,
           MaterializationDiagnosticCode::kConfigurationExtent);
  }
}
void ValidateWork(const MeshMaterializationWorkPolicy& work) {
  RequiredConfiguration(work.parallel_min_bytes, work.partition_bytes,
                        work.cancellation_block_scalars, work.max_jobs);
}
void ValidateLimits(const MeshMaterializationLimits& limits) {
  RequiredConfiguration(
      limits.max_vertices, limits.max_faces, limits.max_corners,
      limits.max_face_offsets, limits.max_attributes,
      limits.max_attribute_scalars, limits.max_attribute_offsets,
      limits.max_presence_bytes, limits.max_metadata_entries,
      limits.max_descriptor_bytes, limits.max_new_requested_bytes);
}
void CountLimit(std::size_t count, std::uint64_t limit,
                std::optional<index_t> attribute = {}) {
  if (count > limit)
    Fail(MaterializationStatus::kLimitExceeded,
         MaterializationDiagnosticCode::kCountLimit, attribute);
}
struct InputCounts {
  std::size_t vertices = 0;
  std::size_t faces = 0;
  std::size_t corners = 0;
  std::size_t face_offsets = 0;
};
void CheckPlan(const numeric_detail::RequestPlan& plan,
               std::optional<index_t> attribute = {}) {
  if (plan.implementation_bytes != 0 && !plan.qualified)
    Fail(MaterializationStatus::kUnsupportedAllocationProfile,
         MaterializationDiagnosticCode::kAllocationProfile, attribute);
  (void)CheckedSum(plan.implementation_bytes, plan.numerical_bytes);
}
template <class T>
using CanonicalScalar = std::conditional_t<std::is_same_v<T, std::int8_t> ||
                                               std::is_same_v<T, std::int16_t>,
                                           std::int32_t, T>;
void ValidationCheckpoint(std::stop_token stop) {
  if (stop.stop_requested()) throw AdmissionFailure(AdmissionCode::kCanceled);
}
template <class Planner>
void ValidateNumericPlan(Planner planner,
                         std::optional<index_t> attribute = {}) {
  // Only the pure extent planner's length error is an expected host limit.
  // Exceptions from allocation backends and workers keep their original type.
  try {
    CheckPlan(planner(), attribute);
  } catch (const std::length_error&) {
    Fail(MaterializationStatus::kLimitExceeded,
         MaterializationDiagnosticCode::kStorageExtent, attribute);
  }
}
InputCounts ValidateInput(const MeshMaterializationInput& input,
                          const MeshMaterializationLimits& limits,
                          const MeshMaterializationWorkPolicy& work,
                          std::stop_token stop) {
  InputCounts counts;
  if (std::holds_alternative<std::monostate>(input.positions) ||
      std::holds_alternative<std::monostate>(input.topology))
    Fail(MaterializationStatus::kInvalidTransport,
         MaterializationDiagnosticCode::kTransportSelector);
  std::visit(
      [&](const auto& values) {
        using Span = std::decay_t<decltype(values)>;
        if constexpr (!std::is_same_v<Span, std::monostate>) {
          if (values.size() % kVector3Components != 0)
            Fail(MaterializationStatus::kInvalidTransport,
                 MaterializationDiagnosticCode::kPositionExtent);
          counts.vertices = values.size() / kVector3Components;
        }
      },
      input.positions);
  std::visit(
      [&](const auto& topology) {
        using Topology = std::decay_t<decltype(topology)>;
        if constexpr (std::is_same_v<Topology, PolygonTopologyInput>) {
          counts.face_offsets = topology.face_offsets.size();
          counts.faces = counts.face_offsets == 0 ? 0 : counts.face_offsets - 1;
          counts.corners = topology.corner_vertices.size();
        } else if constexpr (std::is_same_v<Topology, TriangleTopologyInput>) {
          counts.corners = topology.corner_vertices.size();
          if (counts.corners % kCornersPerTriangle != 0)
            Fail(MaterializationStatus::kInvalidTransport,
                 MaterializationDiagnosticCode::kTriangleExtent);
          counts.faces = counts.corners / kCornersPerTriangle;
          counts.face_offsets = CheckedSum(counts.faces, 1);
        }
      },
      input.topology);
  CountLimit(counts.vertices, limits.max_vertices);
  CountLimit(counts.faces, limits.max_faces);
  CountLimit(counts.corners, limits.max_corners);
  CountLimit(counts.face_offsets, limits.max_face_offsets);
  CountLimit(input.attributes.size(), limits.max_attributes);
  std::size_t scalars = 0, offsets = 0, presence = 0, metadata = 0, text = 0;
  for (std::size_t ordinal = 0; ordinal < input.attributes.size(); ++ordinal) {
    if (ordinal % work.cancellation_block_scalars == 0)
      ValidationCheckpoint(stop);
    const auto& attribute = input.attributes[ordinal];
    if (std::holds_alternative<std::monostate>(attribute.values))
      Fail(MaterializationStatus::kInvalidTransport,
           MaterializationDiagnosticCode::kTransportSelector, ordinal);
    std::visit(
        [&](const auto& values) {
          using Span = std::decay_t<decltype(values)>;
          if constexpr (!std::is_same_v<Span, std::monostate>)
            scalars = CheckedSum(scalars, values.size());
        },
        attribute.values);
    if (attribute.offsets)
      offsets = CheckedSum(offsets, attribute.offsets->size());
    if (attribute.present)
      presence = CheckedSum(presence, attribute.present->size());
    metadata = CheckedSum(metadata, attribute.metadata.size());
    text = CheckedSum(text, attribute.name.size());
    text = CheckedSum(text, attribute.semantic.size());
    for (std::size_t i = 0; i < attribute.metadata.size(); ++i) {
      if (i % work.cancellation_block_scalars == 0) ValidationCheckpoint(stop);
      const auto& entry = attribute.metadata[i];
      text = CheckedSum(text, entry.key.size());
      text = CheckedSum(text, entry.value.size());
    }
    CountLimit(scalars, limits.max_attribute_scalars, ordinal);
    CountLimit(offsets, limits.max_attribute_offsets, ordinal);
    CountLimit(presence, limits.max_presence_bytes, ordinal);
    CountLimit(metadata, limits.max_metadata_entries, ordinal);
    CountLimit(text, limits.max_descriptor_bytes, ordinal);
  }
  // All transport/count checks precede profile planning and allocation.
  ValidationCheckpoint(stop);
  ValidateNumericPlan(
      [&] { return numeric_detail::Access::PositionPlan(counts.vertices); });
  for (std::size_t ordinal = 0; ordinal < input.attributes.size(); ++ordinal)
    std::visit(
        [&](const auto& values) {
          using Span = std::decay_t<decltype(values)>;
          if constexpr (!std::is_same_v<Span, std::monostate>) {
            using T = CanonicalScalar<typename Span::value_type>;
            ValidationCheckpoint(stop);
            ValidateNumericPlan(
                [&] {
                  return numeric_detail::Access::ScalarPlan<T>(values.size());
                },
                ordinal);
          }
        },
        input.attributes[ordinal].values);
  return counts;
}
struct WorkSession {
  const ExecutionContext& context;
  const AllocationContext& allocation;
  const MeshMaterializationWorkPolicy& work;
  std::stop_token stop;
  MaterializationExecutionStats& stats;
  const TestHooks& hooks;
  mutable bool completing_source_failure = false;
  void SourceCheckpoint() const {
    if (!completing_source_failure) allocation.Checkpoint();
  }
  void Run(std::size_t count, std::size_t scalar_bytes,
           RangeOperation operation, void* state) const {
    RunRange(context, allocation, work, count, scalar_bytes, operation, state,
             stop, stats, hooks.workers, hooks.execution_observer);
  }
};
template <class T>
void CopyScalars(const WorkSession& session, std::span<const T> input,
                 std::span<T> output) {
  struct Job {
    const WorkSession& session;
    std::span<const T> input;
    std::span<T> output;
  } job{session, input, output};
  session.Run(
      input.size(), sizeof(T),
      [](void* argument, std::size_t begin, std::size_t end) {
        const auto& job = *static_cast<Job*>(argument);
        while (begin < end) {
          job.session.allocation.Checkpoint();
          const auto count = std::min<std::size_t>(
              end - begin, job.session.work.cancellation_block_scalars);
          std::memcpy(job.output.data() + begin, job.input.data() + begin,
                      count * sizeof(T));
          begin += count;
        }
      },
      &job);
}
template <class Source, class Destination, class Conversion>
void ConvertScalars(const WorkSession& session, std::span<const Source> input,
                    std::span<Destination> output, Conversion conversion) {
  struct Job {
    const WorkSession& session;
    std::span<const Source> input;
    std::span<Destination> output;
    Conversion conversion;
  } job{session, input, output, conversion};
  session.Run(
      input.size(), sizeof(Destination),
      [](void* argument, std::size_t begin, std::size_t end) {
        const auto& job = *static_cast<Job*>(argument);
        const auto block = job.session.work.cancellation_block_scalars;
        for (auto i = begin; i < end; ++i) {
          if ((i - begin) % block == 0) job.session.allocation.Checkpoint();
          job.conversion(job.input[i], job.output[i]);
        }
      },
      &job);
}
void WidenPosition(const float& source, double& destination) noexcept {
  constexpr unsigned kSourceFractionBits = 23;
  constexpr unsigned kDestinationFractionBits = 52;
  constexpr unsigned kSignShift = 32;
  constexpr std::uint32_t kSourceSign = UINT32_C(0x80000000);
  constexpr std::uint32_t kSourceFraction = UINT32_C(0x007fffff);
  constexpr std::uint32_t kSourceExponent = UINT32_C(0xff);
  constexpr std::uint64_t kDestinationFraction = UINT64_C(0x000fffffffffffff);
  constexpr std::uint64_t kDestinationInfinite = UINT64_C(0x7ff0000000000000);
  constexpr int kSourceBias = 127;
  constexpr int kDestinationBias = 1023;
  constexpr int kSourceSubnormalExponent =
      1 - kSourceBias - static_cast<int>(kSourceFractionBits);
  std::uint32_t bits;
  std::memcpy(&bits, &source, sizeof(bits));
  auto result = static_cast<std::uint64_t>(bits & kSourceSign) << kSignShift;
  const auto exponent = (bits >> kSourceFractionBits) & kSourceExponent;
  const auto fraction = bits & kSourceFraction;
  if (exponent == kSourceExponent) {
    result |= kDestinationInfinite |
              (static_cast<std::uint64_t>(fraction)
               << (kDestinationFractionBits - kSourceFractionBits));
  } else if (exponent != 0) {
    const auto widened =
        static_cast<int>(exponent) - kSourceBias + kDestinationBias;
    result |=
        (static_cast<std::uint64_t>(widened) << kDestinationFractionBits) |
        (static_cast<std::uint64_t>(fraction)
         << (kDestinationFractionBits - kSourceFractionBits));
  } else if (fraction != 0) {
    const auto leading = static_cast<unsigned>(std::bit_width(fraction) - 1);
    const auto widened =
        static_cast<int>(leading) + kSourceSubnormalExponent + kDestinationBias;
    result |=
        (static_cast<std::uint64_t>(widened) << kDestinationFractionBits) |
        ((static_cast<std::uint64_t>(fraction)
          << (kDestinationFractionBits - leading)) &
         kDestinationFraction);
  }
  std::memcpy(&destination, &result, sizeof(result));
}
void CaptureDescriptor(const WorkSession& session,
                       const AttributeMaterializationInput& input,
                       AttributeState& result) {
  auto text_size = CheckedSum(input.name.size(), input.semantic.size());
  for (std::size_t i = 0; i < input.metadata.size(); ++i) {
    if (i % session.work.cancellation_block_scalars == 0)
      session.allocation.Checkpoint();
    const auto& entry = input.metadata[i];
    text_size = CheckedSum(text_size, entry.key.size());
    text_size = CheckedSum(text_size, entry.value.size());
  }
  result.text = FlatArray<char>(session.allocation, text_size);
  result.metadata.reserve(input.metadata.size());
  std::size_t offset = 0;
  auto copy_text = [&](std::string_view text) {
    auto destination = result.text.Values().subspan(offset, text.size());
    CopyScalars(session, std::span<const char>(text.data(), text.size()),
                destination);
    offset += text.size();
    return std::string_view(destination.data(), destination.size());
  };
  result.name = copy_text(input.name);
  result.semantic = copy_text(input.semantic);
  for (const auto& entry : input.metadata)
    result.metadata.push_back({copy_text(entry.key), copy_text(entry.value)});
}
std::shared_ptr<const AttributeState> CaptureAttribute(
    const WorkSession& session, const AttributeMaterializationInput& input) {
  const auto& allocation = session.allocation;
  auto result = std::allocate_shared<AttributeState>(
      Allocator<AttributeState>(allocation), allocation);
  result->domain = input.domain;
  result->components = input.components;
  result->set_index = input.set_index;
  std::visit(
      [&](const auto& values) {
        using Span = std::decay_t<decltype(values)>;
        if constexpr (!std::is_same_v<Span, std::monostate>) {
          using Source = typename Span::value_type;
          using Destination = CanonicalScalar<Source>;
          const auto plan =
              numeric_detail::Access::ScalarPlan<Destination>(values.size());
          result->numerical_lease = allocation.ReserveOriginalRequest(
              CheckedSum(plan.implementation_bytes, plan.numerical_bytes));
          auto buffer = numeric_detail::Access::AllocateScalars<Destination>(
              values.size());
          if constexpr (std::is_same_v<Source, Destination>)
            CopyScalars(session, values, buffer.Values());
          else
            ConvertScalars(session, values, buffer.Values(),
                           [](const Source& source, Destination& destination) {
                             destination = static_cast<Destination>(source);
                           });
          result->values = std::move(buffer);
        }
      },
      input.values);
  result->has_offsets = input.offsets.has_value();
  if (input.offsets) {
    result->offsets = FlatArray<index_t>(allocation, input.offsets->size());
    CopyScalars(session, *input.offsets, result->offsets.Values());
  }
  result->has_present = input.present.has_value();
  if (input.present) {
    result->present =
        FlatArray<std::uint8_t>(allocation, input.present->size());
    CopyScalars(session, *input.present, result->present.Values());
  }
  CaptureDescriptor(session, input, *result);
  return result;
}
int CompareText(const WorkSession& session, std::string_view first,
                std::string_view second) {
  const auto common = std::min(first.size(), second.size());
  for (std::size_t offset = 0; offset < common;) {
    session.allocation.Checkpoint();
    const auto count = std::min<std::size_t>(
        common - offset, session.work.cancellation_block_scalars);
    const auto compared =
        std::memcmp(first.data() + offset, second.data() + offset, count);
    if (compared != 0) return compared;
    offset += count;
  }
  return first.size() < second.size()   ? -1
         : first.size() > second.size() ? 1
                                        : 0;
}
template <class Compare>
FlatArray<std::uint8_t> DuplicateFlags(const WorkSession& session,
                                       std::size_t count, Compare compare) {
  FlatArray<index_t> order(session.allocation, count);
  FlatArray<std::uint8_t> flags(session.allocation, count);
  struct Initialize {
    const WorkSession& session;
    std::span<index_t> order;
    std::span<std::uint8_t> flags;
  } initialize{session, order.Values(), flags.Values()};
  session.Run(
      count, sizeof(index_t) + sizeof(std::uint8_t),
      [](void* argument, std::size_t begin, std::size_t end) {
        const auto& job = *static_cast<Initialize*>(argument);
        for (auto i = begin; i < end; ++i) {
          if ((i - begin) % job.session.work.cancellation_block_scalars == 0)
            job.session.allocation.Checkpoint();
          job.order[i] = i;
          job.flags[i] = 0;
        }
      },
      &initialize);
  // Ordered duplicate planning keeps the earliest authored occurrence. Sorting
  // uses only the charged index array and bounded stack, without a name set.
  if (count != 0)
    std::sort(order.Values().begin(), order.Values().end(),
              [&](index_t a, index_t b) {
                session.allocation.Checkpoint();
                const auto compared = compare(a, b);
                return compared < 0 || (compared == 0 && a < b);
              });
  struct Mark {
    const WorkSession& session;
    std::span<const index_t> order;
    std::span<std::uint8_t> flags;
    Compare& compare;
  } mark{session, order.Values(), flags.Values(), compare};
  session.Run(
      count, sizeof(index_t) + sizeof(std::uint8_t),
      [](void* argument, std::size_t begin, std::size_t end) {
        const auto& job = *static_cast<Mark*>(argument);
        for (auto i = begin; i < end; ++i) {
          if ((i - begin) % job.session.work.cancellation_block_scalars == 0)
            job.session.allocation.Checkpoint();
          if (i != 0 && job.compare(job.order[i - 1], job.order[i]) == 0)
            job.flags[job.order[i]] = 1;
        }
      },
      &mark);
  return flags;
}
struct ScanBlock {
  explicit ScanBlock(const AllocationContext& allocation)
      : issues(Allocator<StorageIssue>(allocation)) {}
  ChargedVector<StorageIssue> issues;
  std::optional<StorageIssue> first;
  std::size_t dispatched_end = 0;
};
struct ScanOutput {
  ScanOutput(const AllocationContext& allocation, std::size_t maximum,
             bool first_only)
      : issues(Allocator<StorageIssue>(allocation)),
        maximum(maximum),
        first_only(first_only) {}
  ChargedVector<StorageIssue> issues;
  std::optional<StorageIssue> first;
  std::size_t maximum;
  bool first_only;
};
struct IssueSink {
  ScanBlock& block;
  std::atomic<std::size_t>& count;
  std::size_t maximum;
  bool first_only;
  const TestHooks* hooks = nullptr;
  void Add(StorageIssueCode code, std::optional<index_t> attribute = {},
           std::optional<index_t> element = {}) {
    StorageIssue issue{code, attribute, element};
    if (!block.first) {
      block.first = issue;
      if (first_only && hooks && hooks->source_issue_observer)
        hooks->source_issue_observer(issue, hooks->source_issue_observer_state);
    }
    if (first_only) return;
    auto current = count.load(std::memory_order_relaxed);
    do {
      if (current >= maximum)
        Fail(MaterializationStatus::kLimitExceeded,
             MaterializationDiagnosticCode::kIssueLimit);
    } while (!count.compare_exchange_weak(current, current + 1,
                                          std::memory_order_relaxed));
    block.issues.push_back(issue);
  }
};
using RangeCheck = void (*)(void*, std::size_t, std::size_t, IssueSink&);
void ScanRange(const WorkSession& session, std::size_t count,
               std::size_t row_bytes, RangeCheck check, void* state,
               ScanOutput& output) {
  if (count == 0) return;
  const auto target =
      std::max<std::size_t>(1, session.work.partition_bytes / row_bytes);
  const auto jobs = std::min<std::size_t>(
      count / target + (count % target != 0 ? 1 : 0), session.work.max_jobs);
  ChargedVector<ScanBlock> blocks{Allocator<ScanBlock>(session.allocation)};
  blocks.reserve(jobs);
  for (std::size_t i = 0; i < jobs; ++i)
    blocks.emplace_back(session.allocation);
  std::atomic<std::size_t> total{output.issues.size()};
  struct ScanJob {
    std::size_t count;
    std::size_t jobs;
    RangeCheck check;
    void* state;
    std::span<ScanBlock> blocks;
    std::atomic<std::size_t>& total;
    ScanOutput& output;
    const TestHooks& hooks;
  } job{count, jobs, check, state, blocks, total, output, session.hooks};
  auto retain_first = [&] {
    for (const auto& block : blocks)
      if (!output.first && block.first) output.first = block.first;
  };
  try {
    session.Run(
        count, row_bytes,
        [](void* argument, std::size_t begin, std::size_t end) {
          auto& job = *static_cast<ScanJob*>(argument);
          const auto quotient = job.count / job.jobs;
          const auto remainder = job.count % job.jobs;
          const auto leading = (quotient + 1) * remainder;
          const auto ordinal = begin < leading
                                   ? begin / (quotient + 1)
                                   : remainder + (begin - leading) / quotient;
          job.blocks[ordinal].dispatched_end = end;
          IssueSink sink{job.blocks[ordinal], job.total, job.output.maximum,
                         job.output.first_only, &job.hooks};
          job.check(job.state, begin, end, sink);
        },
        &job);
  } catch (const AdmissionFailure& failure) {
    if (!output.first_only || failure.Code() != AdmissionCode::kCanceled) throw;
    // RunRange has joined and checked all actual runtime failures. A known
    // defect fixes failure status, but canceled earlier rows cannot be assumed
    // valid. Finish only the required source prefix without allocating or
    // launching again, preserving the first source-order diagnostic.
    std::size_t known_block = 0;
    while (known_block < blocks.size() && !blocks[known_block].first)
      ++known_block;
    if (known_block == blocks.size()) throw;
    // The caller fallback may dispatch (0,count) despite multiple planned
    // jobs, so only the recorded actual range bounds certify prefix coverage.
    const auto prefix_end = blocks[known_block].dispatched_end;
    struct SourceCompletion {
      const WorkSession& session;
      explicit SourceCompletion(const WorkSession& session) : session(session) {
        session.completing_source_failure = true;
      }
      ~SourceCompletion() { session.completing_source_failure = false; }
    } completing(session);
    auto& prefix = blocks.front();
    prefix.first.reset();
    IssueSink sink{prefix, total, output.maximum, true};
    for (std::size_t row = 0; row < prefix_end; ++row) {
      check(state, row, row + 1, sink);
      if (prefix.first) {
        output.first = prefix.first;
        return;
      }
    }
    // Immutable deterministic checks must reproduce the known defect. Retain
    // cancellation if a private caller violates that prerequisite.
    throw;
  }
  retain_first();
  if (output.first_only) return;
  const auto old_size = output.issues.size();
  output.issues.resize(total.load(std::memory_order_relaxed));
  std::size_t offset = old_size;
  for (const auto& block : blocks) {
    CopyScalars(session, std::span<const StorageIssue>(block.issues),
                std::span<StorageIssue>(output.issues)
                    .subspan(offset, block.issues.size()));
    offset += block.issues.size();
  }
}
void SingleIssue(const WorkSession& session, ScanOutput& output,
                 StorageIssueCode code, std::optional<index_t> attribute = {}) {
  // First-only admission has already finished once an aggregate defect exists.
  // Do not let a later aggregate's checkpoint replace that established failure.
  if (output.first_only && output.first) return;
  session.allocation.Checkpoint();
  if (!output.first) {
    output.first = StorageIssue{code, attribute, {}};
    if (output.first_only && session.hooks.source_issue_observer)
      session.hooks.source_issue_observer(
          *output.first, session.hooks.source_issue_observer_state);
  }
  if (!output.first_only) {
    if (output.issues.size() >= output.maximum)
      Fail(MaterializationStatus::kLimitExceeded,
           MaterializationDiagnosticCode::kIssueLimit);
    output.issues.push_back({code, attribute, {}});
  }
}
bool StopAtFirst(const ScanOutput& output) noexcept {
  return output.first_only && output.first.has_value();
}
ScanOutput InspectStorage(const WorkSession& session, MeshReadView mesh,
                          std::size_t maximum, bool first_only) {
  ScanOutput output(session.allocation, maximum, first_only);
  struct MeshCheck {
    const WorkSession& session;
    MeshReadView mesh;
  } check{session, mesh};
  const auto vertices = mesh.positions.size() / kVector3Components;
  ScanRange(
      session, vertices, kVector3Components * sizeof(double),
      [](void* argument, std::size_t begin, std::size_t end, IssueSink& sink) {
        auto& job = *static_cast<MeshCheck*>(argument);
        constexpr std::uint64_t kNonfiniteExponent =
            UINT64_C(0x7ff0000000000000);
        for (auto i = begin; i < end; ++i) {
          if ((i - begin) % job.session.work.cancellation_block_scalars == 0)
            job.session.SourceCheckpoint();
          for (int component = 0; component < kVector3Components; ++component) {
            std::uint64_t bits;
            std::memcpy(&bits,
                        &job.mesh.positions[i * kVector3Components + component],
                        sizeof(bits));
            if ((bits & kNonfiniteExponent) == kNonfiniteExponent) {
              sink.Add(StorageIssueCode::kPositionNonfinite, {}, i);
              break;
            }
          }
        }
      },
      &check, output);
  if (StopAtFirst(output)) return output;
  const auto offsets = mesh.face_offsets;
  if (offsets.empty()) {
    SingleIssue(session, output, StorageIssueCode::kFaceOffsetsEmpty);
  } else {
    if (offsets.front() != 0)
      SingleIssue(session, output, StorageIssueCode::kFaceOffsetsStart);
    if (offsets.back() != mesh.corner_vertices.size())
      SingleIssue(session, output, StorageIssueCode::kFaceOffsetsEnd);
    if (StopAtFirst(output)) return output;
    ScanRange(
        session, offsets.size(), sizeof(index_t),
        [](void* argument, std::size_t begin, std::size_t end,
           IssueSink& sink) {
          auto& job = *static_cast<MeshCheck*>(argument);
          for (auto i = begin; i < end; ++i) {
            if ((i - begin) % job.session.work.cancellation_block_scalars == 0)
              job.session.SourceCheckpoint();
            const auto value = job.mesh.face_offsets[i];
            if (value > job.mesh.corner_vertices.size())
              sink.Add(StorageIssueCode::kFaceOffsetRange, {}, i);
            if (i != 0) {
              const auto previous = job.mesh.face_offsets[i - 1];
              if (value < previous)
                sink.Add(StorageIssueCode::kFaceOffsetOrder, {}, i);
              else if (value - previous < kMinimumFaceCorners)
                sink.Add(StorageIssueCode::kFaceTooShort, {}, i - 1);
            }
          }
        },
        &check, output);
  }
  if (StopAtFirst(output)) return output;
  ScanRange(
      session, mesh.corner_vertices.size(), sizeof(index_t),
      [](void* argument, std::size_t begin, std::size_t end, IssueSink& sink) {
        auto& job = *static_cast<MeshCheck*>(argument);
        const auto vertices = job.mesh.positions.size() / kVector3Components;
        for (auto i = begin; i < end; ++i) {
          if ((i - begin) % job.session.work.cancellation_block_scalars == 0)
            job.session.SourceCheckpoint();
          if (job.mesh.corner_vertices[i] >= vertices)
            sink.Add(StorageIssueCode::kCornerVertexRange, {}, i);
        }
      },
      &check, output);
  if (StopAtFirst(output)) return output;
  auto duplicate_names = DuplicateFlags(
      session, mesh.attributes.size(), [&](index_t a, index_t b) {
        const auto& first = mesh.attributes[a];
        const auto& second = mesh.attributes[b];
        if (first.domain != second.domain)
          return static_cast<std::underlying_type_t<AttributeDomain>>(
                     first.domain) <
                         static_cast<std::underlying_type_t<AttributeDomain>>(
                             second.domain)
                     ? -1
                     : 1;
        return CompareText(session, first.name, second.name);
      });
  for (std::size_t ordinal = 0; ordinal < mesh.attributes.size(); ++ordinal) {
    session.allocation.Checkpoint();
    const auto& attribute = mesh.attributes[ordinal];
    if (duplicate_names.Values()[ordinal] != 0)
      SingleIssue(session, output, StorageIssueCode::kAttributeDuplicateName,
                  ordinal);
    std::optional<std::size_t> rows;
    switch (attribute.domain) {
      case AttributeDomain::vertex:
        rows = vertices;
        break;
      case AttributeDomain::face:
        rows = offsets.empty() ? 0 : offsets.size() - 1;
        break;
      case AttributeDomain::corner:
        rows = mesh.corner_vertices.size();
        break;
      default:
        SingleIssue(session, output, StorageIssueCode::kAttributeDomain,
                    ordinal);
        break;
    }
    if (attribute.name.empty())
      SingleIssue(session, output, StorageIssueCode::kAttributeNameEmpty,
                  ordinal);
    if (attribute.components == 0)
      SingleIssue(session, output, StorageIssueCode::kAttributeComponentsZero,
                  ordinal);
    if (StopAtFirst(output)) return output;
    const auto scalar_count = std::visit(
        [](const auto& values) { return values.size(); }, attribute.values);
    struct AttributeCheck {
      const WorkSession& session;
      const AttributeReadView& attribute;
      std::size_t scalar_count;
      index_t ordinal;
    } attribute_check{session, attribute, scalar_count, ordinal};
    if (attribute.offsets) {
      const auto row_offsets = *attribute.offsets;
      if (row_offsets.empty()) {
        SingleIssue(session, output, StorageIssueCode::kAttributeOffsetsEmpty,
                    ordinal);
      } else {
        if (rows && row_offsets.size() - 1 != *rows)
          SingleIssue(session, output, StorageIssueCode::kAttributeOffsetsCount,
                      ordinal);
        if (row_offsets.front() != 0)
          SingleIssue(session, output, StorageIssueCode::kAttributeOffsetsStart,
                      ordinal);
        if (row_offsets.back() != scalar_count)
          SingleIssue(session, output, StorageIssueCode::kAttributeOffsetsEnd,
                      ordinal);
        if (StopAtFirst(output)) return output;
        ScanRange(
            session, row_offsets.size(), sizeof(index_t),
            [](void* argument, std::size_t begin, std::size_t end,
               IssueSink& sink) {
              auto& job = *static_cast<AttributeCheck*>(argument);
              const auto offsets = *job.attribute.offsets;
              for (auto i = begin; i < end; ++i) {
                if ((i - begin) % job.session.work.cancellation_block_scalars ==
                    0)
                  job.session.SourceCheckpoint();
                if (offsets[i] > job.scalar_count)
                  sink.Add(StorageIssueCode::kAttributeOffsetRange, job.ordinal,
                           i);
                if (i != 0 && offsets[i] < offsets[i - 1])
                  sink.Add(StorageIssueCode::kAttributeOffsetOrder, job.ordinal,
                           i);
                if (job.attribute.components != 0 &&
                    offsets[i] % job.attribute.components != 0)
                  sink.Add(StorageIssueCode::kAttributeOffsetAlignment,
                           job.ordinal, i);
              }
            },
            &attribute_check, output);
      }
    } else if (rows && attribute.components != 0 &&
               (scalar_count % attribute.components != 0 ||
                scalar_count / attribute.components != *rows)) {
      SingleIssue(session, output, StorageIssueCode::kAttributeDenseShape,
                  ordinal);
    }
    if (StopAtFirst(output)) return output;
    if (attribute.present) {
      if (rows && attribute.present->size() != *rows)
        SingleIssue(session, output, StorageIssueCode::kAttributePresenceCount,
                    ordinal);
      if (StopAtFirst(output)) return output;
      ScanRange(
          session, attribute.present->size(), sizeof(std::uint8_t),
          [](void* argument, std::size_t begin, std::size_t end,
             IssueSink& sink) {
            auto& job = *static_cast<AttributeCheck*>(argument);
            const auto present = *job.attribute.present;
            for (auto i = begin; i < end; ++i) {
              if ((i - begin) % job.session.work.cancellation_block_scalars ==
                  0)
                job.session.SourceCheckpoint();
              if (present[i] > 1)
                sink.Add(StorageIssueCode::kAttributePresenceValue, job.ordinal,
                         i);
            }
          },
          &attribute_check, output);
    }
    if (StopAtFirst(output)) return output;
    auto duplicate_keys = DuplicateFlags(
        session, attribute.metadata.size(), [&](index_t a, index_t b) {
          return CompareText(session, attribute.metadata[a].key,
                             attribute.metadata[b].key);
        });
    struct MetadataCheck {
      const WorkSession& session;
      std::span<const std::uint8_t> flags;
      index_t ordinal;
    } metadata_check{session, duplicate_keys.Values(), ordinal};
    ScanRange(
        session, attribute.metadata.size(), sizeof(MetadataEntryView),
        [](void* argument, std::size_t begin, std::size_t end,
           IssueSink& sink) {
          const auto& job = *static_cast<MetadataCheck*>(argument);
          for (auto i = begin; i < end; ++i) {
            if ((i - begin) % job.session.work.cancellation_block_scalars == 0)
              job.session.SourceCheckpoint();
            if (job.flags[i] != 0)
              sink.Add(StorageIssueCode::kMetadataDuplicateKey, job.ordinal, i);
          }
        },
        &metadata_check, output);
    if (StopAtFirst(output)) return output;
  }
  return output;
}
void PrefixOffsets(const WorkSession& session, std::span<index_t> offsets) {
  index_t total = 0;
  offsets[0] = 0;
  // Source-order prefix planning admits checked row extents before any fill.
  for (std::size_t i = 1; i < offsets.size(); ++i) {
    if (i % session.work.cancellation_block_scalars == 0)
      session.allocation.Checkpoint();
    total = CheckedSum(total, offsets[i]);
    offsets[i] = total;
  }
}
std::size_t SelectedScalarCount(const WorkSession& session,
                                const AttributeReadView& attribute,
                                std::span<const index_t> rows) {
  if (!attribute.offsets)
    return CheckedProduct(rows.size(), attribute.components);
  std::size_t total = 0;
  const auto offsets = *attribute.offsets;
  for (std::size_t i = 0; i < rows.size(); ++i) {
    if (i % session.work.cancellation_block_scalars == 0)
      session.allocation.Checkpoint();
    total = CheckedSum(total, offsets[rows[i] + 1] - offsets[rows[i]]);
  }
  return total;
}
std::shared_ptr<const AttributeState> SelectAttribute(
    const WorkSession& session, const AttributeReadView& input,
    std::span<const index_t> rows) {
  const auto& allocation = session.allocation;
  auto result = std::allocate_shared<AttributeState>(
      Allocator<AttributeState>(allocation), allocation);
  result->domain = input.domain;
  result->components = input.components;
  result->set_index = input.set_index;
  result->has_offsets = input.offsets.has_value();
  if (input.offsets) {
    result->offsets =
        FlatArray<index_t>(allocation, CheckedSum(rows.size(), 1));
    struct Job {
      const WorkSession& session;
      std::span<const index_t> source_offsets;
      std::span<const index_t> rows;
      std::span<index_t> destination;
    } job{session, *input.offsets, rows, result->offsets.Values()};
    session.Run(
        rows.size(), sizeof(index_t),
        [](void* argument, std::size_t begin, std::size_t end) {
          const auto& job = *static_cast<Job*>(argument);
          for (auto i = begin; i < end; ++i) {
            if ((i - begin) % job.session.work.cancellation_block_scalars == 0)
              job.session.allocation.Checkpoint();
            job.destination[i + 1] = job.source_offsets[job.rows[i] + 1] -
                                     job.source_offsets[job.rows[i]];
          }
        },
        &job);
    PrefixOffsets(session, result->offsets.Values());
  }
  const auto scalar_count = SelectedScalarCount(session, input, rows);
  std::visit(
      [&](const auto& source) {
        using T = typename std::decay_t<decltype(source)>::value_type;
        const auto plan = numeric_detail::Access::ScalarPlan<T>(scalar_count);
        CheckPlan(plan);
        result->numerical_lease = allocation.ReserveOriginalRequest(
            CheckedSum(plan.implementation_bytes, plan.numerical_bytes));
        auto values = numeric_detail::Access::AllocateScalars<T>(scalar_count);
        struct Job {
          const WorkSession& session;
          std::span<const T> source;
          std::span<T> destination;
          std::span<const index_t> rows;
          std::optional<std::span<const index_t>> source_offsets;
          std::span<const index_t> destination_offsets;
          std::size_t components;
        } job{session,         source,
              values.Values(), rows,
              input.offsets,   result->offsets.Values(),
              input.components};
        session.Run(
            scalar_count, sizeof(T),
            [](void* argument, std::size_t begin, std::size_t end) {
              const auto& job = *static_cast<Job*>(argument);
              for (auto i = begin; i < end; ++i) {
                if ((i - begin) % job.session.work.cancellation_block_scalars ==
                    0)
                  job.session.allocation.Checkpoint();
                std::size_t row;
                std::size_t source_scalar;
                if (job.source_offsets) {
                  row = static_cast<std::size_t>(
                      std::upper_bound(job.destination_offsets.begin(),
                                       job.destination_offsets.end(), i) -
                      job.destination_offsets.begin() - 1);
                  source_scalar = (*job.source_offsets)[job.rows[row]] + i -
                                  job.destination_offsets[row];
                } else {
                  row = i / job.components;
                  source_scalar =
                      job.rows[row] * job.components + i % job.components;
                }
                std::memcpy(&job.destination[i], &job.source[source_scalar],
                            sizeof(T));
              }
            },
            &job);
        result->values = std::move(values);
      },
      input.values);
  result->has_present = input.present.has_value();
  if (input.present) {
    result->present = FlatArray<std::uint8_t>(allocation, rows.size());
    struct Job {
      const WorkSession& session;
      std::span<const std::uint8_t> source;
      std::span<std::uint8_t> destination;
      std::span<const index_t> rows;
    } job{session, *input.present, result->present.Values(), rows};
    session.Run(
        rows.size(), sizeof(std::uint8_t),
        [](void* argument, std::size_t begin, std::size_t end) {
          const auto& job = *static_cast<Job*>(argument);
          for (auto i = begin; i < end; ++i) {
            if ((i - begin) % job.session.work.cancellation_block_scalars == 0)
              job.session.allocation.Checkpoint();
            job.destination[i] = job.source[job.rows[i]];
          }
        },
        &job);
  }
  AttributeMaterializationInput descriptor;
  descriptor.name = input.name;
  descriptor.semantic = input.semantic;
  descriptor.metadata = input.metadata;
  CaptureDescriptor(session, descriptor, *result);
  return result;
}
void ValidateCandidateCounts(const WorkSession& session, MeshReadView source,
                             const MeshMaterializationLimits& limits,
                             std::span<const index_t> faces,
                             std::size_t corner_count) {
  std::size_t scalars = 0, offsets = 0, presence = 0, metadata = 0, text = 0;
  for (std::size_t ordinal = 0; ordinal < source.attributes.size(); ++ordinal) {
    session.allocation.Checkpoint();
    const auto& attribute = source.attributes[ordinal];
    const bool shared = attribute.domain == AttributeDomain::vertex;
    const auto row_count =
        attribute.domain == AttributeDomain::face ? faces.size() : corner_count;
    std::size_t scalar_count = 0;
    if (shared) {
      scalar_count = std::visit(
          [](const auto& values) { return values.size(); }, attribute.values);
    } else if (!attribute.offsets) {
      scalar_count = CheckedProduct(row_count, attribute.components);
    } else if (attribute.domain == AttributeDomain::face) {
      scalar_count = SelectedScalarCount(session, attribute, faces);
    } else {
      const auto scalar_offsets = *attribute.offsets;
      for (std::size_t i = 0; i < faces.size(); ++i) {
        if (i % session.work.cancellation_block_scalars == 0)
          session.allocation.Checkpoint();
        scalar_count = CheckedSum(
            scalar_count, scalar_offsets[source.face_offsets[faces[i] + 1]] -
                              scalar_offsets[source.face_offsets[faces[i]]]);
      }
    }
    scalars = CheckedSum(scalars, scalar_count);
    if (attribute.offsets)
      offsets = CheckedSum(offsets, shared ? attribute.offsets->size()
                                           : CheckedSum(row_count, 1));
    if (attribute.present)
      presence =
          CheckedSum(presence, shared ? attribute.present->size() : row_count);
    metadata = CheckedSum(metadata, attribute.metadata.size());
    text = CheckedSum(text, attribute.name.size());
    text = CheckedSum(text, attribute.semantic.size());
    for (const auto& entry : attribute.metadata) {
      session.allocation.Checkpoint();
      text = CheckedSum(text, entry.key.size());
      text = CheckedSum(text, entry.value.size());
    }
    CountLimit(scalars, limits.max_attribute_scalars, ordinal);
    CountLimit(offsets, limits.max_attribute_offsets, ordinal);
    CountLimit(presence, limits.max_presence_bytes, ordinal);
    CountLimit(metadata, limits.max_metadata_entries, ordinal);
    CountLimit(text, limits.max_descriptor_bytes, ordinal);
  }
}
void AdmissionDiagnostic(MeshMaterializationResult& result,
                         const AdmissionFailure& failure) {
  switch (failure.Code()) {
    case AdmissionCode::kSizeLimit:
      result.status = MaterializationStatus::kLimitExceeded;
      result.diagnostic.code = MaterializationDiagnosticCode::kStorageExtent;
      break;
    case AdmissionCode::kBudgetExceeded:
      result.status = MaterializationStatus::kBudgetExceeded;
      result.diagnostic.code = MaterializationDiagnosticCode::kRequestBudget;
      break;
    case AdmissionCode::kCanceled:
      result.status = MaterializationStatus::kCanceled;
      result.diagnostic.code = MaterializationDiagnosticCode::kStopRequested;
      break;
    case AdmissionCode::kInvalidObject:
      throw;
  }
}
}  // namespace
MeshMaterializationResult TestMaterializeMesh(
    const MeshMaterializationInput& input,
    const MeshMaterializationLimits& limits,
    const MeshMaterializationWorkPolicy& work, const ExecutionContext& context,
    std::stop_token stop, const TestHooks& hooks) {
  MeshMaterializationResult result;
  if (hooks.execution_observer) *hooks.execution_observer = {};
  try {
    ValidateLimits(limits);
    ValidateWork(work);
    const auto counts = ValidateInput(input, limits, work, stop);
    auto allocation = AllocationContext(context, stop, hooks.allocation)
                          .WithRequestLimit(limits.max_new_requested_bytes);
    WorkSession session{context, allocation,       work,
                        stop,    result.execution, hooks};
    auto state = std::allocate_shared<MeshState>(
        Allocator<MeshState>(allocation), allocation, context);
    auto vertices = std::allocate_shared<VertexState>(
        Allocator<VertexState>(allocation), allocation);
    const auto plan = numeric_detail::Access::PositionPlan(counts.vertices);
    vertices->numerical_lease = allocation.ReserveOriginalRequest(
        CheckedSum(plan.implementation_bytes, plan.numerical_bytes));
    vertices->positions =
        numeric_detail::Access::AllocatePositions(counts.vertices);
    std::visit(
        [&](const auto& values) {
          using Span = std::decay_t<decltype(values)>;
          if constexpr (!std::is_same_v<Span, std::monostate>) {
            auto destination =
                numeric_detail::Access::PositionScalars(vertices->positions);
            if constexpr (std::is_same_v<typename Span::value_type, double>)
              CopyScalars(session, values, destination);
            else
              ConvertScalars(session, values, destination, WidenPosition);
          }
        },
        input.positions);
    state->vertices = vertices;
    state->face_offsets = FlatArray<index_t>(allocation, counts.face_offsets);
    state->corner_vertices = FlatArray<index_t>(allocation, counts.corners);
    std::visit(
        [&](const auto& topology) {
          using Topology = std::decay_t<decltype(topology)>;
          if constexpr (std::is_same_v<Topology, PolygonTopologyInput>) {
            CopyScalars(session, topology.face_offsets,
                        state->face_offsets.Values());
            CopyScalars(session, topology.corner_vertices,
                        state->corner_vertices.Values());
          } else if constexpr (std::is_same_v<Topology,
                                              TriangleTopologyInput>) {
            ConvertScalars(session, topology.corner_vertices,
                           state->corner_vertices.Values(),
                           [](const std::uint32_t& source,
                              index_t& destination) { destination = source; });
            struct OffsetJob {
              WorkSession& session;
              std::span<index_t> offsets;
            } job{session, state->face_offsets.Values()};
            session.Run(
                counts.face_offsets, sizeof(index_t),
                [](void* argument, std::size_t begin, std::size_t end) {
                  auto& job = *static_cast<OffsetJob*>(argument);
                  for (auto i = begin; i < end; ++i) {
                    if ((i - begin) %
                            job.session.work.cancellation_block_scalars ==
                        0)
                      job.session.allocation.Checkpoint();
                    job.offsets[i] = i * kCornersPerTriangle;
                  }
                },
                &job);
          }
        },
        input.topology);
    state->attributes.reserve(input.attributes.size());
    state->views.reserve(input.attributes.size());
    for (const auto& attribute : input.attributes) {
      allocation.Checkpoint();
      auto captured = CaptureAttribute(session, attribute);
      state->views.push_back(captured->View());
      state->attributes.push_back(std::move(captured));
    }
    allocation.Checkpoint();
    result.mesh = Access::MakeMesh(std::move(state));
    result.status = MaterializationStatus::kSuccess;
  } catch (const ExpectedFailure& failure) {
    result.status = failure.status;
    result.diagnostic = failure.diagnostic;
  } catch (const AdmissionFailure& failure) {
    AdmissionDiagnostic(result, failure);
  }
  return result;
}
MeshInspectionResult TestInspectMaterializedMesh(
    const MaterializedMesh& source, const MeshInspectionLimits& limits,
    const MeshMaterializationWorkPolicy& work, std::stop_token stop,
    const TestHooks& hooks) {
  MeshInspectionResult result;
  if (hooks.execution_observer) *hooks.execution_observer = {};
  try {
    RequiredConfiguration(limits.max_issues, limits.max_new_requested_bytes);
    ValidateWork(work);
    const auto& captured = Access::State(source);
    auto allocation =
        AllocationContext(captured.context, stop, hooks.allocation)
            .WithRequestLimit(limits.max_new_requested_bytes);
    WorkSession session{captured.context, allocation, work, stop,
                        result.execution, hooks};
    auto state = std::allocate_shared<InspectionState>(
        Allocator<InspectionState>(allocation), allocation, source);
    auto inspected =
        InspectStorage(session, source.View(), limits.max_issues, false);
    state->issues = std::move(inspected.issues);
    allocation.Checkpoint();
    result.inspection = Access::MakeInspection(std::move(state));
    result.status = MaterializationStatus::kSuccess;
  } catch (const ExpectedFailure& failure) {
    result.status = failure.status;
    result.diagnostic = failure.diagnostic;
  } catch (const AdmissionFailure& failure) {
    MeshMaterializationResult diagnostic;
    AdmissionDiagnostic(diagnostic, failure);
    result.status = diagnostic.status;
    result.diagnostic = diagnostic.diagnostic;
  }
  return result;
}
MeshFaceSelectionResult TestSelectMeshFaces(
    const MaterializedMesh& source, std::span<const index_t> kept_faces,
    const MeshMaterializationLimits& limits,
    const MeshMaterializationWorkPolicy& work, std::stop_token stop,
    const TestHooks& hooks) {
  MeshFaceSelectionResult result;
  if (hooks.execution_observer) *hooks.execution_observer = {};
  try {
    ValidateLimits(limits);
    ValidateWork(work);
    const auto& captured = Access::State(source);
    const auto view = source.View();
    const auto vertex_count = view.positions.size() / kVector3Components;
    CountLimit(vertex_count, limits.max_vertices);
    CountLimit(kept_faces.size(), limits.max_faces);
    CountLimit(CheckedSum(kept_faces.size(), 1), limits.max_face_offsets);
    CountLimit(view.attributes.size(), limits.max_attributes);
    auto allocation =
        AllocationContext(captured.context, stop, hooks.allocation)
            .WithRequestLimit(limits.max_new_requested_bytes);
    WorkSession session{captured.context, allocation, work, stop,
                        result.execution, hooks};
    const auto inspected = InspectStorage(
        session, view, std::numeric_limits<std::size_t>::max(), true);
    if (inspected.first)
      throw ExpectedFailure{MaterializationStatus::kInvalidSourceStorage,
                            {MaterializationDiagnosticCode::kSourceStorage,
                             inspected.first->attribute,
                             inspected.first->element, inspected.first}};
    const auto face_count = view.face_offsets.size() - 1;
    std::size_t corner_count = 0;
    for (std::size_t i = 0; i < kept_faces.size(); ++i) {
      if (i % work.cancellation_block_scalars == 0) allocation.Checkpoint();
      const auto face = kept_faces[i];
      if (i != 0 && face <= kept_faces[i - 1])
        Fail(MaterializationStatus::kInvalidSelection,
             MaterializationDiagnosticCode::kSelectionOrder, {}, i);
      if (face >= face_count)
        Fail(MaterializationStatus::kInvalidSelection,
             MaterializationDiagnosticCode::kSelectionRange, {}, i);
      corner_count = CheckedSum(
          corner_count, view.face_offsets[face + 1] - view.face_offsets[face]);
    }
    CountLimit(corner_count, limits.max_corners);
    ValidateCandidateCounts(session, view, limits, kept_faces, corner_count);
    auto candidate = std::allocate_shared<MeshState>(
        Allocator<MeshState>(allocation), allocation, captured.context);
    candidate->vertices = captured.vertices;
    candidate->face_offsets =
        FlatArray<index_t>(allocation, CheckedSum(kept_faces.size(), 1));
    candidate->corner_vertices = FlatArray<index_t>(allocation, corner_count);
    auto selection = std::allocate_shared<SelectionState>(
        Allocator<SelectionState>(allocation), allocation,
        Access::MakeMesh(candidate));
    selection->source_faces = FlatArray<index_t>(allocation, kept_faces.size());
    selection->source_corners = FlatArray<index_t>(allocation, corner_count);
    selection->source_vertex_count = vertex_count;
    selection->source_face_count = face_count;
    selection->source_corner_count = view.corner_vertices.size();
    CopyScalars(session, kept_faces, selection->source_faces.Values());
    struct OffsetJob {
      const WorkSession& session;
      std::span<const index_t> source_offsets;
      std::span<const index_t> faces;
      std::span<index_t> destination;
    } offset_job{session, view.face_offsets, kept_faces,
                 candidate->face_offsets.Values()};
    session.Run(
        kept_faces.size(), sizeof(index_t),
        [](void* argument, std::size_t begin, std::size_t end) {
          const auto& job = *static_cast<OffsetJob*>(argument);
          for (auto i = begin; i < end; ++i) {
            if ((i - begin) % job.session.work.cancellation_block_scalars == 0)
              job.session.allocation.Checkpoint();
            job.destination[i + 1] = job.source_offsets[job.faces[i] + 1] -
                                     job.source_offsets[job.faces[i]];
          }
        },
        &offset_job);
    PrefixOffsets(session, candidate->face_offsets.Values());
    struct CornerJob {
      const WorkSession& session;
      std::span<const index_t> source_offsets;
      std::span<const index_t> source_corners;
      std::span<const index_t> faces;
      std::span<const index_t> destination_offsets;
      std::span<index_t> destination_corners;
      std::span<index_t> map;
    } corner_job{session,
                 view.face_offsets,
                 view.corner_vertices,
                 kept_faces,
                 candidate->face_offsets.Values(),
                 candidate->corner_vertices.Values(),
                 selection->source_corners.Values()};
    session.Run(
        corner_count, sizeof(index_t) + sizeof(index_t),
        [](void* argument, std::size_t begin, std::size_t end) {
          const auto& job = *static_cast<CornerJob*>(argument);
          for (auto i = begin; i < end; ++i) {
            if ((i - begin) % job.session.work.cancellation_block_scalars == 0)
              job.session.allocation.Checkpoint();
            const auto face = static_cast<std::size_t>(
                std::upper_bound(job.destination_offsets.begin(),
                                 job.destination_offsets.end(), i) -
                job.destination_offsets.begin() - 1);
            const auto source_corner = job.source_offsets[job.faces[face]] + i -
                                       job.destination_offsets[face];
            job.map[i] = source_corner;
            job.destination_corners[i] = job.source_corners[source_corner];
          }
        },
        &corner_job);
    candidate->attributes.reserve(view.attributes.size());
    candidate->views.reserve(view.attributes.size());
    for (std::size_t ordinal = 0; ordinal < view.attributes.size(); ++ordinal) {
      allocation.Checkpoint();
      const auto& attribute = view.attributes[ordinal];
      auto selected =
          attribute.domain == AttributeDomain::vertex
              ? captured.attributes[ordinal]
              : SelectAttribute(session, attribute,
                                attribute.domain == AttributeDomain::face
                                    ? selection->source_faces.Values()
                                    : selection->source_corners.Values());
      candidate->views.push_back(selected->View());
      candidate->attributes.push_back(std::move(selected));
    }
    allocation.Checkpoint();
    result.selection = Access::MakeSelection(std::move(selection));
    result.status = MaterializationStatus::kSuccess;
  } catch (const ExpectedFailure& failure) {
    result.status = failure.status;
    result.diagnostic = failure.diagnostic;
  } catch (const AdmissionFailure& failure) {
    MeshMaterializationResult diagnostic;
    AdmissionDiagnostic(diagnostic, failure);
    result.status = diagnostic.status;
    result.diagnostic = diagnostic.diagnostic;
  }
  return result;
}
}  // namespace meshvale::geometry::materialization_detail

namespace meshvale::geometry {
MaterializedMesh::MaterializedMesh(
    std::shared_ptr<const materialization_detail::MeshState> state) noexcept
    : state_(std::move(state)) {}
MaterializedMesh::MaterializedMesh(const MaterializedMesh&) noexcept = default;
MaterializedMesh& MaterializedMesh::operator=(
    const MaterializedMesh&) noexcept = default;
MaterializedMesh::~MaterializedMesh() noexcept = default;
MeshReadView MaterializedMesh::View() const noexcept {
  return {numeric_detail::Access::PositionScalars(state_->vertices->positions),
          state_->face_offsets.Values(), state_->corner_vertices.Values(),
          state_->views};
}
std::uint64_t MaterializedMesh::NewRequestedBytes() const noexcept {
  return state_->allocation.OperationActiveBytes();
}
MeshInspection::MeshInspection(
    std::shared_ptr<const materialization_detail::InspectionState>
        state) noexcept
    : state_(std::move(state)) {}
MeshInspection::MeshInspection(const MeshInspection&) noexcept = default;
MeshInspection& MeshInspection::operator=(const MeshInspection&) noexcept =
    default;
MeshInspection::~MeshInspection() noexcept = default;
const MaterializedMesh& MeshInspection::Source() const noexcept {
  return state_->source;
}
std::span<const StorageIssue> MeshInspection::Issues() const noexcept {
  return state_->issues;
}
bool MeshInspection::IsStorageValid() const noexcept {
  return state_->issues.empty();
}
std::uint64_t MeshInspection::NewRequestedBytes() const noexcept {
  return state_->allocation.OperationActiveBytes();
}
MaterializedFaceSelection::MaterializedFaceSelection(
    std::shared_ptr<const materialization_detail::SelectionState>
        state) noexcept
    : state_(std::move(state)) {}
MaterializedFaceSelection::MaterializedFaceSelection(
    const MaterializedFaceSelection&) noexcept = default;
MaterializedFaceSelection& MaterializedFaceSelection::operator=(
    const MaterializedFaceSelection&) noexcept = default;
MaterializedFaceSelection::~MaterializedFaceSelection() noexcept = default;
const MaterializedMesh& MaterializedFaceSelection::Candidate() const noexcept {
  return state_->candidate;
}
std::span<const index_t> MaterializedFaceSelection::SourceFaces()
    const noexcept {
  return state_->source_faces.Values();
}
std::span<const index_t> MaterializedFaceSelection::SourceCorners()
    const noexcept {
  return state_->source_corners.Values();
}
index_t MaterializedFaceSelection::SourceVertexCount() const noexcept {
  return state_->source_vertex_count;
}
index_t MaterializedFaceSelection::SourceFaceCount() const noexcept {
  return state_->source_face_count;
}
index_t MaterializedFaceSelection::SourceCornerCount() const noexcept {
  return state_->source_corner_count;
}
std::uint64_t MaterializedFaceSelection::NewRequestedBytes() const noexcept {
  return state_->allocation.OperationActiveBytes();
}
MeshMaterializationResult MaterializeMesh(
    const MeshMaterializationInput& input,
    const MeshMaterializationLimits& limits,
    const MeshMaterializationWorkPolicy& work, const ExecutionContext& context,
    std::stop_token stop) {
  return materialization_detail::TestMaterializeMesh(input, limits, work,
                                                     context, stop, {});
}
MeshInspectionResult InspectMaterializedMesh(
    const MaterializedMesh& source, const MeshInspectionLimits& limits,
    const MeshMaterializationWorkPolicy& work, std::stop_token stop) {
  return materialization_detail::TestInspectMaterializedMesh(source, limits,
                                                             work, stop, {});
}
MeshFaceSelectionResult SelectMeshFaces(
    const MaterializedMesh& source, std::span<const index_t> kept_faces,
    const MeshMaterializationLimits& limits,
    const MeshMaterializationWorkPolicy& work, std::stop_token stop) {
  return materialization_detail::TestSelectMeshFaces(source, kept_faces, limits,
                                                     work, stop, {});
}
}  // namespace meshvale::geometry
