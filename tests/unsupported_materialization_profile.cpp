// SPDX-License-Identifier: Apache-2.0
// Explicit public contract tests for an unqualified numerical producer.
#include <meshvale/geometry/attributes.h>
#include <meshvale/geometry/editable_mesh.h>
#include <meshvale/geometry/execution.h>
#include <meshvale/geometry/materialization.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <iostream>
#include <optional>
#include <span>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <string_view>
#include <type_traits>
#include <variant>

namespace {
namespace geo = meshvale::geometry;
constexpr std::uint64_t kBytesPerKiB = 1024;
constexpr std::uint64_t kBudgetBytes = kBytesPerKiB * kBytesPerKiB;
constexpr std::uint64_t kPhysicalCountLimit = 1024;
constexpr std::uint64_t kDescriptorByteLimit = 4096;
constexpr std::uint64_t kIssueLimit = 128;
constexpr std::uint64_t kPartitionBytes = 64;
constexpr std::uint64_t kCancellationScalars = 8;
constexpr std::uint64_t kMaximumJobs = 16;
constexpr std::size_t kCallerOnlyWorkerBudget = 1;
constexpr geo::index_t kRejectedAttributeOrdinal = 1;
constexpr std::uint8_t kInvalidPresenceCode = 2;
constexpr std::string_view kEarlierName = "earlier-empty";
constexpr std::string_view kRejectedName = "nonempty-channel";
constexpr std::string_view kSemantic = "test.raw-empty";
constexpr std::string_view kDuplicateKey = "duplicate-key";
constexpr std::string_view kFirstValue = "first-authored";
constexpr std::string_view kSecondValue = "second-authored";
constexpr std::uint32_t kAuthoredSet = 7;

void Require(bool condition, const char* message) {
  if (!condition) throw std::runtime_error(message);
}
geo::ExecutionContext Context(std::uint64_t bytes = kBudgetBytes) {
  geo::ExecutionOptions options;
  options.worker_budget = kCallerOnlyWorkerBudget;
  options.tracked_payload_budget_bytes = static_cast<std::size_t>(bytes);
  return geo::ExecutionContext(options);
}
geo::MeshMaterializationLimits Limits() {
  return {kPhysicalCountLimit,  kPhysicalCountLimit, kPhysicalCountLimit,
          kPhysicalCountLimit,  kPhysicalCountLimit, kPhysicalCountLimit,
          kPhysicalCountLimit,  kPhysicalCountLimit, kPhysicalCountLimit,
          kDescriptorByteLimit, kBudgetBytes};
}
geo::MeshMaterializationWorkPolicy Work() {
  return {kBudgetBytes, kPartitionBytes, kCancellationScalars, kMaximumJobs};
}
geo::MeshInspectionLimits InspectionLimits() {
  return {kIssueLimit, kBudgetBytes};
}
geo::MeshMaterializationInput EmptyTriangle() {
  return {std::span<const double>{},
          geo::TriangleTopologyInput{std::span<const std::uint32_t>{}},
          {}};
}
void Restored(const geo::ExecutionContext& context, std::size_t baseline = 0) {
  Require(context.ActiveTrackedPayload() == baseline &&
              context.ActiveWorkers() == 0,
          "operation left charges or worker slots behind");
}
void Failure(const geo::MeshMaterializationResult& result,
             geo::MaterializationStatus status,
             geo::MaterializationDiagnosticCode diagnostic) {
  Require(result.status == status && result.diagnostic.code == diagnostic &&
              !result.mesh,
          "unexpected capture status/code or partial owner");
  Require(result.execution.launched_workers == result.execution.joined_workers,
          "capture returned with unjoined workers");
}
geo::MaterializedMesh Capture(const geo::MeshMaterializationInput& input,
                              const geo::ExecutionContext& context,
                              std::stop_token stop = {}) {
  auto result = geo::MaterializeMesh(input, Limits(), Work(), context, stop);
  Require(result.status == geo::MaterializationStatus::kSuccess && result.mesh,
          "empty numerical structural capture must remain available");
  return *result.mesh;
}

template <class T>
void UnsupportedPosition() {
  const std::array<T, 3> positions{T{1}, T{2}, T{3}};
  auto input = EmptyTriangle();
  input.positions = std::span<const T>(positions);
  const auto context = Context();
  const auto result = geo::MaterializeMesh(input, Limits(), Work(), context);
  Failure(result, geo::MaterializationStatus::kUnsupportedAllocationProfile,
          geo::MaterializationDiagnosticCode::kAllocationProfile);
  Require(!result.diagnostic.attribute && !result.diagnostic.element,
          "position profile failure acquired an attribute ordinal");
  Restored(context);
}
template <class T>
void UnsupportedScalar() {
  const std::array<T, 1> values{T{1}};
  std::array<geo::AttributeMaterializationInput, 2> attributes;
  attributes[0].name = kEarlierName;
  attributes[0].components = 1;
  attributes[0].values = std::span<const double>{};
  attributes[1].name = kRejectedName;
  attributes[1].components = 1;
  attributes[1].values = std::span<const T>(values);
  auto input = EmptyTriangle();
  input.attributes = attributes;
  const auto context = Context();
  const auto result = geo::MaterializeMesh(input, Limits(), Work(), context);
  Failure(result, geo::MaterializationStatus::kUnsupportedAllocationProfile,
          geo::MaterializationDiagnosticCode::kAllocationProfile);
  Require(result.diagnostic.attribute == kRejectedAttributeOrdinal &&
              !result.diagnostic.element,
          "nonempty channel profile rejection lost source ordinal");
  Restored(context);
}
void ValidationPrecedence() {
  const std::array<double, 6> positions{0, 0, 0, 1, 0, 0};
  const std::array<float, 2> incomplete_xyz{0, 0};
  const std::array<std::uint32_t, 2> incomplete_triangle{0, 0};
  auto input = EmptyTriangle();
  input.positions = std::span<const double>(positions);
  const auto context = Context();
  auto limits = Limits();
  limits.max_vertices = 0;
  geo::MeshMaterializationInput untyped;
  Failure(geo::MaterializeMesh(untyped, limits, Work(), context),
          geo::MaterializationStatus::kInvalidConfiguration,
          geo::MaterializationDiagnosticCode::kRequiredFieldZero);
  Failure(geo::MaterializeMesh(untyped, Limits(), Work(), context),
          geo::MaterializationStatus::kInvalidTransport,
          geo::MaterializationDiagnosticCode::kTransportSelector);
  limits = Limits();
  limits.max_vertices = 1;
  Failure(geo::MaterializeMesh(input, limits, Work(), context),
          geo::MaterializationStatus::kLimitExceeded,
          geo::MaterializationDiagnosticCode::kCountLimit);
  input.positions = std::span<const float>(incomplete_xyz);
  Failure(geo::MaterializeMesh(input, Limits(), Work(), context),
          geo::MaterializationStatus::kInvalidTransport,
          geo::MaterializationDiagnosticCode::kPositionExtent);
  input.positions = std::span<const double>(positions);
  input.topology = geo::TriangleTopologyInput{incomplete_triangle};
  Failure(geo::MaterializeMesh(input, Limits(), Work(), context),
          geo::MaterializationStatus::kInvalidTransport,
          geo::MaterializationDiagnosticCode::kTriangleExtent);
  input.topology = geo::TriangleTopologyInput{};
  std::stop_source stopped;
  stopped.request_stop();
  Failure(geo::MaterializeMesh(input, Limits(), Work(), context,
                               stopped.get_token()),
          geo::MaterializationStatus::kCanceled,
          geo::MaterializationDiagnosticCode::kStopRequested);
  Restored(context);
}

void EmptyStructuralOperations() {
  const std::array<geo::index_t, 1> offsets{0};
  const auto context = Context();
  {
    const auto triangles = Capture(EmptyTriangle(), context);
    const auto view = triangles.View();
    Require(view.positions.empty() && view.corner_vertices.empty() &&
                view.face_offsets.size() == 1 && view.face_offsets.front() == 0,
            "empty triangles did not derive canonical offset zero");
    auto input = EmptyTriangle();
    input.topology = geo::PolygonTopologyInput{offsets, {}};
    const auto polygon = Capture(input, context);
    auto inspection =
        geo::InspectMaterializedMesh(polygon, InspectionLimits(), Work());
    Require(inspection.status == geo::MaterializationStatus::kSuccess &&
                inspection.inspection &&
                inspection.inspection->IsStorageValid() &&
                inspection.inspection->Issues().empty(),
            "empty canonical polygon inspection unavailable");
    auto selection = geo::SelectMeshFaces(polygon, {}, Limits(), Work());
    Require(selection.status == geo::MaterializationStatus::kSuccess &&
                selection.selection &&
                selection.selection->SourceFaces().empty() &&
                selection.selection->SourceCorners().empty() &&
                selection.selection->SourceVertexCount() == 0 &&
                selection.selection->SourceFaceCount() == 0 &&
                selection.selection->SourceCornerCount() == 0,
            "empty canonical polygon selection/maps unavailable");
    const auto candidate = selection.selection->Candidate().View();
    Require(candidate.positions.empty() && candidate.corner_vertices.empty() &&
                candidate.face_offsets.size() == 1 &&
                candidate.face_offsets.front() == 0,
            "empty selection candidate changed topology");
  }
  Restored(context);
}

template <class T>
void EmptyChannelLifetime(geo::AttributeDomain domain) {
  using Canonical = std::conditional_t<std::is_same_v<T, std::int8_t> ||
                                           std::is_same_v<T, std::int16_t>,
                                       std::int32_t, T>;
  const auto ledger = Context();
  std::optional<geo::MaterializedMesh> retained;
  {
    auto original = ledger;
    std::stop_source original_stop;
    std::string name(kEarlierName), semantic(kSemantic);
    std::string key(kDuplicateKey), first(kFirstValue), second(kSecondValue);
    const std::array<geo::MetadataEntryView, 2> metadata{
        {{key, first}, {key, second}}};
    geo::AttributeMaterializationInput attribute;
    attribute.domain = domain;
    attribute.name = name;
    attribute.semantic = semantic;
    attribute.set_index = kAuthoredSet;
    attribute.components = 1;
    attribute.values = std::span<const T>{};
    attribute.offsets = std::span<const geo::index_t>{};
    attribute.present = std::span<const std::uint8_t>{};
    attribute.metadata = metadata;
    std::array<geo::AttributeMaterializationInput, 2> attributes{attribute,
                                                                 attribute};
    attributes[1].name = kRejectedName;
    attributes[1].offsets.reset();
    attributes[1].present.reset();
    attributes[1].metadata = {};
    auto input = EmptyTriangle();
    input.attributes = attributes;
    retained = Capture(input, original, original_stop.get_token());
    original_stop.request_stop();
    name.assign(name.size(), 'x');
    semantic.assign(semantic.size(), 'x');
    key.assign(key.size(), 'x');
    first.assign(first.size(), 'x');
    second.assign(second.size(), 'x');
  }
  {
    const auto copy = *retained;
    retained.reset();
    const auto view = copy.View();
    Require(view.attributes.size() == 2, "empty descriptor lost");
    const auto& attribute = view.attributes.front();
    Require(
        attribute.domain == domain && attribute.name == kEarlierName &&
            attribute.semantic == kSemantic &&
            attribute.set_index == kAuthoredSet && attribute.components == 1 &&
            std::holds_alternative<std::span<const Canonical>>(
                attribute.values) &&
            std::get<std::span<const Canonical>>(attribute.values).empty() &&
            attribute.offsets && attribute.offsets->empty() &&
            attribute.present && attribute.present->empty(),
        "empty encoding/descriptor/optional arrays changed after input death");
    Require(attribute.metadata.size() == 2 &&
                attribute.metadata[0].key == kDuplicateKey &&
                attribute.metadata[1].key == kDuplicateKey &&
                attribute.metadata[0].value == kFirstValue &&
                attribute.metadata[1].value == kSecondValue,
            "ordered duplicate metadata lost owned content");
    const auto& absent = view.attributes[1];
    Require(
        absent.name == kRejectedName && !absent.offsets && !absent.present &&
            absent.metadata.empty() &&
            std::holds_alternative<std::span<const Canonical>>(absent.values) &&
            std::get<std::span<const Canonical>>(absent.values).empty(),
        "absent arrays became authored empties");
    auto inspection =
        geo::InspectMaterializedMesh(copy, InspectionLimits(), Work());
    Require(inspection.status == geo::MaterializationStatus::kSuccess &&
                inspection.inspection &&
                !inspection.inspection->IsStorageValid(),
            "empty raw inspection used destroyed/stopped source token");
    Require(ledger.ActiveTrackedPayload() > 0, "visible owner lost charges");
  }
  Restored(ledger);
}
void RawDefects() {
  const auto context = Context();
  auto input = EmptyTriangle();
  input.topology = geo::PolygonTopologyInput{};
  {
    const auto source = Capture(input, context);
    Require(source.View().face_offsets.empty(), "raw empty offsets repaired");
    auto inspected =
        geo::InspectMaterializedMesh(source, InspectionLimits(), Work());
    Require(inspected.status == geo::MaterializationStatus::kSuccess &&
                inspected.inspection &&
                inspected.inspection->Issues().size() == 1 &&
                inspected.inspection->Issues().front().code ==
                    geo::StorageIssueCode::kFaceOffsetsEmpty,
            "raw empty offsets not reported as source defect");
    auto selected = geo::SelectMeshFaces(source, {}, Limits(), Work());
    Require(
        selected.status == geo::MaterializationStatus::kInvalidSourceStorage &&
            !selected.selection && selected.diagnostic.storage_issue &&
            selected.diagnostic.storage_issue->code ==
                geo::StorageIssueCode::kFaceOffsetsEmpty,
        "source-first rejection changed on unsupported profile");
  }
  Restored(context);
  const std::array<std::uint8_t, 1> presence{kInvalidPresenceCode};
  const std::array<geo::MetadataEntryView, 2> metadata{
      {{kDuplicateKey, kFirstValue}, {kDuplicateKey, kSecondValue}}};
  geo::AttributeMaterializationInput attribute;
  attribute.name = kEarlierName;
  attribute.components = 0;
  attribute.values = std::span<const float>{};
  attribute.present = presence;
  attribute.metadata = metadata;
  input = EmptyTriangle();
  input.attributes =
      std::span<const geo::AttributeMaterializationInput>(&attribute, 1);
  {
    const auto source = Capture(input, context);
    Require(source.View().attributes.front().components == 0 &&
                source.View().attributes.front().present->front() ==
                    kInvalidPresenceCode,
            "raw defective descriptor/presence normalized");
    const auto inspected =
        geo::InspectMaterializedMesh(source, InspectionLimits(), Work());
    const std::array<geo::StorageIssue, 4> expected{
        {{geo::StorageIssueCode::kAttributeComponentsZero, geo::index_t{0}, {}},
         {geo::StorageIssueCode::kAttributePresenceCount, geo::index_t{0}, {}},
         {geo::StorageIssueCode::kAttributePresenceValue, geo::index_t{0},
          geo::index_t{0}},
         {geo::StorageIssueCode::kMetadataDuplicateKey, geo::index_t{0},
          geo::index_t{1}}}};
    Require(inspected.status == geo::MaterializationStatus::kSuccess &&
                inspected.inspection &&
                inspected.inspection->Issues().size() == expected.size(),
            "raw presence/metadata inspection completeness");
    for (std::size_t i = 0; i < expected.size(); ++i)
      Require(
          inspected.inspection->Issues()[i].code == expected[i].code &&
              inspected.inspection->Issues()[i].attribute ==
                  expected[i].attribute &&
              inspected.inspection->Issues()[i].element == expected[i].element,
          "raw presence/metadata source issue order");
    auto selected = geo::SelectMeshFaces(source, {}, Limits(), Work());
    Require(
        selected.status == geo::MaterializationStatus::kInvalidSourceStorage &&
            !selected.selection && selected.diagnostic.storage_issue &&
            selected.diagnostic.storage_issue->code == expected.front().code &&
            selected.diagnostic.storage_issue->attribute ==
                expected.front().attribute &&
            selected.diagnostic.storage_issue->element ==
                expected.front().element,
        "raw channel defect not retained as first selection failure");
    auto limits = InspectionLimits();
    limits.max_issues = expected.size() - 1;
    auto capped = geo::InspectMaterializedMesh(source, limits, Work());
    Require(capped.status == geo::MaterializationStatus::kLimitExceeded &&
                !capped.inspection &&
                capped.diagnostic.code ==
                    geo::MaterializationDiagnosticCode::kIssueLimit,
            "issue cap published a truncated owner");
  }
  Restored(context);
  auto limits = Limits();
  limits.max_metadata_entries = metadata.size();
  {
    const auto source = geo::MaterializeMesh(input, limits, Work(), context);
    Require(
        source.status == geo::MaterializationStatus::kSuccess && source.mesh,
        "inclusive metadata count rejected raw source");
  }
  Restored(context);
  limits.max_metadata_entries = metadata.size() - 1;
  const auto capped = geo::MaterializeMesh(input, limits, Work(), context);
  Failure(capped, geo::MaterializationStatus::kLimitExceeded,
          geo::MaterializationDiagnosticCode::kCountLimit);
  Require(capped.diagnostic.attribute == geo::index_t{0},
          "metadata count rejection lost descriptor ordinal");
  Restored(context);
}

template <class Predicate>
std::uint64_t MinimumAcceptedBudget(Predicate accepts) {
  Require(accepts(kBudgetBytes),
          "bounded budget search lacks a successful endpoint");
  std::uint64_t low = 1, high = kBudgetBytes;
  while (low < high) {
    const auto middle = low + (high - low) / 2;
    if (accepts(middle))
      high = middle;
    else
      low = middle + 1;
  }
  Require(low > 1 && accepts(low) && !accepts(low - 1),
          "exact/one-byte-less budget boundary not observed");
  return low;
}
void ChargedCapsAndFreshCalls() {
  const auto input = EmptyTriangle();
  const auto context = Context();
  (void)MinimumAcceptedBudget([&](std::uint64_t bytes) {
    auto limits = Limits();
    limits.max_new_requested_bytes = bytes;
    auto result = geo::MaterializeMesh(input, limits, Work(), context);
    const bool success = result.status == geo::MaterializationStatus::kSuccess;
    if (success)
      Require(result.mesh.has_value(), "successful capture lacks owner");
    else
      Failure(result, geo::MaterializationStatus::kBudgetExceeded,
              geo::MaterializationDiagnosticCode::kRequestBudget);
    result.mesh.reset();
    Restored(context);
    return success;
  });
  (void)MinimumAcceptedBudget([&](std::uint64_t bytes) {
    const auto limited = Context(bytes);
    auto result = geo::MaterializeMesh(input, Limits(), Work(), limited);
    const bool success = result.status == geo::MaterializationStatus::kSuccess;
    if (success)
      Require(result.mesh.has_value(), "root-cap capture lacks owner");
    else
      Failure(result, geo::MaterializationStatus::kBudgetExceeded,
              geo::MaterializationDiagnosticCode::kRequestBudget);
    result.mesh.reset();
    Restored(limited);
    return success;
  });
  {
    std::stop_source old_stop;
    const auto source = Capture(input, context, old_stop.get_token());
    old_stop.request_stop();
    const auto baseline = context.ActiveTrackedPayload();
    (void)MinimumAcceptedBudget([&](std::uint64_t bytes) {
      auto limits = InspectionLimits();
      limits.max_new_requested_bytes = bytes;
      auto result = geo::InspectMaterializedMesh(source, limits, Work());
      const bool success =
          result.status == geo::MaterializationStatus::kSuccess;
      Require(
          success
              ? result.inspection.has_value()
              : (!result.inspection &&
                 result.status == geo::MaterializationStatus::kBudgetExceeded &&
                 result.diagnostic.code ==
                     geo::MaterializationDiagnosticCode::kRequestBudget),
          "inspection budget failure/fresh call changed");
      result.inspection.reset();
      Restored(context, baseline);
      return success;
    });
    (void)MinimumAcceptedBudget([&](std::uint64_t bytes) {
      auto limits = Limits();
      limits.max_new_requested_bytes = bytes;
      auto result = geo::SelectMeshFaces(source, {}, limits, Work());
      const bool success =
          result.status == geo::MaterializationStatus::kSuccess;
      Require(
          success
              ? result.selection.has_value()
              : (!result.selection &&
                 result.status == geo::MaterializationStatus::kBudgetExceeded &&
                 result.diagnostic.code ==
                     geo::MaterializationDiagnosticCode::kRequestBudget),
          "selection budget failure/fresh call changed");
      result.selection.reset();
      Restored(context, baseline);
      return success;
    });
    std::stop_source stopped;
    stopped.request_stop();
    auto inspection = geo::InspectMaterializedMesh(source, InspectionLimits(),
                                                   Work(), stopped.get_token());
    Require(inspection.status == geo::MaterializationStatus::kCanceled &&
                !inspection.inspection &&
                inspection.diagnostic.code ==
                    geo::MaterializationDiagnosticCode::kStopRequested,
            "inspection did not honor fresh stopped token");
    auto selection =
        geo::SelectMeshFaces(source, {}, Limits(), Work(), stopped.get_token());
    Require(selection.status == geo::MaterializationStatus::kCanceled &&
                !selection.selection &&
                selection.diagnostic.code ==
                    geo::MaterializationDiagnosticCode::kStopRequested,
            "selection did not honor fresh stopped token");
    Restored(context, baseline);
  }
  Restored(context);
}
}  // namespace
int main() {
  try {
    // These checks are mandatory: a qualified producer must fail this mode,
    // rather than silently skipping numerical rejection expectations.
    UnsupportedPosition<float>();
    UnsupportedPosition<double>();
    UnsupportedScalar<float>();
    UnsupportedScalar<double>();
    UnsupportedScalar<std::int32_t>();
    UnsupportedScalar<std::uint8_t>();
    UnsupportedScalar<std::uint16_t>();
    UnsupportedScalar<std::uint32_t>();
    UnsupportedScalar<std::uint64_t>();
    UnsupportedScalar<std::int8_t>();
    UnsupportedScalar<std::int16_t>();
    ValidationPrecedence();
    EmptyStructuralOperations();
    EmptyChannelLifetime<float>(geo::AttributeDomain::vertex);
    EmptyChannelLifetime<double>(geo::AttributeDomain::face);
    EmptyChannelLifetime<std::int32_t>(geo::AttributeDomain::corner);
    EmptyChannelLifetime<std::uint8_t>(geo::AttributeDomain::vertex);
    EmptyChannelLifetime<std::uint16_t>(geo::AttributeDomain::face);
    EmptyChannelLifetime<std::uint32_t>(geo::AttributeDomain::corner);
    EmptyChannelLifetime<std::uint64_t>(geo::AttributeDomain::vertex);
    EmptyChannelLifetime<std::int8_t>(geo::AttributeDomain::face);
    EmptyChannelLifetime<std::int16_t>(geo::AttributeDomain::corner);
    RawDefects();
    ChargedCapsAndFreshCalls();
    std::cout << "Unsupported numerical profile and charged structural checks "
                 "passed\n";
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
