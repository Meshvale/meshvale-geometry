// SPDX-License-Identifier: Apache-2.0
#include <meshvale/geometry/attributes.h>
#include <meshvale/geometry/charged_triangulation.h>
#include <meshvale/geometry/editable_mesh.h>
#include <meshvale/geometry/execution.h>
#include <meshvale/geometry/materialization.h>
#include <meshvale/geometry/mesh.h>
#include <meshvale/geometry/position_buffer.h>
#include <meshvale/geometry/scalar_buffer.h>
#include <meshvale/geometry/triangulation.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <exception>
#include <initializer_list>
#include <limits>
#include <memory>
#include <new>
#include <optional>
#include <span>
#include <stop_token>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>

#include "charged_triangulation_internal.h"
#include "materialization_allocation.h"
#include "materialization_storage.h"
#include "materialization_workers.h"
#include "triangulation_internal.h"
#include "triangulation_numeric.h"

namespace meshvale::geometry {
namespace charged_triangulation_detail {
namespace md = materialization_detail;
namespace td = triangulation_detail;
using Status = ChargedTriangulationStatus;
using Stage = ChargedTriangulationStage;
using Code = ChargedTriangulationCode;
using Serial = ChargedTriangulationSerialReason;
using md::AdmissionCode;
using md::AdmissionFailure;
using md::AllocationContext;
using md::Allocator;
using md::ChargedVector;
using md::FlatArray;
constexpr std::size_t kPositionComponents = td::kPositionComponents;
constexpr std::size_t kTriangleCorners = td::kTriangleCorners;
constexpr std::size_t kCompletedScalarCount = 1;

struct State {
  State(const AllocationContext& owner_allocation,
        const MaterializedMesh& output, const MeshInspection& checked,
        ChargedTriangulationCounts shape, FlatArray<index_t> faces,
        FlatArray<index_t> corners, FlatArray<index_t> ranges)
      : allocation(owner_allocation),
        mesh(output),
        inspection(checked),
        counts(shape),
        face_sources(std::move(faces)),
        corner_sources(std::move(corners)),
        face_output_offsets(std::move(ranges)) {}
  AllocationContext allocation;
  MaterializedMesh mesh;
  MeshInspection inspection;
  ChargedTriangulationCounts counts;
  FlatArray<index_t> face_sources;
  FlatArray<index_t> corner_sources;
  FlatArray<index_t> face_output_offsets;
};
namespace {
struct Failure {
  Status status;
  Code code;
};
[[noreturn]] void Fail(Status status, Code code) {
  throw Failure{status, code};
}
std::size_t Add(std::size_t a, std::size_t b) {
  if (b > std::numeric_limits<std::size_t>::max() - a)
    Fail(Status::kLimitExceeded, Code::kSizeOverflow);
  return a + b;
}
std::size_t Product(std::size_t a, std::size_t b) {
  if (a && b > std::numeric_limits<std::size_t>::max() / a)
    Fail(Status::kLimitExceeded, Code::kSizeOverflow);
  return a * b;
}
void Stop(std::stop_token stop) {
  if (stop.stop_requested()) Fail(Status::kCanceled, Code::kStopRequested);
}
void WorkerStop(std::stop_token stop) {
  if (stop.stop_requested()) throw AdmissionFailure(AdmissionCode::kCanceled);
}
void Limit(std::size_t actual, std::uint64_t maximum, Code code) {
  if (actual > maximum) Fail(Status::kLimitExceeded, code);
}
void Required(std::uint64_t value) {
  if (!value) Fail(Status::kInvalidConfiguration, Code::kRequiredFieldZero);
  if (value > std::numeric_limits<std::size_t>::max())
    Fail(Status::kInvalidConfiguration, Code::kConfigurationExtent);
}
void Validate(const MeshMaterializationLimits& v) {
  for (const auto value :
       {v.max_vertices, v.max_faces, v.max_corners, v.max_face_offsets,
        v.max_attributes, v.max_attribute_scalars, v.max_attribute_offsets,
        v.max_presence_bytes, v.max_metadata_entries, v.max_descriptor_bytes,
        v.max_new_requested_bytes})
    Required(value);
}
void Validate(const ChargedTriangulationLimits& limits,
              const TriangulationOptions& options,
              const MeshMaterializationWorkPolicy& work) {
  Validate(limits.source);
  Validate(limits.output);
  for (const auto value :
       {limits.source_inspection.max_issues,
        limits.source_inspection.max_new_requested_bytes,
        limits.output_inspection.max_issues,
        limits.output_inspection.max_new_requested_bytes,
        limits.max_map_entries, limits.max_core_new_requested_bytes,
        limits.max_bridge_new_requested_bytes, work.parallel_min_bytes,
        work.partition_bytes, work.cancellation_block_scalars, work.max_jobs})
    Required(value);
  if (options.max_corners_per_face < td::kMinimumFaceCorners ||
      options.max_corners_per_face > td::kMaximumFaceCorners)
    Fail(Status::kInvalidConfiguration, Code::kUnsupportedCornerLimit);
}
Status DelegateStatus(MaterializationStatus status) {
  switch (status) {
    case MaterializationStatus::kSuccess:
      return Status::kAccepted;
    case MaterializationStatus::kInvalidConfiguration:
      return Status::kInvalidConfiguration;
    case MaterializationStatus::kLimitExceeded:
      return Status::kLimitExceeded;
    case MaterializationStatus::kUnsupportedAllocationProfile:
      return Status::kUnsupportedAllocationProfile;
    case MaterializationStatus::kBudgetExceeded:
      return Status::kBudgetExceeded;
    case MaterializationStatus::kCanceled:
      return Status::kCanceled;
    case MaterializationStatus::kInvalidSourceStorage:
      return Status::kBlocked;
    case MaterializationStatus::kInvalidTransport:
    case MaterializationStatus::kInvalidSelection:
      return Status::kDelegateFailure;
  }
  std::terminate();
}
ChargedTriangulationMaterializationFacts Facts(
    const MeshMaterializationResult& r) {
  return {r.status, r.diagnostic, r.execution,
          r.mesh ? r.mesh->NewRequestedBytes() : 0};
}
ChargedTriangulationInspectionFacts Facts(const MeshInspectionResult& r) {
  return {r.status, r.diagnostic, r.execution,
          r.inspection ? std::optional<bool>(r.inspection->IsStorageValid())
                       : std::nullopt,
          r.inspection ? r.inspection->NewRequestedBytes() : 0};
}
void Delegate(MaterializationStatus status, Code code) {
  if (status == MaterializationStatus::kSuccess) return;
  if (status == MaterializationStatus::kUnsupportedAllocationProfile)
    code = Code::kAllocationProfile;
  else if (status == MaterializationStatus::kCanceled)
    code = Code::kStopRequested;
  else if (status == MaterializationStatus::kBudgetExceeded)
    code = Code::kRequestBudget;
  Fail(DelegateStatus(status), code);
}

// Measures real requests reaching this backend, not refused reservations.
// Its stable frame outlives every core allocation and accounting seed.
struct RequestObserver {
  explicit RequestObserver(md::AllocationBackend underlying)
      : target(underlying) {}
  RequestObserver(const RequestObserver&) = delete;
  RequestObserver& operator=(const RequestObserver&) = delete;
  std::atomic<std::size_t> active{0};
  std::atomic<std::size_t> peak{0};
  md::AllocationBackend target;
  static void* Allocate(std::size_t bytes, std::size_t alignment, void* state) {
    auto& self = *static_cast<RequestObserver*>(state);
    const auto previous =
        self.active.fetch_add(bytes, std::memory_order_relaxed);
    if (bytes > std::numeric_limits<std::size_t>::max() - previous)
      std::terminate();
    const auto total = previous + bytes;
    auto old_peak = self.peak.load(std::memory_order_relaxed);
    while (old_peak < total &&
           !self.peak.compare_exchange_weak(old_peak, total,
                                            std::memory_order_relaxed)) {
    }
    try {
      auto* block = self.target.allocate(bytes, alignment, self.target.state);
      if (!block) self.active.fetch_sub(bytes, std::memory_order_relaxed);
      return block;
    } catch (...) {
      self.active.fetch_sub(bytes, std::memory_order_relaxed);
      throw;
    }
  }
  static void Deallocate(void* block, std::size_t bytes, std::size_t alignment,
                         void* state) noexcept {
    auto& self = *static_cast<RequestObserver*>(state);
    self.target.deallocate(block, bytes, alignment, self.target.state);
    self.active.fetch_sub(bytes, std::memory_order_relaxed);
  }
  md::AllocationBackend Backend() noexcept {
    return {Allocate, Deallocate, this};
  }
};
using LimbAllocator = Allocator<std::uint32_t>;
using Triangles = ChargedVector<td::Triangle>;
struct FaceOutcome {
  explicit FaceOutcome(const AllocationContext& allocation)
      : triangles(Allocator<td::Triangle>(allocation)) {}
  Triangles triangles;
  const char* failure = nullptr;
  std::optional<AdmissionCode> admission;
  std::exception_ptr exception;
};
struct CoreFrame {
  CoreFrame(const ExecutionContext& execution, std::stop_token stop,
            md::AllocationBackend backend)
      : observer(backend), base(execution, stop, observer.Backend()) {}
  CoreFrame(const CoreFrame&) = delete;
  CoreFrame& operator=(const CoreFrame&) = delete;
  CoreFrame(CoreFrame&&) = delete;
  CoreFrame& operator=(CoreFrame&&) = delete;
  void Begin(std::size_t limit) {
    allocation.emplace(base.WithRequestLimit(limit));
    faces.emplace(Allocator<FaceOutcome>(*allocation));
  }
  RequestObserver observer;
  AllocationContext base;
  std::optional<AllocationContext> allocation;
  std::optional<ChargedVector<FaceOutcome>> faces;
};
struct CoreTeam {
  const MeshReadView& source;
  CoreFrame& frame;
  std::stop_token stop;
  std::size_t workers;
};
void Compute(CoreTeam& team, std::size_t begin, std::size_t end) noexcept {
  td::Context context{team.stop};
  const td::MeshView view{{team.source.positions}, team.source.corner_vertices};
  for (auto face = begin; face < end; ++face) {
    if (team.stop.stop_requested()) break;
    auto& outcome = (*team.frame.faces)[face];
    try {
      outcome.triangles =
          td::TriangulateFace(view, team.source.face_offsets[face],
                              team.source.face_offsets[face + 1], context,
                              LimbAllocator(*team.frame.allocation));
    } catch (const td::ProductError& error) {
      outcome.failure = error.code;
    } catch (const AdmissionFailure& error) {
      if (error.Code() == AdmissionCode::kInvalidObject)
        outcome.exception = std::current_exception();
      else
        outcome.admission = error.Code();
    } catch (...) {
      outcome.exception = std::current_exception();
    }
  }
}
void Worker(md::NativeWorkerRecord& record) noexcept {
  auto& team = *static_cast<CoreTeam*>(record.state);
  const auto count = team.frame.faces->size();
  const auto first = count / team.workers * record.ordinal +
                     std::min(record.ordinal, count % team.workers);
  const auto last = first + count / team.workers +
                    (record.ordinal < count % team.workers ? 1 : 0);
  Compute(team, first, last);
}
Code GeometryCode(std::string_view code) {
  constexpr std::array kGeometryCodes{
      std::pair{td::kConversionFaceCornerLimit, Code::kFaceCornerLimit},
      std::pair{td::kConversionRepeatedPosition, Code::kRepeatedPosition},
      std::pair{td::kConversionDegenerateFace, Code::kDegenerateFace},
      std::pair{td::kConversionNonplanarFace, Code::kNonplanarFace},
      std::pair{td::kConversionBacktrackingBoundary,
                Code::kBacktrackingBoundary},
      std::pair{td::kConversionSelfIntersection, Code::kSelfIntersection},
      std::pair{td::kConversionNoGeometricEar, Code::kNoGeometricEar},
      std::pair{td::kConversionDegenerateResult, Code::kDegenerateResult},
      std::pair{td::kConversionCanceled, Code::kStopRequested}};
  for (const auto& item : kGeometryCodes)
    if (code == item.first) return item.second;
  std::terminate();
}
void Admission(const AdmissionFailure& error, Code budget) {
  switch (error.Code()) {
    case AdmissionCode::kSizeLimit:
      Fail(Status::kLimitExceeded, Code::kSizeOverflow);
    case AdmissionCode::kBudgetExceeded:
      Fail(Status::kBudgetExceeded, budget);
    case AdmissionCode::kCanceled:
      Fail(Status::kCanceled, Code::kStopRequested);
    case AdmissionCode::kInvalidObject:
      throw error;
  }
  std::terminate();
}
void RunCore(CoreFrame& frame, const MeshReadView& source,
             const TriangulationOptions& options,
             const ExecutionContext& execution, std::stop_token stop,
             const md::NativeWorkerBackend& backend,
             ChargedTriangulationResult& result) {
  auto& facts = result.core.emplace();
  try {
    const auto count = source.face_offsets.size() - 1;
    auto& faces = *frame.faces;
    faces.reserve(count);
    for (std::size_t face = 0; face < count; ++face) {
      Stop(stop);
      faces.emplace_back(*frame.allocation);
    }
    const auto desired = count >= options.minimum_parallel_faces
                             ? std::min(count, execution.WorkerBudget())
                             : 0;
    auto workers = TryReserveWorkers(execution, desired);
    CoreTeam team{source, frame, stop, workers.Count()};
    if (!workers.Count()) {
      facts.serial_reason =
          count == 0                      ? Serial::kEmptyMesh
          : execution.WorkerBudget() <= 1 ? Serial::kWorkerBudgetOne
          : count < options.minimum_parallel_faces
              ? Serial::kBelowFaceThreshold
              : Serial::kSharedWorkersUnavailableOrFewerThanTwoFaces;
      Compute(team, 0, count);
    } else {
      ChargedVector<md::NativeWorkerRecord> records{
          Allocator<md::NativeWorkerRecord>(*frame.allocation)};
      ChargedVector<std::uintptr_t> handles{
          Allocator<std::uintptr_t>(*frame.allocation)};
      records.resize(workers.Count());
      handles.reserve(workers.Count());
      try {
        for (std::size_t i = 0; i < workers.Count(); ++i) {
          records[i] = {Worker, &team, i};
          handles.push_back(backend.launch(&records[i], backend.state));
        }
      } catch (...) {
        for (const auto handle : handles) backend.join(handle, backend.state);
        throw;
      }
      for (const auto handle : handles) backend.join(handle, backend.state);
      facts.workers_used = handles.size();
    }
    for (const auto& face : faces)
      if (face.exception) std::rethrow_exception(face.exception);
    Stop(stop);
    for (std::size_t face = 0; face < count; ++face) {
      if (faces[face].admission) {
        result.diagnostic.face = face;
        Admission(AdmissionFailure(*faces[face].admission),
                  Code::kCoreRequestBudget);
      }
      if (faces[face].failure) {
        result.diagnostic.face = face;
        Fail(Status::kBlocked, GeometryCode(faces[face].failure));
      }
    }
    facts.status = TriangulationStatus::kAccepted;
    facts.new_requested_peak_bytes =
        frame.observer.peak.load(std::memory_order_relaxed);
  } catch (const Failure& error) {
    facts.status = error.status == Status::kCanceled
                       ? TriangulationStatus::kCanceled
                       : TriangulationStatus::kBlocked;
    facts.new_requested_peak_bytes =
        frame.observer.peak.load(std::memory_order_relaxed);
    throw;
  } catch (...) {
    facts.new_requested_peak_bytes =
        frame.observer.peak.load(std::memory_order_relaxed);
    throw;
  }
}

struct PhysicalCounts {
  std::size_t vertices = 0, faces = 0, corners = 0, face_offsets = 0;
  std::size_t attributes = 0, scalars = 0, offsets = 0, presence = 0;
  std::size_t metadata = 0, descriptors = 0;
};
void Counts(const PhysicalCounts& c, const MeshMaterializationLimits& l,
            Code code) {
  Limit(c.vertices, l.max_vertices, code);
  Limit(c.faces, l.max_faces, code);
  Limit(c.corners, l.max_corners, code);
  Limit(c.face_offsets, l.max_face_offsets, code);
  Limit(c.attributes, l.max_attributes, code);
  Limit(c.scalars, l.max_attribute_scalars, code);
  Limit(c.offsets, l.max_attribute_offsets, code);
  Limit(c.presence, l.max_presence_bytes, code);
  Limit(c.metadata, l.max_metadata_entries, code);
  Limit(c.descriptors, l.max_descriptor_bytes, code);
}
std::size_t ScalarCount(const ScalarReadView& values) {
  return std::visit([](const auto& span) { return span.size(); }, values);
}
void Descriptor(PhysicalCounts& c, const AttributeReadView& a,
                const MeshMaterializationLimits& limits, std::stop_token stop) {
  c.scalars = Add(c.scalars, ScalarCount(a.values));
  c.offsets = Add(c.offsets, a.offsets ? a.offsets->size() : 0);
  c.presence = Add(c.presence, a.present ? a.present->size() : 0);
  c.metadata = Add(c.metadata, a.metadata.size());
  c.descriptors = Add(c.descriptors, Add(a.name.size(), a.semantic.size()));
  Counts(c, limits, Code::kSourceCountLimit);
  for (const auto& item : a.metadata) {
    Stop(stop);
    c.descriptors = Add(c.descriptors, Add(item.key.size(), item.value.size()));
    Counts(c, limits, Code::kSourceCountLimit);
  }
}
PhysicalCounts Count(const MeshReadView& source,
                     const MeshMaterializationLimits& l, std::stop_token stop) {
  PhysicalCounts c;
  c.vertices = source.positions.size() / kPositionComponents;
  c.face_offsets = source.face_offsets.size();
  c.faces = c.face_offsets ? c.face_offsets - 1 : 0;
  c.corners = source.corner_vertices.size();
  c.attributes = source.attributes.size();
  Counts(c, l, Code::kSourceCountLimit);
  for (const auto& attribute : source.attributes) {
    Stop(stop);
    Descriptor(c, attribute, l, stop);
    Counts(c, l, Code::kSourceCountLimit);
  }
  return c;
}
PhysicalCounts Count(const Mesh& source, const MeshMaterializationLimits& l,
                     std::stop_token stop) {
  PhysicalCounts c;
  c.vertices = source.positions.size();
  c.face_offsets = source.face_offsets.size();
  c.faces = c.face_offsets ? c.face_offsets - 1 : 0;
  c.corners = source.corner_vertices.size();
  c.attributes = source.attributes.size();
  Counts(c, l, Code::kSourceCountLimit);
  for (const auto& a : source.attributes) {
    Stop(stop);
    c.scalars =
        Add(c.scalars,
            std::visit([](const auto& v) { return v.size(); }, a.values));
    c.offsets = Add(c.offsets, a.offsets ? a.offsets->size() : 0);
    c.presence = Add(c.presence, a.present ? a.present->size() : 0);
    c.metadata = Add(c.metadata, a.metadata.size());
    c.descriptors = Add(c.descriptors, Add(a.name.size(), a.semantic.size()));
    Counts(c, l, Code::kSourceCountLimit);
    for (const auto& [key, value] : a.metadata) {
      Stop(stop);
      c.descriptors = Add(c.descriptors, Add(key.size(), value.size()));
      Counts(c, l, Code::kSourceCountLimit);
    }
  }
  return c;
}
struct LegacyIngress {
  LegacyIngress(const AllocationContext& allocation, std::size_t position_count)
      : positions(allocation, position_count),
        metadata(Allocator<MetadataEntryView>(allocation)),
        attributes(Allocator<AttributeMaterializationInput>(allocation)) {}
  FlatArray<double> positions;
  ChargedVector<MetadataEntryView> metadata;
  ChargedVector<AttributeMaterializationInput> attributes;
};
struct PositionCopy {
  const Mesh& source;
  std::span<double> destination;
  std::stop_token stop;
  std::size_t block;
};
void CopyPositions(void* state, std::size_t begin, std::size_t end) {
  auto& copy = *static_cast<PositionCopy*>(state);
  std::size_t cached = std::numeric_limits<std::size_t>::max();
  std::array<double, kPositionComponents> row{};
  for (auto first = begin; first < end;) {
    WorkerStop(copy.stop);
    const auto count = std::min(copy.block, end - first);
    for (std::size_t i = 0; i < count; ++i) {
      const auto scalar = first + i;
      if (scalar / kPositionComponents != cached) {
        cached = scalar / kPositionComponents;
        row = copy.source.positions.Get(cached);
      }
      std::memcpy(&copy.destination[scalar], &row[scalar % kPositionComponents],
                  sizeof(double));
    }
    first += count;
  }
  WorkerStop(copy.stop);
}
MeshMaterializationInput Adapt(const Mesh& source, LegacyIngress& ingress,
                               const PhysicalCounts& counts,
                               const AllocationContext& allocation,
                               const ExecutionContext& execution,
                               const MeshMaterializationWorkPolicy& work,
                               std::stop_token stop,
                               const md::NativeWorkerBackend& backend,
                               MaterializationExecutionStats& stats) {
  PositionCopy copy{source, ingress.positions.Values(), stop,
                    static_cast<std::size_t>(work.cancellation_block_scalars)};
  md::RunRange(execution, allocation, work, copy.destination.size(),
               sizeof(double), CopyPositions, &copy, stop, stats, backend,
               &stats);
  ingress.metadata.reserve(counts.metadata);
  for (const auto& a : source.attributes)
    for (const auto& [key, value] : a.metadata) {
      Stop(stop);
      ingress.metadata.push_back({key, value});
    }
  ingress.attributes.reserve(counts.attributes);
  std::size_t metadata = 0;
  for (const auto& a : source.attributes) {
    Stop(stop);
    AttributeMaterializationInput input;
    input.domain = a.domain;
    input.name = a.name;
    input.semantic = a.semantic;
    input.set_index = a.set_index;
    input.components = a.components;
    input.values = std::visit(
        [](const auto& v) -> ScalarIngress { return v.Values(); }, a.values);
    if (a.offsets) input.offsets = std::span<const index_t>(*a.offsets);
    if (a.present) input.present = std::span<const std::uint8_t>(*a.present);
    input.metadata = std::span<const MetadataEntryView>(ingress.metadata)
                         .subspan(metadata, a.metadata.size());
    metadata = Add(metadata, a.metadata.size());
    ingress.attributes.push_back(input);
  }
  return {std::span<const double>(ingress.positions.Values()),
          PolygonTopologyInput{source.face_offsets, source.corner_vertices},
          ingress.attributes};
}

struct Maps {
  Maps(const AllocationContext& allocation, std::size_t face_count,
       std::size_t corner_count, std::size_t range_count)
      : faces(allocation, face_count),
        corners(allocation, corner_count),
        ranges(allocation, range_count),
        offsets(allocation, Add(face_count, 1)),
        vertices(allocation, corner_count) {}
  FlatArray<index_t> faces, corners, ranges, offsets, vertices;
};
struct MapCopy {
  const MeshReadView& source;
  const CoreFrame& core;
  Maps& maps;
  std::stop_token stop;
  std::size_t block;
};
void CopyMap(void* state, std::size_t begin, std::size_t end) {
  auto& copy = *static_cast<MapCopy*>(state);
  for (auto scalar = begin; scalar < end; ++scalar) {
    if ((scalar - begin) % copy.block == 0) WorkerStop(copy.stop);
    const auto triangle = scalar / kTriangleCorners;
    const auto axis = scalar % kTriangleCorners;
    const auto ranges = copy.maps.ranges.Values();
    const auto upper = std::upper_bound(ranges.begin(), ranges.end(), triangle);
    const auto face = static_cast<std::size_t>(upper - ranges.begin() - 1);
    const auto local = triangle - ranges[face];
    const auto corner = (*copy.core.faces)[face].triangles[local][axis];
    copy.maps.corners.Values()[scalar] = corner;
    copy.maps.vertices.Values()[scalar] = copy.source.corner_vertices[corner];
    if (axis == 0) {
      copy.maps.faces.Values()[triangle] = face;
      copy.maps.offsets.Values()[triangle] = scalar;
    }
  }
  WorkerStop(copy.stop);
}
using GatherValues =
    std::variant<FlatArray<float>, FlatArray<double>, FlatArray<std::int32_t>,
                 FlatArray<std::uint8_t>, FlatArray<std::uint16_t>,
                 FlatArray<std::uint32_t>, FlatArray<std::uint64_t>>;
struct AttributeGather {
  AttributeGather(const AllocationContext& allocation,
                  const AttributeReadView& source_view,
                  std::span<const index_t> selected_rows, std::stop_token stop)
      : source(source_view), rows(selected_rows) {
    if (source.offsets) {
      offsets = FlatArray<index_t>(allocation, Add(rows.size(), 1));
      offsets.Values()[0] = 0;
      for (std::size_t row = 0; row < rows.size(); ++row) {
        Stop(stop);
        const auto first = (*source.offsets)[rows[row]];
        const auto last = (*source.offsets)[rows[row] + 1];
        offsets.Values()[row + 1] = Add(offsets.Values()[row], last - first);
      }
      scalars = offsets.Values().back();
    } else
      scalars = Product(rows.size(), source.components);
    values = std::visit(
        [&](const auto& input) -> GatherValues {
          using T = typename std::remove_cvref_t<decltype(input)>::value_type;
          return FlatArray<T>(allocation, scalars);
        },
        source.values);
    if (source.present)
      present = FlatArray<std::uint8_t>(allocation, rows.size());
  }
  AttributeReadView source;
  std::span<const index_t> rows;
  std::size_t scalars = 0;
  GatherValues values;
  FlatArray<index_t> offsets;
  FlatArray<std::uint8_t> present;
};
struct GatherCopy {
  AttributeGather& gather;
  std::stop_token stop;
  std::size_t block;
  const TransferObserver* observer;
};
void CopyScalars(void* state, std::size_t begin, std::size_t end) {
  auto& copy = *static_cast<GatherCopy*>(state);
  auto& gather = copy.gather;
  std::visit(
      [&](auto& output) {
        using T = typename decltype(output.Values())::value_type;
        const auto input = std::get<std::span<const T>>(gather.source.values);
        for (auto scalar = begin; scalar < end; ++scalar) {
          if ((scalar - begin) % copy.block == 0) WorkerStop(copy.stop);
          std::size_t row;
          std::size_t offset;
          if (gather.source.offsets) {
            const auto offsets = gather.offsets.Values();
            row = static_cast<std::size_t>(
                std::upper_bound(offsets.begin(), offsets.end(), scalar) -
                offsets.begin() - 1);
            offset = (*gather.source.offsets)[gather.rows[row]] + scalar -
                     offsets[row];
          } else {
            row = scalar / gather.source.components;
            offset = Product(gather.rows[row], gather.source.components) +
                     scalar % gather.source.components;
          }
          std::memcpy(&output.Values()[scalar], &input[offset], sizeof(T));
          if (copy.observer && copy.observer->copied)
            copy.observer->copied(kCompletedScalarCount, copy.observer->state);
        }
      },
      gather.values);
  WorkerStop(copy.stop);
}
void CopyPresence(void* state, std::size_t begin, std::size_t end) {
  auto& copy = *static_cast<GatherCopy*>(state);
  auto& gather = copy.gather;
  for (auto row = begin; row < end; ++row) {
    if ((row - begin) % copy.block == 0) WorkerStop(copy.stop);
    gather.present.Values()[row] = (*gather.source.present)[gather.rows[row]];
  }
  WorkerStop(copy.stop);
}

// Borrowed output descriptors remain alive until MaterializeMesh returns. Each
// gather owns its real charged backing even when presence marks its row
// missing.
struct Candidate {
  explicit Candidate(const AllocationContext& allocation)
      : gathers(Allocator<AttributeGather>(allocation)),
        attributes(Allocator<AttributeMaterializationInput>(allocation)) {}
  ChargedVector<AttributeGather> gathers;
  ChargedVector<AttributeMaterializationInput> attributes;
};
void Assemble(Candidate& candidate, const MeshReadView& source,
              const Maps& maps, const AllocationContext& allocation,
              const ExecutionContext& execution,
              const MeshMaterializationWorkPolicy& work, std::stop_token stop,
              const md::NativeWorkerBackend& backend,
              MaterializationExecutionStats& stats,
              const TransferObserver* observer) {
  candidate.gathers.reserve(source.attributes.size());
  candidate.attributes.reserve(source.attributes.size());
  for (const auto& attribute : source.attributes) {
    Stop(stop);
    AttributeMaterializationInput input;
    input.domain = attribute.domain;
    input.name = attribute.name;
    input.semantic = attribute.semantic;
    input.set_index = attribute.set_index;
    input.components = attribute.components;
    input.metadata = attribute.metadata;
    if (attribute.domain == AttributeDomain::vertex) {
      input.values =
          std::visit([](const auto& values) -> ScalarIngress { return values; },
                     attribute.values);
      input.offsets = attribute.offsets;
      input.present = attribute.present;
    } else {
      const auto rows = attribute.domain == AttributeDomain::face
                            ? maps.faces.Values()
                            : maps.corners.Values();
      candidate.gathers.emplace_back(allocation, attribute, rows, stop);
      auto& gather = candidate.gathers.back();
      GatherCopy copy{gather, stop,
                      static_cast<std::size_t>(work.cancellation_block_scalars),
                      observer};
      const auto bytes = std::visit(
          [](const auto& values) {
            return sizeof(typename decltype(values.Values())::value_type);
          },
          gather.values);
      md::RunRange(execution, allocation, work, gather.scalars, bytes,
                   CopyScalars, &copy, stop, stats, backend, &stats);
      if (attribute.present)
        md::RunRange(execution, allocation, work, rows.size(),
                     sizeof(std::uint8_t), CopyPresence, &copy, stop, stats,
                     backend, &stats);
      input.values = std::visit(
          [](const auto& values) -> ScalarIngress { return values.Values(); },
          gather.values);
      if (attribute.offsets)
        input.offsets = std::span<const index_t>(gather.offsets.Values());
      if (attribute.present)
        input.present = std::span<const std::uint8_t>(gather.present.Values());
    }
    candidate.attributes.push_back(input);
  }
}

void PlanOutput(const MeshReadView& source, const Maps& maps,
                const MeshMaterializationLimits& limits, std::stop_token stop) {
  PhysicalCounts c;
  c.vertices = source.positions.size() / kPositionComponents;
  c.faces = maps.faces.Values().size();
  c.corners = maps.corners.Values().size();
  c.face_offsets = maps.offsets.Values().size();
  c.attributes = source.attributes.size();
  Counts(c, limits, Code::kOutputCountLimit);
  for (const auto& a : source.attributes) {
    Stop(stop);
    c.metadata = Add(c.metadata, a.metadata.size());
    c.descriptors = Add(c.descriptors, Add(a.name.size(), a.semantic.size()));
    for (const auto& entry : a.metadata) {
      Stop(stop);
      c.descriptors =
          Add(c.descriptors, Add(entry.key.size(), entry.value.size()));
    }
    if (a.domain == AttributeDomain::vertex) {
      c.scalars = Add(c.scalars, ScalarCount(a.values));
      c.offsets = Add(c.offsets, a.offsets ? a.offsets->size() : 0);
      c.presence = Add(c.presence, a.present ? a.present->size() : 0);
    } else {
      const auto rows = a.domain == AttributeDomain::face
                            ? maps.faces.Values()
                            : maps.corners.Values();
      if (a.offsets) {
        c.offsets = Add(c.offsets, Add(rows.size(), 1));
        for (const auto row : rows) {
          Stop(stop);
          c.scalars = Add(c.scalars, (*a.offsets)[row + 1] - (*a.offsets)[row]);
          Counts(c, limits, Code::kOutputCountLimit);
        }
      } else
        c.scalars = Add(c.scalars, Product(rows.size(), a.components));
      if (a.present) c.presence = Add(c.presence, rows.size());
    }
    Counts(c, limits, Code::kOutputCountLimit);
  }
}

ChargedTriangulationResult Run(const Mesh* legacy,
                               const MaterializedMesh* retained,
                               const TriangulationOptions& options,
                               const ChargedTriangulationLimits& limits,
                               const MeshMaterializationWorkPolicy& work,
                               const ExecutionContext& execution,
                               std::stop_token stop,
                               const md::AllocationBackend& backend,
                               const md::NativeWorkerBackend& workers,
                               std::shared_ptr<const State>& completed,
                               const TransferObserver* observer) {
  ChargedTriangulationResult result;
  auto& diagnostic = result.diagnostic;
  try {
    Validate(limits, options, work);
    (void)execution.WorkerBudget();
    Stop(stop);
    diagnostic.stage = Stage::kSourceCounts;
    const auto physical = legacy ? Count(*legacy, limits.source, stop)
                                 : Count(retained->View(), limits.source, stop);
    auto allocation = AllocationContext(execution, stop, backend)
                          .WithRequestLimit(static_cast<std::size_t>(
                              limits.max_bridge_new_requested_bytes));
    std::optional<LegacyIngress> ingress;
    std::optional<MaterializedMesh> source;
    if (legacy) {
      diagnostic.stage = Stage::kSourceCapture;
      ingress.emplace(allocation,
                      Product(physical.vertices, kPositionComponents));
      const auto input =
          Adapt(*legacy, *ingress, physical, allocation, execution, work, stop,
                workers, result.adaptation_execution);
      auto captured =
          MaterializeMesh(input, limits.source, work, execution, stop);
      result.source_capture = Facts(captured);
      Delegate(captured.status, Code::kMaterialization);
      source.emplace(*captured.mesh);
    } else
      source.emplace(*retained);
    diagnostic.stage = Stage::kSourceInspection;
    auto inspected =
        InspectMaterializedMesh(*source, limits.source_inspection, work, stop);
    result.source_inspection = Facts(inspected);
    Delegate(inspected.status, Code::kInspection);
    if (!inspected.inspection->IsStorageValid()) {
      diagnostic.storage_issue = inspected.inspection->Issues().front();
      diagnostic.attribute = diagnostic.storage_issue->attribute;
      Fail(Status::kBlocked, Code::kSourceStorage);
    }
    const auto view = source->View();
    diagnostic.stage = Stage::kTriangulation;
    std::size_t triangles = 0;
    for (std::size_t face = 0; face < physical.faces; ++face) {
      Stop(stop);
      const auto count = view.face_offsets[face + 1] - view.face_offsets[face];
      if (count > options.max_corners_per_face) {
        diagnostic.face = face;
        Fail(Status::kBlocked, Code::kFaceCornerLimit);
      }
      triangles = Add(triangles, count - td::kMinimumFaceCorners + 1);
    }
    const auto corners = Product(triangles, kTriangleCorners);
    const auto ranges = Add(physical.faces, 1);
    Limit(triangles, limits.output.max_faces, Code::kOutputCountLimit);
    Limit(corners, limits.output.max_corners, Code::kOutputCountLimit);
    Limit(Add(triangles, 1), limits.output.max_face_offsets,
          Code::kOutputCountLimit);
    Limit(physical.vertices, limits.output.max_vertices,
          Code::kOutputCountLimit);
    Limit(Add(Add(triangles, corners), ranges), limits.max_map_entries,
          Code::kMapEntryLimit);
    result.counts = ChargedTriangulationCounts{
        physical.vertices, physical.faces, physical.corners,
        physical.vertices, triangles,      corners};

    // Must be directly constructed here, never returned/moved via a factory:
    // its allocation backend.state borrows this frame's observer address.
    CoreFrame core(execution, stop, backend);
    result.core.emplace();
    try {
      core.Begin(static_cast<std::size_t>(limits.max_core_new_requested_bytes));
      RunCore(core, view, options, execution, stop, workers, result);
    } catch (const AdmissionFailure& error) {
      result.core->new_requested_peak_bytes =
          core.observer.peak.load(std::memory_order_relaxed);
      if (error.Code() == AdmissionCode::kCanceled)
        result.core->status = TriangulationStatus::kCanceled;
      Admission(error, Code::kCoreRequestBudget);
    }
    diagnostic.stage = Stage::kCorrespondence;
    Maps maps(allocation, triangles, corners, ranges);
    auto prefix = maps.ranges.Values();
    prefix[0] = 0;
    for (std::size_t face = 0; face < physical.faces; ++face) {
      Stop(stop);
      prefix[face + 1] =
          Add(prefix[face], (*core.faces)[face].triangles.size());
    }
    if (prefix.back() != triangles) std::terminate();
    maps.offsets.Values().back() = corners;
    MapCopy map_copy{view, core, maps, stop,
                     static_cast<std::size_t>(work.cancellation_block_scalars)};
    md::RunRange(execution, allocation, work, corners, sizeof(index_t), CopyMap,
                 &map_copy, stop, result.transfer_execution, workers,
                 &result.transfer_execution);
    diagnostic.stage = Stage::kAttributeTransfer;
    PlanOutput(view, maps, limits.output, stop);
    Candidate candidate(allocation);
    Assemble(candidate, view, maps, allocation, execution, work, stop, workers,
             result.transfer_execution, observer);
    diagnostic.stage = Stage::kOutputMaterialization;
    MeshMaterializationInput output{
        view.positions,
        PolygonTopologyInput{maps.offsets.Values(), maps.vertices.Values()},
        candidate.attributes};
    auto materialized =
        MaterializeMesh(output, limits.output, work, execution, stop);
    result.output_materialization = Facts(materialized);
    Delegate(materialized.status, Code::kMaterialization);
    diagnostic.stage = Stage::kOutputInspection;
    auto verified = InspectMaterializedMesh(
        *materialized.mesh, limits.output_inspection, work, stop);
    result.output_inspection = Facts(verified);
    Delegate(verified.status, Code::kInspection);
    if (!verified.inspection->IsStorageValid()) {
      diagnostic.storage_issue = verified.inspection->Issues().front();
      diagnostic.attribute = diagnostic.storage_issue->attribute;
      Fail(Status::kBlocked, Code::kOutputStorage);
    }
    auto owner = std::allocate_shared<State>(
        Allocator<State>(allocation), allocation, *materialized.mesh,
        *verified.inspection, *result.counts, std::move(maps.faces),
        std::move(maps.corners), std::move(maps.ranges));
    Stop(stop);
    completed = std::move(owner);
    result.status = Status::kAccepted;
    diagnostic = {Stage::kComplete, Code::kNone, {}, {}, {}};
  } catch (const AdmissionFailure& error) {
    if (error.Code() == AdmissionCode::kInvalidObject) throw;
    result.status = error.Code() == AdmissionCode::kCanceled ? Status::kCanceled
                    : error.Code() == AdmissionCode::kSizeLimit
                        ? Status::kLimitExceeded
                        : Status::kBudgetExceeded;
    diagnostic.code =
        error.Code() == AdmissionCode::kCanceled    ? Code::kStopRequested
        : error.Code() == AdmissionCode::kSizeLimit ? Code::kSizeOverflow
        : diagnostic.stage == Stage::kTriangulation
            ? Code::kCoreRequestBudget
            : Code::kBridgeRequestBudget;
    if (stop.stop_requested()) {
      result.status = Status::kCanceled;
      diagnostic.code = Code::kStopRequested;
      diagnostic.face.reset();
      diagnostic.attribute.reset();
      diagnostic.storage_issue.reset();
    }
  } catch (const Failure& error) {
    result.status = error.status;
    diagnostic.code = error.code;
    if (error.status != Status::kInvalidConfiguration &&
        stop.stop_requested()) {
      result.status = Status::kCanceled;
      diagnostic.code = Code::kStopRequested;
      diagnostic.face.reset();
      diagnostic.attribute.reset();
      diagnostic.storage_issue.reset();
    }
  }
  return result;
}
}  // namespace

ChargedTriangulationResult Access::Run(
    const Mesh& source, const TriangulationOptions& options,
    const ChargedTriangulationLimits& limits,
    const MeshMaterializationWorkPolicy& work,
    const ExecutionContext& execution, std::stop_token stop,
    const md::AllocationBackend& backend,
    const md::NativeWorkerBackend& workers, const TransferObserver* observer) {
  std::shared_ptr<const State> owner;
  auto result = charged_triangulation_detail::Run(
      &source, nullptr, options, limits, work, execution, stop, backend,
      workers, owner, observer);
  if (owner) result.triangulation = ChargedTriangulation(std::move(owner));
  return result;
}
ChargedTriangulationResult Access::Run(
    const MaterializedMesh& source, const TriangulationOptions& options,
    const ChargedTriangulationLimits& limits,
    const MeshMaterializationWorkPolicy& work, std::stop_token stop,
    const md::AllocationBackend& backend,
    const md::NativeWorkerBackend& workers, const TransferObserver* observer) {
  const MaterializedMesh retained(source);
  const auto execution = retained.ShareExecutionContext();
  std::shared_ptr<const State> owner;
  auto result = charged_triangulation_detail::Run(
      nullptr, &retained, options, limits, work, execution, stop, backend,
      workers, owner, observer);
  if (owner) result.triangulation = ChargedTriangulation(std::move(owner));
  return result;
}
}  // namespace charged_triangulation_detail

ChargedTriangulation::ChargedTriangulation(
    std::shared_ptr<const charged_triangulation_detail::State> state) noexcept
    : state_(std::move(state)) {}
ChargedTriangulation::ChargedTriangulation(
    const ChargedTriangulation&) noexcept = default;
ChargedTriangulation& ChargedTriangulation::operator=(
    const ChargedTriangulation&) noexcept = default;
ChargedTriangulation::~ChargedTriangulation() noexcept = default;
const MaterializedMesh& ChargedTriangulation::Mesh() const noexcept {
  return state_->mesh;
}
const MeshInspection& ChargedTriangulation::Inspection() const noexcept {
  return state_->inspection;
}
const ChargedTriangulationCounts& ChargedTriangulation::Counts()
    const noexcept {
  return state_->counts;
}
std::span<const index_t> ChargedTriangulation::FaceSources() const noexcept {
  return state_->face_sources.Values();
}
std::span<const index_t> ChargedTriangulation::CornerSources() const noexcept {
  return state_->corner_sources.Values();
}
std::span<const index_t> ChargedTriangulation::FaceOutputOffsets()
    const noexcept {
  return state_->face_output_offsets.Values();
}
ExecutionContext ChargedTriangulation::ShareExecutionContext() const noexcept {
  return state_->mesh.ShareExecutionContext();
}
std::uint64_t ChargedTriangulation::NewRequestedBytes() const noexcept {
  // These are three deliberately distinct operation owners on one ledger.
  return state_->allocation.OperationActiveBytes() +
         state_->mesh.NewRequestedBytes() +
         state_->inspection.NewRequestedBytes();
}
ChargedTriangulationResult TriangulateCharged(
    const Mesh& source, const TriangulationOptions& options,
    const ChargedTriangulationLimits& limits,
    const MeshMaterializationWorkPolicy& work,
    const ExecutionContext& execution, std::stop_token stop) {
  return charged_triangulation_detail::Access::Run(
      source, options, limits, work, execution, stop,
      materialization_detail::DefaultAllocationBackend(),
      materialization_detail::DefaultNativeWorkerBackend());
}
ChargedTriangulationResult TriangulateCharged(
    const MaterializedMesh& source, const TriangulationOptions& options,
    const ChargedTriangulationLimits& limits,
    const MeshMaterializationWorkPolicy& work, std::stop_token stop) {
  return charged_triangulation_detail::Access::Run(
      source, options, limits, work, stop,
      materialization_detail::DefaultAllocationBackend(),
      materialization_detail::DefaultNativeWorkerBackend());
}
}  // namespace meshvale::geometry
