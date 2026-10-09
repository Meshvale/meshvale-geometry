// SPDX-License-Identifier: Apache-2.0
#include <meshvale/geometry/editable_mesh.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <barrier>
#include <chrono>
#include <cstdint>
#include <exception>
#include <future>
#include <iostream>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

namespace {

using namespace meshvale::geometry;

void Require(bool condition, const char* message) {
  if (!condition) throw std::runtime_error(message);
}

template <class Callable>
void RequireRejected(Callable&& operation, const char* message) {
  bool rejected = false;
  try {
    std::forward<Callable>(operation)();
  } catch (const EditorError&) {
    rejected = true;
  }
  Require(rejected, message);
}

template <class Callable>
void RequireError(Callable&& operation, EditorErrorCode code,
                  const char* message) {
  bool rejected = false;
  try {
    std::forward<Callable>(operation)();
  } catch (const EditorError& error) {
    if (error.Code() != code)
      throw std::runtime_error(std::string(message) + " (error code " +
                               std::to_string(static_cast<int>(error.Code())) +
                               ")");
    rejected = true;
  }
  Require(rejected, message);
}

std::vector<AttributeValues> ScalarSamples() {
  // In particular, these integer values must never take a detour through a
  // floating point representation while properties are moved between rows.
  return {std::vector<float>{-1.25F, 3.5F},
          std::vector<double>{-1.0e100, 1.0e-100},
          std::vector<std::int32_t>{std::numeric_limits<std::int32_t>::min(),
                                    std::numeric_limits<std::int32_t>::max()},
          std::vector<std::uint8_t>{0, 255},
          std::vector<std::uint16_t>{1, 65535},
          std::vector<std::uint32_t>{0, 4294967295U},
          std::vector<std::uint64_t>{
              9007199254740993ULL, std::numeric_limits<std::uint64_t>::max()}};
}

void IdentitiesAndTransactions() {
  const Vertex invalid_vertex;
  Require(!invalid_vertex.IsValid(), "default vertex object resolved as live");
  RequireRejected([&] { (void)invalid_vertex.Position(); },
                  "default vertex position did not fail explicitly");
  EditableMesh mesh;
  auto creation = mesh.BeginEdit();
  const auto original = creation.CreateVertex({1, 2, 3});
  Require(!original.IsValid(), "pending vertex became accepted prematurely");
  RequireRejected([&] { (void)original.Position(); },
                  "ordinary pending vertex query succeeded");
  Require(
      creation.Snapshot().Position(original) == std::array<double, 3>{1, 2, 3},
      "creating session could not resolve its pending vertex");
  Require(creation.Snapshot().IsCandidate() &&
              creation.Snapshot().Materialize().is_candidate &&
              !mesh.Snapshot().IsCandidate(),
          "candidate snapshot disguised itself as an accepted revision");
  auto other_session = mesh.BeginEdit();
  RequireRejected([&] { other_session.SetPosition(original, {4, 5, 6}); },
                  "another session accepted a pending object");
  const auto accepted = creation.Commit();
  Require(original.IsValid() && accepted.created.size() == 1 &&
              accepted.created.front() == original.Identity(),
          "commit did not publish pending identity");
  RequireError([&] { creation.SetPosition(original, {7, 8, 9}); },
               EditorErrorCode::kSessionClosed,
               "committed session stayed open");

  EditableMesh foreign_mesh;
  auto foreign_edit = foreign_mesh.BeginEdit();
  const auto foreign = foreign_edit.CreateVertex({0, 0, 0});
  (void)foreign_edit.Commit();
  auto first = mesh.BeginEdit();
  auto conflicting = mesh.BeginEdit();
  RequireError([&] { first.SetPosition(foreign, {2, 2, 2}); },
               EditorErrorCode::kForeignOwner,
               "foreign owner did not produce a distinct failure");
  RequireError(
      [&] {
        first.SetPosition(original,
                          {std::numeric_limits<double>::infinity(), 0, 0});
      },
      EditorErrorCode::kInvalidTopology, "nonfinite vertex position accepted");
  Require(first.Snapshot().Position(original) == original.Position(),
          "failed operation changed the candidate");
  first.EraseVertex(original);
  Require(original.IsValid(), "pending deletion invalidated accepted object");
  const auto replacement = first.CreateVertex({4, 5, 6});
  Require(replacement.Identity() != original.Identity() &&
              replacement.Identity().OwnerIdentity() ==
                  original.Identity().OwnerIdentity(),
          "replacement did not receive a fresh identity in the same owner");
  const auto replaced = first.Commit();
  Require(
      !original.IsValid() && replacement.IsValid() &&
          replaced.erased == std::vector<ElementIdentity>{original.Identity()},
      "committed deletion did not invalidate old identity");
  RequireRejected([&] { (void)original.Position(); },
                  "slot reuse revived a deleted object");
  conflicting.SetPosition(original, {99, 99, 99});
  RequireError([&] { (void)conflicting.Commit(); },
               EditorErrorCode::kRevisionConflict,
               "stale-base transaction overwrote newer revision");
  Require(replacement.Position() == std::array<double, 3>{4, 5, 6},
          "conflicting candidate leaked into accepted state");

  auto discarded_edit = mesh.BeginEdit();
  const auto discarded = discarded_edit.CreateVertex({8, 8, 8});
  const auto discarded_identity = discarded.Identity();
  const auto discarded_snapshot = discarded_edit.Snapshot();
  discarded_edit.Discard();
  Require(!discarded.IsValid(), "discarded object became valid");
  Require(
      discarded_snapshot.IsCandidate() &&
          discarded_snapshot.Materialize().is_candidate &&
          discarded_snapshot.Position(discarded) ==
              std::array<double, 3>{8, 8, 8},
      "discard removed owned candidate snapshot or accepted its provenance");
  auto later = mesh.BeginEdit();
  const auto survivor = later.CreateVertex({9, 9, 9});
  Require(survivor.Identity().OwnerIdentity() ==
                  discarded_identity.OwnerIdentity() &&
              survivor.Identity() != discarded.Identity(),
          "discarded identity issuance was rolled back");
  (void)later.Commit();
  Require(!discarded.IsValid(), "later allocation revived discarded object");

  std::vector<Vertex> retired;
  auto current = replacement;
  for (int round = 0; round != 80; ++round) {
    auto churn = mesh.BeginEdit();
    retired.push_back(current);
    churn.EraseVertex(current);
    current = churn.CreateVertex({4, 5, 6});
    (void)churn.Commit();
    for (const auto& previous : retired)
      Require(!previous.IsValid() && current.Identity() != previous.Identity(),
              "repeated pool allocation revived an older identity");
  }

  const auto before_cancel = mesh.Snapshot().Materialize();
  auto canceled_edit = mesh.BeginEdit();
  canceled_edit.SetPosition(survivor, {-3, -4, -5});
  const auto canceled_pending = canceled_edit.CreateVertex({20, 30, 40});
  std::stop_source stop;
  stop.request_stop();
  RequireError([&] { (void)canceled_edit.Commit(stop.get_token()); },
               EditorErrorCode::kCanceled,
               "pre-canceled commit accepted candidate");
  Require(mesh.Snapshot().Materialize().positions == before_cancel.positions &&
              mesh.Snapshot().Revision() == before_cancel.revision &&
              !canceled_pending.IsValid(),
          "cancellation changed accepted state");
  canceled_edit.Discard();

  auto growth = mesh.BeginEdit();
  for (int i = 0; i != 600; ++i)
    (void)growth.CreateVertex({static_cast<double>(i), 0, 0});
  (void)growth.Commit();
  Require(current.Position() == std::array<double, 3>{4, 5, 6} &&
              survivor.Position() == std::array<double, 3>{9, 9, 9},
          "unrelated pool growth corrupted live objects");

  auto wrong_thread = mesh.BeginEdit();
  auto failure = std::async(std::launch::async, [&] {
    try {
      wrong_thread.SetPosition(survivor, {0, 0, 0});
    } catch (const EditorError& error) {
      return error.Code() == EditorErrorCode::kWrongThread;
    }
    return false;
  });
  Require(failure.get(), "cross-thread session mutation was accepted");
  wrong_thread.Discard();

  Vertex abandoned;
  {
    auto abandoned_edit = mesh.BeginEdit();
    abandoned = abandoned_edit.CreateVertex({1, 1, 1});
  }
  auto after_abandon = mesh.BeginEdit();
  const auto after_abandon_vertex = after_abandon.CreateVertex({1, 1, 1});
  (void)after_abandon.Commit();
  Require(!abandoned.IsValid() &&
              after_abandon_vertex.Identity() != abandoned.Identity(),
          "session destruction resurrected its abandoned pending identity");
  auto moving_session = mesh.BeginEdit();
  const auto moved_pending = moving_session.CreateVertex({2, 2, 2});
  auto receiving_session = std::move(moving_session);
  Require(receiving_session.Snapshot().Position(moved_pending) ==
              std::array<double, 3>{2, 2, 2},
          "session move lost its pending objects");
  (void)receiving_session.Commit();
  Require(moved_pending.IsValid(),
          "moved session did not publish pending object");
}

void ConcurrentSessionConflict() {
  EditableMesh mesh;
  auto initial = mesh.BeginEdit();
  const auto vertex = initial.CreateVertex({0, 0, 0});
  (void)initial.Commit();
  struct Outcome {
    bool committed;
    Vertex pending;
    double coordinate;
  };
  std::barrier ready(3);
  auto run = [&](double coordinate) {
    auto session = mesh.BeginEdit();
    session.SetPosition(vertex, {coordinate, 0, 0});
    const auto pending = session.CreateVertex({coordinate, 1, 0});
    ready.arrive_and_wait();
    try {
      (void)session.Commit();
      return Outcome{true, pending, coordinate};
    } catch (const EditorError& error) {
      Require(error.Code() == EditorErrorCode::kRevisionConflict,
              "concurrent commit failed without a revision conflict");
      return Outcome{false, pending, coordinate};
    }
  };
  auto left = std::async(std::launch::async, [&] { return run(11); });
  auto right = std::async(std::launch::async, [&] { return run(22); });
  ready.arrive_and_wait();
  const auto left_result = left.get();
  const auto right_result = right.get();
  Require(
      left_result.committed != right_result.committed &&
          mesh.Snapshot().Vertices().size() == 2,
      "same-base simultaneous sessions did not accept exactly one candidate");
  const auto& winner = left_result.committed ? left_result : right_result;
  const auto& loser = left_result.committed ? right_result : left_result;
  Require(
      winner.pending.IsValid() && !loser.pending.IsValid() &&
          vertex.Position() == std::array<double, 3>{winner.coordinate, 0, 0},
      "concurrent loser leaked pending identity or position into accepted "
      "state");
}

void OwnerLifetimeAndFork() {
  EditableMesh mesh;
  auto edit = mesh.BeginEdit();
  const auto first = edit.CreateVertex({1, 2, 3});
  const auto second = edit.CreateVertex({4, 5, 6});
  const auto edge = edit.CreateEdge(first, second);
  (void)edit.Commit();
  Require(first.Identity().Kind() == ElementKind::kVertex &&
              edge.Identity().Kind() == ElementKind::kEdge,
          "typed element objects expose the wrong identity kind");
  const auto old_snapshot = mesh.Snapshot();
  EditableMesh moved = std::move(mesh);
  Require(first.IsValid() &&
              first.Position() == std::array<double, 3>{1, 2, 3} &&
              moved.Snapshot().OwnerIdentity() == old_snapshot.OwnerIdentity(),
          "owner move changed or destroyed element identity");
  auto fork = moved.Fork();
  const auto fork_dense = fork.mesh.Snapshot().Materialize();
  Require(
      fork_dense.owner != old_snapshot.OwnerIdentity() &&
          fork_dense.positions == old_snapshot.Materialize().positions &&
          fork_dense.edge_vertices == old_snapshot.Materialize().edge_vertices,
      "explicit fork did not reproduce topology with a fresh owner");
  Require(fork.correspondence.original_to_fork.size() == 3 &&
              fork.correspondence.fork_to_original.size() == 3,
          "fork correspondence omitted elements");
  for (const auto& [original, copied] : fork.correspondence.original_to_fork)
    Require(fork.correspondence.fork_to_original.at(copied) == original &&
                copied.OwnerIdentity() == fork_dense.owner &&
                copied.Kind() == original.Kind(),
            "fork maps did not preserve kind or inverse correspondence");
  auto fork_edit = fork.mesh.BeginEdit();
  RequireError([&] { fork_edit.SetPosition(first, {7, 8, 9}); },
               EditorErrorCode::kForeignOwner,
               "fork accepted an original-owner object");
  fork_edit.SetPosition(fork_dense.vertices.front(), {7, 8, 9});
  (void)fork_edit.Commit();
  Require(first.Position() == std::array<double, 3>{1, 2, 3},
          "fork edit mutated original owner");
  auto fork_reuse = fork.mesh.BeginEdit();
  const auto copied_vertex = fork_dense.vertices.front();
  fork_reuse.EraseEdge(fork_dense.edges.front());
  fork_reuse.EraseVertex(copied_vertex);
  const auto copied_replacement = fork_reuse.CreateVertex({1, 2, 3});
  (void)fork_reuse.Commit();
  Require(
      !copied_vertex.IsValid() && copied_replacement.IsValid() &&
          copied_replacement.Identity() != copied_vertex.Identity() &&
          first.IsValid(),
      "fork delete/reallocation revived copied identity or invalidated source");

  auto orphaned_session = moved.BeginEdit();
  moved = EditableMesh();
  Require(!first.IsValid() && !edge.IsValid(),
          "destroyed owner remained live through snapshot or session");
  RequireError([&] { (void)first.Position(); },
               EditorErrorCode::kOwnerDestroyed,
               "destroyed-owner query did not fail explicitly");
  RequireError([&] { (void)orphaned_session.CreateVertex({0, 0, 0}); },
               EditorErrorCode::kOwnerDestroyed,
               "session kept destroyed editing owner alive");
  Require(old_snapshot.Position(first) == std::array<double, 3>{1, 2, 3} &&
              old_snapshot.Endpoints(edge)[1].Identity() == second.Identity(),
          "owned snapshot failed after owner destruction");
  Require(old_snapshot.Vertices().size() == 2 &&
              old_snapshot.Vertices().front().Identity() == first.Identity() &&
              !old_snapshot.Vertices().front().IsValid(),
          "snapshot iteration conflated owned identities with current live "
          "objects");
  EditorSnapshot invalid;
  RequireError(
      [&] { (void)invalid.Materialize(); }, EditorErrorCode::kInvalidObject,
      "default snapshot did not report explicit invalid-object failure");
}

Face Triangle(EditSession& edit, Vertex first, Vertex second, Vertex third,
              Edge shared) {
  const auto second_edge = edit.CreateEdge(second, third);
  const auto third_edge = edit.CreateEdge(third, first);
  const std::array<FaceCorner, 3> corners{FaceCorner{first, shared, {}},
                                          FaceCorner{second, second_edge, {}},
                                          FaceCorner{third, third_edge, {}}};
  return edit.CreateFace(corners);
}

void ExplicitTopologyAndDenseMaps() {
  EditableMesh mesh;
  auto edit = mesh.BeginEdit();
  const auto a = edit.CreateVertex({0, 0, 0});
  const auto b = edit.CreateVertex({2, 0, 0});
  const auto c = edit.CreateVertex({1, 2, 0});
  const auto d = edit.CreateVertex({1, -2, 0});
  const auto e = edit.CreateVertex({1, 0, 2});
  const auto shared = edit.CreateEdge(a, b);
  const auto first_face = Triangle(edit, a, b, c, shared);
  (void)Triangle(edit, b, a, d, shared);
  (void)Triangle(edit, a, b, e, shared);
  const auto f = edit.CreateVertex({5, 0, 0});
  const auto g = edit.CreateVertex({5, 2, 0});
  (void)Triangle(edit, a, f, g, edit.CreateEdge(a, f));
  const auto wire = edit.CreateEdge(f, g);
  const auto parallel = edit.CreateEdge(a, b);
  RequireError([&] { edit.EraseVertex(a); },
               EditorErrorCode::kReferencedElement,
               "referenced vertex deletion was implicit");
  RequireError([&] { edit.EraseEdge(shared); },
               EditorErrorCode::kReferencedElement,
               "referenced edge deletion was implicit");
  (void)edit.Commit();
  Require(shared.Corners().size() == 3 && parallel.Corners().empty() &&
              wire.Corners().empty(),
          "non-manifold or distinct wire/parallel-edge identity was lost");
  Require(a.Corners().size() == 4,
          "disconnected vertex fan lost incident corners");
  for (const auto& corner : shared.Corners())
    Require(corner.GetEdge().Identity() == shared.Identity(),
            "edge incidence refers to another authored edge");
  const auto snapshot = mesh.Snapshot();
  const auto dense = snapshot.Materialize();
  Require(dense.owner == snapshot.OwnerIdentity() &&
              dense.revision == snapshot.Revision(),
          "dense snapshot lacks owner/revision provenance");
  for (index_t row = 0; row != dense.vertices.size(); ++row)
    Require(dense.vertex_rows.at(dense.vertices[row].Identity()) == row &&
                dense.positions[row] == snapshot.Position(dense.vertices[row]),
            "vertex dense maps are not inverse owned correspondence");
  for (index_t row = 0; row != dense.edges.size(); ++row) {
    Require(dense.edge_rows.at(dense.edges[row].Identity()) == row,
            "edge dense map missing inverse correspondence");
    const auto endpoints = snapshot.Endpoints(dense.edges[row]);
    for (std::size_t end = 0; end != 2; ++end)
      Require(dense.vertices[dense.edge_vertices[row][end]].Identity() ==
                  endpoints[end].Identity(),
              "edge dense connectivity differs from authored identity");
  }
  for (index_t row = 0; row != dense.faces.size(); ++row)
    Require(dense.face_rows.at(dense.faces[row].Identity()) == row,
            "face dense map lacks inverse correspondence");
  for (index_t row = 0; row != dense.corners.size(); ++row) {
    const auto corner = dense.corners[row];
    Require(dense.corner_rows.at(corner.Identity()) == row &&
                dense.vertices[dense.corner_vertices[row]].Identity() ==
                    corner.GetVertex().Identity() &&
                dense.edges[dense.corner_edges[row]].Identity() ==
                    corner.GetEdge().Identity() &&
                dense.faces[dense.corner_faces[row]].Identity() ==
                    corner.GetFace().Identity(),
            "corner dense correspondence lost authored connectivity");
  }
  RequireError([&] { (void)snapshot.ExportMesh(); },
               EditorErrorCode::kUnsupportedProjection,
               "plain Mesh silently erased authored wires/parallel edges");
  auto fork = mesh.Fork();
  const auto fork_dense = fork.mesh.Snapshot().Materialize();
  Require(fork_dense.positions == dense.positions &&
              fork_dense.edge_vertices == dense.edge_vertices &&
              fork_dense.face_offsets == dense.face_offsets &&
              fork_dense.corner_edges == dense.corner_edges &&
              fork.correspondence.original_to_fork.size() ==
                  dense.vertices.size() + dense.edges.size() +
                      dense.faces.size() + dense.corners.size(),
          "fork lost explicit edge/incidence graph or element correspondence");
  for (const auto& [original, copied] : fork.correspondence.original_to_fork)
    Require(
        original.Kind() == copied.Kind() &&
            fork.correspondence.fork_to_original.at(copied) == original &&
            copied.OwnerIdentity() == fork_dense.owner,
        "topology fork correspondence was not a fresh inverse identity map");
  const auto deleted_corners = first_face.Corners();
  auto erase = mesh.BeginEdit();
  erase.EraseFace(first_face);
  (void)erase.Commit();
  Require(!first_face.IsValid() && shared.Corners().size() == 2,
          "face erase did not atomically remove edge incidence");
  for (const auto& corner : deleted_corners)
    Require(!corner.IsValid(), "face erase left its corner valid");
  Require(snapshot.Corners(first_face).size() == 3 &&
              snapshot.Corners(shared).size() == 3 &&
              snapshot.Materialize().corner_vertices == dense.corner_vertices,
          "later deletion modified an old snapshot");
  auto cascade = mesh.BeginEdit();
  cascade.EraseVertex(a, ErasePolicy::kCascade);
  const auto cascade_result = cascade.Commit();
  Require(
      !a.IsValid() && !shared.IsValid() && !parallel.IsValid() &&
          wire.IsValid() && mesh.Snapshot().Faces().empty() &&
          std::find(cascade_result.erased.begin(), cascade_result.erased.end(),
                    a.Identity()) != cascade_result.erased.end(),
      "explicit cascade did not retain unrelated wire or report erased vertex");
}

void PolygonCyclesAndImport() {
  Mesh raw;
  raw.positions = {{0, 0, 0}, {2, 0, 0}, {2, 2, 0}, {1, 1, 0},
                   {0, 2, 0}, {4, 0, 0}, {5, 0, 0}, {5, 1, 0}};
  raw.face_offsets = {0, 5, 8, 12};
  raw.corner_vertices = {0, 1, 2, 3, 4, 5, 6, 7, 0, 1, 0, 4};
  const auto expected_vertices = raw.corner_vertices;
  auto imported = EditableMesh::ImportMesh(raw);
  Require(imported.edge_policy == "derived-undirected-endpoint-pair",
          "raw import did not disclose its derived-edge policy");
  const auto snapshot = imported.mesh.Snapshot();
  const auto dense = snapshot.Materialize();
  Require(dense.face_offsets == raw.face_offsets &&
              dense.corner_vertices == expected_vertices &&
              dense.corners.size() == 12,
          "mixed concave/repeated-corner cycles changed during import");
  const auto projected = snapshot.ExportMesh();
  Require(projected.positions == raw.positions &&
              projected.face_offsets == raw.face_offsets &&
              projected.corner_vertices == raw.corner_vertices &&
              inspect_storage(projected).empty(),
          "representable raw projection did not retain polygon storage");
  Require(imported.correspondence.vertex_rows == dense.vertex_rows &&
              imported.correspondence.corner_rows == dense.corner_rows,
          "raw import correspondence differs from materialization");
  auto edit = imported.mesh.BeginEdit();
  const auto before = edit.Snapshot().Materialize();
  const std::array<FaceCorner, 3> wrong{
      FaceCorner{dense.vertices[0], dense.edges.front(), {}},
      FaceCorner{dense.vertices[7], dense.edges.front(), {}},
      FaceCorner{dense.vertices[3], dense.edges.front(), {}}};
  RequireError([&] { (void)edit.CreateFace(wrong); },
               EditorErrorCode::kInvalidTopology,
               "face accepted edges disconnected from its ordered vertices");
  Require(
      edit.Snapshot().Materialize().corner_vertices == before.corner_vertices &&
          edit.Snapshot().Faces().size() == before.faces.size(),
      "failed face insertion partially changed candidate");
  edit.EraseFace(dense.faces[1], UnusedEdgePolicy::kPrune);
  (void)edit.Commit();
  Require(imported.mesh.Snapshot().Edges().size() == dense.edges.size() - 3,
          "explicit unused-edge pruning kept isolated triangle edges");
}

PropertyDescriptor Descriptor(std::string name, PropertyScalarType type,
                              PropertyDomain domain = PropertyDomain::kVertex) {
  PropertyDescriptor result;
  result.name = std::move(name);
  result.scalar_type = type;
  result.domain = domain;
  return result;
}

void ExactPropertyRowsAndChannelIdentity() {
  const std::array<PropertyScalarType, 7> types{
      PropertyScalarType::kFloat32, PropertyScalarType::kFloat64,
      PropertyScalarType::kInt32,   PropertyScalarType::kUint8,
      PropertyScalarType::kUint16,  PropertyScalarType::kUint32,
      PropertyScalarType::kUint64};
  const auto samples = ScalarSamples();
  EditableMesh mesh;
  auto edit = mesh.BeginEdit();
  const auto deleted = edit.CreateVertex({0, 0, 0});
  const auto survivor = edit.CreateVertex({1, 0, 0});
  std::vector<Property> properties;
  for (std::size_t i = 0; i != samples.size(); ++i) {
    auto descriptor = Descriptor("scalar_" + std::to_string(i), types[i]);
    descriptor.components = 2;
    descriptor.semantic = "application.exact-data";
    descriptor.set_index = static_cast<std::uint32_t>(i);
    descriptor.metadata = {{"meaning", "opaque authored values"}};
    auto property = edit.CreateProperty(descriptor);
    edit.SetPropertyRow(property, deleted.Identity(), {samples[i], true});
    edit.SetPropertyRow(property, survivor.Identity(), {samples[i], false});
    properties.push_back(property);
  }
  RequireError(
      [&] {
        edit.SetPropertyRow(properties[0], survivor.Identity(),
                            {std::vector<std::uint64_t>{1, 2}, true});
      },
      EditorErrorCode::kInvalidProperty,
      "property accepted a scalar variant different from its descriptor");
  RequireError(
      [&] {
        edit.SetPropertyRow(properties[0], survivor.Identity(),
                            {std::vector<float>{1}, true});
      },
      EditorErrorCode::kInvalidProperty,
      "dense property accepted the wrong component count");
  (void)edit.Commit();
  const auto original_snapshot = mesh.Snapshot();
  const auto raw = original_snapshot.ExportMesh();
  Require(raw.attributes.size() == samples.size(),
          "raw projection omitted an exact scalar variant channel");
  for (std::size_t i = 0; i != properties.size(); ++i)
    Require(properties[i].Row(deleted.Identity()).values == samples[i] &&
                properties[i].Row(deleted.Identity()).present &&
                properties[i].Row(survivor.Identity()).values == samples[i] &&
                !properties[i].Row(survivor.Identity()).present,
            "property lost exact scalar payload or authored missingness");
  for (std::size_t i = 0; i != properties.size(); ++i) {
    const auto found = std::find_if(
        raw.attributes.begin(), raw.attributes.end(), [&](const auto& channel) {
          return channel.name == "scalar_" + std::to_string(i);
        });
    const AttributeValues expected = std::visit(
        [](const auto& row) -> AttributeValues {
          auto rows = row;
          rows.insert(rows.end(), row.begin(), row.end());
          return rows;
        },
        samples[i]);
    Require(found != raw.attributes.end() && found->values == expected &&
                found->present ==
                    std::optional<std::vector<std::uint8_t>>({1, 0}) &&
                found->semantic == "application.exact-data" &&
                found->set_index == static_cast<std::uint32_t>(i) &&
                found->metadata ==
                    std::map<std::string, std::string>{
                        {"meaning", "opaque authored values"}},
            "raw projection changed scalar encoding, missingness or metadata");
  }
  auto copied_row = properties.front().Row(deleted.Identity());
  std::get<std::vector<float>>(copied_row.values).front() = 100;
  auto copied_descriptor = properties.front().Descriptor();
  copied_descriptor.metadata.clear();
  Require(
      properties.front().Row(deleted.Identity()).values == samples.front() &&
          !properties.front().Descriptor().metadata.empty(),
      "returned property values borrowed mutable accepted storage");

  auto replace = mesh.BeginEdit();
  replace.EraseVertex(deleted);
  const auto replacement = replace.CreateVertex({2, 0, 0});
  for (const auto& property : properties)
    Require(!replace.Snapshot().Row(property, replacement.Identity()).present,
            "new row inherited deleted element's authored values");
  (void)replace.Commit();
  for (std::size_t i = 0; i != properties.size(); ++i) {
    Require(properties[i].Row(survivor.Identity()).values == samples[i] &&
                !properties[i].Row(survivor.Identity()).present,
            "deletion/reuse moved another vertex's values");
    Require(original_snapshot.Row(properties[i], deleted.Identity()).values ==
                samples[i],
            "old snapshot property row changed after identity deletion");
  }
  const auto stale_property = properties.front();
  const auto descriptor = stale_property.Descriptor();
  auto recreate = mesh.BeginEdit();
  recreate.RemoveProperty(stale_property);
  const auto new_property = recreate.CreateProperty(descriptor);
  Require(new_property.Identity().OwnerIdentity() ==
                  stale_property.Identity().OwnerIdentity() &&
              new_property.Identity() != stale_property.Identity(),
          "same-name channel reuse did not issue fresh identity");
  RequireError(
      [&] {
        recreate.SetPropertyRow(stale_property, survivor.Identity(),
                                {samples.front(), true});
      },
      EditorErrorCode::kInvalidObject,
      "stale channel object mutated a recreated same-name channel");
  (void)recreate.Commit();
  Require(!stale_property.IsValid() && new_property.IsValid() &&
              !new_property.Row(survivor.Identity()).present,
          "removed channel revived or transferred old authored values");
  Require(
      original_snapshot.Descriptor(stale_property).name == descriptor.name &&
          original_snapshot.Row(stale_property, deleted.Identity()).values ==
              samples.front(),
      "channel recreation modified an old owned snapshot");

  auto fork = mesh.Fork();
  Require(
      fork.correspondence.original_to_fork.contains(new_property.Identity()),
      "fork correspondence omitted property identity");
  const auto fork_snapshot = fork.mesh.Snapshot();
  const auto fork_property_identity =
      fork.correspondence.original_to_fork.at(new_property.Identity());
  const auto fork_properties = fork_snapshot.Properties();
  const auto found =
      std::find_if(fork_properties.begin(), fork_properties.end(),
                   [&](const auto& property) {
                     return property.Identity() == fork_property_identity;
                   });
  Require(found != fork_properties.end() &&
              fork_snapshot.Descriptor(*found).metadata == descriptor.metadata,
          "fork lost channel metadata");
}

void UvInfluencesAndRowPolicies() {
  EditableMesh mesh;
  auto edit = mesh.BeginEdit();
  auto uv0_descriptor = Descriptor("uv0", PropertyScalarType::kFloat32);
  uv0_descriptor.components = 2;
  uv0_descriptor.semantic = "texcoord";
  uv0_descriptor.set_index = 0;
  uv0_descriptor.metadata = {{"origin", "upper-left"}};
  uv0_descriptor.new_row_policy = NewRowPolicy::kRequireExplicit;
  const auto uv0 = edit.CreateProperty(uv0_descriptor);
  auto uv1_descriptor = uv0_descriptor;
  uv1_descriptor.name = "uv1";
  uv1_descriptor.set_index = 1;
  uv1_descriptor.metadata = {{"origin", "lower-left"}};
  const auto uv1 = edit.CreateProperty(uv1_descriptor);
  const auto before_failed_creation = edit.Snapshot().Vertices().size();
  RequireError([&] { (void)edit.CreateVertex({0, 0, 0}); },
               EditorErrorCode::kInvalidProperty,
               "required authored property row silently defaulted");
  Require(edit.Snapshot().Vertices().size() == before_failed_creation,
          "missing required row left a partially created vertex");
  std::vector<Vertex> vertices;
  for (int i = 0; i != 4; ++i) {
    const std::array<PropertyAssignment, 2> assignments{
        PropertyAssignment{
            uv0, {std::vector<float>{static_cast<float>(i), 0}, true}},
        PropertyAssignment{
            uv1, {std::vector<float>{0, static_cast<float>(i)}, true}}};
    vertices.push_back(
        edit.CreateVertex({static_cast<double>(i), 0, 0}, assignments));
  }
  auto joints_descriptor = Descriptor("joints", PropertyScalarType::kUint16);
  joints_descriptor.ragged = true;
  joints_descriptor.semantic = "joint_index";
  joints_descriptor.set_index = 0;
  joints_descriptor.metadata = {{"skeleton", "body"}};
  const auto joints = edit.CreateProperty(joints_descriptor);
  auto weights_descriptor = Descriptor("weights", PropertyScalarType::kFloat64);
  weights_descriptor.ragged = true;
  weights_descriptor.semantic = "joint_weight";
  weights_descriptor.set_index = 0;
  const auto weights = edit.CreateProperty(weights_descriptor);
  edit.SetPropertyRow(joints, vertices[0].Identity(),
                      {std::vector<std::uint16_t>{2, 8, 300}, true});
  edit.SetPropertyRow(weights, vertices[0].Identity(),
                      {std::vector<double>{0.5, 0.25, 0.25}, true});
  edit.SetPropertyRow(joints, vertices[1].Identity(),
                      {std::vector<std::uint16_t>{65535}, true});
  edit.SetPropertyRow(weights, vertices[1].Identity(),
                      {std::vector<double>{1}, true});
  edit.SetPropertyRow(joints, vertices[3].Identity(),
                      {std::vector<std::uint16_t>{}, true});
  edit.SetPropertyRow(weights, vertices[3].Identity(),
                      {std::vector<double>{}, true});
  auto default_descriptor =
      Descriptor("author_id", PropertyScalarType::kUint64);
  default_descriptor.new_row_policy = NewRowPolicy::kDefault;
  default_descriptor.default_row =
      PropertyRow{std::vector<std::uint64_t>{9007199254740993ULL}, true};
  const auto authored_default = edit.CreateProperty(default_descriptor);
  (void)edit.Commit();
  const auto before = mesh.Snapshot();
  const auto raw = before.ExportMesh();
  const auto raw_weights = std::find_if(
      raw.attributes.begin(), raw.attributes.end(),
      [](const auto& attribute) { return attribute.name == "weights"; });
  Require(
      raw_weights != raw.attributes.end() &&
          raw_weights->values ==
              AttributeValues(std::vector<double>{0.5, 0.25, 0.25, 1}) &&
          raw_weights->offsets ==
              std::optional<std::vector<index_t>>({0, 3, 4, 4, 4}) &&
          raw_weights->present ==
              std::optional<std::vector<std::uint8_t>>({1, 1, 0, 1}),
      "dense export merged ragged rows or authored-empty/missing distinction");
  const auto raw_uv1 = std::find_if(
      raw.attributes.begin(), raw.attributes.end(),
      [](const auto& attribute) { return attribute.name == "uv1"; });
  Require(raw_uv1 != raw.attributes.end() && raw_uv1->set_index == 1U &&
              raw_uv1->metadata == uv1_descriptor.metadata &&
              raw_uv1->values ==
                  AttributeValues(std::vector<float>{0, 0, 0, 1, 0, 2, 0, 3}),
          "second UV set or its metadata collapsed into first set");
  auto replace = mesh.BeginEdit();
  replace.EraseVertex(vertices[0]);
  const std::array<PropertyAssignment, 2> assignments{
      PropertyAssignment{uv0, {std::vector<float>{8, 9}, true}},
      PropertyAssignment{uv1, {std::vector<float>{10, 11}, true}}};
  const auto replacement = replace.CreateVertex({8, 0, 0}, assignments);
  (void)replace.Commit();
  Require(uv0.Row(replacement.Identity()).values ==
                  AttributeValues(std::vector<float>{8, 9}) &&
              uv1.Row(replacement.Identity()).values ==
                  AttributeValues(std::vector<float>{10, 11}) &&
              !joints.Row(replacement.Identity()).present &&
              !weights.Row(replacement.Identity()).present &&
              authored_default.Row(replacement.Identity()).values ==
                  AttributeValues(
                      std::vector<std::uint64_t>{9007199254740993ULL}) &&
              weights.Row(vertices[1].Identity()).values ==
                  AttributeValues(std::vector<double>{1}) &&
              weights.Row(vertices[3].Identity()).present,
          "delete/insert lost independent UV/influence/default row policy");
  Require(before.Row(joints, vertices[0].Identity()).values ==
              AttributeValues(std::vector<std::uint16_t>{2, 8, 300}),
          "snapshot did not own old variable-length influence row");
}

void FaceCornerAndEdgeProperties() {
  EditableMesh mesh;
  auto edit = mesh.BeginEdit();
  auto edge_descriptor =
      Descriptor("crease", PropertyScalarType::kUint8, PropertyDomain::kEdge);
  edge_descriptor.new_row_policy = NewRowPolicy::kRequireExplicit;
  const auto crease = edit.CreateProperty(edge_descriptor);
  auto face_descriptor = Descriptor("material", PropertyScalarType::kUint32,
                                    PropertyDomain::kFace);
  face_descriptor.new_row_policy = NewRowPolicy::kRequireExplicit;
  const auto material = edit.CreateProperty(face_descriptor);
  auto corner_descriptor =
      Descriptor("seam", PropertyScalarType::kFloat32, PropertyDomain::kCorner);
  corner_descriptor.components = 2;
  corner_descriptor.new_row_policy = NewRowPolicy::kRequireExplicit;
  corner_descriptor.semantic = "texcoord";
  corner_descriptor.set_index = 1;
  const auto seam = edit.CreateProperty(corner_descriptor);
  const auto a = edit.CreateVertex({0, 0, 0});
  const auto b = edit.CreateVertex({1, 0, 0});
  const auto c = edit.CreateVertex({0, 1, 0});
  const std::array<PropertyAssignment, 1> edge_assignment{
      PropertyAssignment{crease, {std::vector<std::uint8_t>{255}, true}}};
  const auto ab = edit.CreateEdge(a, b, edge_assignment);
  const auto bc = edit.CreateEdge(b, c, edge_assignment);
  const auto ca = edit.CreateEdge(c, a, edge_assignment);
  const std::array<FaceCorner, 3> corners{
      FaceCorner{a, ab, {{seam, {std::vector<float>{0, 0}, true}}}},
      FaceCorner{b, bc, {{seam, {std::vector<float>{1, 0}, true}}}},
      FaceCorner{c, ca, {{seam, {std::vector<float>{0, 1}, true}}}}};
  const std::array<PropertyAssignment, 1> face_assignment{PropertyAssignment{
      material, {std::vector<std::uint32_t>{4294967295U}, true}}};
  const auto face = edit.CreateFace(corners, face_assignment);
  RequireError(
      [&] {
        edit.SetPropertyRow(crease, a.Identity(),
                            {std::vector<std::uint8_t>{1}, true});
      },
      EditorErrorCode::kInvalidProperty, "edge property accepted a vertex row");
  (void)edit.Commit();
  const auto snapshot = mesh.Snapshot();
  Require(crease.Row(ab.Identity()).values ==
                  AttributeValues(std::vector<std::uint8_t>{255}) &&
              material.Row(face.Identity()).values ==
                  AttributeValues(std::vector<std::uint32_t>{4294967295U}),
          "face/edge scalar property rows changed");
  RequireError([&] { (void)snapshot.ExportMesh(); },
               EditorErrorCode::kUnsupportedProjection,
               "plain Mesh silently dropped an edge-domain property");
  auto remove_edge_channel = mesh.BeginEdit();
  remove_edge_channel.RemoveProperty(crease);
  (void)remove_edge_channel.Commit();
  const auto raw = mesh.Snapshot().ExportMesh();
  Require(raw.attributes.size() == 2,
          "plain Mesh projection lost face/corner property channels");
  for (const auto& attribute : raw.attributes) {
    if (attribute.name == "material")
      Require(attribute.domain == AttributeDomain::face &&
                  attribute.values ==
                      AttributeValues(std::vector<std::uint32_t>{4294967295U}),
              "projected face-domain row lost exact integer value");
    else if (attribute.name == "seam")
      Require(attribute.domain == AttributeDomain::corner &&
                  attribute.set_index == 1U &&
                  attribute.values ==
                      AttributeValues(std::vector<float>{0, 0, 1, 0, 0, 1}),
              "projected corner UV rows lost seam ordering");
    else
      Require(false, "unexpected channel in plain projection");
  }
  const auto old_corners = face.Corners();
  auto erase = mesh.BeginEdit();
  erase.EraseFace(face, UnusedEdgePolicy::kPrune);
  (void)erase.Commit();
  Require(mesh.Snapshot().Edges().empty() && mesh.Snapshot().Faces().empty() &&
              mesh.Snapshot().Corners().empty(),
          "pruned face removal retained topology rows");
  Require(snapshot.Row(crease, ab.Identity()).values ==
                  AttributeValues(std::vector<std::uint8_t>{255}) &&
              snapshot.Row(seam, old_corners[1].Identity()).values ==
                  AttributeValues(std::vector<float>{1, 0}),
          "owned snapshot lost erased edge/corner property rows");
  for (const auto& property : mesh.Snapshot().Materialize().properties)
    Require(property.present.empty() &&
                std::visit([](const auto& values) { return values.empty(); },
                           property.values),
            "removed domain elements left visible property rows");
}

void IndependentProjectionRejections() {
  Mesh triangle;
  triangle.positions = {{0, 0, 0}, {1, 0, 0}, {0, 1, 0}};
  triangle.face_offsets = {0, 3};
  triangle.corner_vertices = {0, 1, 2};
  auto with_wire = EditableMesh::ImportMesh(triangle);
  auto wire_edit = with_wire.mesh.BeginEdit();
  const auto wire_a = wire_edit.CreateVertex({3, 3, 3});
  const auto wire_b = wire_edit.CreateVertex({4, 4, 4});
  (void)wire_edit.CreateEdge(wire_a, wire_b);
  (void)wire_edit.Commit();
  RequireError([&] { (void)with_wire.mesh.Snapshot().ExportMesh(); },
               EditorErrorCode::kUnsupportedProjection,
               "plain Mesh erased a wire without any parallel-edge ambiguity");

  EditableMesh with_parallel;
  auto parallel_edit = with_parallel.BeginEdit();
  const auto a = parallel_edit.CreateVertex({0, 0, 0});
  const auto b = parallel_edit.CreateVertex({1, 0, 0});
  const auto c = parallel_edit.CreateVertex({0, 1, 0});
  const auto first_ab = parallel_edit.CreateEdge(a, b);
  const auto second_ab = parallel_edit.CreateEdge(a, b);
  const auto bc = parallel_edit.CreateEdge(b, c);
  const auto ca = parallel_edit.CreateEdge(c, a);
  const std::array<FaceCorner, 3> first{FaceCorner{a, first_ab, {}},
                                        FaceCorner{b, bc, {}},
                                        FaceCorner{c, ca, {}}};
  const std::array<FaceCorner, 3> second{FaceCorner{a, second_ab, {}},
                                         FaceCorner{b, bc, {}},
                                         FaceCorner{c, ca, {}}};
  (void)parallel_edit.CreateFace(first);
  (void)parallel_edit.CreateFace(second);
  (void)parallel_edit.Commit();
  Require(first_ab.Corners().size() == 1 && second_ab.Corners().size() == 1,
          "authored parallel edge identities merged");
  RequireError([&] { (void)with_parallel.Snapshot().ExportMesh(); },
               EditorErrorCode::kUnsupportedProjection,
               "plain Mesh collapsed used parallel edges without any wires");
}

void ParallelBounds() {
  Mesh raw;
  constexpr int kVertices = 40000;
  raw.positions.reserve(kVertices);
  for (int i = 0; i != kVertices; ++i)
    raw.positions.push_back({static_cast<double>(i - 20000),
                             static_cast<double>(i % 31 - 15),
                             static_cast<double>(-i)});
  auto imported = EditableMesh::ImportMesh(raw);
  const auto snapshot = imported.mesh.Snapshot();
  const Bounds expected{{-20000, -15, -39999}, {19999, 15, 0}};
  ExecutionContext execution({3, 1});
  const auto first = ComputeBounds(snapshot, execution);
  Require(first.bounds && first.bounds->minimum == expected.minimum &&
              first.bounds->maximum == expected.maximum &&
              first.workers_used >= 2 && first.workers_used <= 3 &&
              first.serial_reason.empty(),
          "bounds did not actually execute parallel exact reduction");
  Require(execution.WorkerBudget() == 3 && execution.ActiveWorkers() == 0 &&
              execution.PeakWorkers() >= first.workers_used &&
              execution.PeakWorkers() <= 3,
          "execution context did not account actual parallel worker usage");
  for (int repeat = 0; repeat != 5; ++repeat) {
    const auto repeated = ComputeBounds(snapshot, execution);
    Require(repeated.bounds && repeated.bounds->minimum == expected.minimum &&
                repeated.bounds->maximum == expected.maximum,
            "parallel bounds changed across deterministic repetitions");
  }
  ExecutionContext serial({1, 1});
  const auto reference = ComputeBounds(snapshot, serial);
  Require(
      reference.bounds && reference.bounds->minimum == expected.minimum &&
          reference.bounds->maximum == expected.maximum &&
          reference.workers_used == 0 && !reference.serial_reason.empty(),
      "single-budget serial reference lacks reason or disagrees with parallel");
  ExecutionContext copied_context = execution;
  std::barrier start(3);
  auto run = [&](const ExecutionContext& context) {
    start.arrive_and_wait();
    return ComputeBounds(snapshot, context);
  };
  auto left = std::async(std::launch::async, [&] { return run(execution); });
  auto right =
      std::async(std::launch::async, [&] { return run(copied_context); });
  start.arrive_and_wait();
  const auto left_result = left.get();
  const auto right_result = right.get();
  Require(left_result.bounds && right_result.bounds &&
              left_result.bounds->minimum == expected.minimum &&
              left_result.bounds->maximum == expected.maximum &&
              right_result.bounds->minimum == expected.minimum &&
              right_result.bounds->maximum == expected.maximum &&
              execution.PeakWorkers() <= 3 &&
              execution.PeakWorkers() == copied_context.PeakWorkers() &&
              execution.ActiveWorkers() == 0 &&
              copied_context.ActiveWorkers() == 0,
          "overlapping calls exceeded shared worker cap or changed results");
  std::stop_source stop;
  stop.request_stop();
  RequireError(
      [&] { (void)ComputeBounds(snapshot, execution, stop.get_token()); },
      EditorErrorCode::kCanceled, "bounds ignored cancellation");
  Require(execution.ActiveWorkers() == 0,
          "canceled bounds leaked worker reservations");

  ExecutionContext move_source({2, 1});
  auto move_destination = std::move(move_source);
  RequireError([&] { (void)move_source.WorkerBudget(); },
               EditorErrorCode::kInvalidObject,
               "moved-from context budget query was not rejected");
  RequireError([&] { (void)move_source.ActiveWorkers(); },
               EditorErrorCode::kInvalidObject,
               "moved-from context active-worker query was not rejected");
  RequireError([&] { (void)move_source.PeakWorkers(); },
               EditorErrorCode::kInvalidObject,
               "moved-from context peak-worker query was not rejected");
  RequireError([&] { (void)ComputeBounds(snapshot, move_source); },
               EditorErrorCode::kInvalidObject,
               "bounds accepted a moved-from context");
  const auto moved_result = ComputeBounds(snapshot, move_destination);
  Require(moved_result.bounds &&
              moved_result.bounds->minimum == expected.minimum &&
              moved_result.bounds->maximum == expected.maximum &&
              move_destination.WorkerBudget() == 2,
          "execution context move lost live destination state");

  EditableMesh empty;
  const auto empty_bounds = ComputeBounds(empty.Snapshot(), execution);
  Require(!empty_bounds.bounds && empty_bounds.workers_used == 0 &&
              !empty_bounds.serial_reason.empty(),
          "empty snapshot bounds did not report a serial reason");
}

void InFlightBoundsCancellation() {
  Mesh raw;
  constexpr int kVertices = 1000000;
  raw.positions.reserve(kVertices);
  for (int i = 0; i != kVertices; ++i)
    raw.positions.push_back({static_cast<double>(i), 1, -1});
  auto imported = EditableMesh::ImportMesh(raw);
  const auto snapshot = imported.mesh.Snapshot();
  ExecutionContext execution({3, 1});
  const Bounds expected{{0, 1, -1}, {999999, 1, -1}};
  struct AttemptOutcome {
    std::optional<BoundsResult> completed;
    bool canceled;
  };
  int attempts = 0;
  int observed = 0;
  int canceled = 0;
  int canceled_without_observation = 0;
  int completion_races = 0;
  int completed_without_observation = 0;
  int observation_timeouts = 0;
  // Observation is an opportunity, not a guaranteed scheduling window. The
  // operation may finish before this caller sees a reservation, or after its
  // final cancellation check but before this caller requests stop.
  for (; attempts != 8 && observed == 0; ++attempts) {
    std::stop_source stop;
    auto computation = std::async(std::launch::async, [&] {
      try {
        return AttemptOutcome{
            ComputeBounds(snapshot, execution, stop.get_token()), false};
      } catch (const EditorError& error) {
        Require(error.Code() == EditorErrorCode::kCanceled,
                "in-flight bounds failed for a reason other than cancellation");
        Require(stop.stop_requested(),
                "bounds reported cancellation without a stop request");
        return AttemptOutcome{std::nullopt, true};
      }
    });
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(5);
    bool observed_this_attempt = false;
    while (computation.wait_for(std::chrono::seconds(0)) !=
           std::future_status::ready) {
      if (execution.ActiveWorkers() != 0) {
        observed_this_attempt = true;
        stop.request_stop();
        break;
      }
      if (std::chrono::steady_clock::now() >= deadline) {
        ++observation_timeouts;
        stop.request_stop();
        break;
      }
      std::this_thread::yield();
    }
    const auto outcome = computation.get();
    Require(execution.ActiveWorkers() == 0,
            "bounds attempt left active worker reservations");
    if (observed_this_attempt) ++observed;
    if (outcome.canceled) {
      ++canceled;
      if (!observed_this_attempt) ++canceled_without_observation;
    } else {
      Require(outcome.completed && outcome.completed->bounds &&
                  outcome.completed->bounds->minimum == expected.minimum &&
                  outcome.completed->bounds->maximum == expected.maximum,
              "completed bounds attempt differs from the exact reference");
      if (observed_this_attempt)
        ++completion_races;
      else
        ++completed_without_observation;
    }
  }
  std::cout << "Bounds cancellation observation: attempts=" << attempts
            << " observed=" << observed << " canceled=" << canceled
            << " canceled_without_observation=" << canceled_without_observation
            << " completion_races=" << completion_races
            << " completed_without_observation="
            << completed_without_observation
            << " observation_timeouts=" << observation_timeouts << '\n';
}

}  // namespace

int main() {
  try {
    IdentitiesAndTransactions();
    ConcurrentSessionConflict();
    OwnerLifetimeAndFork();
    ExplicitTopologyAndDenseMaps();
    PolygonCyclesAndImport();
    ExactPropertyRowsAndChannelIdentity();
    UvInfluencesAndRowPolicies();
    FaceCornerAndEdgeProperties();
    IndependentProjectionRejections();
    ParallelBounds();
    InFlightBoundsCancellation();
    std::cout << "Pooled editing acceptance scenarios passed\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
