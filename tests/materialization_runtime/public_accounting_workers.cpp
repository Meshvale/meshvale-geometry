// SPDX-License-Identifier: Apache-2.0
// Prepared consumer test source. Execute only against a qualified runtime
// build.
#include <meshvale/geometry/editable_mesh.h>
#include <meshvale/geometry/execution.h>
#include <meshvale/geometry/materialization.h>
#include <meshvale/geometry/vector_buffer.h>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <exception>
#include <initializer_list>
#include <iostream>
#include <limits>
#include <optional>
#include <span>
#include <stdexcept>
#include <stop_token>
#include <string_view>
#include <thread>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace {
namespace geo = meshvale::geometry;
constexpr std::uint64_t kBytesPerKiB = 1024;
constexpr std::uint64_t kBytesPerMiB = kBytesPerKiB * kBytesPerKiB;
constexpr std::uint64_t kLedgerBudgetBytes = 64 * kBytesPerMiB;
constexpr std::size_t kWorkerBudget = 2;
constexpr std::size_t kFaceSelectionStride = 2;
constexpr std::size_t kAttributeCount = 3;
constexpr std::size_t kVertexAttributeSlot = 0;
constexpr std::size_t kFaceAttributeSlot = 1;
constexpr std::size_t kCornerAttributeSlot = 2;
constexpr std::size_t kVertexCount = 4096;
constexpr std::size_t kFaceCount = 4096;
constexpr std::size_t kTriangleCorners = 3;
constexpr std::size_t kCornerCount = kFaceCount * kTriangleCorners;
constexpr std::size_t kLongRowScalars = 65536;
constexpr std::size_t kCancellationLongRowScalars = 1024 * 1024;
constexpr std::uint64_t kLeafPartitionBytes = 1024;
constexpr std::uint64_t kMaximumJobs = 64;
constexpr std::uint64_t kCancellationBlockScalars = 64;
constexpr std::uint64_t kForeignPayloadBytes = 257;
constexpr std::size_t kDefectVertexStride = 17;
constexpr std::size_t kCancellationAttempts = 4;
constexpr std::string_view kVertexAttributeName = "long-vertex-row";
constexpr std::string_view kFaceAttributeName = "face-values";
constexpr std::string_view kCornerAttributeName = "corner-values";
constexpr std::string_view kAttributeSemantic = "proof.scalar";
constexpr std::string_view kMetadataKey = "proof-kind";
constexpr std::string_view kMetadataValue = "accounting-worker-consumer";

void Require(bool condition, const char* message) {
  if (!condition) throw std::runtime_error(message);
}

geo::ExecutionContext Context(std::uint64_t bytes = kLedgerBudgetBytes) {
  geo::ExecutionOptions options;
  options.worker_budget = kWorkerBudget;
  options.tracked_payload_budget_bytes = static_cast<std::size_t>(bytes);
  return geo::ExecutionContext(options);
}

geo::MeshMaterializationWorkPolicy ParallelWork() {
  geo::MeshMaterializationWorkPolicy work;
  work.parallel_min_bytes = 1;
  work.partition_bytes = kLeafPartitionBytes;
  work.cancellation_block_scalars = kCancellationBlockScalars;
  work.max_jobs = kMaximumJobs;
  return work;
}

geo::MeshMaterializationWorkPolicy SerialWork() {
  auto work = ParallelWork();
  work.parallel_min_bytes = kLedgerBudgetBytes;
  return work;
}

struct Fixture {
  std::vector<double> positions;
  std::vector<geo::index_t> face_offsets;
  std::vector<geo::index_t> corners;
  std::vector<float> vertex_values;
  std::vector<geo::index_t> vertex_offsets;
  std::vector<std::uint8_t> vertex_presence;
  std::vector<std::uint32_t> face_values;
  std::vector<double> corner_values;
  std::vector<geo::index_t> kept_faces;
  std::array<geo::MetadataEntryView, 1> metadata{
      {{kMetadataKey, kMetadataValue}}};

  explicit Fixture(std::size_t long_row = kLongRowScalars)
      : positions(kVertexCount * geo::kVector3Components),
        face_offsets(kFaceCount + 1),
        corners(kCornerCount),
        vertex_values(long_row, 1.0f),
        vertex_offsets(kVertexCount + 1, long_row),
        vertex_presence(kVertexCount, 1),
        face_values(kFaceCount),
        corner_values(kCornerCount) {
    vertex_offsets.front() = 0;
    for (std::size_t vertex = 0; vertex < kVertexCount; ++vertex) {
      positions[vertex * geo::kVector3Components] = static_cast<double>(vertex);
      positions[vertex * geo::kVector3Components + 1] = 1.0;
    }
    for (std::size_t face = 0; face < kFaceCount; ++face) {
      face_offsets[face] = face * kTriangleCorners;
      face_values[face] = static_cast<std::uint32_t>(face);
      for (std::size_t local = 0; local < kTriangleCorners; ++local) {
        const auto corner = face * kTriangleCorners + local;
        corners[corner] = local;
        corner_values[corner] = static_cast<double>(corner);
      }
      if (face % kFaceSelectionStride == 0) kept_faces.push_back(face);
    }
    face_offsets.back() = corners.size();
  }

  std::array<geo::AttributeMaterializationInput, kAttributeCount> Attributes()
      const {
    std::array<geo::AttributeMaterializationInput, kAttributeCount> attributes;
    auto& vertex = attributes[kVertexAttributeSlot];
    vertex.domain = geo::AttributeDomain::vertex;
    vertex.name = kVertexAttributeName;
    vertex.semantic = kAttributeSemantic;
    vertex.components = 1;
    vertex.values = std::span<const float>(vertex_values);
    vertex.offsets = std::span<const geo::index_t>(vertex_offsets);
    vertex.present = std::span<const std::uint8_t>(vertex_presence);
    vertex.metadata = metadata;
    auto& face = attributes[kFaceAttributeSlot];
    face.domain = geo::AttributeDomain::face;
    face.name = kFaceAttributeName;
    face.semantic = kAttributeSemantic;
    face.components = 1;
    face.values = std::span<const std::uint32_t>(face_values);
    face.metadata = metadata;
    auto& corner = attributes[kCornerAttributeSlot];
    corner.domain = geo::AttributeDomain::corner;
    corner.name = kCornerAttributeName;
    corner.semantic = kAttributeSemantic;
    corner.components = 1;
    corner.values = std::span<const double>(corner_values);
    corner.metadata = metadata;
    return attributes;
  }

  geo::MeshMaterializationLimits Limits(
      std::uint64_t cap = kLedgerBudgetBytes) const {
    geo::MeshMaterializationLimits limits;
    limits.max_vertices = kVertexCount;
    limits.max_faces = kFaceCount;
    limits.max_corners = kCornerCount;
    limits.max_face_offsets = kFaceCount + 1;
    limits.max_attributes = kAttributeCount;
    limits.max_attribute_scalars =
        vertex_values.size() + face_values.size() + corner_values.size();
    limits.max_attribute_offsets = vertex_offsets.size();
    limits.max_presence_bytes = vertex_presence.size();
    limits.max_metadata_entries = kAttributeCount * metadata.size();
    limits.max_descriptor_bytes = kBytesPerKiB;
    limits.max_new_requested_bytes = cap;
    return limits;
  }

  geo::MeshMaterializationResult Capture(
      const geo::ExecutionContext& context,
      const geo::MeshMaterializationWorkPolicy& work,
      std::uint64_t cap = kLedgerBudgetBytes, std::stop_token stop = {}) const {
    const auto attributes = Attributes();
    geo::MeshMaterializationInput input;
    input.positions = std::span<const double>(positions);
    input.topology = geo::PolygonTopologyInput{face_offsets, corners};
    input.attributes = attributes;
    return geo::MaterializeMesh(input, Limits(cap), work, context, stop);
  }
};

void Joined(const geo::MaterializationExecutionStats& stats) {
  Require(stats.launched_workers == stats.joined_workers,
          "A returned result must report every launched worker joined");
}

void Successful(const geo::MeshMaterializationResult& result) {
  Require(result.status == geo::MaterializationStatus::kSuccess && result.mesh,
          "Capture must succeed on the qualified allocation profile");
  Joined(result.execution);
}

void Released(const geo::ExecutionContext& context,
              std::uint64_t expected_payload = 0) {
  Require(context.ActiveWorkers() == 0, "Worker reservations leaked");
  Require(context.ActiveTrackedPayload() == expected_payload,
          "Payload reservations leaked");
}

void SameBytes(std::span<const std::byte> first,
               std::span<const std::byte> second) {
  Require(first.size() == second.size(), "Byte extents differ");
  Require(first.empty() ||
              std::memcmp(first.data(), second.data(), first.size()) == 0,
          "Serial and parallel bytes differ");
}

template <class T>
void SameScalars(std::span<const T> first, std::span<const T> second) {
  SameBytes(std::as_bytes(first), std::as_bytes(second));
}

void SameOptionalRows(
    const std::optional<std::span<const geo::index_t>>& first,
    const std::optional<std::span<const geo::index_t>>& second) {
  Require(first.has_value() == second.has_value(),
          "Ragged optional presence differs");
  if (first) SameScalars(*first, *second);
}

void SameMesh(geo::MeshReadView first, geo::MeshReadView second) {
  SameScalars(first.positions, second.positions);
  SameScalars(first.face_offsets, second.face_offsets);
  SameScalars(first.corner_vertices, second.corner_vertices);
  Require(first.attributes.size() == second.attributes.size(),
          "Attribute count differs");
  for (std::size_t index = 0; index < first.attributes.size(); ++index) {
    const auto& left = first.attributes[index];
    const auto& right = second.attributes[index];
    Require(left.domain == right.domain && left.name == right.name &&
                left.semantic == right.semantic &&
                left.set_index == right.set_index &&
                left.components == right.components,
            "Attribute descriptor differs");
    Require(left.values.index() == right.values.index(),
            "Scalar encoding differs");
    std::visit(
        [&](auto span) {
          using Span = std::remove_cvref_t<decltype(span)>;
          SameScalars(span, std::get<Span>(right.values));
        },
        left.values);
    SameOptionalRows(left.offsets, right.offsets);
    Require(left.present.has_value() == right.present.has_value(),
            "Presence optional differs");
    if (left.present) SameScalars(*left.present, *right.present);
    Require(left.metadata.size() == right.metadata.size(),
            "Metadata count differs");
    for (std::size_t entry = 0; entry < left.metadata.size(); ++entry)
      Require(left.metadata[entry].key == right.metadata[entry].key &&
                  left.metadata[entry].value == right.metadata[entry].value,
              "Metadata differs");
  }
}

void SameIssues(std::span<const geo::StorageIssue> first,
                std::span<const geo::StorageIssue> second) {
  Require(first.size() == second.size(), "Issue count differs");
  for (std::size_t index = 0; index < first.size(); ++index)
    Require(first[index].code == second[index].code &&
                first[index].attribute == second[index].attribute &&
                first[index].element == second[index].element,
            "Ordered issue differs");
}

geo::MeshInspectionLimits InspectionLimits(
    std::uint64_t cap = kLedgerBudgetBytes) {
  geo::MeshInspectionLimits limits;
  limits.max_issues = kVertexCount;
  limits.max_new_requested_bytes = cap;
  return limits;
}

void CheckSerialParallel(const Fixture& fixture) {
  auto serial_context = Context();
  auto parallel_context = Context();
  auto serial = fixture.Capture(serial_context, SerialWork());
  auto parallel = fixture.Capture(parallel_context, ParallelWork());
  Successful(serial);
  Successful(parallel);
  Require(serial.execution.launched_workers == 0,
          "Serial threshold launched workers");
  Require(parallel.execution.launched_workers > 0,
          "Substantial parallel capture launched no workers");
  SameMesh(serial.mesh->View(), parallel.mesh->View());
  auto serial_selection = geo::SelectMeshFaces(*serial.mesh, fixture.kept_faces,
                                               fixture.Limits(), SerialWork());
  auto parallel_selection = geo::SelectMeshFaces(
      *parallel.mesh, fixture.kept_faces, fixture.Limits(), ParallelWork());
  Require(serial_selection.selection && parallel_selection.selection &&
              serial_selection.status == geo::MaterializationStatus::kSuccess &&
              parallel_selection.status == geo::MaterializationStatus::kSuccess,
          "Selections must succeed");
  Joined(serial_selection.execution);
  Joined(parallel_selection.execution);
  Require(parallel_selection.execution.launched_workers > 0,
          "Parallel selection launched no workers");
  SameMesh(serial_selection.selection->Candidate().View(),
           parallel_selection.selection->Candidate().View());
  SameScalars(serial_selection.selection->SourceFaces(),
              parallel_selection.selection->SourceFaces());
  SameScalars(serial_selection.selection->SourceCorners(),
              parallel_selection.selection->SourceCorners());
  serial_selection.selection.reset();
  parallel_selection.selection.reset();
  serial.mesh.reset();
  parallel.mesh.reset();
  Released(serial_context);
  Released(parallel_context);
}

void CheckParallelIssueOrder() {
  Fixture fixture;
  for (std::size_t vertex = 0; vertex < kVertexCount;
       vertex += kDefectVertexStride)
    fixture.positions[vertex * geo::kVector3Components] =
        std::numeric_limits<double>::quiet_NaN();
  auto context = Context();
  auto source = fixture.Capture(context, ParallelWork());
  Successful(source);
  auto serial = geo::InspectMaterializedMesh(*source.mesh, InspectionLimits(),
                                             SerialWork());
  auto parallel = geo::InspectMaterializedMesh(*source.mesh, InspectionLimits(),
                                               ParallelWork());
  Require(serial.inspection && parallel.inspection &&
              serial.status == geo::MaterializationStatus::kSuccess &&
              parallel.status == geo::MaterializationStatus::kSuccess,
          "Inspection must succeed on representable defective storage");
  Joined(serial.execution);
  Joined(parallel.execution);
  Require(parallel.execution.launched_workers > 0,
          "Substantial parallel inspection launched no workers");
  SameIssues(serial.inspection->Issues(), parallel.inspection->Issues());
  serial.inspection.reset();
  parallel.inspection.reset();
  source.mesh.reset();
  Released(context);
}

std::uint64_t MeasureCapturePeak(const Fixture& fixture) {
  auto context = Context();
  auto result = fixture.Capture(context, ParallelWork());
  Successful(result);
  const auto peak = context.PeakTrackedPayload();
  Require(peak >= result.mesh->NewRequestedBytes() && peak > 1,
          "Capture peak is inconsistent");
  Require(context.ActiveTrackedPayload() == result.mesh->NewRequestedBytes(),
          "Current capture charge differs");
  result.mesh.reset();
  Released(context);
  return peak;
}

void CheckCaptureCapEdges(const Fixture& fixture) {
  const auto peak = MeasureCapturePeak(fixture);
  for (bool one_byte_less : {false, true}) {
    auto context = Context();
    auto result =
        fixture.Capture(context, ParallelWork(), peak - one_byte_less);
    Joined(result.execution);
    if (one_byte_less) {
      Require(result.status == geo::MaterializationStatus::kBudgetExceeded &&
                  !result.mesh &&
                  result.diagnostic.code ==
                      geo::MaterializationDiagnosticCode::kRequestBudget,
              "One byte below observed fresh-context request peak must refuse");
    } else {
      Successful(result);
      Require(context.PeakTrackedPayload() == peak,
              "Exact-cap execution changed its request peak");
    }
    result.mesh.reset();
    Released(context);
  }
  for (bool one_byte_less : {false, true}) {
    auto context = Context(kForeignPayloadBytes + peak - one_byte_less);
    auto foreign = geo::TryReservePayload(context, kForeignPayloadBytes);
    Require(foreign.status == geo::ReservationStatus::kAccepted,
            "Foreign lease admission failed");
    auto result = fixture.Capture(context, ParallelWork(), peak);
    Joined(result.execution);
    if (one_byte_less)
      Require(result.status == geo::MaterializationStatus::kBudgetExceeded &&
                  !result.mesh,
              "Shared ledger must bound retained/new overlap");
    else
      Successful(result);
    result.mesh.reset();
    Released(context, kForeignPayloadBytes);
    foreign.lease.Reset();
    Released(context);
  }
  std::cout << "capture_fresh_context_peak_bytes=" << peak << '\n';
}

enum class SubsequentOperation { kInspect, kSelect };

bool SubsequentCapTrial(const Fixture& fixture, SubsequentOperation operation,
                        std::uint64_t cap) {
  auto context = Context();
  auto source = fixture.Capture(context, SerialWork());
  Successful(source);
  const auto retained = context.ActiveTrackedPayload();
  bool accepted;
  if (operation == SubsequentOperation::kInspect) {
    auto result = geo::InspectMaterializedMesh(
        *source.mesh, InspectionLimits(cap), ParallelWork());
    Joined(result.execution);
    accepted = result.status == geo::MaterializationStatus::kSuccess;
    if (accepted) {
      Require(
          result.inspection && result.inspection->NewRequestedBytes() <= cap,
          "Inspection cap/current accounting is inconsistent");
      Require(context.ActiveTrackedPayload() ==
                  retained + result.inspection->NewRequestedBytes(),
              "Inspection double-charged source or omitted new requests");
    } else
      Require(result.status == geo::MaterializationStatus::kBudgetExceeded &&
                  !result.inspection,
              "Expected a new-operation cap refusal");
    result.inspection.reset();
  } else {
    auto result = geo::SelectMeshFaces(*source.mesh, fixture.kept_faces,
                                       fixture.Limits(cap), ParallelWork());
    Joined(result.execution);
    accepted = result.status == geo::MaterializationStatus::kSuccess;
    if (accepted) {
      Require(result.selection && result.selection->NewRequestedBytes() <= cap,
              "Selection cap/current accounting is inconsistent");
      Require(context.ActiveTrackedPayload() ==
                  retained + result.selection->NewRequestedBytes(),
              "Selection double-charged shared vertex part or omitted new "
              "requests");
    } else
      Require(result.status == geo::MaterializationStatus::kBudgetExceeded &&
                  !result.selection,
              "Expected a new-operation cap refusal");
    result.selection.reset();
  }
  Released(context, retained);
  source.mesh.reset();
  Released(context);
  return accepted;
}

void CheckSubsequentCapEdges(const Fixture& fixture,
                             SubsequentOperation operation) {
  // Public API has no fresh-operation peak accessor. This finds an observed
  // minimum successful cap; it is not an original-request-plan oracle.
  std::uint64_t lower = 1, upper = kLedgerBudgetBytes;
  Require(!SubsequentCapTrial(fixture, operation, lower),
          "Seed/control must require more than one byte");
  Require(SubsequentCapTrial(fixture, operation, upper),
          "Upper cap must admit subsequent operation");
  while (upper - lower > 1) {
    const auto middle = lower + (upper - lower) / 2;
    if (SubsequentCapTrial(fixture, operation, middle))
      upper = middle;
    else
      lower = middle;
  }
  Require(SubsequentCapTrial(fixture, operation, upper),
          "Observed boundary must succeed");
  Require(!SubsequentCapTrial(fixture, operation, upper - 1),
          "One byte below observed boundary must refuse");
  std::cout << (operation == SubsequentOperation::kInspect ? "inspection"
                                                           : "selection")
            << "_observed_minimum_new_cap_bytes=" << upper << '\n';
}

void CheckEmptySeedControl() {
  Fixture configuration_fixture;
  std::array<geo::index_t, 1> offsets{0};
  geo::MeshMaterializationInput input;
  input.positions = std::span<const double>{};
  input.topology = geo::PolygonTopologyInput{offsets, {}};
  for (bool ledger_refusal : {false, true}) {
    auto context = Context(ledger_refusal ? 1 : kLedgerBudgetBytes);
    auto limits =
        configuration_fixture.Limits(ledger_refusal ? kLedgerBudgetBytes : 1);
    auto result = geo::MaterializeMesh(input, limits, ParallelWork(), context);
    Require(result.status == geo::MaterializationStatus::kBudgetExceeded &&
                !result.mesh &&
                result.diagnostic.code ==
                    geo::MaterializationDiagnosticCode::kRequestBudget,
            "Empty output must not bypass initial seed/control accounting");
    Joined(result.execution);
    Released(context);
  }
  auto context = Context();
  auto result = geo::MaterializeMesh(input, configuration_fixture.Limits(),
                                     ParallelWork(), context);
  Successful(result);
  Require(
      result.mesh->NewRequestedBytes() > 0 &&
          context.ActiveTrackedPayload() == result.mesh->NewRequestedBytes(),
      "Empty owner/control requests are uncharged");
  result.mesh.reset();
  Released(context);
}

void CheckOwnerAndTokenLifetime(const Fixture& fixture) {
  auto witness = Context();
  std::optional<geo::MaterializedMesh> retained;
  {
    auto original_context = witness;
    std::stop_source earlier_stop;
    auto source = fixture.Capture(original_context, ParallelWork(),
                                  kLedgerBudgetBytes, earlier_stop.get_token());
    Successful(source);
    const auto original_charge = witness.ActiveTrackedPayload();
    retained.emplace(*source.mesh);
    Require(witness.ActiveTrackedPayload() == original_charge,
            "Copying an immutable owner allocated/charged");
    earlier_stop.request_stop();
    source.mesh.reset();
    Require(witness.ActiveTrackedPayload() == original_charge,
            "Copy failed to retain complete storage");
  }
  const auto old_charge = witness.ActiveTrackedPayload();
  auto inspection = geo::InspectMaterializedMesh(*retained, InspectionLimits(),
                                                 ParallelWork());
  Require(
      inspection.status == geo::MaterializationStatus::kSuccess &&
          inspection.inspection,
      "Old stop source/context destruction must not cancel fresh inspection");
  Require(witness.ActiveTrackedPayload() ==
              old_charge + inspection.inspection->NewRequestedBytes(),
          "Inspection failed retained-source/new-operation separation");
  inspection.inspection.reset();
  auto selection = geo::SelectMeshFaces(*retained, fixture.kept_faces,
                                        fixture.Limits(), ParallelWork());
  Require(selection.status == geo::MaterializationStatus::kSuccess &&
              selection.selection,
          "Fresh selection after context destruction must succeed");
  std::optional<geo::MaterializedMesh> candidate(
      selection.selection->Candidate());
  const auto selection_live = candidate->NewRequestedBytes();
  selection.selection.reset();
  Require(candidate->NewRequestedBytes() < selection_live,
          "Released correspondence requests remained charged");
  const auto with_old_faces = witness.ActiveTrackedPayload();
  retained.reset();
  Require(witness.ActiveTrackedPayload() < with_old_faces,
          "Selection retained obsolete source face/corner storage");
  auto candidate_inspection = geo::InspectMaterializedMesh(
      *candidate, InspectionLimits(), ParallelWork());
  Require(candidate_inspection.status == geo::MaterializationStatus::kSuccess &&
              candidate_inspection.inspection,
          "Candidate lost its retained context/vertex storage");
  candidate_inspection.inspection.reset();
  candidate.reset();
  Released(witness);
}

void CheckFullyDestroyedContext(const Fixture& fixture) {
  std::optional<geo::MaterializedMesh> retained;
  {
    auto only_visible_context = Context();
    auto source = fixture.Capture(only_visible_context, ParallelWork());
    Successful(source);
    retained.emplace(*source.mesh);
  }
  // No visible ExecutionContext survives; the immutable owner must retain it.
  auto inspected = geo::InspectMaterializedMesh(*retained, InspectionLimits(),
                                                ParallelWork());
  Require(
      inspected.status == geo::MaterializationStatus::kSuccess &&
          inspected.inspection,
      "Owner did not retain ledger after every visible context was destroyed");
  Joined(inspected.execution);
  auto selected = geo::SelectMeshFaces(*retained, fixture.kept_faces,
                                       fixture.Limits(), ParallelWork());
  Require(selected.status == geo::MaterializationStatus::kSuccess &&
              selected.selection,
          "Selection lost ledger after every visible context was destroyed");
  Joined(selected.execution);
}

void CheckWorkerContention(const Fixture& fixture) {
  auto context = Context();
  auto held = geo::TryReserveWorkers(context, kWorkerBudget);
  Require(held.Count() == kWorkerBudget, "Could not hold shared slots");
  auto result = fixture.Capture(context, ParallelWork());
  Successful(result);
  Require(result.execution.launched_workers == 0 &&
              result.execution.caller_jobs > 0,
          "Held shared slots must cause actual caller fallback");
  Require(context.ActiveWorkers() == kWorkerBudget,
          "Operation released another owner's slots");
  result.mesh.reset();
  Require(context.ActiveTrackedPayload() == 0,
          "Fallback capture leaked payload");
  held.Reset();
  Released(context);
}

void CheckPreCanceled(const Fixture& fixture) {
  auto context = Context();
  std::stop_source stop;
  stop.request_stop();
  auto result = fixture.Capture(context, ParallelWork(), kLedgerBudgetBytes,
                                stop.get_token());
  Require(result.status == geo::MaterializationStatus::kCanceled &&
              !result.mesh &&
              result.diagnostic.code ==
                  geo::MaterializationDiagnosticCode::kStopRequested,
          "Pre-canceled capture did not return its fixed cancellation failure");
  Joined(result.execution);
  Released(context);
  auto source = fixture.Capture(context, ParallelWork());
  Successful(source);
  const auto old_charge = context.ActiveTrackedPayload();
  auto inspect = geo::InspectMaterializedMesh(*source.mesh, InspectionLimits(),
                                              ParallelWork(), stop.get_token());
  auto select =
      geo::SelectMeshFaces(*source.mesh, fixture.kept_faces, fixture.Limits(),
                           ParallelWork(), stop.get_token());
  Require(inspect.status == geo::MaterializationStatus::kCanceled &&
              !inspect.inspection &&
              select.status == geo::MaterializationStatus::kCanceled &&
              !select.selection,
          "Canceled subsequent operation returned an owner");
  Joined(inspect.execution);
  Joined(select.execution);
  Released(context, old_charge);
  source.mesh.reset();
  Released(context);
}

void CheckConcurrentCancellation() {
  Fixture fixture(kCancellationLongRowScalars);
  std::uint64_t canceled = 0, committed_races = 0, observed_slot_windows = 0;
  for (std::size_t attempt = 0; attempt < kCancellationAttempts; ++attempt) {
    auto context = Context();
    std::stop_source stop;
    std::atomic<bool> completed = false;
    geo::MeshMaterializationResult result;
    std::exception_ptr error;
    std::jthread caller([&] {
      try {
        result = fixture.Capture(context, ParallelWork(), kLedgerBudgetBytes,
                                 stop.get_token());
      } catch (...) {
        error = std::current_exception();
      }
      completed.store(true, std::memory_order_release);
    });
    while (!completed.load(std::memory_order_acquire)) {
      if (context.ActiveWorkers() != 0) {
        ++observed_slot_windows;
        stop.request_stop();
        break;
      }
      std::this_thread::yield();
    }
    caller.join();
    if (error) std::rethrow_exception(error);
    Joined(result.execution);
    // Stop can race the final publication check. Both outcomes are legal;
    // deterministic worker-gated cancellation belongs to the private-seam test.
    if (result.status == geo::MaterializationStatus::kCanceled) {
      ++canceled;
      Require(!result.mesh, "Cancellation published partial storage");
    } else {
      Successful(result);
      ++committed_races;
    }
    result.mesh.reset();
    Released(context);
  }
  // Slot observations are not launched-worker telemetry; a scheduling race may
  // yield no sampled slot window. The private seam proves deterministic
  // cancellation.
  std::cout << "concurrent_canceled=" << canceled
            << " committed_stop_races=" << committed_races
            << " observed_slot_windows=" << observed_slot_windows << '\n';
}
}  // namespace

int main() {
  try {
    Fixture fixture;
    CheckEmptySeedControl();
    CheckCaptureCapEdges(fixture);
    CheckSubsequentCapEdges(fixture, SubsequentOperation::kInspect);
    CheckSubsequentCapEdges(fixture, SubsequentOperation::kSelect);
    CheckOwnerAndTokenLifetime(fixture);
    CheckFullyDestroyedContext(fixture);
    CheckWorkerContention(fixture);
    CheckPreCanceled(fixture);
    CheckSerialParallel(fixture);
    CheckParallelIssueOrder();
    CheckConcurrentCancellation();
    std::cout << "Public accounting/worker consumer passed\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
