// SPDX-License-Identifier: Apache-2.0
#include <meshvale/geometry/attributes.h>
#include <meshvale/geometry/charged_triangulation.h>
#include <meshvale/geometry/editable_mesh.h>
#include <meshvale/geometry/execution.h>
#include <meshvale/geometry/materialization.h>
#include <meshvale/geometry/mesh.h>
#include <meshvale/geometry/triangulation.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <exception>
#include <initializer_list>
#include <iostream>
#include <limits>
#include <optional>
#include <span>
#include <stop_token>
#include <string_view>
#include <type_traits>
#include <variant>

#include "charged_triangulation_fixture.h"

namespace {
namespace g = meshvale::geometry;
namespace f = meshvale::geometry::charged_test;
using Status = g::ChargedTriangulationStatus;
using Code = g::ChargedTriangulationCode;
void EmptyAndConfiguration(bool unsupported) {
  g::ExecutionContext context({f::kWorkers, 0, f::kLedgerBytes});
  const g::Mesh empty;
  const auto accepted =
      g::TriangulateCharged(empty, {}, f::Limits(), f::Work(), context);
  f::Require(accepted.status == Status::kAccepted &&
                 accepted.triangulation.has_value(),
             "actual empty acceptance");
  f::Require(accepted.triangulation->Mesh().View().positions.empty() &&
                 accepted.triangulation->FaceSources().empty() &&
                 accepted.triangulation->CornerSources().empty() &&
                 f::Equal(accepted.triangulation->FaceOutputOffsets(),
                          std::array<g::index_t, 1>{0}),
             "empty complete maps");
  const std::array<g::index_t, 1> empty_offsets{0};
  const g::MeshMaterializationInput empty_input{
      std::span<const double>{},
      g::PolygonTopologyInput{empty_offsets, std::span<const g::index_t>{}},
      std::span<const g::AttributeMaterializationInput>{}};
  const auto captured_empty =
      g::MaterializeMesh(empty_input, f::MeshLimits(), f::Work(), context);
  f::Require(captured_empty.status == g::MaterializationStatus::kSuccess &&
                 captured_empty.mesh,
             "actual retained empty source capture");
  const auto retained_empty =
      g::TriangulateCharged(*captured_empty.mesh, {}, f::Limits(), f::Work());
  f::Require(
      retained_empty.status == Status::kAccepted &&
          retained_empty.triangulation && !retained_empty.source_capture &&
          retained_empty.adaptation_execution.planned_jobs == 0 &&
          retained_empty.triangulation->Inspection().IsStorageValid() &&
          retained_empty.triangulation->Mesh().View().positions.empty() &&
          retained_empty.triangulation->FaceSources().empty() &&
          retained_empty.triangulation->CornerSources().empty() &&
          f::Equal(retained_empty.triangulation->FaceOutputOffsets(),
                   empty_offsets) &&
          f::Equal(retained_empty.triangulation->Mesh().View().face_offsets,
                   empty_offsets) &&
          g::SharesExecutionBudget(
              context, retained_empty.triangulation->ShareExecutionContext()),
      "genuine retained empty output/maps/ledger without invented capture");
  auto limits = f::Limits();
  limits.max_core_new_requested_bytes = 0;
  std::stop_source stop;
  stop.request_stop();
  const auto invalid = g::TriangulateCharged(empty, {}, limits, f::Work(),
                                             context, stop.get_token());
  f::Require(invalid.status == Status::kInvalidConfiguration &&
                 invalid.diagnostic.code == Code::kRequiredFieldZero &&
                 !invalid.source_capture && !invalid.triangulation,
             "configuration precedes stop and capture");
  if (unsupported) {
    const auto rich = f::MakeLegacy();
    const auto result =
        g::TriangulateCharged(rich, {}, f::Limits(), f::Work(), context);
    f::Require(
        result.status == Status::kUnsupportedAllocationProfile &&
            !result.triangulation && result.source_capture &&
            result.source_capture->status ==
                g::MaterializationStatus::kUnsupportedAllocationProfile &&
            !result.source_inspection && !result.core &&
            !result.output_materialization,
        "genuine nonempty unsupported capture, no later invented execution");
  }
}
void CheckPointOnly(const g::MeshReadView& source,
                    const g::ChargedTriangulation& owner) {
  const auto output = owner.Mesh().View();
  const auto& counts = owner.Counts();
  f::Require(counts.source_vertices == f::kFixtureVertices &&
                 counts.output_vertices == f::kFixtureVertices &&
                 counts.source_faces == 0 && counts.output_faces == 0 &&
                 counts.source_corners == 0 && counts.output_corners == 0 &&
                 owner.Inspection().IsStorageValid(),
             "point-only complete original/current counts and inspection");
  f::Require(
      output.positions.size() == source.positions.size() &&
          std::memcmp(output.positions.data(), source.positions.data(),
                      source.positions.size_bytes()) == 0 &&
          output.corner_vertices.empty() && owner.FaceSources().empty() &&
          owner.CornerSources().empty() &&
          f::Equal(output.face_offsets, std::array<g::index_t, 1>{0}) &&
          f::Equal(owner.FaceOutputOffsets(), std::array<g::index_t, 1>{0}),
      "all point position bits and complete zero-topology maps");
  f::Require(output.attributes.size() == f::kFixtureAttributes,
             "point-only keeps all nineteen channel descriptors");
  for (std::size_t ordinal = 0; ordinal < source.attributes.size(); ++ordinal) {
    const auto& a = source.attributes[ordinal];
    const auto& b = output.attributes[ordinal];
    f::Require(a.domain == b.domain && a.name == b.name &&
                   a.semantic == b.semantic && a.set_index == b.set_index &&
                   a.components == b.components &&
                   a.values.index() == b.values.index() &&
                   a.offsets.has_value() == b.offsets.has_value() &&
                   a.present.has_value() == b.present.has_value() &&
                   a.metadata.size() == b.metadata.size(),
               "point-only descriptors/type/optional authoredness");
    if (a.offsets)
      f::Require(f::Equal(*a.offsets, *b.offsets), "point ragged backing");
    if (a.present)
      f::Require(f::Equal(*a.present, *b.present), "point missingness backing");
    for (std::size_t item = 0; item < a.metadata.size(); ++item)
      f::Require(a.metadata[item].key == b.metadata[item].key &&
                     a.metadata[item].value == b.metadata[item].value,
                 "point ordered metadata bits");
    std::visit(
        [&](const auto& original) {
          using T =
              typename std::remove_cvref_t<decltype(original)>::value_type;
          const auto actual = std::get<std::span<const T>>(b.values);
          f::Require(original.size() == actual.size(), "point scalar count");
          for (std::size_t scalar = 0; scalar < original.size(); ++scalar)
            f::Require(f::Bits(original[scalar]) == f::Bits(actual[scalar]),
                       "point complete typed vertex/missing backing bits");
        },
        a.values);
  }
}
void PointOnlyRoutes() {
  for (const bool legacy : {true, false}) {
    std::optional<g::ExecutionContext> ledger;
    std::optional<g::ChargedTriangulation> owner;
    {
      g::ExecutionContext context({f::kWorkers, 0, f::kLedgerBytes});
      ledger.emplace(context);
      f::Fixture fixture;
      fixture.legacy.face_offsets = {0};
      fixture.legacy.corner_vertices.clear();
      // An intentional zero-face domain: keep all vertex data and all nineteen
      // descriptors/types/metadata; nonvertex rows become canonical empty rows.
      for (auto& attribute : fixture.legacy.attributes) {
        if (attribute.domain == g::AttributeDomain::vertex) continue;
        std::visit([](auto& values) { values.clear(); }, attribute.values);
        if (attribute.offsets) *attribute.offsets = {0};
        if (attribute.present) attribute.present->clear();
      }
      const auto source = fixture.Capture(context, !legacy);
      const auto result =
          legacy ? g::TriangulateCharged(fixture.legacy, {}, f::Limits(),
                                         f::Work(), context)
                 : g::TriangulateCharged(source, {}, f::Limits(), f::Work());
      f::Require(result.status == Status::kAccepted && result.triangulation &&
                     result.core && result.core->workers_used == 0 &&
                     result.source_capture.has_value() == legacy &&
                     result.source_inspection &&
                     result.output_materialization && result.output_inspection,
                 "both real point-only routes complete actual phases");
      CheckPointOnly(source.View(), *result.triangulation);
      f::Require(g::SharesExecutionBudget(
                     context, result.triangulation->ShareExecutionContext()),
                 "point-only actual source ledger");
      owner = result.triangulation;
    }
    // Both original input and original context have actually died.
    f::Require(owner && ledger && owner->Inspection().IsStorageValid() &&
                   owner->NewRequestedBytes() == ledger->ActiveTrackedPayload(),
               "point-only retained complete owner survives input/context");
    std::cout << (legacy ? "Legacy" : "Retained") << " point-only retained "
              << owner->Counts().output_vertices << " vertices and "
              << owner->Mesh().View().attributes.size() << " channels\n";
    auto copy = owner;
    const auto live = ledger->ActiveTrackedPayload();
    owner.reset();
    f::Require(copy && ledger->ActiveTrackedPayload() == live,
               "point-only copy retains actual charges");
    copy.reset();
    f::Require(
        ledger->ActiveTrackedPayload() == 0 && ledger->ActiveWorkers() == 0,
        "point-only last owner releases ledger/workers");
  }
}
void RichRoutes() {
  f::Fixture fixture;
  g::ExecutionContext context({f::kWorkers, 0, f::kLedgerBytes});
  const auto source = fixture.Capture(context);
  g::TriangulationOptions options;
  options.minimum_parallel_faces = 0;
  const auto retained =
      g::TriangulateCharged(source, options, f::Limits(), f::Work());
  f::Require(retained.status == Status::kAccepted &&
                 retained.triangulation.has_value(),
             "retained rich acceptance");
  f::Check(source.View(), *retained.triangulation);
  f::Require(!retained.source_capture &&
                 retained.adaptation_execution.planned_jobs == 0 &&
                 retained.source_inspection &&
                 retained.output_materialization &&
                 retained.output_inspection && retained.core &&
                 retained.core->workers_used >= 2 &&
                 retained.core->new_requested_peak_bytes > 0,
             "real retained phase facts/core workers");
  f::Require(g::SharesExecutionBudget(
                 context, retained.triangulation->ShareExecutionContext()),
             "retained same ledger");
  const auto legacy_source = fixture.Capture(context, false);
  const auto legacy = g::TriangulateCharged(fixture.legacy, options,
                                            f::Limits(), f::Work(), context);
  f::Require(
      legacy.status == Status::kAccepted && legacy.triangulation.has_value(),
      "legacy rich acceptance");
  f::Check(legacy_source.View(), *legacy.triangulation);
  f::Require(legacy.source_capture &&
                 legacy.adaptation_execution.launched_workers >= 2 &&
                 legacy.adaptation_execution.launched_workers ==
                     legacy.adaptation_execution.joined_workers,
             "actual parallel position adaptation and joined telemetry");
  const auto old = g::Triangulate(fixture.legacy, options, context);
  f::Require(
      old.status == g::TriangulationStatus::kAccepted && old.mesh &&
          f::Equal(old.face_sources, legacy.triangulation->FaceSources()) &&
          f::Equal(old.corner_sources, legacy.triangulation->CornerSources()) &&
          f::Equal(old.face_output_offsets,
                   legacy.triangulation->FaceOutputOffsets()),
      "unchanged legacy ear order/maps");
  options.minimum_parallel_faces = std::numeric_limits<std::size_t>::max();
  const auto serial =
      g::TriangulateCharged(source, options, f::Limits(), f::Work());
  f::Require(serial.status == Status::kAccepted && serial.triangulation &&
                 serial.core && serial.core->workers_used == 0 &&
                 f::Equal(serial.triangulation->CornerSources(),
                          retained.triangulation->CornerSources()),
             "serial parallel exact correspondence");
  f::Check(source.View(), *serial.triangulation);
}
void TypedRefusals() {
  f::Fixture fixture;
  g::ExecutionContext context({f::kWorkers, 0, f::kLedgerBytes});
  const auto source = fixture.Capture(context);
  auto limits = f::Limits();
  limits.max_map_entries = 1;
  auto result = g::TriangulateCharged(source, {}, limits, f::Work());
  f::Require(result.status == Status::kLimitExceeded &&
                 result.diagnostic.code == Code::kMapEntryLimit &&
                 !result.triangulation && !result.core &&
                 !result.output_materialization,
             "map admission before allocation/core");
  limits = f::Limits();
  limits.output.max_attribute_scalars = 1;
  result = g::TriangulateCharged(source, {}, limits, f::Work());
  f::Require(result.status == Status::kLimitExceeded &&
                 result.diagnostic.code == Code::kOutputCountLimit &&
                 result.core &&
                 result.core->status == g::TriangulationStatus::kAccepted &&
                 !result.output_materialization && !result.triangulation,
             "completed core survives rejected complete output counts");
  std::stop_source stop;
  stop.request_stop();
  result = g::TriangulateCharged(source, {}, f::Limits(), f::Work(),
                                 stop.get_token());
  f::Require(result.status == Status::kCanceled && !result.source_inspection &&
                 !result.core && !result.triangulation,
             "real prestop has no phase invention");
  fixture.legacy.corner_vertices[0] = f::kFixtureVertices;
  result = g::TriangulateCharged(fixture.legacy, {}, f::Limits(), f::Work(),
                                 context);
  f::Require(result.status == Status::kBlocked &&
                 result.diagnostic.code == Code::kSourceStorage &&
                 result.diagnostic.storage_issue &&
                 result.diagnostic.storage_issue->code ==
                     g::StorageIssueCode::kCornerVertexRange &&
                 !result.core && !result.triangulation,
             "actual invalid storage inspection");
}
void Lifetime() {
  g::ExecutionContext context({f::kWorkers, 0, f::kLedgerBytes});
  std::optional<g::ChargedTriangulation> owner;
  {
    f::Fixture fixture;
    const auto source = fixture.Capture(context);
    const auto result =
        g::TriangulateCharged(source, {}, f::Limits(), f::Work());
    f::Require(result.status == Status::kAccepted && result.triangulation,
               "lifetime actual acceptance");
    owner = result.triangulation;
  }
  f::Require(
      owner && owner->NewRequestedBytes() == context.ActiveTrackedPayload(),
      "only actual retained requests remain");
  auto copy = owner;
  const auto live = context.ActiveTrackedPayload();
  owner.reset();
  f::Require(copy && context.ActiveTrackedPayload() == live,
             "copy retains requests without new charge");
  std::optional<g::MaterializedMesh> mesh(copy->Mesh());
  std::optional<g::MeshInspection> inspection(copy->Inspection());
  copy.reset();
  f::Require(mesh && inspection && inspection->IsStorageValid() &&
                 context.ActiveTrackedPayload() > 0 &&
                 context.ActiveTrackedPayload() < live,
             "independent mesh/inspection outlive map owner");
  inspection.reset();
  mesh.reset();
  f::Require(
      context.ActiveTrackedPayload() == 0 && context.ActiveWorkers() == 0,
      "last actual owners release all requests/workers");
}
}  // namespace
int main(int argc, char** argv) {
  try {
    const bool unsupported =
        argc == 2 && std::string_view(argv[1]) == "unsupported";
    f::Require(argc == 1 || unsupported, "closed test profile argument");
    EmptyAndConfiguration(unsupported);
    if (!unsupported) {
      PointOnlyRoutes();
      RichRoutes();
      TypedRefusals();
      Lifetime();
    }
    std::cout << "charged triangulation "
              << (unsupported ? "unsupported" : "qualified")
              << " controls passed\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
