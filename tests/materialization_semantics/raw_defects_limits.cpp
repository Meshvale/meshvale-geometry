// SPDX-License-Identifier: Apache-2.0
#include <meshvale/geometry/attributes.h>
#include <meshvale/geometry/editable_mesh.h>
#include <meshvale/geometry/materialization.h>
#include <meshvale/geometry/mesh.h>

#include <bit>
#include <cstdint>
#include <optional>
#include <variant>

#include "fixture_support.h"
namespace materialization_test {
namespace {
constexpr std::uint64_t kExpectedIssueCount = 8;
constexpr std::uint64_t kOneFewerIssue = kExpectedIssueCount - 1;
constexpr AttributeDomain kForeignDomain = static_cast<AttributeDomain>(255);
template <class T, std::size_t N>
std::span<const T> Read(const std::array<T, N>& values) {
  return values;
}
void RequireFailure(const MeshMaterializationResult& result,
                    MaterializationStatus status,
                    MaterializationDiagnosticCode diagnostic) {
  Require(result.status == status && !result.mesh &&
              result.diagnostic.code == diagnostic,
          "capture failure status/code or absent-output guarantee");
}
void OrderedIssues() {
  const std::array<double, 9> positions{
      std::bit_cast<double>(0x7ff8000000000001ULL),
      std::bit_cast<double>(0x7ff0000000000000ULL),
      0,
      1,
      0,
      0,
      0,
      1,
      0};
  const std::array<index_t, 3> offsets{1, 2, 3};
  const std::array<index_t, 3> corners{0, 99, 1};
  const std::array<float, 3> values{1, 2, 3};
  const std::array<std::uint8_t, 3> presence{1, 2, 0};
  const std::array<MetadataEntryView, 2> metadata{
      {{"k", "first"}, {"k", "second"}}};
  std::array<AttributeMaterializationInput, 2> attributes{
      Channel(AttributeDomain::vertex, "duplicate", Read(values)),
      Channel(AttributeDomain::vertex, "duplicate", Read(values))};
  attributes[1].present = Read(presence);
  attributes[1].metadata = metadata;
  const MeshMaterializationInput input{
      Read(positions), PolygonTopologyInput{offsets, corners}, attributes};
  ExecutionContext context;
  const auto source = Capture(input, context);
  const auto view = source.View();
  RequireBits(view.positions, positions, "nonfinite raw position changed");
  RequireBits(view.face_offsets, offsets, "malformed start offset repaired");
  RequireBits(view.corner_vertices, corners, "out-of-range corner repaired");
  RequireBits(*view.attributes[1].present, presence,
              "invalid presence code changed");
  Require(view.attributes[1].metadata.size() == 2 &&
              view.attributes[1].metadata[0].value == "first" &&
              view.attributes[1].metadata[1].value == "second",
          "duplicate metadata keys collapsed/reordered");
  const std::array<StorageIssue, 8> expected{
      {{StorageIssueCode::kPositionNonfinite, {}, index_t{0}},
       {StorageIssueCode::kFaceOffsetsStart, {}, {}},
       {StorageIssueCode::kFaceTooShort, {}, index_t{0}},
       {StorageIssueCode::kFaceTooShort, {}, index_t{1}},
       {StorageIssueCode::kCornerVertexRange, {}, index_t{1}},
       {StorageIssueCode::kAttributeDuplicateName, index_t{1}, {}},
       {StorageIssueCode::kAttributePresenceValue, index_t{1}, index_t{1}},
       {StorageIssueCode::kMetadataDuplicateKey, index_t{1}, index_t{1}}}};
  const auto inspection = Inspect(source, kExpectedIssueCount);
  Require(!inspection.IsStorageValid() &&
              inspection.Issues().size() == expected.size(),
          "ordered issue completeness/exact cap");
  for (std::size_t i = 0; i < expected.size(); ++i) {
    const auto& actual = inspection.Issues()[i];
    Require(actual.code == expected[i].code &&
                actual.attribute == expected[i].attribute &&
                actual.element == expected[i].element,
            "independent source-ordered issue fixture");
  }
  auto capped =
      InspectMaterializedMesh(source, InspectionLimits(kOneFewerIssue), Work());
  Require(
      capped.status == MaterializationStatus::kLimitExceeded &&
          !capped.inspection &&
          capped.diagnostic.code == MaterializationDiagnosticCode::kIssueLimit,
      "inspection truncated issues into an owner");
  auto selected = SelectMeshFaces(source, {}, Limits(), Work());
  Require(selected.status == MaterializationStatus::kInvalidSourceStorage &&
              !selected.selection &&
              selected.diagnostic.code ==
                  MaterializationDiagnosticCode::kSourceStorage &&
              selected.diagnostic.storage_issue.has_value(),
          "selection admitted defective source");
  const auto issue = *selected.diagnostic.storage_issue;
  Require(issue.code == expected.front().code &&
              issue.attribute == expected.front().attribute &&
              issue.element == expected.front().element,
          "source failure did not identify first ordered issue");
}
void OptionalAndMalformedRetention() {
  const std::array<double, 0> positions{};
  const std::array<index_t, 3> bad_offsets{4, 2, 9};
  const std::array<index_t, 0> corners{};
  const std::array<std::uint16_t, 0> values{};
  const std::array<index_t, 0> empty_rows{};
  const std::array<std::uint8_t, 0> empty_presence{};
  std::array<AttributeMaterializationInput, 2> attributes{
      Channel(kForeignDomain, "", Read(values), 0),
      Channel(AttributeDomain::corner, "empty", Read(values))};
  attributes[0].offsets = Read(empty_rows);
  attributes[0].present = Read(empty_presence);
  const MeshMaterializationInput input{
      Read(positions), PolygonTopologyInput{bad_offsets, corners}, attributes};
  ExecutionContext context;
  const auto source = Capture(input, context);
  const auto view = source.View();
  RequireBits(view.face_offsets, bad_offsets,
              "unordered/range-defective offsets rewritten");
  Require(view.attributes[0].domain == kForeignDomain &&
              view.attributes[0].name.empty() &&
              view.attributes[0].components == 0,
          "raw invalid descriptor treated as transport");
  Require(view.attributes[0].offsets.has_value() &&
              view.attributes[0].offsets->empty() &&
              view.attributes[0].present.has_value() &&
              view.attributes[0].present->empty(),
          "authored empty optional arrays disappeared");
  Require(!view.attributes[1].offsets && !view.attributes[1].present,
          "absent arrays became authored empties");
}
void AggregatePhysicalLimits() {
  const std::array<double, 12> positions{0, 0, 0, 1, 0, 0, 1, 1, 0, 0, 1, 0};
  const std::array<index_t, 3> face_offsets{0, 3, 6};
  const std::array<index_t, 6> corners{0, 1, 2, 0, 2, 3};
  const std::array<std::uint16_t, 4> values{1, 2, 3, 4};
  const std::array<index_t, 3> rows{0, 2, 4};
  const std::array<std::uint8_t, 2> presence{1, 0};
  const std::array<MetadataEntryView, 2> metadata{{{"a", "x"}, {"b", "y"}}};
  std::array<AttributeMaterializationInput, 2> attributes{
      Channel(AttributeDomain::face, "a", Read(values), 2),
      Channel(AttributeDomain::face, "b", Read(values), 2)};
  for (auto& attribute : attributes) {
    attribute.semantic = "S";
    attribute.offsets = Read(rows);
    attribute.present = Read(presence);
    attribute.metadata = metadata;
  }
  const MeshMaterializationInput input{
      Read(positions), PolygonTopologyInput{face_offsets, corners}, attributes};
  struct CountCase {
    std::uint64_t MeshMaterializationLimits::* field;
    std::uint64_t count;
  };
  const std::array<CountCase, 10> counts{
      {{&MeshMaterializationLimits::max_vertices, 4},
       {&MeshMaterializationLimits::max_faces, 2},
       {&MeshMaterializationLimits::max_corners, 6},
       {&MeshMaterializationLimits::max_face_offsets, 3},
       {&MeshMaterializationLimits::max_attributes, 2},
       {&MeshMaterializationLimits::max_attribute_scalars, 8},
       {&MeshMaterializationLimits::max_attribute_offsets, 6},
       {&MeshMaterializationLimits::max_presence_bytes, 4},
       {&MeshMaterializationLimits::max_metadata_entries, 4},
       {&MeshMaterializationLimits::max_descriptor_bytes, 12}}};
  ExecutionContext context;
  auto exact = Limits();
  for (const auto& count : counts) exact.*(count.field) = count.count;
  const auto captured = MaterializeMesh(input, exact, Work(), context);
  Require(captured.status == MaterializationStatus::kSuccess && captured.mesh,
          "all inclusive physical caps");
  for (const auto& count : counts) {
    auto smaller = exact;
    smaller.*(count.field) = count.count - 1;
    RequireFailure(MaterializeMesh(input, smaller, Work(), context),
                   MaterializationStatus::kLimitExceeded,
                   MaterializationDiagnosticCode::kCountLimit);
    auto invalid = exact;
    invalid.*(count.field) = 0;
    RequireFailure(MaterializeMesh(input, invalid, Work(), context),
                   MaterializationStatus::kInvalidConfiguration,
                   MaterializationDiagnosticCode::kRequiredFieldZero);
  }
  const auto source = *captured.mesh;
  const std::array<index_t, 1> kept{0};
  auto selection_limits = Limits();
  selection_limits.max_vertices = 3;
  const auto selected = SelectMeshFaces(source, kept, selection_limits, Work());
  Require(selected.status == MaterializationStatus::kLimitExceeded &&
              !selected.selection &&
              selected.diagnostic.code ==
                  MaterializationDiagnosticCode::kCountLimit,
          "selection count cap omitted unchanged shared vertices");
}
void SelectorsAndPhysicalLimits() {
  const std::array<double, 9> positions{0, 0, 0, 1, 0, 0, 0, 1, 0};
  const std::array<index_t, 2> offsets{0, 3};
  const std::array<index_t, 3> corners{0, 1, 2};
  MeshMaterializationInput input{
      Read(positions), PolygonTopologyInput{offsets, corners}, {}};
  ExecutionContext context;
  auto limit = Limits();
  limit.max_vertices = 2;
  RequireFailure(MaterializeMesh(input, limit, Work(), context),
                 MaterializationStatus::kLimitExceeded,
                 MaterializationDiagnosticCode::kCountLimit);
  limit = Limits();
  limit.max_face_offsets = 1;
  RequireFailure(MaterializeMesh(input, limit, Work(), context),
                 MaterializationStatus::kLimitExceeded,
                 MaterializationDiagnosticCode::kCountLimit);
  limit = Limits();
  limit.max_vertices = 3;
  limit.max_faces = 1;
  limit.max_corners = 3;
  limit.max_face_offsets = 2;
  const auto exact = MaterializeMesh(input, limit, Work(), context);
  Require(exact.status == MaterializationStatus::kSuccess && exact.mesh,
          "inclusive physical count caps");
  using WorkField = std::uint64_t MeshMaterializationWorkPolicy::*;
  const std::array<WorkField, 4> work_fields{
      &MeshMaterializationWorkPolicy::parallel_min_bytes,
      &MeshMaterializationWorkPolicy::partition_bytes,
      &MeshMaterializationWorkPolicy::cancellation_block_scalars,
      &MeshMaterializationWorkPolicy::max_jobs};
  for (const auto field : work_fields) {
    auto invalid_work = Work();
    invalid_work.*field = 0;
    RequireFailure(MaterializeMesh(input, Limits(), invalid_work, context),
                   MaterializationStatus::kInvalidConfiguration,
                   MaterializationDiagnosticCode::kRequiredFieldZero);
  }
  const MeshMaterializationInput unspecified{};
  RequireFailure(MaterializeMesh(unspecified, Limits(), Work(), context),
                 MaterializationStatus::kInvalidTransport,
                 MaterializationDiagnosticCode::kTransportSelector);
  auto bad_config = Limits();
  bad_config.max_vertices = 0;
  RequireFailure(MaterializeMesh(unspecified, bad_config, Work(), context),
                 MaterializationStatus::kInvalidConfiguration,
                 MaterializationDiagnosticCode::kRequiredFieldZero);
  const std::array<double, 2> incomplete_xyz{1, 2};
  input.positions = Read(incomplete_xyz);
  RequireFailure(MaterializeMesh(input, Limits(), Work(), context),
                 MaterializationStatus::kInvalidTransport,
                 MaterializationDiagnosticCode::kPositionExtent);
  input.positions = Read(positions);
  input.topology = std::monostate{};
  RequireFailure(MaterializeMesh(input, Limits(), Work(), context),
                 MaterializationStatus::kInvalidTransport,
                 MaterializationDiagnosticCode::kTransportSelector);
  const std::array<std::uint32_t, 2> incomplete_triangle{0, 1};
  input.topology = TriangleTopologyInput{incomplete_triangle};
  RequireFailure(MaterializeMesh(input, Limits(), Work(), context),
                 MaterializationStatus::kInvalidTransport,
                 MaterializationDiagnosticCode::kTriangleExtent);
  const std::array<std::uint32_t, 3> triangle{0, 0xffffffffU, 0};
  input.topology = TriangleTopologyInput{triangle};
  const auto triangle_source = Capture(input, context);
  const std::array<index_t, 2> triangle_offsets{0, 3};
  const std::array<index_t, 3> triangle_corners{0, 0xffffffffULL, 0};
  RequireBits(triangle_source.View().face_offsets, triangle_offsets,
              "triangle offset derivation");
  RequireBits(triangle_source.View().corner_vertices, triangle_corners,
              "triangle index widening/degeneracy retention");
  const std::array<double, 0> empty_positions{};
  const std::array<index_t, 0> empty_topology{};
  const MeshMaterializationInput raw_empty{
      Read(empty_positions),
      PolygonTopologyInput{empty_topology, empty_topology},
      {}};
  const auto empty_source = Capture(raw_empty, context);
  Require(empty_source.View().face_offsets.empty(),
          "authored empty topology rewritten to canonical zero");
  const auto empty_inspection = Inspect(empty_source, 1);
  Require(empty_inspection.Issues().size() == 1 &&
              empty_inspection.Issues().front().code ==
                  StorageIssueCode::kFaceOffsetsEmpty,
          "authored empty topology missing aggregate issue");
  AttributeMaterializationInput missing_scalar;
  input.topology = PolygonTopologyInput{offsets, corners};
  input.attributes =
      std::span<const AttributeMaterializationInput>(&missing_scalar, 1);
  RequireFailure(MaterializeMesh(input, Limits(), Work(), context),
                 MaterializationStatus::kInvalidTransport,
                 MaterializationDiagnosticCode::kTransportSelector);
}
}  // namespace
void RawDefectsAndLimits() {
  OrderedIssues();
  OptionalAndMalformedRetention();
  AggregatePhysicalLimits();
  SelectorsAndPhysicalLimits();
}
}  // namespace materialization_test
